/****************************************************
 * SMART PET FEEDER - COMPATIBLE WITH BACKEND LOGIC
 * Backend sends: { "mode": "manual|scheduled|voice", "amount": <grams>, "userId": "...", "issuedAt": <timestamp> }
 * Flow: Receive Command -> Tare -> LED ON -> Open 90° -> Monitor Weight -> Close 0° -> LED OFF -> Send ACK
 *
 * SERVO TIMER LOGIC:
 *  - Khi đang cho ăn, theo dõi tốc độ rơi (g/s) từ cân.
 *  - Tính thời gian ước tính còn lại để đạt target.
 *  - Chỉ khi không thấy trọng lượng tăng (stall) trong STALL_WINDOW_MS,
 *    kích hoạt servo đóng (3s delay) để đảm bảo đồ ăn đã rơi hết xuống.
 ****************************************************/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HX711.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

/************** WIFI CONFIG **************/
const char* ssid     = "Nyanko Sensei";
const char* password = "123444556";

/************** MQTT CONFIG **************/
const char* mqtt_server = "e4b01f831a674150bbae2854b6f1735c.s1.eu.hivemq.cloud";
const int   mqtt_port   = 8883;
const char* mqtt_user   = "quandotrung";
const char* mqtt_pass   = "Pass1235";
const char* device_id   = "petfeeder-feed-node-01";

// MQTT Topics - Backend gửi lệnh qua topic này
const char* topic_command = "petfeeder/feed";
String topic_telemetry    = String("feeder/") + device_id + "/telemetry";
String topic_ack          = String("feeder/") + device_id + "/ack";

WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

/************** LCD I2C **************/
LiquidCrystal_I2C lcd(0x27, 16, 2);

/************** HX711 & INTERRUPT **************/
const int LOADCELL_DOUT_PIN = 16;
const int LOADCELL_SCK_PIN  = 4;
HX711 scale;
const float CALIBRATING = 413.96;
float weightCurrentVal = 0.0;
float weightFoodSpout  = 6.0;

// Cờ báo hiệu có dữ liệu từ HX711 (dùng volatile vì thay đổi trong ngắt)
volatile bool hx711DataReady = false;

/************** IR SENSOR CONFIG **************/
const int irSensorPin = 27;
volatile bool isHopperEmpty = false;
volatile bool hopperStateChanged = false;

/************** SERVO CONFIG **************/
const int servoPin     = 13;
const int SERVO_STOP   = 0;   // Góc đóng
const int SERVO_RUN    = 90;  // Góc mở (90 độ)
Servo foodGate;

/************** FEEDING CONFIG **************/
const float DEFAULT_FEED_AMOUNT = 10.0;
const unsigned long MAX_FEED_TIME = 30000; // 30 giây timeout an toàn
const float WEIGHT_TOLERANCE = 0.5;        // Dung sai ±0.5g

bool isFeeding = false;
float targetWeight = 0;
unsigned long feedStartTime = 0;
String currentMode = "idle";
String currentUserId = "";
String currentIssuedAt = "";

/************** SERVO TIMER / FLOW PREDICTION **************/
// Cửa sổ thời gian để phát hiện cân không tăng (stall) - backup path
const unsigned long STALL_WINDOW_MS  = 800;   // ms không thấy tăng -> stall
// Sau khi predictive timer hoặc stall kích hoạt, đợi 3s cho đồ ăn đang rơi hạ xuống
const unsigned long CLOSE_DELAY_MS   = 3000;  // 3 giây đợi đồ rơi hết
// Số mẫu tối thiểu để flowRate đủ ổn định trước khi tính predictive timer
const int           FLOW_STABLE_SAMPLES = 5;
// Ngưỡng flowRate tối thiểu để coi là đang chảy (g/s)
const float         FLOW_MIN_RATE     = 0.3f;

