/****************************************************
 * SMART PET FEEDER - COMPATIBLE WITH BACKEND LOGIC
 * Backend sends: { "mode": "manual|scheduled|voice", "amount": <grams>, "userId": "...", "issuedAt": <timestamp> }
 * Flow: Receive Command -> Tare -> LED ON -> Open 90° -> Monitor Weight -> Close 0° -> LED OFF -> Send ACK
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
#include <esp_wifi.h>

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

/************** HX711 **************/
const int LOADCELL_DOUT_PIN = 16;
const int LOADCELL_SCK_PIN  = 4;
HX711 scale;
const float CALIBRATING = 413.96; 
float weightCurrentVal = 0.0;
float weightFoodSpout  = 6.0;

/************** SERVO CONFIG **************/
const int servoPin     = 13;
const int SERVO_STOP   = 0;   // Góc đóng
const int SERVO_RUN    = 90;  // Góc mở (90 độ)
Servo foodGate;

/************** BUTTON & LED **************/
const int buttonPin = 27; 
const int ledPin    = 26;

/************** FEEDING CONFIG **************/
const float DEFAULT_FEED_AMOUNT = 10.0; // Backend mặc định cũng là 10g
const unsigned long MAX_FEED_TIME = 30000; // 30 giây timeout (chỉ để safety, không dùng để kiểm tra lượng)
const float WEIGHT_TOLERANCE = 0.5; // Dung sai ±0.5g để tránh dao động cân

bool isFeeding = false;
float targetWeight = 0;
unsigned long feedStartTime = 0;
String currentMode = "idle";    // "idle", "manual", "scheduled", "voice"
String currentUserId = "";
String currentIssuedAt = ""; // Lưu dạng String để tránh integer overflow với timestamp JavaScript

/************** TIME **************/
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;
struct tm timeinfo;
unsigned long lastTelemetry = 0;

/************** POWER SAVE (ALWAYS CONNECTED) **************/
// Keep Wi-Fi + MQTT connected so feed commands arrive immediately.
// Save power by enabling Wi-Fi modem power save and slowing the idle loop a bit.
static const unsigned long ACTIVE_LOOP_DELAY_MS = 50;
static const unsigned long IDLE_LOOP_DELAY_MS   = 150;
uint64_t lastHandledIssuedAt = 0;

/************** FUNCTION PROTOTYPES **************/
void connectWiFi();
void reconnectMQTT();
void mqttCallback(char* t, byte* p, unsigned int l);
void readButton();
void updateWeight();
void startFeeding(const String& mode, float amount, const String& userId, const String& issuedAt);
void stopFeeding();
void handleLogic();
void sendTelemetry(bool immediate = false);
void sendFeedingAck(const String& mode, float actualAmount, const String& status);
bool waitForWiFi(unsigned long timeoutMs);
bool parseIssuedAtMs(const String& issuedAtStr, uint64_t& outMs);
void configurePowerSave();

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

  // 2. Setup Button & LED
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(ledPin, OUTPUT);
  // Test LED nháy 2 lần
  digitalWrite(ledPin, HIGH); delay(200); digitalWrite(ledPin, LOW); delay(200);
  digitalWrite(ledPin, HIGH); delay(200); digitalWrite(ledPin, LOW);

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
    scale.tare(); // Reset về 0
    Serial.println("HX711 Ready");
  } else {
    Serial.println("HX711 Error");
    lcd.setCursor(0,1); lcd.print("Scale Error!");
  }

  // 5. Setup WiFi & MQTT
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512); // Tăng buffer size cho payload lớn

  lcd.clear(); lcd.print("WiFi connecting");
  connectWiFi();

  espClient.setInsecure();
  configTime(gmtOffset_sec, 0, ntpServer);

  lcd.clear(); lcd.print("System Ready");
  delay(1000);

  configurePowerSave();
}

/******************** LOOP ********************/
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (!mqtt.connected()) reconnectMQTT();
  mqtt.loop();

  readButton();      
  updateWeight();    
  handleLogic();     

  sendTelemetry();
  delay(isFeeding ? ACTIVE_LOOP_DELAY_MS : IDLE_LOOP_DELAY_MS);
}

/******************** CORE LOGIC ********************/

