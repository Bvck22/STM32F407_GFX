"""In-process GFX device implementing protocol v2 (used by --sim and the tests)."""
from __future__ import annotations

import math
import queue
import random
import re
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from gfx_link import MAX_LINE, SCOPE_POINTS, Algorithm, LinkError, ParamDesc, _fmt, ulaw_encode

INC_DIR = Path(__file__).resolve().parent.parent / "firmware" / "Core" / "Inc"
ALGO_HEADER = INC_DIR / "fx_algo_table.h"
PARAMS_HEADER = INC_DIR / "fx_params.h"

SLOTS = 4
PRESETS = 16
RATE = 48000
SAMPLE_LEN = 288000
# Arenas (bytes): SRAM and CCM, as on the real board.
ARENAS = (88 * 1024, 62 * 1024)
MEM_TOTAL = sum(ARENAS)
REST = -1  # "take the largest remaining region"
MIN_REST = 24000
COST_BUDGET = 2400.0  # cycles per frame
COST_OVERHEAD = 250.0
# algorithm id -> (mem_bytes, cost in cycles/frame), from the firmware descriptors.
RESOURCES = {
    0: (0, 0), 1: (72128, 200), 2: (72128, 260), 3: (72128, 220), 4: (72256, 300), 5: (72000, 150),
    6: (4096, 260), 7: (8192, 170), 8: (4096, 330), 9: (0, 420), 10: (0, 110), 11: (0, 480),
    12: (0, 130), 13: (8192, 650), 14: (8192, 450), 15: (16384, 420), 16: (23476, 640),
    17: (38700, 690), 18: (38538, 670), 19: (7400, 330), 20: (33196, 780), 21: (0, 260),
    22: (0, 240), 23: (0, 70), 24: (0, 200), 25: (0, 240), 26: (0, 170), 27: (REST, 150),
}

_ALGO_ROW = re.compile(r'^\s*A\(\s*(\d+)\s*,\s*\w+\s*,\s*"([^"]*)"\s*,\s*FX_CAT_(\w+)\s*\)')
_PARAMS_DEF = re.compile(r'#define\s+FX_PARAMS_(\w+)\(P\)')
_PARAM_ROW = re.compile(
    r'^\s*P\(\s*(\w+)\s*,\s*(-?[\d.]+)f\s*,\s*(-?[\d.]+)f\s*,\s*(-?[\d.]+)f\s*,'
    r'\s*(-?[\d.]+)f\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\)')
_ALGO_SUFFIX = re.compile(r'^\s*A\(\s*\d+\s*,\s*(\w+)\s*,')
_GLOBAL_ROW = re.compile(
    r'X\(\s*\w+\s*,\s*"([\w.]+)"\s*,\s*(-?[\d.]+)f?\s*,\s*(-?[\d.]+)f?\s*,'
    r'\s*(-?[\d.]+)f?\s*,\s*(-?[\d.]+)f?\s*\)')


SAMPLE_C = INC_DIR.parent / "Src" / "sample_data.c"
_sample_cache: list[int] | None = None


def load_sample() -> list[int]:
    """The firmware test sample (parsed once, on first scope use); a decaying tone as fallback."""
    global _sample_cache
    if _sample_cache is None:
        try:
            body = re.search(r"g_sample_data\[\]\s*=\s*\{(.*?)\}", SAMPLE_C.read_text(encoding="utf-8"), re.S)
            _sample_cache = [int(v) for v in re.findall(r"-?\d+", body.group(1))]
        except (OSError, AttributeError):
            _sample_cache = [int(12000 * math.exp(-i / 24000) * math.sin(i * 0.0143))
                             for i in range(SAMPLE_LEN)]
    return _sample_cache


