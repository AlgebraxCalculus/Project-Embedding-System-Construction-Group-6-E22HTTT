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

/************** HX711 & INTERRUPT **************/
const int LOADCELL_DOUT_PIN = 16;
const int LOADCELL_SCK_PIN  = 4;
HX711 scale;
const float CALIBRATING = 413.96;
// Số sample để average mỗi lần đọc - dùng 3 để giảm nhiễu HX711 mà không quá chậm.
const uint8_t HX711_READ_SAMPLES = 3;
float weightCurrentVal = 0.0;
float weightRawVal     = 0.0;  // Raw không clamp - dùng cho diagnostic, có thể âm
// LƯU Ý VỀ BÁT ĂN: scale.tare() được gọi đầu mỗi lần feeding (xem startFeeding()),
// nó tự lấy lượng cân hiện tại làm "0". Nếu BÁT ĂN đã được đặt trên cân TRƯỚC khi
// lệnh feed gửi xuống → bát được trừ ra tự động, không cần code thêm.
// QUAN TRỌNG: bát phải nằm yên trên cân TRƯỚC lúc bấm feed; nếu đặt bát SAU khi
// đã tare, hệ thống sẽ hiểu trọng lượng bát là đồ ăn.

// Cờ báo hiệu có dữ liệu từ HX711 (dùng volatile vì thay đổi trong ngắt)
volatile bool hx711DataReady = false;

// Chu kỳ in log cân nặng ra Serial (ms). Đặt 500ms = 2Hz, đủ để theo dõi mà
// không spam. Khi đang feeding, log sẽ kèm % và flowRate.
const unsigned long WEIGHT_LOG_INTERVAL_MS = 500;

/************** IR SENSOR CONFIG **************/
const int irSensorPin = 27;
volatile bool isHopperEmpty = false;
// Debounce + rate-limit để tránh spam alert khi cảm biến IR rung ở ngưỡng
volatile unsigned long lastIrEdgeMs = 0;
const unsigned long IR_DEBOUNCE_MS         = 800;     // Chờ tín hiệu ổn định 800ms
const unsigned long IR_ALERT_MIN_INTERVAL  = 30000;   // Tối thiểu 30s giữa 2 alert MQTT
bool stableHopperEmpty = false;       // State đã debounce
unsigned long lastHopperAlertMs = 0;

/************** SERVO CONFIG **************/
const int servoPin     = 13;
const int SERVO_STOP   = 170;   // Góc đóng
const int SERVO_RUN    = 100;  // Góc mở (90 độ)
Servo foodGate;

/************** FEEDING CONFIG **************/
const float DEFAULT_FEED_AMOUNT = 10.0;
const unsigned long MAX_FEED_TIME = 30000; // 30 giây timeout an toàn
// Strict mode: success requires actualAmount >= targetWeight.
// Float noise margin chỉ dùng để chấp nhận bằng đúng (e.g. 9.999 ~= 10.000).
const float SUCCESS_EPSILON = 0.05;        // 50 mg float noise margin

bool isFeeding = false;
float targetWeight = 0;
unsigned long feedStartTime = 0;
String currentMode = "idle";
String currentUserId = "";
String currentIssuedAt = "";

