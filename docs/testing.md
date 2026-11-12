**English** | [Tiếng Việt](testing.vi.md)

# Testing

## 1. Host tests (PC, algorithms and chain)
```bash
cd firmware/test/host && PATH=/c/msys64/ucrt64/bin:$PATH TMP=$LOCALAPPDATA/Temp mingw32-make
```
One suite: `... mingw32-make test_mod`. Built with `-Wall -Wextra -Wdouble-promotion -Wshadow -Werror`. The harness runs a descriptor over the flash sample in 256-frame stereo float blocks as on target, with state and arena memory pre-filled with garbage so a missing clear in `init()` shows up as noise/NaN. WAV renders go to `firmware/test/host/out/` for listening.

| Suite | Links | What it checks |
|---|---|---|
| `test_dyn` | `algo_dyn.c` | Overdrive, Fuzz, Bit Crusher, Compressor, Auto-Wah, EQ: bounded output at parameter extremes, stability, toggling |
| `test_mod` | `algo_mod.c` | Chorus, Vibrato, Flanger, Phaser, Tremolo/Pan, Rotary, Ring Mod: bounded output, tails, toggling |
| `test_delay` | `algo_delay.c` | Digital, Tape, Mod, Ping-Pong, Reverse delay: bounded output at extremes, parameter jumps, time sweeps |
| `test_pitch` | `algo_pitch.c` | Octaver, Harmonizer, Detune: voices bounded by dry + a few unity voices, parameter jumps |
| `test_looper` | `algo_looper.c` | state machine, capacity, playback modes (reverse/half), command chaos, stability |
| `test_reverb` | `algo_reverb.c` | Room, Hall, Plate, Spring, Shimmer: tail decay, wet audible, toggling |
| `test_chain` | whole engine | defaults, arena placement, move keeps loop, toggles, tails, config/params, default-chain WAV |

Result 2026-11-12: all 7 suites pass; `test_chain`: 15129 checks, 0 failed.

## 2. Python tests and GUI selftest
```bash
cd tools
python -m unittest test_gfx_link          # 24 tests
python gfx_gui.py --sim --selftest        # scripted ~4 s smoke test against the simulator
```
`test_gfx_link.py` covers the Scope protocol/simulator, discovery (algorithm table matches `fx_algo_table.h`), editing, memory/CPU rejection, looper state machine, presets, JSON round trip, late-reply timeout and `make_sample.py` output. Result 2026-11-12: 24 pass; selftest passes (opens the Scope window and receives frames from the simulator). The Scope window has never been viewed by a human.

