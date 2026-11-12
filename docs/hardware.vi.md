[English](hardware.md) | **Tiếng Việt**

# Phần cứng

## Board
STM32F4DISCOVERY (MB997): STM32F407VGT6 (Cortex-M4F, flash 1 MB, RAM 192 KB = 128 KB + 64 KB CCM), ST-LINK tích hợp, codec/DAC âm thanh CS43L22 (chỉ có đầu ra), nút nhấn người dùng và 4 LED. Không cần linh kiện nào khác.

### Đầu nối
| Đầu nối | Công dụng |
|---|---|
| CN1 (mini-USB) | ST-LINK: cấp nguồn, nạp/debug SWD |
| CN5 (micro-USB) | USB OTG FS = cổng điều khiển (COM ảo CDC) |
| CN4 (jack 3.5 mm) | đầu ra âm thanh (tai nghe/line) từ CS43L22 |

## Sơ đồ chân
Firmware (`audio_out.c`, `usbd_conf.c`, `main.c`, BSP) là nguồn chuẩn; bảng này phải khớp với nó.

| Chức năng | Chân | Ghi chú |
|---|---|---|
| I2S3 MCK | PC7 | AF6 |
| I2S3 SCK | PC10 | AF6 |
| I2S3 SD | PC12 | AF6 |
| I2S3 WS | PA4 | AF6 |
| I2C1 SCL / SDA | PB6 / PB9 | điều khiển codec |
| Reset codec | PD4 | |
| USB OTG FS DM / DP | PA11 / PA12 | tắt VBUS sensing |
| Nút người dùng B1 | PA0 | ngắn = play/stop, giữ (>= 700 ms) = looper ghi |
| LED xanh lá (LED4) | PD12 | sáng = đang phát; nháy nhanh (nửa chu kỳ 100 ms) = looper đang ghi |
| LED cam (LED3) | PD13 | sáng = bypass |
| LED đỏ (LED5) | PD14 | sáng = đầu ra chạm full scale (clip); nháy mãi trong `Error_Handler` |
| LED xanh dương (LED6) | PD15 | sáng liên tục = USB đã configured; nháy (nửa chu kỳ 500 ms) = chưa configured |

## Clock
| Clock | Thiết lập |
|---|---|
| HSE | thạch anh 8 MHz |
| SYSCLK | 168 MHz (PLL từ HSE) |
| USB | 48 MHz |
| Audio | PLLI2S N=258, R=3 -> tần số lấy mẫu 48 kHz (I2S3, 16-bit stereo) |

Một frame = 168 MHz / 48 kHz = 3500 cycle; một block = 256 frame.

## Bản đồ flash
| Vùng | Địa chỉ | Công dụng |
|---|---|---|
| Sector 0..10 | `0x08000000` ... | mã + mẫu thử (~576 KB); `ROM` trong linker giới hạn 896 KB |
| Sector 11 | `0x080E0000` (128 KB) | preset (xem [architecture](architecture.vi.md#d10--log-marker-chỉ-số-preset-hiện-tại)) |

Linker script `firmware/STM32F407VGTX_FLASH.ld` là nguồn chuẩn. Mức dùng khi build 12/11/2026: ROM 683004 B (74.4 %).

## Trạng thái xác minh
| Mục | Trạng thái | Ngày / bằng chứng |
|---|---|---|
| Nạp qua ST-LINK | đã xác nhận | 12/11/2026 "Download verified successfully" |
| Firmware chạy | đã xác nhận | 12/11/2026 SWD: không vào error handler, LED xanh lá sáng |
| Phát luồng DMA TX I2S3 | đã xác nhận (thanh ghi) | 12/11/2026 SWD: DMA bật circular, NDTR thay đổi |
| USB enumeration | đã xác nhận | 12/11/2026 Windows 10 "USB Serial Device (COM13)", VID `0x0483` PID `0x5740`, driver có sẵn |
| LED xanh dương nháy khi chưa configured | đã xác nhận (trước khi cắm CN5) | 12/11/2026 SWD/quan sát |
| Giao thức v2 qua USB | đã xác nhận | 12/11/2026 smoke test pyserial theo script ([testing](testing.vi.md#smoke-test-giao-thức-12112026)) |
| Ghi flash (`PSAVE`) | đã xác nhận | 12/11/2026 1.02 s gồm cả phản hồi |
| Tải DSP | đã đo | 12/11/2026, 12-58 % ([testing](testing.vi.md#đo-cpu)) |
| Âm thanh nghe được (jack tai nghe) | đã xác nhận (báo cáo người dùng, không có số đo chất lượng) | 12/11/2026 |
| Lưu preset từ GUI + còn sau khi tắt/bật nguồn | đã xác nhận (báo cáo người dùng) | 12/11/2026 |
| Các chức năng GUI khác trên board thật | sơ bộ (suy ra từ việc lưu preset) | 12/11/2026 |
| Bắt Scope qua USB | đã xác nhận (script; cửa sổ chưa được người xem trực tiếp) | 12/11/2026 ([testing](testing.vi.md#scope)) |
| Nút B1 | chưa xác minh | |
| Âm thanh looper | chưa xác minh | mới xác minh trạng thái giao thức |
| Ngõ vào line guitar | không có | CS43L22 chỉ có đầu ra; M3 |