/************** SERVO TIMER / FLOW PREDICTION **************/
// Cửa sổ thời gian để phát hiện cân không tăng (stall) - backup path
const unsigned long STALL_WINDOW_MS  = 2000;  // ms không thấy tăng -> stall
// Thời gian grace period sau khi bắt đầu feeding trước khi kiểm tra stall.
// Phải đủ dài cho đồ ăn rơi từ hopper xuống cân (đo thực tế ~3s).
const unsigned long STALL_GRACE_MS   = 5000;  // 5s để servo mở và thức ăn rơi xuống cân
// Sau khi predictive timer hoặc stall kích hoạt, đóng servo rồi đợi 3s cho đồ ăn rơi hết
const unsigned long CLOSE_DELAY_MS   = 3000;  // 3 giây đợi đồ rơi hết
// Số mẫu tối thiểu để flowRate đủ ổn định trước khi tính predictive timer
const int           FLOW_STABLE_SAMPLES = 5;
// Ngưỡng flowRate tối thiểu để coi là đang chảy (g/s)
const float         FLOW_MIN_RATE     = 0.3f;
// Ngưỡng delta tối thiểu (g) để 1 sample được coi là "đồ ăn rơi thật" thay vì nhiễu HX711.
// HX711 thường nhiễu ±0.05g khi cân tĩnh; đặt 0.15g để loại nhiễu mà vẫn bắt được hạt nhỏ.
const float         FLOW_NOISE_THRESHOLD = 0.15f;
// Trọng lượng tối thiểu (g) để xác nhận "đồ ăn đã bắt đầu rơi" -> mở khoá stall detection.
// Trước khi đạt ngưỡng này, STALL không được trigger (tránh đóng servo khi đồ chưa kịp rơi).
const float         FIRST_FOOD_THRESHOLD = 0.5f;

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
// Cờ xác nhận đồ ăn đã thật sự rơi xuống cân (vượt FIRST_FOOD_THRESHOLD).
// Stall detection chỉ được phép trigger khi cờ này = true.
bool     firstFoodDetected  = false;

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
void checkHopper();
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

  // 1. Setup IR Sensor
  pinMode(irSensorPin, INPUT_PULLUP);
  isHopperEmpty = digitalRead(irSensorPin);
  stableHopperEmpty = isHopperEmpty;
  attachInterrupt(digitalPinToInterrupt(irSensorPin), irSensor_ISR, CHANGE);

  // 2. Setup Servo (Đóng và Ngắt điện ngay để bảo vệ nguồn)
  foodGate.attach(servoPin, 500, 2400);
  foodGate.write(SERVO_STOP);
  delay(500);
  foodGate.detach();
  Serial.println("Servo Init: Closed & Detached");

  // 3. Setup HX711 Loadcell
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
  }

  // 4. Setup WiFi & MQTT
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);

  connectWiFi();

  espClient.setInsecure();
  configTime(gmtOffset_sec, 0, ntpServer);

  Serial.println("System Ready");
}

/******************** LOOP ********************/
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (!mqtt.connected()) reconnectMQTT();
  mqtt.loop();
  updateWeight();
  checkHopper();        // Debounced + rate-limited hopper alert
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
  firstFoodDetected   = false;
}

// Cập nhật tốc độ rơi (g/s) - EMA alpha=0.35 để phản ứng nhanh nhưng vẫn mượt.
// Chỉ count delta khi vượt FLOW_NOISE_THRESHOLD để tránh nhiễu HX711 làm flowRate giả.
void updateFlowRate() {
  unsigned long now = millis();
  float elapsed = (now - flowLastTime) / 1000.0f;
  if (elapsed < 0.05f) return;

  float delta = weightCurrentVal - flowLastWeight;
  if (delta > FLOW_NOISE_THRESHOLD) {
    float instantRate = delta / elapsed;
    if (flowSampleCount == 0) {
      flowRate = instantRate; // Mẫu đầu tiên: gán thẳng
    } else {
      flowRate = 0.35f * instantRate + 0.65f * flowRate; // EMA
    }
    flowSampleCount++;
    flowLastWeight = weightCurrentVal;
    flowLastTime   = now;
  }
  // Nếu delta dưới ngưỡng nhiễu, KHÔNG update flowLastWeight/Time để gom delta cho lần sau.
}

// Tính (hoặc cập nhật) predictedCloseAt dựa trên flowRate hiện tại
// Được gọi mỗi vòng loop khi chưa vào close-delay, để ETA luôn chính xác
void armPredictiveTimer() {
  if (servoClosing) return;               // Đã vào close-delay, không cập nhật nữa
  // Phải có đồ ăn thật sự rơi xuống cân trước, tránh flowRate ảo từ rung servo khi khởi động
  if (!firstFoodDetected) return;
  if (flowRate < FLOW_MIN_RATE) return;   // Chưa đủ flow để tính
  if (flowSampleCount < FLOW_STABLE_SAMPLES) return; // Chờ đủ mẫu ổn định

  float remaining = targetWeight - weightCurrentVal;
  if (remaining <= 0) return;

  // Thời gian ước tính (ms) để phần còn lại rơi xuống
  unsigned long etaMs = (unsigned long)((remaining / flowRate) * 1000.0f);

  predictedCloseAt = millis() + etaMs;
  timerArmed = true;
}