// Trạng thái theo dõi tốc độ rơi
float    flowLastWeight     = 0.0;   // Trọng lượng tại lần kiểm tra trước
unsigned long flowLastTime  = 0;     // Thời điểm lần kiểm tra trước
float    flowRate           = 0.0;   // g/s tính được (EMA)
int      flowSampleCount    = 0;     // Số mẫu đã tích lũy

// ── Predictive Close Timer (Primary path)
// Được tính lại mỗi chu kỳ: predictedCloseAt = now + (remaining / flowRate) * 1000
// Khi millis() >= predictedCloseAt -> bắt đầu giai đoạn CLOSE_DELAY
bool          timerArmed         = false;  // Timer đã được kích hoạt chưa
unsigned long predictedCloseAt  = 0;     // Timestamp (ms) dự đoán đóng servo

// ── Stall Detection (Backup path)
bool     stallDetected      = false;
unsigned long stallStartMs  = 0;

// ── Close-Delay State (dùng chung cho cả 2 path)
bool     servoClosing       = false; // Đang trong giai đoạn đợi 3s
unsigned long servoCloseStartMs = 0;
float    weightAtCloseStart = 0.0;  // Trọng lượng tại thời điểm bắt đầu close-delay

// Mốc trọng lượng cuối dùng để phát hiện stall
float    weightAtStallCheck = 0.0;
unsigned long stallCheckTime = 0;

/************** TIME **************/
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;
struct tm timeinfo;
unsigned long lastTelemetry = 0;

/************** LOOP DELAY **************/
static const unsigned long ACTIVE_LOOP_DELAY_MS = 50;
static const unsigned long IDLE_LOOP_DELAY_MS   = 150;
uint64_t lastHandledIssuedAt = 0;

/************** FUNCTION PROTOTYPES **************/
void connectWiFi();
void reconnectMQTT();
void mqttCallback(char* t, byte* p, unsigned int l);
void updateWeight();
void startFeeding(const String& mode, float amount, const String& userId, const String& issuedAt);
void stopFeeding();
void handleLogic();
void sendTelemetry(bool immediate = false);
void sendFeedingAck(const String& mode, float actualAmount, const String& status);
bool waitForWiFi(unsigned long timeoutMs);
bool parseIssuedAtMs(const String& issuedAtStr, uint64_t& outMs);
void IRAM_ATTR irSensor_ISR();
void sendHopperAlert();
void IRAM_ATTR hx711_isr();
void resetFlowTracker();
void updateFlowRate();
bool checkStall();
void armPredictiveTimer();
void enterCloseDelay(const char* reason);


/******************** SETUP ********************/
void setup() {
  Serial.begin(115200);
  delay(100);

  // 1. Setup I2C & LCD
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("Booting...");

  // 2. Setup IR Sensor
  pinMode(irSensorPin, INPUT_PULLUP);
  isHopperEmpty = digitalRead(irSensorPin);
  attachInterrupt(digitalPinToInterrupt(irSensorPin), irSensor_ISR, CHANGE);

  // 3. Setup Servo (Đóng và Ngắt điện ngay để bảo vệ nguồn)
  foodGate.attach(servoPin, 500, 2400);
  foodGate.write(SERVO_STOP);
  delay(500);
  foodGate.detach();
  Serial.println("Servo Init: Closed & Detached");

  // 4. Setup HX711 Loadcell
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  unsigned long timeout = millis();
  while (!scale.is_ready() && millis() - timeout < 2000) { delay(10); }

  if (scale.is_ready()) {
    scale.set_scale(CALIBRATING);
    scale.tare();
    Serial.println("HX711 Ready");

    attachInterrupt(digitalPinToInterrupt(LOADCELL_DOUT_PIN), hx711_isr, FALLING);
    Serial.println("HX711 Interrupt Attached");
  } else {
    Serial.println("HX711 Error");
    lcd.setCursor(0,1); lcd.print("Scale Error!");
  }

  // 5. Setup WiFi & MQTT
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);

  lcd.clear(); lcd.print("WiFi connecting");
  connectWiFi();

  espClient.setInsecure();
  configTime(gmtOffset_sec, 0, ntpServer);

  lcd.clear(); lcd.print("System Ready");
  delay(1000);
}

