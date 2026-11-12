**English** | [Tiếng Việt](protocol.vi.md)

# GFX USB Control Protocol — v2

Transport: USB CDC-ACM (virtual COM port) on the STM32F4-Discovery **micro-USB
connector CN5** (USB OTG FS, PA11/PA12). VID `0x0483`, PID `0x5740`.
The baud rate setting is ignored.

Framing: ASCII lines. Host commands end with `\n` (a trailing `\r` is ignored).
Device replies end with `\n`. Command keywords are case-insensitive, names are
case-sensitive. Max command line: 63 characters → otherwise `ERR line too long`.
Fields are separated by single spaces. Where a field may contain spaces (algorithm
or preset names) it is always the **last** field of the line.
Numbers print in `%g` style (`0.35`, `350`, `-6.5`). Missing text fields print as `-`.

Slots are numbered `0..3`. Algorithm ids come from `firmware/Core/Inc/fx_algo_table.h`
(stable, stored in presets). Parameter index = position in the algorithm's list.

## Discovery

| Host | Device |
|---|---|
| `PING` | `PONG GFX 2.0` |
| `ALGOS` | per algorithm (id order, Empty included): `A <id> <category> <nparams> <name...>`, then its params `AP <id> <idx> <key> <min> <max> <step> <def> <unit> <labels>`; finally `END` |
| `STATE` | `G <name> <min> <max> <step> <def> <value>` per global, `SLOT <n> <algo_id> <on>` per slot, `V <n> <idx> <value>` per param of each non-empty slot, `PRESET <index> <name...>` (current preset, `-1 -` if none), then `END` |

`category` is one of `None Delay Modulation Pitch Reverb Drive Looper`.
`unit` / `labels` are `-` when empty; `labels` is `|` separated (selector names
for values `min, min+step, ...`).

## Editing

| Host | Device |
|---|---|
| `SETG <name> <value>` | `OK G <name> <applied>` |
| `SLOT <n> <algo_id>` | `OK SLOT <n> <algo_id>` then the new slot's `V <n> <idx> <value>` lines, then `END` — or `ERR memory` / `ERR cpu` / `ERR range` |
| `ON <n> <0/1>` | `OK ON <n> <0/1>` |
| `SET <n> <idx> <value>` | `OK V <n> <idx> <applied>` |
| `MOVE <from> <to>` | `OK MOVE <from> <to>` (remove at `from`, insert at `to`) — or `ERR memory` |
| `DEFAULTS` | `OK DEFAULTS` (host re-reads `STATE`) |

## Sample playback (test source)

| Host | Device |
|---|---|
| `PLAY` | `OK PLAY` (restarts from the beginning, loops) |
| `STOP` | `OK STOP` (source muted; tails ring out) |

## Looper

The looper is the `Looper` algorithm placed in a slot.

| Host | Device |
|---|---|
| `LOOP <n> REC\|DUB\|PLAY\|STOP\|CLEAR` | `OK LOOP <n> <state>` — or `ERR no looper` |

`state` is one of `empty recording playing overdub stopped`.
`REC`: empty → recording; recording → close loop and play.
`DUB` toggles overdub while playing.

## Presets (flash, 16 slots)

| Host | Device |
|---|---|
| `PLIST` | `PR <index> <name...>` for each used preset (index `0..15`), then `END` |
| `PSAVE <index> <name...>` | `OK PSAVE <index>` — or `ERR flash` (name ≤ 15 chars, spaces allowed) |
| `PLOAD <index>` | `OK PLOAD <index>` (host re-reads `STATE`) — or `ERR no preset` / `ERR memory` |
| `PDEL <index>` | `OK PDEL <index>` |

Saving rewrites flash sector 11: audio mutes and USB pauses for ~1–2 s.
The last loaded or saved preset index is restored at boot.

## Status

`STATUS` → `STAT play=<0/1> bypass=<0/1> cpu=<pct> peak=<0..1> pos=<sample> len=<samples> mem=<used>/<total> cost=<used>/<budget> loop=<slot>:<state>:<pos>:<len>:<max>`

- `cpu`: measured DSP time of the last block ÷ block period × 100.
- `peak`: highest absolute output since the previous `STATUS` (then reset).
- `loop=-` when no slot holds a looper; positions/lengths are samples at 48 kHz.

## Scope (waveform capture)

256 points on three channels: `in` (mono source block, before `in.level`), `l` and
`r` (final output). Each point is the min and max of `decim` consecutive samples,
`decim = max(1, round(window_ms * 48 / 256))`, `window_ms` clamped to 5..5000.
Samples are G.711 µ-law bytes (decode with the standard µ-law table, bias 0x84).

| Host | Device |
|---|---|
| `SCOPE <window_ms> <0/1>` | `OK SCOPE <decim> 256`; the second argument requests a trigger. Re-arming discards any previous capture. |
| `SCOPEDATA` | `SD idle` (never armed) or `SD busy` (not finished); when ready: `SD <decim> 256 <triggered 0/1>`, then for each channel `in l r` and offset `0 128`: `SC <ch> <offset> <hex>`, then `END` |

- `hex` = 128 points × (min byte, max byte) as 512 uppercase hex digits.
- Trigger: capture starts at the first rising zero crossing of `in` after it has
  been below −0.02 FS and then rises above +0.02 FS. If none occurs within
  max(50 ms, window), capture free-runs and `triggered` is `0`.
- Without trigger the capture starts immediately (`triggered` = `0`).
- The data stays readable (repeat `SCOPEDATA`) until the next `SCOPE`.

## Errors

`ERR unknown command`, `ERR bad value`, `ERR range`, `ERR memory`, `ERR cpu`,
`ERR no looper`, `ERR no preset`, `ERR flash`, `ERR line too long`.

## Unsolicited events

May appear between replies; hosts ignore unknown `EVT` lines.

| Line | Meaning |
|---|---|
| `EVT PLAY <0/1>` | playback toggled by the user button |
| `EVT LOOP <n> <state>` | looper state changed by itself (e.g. record reached capacity) |
