# Smart Pet Feeder — IoT System

**BTL môn Lập trình Xây dựng hệ thống nhúng — Nhóm 6 (E22HTTT)**
Đề tài: *Hệ thống cho thú cưng ăn tự động*.

Hệ thống IoT hoàn chỉnh gồm 5 thành phần phối hợp với nhau: firmware ESP32, backend Node.js, web frontend, ứng dụng mobile và một module Speech-to-Text chạy offline.

---

## 1. Kiến trúc tổng quan

```
 Frontend (Web)  ─┐
                  ├─ REST ──► Backend (Express) ──► MongoDB Atlas
 Mobile (Expo)  ──┤                │
                  │                ├─ MQTT (HiveMQ Cloud, SSL/TLS)
                  │                │       │
                  │                │       ▼
                  │                │   ESP32 Firmware
                  │                │  (Servo + HX711 + LCD)
                  │                │
 Microphone ──► Speech Module (Whisper offline) ──► text/command
                                   │
                                   └──► Backend `/api/feed` hoặc schedule
```

- **Backend** là trung tâm: xác thực JWT, lưu MongoDB, chạy `node-cron` cho lịch tự động, cầu nối REST ↔ MQTT.
- **MQTT**: backend publish lệnh cho ăn, ESP32 thực thi và publish telemetry trở lại. Frontend/Mobile subscribe trực tiếp telemetry để cập nhật realtime. ACK đồng bộ qua `issuedAt`, timeout 35 s.
- **Lịch trình**: `schedulerService` chạy mỗi phút, kiểm tra `Schedule` đến hạn và kích hoạt pipeline cho ăn.
- **Speech**: audio → `POST /api/speech-command` (module speech) → Whisper (`@xenova/transformers`) → command object → backend `/api/feed`.
- **Auth**: JWT 7 ngày, validate ở `middleware/auth.js`.

---

## 2. Thành phần & chức năng

| # | Thành phần | Đường dẫn | Stack | Chức năng chính |
|---|---|---|---|---|
| 1 | **Backend** | [SourceCode/backend/](SourceCode/backend/) | Node.js, Express, Mongoose, MQTT, node-cron | REST API, JWT auth, CRUD lịch cho ăn, thống kê 7 ngày, bridge REST↔MQTT, cron scheduler |
| 2 | **Speech Module** | [SourceCode/speech-module/](SourceCode/speech-module/) | Node.js, Express, `@xenova/transformers`, ffmpeg-static | Speech-to-Text offline (Whisper), parse câu lệnh tiếng Việt thành command object |
| 3 | **ESP32 Firmware** | [SourceCode/esp32/smart_pet_feeder/](SourceCode/esp32/smart_pet_feeder/) | Arduino, PubSubClient, ESP32Servo, HX711, LiquidCrystal_I2C | Nhận lệnh MQTT → mở servo, cân HX711 đo lượng thức ăn, dự đoán đóng cổng tránh tràn, phát hiện kẹt, hiển thị LCD |
| 4 | **Frontend (Web)** | [SourceCode/frontend/](SourceCode/frontend/) | React + Vite, MQTT.js | Đăng ký/đăng nhập, dashboard thống kê, manual feed, voice feed (Web Speech API), CRUD lịch, telemetry realtime |
| 5 | **Mobile App** | [SourceCode/mobile/](SourceCode/mobile/) | React Native + Expo, paho-mqtt, axios | Trải nghiệm tương đương web trên Android/iOS: feed thủ công, lịch, biểu đồ, telemetry MQTT |

### Data models (MongoDB)
- **User** — username, password (bcrypt), JWT.
- **FeedLog** — timestamp, lượng thức ăn (g), nguồn kích hoạt (`manual` / `schedule` / `voice`).
- **Schedule** — giờ (HH:MM), ngày trong tuần, lượng (g), `enabled`.

---

## 3. Hướng dẫn chạy theo thứ tự

> ⚠️ **Yêu cầu chung**: Node.js ≥ 18, npm, Arduino IDE (cho ESP32), tài khoản MongoDB Atlas + HiveMQ Cloud (đã có sẵn trong file `.env` mẫu), microphone (cho voice feed).

### Bước 1 — Backend (bắt buộc trước tiên)

Backend phải chạy đầu tiên vì cả frontend, mobile và scheduler đều phụ thuộc vào nó.

```bash
cd SourceCode/backend
npm install
```

Tạo file `.env` trong [SourceCode/backend/](SourceCode/backend/):