/******************** LOOP ********************/
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (!mqtt.connected()) reconnectMQTT();
  mqtt.loop();
  updateWeight();
  handleLogic();

  sendTelemetry();
  delay(isFeeding ? ACTIVE_LOOP_DELAY_MS : IDLE_LOOP_DELAY_MS);
}

/******************** INTERRUPT SERVICE ROUTINES (ISR) ********************/
void IRAM_ATTR hx711_isr() {
  hx711DataReady = true;
}

/******************** SERVO TIMER - FLOW PREDICTION ********************/

// Reset toàn bộ trạng thái flow tracker khi bắt đầu feeding mới
void resetFlowTracker() {
  flowLastWeight      = 0.0;
  flowLastTime        = millis();
  flowRate            = 0.0;
  flowSampleCount     = 0;
  timerArmed          = false;
  predictedCloseAt    = 0;
  stallDetected       = false;
  stallStartMs        = 0;
  servoClosing        = false;
  servoCloseStartMs   = 0;
  weightAtCloseStart  = 0.0;
  weightAtStallCheck  = 0.0;
  stallCheckTime      = millis();
}

// Cập nhật tốc độ rơi (g/s) - EMA alpha=0.35 để phản ứng nhanh nhưng vẫn mượt
void updateFlowRate() {
  unsigned long now = millis();
  float elapsed = (now - flowLastTime) / 1000.0f;
  if (elapsed < 0.05f) return;

  float delta = weightCurrentVal - flowLastWeight;
  if (delta > 0.0f) {
    float instantRate = delta / elapsed;
    if (flowSampleCount == 0) {
      flowRate = instantRate; // Mẫu đầu tiên: gán thẳng
    } else {
      flowRate = 0.35f * instantRate + 0.65f * flowRate; // EMA
    }
    flowSampleCount++;
  }
  flowLastWeight = weightCurrentVal;
  flowLastTime   = now;
}

// Tính (hoặc cập nhật) predictedCloseAt dựa trên flowRate hiện tại
// Được gọi mỗi vòng loop khi chưa vào close-delay, để ETA luôn chính xác
void armPredictiveTimer() {
  if (servoClosing) return;               // Đã vào close-delay, không cập nhật nữa
  if (flowRate < FLOW_MIN_RATE) return;   // Chưa đủ flow để tính
  if (flowSampleCount < FLOW_STABLE_SAMPLES) return; // Chờ đủ mẫu ổn định

  float remaining = targetWeight - weightCurrentVal;
  if (remaining <= 0) return;

  // Thời gian ước tính (ms) để phần còn lại rơi xuống
  unsigned long etaMs = (unsigned long)((remaining / flowRate) * 1000.0f);

  predictedCloseAt = millis() + etaMs;
  timerArmed = true;
}

// Kích hoạt giai đoạn close-delay (dùng chung cho predictive timer & stall)
void enterCloseDelay(const char* reason) {
  if (servoClosing) return; // Đã vào rồi, không kích hoạt lại
  servoClosing        = true;
  servoCloseStartMs   = millis();
  weightAtCloseStart  = weightCurrentVal;

  float pct    = (targetWeight > 0) ? (weightCurrentVal / targetWeight * 100.0f) : 0.0f;
  Serial.printf("[%s] %.1fg/%.1fg (%.0f%%) | flowRate=%.2fg/s -> Wait %.0fs for in-flight food\n",
                reason, weightCurrentVal, targetWeight, pct, flowRate,
                CLOSE_DELAY_MS / 1000.0f);

  lcd.setCursor(0, 1);
  lcd.print("Wait 3s...      ");
}

// Kiểm tra xem cân có đang "stall" (không tăng) trong STALL_WINDOW_MS không
bool checkStall() {
  unsigned long now = millis();
  if (now - stallCheckTime >= STALL_WINDOW_MS) {
    float gained = weightCurrentVal - weightAtStallCheck;
    weightAtStallCheck = weightCurrentVal;
    stallCheckTime = now;
    if (gained < 0.3f) return true;
  }
  return false;
}

