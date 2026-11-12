[English](CHANGELOG.md) | **Tiếng Việt**

# Changelog

Định dạng theo [Keep a Changelog](https://keepachangelog.com/); phiên bản theo SemVer.

## [Unreleased]

v2 (12/11/2026): firmware và công cụ đã viết xong; bring-up phần cứng (M2) đang tiến hành.

### Added
- Chuỗi hiệu ứng nối tiếp 4 slot (`fx_engine`) với các global in/out level và bypass.
- 27 thuật toán thuộc 6 nhóm, định nghĩa bằng catalogue X-macro `fx_algo_table.h`.
- Looper (mu-law 8-bit, 24 kHz mono, reverse và half-speed).
- Giao thức điều khiển ASCII USB CDC v2: [docs/protocol.vi.md](docs/protocol.vi.md).
- 16 preset trong sector 11 của flash, preset hiện tại được khôi phục khi khởi động.
- GUI Tk kiểu pedal (`tools/gfx_gui.py`) và simulator tương đương firmware (`tools/gfx_sim.py`).
- Bộ host test (7) và unit test Python (24).
- Scope (xem dạng sóng): firmware `scope.c`, giao thức `SCOPE`/`SCOPEDATA`, cửa sổ GUI `tools/gfx_scope.py` (nút hoặc phím `S`), simulator hỗ trợ.
- Tài liệu song ngữ.

### Changed
- Chuỗi mặc định: Overdrive (on), Chorus (off), Digital Delay (on), Hall (on).

### Verified on hardware (12/11/2026)
- USB enumerate trên Windows 10 thành "USB Serial Device (COM13)", VID `0x0483` PID `0x5740`, driver có sẵn của Windows.
- Smoke test giao thức v2 qua COM13 pass (khám phá, chỉnh sửa, lỗi, looper, `PSAVE`/`PLIST`/`PLOAD`/`PDEL`); chi tiết trong [docs/testing.vi.md](docs/testing.vi.md).
- Người dùng xác nhận: hiệu ứng nghe được qua jack tai nghe; preset lưu từ GUI còn sau khi tắt/bật nguồn (báo cáo của người dùng, không có số đo chất lượng).
- Bắt Scope qua USB: bắt có trigger 20 ms sẵn sàng sau 0.20 s, free-run 2000 ms sẵn sàng sau 2.07 s, đầu ra khác đầu vào với chuỗi mặc định; `gfx_link.Connection` với COM13 trả 256 điểm mỗi kênh.
- Đo tải DSP 12-58 % với 7 chuỗi đã thử; các ước lượng cost là bảo thủ ([docs/testing.vi.md](docs/testing.vi.md)).

### Known issues
- Chưa xác minh trên phần cứng: nút nhấn, chất lượng âm thanh, các chức năng GUI ngoài lưu preset (sơ bộ).
- Cửa sổ Scope chưa được người xem trực tiếp (selftest GUI pass với simulator).
- RAM đầy 93.82 % (122976 B) và CCMRAM đầy 96.9 %; còn ít chỗ cho tính năng mới.
- Không có ngõ vào guitar: CS43L22 trên Discovery chỉ có đầu ra (xem M3 trong [docs/plan-roadmap.vi.md](docs/plan-roadmap.vi.md)).