// 1. BẮT ĐẦU CHO ĂN
void startFeeding(const String& mode, float amount, const String& userId, const String& issuedAt) {
  if (isFeeding) {
    Serial.println("Already feeding, ignoring command");
    return;
  }
  
  // Lưu thông tin lệnh
  currentMode = mode;
  targetWeight = amount;
  currentUserId = userId;
  currentIssuedAt = issuedAt;
  
  // A. Reset cân về 0 để đếm lượng thức ăn mới
  if (scale.is_ready()) {
    scale.tare();
    delay(100);
  }
  
  // B. Bật đèn LED
  digitalWrite(ledPin, HIGH);
  
  // C. Mở Servo 90 độ
  if (!foodGate.attached()) foodGate.attach(servoPin, 500, 2400);
  foodGate.write(SERVO_RUN); 
  delay(300); // Đợi servo ổn định

  isFeeding = true;
  feedStartTime = millis();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(mode); lcd.print(": "); lcd.print((int)amount); lcd.print("g");
  
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

  // B. Tắt đèn
  digitalWrite(ledPin, LOW);

  // C. Lấy lượng thức ăn đã phát (đã tare từ trước)
  float actualAmount = weightCurrentVal;
  
  // D. Xác định trạng thái kết quả
  String status = "success";
  if (actualAmount < (targetWeight - WEIGHT_TOLERANCE)) {
    status = "failed"; // Không đủ lượng yêu cầu
    Serial.printf("FAILED: Only %.1fg/%.1fg dispensed\n", actualAmount, targetWeight);
  } else {
    Serial.printf("SUCCESS: %.1fg dispensed\n", actualAmount);
  }
  
  // E. Gửi ACK về backend
  sendFeedingAck(currentMode, actualAmount, status);
  
  // F. Hiển thị kết quả
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
  isFeeding = false;
  currentMode = "idle";
  currentUserId = "";
  currentIssuedAt = "";
  
  delay(2000); // Hiển thị kết quả 2 giây
}

// 3. LOGIC KIỂM TRA CÂN (Chạy liên tục trong loop)
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
    return;
  }

  // --- ĐANG CHO ĂN ---
  
  // Cập nhật màn hình tiến độ
  float currentAmount = weightCurrentVal;
  
  lcd.setCursor(0, 1);
  lcd.print((int)currentAmount); 
  lcd.print("/"); 
  lcd.print((int)targetWeight); 
  lcd.print("g     ");

  // ĐIỀU KIỆN DỪNG CHÍNH: Đạt đủ lượng thức ăn >= target 
  if (currentAmount >= (targetWeight - WEIGHT_TOLERANCE)) {
    Serial.printf("Target reached! %.1fg >= %.1fg\n", currentAmount, targetWeight);
    stopFeeding();
    return;
  }
  
  // ĐIỀU KIỆN DỪNG DỰ PHÒNG: Timeout 30s (trong trường hợp hết thức ăn)
  if (millis() - feedStartTime > MAX_FEED_TIME) {
    Serial.printf("TIMEOUT! Only %.1fg/%.1fg after 30s\n", currentAmount, targetWeight);
    lcd.clear();
    lcd.print("TIMEOUT!");
    lcd.setCursor(0, 1);
    lcd.print("Not enough food");
    delay(2000);
    stopFeeding();
    return;
  }
}

/******************** INPUT HANDLERS ********************/

