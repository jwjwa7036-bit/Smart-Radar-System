// =====================================================================
// نظام رادار استشعار ذكي - نسخة مُصلّحة (بدون أي delay() في التنفيذ)
// =====================================================================

#include <Servo.h>
#include <SoftwareSerial.h>
#include <LiquidCrystal_I2C.h>

// ----- تعريف المنافذ -----
#define TRIG_PIN     13
#define ECHO_PIN     11
#define SERVO_PIN    6
#define BUZZER_PIN   5
#define BLUE_LED_PIN 9
#define RED_LED_PIN  10
#define SIM_RX_PIN   2
#define SIM_TX_PIN   3
#define ESP_CAPTURE_PIN 8
// ملاحظة: ESP32-CAM أصبح متصلاً بـ D0(RX)/D1(TX) — وهما منفذا Serial الأساسي
// نفسه (نفس خط USB/COM3)، وليسا منفذين منفصلين، لذا لا حاجة لتعريفهما هنا
// ولا لكائن SoftwareSerial خاص بهما. راجع التحذير في الشرح المرفق حول
// ضرورة استخدام خافض جهد (Level Shifter) بين TX (5V) وRX ESP32-CAM (3.3V).

// ----- كائنات النظام -----
Servo radarServo;
SoftwareSerial sim800l(SIM_RX_PIN, SIM_TX_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ----- ثوابت المسافات -----
const int SAFE_MIN = 101;
const int INITIAL_ALERT_MIN = 71;
const int WARNING_MIN = 51;
const int CRITICAL_MAX = 50;

// ----- ثوابت التوقيت -----
const unsigned long SERVO_INTERVAL = 20;
const unsigned long MEASURE_INTERVAL = 100;
const unsigned long LCD_INTERVAL = 300;

// ----- متغيرات -----
int currentAngle = 0;
bool forward = true;
unsigned long lastServoTime = 0;
unsigned long lastMeasureTime = 0;
unsigned long lastLcdTime = 0;

long lastDistance = -1;
String lastStatus = "WAITING";

// منع التكرار
bool dangerEventSent = false;

// رقم الهاتف
const char PHONE_NUMBER[] = "+967712090747";
const char SMS_TEXT[] = "Danger! Intruder Detected!";

// =====================================================================
// آلة حالة إرسال الـ SMS (غير-blocking بديلة عن delay)
// =====================================================================
enum SmsStep { SMS_IDLE, SMS_STEP1_MODE, SMS_STEP2_NUMBER, SMS_STEP3_SENDING };
SmsStep smsStep = SMS_IDLE;
unsigned long smsStepTime = 0;

void startSMS() {
  if (smsStep == SMS_IDLE) {
    sim800l.println("AT+CMGF=1");
    smsStep = SMS_STEP1_MODE;
    smsStepTime = millis();
    Serial.println("📱 SMS: بدء الإرسال...");
  }
}

void updateSMS() {
  if (smsStep == SMS_IDLE) return;
  unsigned long elapsed = millis() - smsStepTime;

  switch (smsStep) {
    case SMS_STEP1_MODE:
      if (elapsed >= 500) {
        sim800l.print("AT+CMGS=\"");
        sim800l.print(PHONE_NUMBER);
        sim800l.println("\"");
        smsStep = SMS_STEP2_NUMBER;
        smsStepTime = millis();
      }
      break;

    case SMS_STEP2_NUMBER:
      if (elapsed >= 1000) {
        sim800l.print(SMS_TEXT);
        sim800l.write(26); // Ctrl+Z لإنهاء الرسالة
        smsStep = SMS_STEP3_SENDING;
        smsStepTime = millis();
      }
      break;

    case SMS_STEP3_SENDING:
      if (elapsed >= 5000) {
        smsStep = SMS_IDLE;
        Serial.println("📱 SMS: تم الإرسال");
      }
      break;

    default:
      smsStep = SMS_IDLE;
      break;
  }
}

// =====================================================================
// آلة حالة إرسال نبضة Capture (غير-blocking بديلة عن delay)
// =====================================================================
bool captureActive = false;
unsigned long captureStartTime = 0;

void startCapture() {
  digitalWrite(ESP_CAPTURE_PIN, HIGH);
  captureStartTime = millis();
  captureActive = true;
}

void updateCapture() {
  if (captureActive && (millis() - captureStartTime >= 100)) {
    digitalWrite(ESP_CAPTURE_PIN, LOW);
    captureActive = false;
  }
}

// =====================================================================
// قياس المسافة (مع تجاهل المنطقة العمياء)
// =====================================================================

long measureDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1;

  long distance = (duration * 0.0343) / 2.0;

  if (distance < 5) {
    return -1;
  }

  return distance;
}

