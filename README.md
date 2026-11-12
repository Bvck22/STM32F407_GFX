**English** | [Tiếng Việt](README.vi.md)

# GFX

Guitar multi-effects processor on an STM32F4DISCOVERY (STM32F407VGT6): a 4-slot serial effect chain with 27 algorithms (delay, modulation, pitch, reverb, drive, looper), 16 flash presets, a scope (waveform viewer), 48 kHz / 16-bit stereo output through the on-board CS43L22 codec, and a USB CDC control link. A Python/Tk GUI styled like a hardware multi-effects pedal (inspired by the Eventide H9 Max, no affiliation) controls the board or a built-in simulator. The audio source is a built-in test sample stored in flash; there is no guitar input yet.

**Status:** M1 firmware + tools code complete; M2 hardware bring-up in progress (2026-11-12): USB enumerates as COM13, the protocol v2 smoke test passes on the board (including preset save/load/delete and the looper), and the measured DSP load is 12-58 % for the tested chains. User-confirmed: effects are audible through the headphone jack and a preset saved from the GUI survives a power cycle. Not yet verified: button, audio quality measurements, other GUI functions on the board (preliminary), the Scope window (never viewed by a human). Roadmap: [docs/plan-roadmap.md](docs/plan-roadmap.md).

## Layout
```
firmware/            STM32 firmware (Makefile project, arm-none-eabi-gcc)
  Core/Inc, Core/Src   engine, audio output, USB commands, presets, main
  Core/Src/algos/      effect algorithms (delay, dyn, looper, mod, pitch, reverb)
  USB_DEVICE/          USB CDC device
  Drivers, Middlewares ST HAL, BSP (CS43L22), USB device library
  test/host/           PC unit tests for the algorithms and the chain
tools/               Python: GUI, serial link + simulator, sample generator, tests
docs/                documentation (index below), measurements/ raw data
```

## Quick start
Flash (Git Bash; toolchain paths in [docs/development.md](docs/development.md)):
```bash
cd firmware
make -j8
make flash
```
GUI (Python + pyserial; CN5 micro-USB connected, or use the simulator), from the repo root:
```bash
pip install -r tools/requirements.txt
python tools/gfx_gui.py --sim            # simulator
python tools/gfx_gui.py --port COM13     # board (use your COM port)
```

## Documentation
| Document | Contents |
|---|---|
| [docs/plan-roadmap.md](docs/plan-roadmap.md) | Milestones M1-M3, checklists, risks |
| [docs/architecture.md](docs/architecture.md) | Block diagram, modules, memory map, concurrency, decisions D1-D11 |
| [docs/hardware.md](docs/hardware.md) | Board, connectors, pin map, clocks, flash map, verification |
| [docs/development.md](docs/development.md) | Toolchain, build, flash, CubeIDE import, adding an algorithm |
| [docs/testing.md](docs/testing.md) | Host tests, Python tests, hardware test procedure, CPU measurements, checklist |
| [docs/protocol.md](docs/protocol.md) | USB ASCII control protocol v2 |
| [CHANGELOG.md](CHANGELOG.md) | Changes and known issues |

## Board quick reference
| Control | Function |
|---|---|
| B1 (PA0) short press | play / stop the test sample |
| B1 long press (>= 700 ms) | looper record (fires while held) |
| Green LED (PD12) | on = playing; fast blink = looper recording |
| Orange LED (PD13) | on = bypass |
| Red LED (PD14) | output reached full scale (clip) |
| Blue LED (PD15) | solid = USB configured; blink = not configured |
| GUI key `S` / Scope button | open the Scope window (waveform viewer) |
| CN5 (micro-USB) | control port (CDC); CN1 (mini-USB) is ST-LINK |

The button behavior is read from `firmware/Core/Src/main.c`; it is not yet verified on hardware.
