#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <NimBLEDevice.h>
#include <LittleFS.h>
#include <time.h>

// --- WiFi Credentials ---
const char* ssid = "WIFI_SSID";
const char* password = "WIFI_PASSWORD";

// --- BLE Variables ---
static NimBLEUUID serviceUUID("0000ffe0-0000-1000-8000-00805f9b34fb");
static NimBLEUUID charUUID("0000ffe1-0000-1000-8000-00805f9b34fb");
static NimBLERemoteCharacteristic* pChar = nullptr;

// --- Web Server ---
AsyncWebServer server(80);

// --- Sunrise ramp colors — NEVER changed by the UI ---
const uint8_t RAMP_R = 255, RAMP_G = 60, RAMP_B = 10;

// --- Sunrise State ---
bool isFading = false;
unsigned long fadeStartTime = 0;
unsigned long fadeDuration = 3600000;
uint8_t currentR = 0, currentG = 0, currentB = 0;

// --- Manual On color (only used by /on, never touches the ramp) ---
uint8_t manualR = 255, manualG = 255, manualB = 220;

// --- Fade startup pending (prevents white flash from powerOn) ---
bool fadePending = false;
unsigned long fadePendingTime = 0;

// --- Pending instant-on color send (150ms gap after powerOn) ---
bool pendingColorSend = false;
unsigned long pendingColorTime = 0;

// --- Reliable repeat: resend last command up to 2 more times ---
uint8_t repeatCmd[9];
size_t repeatCmdLen = 0;
int repeatCount = 0;
unsigned long lastRepeatTime = 0;

// --- Alarm State ---
bool alarmEnabled = false;
int alarmHour = 7;
int alarmMin = 0;
bool alarmTriggeredToday = false;
int lastResetDay = -1;
long utcOffsetSeconds = -18000; // EST (UTC-5)

const char* ntpServer = "pool.ntp.org";

// --- BLE Helpers ---
void sendCommand(uint8_t* cmd, size_t size) {
  if (pChar) {
    pChar->writeValue(cmd, size, false);
  } else {
    Serial.println("[BLE] sendCommand skipped — not connected");
  }
}

// Send immediately + queue 2 retries at 120ms intervals
void sendReliable(uint8_t* cmd, size_t size) {
  sendCommand(cmd, size);
  memcpy(repeatCmd, cmd, size);
  repeatCmdLen = size;
  repeatCount = 2;
  lastRepeatTime = millis();
}

void powerOn() {
  Serial.println("[BLE] powerOn");
  uint8_t cmd[] = {0x7B, 0xFF, 0x04, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xBF};
  sendReliable(cmd, sizeof(cmd));
}

void powerOff() {
  Serial.println("[BLE] powerOff");
  uint8_t cmd[] = {0x7B, 0xFF, 0x04, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xBF};
  sendReliable(cmd, sizeof(cmd));
  isFading = false;
  fadePending = false;
}

void setColor(uint8_t r, uint8_t g, uint8_t b) {
  Serial.printf("[BLE] setColor r=%d g=%d b=%d\n", r, g, b);
  uint8_t cmd[] = {0x7B, 0xFF, 0x07, r, g, b, 0x00, 0xFF, 0xBF};
  sendCommand(cmd, sizeof(cmd)); // ramp uses single sends (high frequency already)
}

void setColorReliable(uint8_t r, uint8_t g, uint8_t b) {
  Serial.printf("[BLE] setColorReliable r=%d g=%d b=%d\n", r, g, b);
  uint8_t cmd[] = {0x7B, 0xFF, 0x07, r, g, b, 0x00, 0xFF, 0xBF};
  sendReliable(cmd, sizeof(cmd));
}

