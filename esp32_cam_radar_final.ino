/*
  ============================================================================
  Smart Radar System — ESP32-CAM Web Server (النسخة النهائية المدمجة)
  ============================================================================
  هذه النسخة تدمج كل ما كان صحيحًا في محاولتك الأخيرة، وتضيف الإصلاح
  الجذري الوحيد الذي كان ناقصًا:

  ✅ من نسختك: تخزين الصورة كبيانات ثنائية خام (لا base64) → يوفر ذاكرة
  ✅ من نسختك: قراءة Serial لا-حاجزة حرفًا بحرف → لا فقدان بيانات
  ✅ من نسختك: إعادة اتصال Wi-Fi تلقائية عند الانقطاع
  ✅ من نسختك: CAMERA_GRAB_LATEST لبث أكثر سلاسة
  ✅ من نسختك: setContentLength بدل sendHeader اليدوي

  🔧 الإصلاح الجذري المضاف الآن:
  البث أصبح يعمل في Task منفصل تمامًا على النواة Core 0، على منفذ
  مستقل (81)، بحيث لا يحتكر خادم اللوحة الرئيسي (80) أبدًا — وهذا
  يحل المشكلتين معًا في آنٍ واحد:
    • الصفحة تتحدّث الآن فعليًا (عبر AJAX خفيف كل ثانية، بدل
      meta-refresh الذي كان عاجزًا عن الوصول للسيرفر المشغول)
    • صورة آخر التقاط تظهر فورًا لأن /captured_image يُخدَّم مباشرة
      دون انتظار انتهاء البث
  ============================================================================
*/

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>

// ====================================================================
// إعدادات Wi-Fi (شبكتان مختلفتان)
// ====================================================================

const char* WIFI_SSID_1     = "khalifa";
const char* WIFI_PASSWORD_1 = "7379518867";

const char* WIFI_SSID_2     = "MyHomeWiFi";
const char* WIFI_PASSWORD_2 = "MyPassword123";

// ====================================================================
// إعدادات الكاميرا (AI-Thinker ESP32-CAM)
// ====================================================================

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM       5

#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ====================================================================
// متغيرات تخزين البيانات القادمة من الأردوينو
// ====================================================================

float lastDistance = 0.0;
int lastAngle = 0;
String lastStatus = "WAITING";

// الصورة الملتقطة: بيانات ثنائية خام (توفير ذاكرة، بدون base64)
uint8_t* capturedImageBuf = nullptr;
size_t   capturedImageLen = 0;
int capturedAngle = 0;
float capturedDistance = 0;
String capturedStatus = "";

bool autoCaptureEnabled = true;
int photoCount = 0;
unsigned long lastCaptureTime = 0;

// حاجز استقبال Serial اللا-حاجز
String serialLineBuffer = "";

// توقيت فحص Wi-Fi الدوري
unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL_MS = 10000;

// ====================================================================
// كائنات النظام
// ====================================================================

WebServer server(80);        // لوحة التحكم + البيانات + الالتقاط
WiFiServer streamServer(81); // بث الفيديو المباشر (Task منفصل تمامًا)
SemaphoreHandle_t camMutex;  // حماية الوصول للكاميرا من أكثر من Task

// ====================================================================
// دالة: الاتصال بشبكة Wi-Fi (مع Fallback لشبكتين)
// ====================================================================

void connectWiFi() {
    WiFi.mode(WIFI_STA);

    Serial.println("========================================");
    Serial.println("📶 Starting Wi-Fi Connection...");
    Serial.println("========================================");

    Serial.print("📶 Connecting to Primary Network: ");
    Serial.println(WIFI_SSID_1);

    WiFi.begin(WIFI_SSID_1, WIFI_PASSWORD_1);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ Wi-Fi Connected (Primary)");
        Serial.print("🌐 IP Address: ");
        Serial.println(WiFi.localIP());
        Serial.println("========================================");
        return;
    }

    Serial.println("\n❌ Primary network failed. Trying backup...");
    Serial.println("========================================");

    WiFi.disconnect(true);
    delay(200);

    Serial.print("📶 Connecting to Backup Network: ");
    Serial.println(WIFI_SSID_2);

    WiFi.begin(WIFI_SSID_2, WIFI_PASSWORD_2);

    attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ Wi-Fi Connected (Backup)");
        Serial.print("🌐 IP Address: ");
        Serial.println(WiFi.localIP());
        Serial.println("========================================");
    } else {
        Serial.println("\n❌ All Wi-Fi Networks Failed!");
        Serial.println("========================================");
    }
}

