import re
import tempfile
import threading
import time
import unittest
from pathlib import Path

import gfx_link
import gfx_sim
import make_sample
from gfx_link import Connection, DeviceError, ParamDesc, chain_commands, export_chain
from gfx_sim import SimDevice


class FakeClock:
    def __init__(self) -> None:
        self.t = 100.0

    def __call__(self) -> float:
        return self.t


class SimCase(unittest.TestCase):
    def open(self, **kwargs) -> Connection:
        self.events: list[str] = []
        self.sim = SimDevice(**kwargs)
        conn = Connection(self.sim, on_event=self.events.append)
        self.addCleanup(conn.close)
        return conn

    def setUp(self) -> None:
        self.conn = self.open()


class DiscoveryTests(SimCase):
    def test_algos_match_header(self) -> None:
        algos = self.conn.algos().result(3)
        expected = gfx_sim.load_algorithms()
        self.assertEqual(len(algos), 28)
        self.assertEqual([(a.id, a.name, len(a.params)) for a in algos],
                         [(a.id, a.name, len(a.params)) for a in expected])
        self.assertEqual(algos[0].name, "Empty")
        shape = next(a for a in algos if a.name == "Tremolo/Pan").params[2]
        self.assertEqual(shape.labels, ["Sine", "Triangle", "Square"])
        self.assertEqual(next(a for a in algos if a.name == "Looper").category, "Looper")

    def test_state(self) -> None:
        chain = self.conn.state().result(2)
        self.assertEqual(list(chain.globals), ["bypass", "in.level", "out.level"])
        self.assertEqual(len(chain.slots), 4)
        self.assertEqual(chain.preset_index, -1)
        self.assertEqual([s.algo_id for s in chain.slots], [21, 6, 1, 17])
        self.assertEqual([s.on for s in chain.slots], [True, False, True, True])
        self.assertEqual(len(chain.slots[2].values), 5)

    def test_ping_and_errors(self) -> None:
        self.assertEqual(self.conn.call("ping"), ["PONG GFX 2.0"])
        with self.assertRaisesRegex(DeviceError, "unknown command"):
            self.conn.call("FROB")
        with self.assertRaisesRegex(DeviceError, "too long"):
            self.conn.call("SET " + "x" * 80)
        with self.assertRaisesRegex(DeviceError, "range"):
            self.conn.call("SET 9 0 1")

    def test_param_desc_format(self) -> None:
        self.assertEqual(ParamDesc("time", 20, 750, 1, 400, "ms").format(320), "320 ms")
        self.assertEqual(ParamDesc("shape", 0, 2, 1, 0, "", ["A", "B", "C"]).format(2), "C")


class EditingTests(SimCase):
    def test_set_clamps_on_and_global(self) -> None:
        self.assertEqual(self.conn.set_value(2, 0, 5000).result(1), 750)
        self.assertEqual(self.conn.set_global("out.level", -3).result(1), 0)
        self.conn.set_on(1, False).result(1)
        self.assertFalse(self.conn.state().result(2).slots[1].on)

    def test_slot_change_returns_defaults(self) -> None:
        values = self.conn.set_slot(2, 2).result(2)
        self.assertEqual(len(values), 6)
        self.assertEqual(values[0], 320)
        self.assertTrue(self.conn.state().result(2).slots[2].on)
        self.conn.set_slot(2, 0).result(2)
        self.assertFalse(self.conn.state().result(2).slots[2].on)

    def test_move(self) -> None:
        before = [s.algo_id for s in self.conn.state().result(2).slots]
        self.conn.move(0, 2).result(1)
        after = [s.algo_id for s in self.conn.state().result(2).slots]
        self.assertEqual(after, [before[1], before[2], before[0], before[3]])

    def test_memory_rejection(self) -> None:
        with self.assertRaisesRegex(DeviceError, "memory"):
            self.conn.call("SLOT 1 2")  # a second 72 KB delay fits neither arena
        self.conn.call("SLOT 3 0")
        self.assertEqual(self.conn.call("SLOT 1 17")[0], "OK SLOT 1 17")  # 38.7 KB Hall fits in CCM
        with self.assertRaisesRegex(DeviceError, "memory"):
            self.conn.call("SLOT 0 20")  # Shimmer: nowhere left

    def test_cpu_rejection_checked_first(self) -> None:
        for slot in range(4):
            self.conn.call(f"SLOT {slot} 11")  # Rotary: 4 x 480 + 250 = 2170 of 2400
        self.conn.call("SLOT 0 13")
        with self.assertRaisesRegex(DeviceError, "cpu"):
            self.conn.call("SLOT 1 13")
        self.assertIn("cost=2340/2400", self.conn.call("STATUS")[0])

    def test_status_memory(self) -> None:
        self.assertIn("mem=114924/153600", self.conn.call("STATUS")[0])

    def test_defaults(self) -> None:
        self.conn.set_value(2, 0, 100).result(1)
        self.conn.call("DEFAULTS")
        self.assertEqual(self.conn.state().result(2).slots[2].values[0], 400)


