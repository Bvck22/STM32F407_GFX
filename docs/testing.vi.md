[English](testing.md) | **Tiếng Việt**

# Kiểm thử

## 1. Host test (PC, thuật toán và chuỗi)
```bash
cd firmware/test/host && PATH=/c/msys64/ucrt64/bin:$PATH TMP=$LOCALAPPDATA/Temp mingw32-make
```
Một suite: `... mingw32-make test_mod`. Biên dịch với `-Wall -Wextra -Wdouble-promotion -Wshadow -Werror`. Harness chạy một descriptor trên mẫu trong flash theo block stereo float 256 frame như trên target, với state và bộ nhớ arena được điền rác trước để việc thiếu xóa trong `init()` lộ ra thành nhiễu/NaN. File WAV render nằm ở `firmware/test/host/out/` để nghe thử.

| Suite | Liên kết với | Kiểm tra gì |
|---|---|---|
| `test_dyn` | `algo_dyn.c` | Overdrive, Fuzz, Bit Crusher, Compressor, Auto-Wah, EQ: đầu ra bị chặn ở các cực trị tham số, ổn định, bật/tắt |
| `test_mod` | `algo_mod.c` | Chorus, Vibrato, Flanger, Phaser, Tremolo/Pan, Rotary, Ring Mod: đầu ra bị chặn, tails, bật/tắt |
| `test_delay` | `algo_delay.c` | Digital, Tape, Mod, Ping-Pong, Reverse delay: đầu ra bị chặn ở cực trị, nhảy tham số, quét time |
| `test_pitch` | `algo_pitch.c` | Octaver, Harmonizer, Detune: các voice bị chặn bởi dry + vài voice unity, nhảy tham số |
| `test_looper` | `algo_looper.c` | state machine, dung lượng, chế độ phát (reverse/half), lệnh ngẫu nhiên, ổn định |
| `test_reverb` | `algo_reverb.c` | Room, Hall, Plate, Spring, Shimmer: đuôi tắt dần, phần wet nghe được, bật/tắt |
| `test_chain` | toàn bộ engine | mặc định, đặt arena, move giữ loop, bật/tắt, tails, config/tham số, WAV chuỗi mặc định |

Kết quả 12/11/2026: cả 7 suite pass; `test_chain`: 15129 check, 0 failed.

## 2. Test Python và selftest GUI
```bash
cd tools
python -m unittest test_gfx_link          # 24 test
python gfx_gui.py --sim --selftest        # smoke test theo script ~4 s với simulator
```
`test_gfx_link.py` bao gồm giao thức/simulator Scope, khám phá (bảng thuật toán khớp `fx_algo_table.h`), chỉnh sửa, từ chối do bộ nhớ/CPU, state machine looper, preset, JSON round trip, timeout phản hồi muộn và đầu ra của `make_sample.py`. Kết quả 12/11/2026: 24 pass; selftest pass (mở cửa sổ Scope và nhận frame từ simulator). Cửa sổ Scope chưa từng được người xem trực tiếp.

