# Máy điểm danh học sinh (ESP32 + RC522)

Máy quét thẻ NFC/RFID đặt tại lớp học. Học sinh quẹt thẻ, thiết bị gửi UID của thẻ lên API điểm danh, rồi hiển thị tên học sinh, lớp và trạng thái (đúng giờ / đi muộn / lỗi) trên màn hình, kèm âm báo bằng buzzer.

Thiết bị tự cấu hình qua WiFi: không cần hardcode WiFi hay mã máy đọc, một firmware dùng chung cho mọi lớp.

## Tính năng

- Đọc UID thẻ 13.56MHz (Mifare Classic, NTAG, Ultralight) bằng RC522
- Gửi điểm danh lên API qua HTTPS
- Màn hình ST7789 1.9" nằm ngang, hiển thị tiếng Việt có dấu
- Âm báo khác nhau cho từng trạng thái
- Chế độ cài đặt qua WiFi (captive portal): nhập WiFi, mã máy đọc, API URL
- Lưu cấu hình vào flash (NVS), giữ nguyên sau khi mất điện
- Trang quản trị trong mạng LAN qua IP của thiết bị
- Tự kết nối lại WiFi; tự bật lại chế độ cài đặt nếu không vào được WiFi
- Watchdog 30 giây, tự khởi tạo lại RC522 khi bị treo
- Giữ nút BOOT 5 giây để xoá cấu hình

## Phần cứng

| Linh kiện | Ghi chú |
|---|---|
| ESP32 DevKit (WROOM-32, bản DOIT DevKit V1) | Flash 4MB |
| Module RC522 | Chỉ cấp **3.3V** |
| Màn hình IPS 1.9" 170x320, IC ST7789 | Chân: GND VCC SCL SDA RES DC CS BLK |
| Buzzer passive 3.3V | Nên có điện trở 100Ω nối tiếp hoặc transistor S8050 |

### Sơ đồ đấu nối

RC522 và màn hình dùng chung bus SPI (VSPI), tách bằng chân CS riêng.

| Tín hiệu | GPIO | RC522 | ST7789 |
|---|---|---|---|
| SCK | 18 | SCK | SCL |
| MOSI | 23 | MOSI | SDA |
| MISO | 19 | MISO | — |
| CS | 21 / 5 | SDA (SS) | CS |
| RST | 22 / 17 | RST | RES |
| DC | 16 | — | DC |
| Đèn nền | 4 | — | BLK |
| IRQ | — | bỏ trống | — |
| Nguồn | 3.3V | 3.3V | VCC |
| GND | GND | GND | GND |

Buzzer: chân (+) nối GPIO25, chân (−) nối GND.

> Chân `SCL`/`SDA` trên màn hình là SPI (SCK/MOSI), không phải I2C.
> Có thể dùng thanh GND chung trên breadboard cho tất cả module.

## Cài đặt môi trường