/******************** CORE LOGIC ********************/

// 1. BẮT ĐẦU CHO ĂN
void startFeeding(const String& mode, float amount, const String& userId, const String& issuedAt) {
  if (isFeeding) {
    Serial.println("Already feeding, ignoring command");
    return;
  }

  currentMode     = mode;
  targetWeight    = amount;
  currentUserId   = userId;
  currentIssuedAt = issuedAt;

  // Reset cân về 0 một cách an toàn
  if (scale.is_ready()) {
    detachInterrupt(digitalPinToInterrupt(LOADCELL_DOUT_PIN));
    scale.tare();
    delay(10);
    hx711DataReady = false;
    attachInterrupt(digitalPinToInterrupt(LOADCELL_DOUT_PIN), hx711_isr, FALLING);
  }

  weightCurrentVal = 0.0;

  // Mở Servo 90 độ
  if (!foodGate.attached()) foodGate.attach(servoPin, 500, 2400);
  foodGate.write(SERVO_RUN);
  delay(300); // Đợi servo ổn định

  isFeeding     = true;
  feedStartTime = millis();

  // Khởi tạo flow tracker
  resetFlowTracker();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(mode); lcd.print(": ");
  lcd.print((int)amount); lcd.print("g");

  Serial.printf("START FEEDING: Mode=%s, Target=%.1fg, User=%s\n",
                mode.c_str(), targetWeight, userId.c_str());
}

// 2. DỪNG CHO ĂN
void stopFeeding() {
  if (!isFeeding) return;

  // A. Đóng Servo về 0 độ
  foodGate.write(SERVO_STOP);
  delay(1000); // Đợi servo đóng hoàn toàn
  foodGate.detach();

  // B. Lấy lượng thức ăn đã phát
  float actualAmount = weightCurrentVal;

  // C. Xác định trạng thái kết quả
  String status = "success";
  if (actualAmount < (targetWeight - WEIGHT_TOLERANCE)) {
    status = "failed";
    Serial.printf("FAILED: Only %.1fg/%.1fg dispensed\n", actualAmount, targetWeight);
  } else {
    Serial.printf("SUCCESS: %.1fg dispensed\n", actualAmount);
  }

  // D. Gửi ACK về backend
  sendFeedingAck(currentMode, actualAmount, status);

  // E. Hiển thị kết quả
  lcd.clear();
  lcd.setCursor(0, 0);
  if (status == "success") {
    lcd.print("SUCCESS: "); lcd.print((int)actualAmount); lcd.print("g");
  } else {
    lcd.print("FAILED: "); lcd.print((int)actualAmount); lcd.print("g");
  }
  lcd.setCursor(0, 1);
  lcd.print(currentMode); lcd.print(status == "success" ? " OK" : " FAIL");

  Serial.printf("STOP FEEDING: Actual=%.1fg, Status=%s\n", actualAmount, status.c_str());

  // F. Reset trạng thái
  isFeeding       = false;
  currentMode     = "idle";
  currentUserId   = "";
  currentIssuedAt = "";

  delay(2000);
}