void startRamp() {
  Serial.printf("[RAMP] Starting — duration=%lu\n", fadeDuration);
  pendingColorSend = false;
  isFading = false;
  powerOn();
  // Wait 200ms for strip to wake, then snap to black and begin ramp
  fadePending = true;
  fadePendingTime = millis();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 1. WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("\nIP: ");
  Serial.println(WiFi.localIP());

  // 2. NTP
  configTime(utcOffsetSeconds, 0, ntpServer);

  // 3. LittleFS
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
  }

  // 4. CORS
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

  // --- API Endpoints ---

  // Manual ramp start: /start?time=ms
  server.on("/start", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("time")) fadeDuration = request->getParam("time")->value().toInt();
    Serial.printf("[API] /start duration=%lu\n", fadeDuration);
    startRamp();
    request->send(200, "text/plain", "OK");
  });

  // Stop / power off
  server.on("/stop", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("[API] /stop");
    pendingColorSend = false;
    powerOff();
    request->send(200, "text/plain", "OK");
  });

  // Instant full-brightness on: /on?r=&g=&b=
  server.on("/on", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("r")) manualR = request->getParam("r")->value().toInt();
    if (request->hasParam("g")) manualG = request->getParam("g")->value().toInt();
    if (request->hasParam("b")) manualB = request->getParam("b")->value().toInt();
    Serial.printf("[API] /on r=%d g=%d b=%d\n", manualR, manualG, manualB);
    isFading = false;
    fadePending = false;
    powerOn();
    pendingColorSend = true;
    pendingColorTime = millis();
    request->send(200, "text/plain", "OK");
  });

  // Set alarm: /setalarm?hour=&min=&duration=&enabled=&utcoffset=
  server.on("/setalarm", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("hour"))      alarmHour = request->getParam("hour")->value().toInt();
    if (request->hasParam("min"))       alarmMin = request->getParam("min")->value().toInt();
    if (request->hasParam("duration"))  fadeDuration = request->getParam("duration")->value().toInt();
    if (request->hasParam("enabled"))   alarmEnabled = request->getParam("enabled")->value().toInt() == 1;
    if (request->hasParam("utcoffset")) {
      utcOffsetSeconds = request->getParam("utcoffset")->value().toInt();
      configTime(utcOffsetSeconds, 0, ntpServer);
    }
    Serial.printf("[API] /setalarm %02d:%02d duration=%lu enabled=%d\n",
      alarmHour, alarmMin, fadeDuration, alarmEnabled);
    alarmTriggeredToday = false;
    request->send(200, "text/plain", "OK");
  });

  // Status JSON
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    struct tm timeinfo;
    char timeBuf[6] = "--:--";
    if (getLocalTime(&timeinfo)) {
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    }

    float progress = 0.0f;
    if (isFading) {
      unsigned long elapsed = millis() - fadeStartTime;
      progress = min(1.0f, (float)elapsed / (float)fadeDuration);
    }

    String json = "{";
    json += "\"time\":\"" + String(timeBuf) + "\",";
    json += "\"alarmEnabled\":" + String(alarmEnabled ? "true" : "false") + ",";
    json += "\"alarmHour\":" + String(alarmHour) + ",";
    json += "\"alarmMin\":" + String(alarmMin) + ",";
    json += "\"fadeDuration\":" + String(fadeDuration) + ",";
    json += "\"isFading\":" + String(isFading ? "true" : "false") + ",";
    json += "\"progress\":" + String(progress, 3) + ",";
    json += "\"utcOffset\":" + String(utcOffsetSeconds);
    json += "}";

    request->send(200, "application/json", json);
  });

  // Serve frontend from LittleFS
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.begin();

  // 5. BLE init
  NimBLEDevice::init("");
}

void loop() {
  // BLE connection
  if (!pChar) {
    Serial.println("[BLE] Scanning...");
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setActiveScan(true);
    NimBLEScanResults results = pScan->start(5, false);
    for (int i = 0; i < results.getCount(); i++) {
      NimBLEAdvertisedDevice device = results.getDevice(i);
      if (device.getName() == "LEDDMX-00-6627") {
        Serial.println("[BLE] Found LEDDMX-00-6627, connecting...");
        NimBLEClient* pClient = NimBLEDevice::createClient();
        if (pClient->connect(&device)) {
          NimBLERemoteService* pService = pClient->getService(serviceUUID);
          if (pService) {
            pChar = pService->getCharacteristic(charUUID);
            if (pChar) Serial.println("[BLE] Connected and characteristic found");
            else       Serial.println("[BLE] Characteristic not found");
          } else {
            Serial.println("[BLE] Service not found");
          }
        } else {
          Serial.println("[BLE] Connection failed");
        }
      }
    }
    if (!pChar) delay(2000);
  }

  // Fade startup: 200ms after powerOn, snap to black then begin ramp
  if (fadePending && pChar && (millis() - fadePendingTime > 200)) {
    fadePending = false;
    currentR = 0; currentG = 0; currentB = 0;
    setColor(0, 0, 0);
    isFading = true;
    fadeStartTime = millis();
    Serial.println("[RAMP] Began fading from black");
  }

  // Reliable repeat: resend queued command up to 2 more times
  if (repeatCount > 0 && pChar && (millis() - lastRepeatTime >= 120)) {
    pChar->writeValue(repeatCmd, repeatCmdLen, false);
    repeatCount--;
    lastRepeatTime = millis();
    Serial.printf("[BLE] retry, %d left\n", repeatCount);
  }

  // Pending instant-on color send (150ms after powerOn)
  if (pendingColorSend && pChar && (millis() - pendingColorTime > 150)) {
    pendingColorSend = false;
    setColorReliable(manualR, manualG, manualB);
  }

  // Alarm check
  if (!isFading && !fadePending && alarmEnabled && pChar) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      if (timeinfo.tm_mday != lastResetDay) {
        alarmTriggeredToday = false;
        lastResetDay = timeinfo.tm_mday;
      }
      if (timeinfo.tm_hour == alarmHour && timeinfo.tm_min == alarmMin && !alarmTriggeredToday) {
        Serial.println("[ALARM] Triggered!");
        alarmTriggeredToday = true;
        startRamp();
      }
    }
  }

  // Sunrise ramp
  if (isFading && pChar) {
    unsigned long elapsed = millis() - fadeStartTime;
    if (elapsed >= fadeDuration) {
      setColor(RAMP_R, RAMP_G, RAMP_B);
      isFading = false;
      Serial.println("[RAMP] Complete");
    } else {
      float progress = (float)elapsed / (float)fadeDuration;
      float curve = progress * progress * progress;
      uint8_t newR = (uint8_t)(RAMP_R * curve);
      uint8_t newG = (uint8_t)(RAMP_G * curve);
      uint8_t newB = (uint8_t)(RAMP_B * curve);
      if (newR != currentR || newG != currentG || newB != currentB) {
        currentR = newR; currentG = newG; currentB = newB;
        setColor(currentR, currentG, currentB);
      }
    }
    delay(50);
  }
}