// ====================================================================
// دالة: تهيئة الكاميرا
// ====================================================================

bool initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    if (psramFound()) {
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 10;
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
        Serial.println("✅ PSRAM Found - VGA Mode");
    } else {
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
        Serial.println("⚠️ PSRAM Not Found - QVGA Mode");
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.print("❌ Camera Init Failed: ");
        Serial.println(err);
        return false;
    }
    Serial.println("✅ Camera Initialized");
    return true;
}

// ====================================================================
// دوال مساعدة: تصنيف الحالة (مطابقة تمامًا لنصوص الأردوينو الفعلية)
// ====================================================================

bool isDangerStatus(const String &s)  { return s == "CRITICAL"; }
bool isWarningStatus(const String &s) { return s == "WARNING" || s == "ALERT"; }
bool isUnknownStatus(const String &s) { return s == "NO ECHO" || s == "WAITING"; }

String formatDistance(float d) {
    if (d < 0) return "---";
    return String(d, 2);
}

// ====================================================================
// دالة: حفظ إطار ملتقط في المخزن الثابت (تحرر القديم أولاً)
// ====================================================================

void saveCapturedFrame(camera_fb_t *fb) {
    if (capturedImageBuf != nullptr) {
        free(capturedImageBuf);
        capturedImageBuf = nullptr;
        capturedImageLen = 0;
    }

    capturedImageBuf = (uint8_t*) malloc(fb->len);
    if (capturedImageBuf == nullptr) {
        Serial.println("❌ Failed to allocate memory for captured image");
        return;
    }

    memcpy(capturedImageBuf, fb->buf, fb->len);
    capturedImageLen = fb->len;
    capturedAngle = lastAngle;
    capturedDistance = lastDistance;
    capturedStatus = lastStatus;
    photoCount++;
    lastCaptureTime = millis();
}

// التقاط آمن (محمي بـ Mutex لأن الكاميرا مشتركة الآن مع Task البث)
bool captureAndStore() {
    bool ok = false;
    if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            saveCapturedFrame(fb);
            esp_camera_fb_return(fb);
            ok = true;
        }
        xSemaphoreGive(camMutex);
    }
    return ok;
}

// ====================================================================
// دالة: التقاط صورة عند الطلب اليدوي المباشر (ترجع الصورة الخام)
// ====================================================================

void handleCapture() {
    Serial.println("📸 Manual Capture Requested (raw)");

    if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        server.send(503, "text/plain", "Camera busy, try again");
        return;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        xSemaphoreGive(camMutex);
        server.send(500, "text/plain", "Camera capture failed");
        return;
    }

    server.setContentLength(fb->len);
    server.send(200, "image/jpeg", "");
    server.client().write(fb->buf, fb->len);

    saveCapturedFrame(fb);
    esp_camera_fb_return(fb);
    xSemaphoreGive(camMutex);
    Serial.println("✅ Photo sent");
}