def load_algorithms(header: Path = ALGO_HEADER) -> list[Algorithm]:
    """Parse the A(...) / FX_PARAMS_*(P) macros of fx_algo_table.h."""
    text = header.read_text(encoding="utf-8")
    cats = re.search(r"FX_CATEGORY_NAMES\s*\{([^}]*)\}", text).group(1)
    names = re.findall(r'"([^"]*)"', cats)
    enum = re.search(r"typedef enum\s*\{([^}]*)\}\s*fx_category_t", text).group(1)
    suffixes = [m for m in re.findall(r"FX_CAT_(\w+)", enum) if m != "COUNT"]
    cat_by_suffix = dict(zip(suffixes, names))

    algos: dict[str, Algorithm] = {}
    current: Algorithm | None = None
    for line in text.splitlines():
        if m := _ALGO_ROW.match(line):
            suffix = _ALGO_SUFFIX.match(line).group(1)
            algos[suffix] = Algorithm(int(m[1]), m[2], cat_by_suffix[m[3]])
        elif m := _PARAMS_DEF.match(line):
            current = algos.get(m[1])
        elif current and (m := _PARAM_ROW.match(line)):
            key, mn, mx, dflt, step, unit, labels = m.groups()
            current.params.append(ParamDesc(key, float(mn), float(mx), float(step), float(dflt),
                                            unit, labels.split("|") if labels else []))
    return sorted(algos.values(), key=lambda a: a.id)


def load_globals(header: Path = PARAMS_HEADER) -> list[ParamDesc]:
    rows = _GLOBAL_ROW.findall(header.read_text(encoding="utf-8"))
    return [ParamDesc(n, float(mn), float(mx), float(st), float(df))
            for n, mn, mx, df, st in rows]


@dataclass
class _Loop:
    state: str = "empty"
    length: int = 0
    pos0: int = 0
    t0: float = 0.0


@dataclass
class _Slot:
    algo_id: int = 0
    on: bool = False
    values: dict[int, float] = field(default_factory=dict)
    loop: _Loop = field(default_factory=_Loop)


