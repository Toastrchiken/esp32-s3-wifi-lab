/*
 * Project 1 — WiFi scanner + RSSI logger
 * Board:     ESP32-S3-DevKitC-1 (plug into the "COM"/UART USB port)
 * Framework: Arduino-ESP32 core 2.0.x (official PlatformIO) or 3.x (Arduino IDE)
 * Serial:    115200 baud, line ending = Newline
 *
 * Two modes:
 *   ALL   – sweeps channels 1–13 every few seconds, lists every AP it hears
 *           (channel, RSSI, BSSID, security, SSID) + per-channel occupancy.
 *   TRACK – locks onto ONE access point (one BSSID) and samples its RSSI on
 *           its channel only, ~5x per second, with running mean/min/max.
 *           Use with `m <distance>` marks for the path-loss experiment.
 *
 * Commands (type into Serial Monitor):
 *   a            all-networks mode (default)
 *   t <ssid>     track the strongest AP broadcasting <ssid>
 *   m <label>    drop a marker (e.g. "m 2" = standing 2 m away); resets stats
 *   r            reset tracking stats
 *   i <ms>       set scan interval for the current mode
 *   f csv|table  output format (wifilog.py switches this to csv itself)
 *   p            pause / resume
 *   h            help
 *
 * Machine-readable lines (csv format), consumed by wifilog.py:
 *   SCAN,<scan_id>,<esp_ms>,<n_aps>,<scan_ms>        (ALL mode only)
 *   AP,<scan_id>,<esp_ms>,<ch>,<rssi>,<bssid>,<auth>,<ssid>
 *   MARK,<esp_ms>,<label>                             (always printed)
 * Everything human-readable starts with '#'.  SSID is the last field because
 * SSIDs may contain commas.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

// ---------------- configuration ----------------
static const char*    COUNTRY        = "AU";   // regulatory domain -> channels 1–13
static const bool     SHOW_HIDDEN    = true;   // include hidden-SSID APs
static const bool     PASSIVE_SCAN   = false;  // false = send probe requests (faster)
static const uint32_t MS_PER_CHANNEL = 120;    // dwell time per channel
static const uint8_t  MAX_MISSES     = 3;      // track mode: misses before re-search
static const size_t   AVG_WINDOW     = 10;     // moving-average length (samples)
static const int      MAX_APS        = 64;

// ---------------- state ----------------
enum Mode { MODE_ALL, MODE_TRACK };
Mode     mode        = MODE_ALL;
bool     paused      = false;
bool     csvMode     = false;
uint32_t allInterval   = 3000;   // ms between full sweeps
uint32_t trackInterval = 0;      // ms between track samples (0 = as fast as possible)
uint32_t lastScanMs  = 0;
uint32_t scanId      = 0;

String   targetSsid;
uint8_t  targetBssid[6];
uint8_t  targetChannel = 0;      // 0 = not located yet
uint8_t  misses        = 0;

// running stats for TRACK mode
int32_t  ring[AVG_WINDOW];
size_t   ringCount = 0, ringIdx = 0;
int64_t  statSum = 0;
uint32_t statN = 0;
int32_t  statMin = 0, statMax = 0;

String   rxLine;

// ---------------- helpers ----------------
const char* authName(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "OTHER";
  }
}

String macToStr(const uint8_t* m) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(buf);
}

// Bar graph: -100 dBm = empty, -30 dBm = full (35 chars)
String rssiBar(int32_t rssi) {
  int len = constrain((rssi + 100) / 2, 0, 35);
  String s;
  for (int i = 0; i < len; i++) s += '#';
  return s;
}

void resetStats() {
  ringCount = ringIdx = 0;
  statSum = 0; statN = 0;
  statMin = 0; statMax = 0;
}

void addSample(int32_t rssi) {
  ring[ringIdx] = rssi;
  ringIdx = (ringIdx + 1) % AVG_WINDOW;
  if (ringCount < AVG_WINDOW) ringCount++;
  if (statN == 0) { statMin = statMax = rssi; }
  statMin = min(statMin, rssi);
  statMax = max(statMax, rssi);
  statSum += rssi;
  statN++;
}

float movingAvg() {
  if (!ringCount) return NAN;
  int32_t s = 0;
  for (size_t i = 0; i < ringCount; i++) s += ring[i];
  return (float)s / ringCount;
}

// Blocking scan. channel 0 = all channels; ssid/bssid nullptr = no filter.
int16_t scan(uint8_t channel, const char* ssid, const uint8_t* bssid) {
  return WiFi.scanNetworks(false, SHOW_HIDDEN, PASSIVE_SCAN, MS_PER_CHANNEL,
                           channel, ssid, bssid);
}

// ---------------- ALL mode ----------------
void runFullScan() {
  uint32_t t0 = millis();
  int16_t n = scan(0, nullptr, nullptr);
  uint32_t scanMs = millis() - t0;
  scanId++;

  if (n < 0) { Serial.printf("# scan failed (%d)\n", n); return; }
  if (n > MAX_APS) n = MAX_APS;

  // sort indices strongest-first (insertion sort; n is small)
  int16_t idx[MAX_APS];
  for (int i = 0; i < n; i++) idx[i] = i;
  for (int i = 1; i < n; i++) {
    int16_t k = idx[i]; int j = i - 1;
    while (j >= 0 && WiFi.RSSI(idx[j]) < WiFi.RSSI(k)) { idx[j + 1] = idx[j]; j--; }
    idx[j + 1] = k;
  }

  uint8_t perCh[15] = {0};

  if (csvMode) {
    Serial.printf("SCAN,%lu,%lu,%d,%lu\n", (unsigned long)scanId,
                  (unsigned long)t0, n, (unsigned long)scanMs);
  } else {
    Serial.printf("#\n# --- scan %lu | %d APs | %lu ms ---\n",
                  (unsigned long)scanId, n, (unsigned long)scanMs);
    Serial.println("#  CH  RSSI  AUTH       BSSID              SSID");
  }

  for (int k = 0; k < n; k++) {
    int i = idx[k];
    uint8_t ch    = WiFi.channel(i);
    int32_t rssi  = WiFi.RSSI(i);
    String  ssid  = WiFi.SSID(i);
    String  bssid = WiFi.BSSIDstr(i);
    const char* auth = authName(WiFi.encryptionType(i));
    if (ch < 15) perCh[ch]++;

    if (csvMode) {
      Serial.printf("AP,%lu,%lu,%u,%ld,%s,%s,%s\n", (unsigned long)scanId,
                    (unsigned long)t0, ch, (long)rssi, bssid.c_str(), auth,
                    ssid.c_str());
    } else {
      Serial.printf("#  %2u  %4ld  %-9s  %s  %s\n", ch, (long)rssi, auth,
                    bssid.c_str(), ssid.length() ? ssid.c_str() : "<hidden>");
    }
  }

  if (!csvMode) {
    Serial.print("# APs per channel:");
    for (int c = 1; c <= 13; c++) Serial.printf("  %d:%u", c, perCh[c]);
    Serial.println();
  }
  WiFi.scanDelete();
}

// ---------------- TRACK mode ----------------
bool locateTarget() {
  int16_t n = scan(0, targetSsid.c_str(), nullptr);
  int best = -1;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) != targetSsid) continue;
    if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
  }
  if (best < 0) {
    Serial.printf("# '%s' not heard on any channel, retrying...\n", targetSsid.c_str());
    WiFi.scanDelete();
    return false;
  }
  targetChannel = WiFi.channel(best);
  memcpy(targetBssid, WiFi.BSSID(best), 6);
  Serial.printf("# locked on '%s'  BSSID %s  ch %u  (%d APs share this SSID)\n",
                targetSsid.c_str(), macToStr(targetBssid).c_str(), targetChannel, n);
  WiFi.scanDelete();
  misses = 0;
  return true;
}

void runTrackScan() {
  if (targetChannel == 0 && !locateTarget()) return;

  uint32_t t0 = millis();
  int16_t n = scan(targetChannel, targetSsid.c_str(), targetBssid);
  scanId++;

  int hit = -1;
  for (int i = 0; i < n; i++) {
    if (memcmp(WiFi.BSSID(i), targetBssid, 6) == 0) { hit = i; break; }
  }

  if (hit < 0) {
    misses++;
    Serial.printf("# miss %u/%u\n", misses, MAX_MISSES);
    if (misses >= MAX_MISSES) {
      Serial.println("# lost AP (moved channel or out of range) - re-searching all channels");
      targetChannel = 0;
    }
    WiFi.scanDelete();
    return;
  }
  misses = 0;

  int32_t rssi = WiFi.RSSI(hit);
  addSample(rssi);

  if (csvMode) {
    Serial.printf("AP,%lu,%lu,%u,%ld,%s,%s,%s\n", (unsigned long)scanId,
                  (unsigned long)t0, targetChannel, (long)rssi,
                  macToStr(targetBssid).c_str(),
                  authName(WiFi.encryptionType(hit)), targetSsid.c_str());
  } else {
    Serial.printf("# %4ld dBm  avg%u %6.1f  min %4ld  max %4ld  n=%-5lu |%s\n",
                  (long)rssi, (unsigned)AVG_WINDOW, movingAvg(), (long)statMin,
                  (long)statMax, (unsigned long)statN, rssiBar(rssi).c_str());
  }
  WiFi.scanDelete();
}

// ---------------- commands ----------------
void printHelp() {
  Serial.println("# commands: a | t <ssid> | m <label> | r | i <ms> | f csv|table | p | h");
}

void handleCommand(String line) {
  line.trim();
  if (!line.length()) return;
  char c = line[0];
  String arg = line.length() > 1 ? line.substring(1) : String();
  arg.trim();

  switch (c) {
    case 'a':
      mode = MODE_ALL;
      Serial.println("# mode: ALL");
      break;
    case 't':
      if (!arg.length()) { Serial.println("# usage: t <ssid>"); break; }
      targetSsid = arg;
      targetChannel = 0;
      misses = 0;
      resetStats();
      mode = MODE_TRACK;
      Serial.printf("# mode: TRACK '%s'\n", targetSsid.c_str());
      break;
    case 'm':
      Serial.printf("MARK,%lu,%s\n", (unsigned long)millis(), arg.c_str());
      if (!csvMode) Serial.printf("# ---- mark '%s' (stats reset) ----\n", arg.c_str());
      resetStats();
      break;
    case 'r':
      resetStats();
      Serial.println("# stats reset");
      break;
    case 'i': {
      long v = constrain(arg.toInt(), 0L, 60000L);
      if (mode == MODE_ALL) allInterval = v; else trackInterval = v;
      Serial.printf("# interval: %ld ms\n", v);
      break;
    }
    case 'f':
      if (arg == "csv") csvMode = true;
      else if (arg == "table") csvMode = false;
      else csvMode = !csvMode;
      Serial.printf("# format: %s\n", csvMode ? "csv" : "table");
      break;
    case 'p':
      paused = !paused;
      Serial.println(paused ? "# paused" : "# resumed");
      break;
    case 'h':
    default:
      printHelp();
      break;
  }
}

void pollSerial() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (rxLine.length()) { handleCommand(rxLine); rxLine = ""; }
    } else if (rxLine.length() < 80) {
      rxLine += ch;
    }
  }
}

// ---------------- Arduino entry points ----------------
void setup() {
  Serial.begin(115200);
  delay(500);

  WiFi.mode(WIFI_STA);     // station mode, but we never associate
  WiFi.disconnect();
  delay(100);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_err_t err = esp_wifi_set_country_code(COUNTRY, true);
#else  // Arduino core 2.x (ESP-IDF 4.4): older API
  wifi_country_t cc = {};
  strlcpy(cc.cc, COUNTRY, sizeof(cc.cc));
  cc.schan = 1;
  cc.nchan = 13;            // AU: channels 1-13
  cc.max_tx_power = 20;     // ignored on set; use esp_wifi_set_max_tx_power()
  cc.policy = WIFI_COUNTRY_POLICY_MANUAL;
  esp_err_t err = esp_wifi_set_country(&cc);
#endif
  Serial.printf("# ESP32-S3 WiFi scanner | MAC %s | country %s (%s)\n",
                WiFi.macAddress().c_str(), COUNTRY, esp_err_to_name(err));
  printHelp();
}

void loop() {
  pollSerial();
  if (paused) return;

  uint32_t interval = (mode == MODE_ALL) ? allInterval : trackInterval;
  if (millis() - lastScanMs >= interval) {
    lastScanMs = millis();
    if (mode == MODE_ALL) runFullScan();
    else runTrackScan();
  }
}