// 3. LOGIC KIỂM TRA CÂN + SERVO TIMER (Chạy liên tục trong loop)
void handleLogic() {
  // Nếu đang không cho ăn -> Hiển thị thời gian và trạng thái sẵn sàng
  if (!isFeeding) {
    static unsigned long lastUpdate = 0;
    if (millis() - lastUpdate > 1000) {
      lastUpdate = millis();
      if (getLocalTime(&timeinfo)) {
        char buf[17];
        strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
        lcd.setCursor(0, 0);
        lcd.print(buf); lcd.print("       ");
      }
      lcd.setCursor(0, 1);
      lcd.print("Ready           ");
    }
    if (hopperStateChanged) {
      hopperStateChanged = false;
      sendHopperAlert();
    }
    return;
  }

  // --- ĐANG CHO ĂN ---
  float currentAmount = weightCurrentVal;
  unsigned long now   = millis();

  // Cập nhật tốc độ rơi
  updateFlowRate();

  // ── ĐIỀU KIỆN 1: Đạt đủ target (hoặc vượt)
  if (currentAmount >= (targetWeight - WEIGHT_TOLERANCE)) {
    Serial.printf("Target reached! %.1fg >= %.1fg\n", currentAmount, targetWeight);
    stopFeeding();
    return;
  }

  // ── ĐIỀU KIỆN 2: Timeout an toàn 30s
  if (now - feedStartTime > MAX_FEED_TIME) {
    Serial.printf("TIMEOUT! Only %.1fg/%.1fg after 30s\n", currentAmount, targetWeight);
    lcd.clear();
    lcd.print("TIMEOUT!");
    lcd.setCursor(0, 1);
    lcd.print("Not enough food");
    delay(2000);
    stopFeeding();
    return;
  }

  // ── ĐIỀU KIỆN 3: SERVO TIMER (PRIMARY) + STALL DETECTION (BACKUP)
  //
  // PRIMARY  – Predictive Close Timer:
  //   a) Liên tục đo flowRate (g/s) bằng EMA.
  //   b) Khi có đủ mẫu ổn định, tính predictedCloseAt = now + (remaining/flowRate)*1000.
  //   c) Khi millis() >= predictedCloseAt -> enterCloseDelay("TIMER") -> đợi 3s
  //      để phần đồ ăn đang lơ lửng hạ xuống cân.
  //   d) Trong 3s close-delay: nếu cân chạm target -> stopFeeding() ngay.
  //   e) Sau 3s close-delay -> stopFeeding() với lượng đã đạt.
  //
  // BACKUP   – Stall Detection:
  //   Nếu predictive timer chưa arm (flowRate chưa ổn) hoặc bị sai,
  //   stall detection sẽ kích hoạt enterCloseDelay("STALL") độc lập.

  if (!servoClosing) {
    // ── Cập nhật & arm predictive timer
    armPredictiveTimer();

    // PRIMARY: Timer đã arm và đã đến giờ đóng
    if (timerArmed && now >= predictedCloseAt) {
      enterCloseDelay("TIMER");
    }
    // BACKUP: Stall detection (chạy song song, kích hoạt khi flowRate còn thấp hoặc timer miss)
    else if (checkStall()) {
      if (!stallDetected) {
        stallDetected = true;
        stallStartMs  = now;
        enterCloseDelay("STALL");
      }
    }
  } else {
    // ── Đang trong giai đoạn close-delay (3s)
    float gainedSinceClose = weightCurrentVal - weightAtCloseStart;

    // Nếu thấy cân tăng đáng kể trong 3s -> đồ ăn vẫn đang rơi, chờ tiếp
    // Chỉ cancel nếu timer chưa hết và lượng còn xa target
    bool canCancel = !timerArmed  // Chưa có predictive timer -> stall cancel được
                     && gainedSinceClose > 0.5f
                     && currentAmount < (targetWeight - WEIGHT_TOLERANCE * 2);
    if (canCancel) {
      Serial.printf("[STALL CANCEL] +%.1fg since close start, resuming.\n", gainedSinceClose);
      stallDetected      = false;
      servoClosing       = false;
      weightAtCloseStart = weightCurrentVal;
      // Cập nhật lại mốc stall check
      weightAtStallCheck = weightCurrentVal;
      stallCheckTime     = now;
    }
    // Hết 3s -> đóng servo với lượng hiện tại
    else if (now - servoCloseStartMs >= CLOSE_DELAY_MS) {
      Serial.printf("[CLOSE] After %.0fs delay: final=%.1fg/%.1fg\n",
                    CLOSE_DELAY_MS / 1000.0f, currentAmount, targetWeight);
      stopFeeding();
      return;
    }
  }

  // ── Cập nhật LCD tiến độ + ETA mỗi 300ms
  static unsigned long lastLcdUpdate = 0;
  if (now - lastLcdUpdate > 300) {
    lastLcdUpdate = now;
    float pct = (targetWeight > 0) ? (currentAmount / targetWeight * 100.0f) : 0.0f;

    lcd.setCursor(0, 0);
    lcd.print(currentMode); lcd.print(": ");
    lcd.print((int)targetWeight); lcd.print("g   ");

    lcd.setCursor(0, 1);
    char buf[17];
    if (servoClosing) {
      // Hiển thị đếm ngược 3s
      unsigned long remaining3s = CLOSE_DELAY_MS - (now - servoCloseStartMs);
      snprintf(buf, sizeof(buf), "%.0f/%.0fg cls:%lus",
               currentAmount, targetWeight, remaining3s / 1000UL);
    } else if (timerArmed && predictedCloseAt > now) {
      // Hiển thị ETA theo predictive timer
      float etaSec = (predictedCloseAt - now) / 1000.0f;
      snprintf(buf, sizeof(buf), "%.0f/%.0fg ETA:%.0fs",
               currentAmount, targetWeight, etaSec);
    } else if (flowRate >= FLOW_MIN_RATE) {
      // flowRate có nhưng timer chưa arm đủ mẫu
      float etaSec = (targetWeight - currentAmount) / flowRate;
      snprintf(buf, sizeof(buf), "%.0f/%.0fg~%.0fs",
               currentAmount, targetWeight, etaSec);
    } else {
      // Chưa có flow rate đủ tin cậy
      snprintf(buf, sizeof(buf), "%.0f/%.0fg (%.0f%%)",
               currentAmount, targetWeight, pct);
    }
    lcd.print(buf);
  }
}

