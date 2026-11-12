**English** | [Tiếng Việt](hardware.vi.md)

# Hardware

## Board
STM32F4DISCOVERY (MB997): STM32F407VGT6 (Cortex-M4F, 1 MB flash, 192 KB RAM = 128 KB + 64 KB CCM), on-board ST-LINK, CS43L22 audio DAC/codec (output only), user button and 4 LEDs. No other parts are required.

### Connectors
| Connector | Use |
|---|---|
| CN1 (mini-USB) | ST-LINK: power, SWD flashing/debug |
| CN5 (micro-USB) | USB OTG FS = control port (CDC virtual COM) |
| CN4 (3.5 mm jack) | audio output (headphones/line) from the CS43L22 |

## Pin map
The firmware (`audio_out.c`, `usbd_conf.c`, `main.c`, BSP) is the source of truth; this table must match it.

| Function | Pin | Notes |
|---|---|---|
| I2S3 MCK | PC7 | AF6 |
| I2S3 SCK | PC10 | AF6 |
| I2S3 SD | PC12 | AF6 |
| I2S3 WS | PA4 | AF6 |
| I2C1 SCL / SDA | PB6 / PB9 | codec control |
| Codec reset | PD4 | |
| USB OTG FS DM / DP | PA11 / PA12 | VBUS sensing off |
| User button B1 | PA0 | short = play/stop, long (>= 700 ms) = looper record |
| LED green (LED4) | PD12 | on = playing; fast blink (100 ms half-period) = looper recording |
| LED orange (LED3) | PD13 | on = bypass |
| LED red (LED5) | PD14 | on = output reached full scale (clip); blinks forever in `Error_Handler` |
| LED blue (LED6) | PD15 | solid = USB configured; blink (500 ms half-period) = not configured |

## Clocks
| Clock | Setting |
|---|---|
| HSE | 8 MHz crystal |
| SYSCLK | 168 MHz (PLL from HSE) |
| USB | 48 MHz |
| Audio | PLLI2S N=258, R=3 -> 48 kHz sample rate (I2S3, 16-bit stereo) |

One frame = 168 MHz / 48 kHz = 3500 cycles; block = 256 frames.

## Flash map
| Region | Address | Use |
|---|---|---|
| Sectors 0..10 | `0x08000000` ... | code + test sample (~576 KB); linker `ROM` limited to 896 KB |
| Sector 11 | `0x080E0000` (128 KB) | presets (see [architecture](architecture.md#d10--preset-current-index-marker-log)) |

The linker script `firmware/STM32F407VGTX_FLASH.ld` is the source of truth. Build usage 2026-11-12: ROM 683004 B (74.4 %).

## Verification status
| Item | Status | Date / evidence |
|---|---|---|
| Flash over ST-LINK | confirmed | 2026-11-12 "Download verified successfully" |
| Firmware runs | confirmed | 2026-11-12 SWD: no error handler, green LED on |
| I2S3 TX DMA streaming | confirmed (registers) | 2026-11-12 SWD: DMA enabled circular, NDTR changing |
| USB enumeration | confirmed | 2026-11-12 Windows 10 "USB Serial Device (COM13)", VID `0x0483` PID `0x5740`, inbox driver |
| Blue LED not-configured blink | confirmed (before CN5 was connected) | 2026-11-12 SWD/visual |
| Protocol v2 over USB | confirmed | 2026-11-12 scripted pyserial smoke test ([testing](testing.md#protocol-smoke-test-2026-11-12)) |
| Flash write (`PSAVE`) | confirmed | 2026-11-12 1.02 s incl. reply |
| DSP load | measured | 2026-11-12, 12-58 % ([testing](testing.md#cpu-measurements)) |
| Audible output (headphone jack) | confirmed (user report, no quality measurement) | 2026-11-12 |
| Preset save from GUI + power-cycle persistence | confirmed (user report) | 2026-11-12 |
| Other GUI functions on real board | preliminary (implied by the preset save) | 2026-11-12 |
| Scope capture over USB | confirmed (scripts; window never viewed by a human) | 2026-11-12 ([testing](testing.md#scope)) |
| Button B1 | not yet verified | |
| Looper audio | not yet verified | protocol states verified only |
| Guitar line input | not available | CS43L22 is output-only; M3 |