class SimDevice:
    """Implements the Transport protocol: write_line() feeds commands, read_line() yields replies."""

    def __init__(self, clock: Callable[[], float] = time.monotonic) -> None:
        self.algos = {a.id: a for a in load_algorithms()}
        self.global_descs = {g.key: g for g in load_globals()}
        self.globals = {k: g.default for k, g in self.global_descs.items()}
        self.slots = [_Slot() for _ in range(SLOTS)]
        self.presets: list[tuple[str, dict] | None] = [None] * PRESETS
        self.preset_index = -1
        self._clock = clock
        self._out: queue.Queue[str] = queue.Queue()
        self._lock = threading.Lock()
        self._rng = random.Random(1)
        self._play = False
        self._play_t0 = 0.0
        self._closed = False
        self._scope: tuple[float, int, bool, int, int] | None = None  # t0, window ms, trigger, decim, play pos
        self._default_chain()

    # -- Transport ------------------------------------------------------
    def emit_event(self, line: str) -> None:
        self._out.put(line)

    def write_line(self, line: str) -> None:
        if self._closed:
            raise LinkError("simulator closed")
        with self._lock:
            replies = self._handle(line.strip("\r\n"))
        for reply in replies:
            self._out.put(reply)

    def read_line(self) -> str | None:
        if self._closed:
            raise LinkError("simulator closed")
        with self._lock:
            self._tick_loops()
        try:
            return self._out.get(timeout=0.05)
        except queue.Empty:
            return None

    def close(self) -> None:
        self._closed = True

    # -- chain helpers -----------------------------------------------------
    def _default_chain(self) -> None:
        for n, algo_id in enumerate((21, 6, 1, 17)):
            self._assign(n, algo_id)
        self.slots[1].on = False

    def _assign(self, n: int, algo_id: int) -> None:
        algo = self.algos[algo_id]
        self.slots[n] = _Slot(algo_id, algo_id != 0,
                              {i: p.default for i, p in enumerate(algo.params)})

    def _category(self, slot: _Slot) -> str:
        return self.algos[slot.algo_id].category

    @staticmethod
    def _cost(slots: list[_Slot]) -> float:
        return COST_OVERHEAD + sum(RESOURCES[s.algo_id][1] for s in slots)

    @staticmethod
    def _layout(slots: list[_Slot]) -> tuple[int, int] | None:
        """(used bytes, bytes given to the first REST algorithm) or None if it does not fit."""
        free = list(ARENAS)
        fixed = sorted((RESOURCES[s.algo_id][0] for s in slots if RESOURCES[s.algo_id][0] > 0),
                       reverse=True)
        for size in fixed:
            fits = [i for i, f in enumerate(free) if f >= size]
            if not fits:
                return None
            free[min(fits, key=lambda i: free[i] - size)] -= size
        rest_bytes = 0
        for s in slots:
            if RESOURCES[s.algo_id][0] != REST:
                continue
            biggest = max(range(len(free)), key=free.__getitem__)
            if free[biggest] < MIN_REST:
                return None
            rest_bytes = rest_bytes or free[biggest]
            free[biggest] = 0
        return MEM_TOTAL - sum(free), rest_bytes

    def _loop_slot(self) -> int | None:
        return next((i for i, s in enumerate(self.slots) if self._category(s) == "Looper"), None)

    def _loop_cap(self) -> int:
        layout = self._layout(self.slots)
        return layout[1] // 2 if layout else 0

    def _snapshot(self) -> dict:
        return {"globals": dict(self.globals),
                "slots": [(s.algo_id, s.on, dict(s.values)) for s in self.slots]}

    def _restore(self, snap: dict) -> None:
        self.globals = dict(snap["globals"])
        self.slots = [_Slot(a, on, dict(v)) for a, on, v in snap["slots"]]

    # -- looper state machine ------------------------------------------------
    def _loop_pos(self, loop: _Loop, now: float) -> int:
        elapsed = round((now - loop.t0) * RATE)
        if loop.state == "recording":
            return elapsed
        if loop.state in ("playing", "overdub") and loop.length:
            return (loop.pos0 + elapsed) % loop.length
        return 0

    def _tick_loops(self) -> None:
        n = self._loop_slot()
        if n is None:
            return
        loop, cap = self.slots[n].loop, self._loop_cap()
        now = self._clock()
        if loop.state == "recording" and self._loop_pos(loop, now) >= cap:
            loop.state, loop.length, loop.pos0, loop.t0 = "playing", cap, 0, now
            self._out.put(f"EVT LOOP {n} playing")

    def _loop_action(self, loop: _Loop, action: str) -> None:
        now = self._clock()
        pos = self._loop_pos(loop, now)
        if loop.state in ("playing", "overdub"):
            loop.pos0 = pos  # re-base so the playhead keeps its phase
        loop.t0 = now
        if action == "REC":
            if loop.state == "recording":
                loop.length = pos
                loop.state = "playing" if pos else "empty"
            else:
                loop.state, loop.length = "recording", 0
            loop.pos0 = 0
        elif action == "DUB" and loop.state in ("playing", "overdub"):
            loop.state = "overdub" if loop.state == "playing" else "playing"
        elif action == "PLAY" and loop.state == "stopped":
            loop.state, loop.pos0 = "playing", 0
        elif action == "STOP":
            if loop.state == "recording":
                loop.length = pos
            loop.state = "stopped" if loop.length else "empty"
        elif action == "CLEAR":
            loop.state, loop.length, loop.pos0 = "empty", 0, 0

    # -- command dispatch -------------------------------------------------------
    def _handle(self, line: str) -> list[str]:
        if len(line) > MAX_LINE:
            return ["ERR line too long"]
        parts = line.split()
        if not parts:
            return []
        handler = getattr(self, "_cmd_" + parts[0].lower(), None)
        if handler is None:
            return ["ERR unknown command"]
        try:
            return handler(parts[1:], line)
        except (ValueError, IndexError):
            return ["ERR bad value"]

    def _slot_index(self, text: str) -> int | None:
        n = int(text)
        return n if 0 <= n < SLOTS else None

    def _cmd_ping(self, _a: list[str], _l: str) -> list[str]:
        return ["PONG GFX 2.0"]

    def _cmd_algos(self, _a: list[str], _l: str) -> list[str]:
        out = []
        for algo in self.algos.values():
            out.append(f"A {algo.id} {algo.category} {len(algo.params)} {algo.name}")
            for i, p in enumerate(algo.params):
                out.append(f"AP {algo.id} {i} {p.key} {_fmt(p.min)} {_fmt(p.max)} {_fmt(p.step)} "
                           f"{_fmt(p.default)} {p.unit or '-'} {'|'.join(p.labels) or '-'}")
        return out + ["END"]

    def _slot_values(self, n: int) -> list[str]:
        return [f"V {n} {i} {_fmt(v)}" for i, v in sorted(self.slots[n].values.items())]

    def _cmd_state(self, _a: list[str], _l: str) -> list[str]:
        out = [f"G {k} {_fmt(d.min)} {_fmt(d.max)} {_fmt(d.step)} {_fmt(d.default)} "
               f"{_fmt(self.globals[k])}" for k, d in self.global_descs.items()]
        for n, s in enumerate(self.slots):
            out.append(f"SLOT {n} {s.algo_id} {int(s.on)}")
            out += self._slot_values(n)
        preset = self.presets[self.preset_index] if self.preset_index >= 0 else None
        out.append(f"PRESET {self.preset_index} {preset[0] if preset else '-'}")
        return out + ["END"]

    def _cmd_setg(self, args: list[str], _l: str) -> list[str]:
        desc = self.global_descs.get(args[0])
        if desc is None:
            return ["ERR range"]
        self.globals[desc.key] = desc.quantize(float(args[1])) if desc.step else float(args[1])
        return [f"OK G {desc.key} {_fmt(self.globals[desc.key])}"]

    def _cmd_slot(self, args: list[str], _l: str) -> list[str]:
        n = self._slot_index(args[0])
        algo_id = int(args[1])
        if n is None or algo_id not in self.algos:
            return ["ERR range"]
        trial = list(self.slots)
        trial[n] = _Slot(algo_id)
        if self._cost(trial) > COST_BUDGET:
            return ["ERR cpu"]
        if self._layout(trial) is None:
            return ["ERR memory"]
        self._assign(n, algo_id)
        return [f"OK SLOT {n} {algo_id}"] + self._slot_values(n) + ["END"]

    def _cmd_on(self, args: list[str], _l: str) -> list[str]:
        n = self._slot_index(args[0])
        if n is None:
            return ["ERR range"]
        self.slots[n].on = bool(int(args[1]))
        return [f"OK ON {n} {int(self.slots[n].on)}"]

    def _cmd_set(self, args: list[str], _l: str) -> list[str]:
        n, idx, value = self._slot_index(args[0]), int(args[1]), float(args[2])
        params = self.algos[self.slots[n].algo_id].params if n is not None else []
        if not 0 <= idx < len(params):
            return ["ERR range"]
        applied = params[idx].quantize(value)
        self.slots[n].values[idx] = applied
        return [f"OK V {n} {idx} {_fmt(applied)}"]

    def _cmd_move(self, args: list[str], _l: str) -> list[str]:
        src, dst = self._slot_index(args[0]), self._slot_index(args[1])
        if src is None or dst is None:
            return ["ERR range"]
        self.slots.insert(dst, self.slots.pop(src))
        return [f"OK MOVE {src} {dst}"]

    def _cmd_defaults(self, _a: list[str], _l: str) -> list[str]:
        self.globals = {k: d.default for k, d in self.global_descs.items()}
        for n, s in enumerate(self.slots):
            s.values = {i: p.default for i, p in enumerate(self.algos[s.algo_id].params)}
        return ["OK DEFAULTS"]

    def _cmd_play(self, _a: list[str], _l: str) -> list[str]:
        self._play, self._play_t0 = True, self._clock()
        return ["OK PLAY"]

    def _cmd_stop(self, _a: list[str], _l: str) -> list[str]:
        self._play = False
        return ["OK STOP"]

    def _cmd_loop(self, args: list[str], _l: str) -> list[str]:
        n = self._slot_index(args[0])
        action = args[1].upper()
        if n is None:
            return ["ERR range"]
        if self._category(self.slots[n]) != "Looper":
            return ["ERR no looper"]
        if action not in ("REC", "DUB", "PLAY", "STOP", "CLEAR"):
            return ["ERR bad value"]
        self._tick_loops()
        loop = self.slots[n].loop
        self._loop_action(loop, action)
        return [f"OK LOOP {n} {loop.state}"]

    def _cmd_plist(self, _a: list[str], _l: str) -> list[str]:
        rows = [f"PR {i} {p[0]}" for i, p in enumerate(self.presets) if p]
        return rows + ["END"]

    def _cmd_psave(self, args: list[str], line: str) -> list[str]:
        index = int(args[0])
        name = line.split(None, 2)[2].strip() if len(args) > 1 else ""
        if not 0 <= index < PRESETS:
            return ["ERR range"]
        if not name or len(name) > 15:
            return ["ERR bad value"]
        self.presets[index] = (name, self._snapshot())
        self.preset_index = index
        return [f"OK PSAVE {index}"]

    def _cmd_pload(self, args: list[str], _l: str) -> list[str]:
        index = int(args[0])
        if not 0 <= index < PRESETS:
            return ["ERR range"]
        if self.presets[index] is None:
            return ["ERR no preset"]
        self._restore(self.presets[index][1])
        self.preset_index = index
        return [f"OK PLOAD {index}"]

    def _cmd_pdel(self, args: list[str], _l: str) -> list[str]:
        index = int(args[0])
        if not 0 <= index < PRESETS:
            return ["ERR range"]
        self.presets[index] = None
        if self.preset_index == index:
            self.preset_index = -1
        return [f"OK PDEL {index}"]

    def _play_pos(self) -> int:
        return int((self._clock() - self._play_t0) * RATE) % SAMPLE_LEN if self._play else 0

    def _cmd_scope(self, args: list[str], _l: str) -> list[str]:
        window, trigger = int(args[0]), int(args[1])
        if not 5 <= window <= 5000 or trigger not in (0, 1):
            return ["ERR range"]
        decim = max(1, round(window * 48 / SCOPE_POINTS))
        self._scope = (self._clock(), window, bool(trigger), decim, self._play_pos())
        return [f"OK SCOPE {decim} {SCOPE_POINTS}"]

    def _cmd_scopedata(self, _a: list[str], _l: str) -> list[str]:
        if self._scope is None:
            return ["SD idle"]
        t0, window, trigger, decim, pos = self._scope
        if self._clock() - t0 < window / 1000:
            return ["SD busy"]
        start, found = self._trigger_start(pos, window) if trigger else (pos, False)
        channels = self._scope_channels(start, decim)
        out = [f"SD {decim} {SCOPE_POINTS} {int(found)}"]
        half = SCOPE_POINTS // 2
        for name, points in channels.items():
            for offset in (0, half):
                hexes = "".join(f"{ulaw_encode(round(lo * 32767)):02X}{ulaw_encode(round(hi * 32767)):02X}"
                                for lo, hi in points[offset:offset + half])
                out.append(f"SC {name} {offset} {hexes}")
        return out + ["END"]

    def _source(self, index: int) -> float:
        if not self._play:
            return 0.0
        sample = load_sample()
        return sample[index % len(sample)] / 32768

    def _trigger_start(self, pos: int, window: int) -> tuple[int, bool]:
        """First rising zero crossing (+-0.02 FS hysteresis) within max(50 ms, window)."""
        armed = False
        for i in range(pos, pos + max(2400, window * 48), 8):
            x = self._source(i)
            if x < -0.02:
                armed = True
            elif armed and x > 0.02:
                return i, True
        return pos, False

    def _scope_channels(self, start: int, decim: int) -> dict[str, list[tuple[float, float]]]:
        """Crude 'processed' output: soft clip plus an echo, slightly different per side."""
        def render(gain: float, echo_ms: int, i: int) -> float:
            echo = self._source(i - echo_ms * 48)
            return math.tanh(3 * gain * self._source(i)) * 0.7 + 0.3 * echo

        stride = max(1, decim // 16)  # sub-sample wide buckets to keep the sim fast
        channels: dict[str, list[tuple[float, float]]] = {"in": [], "l": [], "r": []}
        for p in range(SCOPE_POINTS):
            idx = range(start + p * decim, start + (p + 1) * decim, stride)
            for name, fn in (("in", self._source), ("l", lambda i: render(1.0, 350, i)),
                             ("r", lambda i: render(0.9, 300, i))):
                vals = [fn(i) for i in idx]
                channels[name].append((min(vals), max(vals)))
        return channels

    def _cmd_status(self, _a: list[str], _l: str) -> list[str]:
        now = self._clock()
        pos = self._play_pos()
        peak = min(1.0, 0.3 + 0.5 * self._rng.random()) if self._play else 0.0
        used = (self._layout(self.slots) or (0, 0))[0]
        cost = self._cost(self.slots)
        cpu = int(cost / COST_BUDGET * 100 + self._rng.randint(0, 3)) if self._play else 2
        n = self._loop_slot()
        loop = "-"
        if n is not None:
            lp = self.slots[n].loop
            pos_l = self._loop_pos(lp, now)
            length = pos_l if lp.state == "recording" else lp.length
            loop = f"{n}:{lp.state}:{pos_l}:{length}:{self._loop_cap()}"
        return [f"STAT play={int(self._play)} bypass={int(self.globals.get('bypass', 0))} "
                f"cpu={cpu} peak={peak:.3f} pos={pos} len={SAMPLE_LEN} mem={used}/{MEM_TOTAL} "
                f"cost={_fmt(cost)}/{_fmt(COST_BUDGET)} loop={loop}"]