## 3. Hardware test procedure
Prerequisites: firmware flashed ([development](development.md#3-flash--deploy)), CN5 connected, headphones/amp on CN4.

0. **Audio (confirmed 2026-11-12, user report):** effects are audible through the headphone jack; no quality measurement.
1. **Audio:** after reset the green LED is on and the sample loops (default chain). Listen for clean output, no clicks/dropouts. Short press B1 stops/starts it; tails keep ringing after stop.
2. **USB enumeration:** blue LED turns solid; the PC lists a virtual COM port (VID `0x0483`, PID `0x5740`).
3. **Protocol smoke:** run a scripted session or the GUI: `PING`, `ALGOS`, `STATE`, `SLOT/ON/SET/SETG/MOVE/DEFAULTS`, error replies, `LOOP`, `PSAVE/PLIST/PLOAD/PDEL`.
4. **CPU measurement for heavy chains:** `STATUS` repeatedly while the sample plays; record max `cpu` (method below).
5. **Preset save/power-cycle:** `PSAVE`, unplug both USB cables, power up, check that the last loaded/saved preset is active (`STATE` `PRESET` line).
6. **Looper:** `LOOP n REC`, play, `REC` (close), `DUB`, `CLEAR`; listen to loop quality (mu-law 24 kHz).
7. **Button:** short press = play/stop (`EVT PLAY`), long press >= 700 ms = looper record.
8. **LEDs:** orange = bypass, red = clip, green blink = recording.
9. **Scope:** see [Scope](#scope).

### Protocol smoke test (2026-11-12)
Scripted raw pyserial session over COM13 (USB Serial Device, inbox driver). Confirmed:
- `PING` -> `PONG GFX 2.0`; `ALGOS` -> 28 `A` lines + 118 `AP` lines + `END`; `STATE` correct for the default chain.
- `SLOT`, `ON`, `SET`, `SETG`, `MOVE`, `DEFAULTS` OK; error replies OK (`ERR unknown command`, `ERR range`).
- `LOOP` REC -> REC -> DUB -> DUB -> CLEAR states correct; 1.5 s recording gave len 72192 samples; looper capacity with Overdrive alongside = 180224 samples (3.75 s).
- `PSAVE` (1.02 s including reply), `PLIST`, `PLOAD`, `PDEL` OK.

Preset persistence: a preset saved from the GUI survives a power cycle (unplug/replug), confirmed by the user 2026-11-12 (user report). Other GUI functions on the board: preliminary.

### Scope
Steps (GUI: Scope button or key `S`):
1. Start playback; `SCOPE 20 1` then poll `SCOPEDATA` until `SD <decim> 256 <triggered>`.
2. Check `decim`, 256 points per channel, trigger flag; try `SCOPE 2000 0` (free-run).
3. With the default chain the `l`/`r` traces differ from `in`.
4. In the GUI check time base 5 ms-5 s, trigger, Linear/dB (-60..0 dBFS), zoom x1-x8, Run/Freeze/Single, traces (IN grey, OUT L cyan, OUT R magenta), min/max envelope when `decim` > 1.

Results 2026-11-12 (scripts on COM13): 20 ms triggered capture ready after 0.20 s (`SD 4 256 1`); 2000 ms free-run ready after 2.07 s (`SD 375 256 0`); output differs from input with the default chain; through `gfx_link.Connection` trigger True and 256 points per channel. Simulator and GUI selftest pass. Step 4 was not done by a human: the Scope window has not been viewed yet.

### CPU measurements
#### Method
`STATUS` `cpu` = DSP time of the last 256-frame block / block period x 100. Per chain: max of 5 readings about 150 ms apart, sample playing, firmware `-O2`, 168 MHz. Estimated cost includes 250 overhead; budget 2400 cycles/frame (68.6 % of 3500).

#### Results (2026-11-12)
Raw data: [measurements/2026-11-12_cpu_v2.csv](measurements/2026-11-12_cpu_v2.csv).

| Chain | Est. cost | Measured cpu max | Mem used / 153600 B |
|---|---|---|---|
| Overdrive, Chorus (off), Digital Delay, Hall (default) | 1660 | 41 % | 114928 |
| Compressor, Phaser, Ping-Pong, Plate | 1840 | 49 % | 110800 |
| Fuzz, Rotary, Tape Echo, Shimmer | 2010 | 54 % | 105328 |
| Octaver, Harmonizer, Detune, Spring | 2100 | 58 % | 40168 |
| Overdrive, Flanger, Mod Delay, Hall | 1750 | 48 % | 114928 |
| Auto-Wah, Chorus, Reverse Delay, Room | 1540 | 35 % | 99576 |
| Overdrive, Looper | 660 | 12 % | 90112 |

Conclusion: cost estimates are conservative (worst measured 58 % at cost 2100, budget 68.6 %), all within budget. Error bound: reading granularity 1 %; short window (about 1 s per chain); pitch re-alignment spikes may not have been captured, so this is not a proven worst case.

## Milestone checklist
| Item | M1 | M2 |
|---|---|---|
| Host tests (7 suites) | pass | - |
| Python tests (24) + GUI selftest | pass | - |
| Firmware builds, zero warnings | pass | - |
| Flash via ST-LINK | - | pass |
| Firmware runs, I2S DMA streaming | - | pass (SWD registers) |
| USB enumeration | - | pass |
| Protocol v2 smoke | - | pass |
| CPU measurement (7 chains) | - | pass |
| Audible output | - | pass (user report) |
| Preset save from GUI + power cycle | - | pass (user report) |
| Other GUI functions on real board | - | preliminary |
| Scope over USB (scripts) | - | pass |
| Scope window viewed by a human | - | not yet verified |
| Button | - | not yet verified |
| Looper audio | - | not yet verified |
