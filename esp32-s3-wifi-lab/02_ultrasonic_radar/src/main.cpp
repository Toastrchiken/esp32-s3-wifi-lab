/*
 * Project 02 — Wi-Fi ultrasonic "radar" security scanner
 * Board:     ESP32-S3-DevKitC-1  ·  Framework: Arduino (PlatformIO espressif32, core 2.0.x; 3.x OK)
 *
 * An HC-SR04 ultrasonic sensor rides on an SG90 servo that sweeps 0–180°.
 * The ESP32 hosts a phone dashboard over Wi-Fi:
 *   - live radar display of range vs bearing
 *   - manual aiming of the sensor from the phone
 *   - "Learn room": averages 3 sweeps into a clutter map (baseline range per bearing)
 *   - "Arm": anything closer than the baseline by > threshold is a target; an alarm
 *     needs 2 of 3 pings at that bearing to agree (M-of-N confirmation)
 *   - event log with timestamps, CSV export, optional ntfy.sh push notifications
 *
 * Wi-Fi: joins your router if include/secrets.h has credentials (STA), otherwise
 * starts its own network "ESP32-Radar" / "radar1234" (AP) at http://192.168.4.1
 *
 * Wiring (see README):
 *   Servo signal -> GPIO4    HC-SR04 TRIG -> GPIO5
 *   HC-SR04 ECHO -> 1k -> GPIO6, and GPIO6 -> 2k -> GND   (5 V echo -> 3.3 V!)
 *   Servo + HC-SR04 VCC -> 5V0, all grounds common
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef NTFY_TOPIC
#define NTFY_TOPIC ""
#endif

#include "index_html.h"

// ------------------------------------------------------------------ pins & constants
static const int PIN_SERVO = 4;
static const int PIN_TRIG  = 5;
static const int PIN_ECHO  = 6;            // through the 1k/2k divider

// Servo pulse range. Calibrate: if 0/180 don't reach the ends or the servo buzzes
// against its stop, narrow/widen these (typical SG90: 500–2400 us).
static const int SERVO_MIN_US = 500;
static const int SERVO_MAX_US = 2400;
static const int LEDC_CH = 0, LEDC_FREQ = 50, LEDC_BITS = 14;

static const float    MIN_CM = 2.0f, MAX_CM = 400.0f;   // HC-SR04 usable range
static const uint32_t ECHO_TIMEOUT_US = 25000;          // ~4.3 m round trip
static const uint32_t PING_GAP_MS = 50;                 // let old echoes die out
static const int      SCAN_MIN = 0, SCAN_MAX = 180;

static const char*    HOSTNAME = "esp32-radar";
static const char*    AP_SSID  = "ESP32-Radar";
static const char*    AP_PASS  = "radar1234";           // >= 8 chars for WPA2
static const uint32_t STA_TIMEOUT_MS = 12000;

static const uint8_t  LEARN_SWEEPS = 3;
static const uint32_t ALERT_COOLDOWN_MS = 15000;        // re-alarm suppression window...
static const int      ALERT_GROUP_DEG = 15;             // ...for targets within ±15°
static const uint32_t PUSH_COOLDOWN_MS   = 30000;       // max 1 push per 30 s

WebServer http(80);

// ------------------------------------------------------------------ settings (from page)
int   stepDeg = 5;        // sweep step
int   settleMs = 60;      // wait after each servo move before pinging
int   pingsPerAngle = 2;  // when not armed (armed always uses >= 3)
int   thresholdCm = 25;   // min intrusion depth
float tempC = 20.0f;      // air temperature for speed of sound

// ------------------------------------------------------------------ state
enum Mode { M_IDLE, M_SCAN, M_AIM };
Mode     mode = M_IDLE;
bool     armed = false, alarmOn = false, learning = false, learned = false;
bool     apMode = false;
int      servoAngle = 90;
bool     servoOn = false;
int      sweepDir = 1;
uint32_t tMoved = 0, tLastPing = 0;
float    lastDist = -1;

float    pingRaw[5];
int      pingCount = 0;

uint8_t  sweepsDone = 0;
float    learnVals[181][LEARN_SWEEPS];
uint8_t  learnN[181];
float    baseline[181];
int      baselineStep = 5;
uint32_t baselineVer = 0;

struct Point { uint32_t seq; int16_t angle, dist, base; uint8_t flag; };
static const int NPTS = 256;
Point    pts[NPTS];
uint32_t ptSeq = 0;

struct Event { uint32_t seq, epoch, upt; int16_t angle, dist, base; };
static const int NEV = 64;
Event    evs[NEV];
uint32_t evSeq = 0;

struct RecentAlert { uint32_t t; int angle; };
RecentAlert recentAlert[8];
uint32_t lastPushAt = 0;
uint32_t alarmCount = 0;

// ------------------------------------------------------------------ servo (LEDC PWM)
void servoInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_SERVO, LEDC_FREQ, LEDC_BITS);
#else
  ledcSetup(LEDC_CH, LEDC_FREQ, LEDC_BITS);
  ledcAttachPin(PIN_SERVO, LEDC_CH);
#endif
}

void servoDuty(uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_SERVO, duty);
#else
  ledcWrite(LEDC_CH, duty);
#endif
}

void moveTo(int a, uint32_t extraSettle = 0) {
  a = constrain(a, SCAN_MIN, SCAN_MAX);
  int us = SERVO_MIN_US + (long)(SERVO_MAX_US - SERVO_MIN_US) * a / 180;
  // 50 Hz -> 20 000 us period; duty counts = us / 20000 * 2^bits
  servoDuty((uint32_t)((uint64_t)us * ((1u << LEDC_BITS) - 1) / 20000UL));
  // a big jump needs longer to arrive (SG90 ~0.1 s per 60°)
  uint32_t travel = (uint32_t)abs(a - servoAngle) * 2;
  servoAngle = a;
  servoOn = true;
  tMoved = millis() + travel + extraSettle;   // settle timer starts after travel
  pingCount = 0;
}

void servoRelease() { servoDuty(0); servoOn = false; }

// ------------------------------------------------------------------ ultrasonic
float pingOnce() {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, ECHO_TIMEOUT_US);
  if (us == 0) return -1;                         // no echo within range
  float c  = 331.3f + 0.606f * tempC;             // speed of sound, m/s
  float cm = us * c / 20000.0f;                   // round trip us -> one-way cm
  return (cm < MIN_CM || cm > MAX_CM) ? -1 : cm;
}

int neededPings() { return armed ? max(pingsPerAngle, 3) : pingsPerAngle; }

float medianValid(const float* v, int n) {
  float tmp[5]; int k = 0;
  for (int i = 0; i < n; i++) if (v[i] >= 0) tmp[k++] = v[i];
  if (!k) return -1;
  for (int i = 1; i < k; i++) {                   // insertion sort
    float x = tmp[i]; int j = i - 1;
    while (j >= 0 && tmp[j] > x) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = x;
  }
  return (k % 2) ? tmp[k / 2] : 0.5f * (tmp[k / 2 - 1] + tmp[k / 2]);
}

// "No echo" is treated as far away (MAX_CM) so an echo appearing where there
// used to be nothing still counts as an intrusion.
bool intrusive(int a, float d) {
  if (!learned || isnan(baseline[a])) return false;
  float dd = (d < 0) ? MAX_CM : d;
  float b = baseline[a];
  float margin = max((float)thresholdCm, 0.10f * b);
  return dd < b - margin;
}

// ------------------------------------------------------------------ logs
void pushPoint(int a, float d, uint8_t flag) {
  ptSeq++;
  Point& p = pts[ptSeq % NPTS];
  p.seq = ptSeq; p.angle = a; p.flag = flag;
  p.dist = (d < 0) ? -1 : (int16_t)lroundf(d);
  p.base = isnan(baseline[a]) ? -1 : (int16_t)lroundf(baseline[a]);
}

bool timeValid() { return time(nullptr) > 1600000000; }

struct PushMsg { char text[160]; };

void pushTask(void* arg) {
  PushMsg* m = (PushMsg*)arg;
  WiFiClientSecure client;
  client.setInsecure();                           // demo: no cert pinning
  HTTPClient https;
  if (https.begin(client, String("https://ntfy.sh/") + NTFY_TOPIC)) {
    https.addHeader("Title", "ESP32 radar: intruder detected");
    https.addHeader("Priority", "high");
    https.addHeader("Tags", "rotating_light");
    int code = https.POST((uint8_t*)m->text, strlen(m->text));
    Serial.printf("[push] ntfy.sh -> HTTP %d\n", code);
    https.end();
  }
  delete m;
  vTaskDelete(nullptr);
}

void raiseAlarm(int a, float d) {
  alarmOn = true;
  uint32_t now = millis();
  // One target usually spans several bearings: suppress repeats within ±15° of a
  // recent alert so one person = one event.
  for (int i = 0; i < 8; i++) {
    if (recentAlert[i].t && now - recentAlert[i].t < ALERT_COOLDOWN_MS &&
        abs(a - recentAlert[i].angle) <= ALERT_GROUP_DEG) {
      recentAlert[i].t = now ? now : 1;         // target still there: extend the window
      return;
    }
  }
  int slot = 0;                                  // reuse the oldest slot
  for (int i = 1; i < 8; i++) if (recentAlert[i].t < recentAlert[slot].t) slot = i;
  recentAlert[slot].t = now ? now : 1;
  recentAlert[slot].angle = a;

  alarmCount++;
  evSeq++;
  Event& e = evs[evSeq % NEV];
  e.seq = evSeq;
  e.epoch = timeValid() ? (uint32_t)time(nullptr) : 0;
  e.upt = now / 1000;
  e.angle = a;
  e.dist = (d < 0) ? -1 : (int16_t)lroundf(d);
  e.base = (int16_t)lroundf(baseline[a]);
  Serial.printf("[ALARM] %d deg  %.0f cm  (baseline %d cm)\n", a, d, e.base);

  if (strlen(NTFY_TOPIC) && !apMode && WiFi.isConnected() &&
      (lastPushAt == 0 || now - lastPushAt > PUSH_COOLDOWN_MS)) {
    lastPushAt = now;
    PushMsg* m = new PushMsg;
    snprintf(m->text, sizeof m->text, "Object at %d deg, %.2f m (room baseline %.2f m)",
             a, d / 100.0f, e.base / 100.0f);
    if (xTaskCreate(pushTask, "push", 10240, m, 1, nullptr) != pdPASS) delete m;
  }
}

// ------------------------------------------------------------------ scan logic
void startScan(uint32_t extra = 300) {
  mode = M_SCAN;
  sweepDir = 1;
  moveTo(SCAN_MIN, extra);
}

void startLearning() {
  armed = false; alarmOn = false;
  memset(learnN, 0, sizeof learnN);
  sweepsDone = 0;
  learning = true;
  baselineStep = stepDeg;
  startScan(500);
  Serial.println("[learn] learning room clutter map...");
}

void finishLearning() {
  for (int a = 0; a <= 180; a++) {
    baseline[a] = learnN[a] ? medianValid(learnVals[a], learnN[a]) : NAN;
  }
  learning = false;
  learned = true;
  baselineVer++;
  Serial.println("[learn] baseline stored");
}

void startArmed() {
  stepDeg = baselineStep;                         // visit the same bearings as the baseline
  learning = false;
  armed = true;
  alarmOn = false;
  memset(recentAlert, 0, sizeof recentAlert);
  startScan(300);
  Serial.println("[arm] armed");
}

void stopAll() {
  mode = M_IDLE;
  learning = false;
  armed = false;
}

void finishAngle() {
  int n = pingCount;
  float d = medianValid(pingRaw, n);
  lastDist = d;
  uint8_t flag = 0;

  if (learning) {
    if (learnN[servoAngle] < LEARN_SWEEPS)
      learnVals[servoAngle][learnN[servoAngle]++] = (d < 0) ? MAX_CM : d;
  } else if (learned) {
    int hits = 0;
    for (int i = 0; i < n; i++) if (intrusive(servoAngle, pingRaw[i])) hits++;
    if (armed && hits >= 2) { flag = 2; raiseAlarm(servoAngle, d); }  // confirmed: 2 of 3
    else if (hits >= 1) flag = 1;                                     // candidate
  }
  pushPoint(servoAngle, d, flag);
}

void advance() {
  int next = servoAngle + sweepDir * stepDeg;
  if (next > SCAN_MAX || next < SCAN_MIN) {       // end of a sweep: turn around
    sweepDir = -sweepDir;
    next = servoAngle + sweepDir * stepDeg;
    if (learning && ++sweepsDone >= LEARN_SWEEPS) finishLearning();
  }
  moveTo(next);
}

// ------------------------------------------------------------------ status LED
void updateLed() {
#ifdef RGB_BUILTIN
  static uint32_t last = 0xFFFFFFFF;
  uint8_t r = 0, g = 0, b = 0;
  bool blink = (millis() / 300) % 2;
  if (alarmOn)        { r = blink ? 40 : 0; }
  else if (armed)     { r = 12; g = 6; }
  else if (learning)  { b = blink ? 20 : 0; }
  else                { g = 6; }
  uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  if (c != last) { neopixelWrite(RGB_BUILTIN, r, g, b); last = c; }
#endif
}

// ------------------------------------------------------------------ Wi-Fi
int linkRssi() {
  if (!apMode) return WiFi.isConnected() ? WiFi.RSSI() : 0;
  wifi_sta_list_t list;
  if (esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num > 0) return list.sta[0].rssi;
  return 0;
}

void startWiFi() {
  WiFi.setHostname(HOSTNAME);
  if (strlen(WIFI_SSID)) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("[wifi] joining '%s'", WIFI_SSID);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < STA_TIMEOUT_MS) {
      delay(300); Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      apMode = false;
      WiFi.setAutoReconnect(true);
      configTime(0, 0, "pool.ntp.org", "time.google.com");   // real timestamps for the log
      Serial.printf("[wifi] connected: http://%s  (or http://%s.local)  RSSI %d dBm  ch %d\n",
                    WiFi.localIP().toString().c_str(), HOSTNAME, WiFi.RSSI(), WiFi.channel());
      return;
    }
    Serial.println("[wifi] could not join - starting own access point instead");
    WiFi.disconnect(true);
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  apMode = true;
  Serial.printf("[wifi] AP '%s' password '%s' -> http://%s\n",
                AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
}

// ------------------------------------------------------------------ HTTP API
const char* modeName() {
  if (learning) return "learn";
  return mode == M_SCAN ? "scan" : mode == M_AIM ? "aim" : "idle";
}

void appendStatus(String& o) {
  char b[640];
  IPAddress ip = apMode ? WiFi.softAPIP() : WiFi.localIP();
  snprintf(b, sizeof b,
    "{\"mode\":\"%s\",\"armed\":%d,\"alarm\":%d,\"learned\":%d,\"lp\":%u,\"ls\":%u,"
    "\"angle\":%d,\"servo\":%d,\"dist\":%.1f,\"step\":%d,\"settle\":%d,\"pings\":%d,"
    "\"thr\":%d,\"temp\":%.1f,\"bstep\":%d,\"bv\":%lu,\"wifi\":\"%s\",\"rssi\":%d,"
    "\"ip\":\"%s\",\"tv\":%d,\"up\":%lu,\"alarms\":%lu,\"push\":%d,\"ps\":%lu,\"es\":%lu}",
    modeName(), armed, alarmOn, learned, sweepsDone, LEARN_SWEEPS,
    servoAngle, servoOn, lastDist, stepDeg, settleMs, pingsPerAngle,
    thresholdCm, tempC, baselineStep, (unsigned long)baselineVer, apMode ? "ap" : "sta",
    linkRssi(), ip.toString().c_str(), timeValid(), (unsigned long)(millis() / 1000),
    (unsigned long)alarmCount, (int)(strlen(NTFY_TOPIC) > 0),
    (unsigned long)ptSeq, (unsigned long)evSeq);
  o += b;
}

uint32_t argU32(const char* name) {
  return http.hasArg(name) ? strtoul(http.arg(name).c_str(), nullptr, 10) : 0;
}

void handlePoll() {
  uint32_t ps = argU32("p"), es = argU32("e");
  String o;
  o.reserve(6000);
  o += "{\"s\":";
  appendStatus(o);

  o += ",\"p\":[";
  uint32_t first = (ptSeq >= NPTS) ? ptSeq - NPTS + 1 : 1;
  if (ps + 1 > first) first = ps + 1;
  char b[96];
  for (uint32_t s = first; s <= ptSeq; s++) {
    const Point& p = pts[s % NPTS];
    snprintf(b, sizeof b, "%s[%lu,%d,%d,%d,%u]", s == first ? "" : ",",
             (unsigned long)p.seq, p.angle, p.dist, p.base, p.flag);
    o += b;
  }

  o += "],\"e\":[";
  first = (evSeq >= NEV) ? evSeq - NEV + 1 : 1;
  if (es + 1 > first) first = es + 1;
  for (uint32_t s = first; s <= evSeq; s++) {
    const Event& e = evs[s % NEV];
    snprintf(b, sizeof b, "%s[%lu,%lu,%lu,%d,%d,%d]", s == first ? "" : ",",
             (unsigned long)e.seq, (unsigned long)e.epoch, (unsigned long)e.upt,
             e.angle, e.dist, e.base);
    o += b;
  }
  o += "]}";
  http.send(200, "application/json", o);
}

void handleBaseline() {
  String o;
  o.reserve(1200);
  o += "[";
  for (int a = 0; a <= 180; a++) {
    if (a) o += ",";
    if (isnan(baseline[a])) o += "null"; else o += String((int)lroundf(baseline[a]));
  }
  o += "]";
  http.send(200, "application/json", o);
}

void handleTime() {
  // the phone sends its clock so the log has real times even in AP mode (no internet)
  if (http.hasArg("ms")) {
    uint64_t ms = strtoull(http.arg("ms").c_str(), nullptr, 10);
    if (ms > 1600000000000ULL) {
      struct timeval tv;
      tv.tv_sec = ms / 1000;
      tv.tv_usec = (ms % 1000) * 1000;
      settimeofday(&tv, nullptr);
    }
  }
  http.send(200, "application/json", "{\"ok\":true}");
}

void reply(bool ok, const char* err = "") {
  String o = ok ? String("{\"ok\":true}") : String("{\"ok\":false,\"err\":\"") + err + "\"}";
  http.send(200, "application/json", o);
}

void handleCmd() {
  String c = http.arg("c");
  if (c == "scan")          { learning = false; armed = false; startScan(); }
  else if (c == "stop")     { stopAll(); }
  else if (c == "aim")      { stopAll(); mode = M_AIM; moveTo(http.arg("angle").toInt()); }
  else if (c == "learn")    { startLearning(); }
  else if (c == "arm")      { if (!learned) return reply(false, "Learn the room first"); startArmed(); }
  else if (c == "disarm")   { armed = false; alarmOn = false; }
  else if (c == "ack")      { alarmOn = false; }
  else if (c == "release")  { stopAll(); servoRelease(); }
  else if (c == "set") {
    if (http.hasArg("step")) {
      int s = constrain(http.arg("step").toInt(), 1, 20);
      if (s != stepDeg && (armed || learning))
        return reply(false, "Disarm / finish learning before changing the step");
      stepDeg = s;
    }
    if (http.hasArg("settle")) settleMs = constrain(http.arg("settle").toInt(), 20, 500);
    if (http.hasArg("pings"))  pingsPerAngle = constrain(http.arg("pings").toInt(), 1, 5);
    if (http.hasArg("thr"))    thresholdCm = constrain(http.arg("thr").toInt(), 5, 200);
    if (http.hasArg("temp"))   tempC = constrain(http.arg("temp").toFloat(), -10.0f, 50.0f);
  }
  else return reply(false, "unknown command");
  reply(true);
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n# ESP32-S3 ultrasonic radar");

  pinMode(PIN_TRIG, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_ECHO, INPUT);
  for (int a = 0; a <= 180; a++) baseline[a] = NAN;

  servoInit();
  moveTo(90);
  updateLed();

  startWiFi();
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  http.on("/",             HTTP_GET, [] { http.send_P(200, "text/html", INDEX_HTML); });
  http.on("/api/poll",     HTTP_GET, handlePoll);
  http.on("/api/baseline", HTTP_GET, handleBaseline);
  http.on("/api/cmd",      HTTP_GET, handleCmd);
  http.on("/api/time",     HTTP_GET, handleTime);
  http.onNotFound([] { http.send(404, "text/plain", "not found"); });
  http.begin();
  Serial.println("# web server up");
}

void loop() {
  http.handleClient();
  uint32_t now = millis();

  if (mode == M_SCAN) {
    if ((int32_t)(now - tMoved) >= settleMs && now - tLastPing >= PING_GAP_MS) {
      tLastPing = now;
      pingRaw[pingCount++] = pingOnce();
      if (pingCount >= neededPings()) { finishAngle(); advance(); }
    }
  } else if (mode == M_AIM) {
    if ((int32_t)(now - tMoved) >= settleMs && now - tLastPing >= 100) {
      tLastPing = now;
      float d = pingOnce();
      lastDist = d;
      pushPoint(servoAngle, d, intrusive(servoAngle, d) ? 1 : 0);
    }
  }
  updateLed();
}