void readButton() {
  // Nút bấm vật lý -> Tạo lệnh Manual Feed với 10g
  static unsigned long lastPress = 0;
  
  if (digitalRead(buttonPin) == LOW && !isFeeding && currentMode == "idle") {
    if (millis() - lastPress > 1000) { // Debounce 1 giây
      lastPress = millis();
      Serial.println("Physical button pressed -> Manual feed 10g");
      startFeeding("manual", DEFAULT_FEED_AMOUNT, "local-user", String(millis()));
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Parse JSON từ backend
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

  // Backend format: { "mode": "manual|scheduled|voice", "amount": 50, "userId": "...", "issuedAt": 123456 }
  String mode = doc["mode"] | "";
  float amount = doc["amount"] | DEFAULT_FEED_AMOUNT;
  String userId = doc["userId"] | "unknown";
  String issuedAt = doc["issuedAt"].as<String>(); // Lưu dạng String để tránh overflow
  
  Serial.println("Parsed issuedAt: " + issuedAt); // DEBUG

  // Validate mode
  if (mode != "manual" && mode != "scheduled" && mode != "voice") {
    Serial.println("Invalid mode, ignoring command");
    return;
  }

  // Validate amount
  if (amount <= 0 || amount > 200) {
    Serial.println("Invalid amount, using default 10g");
    amount = DEFAULT_FEED_AMOUNT;
  }

  // De-duplicate by issuedAt to avoid re-feeding on reconnect / retained message
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

  // Nếu đang không feed thì bắt đầu
  if (!isFeeding && currentMode == "idle") {
    lastHandledIssuedAt = issuedAtMs;
    startFeeding(mode, amount, userId, issuedAt);
  } else {
    Serial.println("Busy feeding, command ignored");
  }
}

/******************** UTILS ********************/

void updateWeight() {
  if (scale.is_ready()) {
    float raw = scale.get_units(3); // Đọc 3 lần để lọc nhiễu
    // Lọc giá trị âm
    float net = raw - weightFoodSpout;  
    weightCurrentVal = (net < 0) ? 0.0 : net;
  }
}

void sendTelemetry(bool immediate) {
  if (!mqtt.connected()) return;
  if (!immediate && millis() - lastTelemetry < 5000) return;
  
  StaticJsonDocument<256> doc;
  doc["device_id"] = device_id; 
  doc["timestamp"] = millis(); 
  doc["type"] = "telemetry";
  doc["data"]["weight"] = weightCurrentVal;
  doc["data"]["is_feeding"] = isFeeding;
  doc["data"]["mode"] = currentMode;
  doc["data"]["led"] = digitalRead(ledPin);
  
  String output; 
  serializeJson(doc, output);
  mqtt.publish(topic_telemetry.c_str(), output.c_str());
  
  lastTelemetry = millis();
}

void sendFeedingAck(const String& mode, float actualAmount, const String& status) {
  if (!mqtt.connected()) return;
  
  StaticJsonDocument<384> doc;
  doc["device_id"] = device_id;
  doc["timestamp"] = millis();
  doc["type"] = "feeding_complete";
  doc["mode"] = mode;
  doc["amount"] = actualAmount;
  doc["targetAmount"] = targetWeight;
  doc["status"] = status;
  doc["userId"] = currentUserId;
  doc["issuedAt"] = currentIssuedAt;
  
  String out;
  serializeJson(doc, out);
  
  Serial.println("ACK JSON: " + out); // DEBUG - Hiển thị toàn bộ JSON
  
  bool published = mqtt.publish(topic_ack.c_str(), out.c_str(), true); // retained = true
  
  Serial.println("ACK sent: " + String(published ? "OK" : "FAILED"));
  Serial.println("  Mode: " + mode);
  Serial.println("  Amount: " + String(actualAmount) + "g");
  Serial.println("  Status: " + status);
  Serial.println("  issuedAt: " + String(currentIssuedAt)); // DEBUG
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  waitForWiFi(15000);
  if (WiFi.status() == WL_CONNECTED) configurePowerSave();
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
  if (millis() - lastTry < 5000) return; // Thử lại mỗi 5 giây
  lastTry = millis();
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, reconnecting...");
    WiFi.reconnect();
    return;
  }
  
  Serial.print("Connecting to MQTT...");
  
  if (mqtt.connect(device_id, mqtt_user, mqtt_pass)) {
    Serial.println("Connected!");
    
    // Subscribe topic từ backend
    bool subOk = mqtt.subscribe(topic_command);
    Serial.printf("Subscribed to '%s': %s\n", topic_command, subOk ? "OK" : "FAILED");
    
    // Gửi telemetry ngay để báo online
    sendTelemetry(true);
    
    lcd.clear();
    lcd.print("MQTT Connected");
    delay(1000);
  } else {
    Serial.print("Failed, rc=");
    Serial.println(mqtt.state());
  }
}

/******************** POWER SAVE HELPERS ********************/

bool parseIssuedAtMs(const String& issuedAtStr, uint64_t& outMs) {
  if (issuedAtStr.length() == 0) return false;
  // accept numeric strings (from backend Date.now())
  char* endp = nullptr;
  outMs = strtoull(issuedAtStr.c_str(), &endp, 10);
  return endp != issuedAtStr.c_str();
}

void configurePowerSave() {
  WiFi.setSleep(true);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  Serial.println("WiFi power save enabled (MIN_MODEM)");
}