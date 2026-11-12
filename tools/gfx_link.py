"""GFX USB-CDC protocol v2 client (no GUI dependency)."""
from __future__ import annotations

import queue
import threading
from concurrent.futures import Future
from dataclasses import dataclass, field
from typing import Callable, Protocol

GFX_VID = 0x0483
GFX_PID = 0x5740
MAX_LINE = 63
DEFAULT_TIMEOUT = 1.0
# SAVE erases a flash sector, which stalls the MCU for a while.
COMMAND_TIMEOUTS = {"PSAVE": 4.0, "PDEL": 4.0, "PLOAD": 2.0, "ALGOS": 3.0, "STATE": 2.0}
# Replies that end with END (an ERR line also terminates them).
MULTILINE = {"ALGOS", "STATE", "PLIST", "SLOT"}


class LinkError(Exception):
    """Transport failure (cable unplugged, port closed)."""


class DeviceError(Exception):
    """Device answered with an `ERR ...` line."""


@dataclass
class ParamDesc:
    key: str
    min: float
    max: float
    step: float
    default: float
    unit: str = ""
    labels: list[str] = field(default_factory=list)

    @property
    def title(self) -> str:
        return self.key.upper().replace("_", " ")

    @property
    def is_selector(self) -> bool:
        return bool(self.labels)

    def quantize(self, value: float) -> float:
        if self.step > 0:
            value = self.min + round((value - self.min) / self.step) * self.step
        return min(self.max, max(self.min, round(value, 6)))

    def format(self, value: float) -> str:
        if self.labels:
            idx = round((value - self.min) / self.step) if self.step else 0
            return self.labels[min(max(idx, 0), len(self.labels) - 1)]
        text = f"{value:.{_decimals(self.step)}f}"
        return f"{text} {self.unit}".strip()


def _decimals(step: float) -> int:
    text = f"{step:.6f}".rstrip("0")
    return len(text.split(".")[1]) if "." in text else 0


@dataclass
class Algorithm:
    id: int
    name: str
    category: str
    params: list[ParamDesc] = field(default_factory=list)


@dataclass
class SlotState:
    algo_id: int = 0
    on: bool = False
    values: dict[int, float] = field(default_factory=dict)


@dataclass
class ChainState:
    globals: dict[str, ParamDesc] = field(default_factory=dict)
    global_values: dict[str, float] = field(default_factory=dict)
    slots: list[SlotState] = field(default_factory=list)
    preset_index: int = -1
    preset_name: str = ""


@dataclass
class LoopStatus:
    slot: int
    state: str
    pos: int
    len: int
    max: int


@dataclass
class Status:
    play: bool = False
    bypass: bool = False
    cpu: float = 0.0
    peak: float = 0.0
    pos: int = 0
    len: int = 0
    mem_used: int = 0
    mem_total: int = 1
    cost_used: float = 0.0
    cost_budget: float = 1.0
    loop: LoopStatus | None = None


def _pair(text: str) -> tuple[float, float]:
    used, total = text.split("/")
    return float(used), float(total)


def parse_status(line: str) -> Status:
    parts = line.split()
    if not parts or parts[0] != "STAT":
        raise ValueError(f"bad status line: {line!r}")
    kv = dict(p.split("=", 1) for p in parts[1:] if "=" in p)
    mem, cost = _pair(kv["mem"]), _pair(kv["cost"])
    loop = None
    if kv.get("loop", "-") != "-":
        slot, state, pos, length, cap = kv["loop"].split(":")
        loop = LoopStatus(int(slot), state, int(pos), int(length), int(cap))
    return Status(kv["play"] == "1", kv["bypass"] == "1", float(kv["cpu"]), float(kv["peak"]),
                  int(float(kv["pos"])), int(float(kv["len"])), int(mem[0]), int(mem[1]),
                  cost[0], cost[1], loop)


def parse_algos(lines: list[str]) -> list[Algorithm]:
    algos: dict[int, Algorithm] = {}
    for line in lines:
        if line.startswith("AP "):
            _, aid, _idx, key, mn, mx, step, dflt, unit, *rest = line.split(" ", 9)
            labels = rest[0] if rest else "-"
            algos[int(aid)].params.append(ParamDesc(
                key, float(mn), float(mx), float(step), float(dflt),
                "" if unit == "-" else unit, [] if labels == "-" else labels.split("|")))
        elif line.startswith("A "):
            _, aid, category, _n, name = line.split(" ", 4)
            algos[int(aid)] = Algorithm(int(aid), name, category)
    return list(algos.values())