// زر "Take Snapshot" في اللوحة (AJAX - بدون مغادرة الصفحة)
void handleTakeSnapshot() {
    Serial.println("📸 Manual Capture Requested (AJAX)");
    bool ok = captureAndStore();
    server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// تقديم آخر صورة محفوظة كملف ثنائي خام مباشر
void handleCapturedImage() {
    if (capturedImageBuf == nullptr || capturedImageLen == 0) {
        server.send(404, "text/plain", "No image captured yet");
        return;
    }
    server.setContentLength(capturedImageLen);
    server.send(200, "image/jpeg", "");
    server.client().write(capturedImageBuf, capturedImageLen);
}

// ====================================================================
// دالة: بث الفيديو المباشر (منفذ 81، Task مستقل على Core 0)
// هذا هو الإصلاح الجذري: لا يلمس WebServer(80) إطلاقًا، لذا لا يمكنه
// حجب أي طلب آخر (اللوحة، البيانات، الصورة الملتقطة) بعد الآن.
// ====================================================================

void streamTask(void *parameter) {
    streamServer.begin();
    Serial.println("✅ Stream Server started on port 81");

    for (;;) {
        WiFiClient client = streamServer.available();

        if (client) {
            Serial.println("📹 Stream client connected");
            const char* boundary = "123456789000000000000987654321";

            client.print("HTTP/1.1 200 OK\r\n");
            client.print("Content-Type: multipart/x-mixed-replace; boundary=");
            client.print(boundary);
            client.print("\r\n\r\n");

            while (client.connected()) {
                if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    camera_fb_t *fb = esp_camera_fb_get();
                    if (!fb) {
                        xSemaphoreGive(camMutex);
                        Serial.println("❌ Camera capture failed (stream)");
                        break;
                    }

                    client.print("--");
                    client.print(boundary);
                    client.print("\r\n");
                    client.print("Content-Type: image/jpeg\r\n");
                    client.print("Content-Length: ");
                    client.print(fb->len);
                    client.print("\r\n\r\n");
                    client.write(fb->buf, fb->len);
                    client.print("\r\n");

                    esp_camera_fb_return(fb);
                    xSemaphoreGive(camMutex);
                } else {
                    Serial.println("⚠️ Camera busy, skipping frame");
                }

                vTaskDelay(pdMS_TO_TICKS(40)); // ≈ 25 fps كحد أقصى + إعطاء فرصة لبقية الـ Tasks
            }

            client.stop();
            Serial.println("📹 Stream client disconnected");
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ====================================================================
// دالة: صفحة HTML الرئيسية (بدون meta refresh — تحديث عبر AJAX فقط)
// ====================================================================

void handleRoot() {
    String ip = WiFi.localIP().toString();
    String streamUrl = "http://" + ip + ":81/stream";

    String html = "";
    html += "<!DOCTYPE html><html>";
    html += "<head><meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>Smart Radar - ESP32-CAM</title>";
    html += "<style>";
    html += "body{background:#0a0a1a;color:#00ff88;font-family:Arial;text-align:center;padding-top:20px;}";
    html += "h1{color:#00ff88;text-shadow:0 0 20px #00ff88;}";
    html += ".container{display:flex;flex-wrap:wrap;justify-content:center;gap:20px;max-width:1200px;margin:auto;}";
    html += ".card{background:#1a1a2e;border:2px solid #00ff88;border-radius:15px;padding:20px;width:45%;min-width:300px;box-sizing:border-box;}";
    html += ".card h2{color:#00ff88;margin-top:0;border-bottom:1px solid #00ff88;padding-bottom:10px;}";
    html += "img{max-width:100%;border-radius:10px;border:2px solid #00ff88;}";
    html += ".data-box{background:#0a0a1a;border:2px solid #00ff88;border-radius:10px;padding:15px;margin:10px 0;}";
    html += ".data-box span{color:#ffaa00;font-size:24px;font-weight:bold;}";
    html += ".info{color:#888;margin-top:10px;}";
    html += ".danger{color:#ff4444;}";
    html += ".warning{color:#ffcc00;}";
    html += ".alertc{color:#00aaff;}";
    html += ".safe{color:#00ff88;}";
    html += ".unknown{color:#888;}";
    html += "a,button{color:#00ff88;background:transparent;text-decoration:none;font-size:15px;";
    html += "padding:10px 20px;border:2px solid #00ff88;border-radius:8px;display:inline-block;margin:6px;cursor:pointer;font-family:Arial;box-sizing:border-box;}";
    html += "a:hover,button:hover{background:#00ff88;color:#0a0a1a;}";
    html += ".btnRow{display:flex;flex-wrap:wrap;justify-content:center;}";
    html += ".grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;}";
    html += "@media(max-width:700px){.card{width:90%;}.grid{grid-template-columns:1fr;}}";
    html += "</style>";
    html += "</head><body>";

    html += "<h1>📡 SMART RADAR SYSTEM</h1>";

    html += "<div class='container'>";

    // بطاقة الفيديو — لا يُعاد تحميلها أبدًا بعد الآن (لا meta refresh)
    html += "<div class='card'>";
    html += "<h2>🎥 Live Video Stream</h2>";
    html += "<img id='liveStream' src='" + streamUrl + "' alt='Live Stream'>";
    html += "<p class='info'>🟢 بث مباشر للكاميرا</p>";
    html += "</div>";

    // بطاقة البيانات
    html += "<div class='card'>";
    html += "<h2>📊 Radar Data</h2>";
    html += "<div class='data-box'>";
    html += "<div class='grid'>";
    html += "<div><p>📏 Distance</p><span id='distance'>--- cm</span></div>";
    html += "<div><p>🎯 Angle</p><span id='angle'>--°</span></div>";
    html += "</div>";
    html += "</div>";

    html += "<div class='data-box'>";
    html += "<p>📌 Status</p>";
    html += "<span id='statusBadge'>⏳ WAITING</span>";
    html += "</div>";

    html += "<div class='data-box'>";
    html += "<p>📸 Photos Captured: <span id='photoCount' style='color:#00ff88;'>0</span></p>";
    html += "</div>";

    html += "<div class='btnRow'>";
    html += "<button onclick='takeSnapshot()'>📸 Take Snapshot</button>";
    html += "<a href='/captured' target='_blank'>🖼️ View Last Photo</a>";
    html += "<a href='" + streamUrl + "' target='_blank'>📹 View Stream</a>";
    html += "<button id='autoBtn' onclick='toggleAuto()'>⏸️ Stop Auto Capture</button>";
    html += "</div>";
    html += "</div>"; // إغلاق بطاقة Radar Data

    // بطاقة آخر صورة
    html += "<div class='card' style='width:100%;'>";
    html += "<h2>🖼️ Last Captured Image</h2>";
    html += "<div id='lastImageWrap'>";
    html += "<p class='info'>⏳ No image captured yet</p>";
    html += "<p class='info'>Press 'Take Snapshot' to capture</p>";
    html += "</div>";
    html += "</div>";

    html += "</div>"; // إغلاق .container

    html += "<div class='info' style='margin-top:30px;'>";
    html += "🌐 IP: " + ip + " &nbsp;|&nbsp; ";
    html += "📡 Smart Radar System v2.2";
    html += "</div>";

    // ---------------- JavaScript: تحديث الأرقام والصورة عبر AJAX ----------------
    html += "<script>";
    html += "let lastKnownPhotoCount = -1;";

    html += "function statusInfo(s){";
    html += "  if(s==='CRITICAL') return {cls:'danger', icon:'⛔'};";
    html += "  if(s==='WARNING')  return {cls:'warning', icon:'⚠️'};";
    html += "  if(s==='ALERT')    return {cls:'alertc', icon:'🔶'};";
    html += "  if(s==='SAFE')     return {cls:'safe', icon:'✅'};";
    html += "  return {cls:'unknown', icon:'❔'};";
    html += "}";

    html += "function renderLastImage(d){";
    html += "  const info = statusInfo(d.capturedStatus);";
    html += "  let h = \"<img src='/captured_image?t=\" + Date.now() + \"'>\";";
    html += "  h += \"<div class='data-box'>\";";
    html += "  h += \"<p>📏 Distance: <span>\" + (d.capturedDistance < 0 ? '---' : d.capturedDistance.toFixed(2)) + \" cm</span> &nbsp;|&nbsp; 🎯 Angle: <span>\" + d.capturedAngle + \"°</span></p>\";";
    html += "  h += \"<p>📌 Status: <span class='\" + info.cls + \"'>\" + info.icon + ' ' + d.capturedStatus + \"</span></p>\";";
    html += "  h += \"</div>\";";
    html += "  document.getElementById('lastImageWrap').innerHTML = h;";
    html += "}";

    html += "async function updateStatus(){";
    html += "  try{";
    html += "    const r = await fetch('/status');";
    html += "    const d = await r.json();";
    html += "    document.getElementById('distance').textContent = (d.distance < 0 ? '---' : d.distance.toFixed(2)) + ' cm';";
    html += "    document.getElementById('angle').textContent = d.angle + '°';";
    html += "    const info = statusInfo(d.status);";
    html += "    const badge = document.getElementById('statusBadge');";
    html += "    badge.className = info.cls;";
    html += "    badge.textContent = info.icon + ' ' + d.status;";
    html += "    document.getElementById('photoCount').textContent = d.photoCount;";
    html += "    document.getElementById('autoBtn').textContent = d.autoCapture ? '⏸️ Stop Auto Capture' : '▶️ Start Auto Capture';";
    html += "    if(d.hasImage && d.photoCount !== lastKnownPhotoCount){";
    html += "      lastKnownPhotoCount = d.photoCount;";
    html += "      renderLastImage(d);";
    html += "    }";
    html += "  }catch(e){ console.log('status poll failed', e); }";
    html += "}";

    html += "async function takeSnapshot(){ await fetch('/take_snapshot'); await updateStatus(); }";

    html += "async function toggleAuto(){";
    html += "  const r = await fetch('/toggle_auto');";
    html += "  const d = await r.json();";
    html += "  document.getElementById('autoBtn').textContent = d.autoCapture ? '⏸️ Stop Auto Capture' : '▶️ Start Auto Capture';";
    html += "}";

    html += "updateStatus();";
    html += "setInterval(updateStatus, 1000);";
    html += "</script>";

    html += "</body></html>";

    server.send(200, "text/html", html);
}

// نقطة بيانات JSON خفيفة (تُستطلع كل ثانية)
void handleStatus() {
    String json = "{";
    json += "\"angle\":" + String(lastAngle) + ",";
    json += "\"distance\":" + String(lastDistance, 2) + ",";
    json += "\"status\":\"" + lastStatus + "\",";
    json += "\"photoCount\":" + String(photoCount) + ",";
    json += "\"autoCapture\":" + String(autoCaptureEnabled ? "true" : "false") + ",";
    json += "\"hasImage\":" + String(capturedImageLen > 0 ? "true" : "false") + ",";
    json += "\"capturedAngle\":" + String(capturedAngle) + ",";
    json += "\"capturedDistance\":" + String(capturedDistance, 2) + ",";
    json += "\"capturedStatus\":\"" + capturedStatus + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

// ====================================================================
// دالة: عرض آخر صورة ملتقطة (صفحة مستقلة كاملة)
// ====================================================================

void handleCaptured() {
    if (capturedImageLen > 0) {
        String html = "";
        html += "<!DOCTYPE html><html>";
        html += "<head><meta charset='UTF-8'>";
        html += "<title>Last Captured Image</title>";
        html += "<style>";
        html += "body{background:#0a0a1a;color:#00ff88;font-family:Arial;text-align:center;padding-top:30px;}";
        html += "h1{color:#00ff88;}";
        html += "img{max-width:90%;border:3px solid #00ff88;border-radius:10px;}";
        html += ".info{color:#888;margin-top:20px;}";
        html += "a{color:#00ff88;text-decoration:none;padding:10px 20px;border:1px solid #00ff88;border-radius:5px;}";
        html += "a:hover{background:#00ff88;color:#0a0a1a;}";
        html += ".danger{color:#ff4444;}";
        html += ".warning{color:#ffcc00;}";
        html += ".alertc{color:#00aaff;}";
        html += ".safe{color:#00ff88;}";
        html += "</style>";
        html += "</head><body>";
        html += "<h1>🖼️ Last Captured Image</h1>";
        html += "<img src='/captured_image'>";
        html += "<div class='info'>";
        html += "📏 Distance: <span style='color:#ffaa00;'>" + formatDistance(capturedDistance) + " cm</span> &nbsp;|&nbsp; ";
        html += "🎯 Angle: <span style='color:#ffaa00;'>" + String(capturedAngle) + "°</span>";
        html += "<br>📌 Status: <span ";
        if (isDangerStatus(capturedStatus)) {
            html += "class='danger'>⛔ " + capturedStatus;
        } else if (isWarningStatus(capturedStatus)) {
            html += "class='warning'>⚠️ " + capturedStatus;
        } else if (isUnknownStatus(capturedStatus)) {
            html += "class='info'>❔ " + capturedStatus;
        } else {
            html += "class='safe'>✅ " + capturedStatus;
        }
        html += "</span>";
        html += "</div>";
        html += "<p><a href='/'>⬅️ Back to Dashboard</a></p>";
        html += "</body></html>";
        server.send(200, "text/html", html);
    } else {
        server.send(404, "text/plain", "No image captured yet");
    }
}

void handleToggleAuto() {
    autoCaptureEnabled = !autoCaptureEnabled;
    String json = "{\"autoCapture\":" + String(autoCaptureEnabled ? "true" : "false") + "}";
    server.send(200, "application/json", json);
}

// ====================================================================
// دالة: إعداد خادم الويب (منفذ 80 فقط — البث منفصل على 81)
// ====================================================================

void setupWebServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/take_snapshot", HTTP_GET, handleTakeSnapshot);
    server.on("/capture", HTTP_GET, handleCapture);
    server.on("/captured", HTTP_GET, handleCaptured);
    server.on("/captured_image", HTTP_GET, handleCapturedImage);
    server.on("/toggle_auto", HTTP_GET, handleToggleAuto);

    server.begin();
    Serial.println("✅ Web Server Started (port 80)");
}

// ====================================================================
// دالة: معالجة سطر بيانات مكتمل قادم من الأردوينو
// ====================================================================

void processSerialLine(const String &data) {
    // تجاهل أي سطر لا يبدأ برقم (رسائل تشخيص الأردوينو المشتركة على D0/D1)
    if (data.length() == 0 || !isDigit(data.charAt(0))) return;

    int firstComma = data.indexOf(',');
    int secondComma = data.indexOf(',', firstComma + 1);

    if (firstComma > 0 && secondComma > 0) {
        String angleStr = data.substring(0, firstComma);
        String distStr = data.substring(firstComma + 1, secondComma);
        String statusStr = data.substring(secondComma + 1);
        statusStr.trim();

        lastAngle = angleStr.toInt();
        lastDistance = distStr.toFloat();
        lastStatus = statusStr;

        if (autoCaptureEnabled && isDangerStatus(lastStatus)) {
            if (photoCount == 0 || (millis() - lastCaptureTime > 3000)) {
                Serial.println("📸 Auto Capture Triggered!");
                if (captureAndStore()) {
                    Serial.println("✅ Auto Photo Captured");
                }
            }
        }
    }
}

// قراءة Serial لا-حاجزة حرفًا بحرف
void pollSerialData() {
    while (Serial.available()) {
        char c = (char) Serial.read();

        if (c == '\n') {
            serialLineBuffer.trim();
            if (serialLineBuffer.length() > 0) {
                processSerialLine(serialLineBuffer);
            }
            serialLineBuffer = "";
        } else if (c != '\r') {
            serialLineBuffer += c;
            if (serialLineBuffer.length() > 120) {
                serialLineBuffer = ""; // حماية من نمو غير محدود عند بيانات تالفة
            }
        }
    }
}

// ====================================================================
// دالة الإعداد الأولي
// ====================================================================

void setup() {
    Serial.begin(115200);

    delay(3000);

    Serial.println("========================================");
    Serial.println("📷 ESP32-CAM Initializing...");
    Serial.println("========================================");

    camMutex = xSemaphoreCreateMutex();

    if (!initCamera()) {
        Serial.println("❌ Camera init failed. Restarting...");
        delay(1000);
        ESP.restart();
    }

    connectWiFi();
    setupWebServer();

    // تشغيل مهمة البث على Core 0 — مستقلة تمامًا عن الحلقة الرئيسية
    xTaskCreatePinnedToCore(
        streamTask, "streamTask", 8192, NULL, 1, NULL, 0
    );

    Serial.println("========================================");
    Serial.println("✅ ESP32-CAM Ready");
    Serial.println("📷 Dashboard: http://" + WiFi.localIP().toString() + "/");
    Serial.println("📷 Stream:    http://" + WiFi.localIP().toString() + ":81/stream");
    Serial.println("📷 Snapshot:  http://" + WiFi.localIP().toString() + "/capture");
    Serial.println("========================================");
}

// ====================================================================
// الحلقة الرئيسية (Core 1) — حرة تمامًا الآن، لا يحجبها البث إطلاقًا
// ====================================================================

void loop() {
    server.handleClient();
    pollSerialData();

    if (millis() - lastWiFiCheck > WIFI_CHECK_INTERVAL_MS) {
        lastWiFiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("⚠️ Wi-Fi disconnected. Reconnecting...");
            connectWiFi();
        }
    }

    delay(2);
}