## 3. Quy trình test phần cứng
Điều kiện: đã nạp firmware ([development](development.vi.md#3-nạp-firmware)), đã cắm CN5, tai nghe/ampli ở CN4.

0. **Âm thanh (đã xác nhận 12/11/2026, báo cáo người dùng):** hiệu ứng nghe được qua jack tai nghe; không có số đo chất lượng.
1. **Âm thanh:** sau reset LED xanh lá sáng và mẫu lặp (chuỗi mặc định). Nghe xem đầu ra có sạch, không tiếng lách tách/ngắt quãng. Nhấn ngắn B1 dừng/phát lại; phần đuôi vẫn ngân sau khi dừng.
2. **USB enumeration:** LED xanh dương sáng liên tục; PC thấy một cổng COM ảo (VID `0x0483`, PID `0x5740`).
3. **Smoke giao thức:** chạy phiên theo script hoặc GUI: `PING`, `ALGOS`, `STATE`, `SLOT/ON/SET/SETG/MOVE/DEFAULTS`, phản hồi lỗi, `LOOP`, `PSAVE/PLIST/PLOAD/PDEL`.
4. **Đo CPU cho chuỗi nặng:** gọi `STATUS` liên tục khi mẫu đang phát; ghi `cpu` lớn nhất (phương pháp bên dưới).
5. **Lưu preset/tắt bật nguồn:** `PSAVE`, rút cả hai cáp USB, cấp nguồn lại, kiểm tra preset nạp/lưu gần nhất đang hoạt động (dòng `PRESET` của `STATE`).
6. **Looper:** `LOOP n REC`, phát, `REC` (đóng), `DUB`, `CLEAR`; nghe chất lượng loop (mu-law 24 kHz).
7. **Nút nhấn:** nhấn ngắn = play/stop (`EVT PLAY`), nhấn giữ >= 700 ms = looper ghi.
8. **LED:** cam = bypass, đỏ = clip, xanh lá nháy = đang ghi.
9. **Scope:** xem [Scope](#scope).

### Smoke test giao thức (12/11/2026)
Phiên pyserial thô theo script qua COM13 (USB Serial Device, driver có sẵn). Đã xác nhận:
- `PING` -> `PONG GFX 2.0`; `ALGOS` -> 28 dòng `A` + 118 dòng `AP` + `END`; `STATE` đúng với chuỗi mặc định.
- `SLOT`, `ON`, `SET`, `SETG`, `MOVE`, `DEFAULTS` OK; phản hồi lỗi OK (`ERR unknown command`, `ERR range`).
- Trạng thái `LOOP` REC -> REC -> DUB -> DUB -> CLEAR đúng; ghi 1.5 s cho len 72192 sample; dung lượng looper khi có Overdrive cùng chuỗi = 180224 sample (3.75 s).
- `PSAVE` (1.02 s gồm cả phản hồi), `PLIST`, `PLOAD`, `PDEL` OK.

Preset còn lại: preset lưu từ GUI còn sau khi tắt/bật nguồn (rút/cắm lại), người dùng xác nhận 12/11/2026 (báo cáo người dùng). Các chức năng GUI khác trên board: sơ bộ.

### Scope
Các bước (GUI: nút Scope hoặc phím `S`):
1. Bắt đầu phát; `SCOPE 20 1` rồi poll `SCOPEDATA` đến khi có `SD <decim> 256 <triggered>`.
2. Kiểm tra `decim`, 256 điểm mỗi kênh, cờ trigger; thử `SCOPE 2000 0` (free-run).
3. Với chuỗi mặc định, đường `l`/`r` khác `in`.
4. Trong GUI kiểm tra time base 5 ms-5 s, trigger, Linear/dB (-60..0 dBFS), zoom x1-x8, Run/Freeze/Single, các đường (IN xám, OUT L cyan, OUT R magenta), đường bao min/max khi `decim` > 1.

Kết quả 12/11/2026 (script trên COM13): bắt có trigger 20 ms sẵn sàng sau 0.20 s (`SD 4 256 1`); free-run 2000 ms sẵn sàng sau 2.07 s (`SD 375 256 0`); đầu ra khác đầu vào với chuỗi mặc định; qua `gfx_link.Connection` trigger True và 256 điểm mỗi kênh. Simulator và selftest GUI pass. Bước 4 chưa được người thực hiện: cửa sổ Scope chưa được xem trực tiếp.

### Đo CPU
#### Phương pháp
`cpu` của `STATUS` = thời gian DSP của block 256 frame gần nhất / chu kỳ block x 100. Mỗi chuỗi: lớn nhất trong 5 lần đọc cách nhau khoảng 150 ms, mẫu đang phát, firmware `-O2`, 168 MHz. Cost ước lượng đã gồm 250 overhead; ngân sách 2400 cycle/frame (68.6 % của 3500).

#### Kết quả (12/11/2026)
Dữ liệu thô: [measurements/2026-11-12_cpu_v2.csv](measurements/2026-11-12_cpu_v2.csv).

| Chuỗi | Cost ước lượng | cpu đo lớn nhất | Bộ nhớ dùng / 153600 B |
|---|---|---|---|
| Overdrive, Chorus (off), Digital Delay, Hall (mặc định) | 1660 | 41 % | 114928 |
| Compressor, Phaser, Ping-Pong, Plate | 1840 | 49 % | 110800 |
| Fuzz, Rotary, Tape Echo, Shimmer | 2010 | 54 % | 105328 |
| Octaver, Harmonizer, Detune, Spring | 2100 | 58 % | 40168 |
| Overdrive, Flanger, Mod Delay, Hall | 1750 | 48 % | 114928 |
| Auto-Wah, Chorus, Reverse Delay, Room | 1540 | 35 % | 99576 |
| Overdrive, Looper | 660 | 12 % | 90112 |

Kết luận: ước lượng cost là bảo thủ (đo tệ nhất 58 % ở cost 2100, ngân sách 68.6 %), tất cả nằm trong ngân sách. Sai số: độ phân giải mỗi lần đọc 1 %; cửa sổ ngắn (khoảng 1 s mỗi chuỗi); đỉnh khi pitch căn lại có thể chưa bị bắt được, nên đây không phải trường hợp tệ nhất đã được chứng minh.

## Checklist theo mốc
| Mục | M1 | M2 |
|---|---|---|
| Host test (7 suite) | pass | - |
| Test Python (24) + selftest GUI | pass | - |
| Firmware build, không warning | pass | - |
| Nạp qua ST-LINK | - | pass |
| Firmware chạy, DMA I2S phát luồng | - | pass (thanh ghi SWD) |
| USB enumeration | - | pass |
| Smoke giao thức v2 | - | pass |
| Đo CPU (7 chuỗi) | - | pass |
| Âm thanh nghe được | - | pass (báo cáo người dùng) |
| Lưu preset từ GUI + tắt/bật nguồn | - | pass (báo cáo người dùng) |
| Các chức năng GUI khác trên board thật | - | sơ bộ |
| Scope qua USB (script) | - | pass |
| Cửa sổ Scope được người xem trực tiếp | - | chưa xác minh |
| Nút nhấn | - | chưa xác minh |
| Âm thanh looper | - | chưa xác minh |
