/****************************************************
 * SMART PET FEEDER - COMPATIBLE WITH BACKEND LOGIC
 * Flow: Receive Command -> Tare -> LED ON -> Open 90° -> Monitor Weight -> Close 0° -> LED OFF -> Send ACK
 * Added: Hardware Watchdog Timer (WDT) & HX711 Data Ready Interrupt
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
#include <esp_task_wdt.h> // [NEW] Thư viện Watchdog Timer

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

// [NEW] Cờ báo hiệu có dữ liệu từ HX711 (dùng volatile vì thay đổi trong ngắt)
volatile bool hx711DataReady = false; 

/************** SERVO CONFIG **************/
const int servoPin     = 13;
const int SERVO_STOP   = 0;   // Góc đóng
const int SERVO_RUN    = 90;  // Góc mở (90 độ)
Servo foodGate;

/************** BUTTON & LED **************/
const int buttonPin = 27; 
const int ledPin    = 26;

/************** WATCHDOG CONFIG **************/
#define WDT_TIMEOUT 10 // [NEW] Thời gian Watchdog timeout (10 giây)

/************** FEEDING CONFIG **************/
const float DEFAULT_FEED_AMOUNT = 10.0; // Backend mặc định cũng là 10g
const unsigned long MAX_FEED_TIME = 30000; // 30 giây timeout
const float WEIGHT_TOLERANCE = 0.5; // Dung sai ±0.5g

bool isFeeding = false;
float targetWeight = 0;
unsigned long feedStartTime = 0;
String currentMode = "idle";    // "idle", "manual", "scheduled", "voice"
String currentUserId = "";
String currentIssuedAt = ""; 

/************** TIME **************/
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;
struct tm timeinfo;
unsigned long lastTelemetry = 0;

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
void IRAM_ATTR hx711_isr(); // [NEW] Hàm ngắt cho HX711

/******************** SETUP ********************/
void setup() {
  Serial.begin(115200);
  delay(100);

  // [NEW] 0. Setup Watchdog Timer (WDT)
  esp_task_wdt_init(WDT_TIMEOUT, true); // Khởi tạo WDT, true = kích hoạt Hard Reset nếu tràn
  esp_task_wdt_add(NULL);               // Thêm task hiện tại (loop) vào WDT
  Serial.println("WDT Initialized (10s)");

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

    // [NEW] Đăng ký Ngắt sườn xuống (FALLING) cho chân DOUT của HX711
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
  if (!mqtt.connected()) reconnectMQTT();
  mqtt.loop();

  readButton();      
  updateWeight();    
  handleLogic();     

  sendTelemetry();
  delay(50);

  // [NEW] Đá chó (Reset WDT) ở cuối vòng lặp để tránh chip tự reset
  esp_task_wdt_reset(); 
}

/******************** INTERRUPT SERVICE ROUTINES (ISR) ********************/

// [NEW] Hàm ISR kích hoạt khi chân DOUT rớt xuống LOW (Data Ready)
void IRAM_ATTR hx711_isr() {
  hx711DataReady = true; // Bật cờ báo hiệu có dữ liệu
}

/******************** CORE LOGIC ********************/

