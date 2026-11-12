[English](development.md) | **Tiếng Việt**

# Môi trường phát triển & quy trình

## 1. Cài đặt
| Thành phần | Phiên bản | Ghi chú |
|---|---|---|
| STM32CubeIDE | 2.2.0 | kèm GNU Tools for STM32 14.3 (`arm-none-eabi-gcc`), `make` của nó, `STM32_Programmer_CLI` 2.2.5; đường dẫn đặt ở đầu `firmware/Makefile` (`CUBEIDE_DIR`, `TOOLCHAIN_DIR`, `PROGRAMMER`) |
| Python | 3.15 | `pip install -r tools/requirements.txt` (pyserial); Tk có sẵn trong bản cài chuẩn |
| MSYS2 UCRT64 gcc | `C:\msys64\ucrt64` | chỉ cho host test |
| Shell | Git Bash | mọi lệnh bên dưới |

Ghi đè vị trí công cụ trên dòng lệnh, ví dụ `make GCC_PATH=/opt/arm-gnu/bin PROGRAMMER=STM32_Programmer_CLI`. `make V=1` hiện đầy đủ dòng lệnh.

## 2. Build
Từ thư mục gốc repo (Git Bash), dùng `make` của CubeIDE trên `PATH` (hoặc GNU make bất kỳ):
```bash
cd firmware
make -j8          # build/gfx.elf, .hex, .bin + báo cáo kích thước
make clean
```
Cờ: Cortex-M4 hard-float, `-O2 -g3 -Wall -std=gnu11`, `--gc-sections`, `-u _printf_float`. Build phải giữ không có warning. Kết quả build 12/11/2026: ROM 683004 B / 896 KB (74.4 %), RAM 122976 B / 128 KB (93.82 %), CCMRAM 62 KB / 64 KB (96.9 %).

## 3. Nạp firmware
Cắm CN1 (ST-LINK), rồi:
```bash
cd firmware
make flash        # SWD qua STM32_Programmer_CLI; kết thúc bằng "Download verified successfully"
```
Cổng điều khiển: cắm CN5 (micro-USB) vào PC; board enumerate thành cổng COM ảo.

## 4. Gỡ lỗi
- Import vào STM32CubeIDE: File -> Import -> C/C++ -> **Existing Code as Makefile Project**, thư mục `firmware/`, toolchain **MCU ARM GCC**. Build bằng Makefile của dự án.
- Debug bằng **ST-LINK GDB server** và `firmware/build/gfx.elf` (tạo debug configuration loại STM32 C/C++ Application / ST-LINK GDB server).
- Không có debugger: LED đỏ nháy mãi nghĩa là `Error_Handler`; `STATUS` qua USB báo `cpu`, `mem`, `cost`.

## 5. Công cụ và test
Host test (thuật toán và chuỗi, chạy trên PC):
```bash
cd firmware/test/host && PATH=/c/msys64/ucrt64/bin:$PATH TMP=$LOCALAPPDATA/Temp mingw32-make
```
Phải đặt `TMP`, nếu không gcc lỗi "Cannot create temporary file in C:\Windows". Chạy một suite: `... mingw32-make test_mod`. Chi tiết: [testing](testing.vi.md).

Test Python và GUI:
```bash
cd tools
python -m unittest test_gfx_link
python gfx_gui.py --sim --selftest
python gfx_gui.py --sim            # hoặc --port COM13
```
Phím GUI: `1`-`4` chọn slot, `Space` play/stop, `B` bypass, `R` looper REC, `S` mở Scope (bị bỏ qua khi ô nhập văn bản đang có focus).

Tạo lại mẫu thử (ghi vào `firmware/Core/Src/sample_data.c`, tối đa 8 s; mặc định đoạn nhạc tổng hợp 6 s = 576 KB flash):
```bash
python tools/make_sample.py
python tools/make_sample.py --wav file.wav --start S --seconds N
```
Giới hạn ROM (896 KB) chặn độ dài mẫu; build và nạp lại sau đó.

## 6. Thêm thuật toán mới
1. Thêm một dòng `A(id, SUFFIX, "Name", category)` vào `FX_ALGO_LIST` trong [fx_algo_table.h](../firmware/Core/Inc/fx_algo_table.h) với id trống kế tiếp (chỉ thêm vào cuối: id được lưu trong preset) và một khối `FX_PARAMS_<SUFFIX>(P)` (tối đa 8 tham số, mỗi mục một dòng).
2. Cài `init/update/process` (và tùy chọn `command/get_status`) trong một file `firmware/Core/Src/algos/algo_*.c` và định nghĩa descriptor `fx_algo_t` ở cuối: `id`, `mem_bytes`, `cost` (cycle mỗi frame; ước lượng rồi đo), `tails`. State phải vừa `FX_STATE_BYTES` (`_Static_assert`); buffer lớn lấy từ arena và `init()` phải xóa chúng.
3. Đăng ký là tự động (`fx_algo_registry.c` đọc bảng); simulator và GUI lấy id, tên, tham số từ header. Simulator giữ bản sao riêng `(mem_bytes, cost)` theo id (bảng ở đầu `tools/gfx_sim.py`): thêm thuật toán mới vào đó nữa.
4. Thêm host test (một suite `test_*.c` hoặc một case trong suite có sẵn) và chạy test Python (`test_algos_match_header`).
5. Đo bằng `STATUS` trên board và ghi vào [testing](testing.vi.md#đo-cpu).

## 7. Quy trình làm việc
Hiện chưa phải git repository; giữ các quy tắc này khi nó trở thành repo.

### Trước khi merge (Definition of Done)
- [ ] firmware build được, không có warning mới
- [ ] host test và test Python pass
- [ ] đã test trên phần cứng nếu liên quan (ghi kết quả vào [testing](testing.vi.md) / [hardware](hardware.vi.md))
- [ ] file nguồn chuẩn và tài liệu phản chiếu nó được cập nhật cùng nhau (`fx_algo_table.h` -> catalogue trong architecture; mã chân/clock -> hardware)
- [ ] CHANGELOG `[Unreleased]` + checklist roadmap đã cập nhật
- [ ] cả hai ngôn ngữ đã cập nhật, `python <skill-dir>/scripts/check_docs.py .` sạch

### Quyết định kỹ thuật mới
Thêm bản ghi `D<n>` vào [architecture](architecture.vi.md) (Lý do / Phương án khác / Đánh đổi); không bao giờ xóa quyết định đã bị đảo ngược, đánh dấu là superseded.