// =====================================================================
// تحديد المنطقة
// =====================================================================

String getZone(long distance) {
  if (distance > 100) return "SAFE";
  else if (distance >= 71) return "ALERT";
  else if (distance >= 51) return "WARNING";
  else return "CRITICAL";
}

// =====================================================================
// تحديث LCD
// =====================================================================

void updateLCD(int angle, long distance, String status) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("A:");
  lcd.print(angle);
  lcd.print(" D:");
  if (distance < 0) lcd.print("---");
  else lcd.print(distance);
  lcd.print("cm");

  lcd.setCursor(0, 1);
  lcd.print(status);
}

// =====================================================================
// تحديث الإنذار
// =====================================================================

void updateAlert(unsigned long now, String status) {
  digitalWrite(BLUE_LED_PIN, (status == "ALERT" || status == "WARNING") ? HIGH : LOW);
  digitalWrite(RED_LED_PIN, (status == "CRITICAL") ? HIGH : LOW);

  if (status == "CRITICAL") {
    tone(BUZZER_PIN, 2400);
  } else if (status == "WARNING") {
    tone(BUZZER_PIN, 1700);
  } else if (status == "ALERT") {
    tone(BUZZER_PIN, 1200);
  } else {
    noTone(BUZZER_PIN);
  }
}

// =====================================================================
// إرسال البيانات عبر Serial (D0/D1)
// يصل هذا البث في آنٍ واحد إلى: (1) واجهة Processing عبر USB/COM3
// و(2) ESP32-CAM الموصول فعليًا بنفس D0/D1 (عبر خافض جهد إلزامي)
// =====================================================================

void sendData(int angle, long distance, String status) {
  Serial.print(angle);
  Serial.print(",");
  Serial.print(distance);
  Serial.print(",");
  Serial.println(status);
}

// =====================================================================
// الإعداد الأولي
// =====================================================================

void setup() {
  Serial.begin(115200);
  sim800l.begin(9600);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(ESP_CAPTURE_PIN, OUTPUT);

  digitalWrite(BLUE_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(ESP_CAPTURE_PIN, LOW);
  noTone(BUZZER_PIN);

  radarServo.attach(SERVO_PIN);
  radarServo.write(0);

  lcd.init();
  lcd.backlight();
  lcd.print("Smart Radar");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");
  delay(2000); // مقبول هنا لأنه قبل بدء loop() فقط

  Serial.println("📡 System Ready");
  Serial.println("📱 SMS will be sent to: " + String(PHONE_NUMBER));

  lcd.clear();
  lcd.print("System Ready");
  lcd.setCursor(0, 1);
  lcd.print("Scanning...");
}

// =====================================================================
// الحلقة الرئيسية - بدون أي delay() إطلاقًا
// =====================================================================

void loop() {
  unsigned long now = millis();

  // ----- 1. تحريك السيرفو (كل 20ms) -----
  if (now - lastServoTime >= SERVO_INTERVAL) {
    lastServoTime = now;
    radarServo.write(currentAngle);

    if (forward) {
      currentAngle += 2;
      if (currentAngle >= 180) { currentAngle = 180; forward = false; }
    } else {
      currentAngle -= 2;
      if (currentAngle <= 0) { currentAngle = 0; forward = true; }
    }
  }

  // ----- 2. قياس المسافة (كل 100ms) -----
  if (now - lastMeasureTime >= MEASURE_INTERVAL) {
    lastMeasureTime = now;

    long distance = measureDistance();
    lastDistance = distance;

    String status = (distance < 0) ? "NO ECHO" : getZone(distance);
    lastStatus = status;

    updateAlert(now, status);

    // معالجة الخطر (مرة واحدة، وبدون توقف التنفيذ)
    if (status == "CRITICAL" && !dangerEventSent) {
      dangerEventSent = true;
      startSMS();      // ← غير-blocking الآن
      startCapture();  // ← غير-blocking الآن
    }
    if (status != "CRITICAL") {
      dangerEventSent = false;
    }

    sendData(currentAngle, distance, status);  // → يصل لـ Processing (COM3) وESP32-CAM معًا عبر D0/D1

    Serial.print("📡 A:");
    Serial.print(currentAngle);
    Serial.print(" D:");
    Serial.print(distance);
    Serial.print(" S:");
    Serial.println(status);
  }

  // ----- 3. تحديث LCD (كل 300ms) -----
  if (now - lastLcdTime >= LCD_INTERVAL) {
    lastLcdTime = now;
    updateLCD(currentAngle, lastDistance, lastStatus);
  }

  // ----- 4. تحديث آلات الحالة غير-blocking (يجب استدعاؤها كل دورة) -----
  updateSMS();
  updateCapture();
}