def parse_state(lines: list[str]) -> ChainState:
    chain = ChainState()
    for line in lines:
        parts = line.split()
        if parts[0] == "G":
            name = parts[1]
            mn, mx, step, dflt, value = (float(x) for x in parts[2:7])
            chain.globals[name] = ParamDesc(name, mn, mx, step, dflt)
            chain.global_values[name] = value
        elif parts[0] == "SLOT":
            chain.slots.append(SlotState(int(parts[2]), parts[3] == "1"))
        elif parts[0] == "V":
            chain.slots[int(parts[1])].values[int(parts[2])] = float(parts[3])
        elif parts[0] == "PRESET":
            chain.preset_index = int(parts[1])
            chain.preset_name = "" if len(parts) < 3 or parts[2] == "-" else line.split(" ", 2)[2]
    return chain


SCOPE_CHANNELS = ("in", "l", "r")
SCOPE_POINTS = 256


@dataclass
class ScopeFrame:
    decim: int
    triggered: bool
    channels: dict[str, list[tuple[float, float]]]  # per channel: (min, max) in -1..1


def ulaw_decode(byte: int) -> int:
    """ITU-T G.711 mu-law byte -> 16-bit linear sample."""
    byte = ~byte & 0xFF
    magnitude = (((byte & 0x0F) << 3) + 0x84 << ((byte >> 4) & 7)) - 0x84
    return -magnitude if byte & 0x80 else magnitude


def ulaw_encode(sample: int) -> int:
    sign = 0x80 if sample < 0 else 0
    magnitude = min(abs(sample), 32635) + 0x84
    exponent = magnitude.bit_length() - 8
    mantissa = (magnitude >> (exponent + 3)) & 0x0F
    return ~(sign | exponent << 4 | mantissa) & 0xFF