```env
PORT=5000
MONGODB_URI=mongodb+srv://root:12345@cluster-1.k28cwf8.mongodb.net/IOT_PetFeederDB?retryWrites=true&w=majority&appName=Cluster-1
JWT_SECRET=e4a0f5857b91a2c990bbcf7af2e16c0b4a830bc3b2eaa6cc8bbc885de9133f45
MQTT_BROKER_URL=mqtts://e4b01f831a674150bbae2854b6f1735c.s1.eu.hivemq.cloud:8883
MQTT_FEED_TOPIC=petfeeder/feed
MQTT_USERNAME=quandotrung
MQTT_PASSWORD=Pass1235
```

Chạy server:

```bash
npm run dev      # development (nodemon)
# hoặc
npm start        # production
```

Backend chạy tại `http://localhost:5000`. Kiểm tra log phải có:
- `MongoDB connected successfully`
- `MQTT connected to broker`

Health check: `curl http://localhost:5000/health`.

### Bước 2 — Speech Module (chạy song song với Backend)

```bash
cd SourceCode/speech-module/backend
npm install
npm start
```

- Lần chạy đầu sẽ tự tải model Whisper (`Xenova/whisper-small`, ~240 MB) — cần Internet một lần, sau đó chạy offline.
- Server chạy tại `http://localhost:3001`. Mở trình duyệt vào URL này để test giao diện ghi âm.
- Cấu hình tuỳ chọn qua `.env` trong cùng thư mục (`WHISPER_MODEL`, `WHISPER_QUANTIZED`, `PORT`).

### Bước 3 — Nạp Firmware ESP32

1. Mở [SourceCode/esp32/smart_pet_feeder/smart_pet_feeder.ino](SourceCode/esp32/smart_pet_feeder/smart_pet_feeder.ino) bằng Arduino IDE.
2. Cài thư viện qua Library Manager: `Wire`, `LiquidCrystal_I2C`, `HX711`, `ESP32Servo`, `WiFi`, `PubSubClient`, `ArduinoJson`.
3. Sửa SSID/password Wi-Fi và thông tin MQTT (`MQTT_BROKER_URL`, `MQTT_USERNAME`, `MQTT_PASSWORD`) trực tiếp trong file `.ino` cho khớp với backend.
4. Chọn board ESP32, cổng COM tương ứng, sau đó Upload.
5. Khởi động: HX711 sẽ tare tự động lúc boot, LCD hiển thị trạng thái kết nối Wi-Fi/MQTT.

> ESP32 không bắt buộc phải có trong giai đoạn dev — backend vẫn chạy được, chỉ là không có thiết bị thật phản hồi telemetry.

### Bước 4 — Frontend (Web)

```bash
cd SourceCode/frontend
npm install
npm run dev
```

Frontend chạy tại `http://localhost:5173`. Tạo file `.env` (tuỳ chọn — có giá trị mặc định) trong [SourceCode/frontend/](SourceCode/frontend/) nếu cần override:

```env
VITE_API_BASE_URL=http://localhost:5000
VITE_MQTT_URL=wss://e4b01f831a674150bbae2854b6f1735c.s1.eu.hivemq.cloud:8884/mqtt
VITE_MQTT_USERNAME=quandotrung
VITE_MQTT_PASSWORD=Pass1235
VITE_DEVICE_ID=petfeeder-feed-node-01
```

Build production:

```bash
npm run build
npm run preview
```

### Bước 5 — Mobile App (tuỳ chọn)

```bash
cd SourceCode/mobile
npm install
npm start                # mở Expo Dev Tools
# Chạy nhanh trên thiết bị/giả lập:
npm run android
npm run ios
npm run web
```

Quét QR bằng Expo Go trên điện thoại (cùng mạng LAN với máy chạy backend) hoặc bấm `a`/`i` để mở emulator. API base URL được khai báo trong [SourceCode/mobile/src/services/api.js](SourceCode/mobile/src/services/api.js) — chỉnh thành IP LAN của máy backend nếu test trên thiết bị thật (ví dụ `http://192.168.1.10:5000`).

---

## 4. Test luồng end-to-end

1. **Đăng ký** tại `http://localhost:5173/register` (username ≥ 3 ký tự, password ≥ 6 ký tự).
2. **Đăng nhập** → vào Dashboard.
3. **Manual Feed**: bấm *Feed Now* → backend publish MQTT → ESP32 mở servo (nếu có) → log lưu vào MongoDB.
4. **Voice Feed**: bấm *Voice Feed*, nói “cho ăn 200 gram”. Web Speech API (Chrome/Edge) chuyển sang text → gửi lên backend.
5. **Schedule**: tạo lịch HH:MM + chọn ngày + lượng gram → mỗi phút `schedulerService` quét và kích hoạt khi đến hạn.
6. **Stats**: Dashboard hiển thị biểu đồ 7 ngày từ `/api/feed/stats/weekly`.