/******************** INPUT HANDLERS ********************/

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.println("MQTT Received: " + msg);

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, msg);

  if (error) {
    Serial.print("JSON parse error: ");
    Serial.println(error.c_str());
    return;
  }

  String mode     = doc["mode"] | "";
  float  amount   = doc["amount"] | DEFAULT_FEED_AMOUNT;
  String userId   = doc["userId"] | "unknown";
  String issuedAt = doc["issuedAt"].as<String>();

  Serial.println("Parsed issuedAt: " + issuedAt);

  if (mode != "manual" && mode != "scheduled" && mode != "voice") {
    Serial.println("Invalid mode, ignoring command");
    return;
  }

  if (amount <= 0 || amount > 200) {
    Serial.println("Invalid amount, using default 10g");
    amount = DEFAULT_FEED_AMOUNT;
  }

  uint64_t issuedAtMs = 0;
  if (!parseIssuedAtMs(issuedAt, issuedAtMs)) {
    Serial.println("Invalid issuedAt, ignoring command");
    return;
  }
  if (issuedAtMs <= lastHandledIssuedAt) {
    Serial.printf("Duplicate/old command ignored (issuedAt=%llu, lastHandled=%llu)\n",
                  (unsigned long long)issuedAtMs,
                  (unsigned long long)lastHandledIssuedAt);
    return;
  }

  if (!isFeeding && currentMode == "idle") {
    lastHandledIssuedAt = issuedAtMs;
    startFeeding(mode, amount, userId, issuedAt);
  } else {
    Serial.println("Busy feeding, command ignored");
  }
}

/******************** UTILS ********************/

void updateWeight() {
  if (hx711DataReady) {
    detachInterrupt(digitalPinToInterrupt(LOADCELL_DOUT_PIN));

    if (scale.is_ready()) {
      float raw = scale.get_units(1);
      weightCurrentVal = (raw < 0) ? 0.0 : raw;
    }

    hx711DataReady = false;
    attachInterrupt(digitalPinToInterrupt(LOADCELL_DOUT_PIN), hx711_isr, FALLING);
  }
}