// 1. BẮT ĐẦU CHO ĂN
void startFeeding(const String& mode, float amount, const String& userId, const String& issuedAt) {
  if (isFeeding) {
    Serial.println("Already feeding, ignoring command");
    return;
  }
  
  currentMode = mode;
  targetWeight = amount;
  currentUserId = userId;
  currentIssuedAt = issuedAt;
  
  if (scale.is_ready()) {
    // Tạm tắt ngắt lúc tare để tránh xung đột
    detachInterrupt(digitalPinToInterrupt(LOADCELL_DOUT_PIN));
    scale.tare();
    attachInterrupt(digitalPinToInterrupt(LOADCELL_DOUT_PIN), hx711_isr, FALLING);
    delay(100);
  }
  
  digitalWrite(ledPin, HIGH);
  
  if (!foodGate.attached()) foodGate.attach(servoPin, 500, 2400);
  foodGate.write(SERVO_RUN); 
  delay(300);

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

  foodGate.write(SERVO_STOP);
  delay(1000);
  foodGate.detach(); 

  digitalWrite(ledPin, LOW);

  float actualAmount = weightCurrentVal;
  
  String status = "success";
  if (actualAmount < (targetWeight - WEIGHT_TOLERANCE)) {
    status = "failed"; 
    Serial.printf("FAILED: Only %.1fg/%.1fg dispensed\n", actualAmount, targetWeight);
  } else {
    Serial.printf("SUCCESS: %.1fg dispensed\n", actualAmount);
  }
  
  sendFeedingAck(currentMode, actualAmount, status);
  
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
  
  isFeeding = false;
  currentMode = "idle";
  currentUserId = "";
  currentIssuedAt = "";
  
  delay(2000); 
}

// 3. LOGIC KIỂM TRA CÂN (Chạy liên tục trong loop)
void handleLogic() {
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
       lcd.print("Ready            ");
    }
    return;
  }
  
  float currentAmount = weightCurrentVal;
  
  lcd.setCursor(0, 1);
  lcd.print((int)currentAmount); 
  lcd.print("/"); 
  lcd.print((int)targetWeight); 
  lcd.print("g     ");

  if (currentAmount >= (targetWeight - WEIGHT_TOLERANCE)) {
    Serial.printf("Target reached! %.1fg >= %.1fg\n", currentAmount, targetWeight);
    stopFeeding();
    return;
  }
  
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
  static unsigned long lastPress = 0;
  if (digitalRead(buttonPin) == LOW && !isFeeding && currentMode == "idle") {
    if (millis() - lastPress > 1000) {
      lastPress = millis();
      Serial.println("Physical button pressed -> Manual feed 10g");
      startFeeding("manual", DEFAULT_FEED_AMOUNT, "local-user", String(millis()));
    }
  }
}

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

  String mode = doc["mode"] | "";
  float amount = doc["amount"] | DEFAULT_FEED_AMOUNT;
  String userId = doc["userId"] | "unknown";
  String issuedAt = doc["issuedAt"].as<String>();
  
  if (mode != "manual" && mode != "scheduled" && mode != "voice") {
    Serial.println("Invalid mode, ignoring command");
    return;
  }

  if (amount <= 0 || amount > 200) {
    Serial.println("Invalid amount, using default 10g");
    amount = DEFAULT_FEED_AMOUNT;
  }

  if (!isFeeding && currentMode == "idle") {
    startFeeding(mode, amount, userId, issuedAt);
  } else {
    Serial.println("Busy feeding, command ignored");
  }
}

/******************** UTILS ********************/

void updateWeight() {
  // [NEW] Thay vì polling scale.is_ready(), chúng ta kiểm tra cờ từ ngắt phần cứng
  if (hx711DataReady) {
    hx711DataReady = false; // Xóa cờ báo hiệu ngay khi bắt đầu xử lý
    
    // float raw = scale.get_units(3); // Vẫn lấy 3 sample để lọc nhiễu
    float raw = scale.get_units(1);
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
  
  bool published = mqtt.publish(topic_ack.c_str(), out.c_str(), true);
  Serial.println("ACK sent: " + String(published ? "OK" : "FAILED"));
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
    
    // [NEW] Trong vòng lặp chờ dài (như chờ WiFi), cần "đá chó" để tránh bị reset ngoài ý muốn
    esp_task_wdt_reset(); 
  }
  Serial.println("\nWiFi Connected!");
  return true;
}

void reconnectMQTT() {
  static unsigned long lastTry = 0;
  if (millis() - lastTry < 5000) return; 
  lastTry = millis();
  
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    return;
  }
  
  if (mqtt.connect(device_id, mqtt_user, mqtt_pass)) {
    mqtt.subscribe(topic_command);
    sendTelemetry(true);
    lcd.clear();
    lcd.print("MQTT Connected");
    delay(1000);
  }
}