[English](protocol.md) | **Tiếng Việt**

# Giao thức điều khiển USB GFX — v2

Đường truyền: USB CDC-ACM (cổng COM ảo) trên **cổng micro-USB CN5** của STM32F4-Discovery (USB OTG FS, PA11/PA12). VID `0x0483`, PID `0x5740`. Cài đặt baud rate bị bỏ qua.

Đóng khung: các dòng ASCII. Lệnh từ host kết thúc bằng `\n` (`\r` ở cuối bị bỏ qua). Phản hồi từ thiết bị kết thúc bằng `\n`. Từ khóa lệnh không phân biệt hoa/thường, tên thì phân biệt. Tối đa 63 ký tự mỗi dòng lệnh, vượt quá trả `ERR line too long`. Các trường cách nhau bằng một dấu cách. Trường nào có thể chứa dấu cách (tên thuật toán hoặc preset) luôn là trường **cuối** của dòng. Số in theo kiểu `%g` (`0.35`, `350`, `-6.5`). Trường văn bản thiếu in là `-`.

Slot đánh số `0..3`. Id thuật toán lấy từ `firmware/Core/Inc/fx_algo_table.h` (ổn định, được lưu trong preset). Chỉ số tham số = vị trí trong danh sách tham số của thuật toán.

## Khám phá

| Host | Thiết bị |
|---|---|
| `PING` | `PONG GFX 2.0` |
| `ALGOS` | với mỗi thuật toán (theo thứ tự id, gồm Empty): `A <id> <category> <nparams> <name...>`, rồi các tham số `AP <id> <idx> <key> <min> <max> <step> <def> <unit> <labels>`; cuối cùng `END` |
| `STATE` | `G <name> <min> <max> <step> <def> <value>` cho mỗi global, `SLOT <n> <algo_id> <on>` cho mỗi slot, `V <n> <idx> <value>` cho mỗi tham số của từng slot không rỗng, `PRESET <index> <name...>` (preset hiện tại, `-1 -` nếu không có), rồi `END` |

`category` là một trong `None Delay Modulation Pitch Reverb Drive Looper`. `unit` / `labels` là `-` khi rỗng; `labels` phân tách bằng `|` (tên lựa chọn cho các giá trị `min, min+step, ...`).

## Chỉnh sửa

| Host | Thiết bị |
|---|---|
| `SETG <name> <value>` | `OK G <name> <applied>` |
| `SLOT <n> <algo_id>` | `OK SLOT <n> <algo_id>` rồi các dòng `V <n> <idx> <value>` của slot mới, rồi `END` — hoặc `ERR memory` / `ERR cpu` / `ERR range` |
| `ON <n> <0/1>` | `OK ON <n> <0/1>` |
| `SET <n> <idx> <value>` | `OK V <n> <idx> <applied>` |
| `MOVE <from> <to>` | `OK MOVE <from> <to>` (bỏ tại `from`, chèn tại `to`) — hoặc `ERR memory` |
| `DEFAULTS` | `OK DEFAULTS` (host đọc lại `STATE`) |

## Phát mẫu (nguồn thử)

| Host | Thiết bị |
|---|---|
| `PLAY` | `OK PLAY` (phát lại từ đầu, lặp) |
| `STOP` | `OK STOP` (nguồn bị tắt tiếng; phần đuôi hiệu ứng vẫn ngân) |

## Looper

Looper là thuật toán `Looper` đặt trong một slot.

| Host | Thiết bị |
|---|---|
| `LOOP <n> REC\|DUB\|PLAY\|STOP\|CLEAR` | `OK LOOP <n> <state>` — hoặc `ERR no looper` |

`state` là một trong `empty recording playing overdub stopped`. `REC`: empty → recording; recording → đóng vòng và phát. `DUB` bật/tắt overdub khi đang phát.

## Preset (flash, 16 slot)

| Host | Thiết bị |
|---|---|
| `PLIST` | `PR <index> <name...>` cho mỗi preset đã dùng (index `0..15`), rồi `END` |
| `PSAVE <index> <name...>` | `OK PSAVE <index>` — hoặc `ERR flash` (tên ≤ 15 ký tự, cho phép dấu cách) |
| `PLOAD <index>` | `OK PLOAD <index>` (host đọc lại `STATE`) — hoặc `ERR no preset` / `ERR memory` |
| `PDEL <index>` | `OK PDEL <index>` |

Lưu preset ghi lại sector 11 của flash: âm thanh bị tắt và USB tạm dừng khoảng 1–2 s. Chỉ số preset được nạp hoặc lưu gần nhất được khôi phục khi khởi động.

## Trạng thái

`STATUS` → `STAT play=<0/1> bypass=<0/1> cpu=<pct> peak=<0..1> pos=<sample> len=<samples> mem=<used>/<total> cost=<used>/<budget> loop=<slot>:<state>:<pos>:<len>:<max>`

- `cpu`: thời gian DSP đo được của block gần nhất ÷ chu kỳ block × 100.
- `peak`: giá trị tuyệt đối cao nhất của đầu ra kể từ lần `STATUS` trước (sau đó được đặt lại).
- `loop=-` khi không có slot nào chứa looper; vị trí/độ dài tính bằng sample ở 48 kHz.

## Scope (bắt dạng sóng)

256 điểm trên ba kênh: `in` (block nguồn mono, trước `in.level`), `l` và `r` (đầu ra cuối). Mỗi điểm là min và max của `decim` sample liên tiếp, `decim = max(1, round(window_ms * 48 / 256))`, `window_ms` được kẹp trong khoảng 5..5000. Sample là byte G.711 µ-law (giải mã bằng bảng µ-law chuẩn, bias 0x84).

| Host | Thiết bị |
|---|---|
| `SCOPE <window_ms> <0/1>` | `OK SCOPE <decim> 256`; đối số thứ hai yêu cầu trigger. Arm lại sẽ bỏ lần bắt trước. |
| `SCOPEDATA` | `SD idle` (chưa từng arm) hoặc `SD busy` (chưa xong); khi sẵn sàng: `SD <decim> 256 <triggered 0/1>`, rồi với mỗi kênh `in l r` và offset `0 128`: `SC <ch> <offset> <hex>`, rồi `END` |

- `hex` = 128 điểm × (byte min, byte max) dạng 512 chữ số hex viết hoa.
- Trigger: bắt đầu bắt tại lần cắt không đi lên đầu tiên của `in` sau khi tín hiệu đã xuống dưới −0.02 FS rồi vượt lên trên +0.02 FS. Nếu không xảy ra trong max(50 ms, window), thiết bị chạy tự do và `triggered` là `0`.
- Không yêu cầu trigger thì bắt ngay lập tức (`triggered` = `0`).
- Dữ liệu vẫn đọc được (lặp lại `SCOPEDATA`) cho đến lần `SCOPE` kế tiếp.

## Lỗi

`ERR unknown command`, `ERR bad value`, `ERR range`, `ERR memory`, `ERR cpu`, `ERR no looper`, `ERR no preset`, `ERR flash`, `ERR line too long`.

## Sự kiện không yêu cầu

Có thể xuất hiện giữa các phản hồi; host bỏ qua các dòng `EVT` không biết.

| Dòng | Ý nghĩa |
|---|---|
| `EVT PLAY <0/1>` | phát mẫu được bật/tắt bằng nút nhấn của người dùng |
| `EVT LOOP <n> <state>` | looper tự đổi trạng thái (ví dụ ghi đầy dung lượng) |