void sendTelemetry(bool immediate) {
  if (!mqtt.connected()) return;
  if (!immediate && millis() - lastTelemetry < 5000) return;

  StaticJsonDocument<256> doc;
  doc["device_id"]        = device_id;
  doc["timestamp"]        = millis();
  doc["type"]             = "telemetry";
  doc["data"]["weight"]   = weightCurrentVal;
  doc["data"]["is_feeding"] = isFeeding;
  doc["data"]["mode"]     = currentMode;
  doc["data"]["hopper_empty"] = isHopperEmpty;

  String output;
  serializeJson(doc, output);
  mqtt.publish(topic_telemetry.c_str(), output.c_str());

  lastTelemetry = millis();
}

void sendFeedingAck(const String& mode, float actualAmount, const String& status) {
  if (!mqtt.connected()) return;

  StaticJsonDocument<384> doc;
  doc["device_id"]     = device_id;
  doc["timestamp"]     = millis();
  doc["type"]          = "feeding_complete";
  doc["mode"]          = mode;
  doc["amount"]        = actualAmount;
  doc["targetAmount"]  = targetWeight;
  doc["status"]        = status;
  doc["userId"]        = currentUserId;
  doc["issuedAt"]      = currentIssuedAt;

  String out;
  serializeJson(doc, out);

  Serial.println("ACK JSON: " + out);

  bool published = mqtt.publish(topic_ack.c_str(), out.c_str(), true);
  Serial.println("ACK sent: " + String(published ? "OK" : "FAILED"));
  Serial.println("  Mode: " + mode);
  Serial.println("  Amount: " + String(actualAmount) + "g");
  Serial.println("  Status: " + status);
  Serial.println("  issuedAt: " + String(currentIssuedAt));
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  waitForWiFi(15000);
}

bool waitForWiFi(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start >= timeoutMs) {
      Serial.println("\nWiFi Connection Failed!");
      return false;
    }
    Serial.print(".");
    lcd.print(".");
    delay(500);
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

void reconnectMQTT() {
  static unsigned long lastTry = 0;
  if (millis() - lastTry < 5000) return;
  lastTry = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, reconnecting...");
    WiFi.reconnect();
    return;
  }

  Serial.print("Connecting to MQTT...");

  if (mqtt.connect(device_id, mqtt_user, mqtt_pass)) {
    Serial.println("Connected!");

    bool subOk = mqtt.subscribe(topic_command);
    Serial.printf("Subscribed to '%s': %s\n", topic_command, subOk ? "OK" : "FAILED");

    sendTelemetry(true);

    lcd.clear();
    lcd.print("MQTT Connected");
    delay(1000);
  } else {
    Serial.print("Failed, rc=");
    Serial.println(mqtt.state());
  }
}

/******************** IR SENSOR INTERRUPT *******************/

void IRAM_ATTR irSensor_ISR() {
  bool currentState = digitalRead(irSensorPin);
  if (currentState != isHopperEmpty) {
    isHopperEmpty      = currentState;
    hopperStateChanged = true;
  }
}

void sendHopperAlert() {
  if (!mqtt.connected()) return;
  StaticJsonDocument<256> doc;
  doc["device_id"] = device_id;
  doc["timestamp"] = millis();
  doc["type"]      = "alert";
  doc["is_empty"]  = isHopperEmpty;
  doc["message"]   = isHopperEmpty ? "Hopper Empty!" : "Hopper Refilled";

  String output; serializeJson(doc, output);
  String topic_alert = String("feeder/") + device_id + "/alert";
  mqtt.publish(topic_alert.c_str(), output.c_str());
  Serial.println(isHopperEmpty ? "ALERT: Hopper Empty" : "INFO: Hopper Refilled");
}

/******************** HELPERS ********************/

bool parseIssuedAtMs(const String& issuedAtStr, uint64_t& outMs) {
  if (issuedAtStr.length() == 0) return false;
  char* endp = nullptr;
  outMs = strtoull(issuedAtStr.c_str(), &endp, 10);
  return endp != issuedAtStr.c_str();
}