class LooperTests(unittest.TestCase):
    def setUp(self) -> None:
        self.clock = FakeClock()
        self.events: list[str] = []
        self.conn = Connection(SimDevice(clock=self.clock), on_event=self.events.append)
        self.addCleanup(self.conn.close)

    def test_no_looper(self) -> None:
        with self.assertRaisesRegex(DeviceError, "no looper"):
            self.conn.call("LOOP 0 REC")
        self.assertIn("loop=-", self.conn.call("STATUS")[0])

    def test_state_machine(self) -> None:
        self.conn.set_slot(3, 27).result(2)
        self.assertEqual(self.conn.loop(3, "REC").result(1), "recording")
        self.clock.t += 0.1
        st = self.conn.status().result(1)
        self.assertEqual((st.loop.slot, st.loop.state), (3, "recording"))
        self.assertAlmostEqual(st.loop.pos, 4800, delta=2)
        self.assertEqual(self.conn.loop(3, "REC").result(1), "playing")
        self.assertEqual(self.conn.loop(3, "DUB").result(1), "overdub")
        self.assertEqual(self.conn.loop(3, "DUB").result(1), "playing")
        self.clock.t += 0.03
        st = self.conn.status().result(1)
        self.assertEqual(st.loop.len, 4800)
        self.assertAlmostEqual(st.loop.pos, 1440, delta=2)
        self.assertEqual(self.conn.loop(3, "STOP").result(1), "stopped")
        self.assertEqual(self.conn.loop(3, "PLAY").result(1), "playing")
        self.assertEqual(self.conn.loop(3, "CLEAR").result(1), "empty")

    def test_capacity_event(self) -> None:
        self.conn.set_slot(3, 27).result(2)
        cap = self.conn.status().result(1).loop.max
        self.assertGreater(cap, 0)
        self.conn.loop(3, "REC").result(1)
        self.clock.t += cap / 48000 + 0.1
        for _ in range(20):
            if self.events:
                break
            time.sleep(0.05)
        self.assertEqual(self.events, ["EVT LOOP 3 playing"])
        self.assertEqual(self.conn.status().result(1).loop.state, "playing")


class PresetTests(SimCase):
    def test_save_load_delete_round_trip(self) -> None:
        self.conn.set_value(2, 0, 123).result(1)
        self.conn.preset_save(4, "My Echo").result(2)
        self.assertEqual(self.conn.preset_list().result(1), {4: "My Echo"})
        self.assertEqual(self.conn.state().result(2).preset_name, "My Echo")
        self.conn.set_value(2, 0, 700).result(1)
        self.conn.preset_load(4).result(1)
        self.assertEqual(self.conn.state().result(2).slots[2].values[0], 123)
        self.conn.preset_delete(4).result(1)
        self.assertEqual(self.conn.preset_list().result(1), {})
        with self.assertRaisesRegex(DeviceError, "no preset"):
            self.conn.preset_load(4).result(1)

    def test_bad_names(self) -> None:
        with self.assertRaisesRegex(DeviceError, "bad value"):
            self.conn.call("PSAVE 0 " + "n" * 16)
        with self.assertRaisesRegex(DeviceError, "range"):
            self.conn.call("PSAVE 16 x")

    def test_json_round_trip(self) -> None:
        algos = {a.id: a for a in self.conn.algos().result(3)}
        self.conn.set_value(0, 0, 0.9).result(1)
        self.conn.set_slot(3, 6).result(1)
        original = self.conn.state().result(2)
        data = export_chain(original, algos)
        other = self.open()
        for cmd in chain_commands(data, algos):
            other.call(cmd)
        restored = other.state().result(2)
        self.assertEqual(export_chain(restored, algos), data)


class ScopeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.clock = FakeClock()
        self.conn = Connection(SimDevice(clock=self.clock))
        self.addCleanup(self.conn.close)

    def test_ulaw_vectors(self) -> None:
        self.assertEqual(gfx_link.ulaw_decode(0xFF), 0)
        self.assertEqual(gfx_link.ulaw_decode(0x80), 32124)
        self.assertEqual(gfx_link.ulaw_decode(0x00), -32124)
        self.assertEqual(gfx_link.ulaw_decode(0xE0), -(gfx_link.ulaw_decode(0x60)))

    def test_ulaw_round_trip(self) -> None:
        for value in range(-32768, 32768, 13):
            back = gfx_link.ulaw_decode(gfx_link.ulaw_encode(value))
            self.assertLessEqual(abs(back - max(-32635, min(32635, value))), abs(value) // 16 + 8)

    def test_idle_busy_ready(self) -> None:
        self.assertEqual(self.conn.call("SCOPEDATA"), ["SD idle"])
        self.assertIsNone(self.conn.scope_data().result(1))
        self.assertEqual(self.conn.scope_arm(100, True).result(1), 19)
        self.assertEqual(self.conn.call("SCOPEDATA"), ["SD busy"])
        self.clock.t += 0.2
        frame = self.conn.scope_data().result(2)
        self.assertEqual(frame.decim, 19)
        self.assertEqual(set(frame.channels), {"in", "l", "r"})
        self.assertTrue(all(len(p) == 256 for p in frame.channels.values()))

    def test_reply_format_and_content(self) -> None:
        self.conn.call("PLAY")
        self.clock.t += 0.01
        self.conn.scope_arm(20, True).result(1)
        self.clock.t += 0.1
        lines = self.conn.call("SCOPEDATA")
        self.assertRegex(lines[0], r"^SD 4 256 1$")
        self.assertEqual([x.split()[1:3] for x in lines[1:-1]],
                         [[c, o] for c in ("in", "l", "r") for o in ("0", "128")])
        self.assertTrue(all(re.fullmatch(r"SC \w+ \d+ [0-9A-F]{512}", x) for x in lines[1:-1]))
        self.assertEqual(lines[-1], "END")
        frame = gfx_link.parse_scope(lines)
        self.assertTrue(frame.triggered)
        peak = max(hi for _lo, hi in frame.channels["in"])
        self.assertGreater(peak, 0.02)
        self.assertTrue(all(lo <= hi for lo, hi in frame.channels["l"]))

    def test_silent_when_stopped_and_range(self) -> None:
        self.conn.scope_arm(5, False).result(1)
        self.clock.t += 0.1
        frame = self.conn.scope_data().result(2)
        self.assertFalse(frame.triggered)
        self.assertEqual(frame.decim, 1)
        self.assertTrue(all(p == (0.0, 0.0) for p in frame.channels["in"]))
        with self.assertRaisesRegex(DeviceError, "range"):
            self.conn.call("SCOPE 4 1")


class DelayedTransport(SimDevice):
    """Delivers the reply to the first command only after a delay."""

    def __init__(self) -> None:
        super().__init__()
        self.delayed = True

    def write_line(self, line: str) -> None:
        if not self.delayed:
            return super().write_line(line)
        self.delayed = False
        with self._lock:
            replies = self._handle(line)
        threading.Timer(0.4, lambda: [self._out.put(r) for r in replies]).start()


class TimeoutTests(unittest.TestCase):
    def test_late_reply_is_dropped(self) -> None:
        conn = Connection(DelayedTransport())
        self.addCleanup(conn.close)
        with self.assertRaises(TimeoutError):
            conn.request("GET_ME_STATUS", timeout=0.2).result(2)
        self.assertEqual(conn.call("PING"), ["PONG GFX 2.0"])
        self.assertEqual(conn.status().result(1).len, gfx_sim.SAMPLE_LEN)


class MakeSampleTest(unittest.TestCase):
    def test_output_parses(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "sample_data.c"
            self.assertEqual(make_sample.main(["--out", str(out)]), 0)
            text = out.read_text(encoding="utf-8")
        body = re.search(r"g_sample_data\[\] = \{(.*?)\};", text, re.S).group(1)
        values = [int(v) for v in body.replace("\n", " ").split(",") if v.strip()]
        length = int(re.search(r"g_sample_len = (\d+)u", text).group(1))
        self.assertEqual(len(values), length)
        self.assertTrue(all(-32768 <= v <= 32767 for v in values))
        self.assertIn("g_sample_rate = 48000u", text)
        self.assertLessEqual(length, 8 * 48000)


if __name__ == "__main__":
    unittest.main()