// Kích hoạt giai đoạn close-delay: đóng servo ngay, rồi đợi 3s cho đồ ăn đang rơi hạ xuống
void enterCloseDelay(const char* reason) {
  if (servoClosing) return; // Đã vào rồi, không kích hoạt lại

  // Đóng servo ngay để dừng thức ăn chảy
  foodGate.write(SERVO_STOP);

  servoClosing        = true;
  servoCloseStartMs   = millis();
  weightAtCloseStart  = weightCurrentVal;

  float pct    = (targetWeight > 0) ? (weightCurrentVal / targetWeight * 100.0f) : 0.0f;
  Serial.printf("[%s] %.1fg/%.1fg (%.0f%%) | flowRate=%.2fg/s -> Close servo & wait %.0fs for in-flight food\n",
                reason, weightCurrentVal, targetWeight, pct, flowRate,
                CLOSE_DELAY_MS / 1000.0f);
}

// Kiểm tra xem cân có đang "stall" (không tăng) trong STALL_WINDOW_MS không.
// LƯU Ý: Chỉ trigger sau khi đồ ăn đã thật sự rơi xuống cân (firstFoodDetected),
// nếu không sẽ đóng servo trước cả khi đồ kịp rơi từ chute.
bool checkStall() {
  unsigned long now = millis();
  // Grace period: bỏ qua stall check trong STALL_GRACE_MS đầu để servo mở và thức ăn bắt đầu chảy
  if (now - feedStartTime < STALL_GRACE_MS) return false;
  // Chưa thấy đồ ăn rơi -> không thể coi là stall (có thể chỉ là delay rơi từ chute)
  if (!firstFoodDetected) return false;

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

  // C. Xác định trạng thái kết quả (strict: actualAmount phải >= targetWeight)
  String status = "success";
  if (actualAmount + SUCCESS_EPSILON < targetWeight) {
    status = "failed";
    Serial.printf("FAILED: Only %.2fg/%.2fg dispensed\n", actualAmount, targetWeight);
  } else {
    Serial.printf("SUCCESS: %.2fg dispensed (target %.2fg)\n", actualAmount, targetWeight);
  }

  // D. Gửi ACK về backend
  sendFeedingAck(currentMode, actualAmount, status);

  Serial.printf("STOP FEEDING: Actual=%.2fg, Status=%s\n", actualAmount, status.c_str());

  // E. Reset trạng thái
  isFeeding       = false;
  currentMode     = "idle";
  currentUserId   = "";
  currentIssuedAt = "";
}