def parse_scope(lines: list[str]) -> ScopeFrame | None:
    """SCOPEDATA reply -> frame, or None while the device is idle or still capturing."""
    head = lines[0].split()
    if head[1] in ("idle", "busy"):
        return None
    decim, triggered = int(head[1]), head[3] == "1"
    halves: dict[tuple[str, int], list[tuple[float, float]]] = {}
    for line in lines[1:]:
        parts = line.split()
        if parts[0] != "SC":
            continue
        raw = bytes.fromhex(parts[3])
        points = []
        for lo, hi in zip(raw[0::2], raw[1::2]):
            a, b = ulaw_decode(lo) / 32768, ulaw_decode(hi) / 32768
            points.append((min(a, b), max(a, b)))
        halves[(parts[1], int(parts[2]))] = points
    channels = {ch: halves[(ch, 0)] + halves[(ch, SCOPE_POINTS // 2)] for ch in SCOPE_CHANNELS}
    return ScopeFrame(decim, triggered, channels)


def parse_values(lines: list[str]) -> dict[int, float]:
    """V lines of a SLOT reply -> {param index: value}."""
    return {int(p[2]): float(p[3]) for p in (x.split() for x in lines) if p[0] == "V"}


def export_chain(chain: ChainState, algos: dict[int, Algorithm]) -> dict:
    slots = []
    for slot in chain.slots:
        algo = algos[slot.algo_id]
        slots.append({"algo_id": algo.id, "algo": algo.name, "on": slot.on,
                      "params": {algo.params[i].key: v for i, v in sorted(slot.values.items())}})
    return {"version": 2, "globals": dict(chain.global_values), "slots": slots}


def chain_commands(data: dict, algos: dict[int, Algorithm]) -> list[str]:
    """Command sequence that rebuilds an exported chain (slots are cleared first
    so a transient memory shortage cannot reject the new layout)."""
    cmds = [f"SETG {name} {_fmt(float(v))}" for name, v in data.get("globals", {}).items()]
    slots = data["slots"]
    cmds += [f"SLOT {n} 0" for n in range(len(slots))]
    for n, slot in enumerate(slots):
        algo = algos[int(slot["algo_id"])]
        if algo.id == 0:
            continue
        cmds.append(f"SLOT {n} {algo.id}")
        cmds.append(f"ON {n} {int(bool(slot['on']))}")
        keys = {p.key: i for i, p in enumerate(algo.params)}
        cmds += [f"SET {n} {keys[k]} {_fmt(float(v))}" for k, v in slot.get("params", {}).items()
                 if k in keys]
    return cmds


def find_ports() -> list[str]:
    """Serial port names, the GFX device first."""
    from serial.tools import list_ports

    ports = sorted(list_ports.comports(), key=lambda p: p.device)
    ports.sort(key=lambda p: not (p.vid == GFX_VID and p.pid == GFX_PID))
    return [p.device for p in ports]


def is_gfx_port(device: str) -> bool:
    from serial.tools import list_ports

    return any(p.device == device and p.vid == GFX_VID and p.pid == GFX_PID
               for p in list_ports.comports())


class Transport(Protocol):
    def write_line(self, line: str) -> None: ...
    def read_line(self) -> str | None:
        """Next complete line, or None if nothing arrived within ~100 ms."""
    def close(self) -> None: ...


class SerialTransport:
    def __init__(self, port: str, baud: int = 115200) -> None:
        import serial

        self._serial_exc = serial.SerialException
        try:
            self._ser = serial.Serial(port, baud, timeout=0.05, write_timeout=1.0)
        except self._serial_exc as exc:
            raise LinkError(str(exc)) from exc
        self._buf = bytearray()

    def write_line(self, line: str) -> None:
        try:
            self._ser.write(line.encode("ascii", "replace") + b"\n")
        except (self._serial_exc, OSError) as exc:
            raise LinkError(str(exc)) from exc

    def read_line(self) -> str | None:
        try:
            while (idx := self._buf.find(b"\n")) < 0:
                data = self._ser.read(max(1, self._ser.in_waiting))
                if not data:
                    return None
                self._buf += data
        except (self._serial_exc, OSError, TypeError) as exc:
            raise LinkError(str(exc)) from exc
        line = bytes(self._buf[:idx]).decode("ascii", "replace").strip()
        del self._buf[:idx + 1]
        return line

    def close(self) -> None:
        try:
            self._ser.close()
        except (self._serial_exc, OSError):
            pass


def _fmt(value: float) -> str:
    return f"{value:g}"


@dataclass
class _Request:
    command: str
    timeout: float
    future: Future
    lines: list[str] = field(default_factory=list)
    done: threading.Event = field(default_factory=threading.Event)
    discard: bool = False  # resync probe: swallow everything up to PONG

    @property
    def keyword(self) -> str:
        return self.command.split()[0].upper()


class Connection:
    """Threaded command/response client. All public calls are non-blocking.

    Callbacks (`on_event`, `on_disconnect`, future done-callbacks) run on
    background threads; GUI code must hop to its own thread.
    """

    def __init__(self, transport: Transport,
                 on_event: Callable[[str], None] | None = None,
                 on_disconnect: Callable[[str], None] | None = None) -> None:
        self._transport = transport
        self._on_event = on_event or (lambda _line: None)
        self._on_disconnect = on_disconnect or (lambda _reason: None)
        self._requests: queue.Queue[_Request] = queue.Queue()
        self._current: _Request | None = None
        self._lock = threading.Lock()
        self._closed = threading.Event()
        self._reader = threading.Thread(target=self._read_loop, name="gfx-reader", daemon=True)
        self._writer = threading.Thread(target=self._write_loop, name="gfx-writer", daemon=True)
        self._reader.start()
        self._writer.start()

    @classmethod
    def open_serial(cls, port: str, **kwargs) -> "Connection":
        return cls(SerialTransport(port), **kwargs)

    @classmethod
    def open_sim(cls, **kwargs) -> "Connection":
        from gfx_sim import SimDevice
        return cls(SimDevice(), **kwargs)

    @property
    def connected(self) -> bool:
        return not self._closed.is_set()

    @property
    def transport(self) -> Transport:
        return self._transport

    # -- generic -------------------------------------------------------
    def request(self, command: str, timeout: float | None = None) -> Future:
        """Queue a command; the future yields the reply lines (list[str])."""
        future: Future = Future()
        if self._closed.is_set():
            future.set_exception(LinkError("not connected"))
            return future
        if timeout is None:
            timeout = COMMAND_TIMEOUTS.get(command.split()[0].upper(), DEFAULT_TIMEOUT)
        self._requests.put(_Request(command, timeout, future))
        return future

    def call(self, command: str, timeout: float | None = None) -> list[str]:
        """Blocking variant for scripts and tests; never use from a GUI thread."""
        return self.request(command, timeout).result()

    # -- typed protocol calls ---------------------------------------------
    def algos(self) -> Future:
        return self._map(self.request("ALGOS"), parse_algos)

    def state(self) -> Future:
        return self._map(self.request("STATE"), parse_state)

    def status(self) -> Future:
        return self._map(self.request("STATUS"), lambda lines: parse_status(lines[0]))

    def set_global(self, name: str, value: float) -> Future:
        return self._map(self.request(f"SETG {name} {_fmt(value)}"), _last_float)

    def set_slot(self, slot: int, algo_id: int) -> Future:
        """Future yields the new slot's {param index: value}."""
        return self._map(self.request(f"SLOT {slot} {algo_id}"), parse_values)

    def set_on(self, slot: int, on: bool) -> Future:
        return self.request(f"ON {slot} {int(on)}")

    def set_value(self, slot: int, idx: int, value: float) -> Future:
        return self._map(self.request(f"SET {slot} {idx} {_fmt(value)}"), _last_float)

    def move(self, src: int, dst: int) -> Future:
        return self.request(f"MOVE {src} {dst}")

    def loop(self, slot: int, action: str) -> Future:
        """Future yields the resulting looper state name."""
        return self._map(self.request(f"LOOP {slot} {action}"), lambda lines: lines[0].split()[-1])

    def preset_list(self) -> Future:
        return self._map(self.request("PLIST"), _parse_presets)

    def preset_save(self, index: int, name: str) -> Future:
        return self.request(f"PSAVE {index} {name}")

    def preset_load(self, index: int) -> Future:
        return self.request(f"PLOAD {index}")

    def preset_delete(self, index: int) -> Future:
        return self.request(f"PDEL {index}")

    def scope_arm(self, window_ms: int, trigger: bool) -> Future:
        """Future yields the device's decimation factor."""
        return self._map(self.request(f"SCOPE {window_ms} {int(trigger)}"),
                         lambda lines: int(lines[0].split()[2]))

    def scope_data(self) -> Future:
        """Future yields a ScopeFrame, or None while idle/busy."""
        return self._map(self.request("SCOPEDATA"), parse_scope)

    def close(self) -> None:
        self._shutdown("closed by user", notify=False)

    # -- internals ----------------------------------------------------
    @staticmethod
    def _map(source: Future, fn: Callable[[list[str]], object]) -> Future:
        result: Future = Future()

        def done(f: Future) -> None:
            try:
                result.set_result(fn(f.result()))
            except Exception as exc:  # propagate any failure to the caller's future
                result.set_exception(exc)

        source.add_done_callback(done)
        return result

    def _write_loop(self) -> None:
        while not self._closed.is_set():
            try:
                req = self._requests.get(timeout=0.1)
            except queue.Empty:
                continue
            if self._execute(req):
                self._resync()

    def _execute(self, req: _Request) -> bool:
        """Send and wait; True if the command timed out."""
        with self._lock:
            self._current = req
        try:
            self._transport.write_line(req.command)
        except LinkError as exc:
            self._finish(req, error=exc)
            self._shutdown(str(exc))
            return False
        if req.done.wait(req.timeout):
            return False
        self._finish(req, error=TimeoutError(f"timeout waiting for {req.command!r}"))
        return True

    def _resync(self) -> None:
        """After a timeout the late reply is still in flight: PING and drop
        everything up to the PONG so it cannot be taken for the next answer."""
        self._execute(_Request("PING", 2.0, Future(), discard=True))

    def _finish(self, req: _Request, error: Exception | None = None) -> None:
        with self._lock:
            if req.done.is_set():
                return
            req.done.set()
            if self._current is req:
                self._current = None
        if error is not None:
            req.future.set_exception(error)
        elif req.lines and req.lines[-1].startswith("ERR"):
            req.future.set_exception(DeviceError(req.lines[-1]))
        else:
            req.future.set_result(req.lines)

    def _read_loop(self) -> None:
        while not self._closed.is_set():
            try:
                line = self._transport.read_line()
            except LinkError as exc:
                self._shutdown(str(exc))
                return
            if line:
                self._on_line(line)

    def _on_line(self, line: str) -> None:
        with self._lock:
            req = self._current
        if req is None or line.startswith("EVT"):
            self._on_event(line)
        elif req.discard:
            if line.startswith("PONG"):
                self._finish(req)
        else:
            req.lines.append(line)
            if _is_final(req, line):
                self._finish(req)

    def _shutdown(self, reason: str, notify: bool = True) -> None:
        if self._closed.is_set():
            return
        self._closed.set()
        self._transport.close()
        with self._lock:
            current, self._current = self._current, None
        if current is not None:
            self._finish(current, error=LinkError(reason))
        while True:
            try:
                req = self._requests.get_nowait()
            except queue.Empty:
                break
            req.future.set_exception(LinkError(reason))
        if notify:
            self._on_disconnect(reason)


def _is_final(req: _Request, line: str) -> bool:
    if req.keyword == "SCOPEDATA" and line in ("SD idle", "SD busy"):
        return True  # only the ready reply is followed by SC lines and END
    return req.keyword not in MULTILINE | {"SCOPEDATA"} or line == "END" or line.startswith("ERR")


def _last_float(lines: list[str]) -> float:
    return float(lines[0].split()[-1])


def _parse_presets(lines: list[str]) -> dict[int, str]:
    out = {}
    for line in lines:
        if line.startswith("PR "):
            parts = line.split(" ", 2)
            out[int(parts[1])] = parts[2] if len(parts) > 2 and parts[2] != "-" else ""
    return out
