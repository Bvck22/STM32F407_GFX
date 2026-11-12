**English** | [Tiếng Việt](development.vi.md)

# Development environment & workflow

## 1. Setup
| Component | Version | Notes |
|---|---|---|
| STM32CubeIDE | 2.2.0 | bundled GNU Tools for STM32 14.3 (`arm-none-eabi-gcc`), its `make`, `STM32_Programmer_CLI` 2.2.5; paths are set at the top of `firmware/Makefile` (`CUBEIDE_DIR`, `TOOLCHAIN_DIR`, `PROGRAMMER`) |
| Python | 3.15 | `pip install -r tools/requirements.txt` (pyserial); Tk is part of the standard install |
| MSYS2 UCRT64 gcc | `C:\msys64\ucrt64` | host tests only |
| Shell | Git Bash | all commands below |

Override tool locations on the command line, e.g. `make GCC_PATH=/opt/arm-gnu/bin PROGRAMMER=STM32_Programmer_CLI`. `make V=1` shows full command lines.

## 2. Build
From the repo root (Git Bash), using CubeIDE's `make` on `PATH` (or any GNU make):
```bash
cd firmware
make -j8          # build/gfx.elf, .hex, .bin + size report
make clean
```
Flags: Cortex-M4 hard-float, `-O2 -g3 -Wall -std=gnu11`, `--gc-sections`, `-u _printf_float`. The build must stay warning-free. Build output 2026-11-12: ROM 683004 B / 896 KB (74.4 %), RAM 122976 B / 128 KB (93.82 %), CCMRAM 62 KB / 64 KB (96.9 %).

## 3. Flash / deploy
Connect CN1 (ST-LINK), then:
```bash
cd firmware
make flash        # SWD via STM32_Programmer_CLI; ends with "Download verified successfully"
```
Control port: connect CN5 (micro-USB) to the PC; the board enumerates as a virtual COM port.

## 4. Debugging
- Import into STM32CubeIDE: File -> Import -> C/C++ -> **Existing Code as Makefile Project**, location `firmware/`, toolchain **MCU ARM GCC**. Build with the project's Makefile.
- Debug with the **ST-LINK GDB server** and `firmware/build/gfx.elf` (create a debug configuration of type STM32 C/C++ Application / ST-LINK GDB server).
- Without a debugger: red LED blinking forever means `Error_Handler`; `STATUS` over USB reports `cpu`, `mem`, `cost`.

## 5. Tools and tests
Host tests (algorithms and chain, on the PC):
```bash
cd firmware/test/host && PATH=/c/msys64/ucrt64/bin:$PATH TMP=$LOCALAPPDATA/Temp mingw32-make
```
`TMP` must be set, otherwise gcc fails with "Cannot create temporary file in C:\Windows". One suite: `... mingw32-make test_mod`. Details: [testing](testing.md).

Python tests and GUI:
```bash
cd tools
python -m unittest test_gfx_link
python gfx_gui.py --sim --selftest
python gfx_gui.py --sim            # or --port COM13
```
GUI keys: `1`-`4` select slot, `Space` play/stop, `B` bypass, `R` looper REC, `S` open Scope (ignored while a text field has focus).

Regenerate the test sample (written to `firmware/Core/Src/sample_data.c`, at most 8 s; default synthesized 6 s phrase = 576 KB flash):
```bash
python tools/make_sample.py
python tools/make_sample.py --wav file.wav --start S --seconds N
```
The ROM limit (896 KB) bounds the sample length; rebuild and re-flash afterwards.

## 6. Adding a new algorithm
1. Add one `A(id, SUFFIX, "Name", category)` line to `FX_ALGO_LIST` in [fx_algo_table.h](../firmware/Core/Inc/fx_algo_table.h) with the next free id (append only: ids are stored in presets) and a `FX_PARAMS_<SUFFIX>(P)` block (max 8 params, one entry per line).
2. Implement `init/update/process` (and optional `command/get_status`) in a `firmware/Core/Src/algos/algo_*.c` file and define the `fx_algo_t` descriptor at its end: `id`, `mem_bytes`, `cost` (cycles per frame; estimate, then measure), `tails`. State must fit `FX_STATE_BYTES` (`_Static_assert`); large buffers come from the arena and `init()` must clear them.
3. Registration is automatic (`fx_algo_registry.c` reads the table); the simulator and GUI pick up ids, names and parameters from the header. The simulator keeps its own copy of `(mem_bytes, cost)` per id (table at the top of `tools/gfx_sim.py`): add the new algorithm there too.
4. Add a host test (a `test_*.c` suite or a case in an existing one) and run Python tests (`test_algos_match_header`).
5. Measure with `STATUS` on the board and record in [testing](testing.md#cpu-measurements).

## 7. Workflow
Not a git repository for now; keep these rules when it becomes one.

### Before merging (Definition of Done)
- [ ] firmware builds, no new warnings
- [ ] host tests and Python tests pass
- [ ] tested on hardware if relevant (results recorded in [testing](testing.md) / [hardware](hardware.md))
- [ ] source-of-truth files and their mirrored docs updated together (`fx_algo_table.h` -> architecture catalogue; pin/clock code -> hardware)
- [ ] CHANGELOG `[Unreleased]` + roadmap checklist updated
- [ ] both languages updated, `python <skill-dir>/scripts/check_docs.py .` clean

### New technical decisions
Append a `D<n>` record to [architecture](architecture.md) (Why / Alternatives / Trade-off); never delete a reversed one, mark it superseded.