// 3. LOGIC KIỂM TRA CÂN + SERVO TIMER (Chạy liên tục trong loop)
void handleLogic() {
  if (!isFeeding) return;

  // --- ĐANG CHO ĂN ---
  float currentAmount = weightCurrentVal;
  unsigned long now   = millis();

  // Đánh dấu thời điểm đồ ăn lần đầu rơi xuống cân -> mở khoá stall + predictive timer.
  // Reset flow tracker tại đây để xoá flowRate ảo từ rung servo / nhiễu lúc khởi động.
  if (!firstFoodDetected && currentAmount >= FIRST_FOOD_THRESHOLD) {
    firstFoodDetected = true;
    flowLastWeight    = currentAmount;
    flowLastTime      = now;
    flowRate          = 0.0f;
    flowSampleCount   = 0;
    weightAtStallCheck = currentAmount;
    stallCheckTime     = now;
    Serial.printf("[FIRST FOOD] Detected at %.2fg, flow tracker reset, timers armed\n", currentAmount);
  }

  // Chỉ tính flowRate sau khi đồ ăn đã thật sự rơi xuống cân.
  // Trước đó cân chỉ có nhiễu/rung servo, không phản ánh tốc độ rơi thật.
  if (firstFoodDetected) {
    updateFlowRate();
  }

  // ── ĐIỀU KIỆN 1: Đạt đủ target (strict: phải >= targetWeight)
  if (currentAmount + SUCCESS_EPSILON >= targetWeight) {
    Serial.printf("Target reached! %.2fg >= %.2fg\n", currentAmount, targetWeight);
    stopFeeding();
    return;
  }

  // ── ĐIỀU KIỆN 2: Timeout an toàn 30s
  if (now - feedStartTime > MAX_FEED_TIME) {
    Serial.printf("TIMEOUT! Only %.2fg/%.2fg after 30s\n", currentAmount, targetWeight);
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
    // Chỉ cancel nếu timer chưa hết và lượng còn xa target (strict: dùng 1g margin để tránh resume khi gần đạt)
    bool canCancel = !timerArmed  // Chưa có predictive timer -> stall cancel được
                     && gainedSinceClose > 0.5f
                     && currentAmount < (targetWeight - 1.0f);
    if (canCancel) {
      Serial.printf("[STALL CANCEL] +%.1fg since close start, resuming.\n", gainedSinceClose);
      // Mở lại servo vì thức ăn đang tiếp tục chảy
      if (!foodGate.attached()) foodGate.attach(servoPin, 500, 2400);
      foodGate.write(SERVO_RUN);

      stallDetected      = false;
      servoClosing       = false;
      weightAtCloseStart = weightCurrentVal;
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
      float raw = scale.get_units(HX711_READ_SAMPLES);
      weightRawVal     = raw;                       // giữ giá trị thật (có thể âm) cho debug
      weightCurrentVal = (raw < 0) ? 0.0 : raw;     // clamp âm về 0 cho feeding logic
    }

    hx711DataReady = false;
    attachInterrupt(digitalPinToInterrupt(LOADCELL_DOUT_PIN), hx711_isr, FALLING);

    // ── Log cân nặng ra Serial theo chu kỳ (không spam mỗi sample HX711)
    static unsigned long lastWeightLog = 0;
    unsigned long now = millis();
    if (now - lastWeightLog >= WEIGHT_LOG_INTERVAL_MS) {
      lastWeightLog = now;

      // Cảnh báo wiring sai: raw âm đáng kể khi đặt food lên cân -> chắc chắn có vấn đề.
      // Nguyên nhân thường gặp: load cell nối ngược cực (E+/E- hoặc A+/A- đảo)
      // hoặc CALIBRATING sai dấu (thử -413.96 thay vì 413.96).
      if (weightRawVal < -0.5f) {
        Serial.printf("[LOADCELL WARN] raw=%.2fg ÂM -> KIỂM TRA WIRING load cell hoặc đổi dấu CALIBRATING\n",
                      weightRawVal);
      }

      if (isFeeding) {
        float pct = (targetWeight > 0) ? (weightCurrentVal / targetWeight * 100.0f) : 0.0f;
        Serial.printf("[LOADCELL] %.2f g (raw %.2f, target %.1f g, %.0f%%, flow %.2f g/s)\n",
                      weightCurrentVal, weightRawVal, targetWeight, pct, flowRate);
      } else {
        Serial.printf("[LOADCELL] %.2f g (raw %.2f, idle)\n", weightCurrentVal, weightRawVal);
      }
    }
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

  bool published = mqtt.publish(topic_ack.c_str(), out.c_str(), false); // không dùng retained
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
  } else {
    Serial.print("Failed, rc=");
    Serial.println(mqtt.state());
  }
}

/******************** IR SENSOR INTERRUPT *******************/

// ISR chỉ ghi nhận edge time. Việc đọc + debounce làm trong main loop để
// tránh bouncing tạo ra hàng loạt alert.
void IRAM_ATTR irSensor_ISR() {
  lastIrEdgeMs = millis();
}

// Gọi mỗi loop. Sau khi tín hiệu IR ổn định IR_DEBOUNCE_MS,
// đọc lại trạng thái và chỉ gửi alert nếu state thật sự đổi
// và đã qua IR_ALERT_MIN_INTERVAL kể từ alert trước.
void checkHopper() {
  unsigned long edgeMs = lastIrEdgeMs;
  if (edgeMs == 0) return;

  unsigned long now = millis();
  if (now - edgeMs < IR_DEBOUNCE_MS) return; // Chờ ổn định

  // Sample state ổn định
  bool currentState = digitalRead(irSensorPin);

  // Chỉ reset edge time nếu state đã đứng im suốt window debounce
  // (lastIrEdgeMs có thể đã được ISR cập nhật trong lúc chờ)
  if (lastIrEdgeMs != edgeMs) return; // Vẫn còn bouncing -> đợi tiếp

  noInterrupts();
  lastIrEdgeMs = 0;
  interrupts();

  isHopperEmpty = currentState;

  if (currentState == stableHopperEmpty) return; // Không thay đổi thực
  stableHopperEmpty = currentState;

  // Rate-limit MQTT alert
  if (now - lastHopperAlertMs < IR_ALERT_MIN_INTERVAL) {
    Serial.printf("[Hopper] State changed but rate-limited (last %lus ago)\n",
                  (now - lastHopperAlertMs) / 1000UL);
    return;
  }

  lastHopperAlertMs = now;
  sendHopperAlert();
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