Test API nhanh bằng curl:

```bash
# Đăng ký
curl -X POST http://localhost:5000/api/auth/register \
  -H "Content-Type: application/json" \
  -d '{"username":"testuser","password":"123456"}'

# Đăng nhập (lưu token)
curl -X POST http://localhost:5000/api/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username":"testuser","password":"123456"}'

# Cho ăn thủ công
curl -X POST http://localhost:5000/api/feed/manual \
  -H "Authorization: Bearer <TOKEN>"

# Thống kê 7 ngày
curl -X GET http://localhost:5000/api/feed/stats/weekly \
  -H "Authorization: Bearer <TOKEN>"
```

---

## 5. Cấu trúc thư mục

```
Project-Embedding-System-Construction-Group-6-E22HTTT/
├── README.md
├── LICENSE
├── CLAUDE.md
├── Documents/                            # Báo cáo, tài liệu
└── SourceCode/
    ├── QUICK_START.md
    ├── backend/                          # Express + MongoDB + MQTT + cron
    │   ├── server.js
    │   ├── config/
    │   ├── controllers/
    │   ├── middleware/                   # auth.js (JWT)
    │   ├── models/                       # User, FeedLog, Schedule
    │   ├── routes/
    │   └── services/                     # mqttService, schedulerService, feedService
    ├── frontend/                         # React + Vite
    │   ├── index.html
    │   └── src/{pages,components,services,hooks}
    ├── mobile/                           # React Native + Expo
    │   ├── App.js
    │   └── src/
    ├── speech-module/                    # Whisper STT offline
    │   ├── backend/{server.js, speechService.js}
    │   └── frontend/index.html
    └── esp32/
        └── smart_pet_feeder/smart_pet_feeder.ino
```

### File quan trọng

| Vấn đề | Đường dẫn |
|---|---|
| MQTT (publish/subscribe) | [SourceCode/backend/services/mqttService.js](SourceCode/backend/services/mqttService.js) |
| Cron auto-feed | [SourceCode/backend/services/schedulerService.js](SourceCode/backend/services/schedulerService.js) |
| Logic cho ăn | [SourceCode/backend/services/feedService.js](SourceCode/backend/services/feedService.js) |
| Firmware ESP32 | [SourceCode/esp32/smart_pet_feeder/smart_pet_feeder.ino](SourceCode/esp32/smart_pet_feeder/smart_pet_feeder.ino) |
| MQTT client (web) | [SourceCode/frontend/src/services/mqtt.js](SourceCode/frontend/src/services/mqtt.js) |
| API client (mobile) | [SourceCode/mobile/src/services/api.js](SourceCode/mobile/src/services/api.js) |
| Whisper service | [SourceCode/speech-module/backend/speechService.js](SourceCode/speech-module/backend/speechService.js) |

---

## 6. Troubleshooting

| Triệu chứng | Hướng xử lý |
|---|---|
| Backend không kết nối MongoDB | Kiểm tra `MONGODB_URI`, IP whitelist trên Atlas, mạng. |
| Backend không kết nối MQTT | Xác nhận credentials HiveMQ, firewall cho phép port 8883. |
| Frontend không gọi được API | Kiểm tra backend đã chạy, `VITE_API_BASE_URL`, CORS. |
| Voice Feed không hoạt động | Dùng Chrome/Edge, cấp quyền microphone, nói rõ “cho ăn [N] gram”. |
| Mobile không kết nối backend | Đổi `localhost` → IP LAN trong `mobile/src/services/api.js`. |
| Whisper tải model thất bại | Lần đầu phải có Internet; chuyển model nhỏ hơn (`Xenova/whisper-tiny`). |
| ESP32 không nhận lệnh | Kiểm tra Wi-Fi, MQTT credentials, topic `petfeeder/feed`, serial monitor. |

---

## 7. Lưu ý vận hành

- JWT có hiệu lực 7 ngày.
- Lệnh feed gửi qua **REST**; MQTT chỉ dùng cho telemetry và lệnh xuống ESP32 từ backend.
- Cron quét lịch **mỗi phút** — chấp nhận sai số đến 1 phút.
- Whisper xử lý audio cục bộ; dữ liệu không gửi ra ngoài server.
- Khi deploy production: bật HTTPS, rate limiting, không hard-code credential trong firmware/source.

---

## License

Xem [LICENSE](LICENSE).
