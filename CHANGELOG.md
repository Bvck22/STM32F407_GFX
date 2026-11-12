**English** | [Tiếng Việt](CHANGELOG.vi.md)

# Changelog

Format follows [Keep a Changelog](https://keepachangelog.com/); versions follow SemVer.

## [Unreleased]

v2 (2026-11-12): firmware and tools code complete; hardware bring-up (M2) in progress.

### Added
- 4-slot serial effect chain (`fx_engine`) with in/out level and bypass globals.
- 27 algorithms in 6 categories, defined by the X-macro catalogue `fx_algo_table.h`.
- Looper (8-bit mu-law, 24 kHz mono, reverse and half-speed).
- USB CDC ASCII control protocol v2: [docs/protocol.md](docs/protocol.md).
- 16 presets in flash sector 11, current preset restored at boot.
- Pedal-style Tk GUI (`tools/gfx_gui.py`) and a firmware-equivalent simulator (`tools/gfx_sim.py`).
- Host test suites (7) and Python unit tests (24).
- Scope (waveform viewer): firmware `scope.c`, protocol `SCOPE`/`SCOPEDATA`, GUI window `tools/gfx_scope.py` (button or key `S`), simulator support.
- Bilingual documentation.

### Changed
- Default chain: Overdrive (on), Chorus (off), Digital Delay (on), Hall (on).

### Verified on hardware (2026-11-12)
- USB enumerates on Windows 10 as "USB Serial Device (COM13)", VID `0x0483` PID `0x5740`, inbox driver.
- Protocol v2 smoke test over COM13 passes (discovery, editing, errors, looper, `PSAVE`/`PLIST`/`PLOAD`/`PDEL`); details in [docs/testing.md](docs/testing.md).
- User-confirmed: effects audible through the headphone jack; a preset saved from the GUI survives a power cycle (user report, no quality measurement).
- Scope capture over USB: 20 ms triggered capture ready after 0.20 s, 2000 ms free-run ready after 2.07 s, output differs from input with the default chain; `gfx_link.Connection` against COM13 returns 256 points per channel.
- Measured DSP load 12-58 % for 7 tested chains; the cost estimates are conservative ([docs/testing.md](docs/testing.md)).

### Known issues
- Not yet verified on hardware: button, audio quality, GUI functions other than preset save (preliminary).
- The Scope window has not been viewed by a human yet (GUI selftest passes against the simulator).
- RAM 93.82 % (122976 B) and CCMRAM 96.9 % full; little headroom for new features.
- No guitar input: the CS43L22 on the Discovery is output-only (see M3 in [docs/plan-roadmap.md](docs/plan-roadmap.md)).
