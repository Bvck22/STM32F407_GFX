[English](README.md) | **Tiếng Việt**

# GFX

Bộ xử lý đa hiệu ứng guitar trên STM32F4DISCOVERY (STM32F407VGT6): chuỗi hiệu ứng nối tiếp 4 slot với 27 thuật toán (delay, modulation, pitch, reverb, drive, looper), 16 preset trong flash, scope (xem dạng sóng), đầu ra stereo 48 kHz / 16-bit qua codec CS43L22 trên board, và đường điều khiển USB CDC. GUI Python/Tk thiết kế giống một pedal đa hiệu ứng phần cứng (lấy cảm hứng từ Eventide H9 Max, không liên kết) điều khiển board hoặc bộ mô phỏng tích hợp. Nguồn âm thanh là mẫu thử lưu trong flash; chưa có ngõ vào guitar.

**Trạng thái:** M1 firmware + công cụ đã viết xong; M2 bring-up phần cứng đang tiến hành (12/11/2026): USB enumerate thành COM13, smoke test giao thức v2 pass trên board (gồm lưu/nạp/xóa preset và looper), tải DSP đo được 12-58 % với các chuỗi đã thử. Người dùng đã xác nhận: hiệu ứng nghe được qua jack tai nghe và preset lưu từ GUI còn sau khi tắt/bật nguồn. Chưa xác minh: nút nhấn, số đo chất lượng âm thanh, các chức năng GUI khác trên board (sơ bộ), cửa sổ Scope (chưa được người xem trực tiếp). Roadmap: [docs/plan-roadmap.vi.md](docs/plan-roadmap.vi.md).

## Cấu trúc
```
firmware/            firmware STM32 (dự án Makefile, arm-none-eabi-gcc)
  Core/Inc, Core/Src   engine, đầu ra audio, lệnh USB, preset, main
  Core/Src/algos/      các thuật toán hiệu ứng (delay, dyn, looper, mod, pitch, reverb)
  USB_DEVICE/          thiết bị USB CDC
  Drivers, Middlewares ST HAL, BSP (CS43L22), thư viện USB device
  test/host/           unit test trên PC cho thuật toán và chuỗi hiệu ứng
tools/               Python: GUI, kết nối serial + simulator, tạo mẫu, test
docs/                tài liệu (mục lục bên dưới), measurements/ dữ liệu thô
```

## Bắt đầu nhanh
Nạp firmware (Git Bash; đường dẫn toolchain trong [docs/development.vi.md](docs/development.vi.md)):
```bash
cd firmware
make -j8
make flash
```
GUI (Python + pyserial; cắm CN5 micro-USB, hoặc dùng simulator), từ thư mục gốc repo:
```bash
pip install -r tools/requirements.txt
python tools/gfx_gui.py --sim            # simulator
python tools/gfx_gui.py --port COM13     # board (dùng cổng COM của bạn)
```

## Tài liệu
| Tài liệu | Nội dung |
|---|---|
| [docs/plan-roadmap.vi.md](docs/plan-roadmap.vi.md) | Các mốc M1-M3, checklist, rủi ro |
| [docs/architecture.vi.md](docs/architecture.vi.md) | Sơ đồ khối, module, bản đồ bộ nhớ, đồng thời, quyết định D1-D11 |
| [docs/hardware.vi.md](docs/hardware.vi.md) | Board, đầu nối, sơ đồ chân, clock, bản đồ flash, xác minh |
| [docs/development.vi.md](docs/development.vi.md) | Toolchain, build, nạp, import CubeIDE, thêm thuật toán |
| [docs/testing.vi.md](docs/testing.vi.md) | Host test, test Python, quy trình test phần cứng, đo CPU, checklist |
| [docs/protocol.vi.md](docs/protocol.vi.md) | Giao thức điều khiển USB ASCII v2 |
| [CHANGELOG.vi.md](CHANGELOG.vi.md) | Thay đổi và vấn đề đã biết |

## Tham khảo nhanh trên board
| Điều khiển | Chức năng |
|---|---|
| B1 (PA0) nhấn ngắn | phát / dừng mẫu thử |
| B1 nhấn giữ (>= 700 ms) | looper ghi (kích hoạt khi đang giữ) |
| LED xanh lá (PD12) | sáng = đang phát; nháy nhanh = looper đang ghi |
| LED cam (PD13) | sáng = bypass |
| LED đỏ (PD14) | đầu ra chạm full scale (clip) |
| LED xanh dương (PD15) | sáng liên tục = USB đã configured; nháy = chưa configured |
| Phím `S` trên GUI / nút Scope | mở cửa sổ Scope (xem dạng sóng) |
| CN5 (micro-USB) | cổng điều khiển (CDC); CN1 (mini-USB) là ST-LINK |

Hành vi nút nhấn được đọc từ `firmware/Core/Src/main.c`; chưa xác minh trên phần cứng.