Dự án dùng [PlatformIO](https://platformio.org/) trên VS Code.

`platformio.ini`:

```ini
[env:esp32doit-devkit-v1]
platform = espressif32
board = esp32doit-devkit-v1
framework = arduino
monitor_speed = 115200
board_build.partitions = min_spiffs.csv
lib_deps =
    miguelbalboa/MFRC522@^1.4.11
    bblanchon/ArduinoJson@^7
    lovyan03/LovyanGFX@^1
    olikraus/U8g2@^2
```

- `min_spiffs.csv` cho app khoảng 1.9MB, giữ 2 slot OTA. Bảng phân vùng mặc định (1.25MB) không đủ chỗ.
- U8g2 chỉ dùng để lấy font `unifont_t_vietnamese2` cho LovyanGFX.
- Code tương thích cả ESP32 Arduino core 2.x và 3.x.

Build và nạp:

```bash
pio run -t upload
pio device monitor
```

Nếu vừa đổi bảng phân vùng, chạy `pio run -t clean` trước khi nạp.

## Cấu trúc dự án

```
.
├── include/
│   └── lgfx_config.hpp   # cấu hình màn ST7789 cho LovyanGFX
├── src/
│   └── main.cpp          # toàn bộ logic: NFC, WiFi, API, màn hình, buzzer, web
├── platformio.ini
└── README.md
```

## Cài đặt thiết bị lần đầu

1. Cấp nguồn. Khi chưa có cấu hình, thiết bị phát WiFi `DiemDanh-XXXX` (không mật khẩu). Tên WiFi và IP `192.168.4.1` hiện trên màn hình.
2. Dùng điện thoại kết nối vào WiFi đó. Trang cài đặt tự bật lên; nếu không, mở trình duyệt và vào `http://192.168.4.1`. Nếu điện thoại báo "không có Internet", chọn vẫn kết nối.
3. Nhập:
   - **Tên WiFi và mật khẩu.** Chỉ hỗ trợ **WiFi 2.4GHz**; danh sách gợi ý chỉ hiện mạng 2.4GHz mà thiết bị quét được.
   - **Mã máy đọc (key):** lấy ở trang Lớp học (tài khoản admin) trên hệ thống điểm danh.
   - **API URL:** để mặc định `https://theantam.aiturbox.com/api/reader/scan` nếu không có yêu cầu thay đổi.
4. Bấm **Lưu và kết nối**. Thiết bị thử kết nối WiFi:
   - Thành công: lưu cấu hình, hiện IP trong mạng LAN. WiFi cài đặt tự tắt sau 1 phút.
   - Thất bại: báo lỗi trên màn hình và trang web, giữ chế độ cài đặt để nhập lại.

### Quản trị khi đã hoạt động

IP của thiết bị luôn hiện ở thanh trên màn hình. Mở `http://<IP>` từ máy cùng mạng WiFi để vào trang quản trị:

- Chỉ đổi mã máy đọc hoặc API URL: áp dụng ngay.
- Đổi WiFi: thiết bị lưu và khởi động lại. Nếu sai mật khẩu, thiết bị tự bật lại WiFi cài đặt.
- **Xoá cấu hình:** bấm nút trên trang quản trị, hoặc giữ nút **BOOT** trên board 5 giây.

Trong router, thiết bị xuất hiện với hostname `DiemDanh-XXXX`.

## Màn hình

Màn hình nằm ngang 320x170, chia 3 vùng:

- **Thanh trên:** tên WiFi và IP của thiết bị (ở chế độ cài đặt: tên WiFi cài đặt và `192.168.4.1`).
- **Giữa:** trạng thái chính (khởi động, đang kết nối, mời quẹt thẻ, đang xử lý, kết quả).
- **Thanh dưới:** UID vừa quét hoặc thông báo lỗi.

Kết quả quét hiện trong 3 giây rồi quay về màn chờ:

| `kind` | Hiển thị | Âm báo |
|---|---|---|
| `ok` | Tên + lớp + message màu xanh | 1 bíp |
| `late` | Tên + lớp + message màu vàng | 2 bíp |
| `bad` | Message màu đỏ | 1 bíp dài, trầm |
| Lỗi mạng | "Lỗi kết nối" màu đỏ | 3 bíp nhanh |

Khi vừa đọc được thẻ, thiết bị kêu một tiếng tít ngắn và hiện "Đang xử lý..." trong lúc chờ API.

## API

```bash
curl -X POST https://theantam.aiturbox.com/api/reader/scan \
  -H "Content-Type: application/json" \
  -d '{"key":"KHOA_MAY_DOC_CUA_LOP","uid":"04:A3:5B:1A:2C:6F:80"}'
```

- `key`: khoá máy đọc của lớp.
- `uid`: UID của thẻ, hex viết hoa, ngăn cách bằng `:` (4 hoặc 7 byte). Thẻ phải được gán UID trước ở trang Thẻ học sinh.
- Thiếu `Content-Type: application/json` trả về 415; sai `key` trả về 403.

Ví dụ phản hồi:

```json
{"ok":true,"kind":"ok","name":"Nguyễn Minh Anh","message":"Có mặt – 06:45"}
{"ok":true,"kind":"late","name":"Nguyễn Minh Anh","message":"Đi muộn – 07:12"}
{"ok":false,"kind":"bad","name":"","message":"Không tìm thấy thẻ này."}
```

Tên lớp được đọc từ phản hồi qua hàm `pickClass()` trong `main.cpp`. Nếu API đổi tên trường, sửa mảng `keys` trong hàm này.

## Cơ chế tự phục hồi

| Tình huống | Xử lý |
|---|---|
| Mất WiFi | Thử kết nối lại mỗi 30 giây |
| Mất WiFi quá 5 phút | Khởi động lại |
| Khởi động không vào được WiFi | Bật WiFi cài đặt, đồng thời vẫn thử WiFi cũ ở chế độ nền |
| RC522 không phản hồi | Kiểm tra mỗi 10 giây, tự khởi tạo lại |
| Chương trình bị treo | Watchdog reset sau 30 giây |

## Xử lý sự cố

**Build báo vượt dung lượng flash**
Kiểm tra dòng `board_build.partitions = min_spiffs.csv` trong `platformio.ini`, sau đó clean và build lại.

**RC522 không đọc được thẻ**
- Xem Serial: dòng `RC522: LOI` nghĩa là ESP32 không giao tiếp được với module, cần kiểm tra lại dây.
- Hàn chắc chân header của module.
- Thẻ phải là loại 13.56MHz; RC522 không đọc được thẻ 125kHz.

**Màn hình hiển thị sai** (chỉnh trong `include/lgfx_config.hpp`)
- Màu bị đảo: đổi `cfg.invert`.
- Hình lệch hoặc có sọc ở mép: chỉnh `offset_x` / `offset_y` (mặc định `offset_x = 35`).
- Hình bị ngược: đổi `tft.setRotation(1)` thành `3` trong `main.cpp`.

**Buzzer chỉ kêu "tạch tạch"**
Buzzer là loại passive: đặt `BUZZER_PASSIVE true`. Nếu là loại active: đặt `false`.

**ESP32 tự reset hoặc màn hình chớp khi buzzer kêu**
Buzzer kéo quá dòng từ GPIO. Thêm điện trở 100Ω nối tiếp, hoặc dùng transistor S8050 (GPIO25 → 1kΩ → chân B).

## Lưu ý bảo mật

- WiFi cài đặt không có mật khẩu, và trang quản trị trong LAN chưa yêu cầu đăng nhập. Hướng cải thiện: thêm mã PIN quản trị hiện trên màn hình.
- Kết nối HTTPS đang dùng `setInsecure()` (bỏ qua kiểm tra chứng chỉ). Khi triển khai chính thức nên gắn root CA của domain API.
- Mã máy đọc và mật khẩu WiFi chỉ lưu trong flash của thiết bị, không nằm trong mã nguồn.

## Hướng phát triển

- Cập nhật firmware qua mạng (OTA)
- Mã PIN cho trang quản trị
- Smooth font (`.vlw`) trong LittleFS để chữ đẹp hơn
- Thiết kế hộp vỏ để lắp tại lớp