#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include "esp_bt.h"

#include <DHT.h>
#include <time.h>
#include <math.h>

// ============================================================
// CONFIG (Firefighter A, Team A) — Minimal-change topic scheme
// ============================================================

// Physical node label (used for failover/origin semantics)
static constexpr char NODE_ID_CHAR = 'A';
static const char*   NODE_ID       = "A";       // physical node label

// Logical identity
static const char*   TEAM_ID       = "Team_A";
static const char*   FIREFIGHTER_ID= "FF_A";    // <-- changed from FF_03 to FF_A

// Wi-Fi failover list (primary functionality)
struct WifiCred { const char* ssid; const char* pass; };
static WifiCred WIFI_LIST[] = {
  { "VODAFONE_H268Q-4057", "SFzDE5ZxHyQPQ2FQ" },
  { "FirefighterA",        "firefighterA" },
};
static constexpr int WIFI_LIST_N = sizeof(WIFI_LIST) / sizeof(WIFI_LIST[0]);

// MQTT
static const char* MQTT_HOST = "192.168.2.13";
static const int   MQTT_PORT = 1883;

// Topics (Minimal-change option you chose)
static const char* TOPIC_ENV  = "ngsi/Environment/Team_A/FF_A";
static const char* TOPIC_BIO  = "ngsi/Biomedical/Team_A/FF_A";
static const char* TOPIC_GW   = "ngsi/Gateway/A";
static const char* TOPIC_ECG  = "raw/ECG/Team_A/FF_A";   // recommended over raw/ECG/A

// Sensors
static const int MQ2_APIN = 34;     // ADC1
static const int MQ2_DPIN = 16;

static const int DHT_PIN  = 14;
static const int DHT_TYPE = DHT22;

// Timing
static const uint32_t PUB_INTERVAL_MS = 2000;
static const uint32_t ECG_FLUSH_MS    = 50;
static const uint32_t MQTT_RETRY_MS   = 2000;

// WiFi attempt logic (event-driven + SSID rotation)
static const uint32_t WIFI_ATTEMPT_WINDOW_MS = 15000;

// Polar BLE
static const char* POLAR_PREFIX = "Polar H10";

// Polar PMD UUIDs
static BLEUUID PMD_SERVICE("FB005C80-02E7-F387-1CAD-8ACD2D8DF0C8");
static BLEUUID PMD_CP     ("FB005C81-02E7-F387-1CAD-8ACD2D8DF0C8");
static BLEUUID PMD_DATA   ("FB005C82-02E7-F387-1CAD-8ACD2D8DF0C8");

// Standard Heart Rate Service/Characteristic
static BLEUUID HRS_SERVICE((uint16_t)0x180D);
static BLEUUID HRM_CHAR  ((uint16_t)0x2A37);

static const uint32_t BLE_SCAN_SEC = 5;
static const uint32_t BLE_RETRY_MS = 8000;

// MQTT sizing / ECG bundling
static const uint16_t MQTT_BUF_SIZE  = 1024;
static const uint16_t ECG_BUNDLE_MAX = 700;

static const int ECG_QUEUE_LEN = 24;
static const int ECG_PKT_MAX   = 260;

// ============================================================
// DATA STRUCTURES
// ============================================================

struct EcgPkt {
  uint16_t len;
  uint8_t  data[ECG_PKT_MAX];
};
static QueueHandle_t ecgQueue;

// ============================================================
// GLOBALS
// ============================================================

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);
static DHT dht(DHT_PIN, DHT_TYPE);

static uint32_t lastMqttTry = 0;
static uint32_t lastPub = 0;
static uint32_t lastEcgFlush = 0;

// ---- WiFi state machine (event-driven, stops the spam) ----
static volatile bool wifiGotIp = false;
static volatile bool wifiConnecting = false;

static int wifiIndex = -1;
static uint32_t wifiAttemptStart = 0;

// ---- NTP ----
static bool timeInited = false;
static uint32_t lastTimeTry = 0;

// ---- BLE state ----
static BLEAdvertisedDevice* polarDevice = nullptr;
static BLEClient* polarClient = nullptr;

static BLERemoteCharacteristic* hrmChr = nullptr;
static BLERemoteCharacteristic* pmdCpChr = nullptr;
static BLERemoteCharacteristic* pmdDataChr = nullptr;

static volatile bool bleConnected = false;
static bool ecgEnabled = false;
static uint32_t lastBleTry = 0;

// HR + RR
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint16_t latestBpm = 0;
static volatile bool gotHr = false;

static volatile uint16_t latestRrMs = 0;
static volatile bool gotRr = false;

// RR ring buffer
static const int RR_BUF_N = 64;
static uint16_t rrBuf[RR_BUF_N];
static int rrHead = 0;
static int rrCount = 0;

// PMD CP capture
static volatile bool gotCp = false;
static uint8_t cpBuf[128];
static size_t  cpLen = 0;

// ECG stats
static volatile uint32_t ecgPacketCount = 0;
static volatile uint32_t ecgDropCount   = 0;

// ============================================================
// HELPERS
// ============================================================

static inline int16_t wifiRssiDbmOrNeg127() {
  return (WiFi.status() == WL_CONNECTED) ? (int16_t)WiFi.RSSI() : (int16_t)-127;
}

static uint16_t readMq2Adc() { return (uint16_t)analogRead(MQ2_APIN); }
static uint8_t  readMq2Dig() { return (uint8_t)(digitalRead(MQ2_DPIN) ? 1 : 0); }

static bool readDht(float& tempC, float& humPct) {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) return false;
  tempC = t; humPct = h;
  return true;
}

static void ensureTimeUtc() {
  if (timeInited) return;
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t now = millis();
  if (now - lastTimeTry < 10000) return;
  lastTimeTry = now;

  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  time_t t = time(nullptr);
  if (t > 1700000000) {
    timeInited = true;
    Serial.println("[TIME] NTP OK");
  }
}

static void iso8601UtcNow(char* out, size_t outSz) {
  time_t t = time(nullptr);
  if (t <= 100000) { snprintf(out, outSz, "1970-01-01T00:00:00Z"); return; }
  struct tm tm{};
  gmtime_r(&t, &tm);
  strftime(out, outSz, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static bool enableCCCD(BLERemoteCharacteristic* chr, uint16_t value) {
  BLERemoteDescriptor* cccd = chr->getDescriptor(BLEUUID((uint16_t)0x2902));
  if (!cccd) return false;
  uint8_t v[2] = { (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF) };
  return cccd->writeValue(v, 2, true);
}

static bool waitCp(uint32_t ms) {
  uint32_t t0 = millis();
  while ((millis() - t0) < ms) {
    if (gotCp) return true;
    delay(5);
  }
  return false;
}

static void computeHrv(uint16_t& out_rmssd, uint16_t& out_sdnn) {
  out_rmssd = 0xFFFF;
  out_sdnn  = 0xFFFF;

  portENTER_CRITICAL(&g_mux);
  int n = rrCount;
  uint16_t tmp[RR_BUF_N];
  for (int i = 0; i < n; i++) {
    int idx = (rrHead - n + i);
    while (idx < 0) idx += RR_BUF_N;
    tmp[i] = rrBuf[idx % RR_BUF_N];
  }
  portEXIT_CRITICAL(&g_mux);

  if (n < 5) return;

  double mean = 0.0;
  for (int i = 0; i < n; i++) mean += tmp[i];
  mean /= n;

  double var = 0.0;
  for (int i = 0; i < n; i++) {
    double d = tmp[i] - mean;
    var += d * d;
  }
  var /= (n - 1);
  double sdnn = sqrt(var);

  double acc = 0.0;
  int m = 0;
  for (int i = 1; i < n; i++) {
    double d = (double)tmp[i] - (double)tmp[i - 1];
    acc += d * d;
    m++;
  }
  double rmssd = (m > 0) ? sqrt(acc / m) : 0.0;

  out_sdnn  = (sdnn  > 65534.0) ? 65534 : (uint16_t)lround(sdnn);
  out_rmssd = (rmssd > 65534.0) ? 65534 : (uint16_t)lround(rmssd);
}

// ============================================================
// WIFI (event-driven) — avoids begin() spam
// ============================================================

static void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      wifiConnecting = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiGotIp = true;
      wifiConnecting = false;
      Serial.print("[WiFi] GOT IP: ");
      Serial.println(WiFi.localIP());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiGotIp = false;
      wifiConnecting = false;
      break;
    default:
      break;
  }
}

static void startNextWifiAttempt() {
  wifiIndex = (wifiIndex + 1) % WIFI_LIST_N;
  wifiAttemptStart = millis();

  Serial.printf("[WiFi] trying ssid=%s\n", WIFI_LIST[wifiIndex].ssid);

  WiFi.disconnect(true);
  delay(50);

  WiFi.begin(WIFI_LIST[wifiIndex].ssid, WIFI_LIST[wifiIndex].pass);
}

static void ensureWiFi() {
  if (wifiGotIp) return;

  // If currently connecting and still within attempt window, wait.
  if (wifiConnecting && (millis() - wifiAttemptStart) < WIFI_ATTEMPT_WINDOW_MS) return;

  // If no attempt yet OR attempt window expired, rotate SSID.
  if (wifiAttemptStart == 0 || (millis() - wifiAttemptStart) >= WIFI_ATTEMPT_WINDOW_MS) {
    startNextWifiAttempt();
  }
}

// ============================================================
// MQTT
// ============================================================

static void ensureMQTT() {
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t now = millis();
  if (now - lastMqttTry < MQTT_RETRY_MS) return;
  lastMqttTry = now;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  char cid[48];
  uint64_t mac = ESP.getEfuseMac();
  snprintf(cid, sizeof(cid), "ff-%s-%s-node%c-%06lX%06lX",
           TEAM_ID, FIREFIGHTER_ID, NODE_ID_CHAR,
           (unsigned long)(mac >> 24), (unsigned long)(mac & 0xFFFFFF));

  bool ok = mqtt.connect(cid);
  Serial.printf("[MQTT] connect %s state=%d\n", ok ? "OK" : "FAIL", mqtt.state());
}

// ============================================================
// BLE (HR always; ECG only when uplink OK)
// ============================================================

static void cpCb(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  size_t n = min(len, sizeof(cpBuf));
  memcpy(cpBuf, data, n);
  cpLen = n;
  gotCp = true;
}

static void hrCb(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len < 2) return;

  uint8_t flags = data[0];
  bool hr16 = (flags & 0x01) != 0;
  bool rrPresent = (flags & 0x10) != 0;

  uint16_t bpm = 0;
  int idx = 1;

  if (!hr16) bpm = data[idx++];
  else {
    if (len < 3) return;
    bpm = (uint16_t)data[idx] | ((uint16_t)data[idx + 1] << 8);
    idx += 2;
  }

  portENTER_CRITICAL(&g_mux);
  latestBpm = bpm;
  gotHr = true;
  portEXIT_CRITICAL(&g_mux);

  if (rrPresent) {
    bool first = true;
    while (idx + 1 < (int)len) {
      uint16_t rr_1024 = (uint16_t)data[idx] | ((uint16_t)data[idx + 1] << 8);
      idx += 2;
      uint16_t rr_ms = (uint16_t)lround((rr_1024 * 1000.0f) / 1024.0f);

      portENTER_CRITICAL(&g_mux);
      rrBuf[rrHead] = rr_ms;
      rrHead = (rrHead + 1) % RR_BUF_N;
      if (rrCount < RR_BUF_N) rrCount++;
      if (first) { latestRrMs = rr_ms; gotRr = true; first = false; }
      portEXIT_CRITICAL(&g_mux);
    }
  }
}

static void ecgCb(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (!ecgEnabled) return;
  if (len < 10) return;
  if (data[0] != 0x00) return;

  ecgPacketCount++;

  EcgPkt pkt;
  uint16_t n = (len > (size_t)ECG_PKT_MAX) ? (uint16_t)ECG_PKT_MAX : (uint16_t)len;
  pkt.len = n;
  memcpy(pkt.data, data, n);

  if (uxQueueSpacesAvailable(ecgQueue) == 0) {
    EcgPkt dump;
    if (xQueueReceive(ecgQueue, &dump, 0) == pdTRUE) ecgDropCount++;
  }
  xQueueSend(ecgQueue, &pkt, 0);
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (!dev.haveName()) return;
    String name = dev.getName();
    if (!name.startsWith(POLAR_PREFIX)) return;
    polarDevice = new BLEAdvertisedDevice(dev);
    BLEDevice::getScan()->stop();
  }
};

static bool connectPolarHR_and_PMD() {
  polarDevice = nullptr;

  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), true);
  scan->setActiveScan(true);
  scan->start(BLE_SCAN_SEC, false);

  if (!polarDevice) { Serial.println("[BLE] scan: no Polar"); return false; }

  polarClient = BLEDevice::createClient();
  if (!polarClient->connect(polarDevice)) { Serial.println("[BLE] connect fail"); return false; }
  polarClient->setMTU(247);

  BLERemoteService* hrs = polarClient->getService(HRS_SERVICE);
  if (!hrs) return false;

  hrmChr = hrs->getCharacteristic(HRM_CHAR);
  if (!hrmChr) return false;

  hrmChr->registerForNotify(hrCb);
  if (!enableCCCD(hrmChr, 0x0001)) return false;

  BLERemoteService* pmd = polarClient->getService(PMD_SERVICE);
  if (!pmd) { Serial.println("[BLE] PMD missing (HR OK)"); return true; }

  pmdCpChr   = pmd->getCharacteristic(PMD_CP);
  pmdDataChr = pmd->getCharacteristic(PMD_DATA);

  if (pmdCpChr) { pmdCpChr->registerForNotify(cpCb); enableCCCD(pmdCpChr, 0x0002); }
  if (pmdDataChr) { pmdDataChr->registerForNotify(ecgCb); enableCCCD(pmdDataChr, 0x0001); }

  Serial.println("[BLE] HR ready (+PMD)");
  return true;
}

static bool polarStartECG() {
  if (!pmdCpChr) return false;

  gotCp = false;
  uint8_t getSettings[2] = { 0x01, 0x00 };
  pmdCpChr->writeValue(getSettings, sizeof(getSettings), true);
  if (!waitCp(3000)) return false;

  if (cpLen < 5) return false;
  if (cpBuf[0] != 0xF0 || cpBuf[1] != 0x01) return false;
  if (cpBuf[3] != 0x00) return false;

  uint8_t startCmd[64];
  size_t startLen = 0;
  startCmd[startLen++] = 0x02;
  startCmd[startLen++] = 0x00;

  for (size_t i = 5; i < cpLen && startLen < sizeof(startCmd); i++) startCmd[startLen++] = cpBuf[i];

  gotCp = false;
  pmdCpChr->writeValue(startCmd, startLen, true);
  if (!waitCp(3000)) return false;

  if (cpLen >= 4 && cpBuf[0] == 0xF0 && cpBuf[1] == 0x02 && cpBuf[3] != 0x00) return false;
  return true;
}

static void polarStopECG() {
  if (!pmdCpChr) return;
  uint8_t stopCmd[2] = { 0x03, 0x00 };
  pmdCpChr->writeValue(stopCmd, sizeof(stopCmd), true);
}

static void bleTick() {
  if (bleConnected && polarClient && !polarClient->isConnected()) {
    Serial.println("[BLE] disconnected");
    bleConnected = false;
    gotHr = false;
    gotRr = false;
    ecgEnabled = false;
  }
  if (bleConnected) return;

  uint32_t now = millis();
  if (now - lastBleTry < BLE_RETRY_MS) return;
  lastBleTry = now;

  Serial.println("[BLE] trying connect...");
  bleConnected = connectPolarHR_and_PMD();
}

static void setEcgEnabled(bool on) {
  if (!bleConnected) { ecgEnabled = false; return; }
  if (on == ecgEnabled) return;

  if (on) {
    bool ok = polarStartECG();
    ecgEnabled = ok;
    Serial.printf("[ECG] START %s\n", ok ? "OK" : "FAIL");
  } else {
    polarStopECG();
    ecgEnabled = false;
    Serial.println("[ECG] STOP");
  }
}

// ============================================================
// MQTT publishers (no ArduinoJson; small payloads)
//   - Minimal-change topic scheme
//   - Consistent naming + failover fields
// ============================================================

static void publishEnvBioGw() {
  if (!mqtt.connected()) return;

  ensureTimeUtc();
  char ts[32];
  iso8601UtcNow(ts, sizeof(ts));

  // Read sensors
  uint16_t mq2_adc = readMq2Adc();
  uint8_t  mq2_dig = readMq2Dig();

  float tempC = NAN, humPct = NAN;
  bool dhtOk = readDht(tempC, humPct);

  // Read HR/RR
  uint16_t bpm; bool hrOk;
  uint16_t rr;  bool rrOk;
  portENTER_CRITICAL(&g_mux);
  bpm = latestBpm;   hrOk = gotHr;
  rr  = latestRrMs;  rrOk = gotRr;
  portEXIT_CRITICAL(&g_mux);

  // Format numbers without heap churn
  char tbuf[16], hbuf[16], bpmbuf[8], rrbuf[8];
  if (dhtOk) { dtostrf(tempC, 0, 2, tbuf); dtostrf(humPct, 0, 2, hbuf); }
  else { strcpy(tbuf, "null"); strcpy(hbuf, "null"); }

  if (hrOk) { snprintf(bpmbuf, sizeof(bpmbuf), "%u", (unsigned)bpm); } else { strcpy(bpmbuf, "null"); }
  if (rrOk) { snprintf(rrbuf,  sizeof(rrbuf),  "%u", (unsigned)rr); }  else { strcpy(rrbuf,  "null"); }

  // ---------- ENVIRONMENT ----------
  // Note: wifiRssiDbm is clearer than rssi (later you may add BLE RSSI etc.)
  char env[520];
  snprintf(env, sizeof(env),
    "{"
      "\"teamId\":\"%s\",\"ffId\":\"%s\",\"nodeId\":\"%s\","
      "\"originNodeId\":\"%s\",\"via\":\"self\",\"failover\":false,\"forwardHopCount\":0,"
      "\"observedAt\":\"%s\","
      "\"tempC\":%s,\"humidityPct\":%s,"
      "\"mq2Raw\":%u,\"mq2Digital\":%u,"
      "\"wifiRssiDbm\":%d,"
      "\"source\":\"esp32\""
    "}",
    TEAM_ID, FIREFIGHTER_ID, NODE_ID,
    NODE_ID, ts,
    tbuf, hbuf,
    (unsigned)mq2_adc, (unsigned)mq2_dig,
    (int)wifiRssiDbmOrNeg127()
  );
  mqtt.publish(TOPIC_ENV, env);

  // ---------- BIOMEDICAL ----------
  // rrMs (simpler than rrMsLatest)
  char bio[420];
  snprintf(bio, sizeof(bio),
    "{"
      "\"teamId\":\"%s\",\"ffId\":\"%s\","
      "\"originNodeId\":\"%s\",\"via\":\"self\",\"failover\":false,\"forwardHopCount\":0,"
      "\"observedAt\":\"%s\","
      "\"hrBpm\":%s,\"rrMs\":%s,"
      "\"wearableOk\":%s,"
      "\"source\":\"ble\""
    "}",
    TEAM_ID, FIREFIGHTER_ID,
    NODE_ID, ts,
    bpmbuf, rrbuf,
    (bleConnected && hrOk) ? "true" : "false"
  );
  mqtt.publish(TOPIC_BIO, bio);

  // ---------- GATEWAY ----------
  bool uplink_ok = (WiFi.status() == WL_CONNECTED && mqtt.connected());
  char gw[320];
  snprintf(gw, sizeof(gw),
    "{"
      "\"nodeId\":\"%s\",\"observedAt\":\"%s\","
      "\"uplinkReal\":%s,\"uplinkEffective\":%s,"
      "\"wifiRssiDbm\":%d,"
      "\"bleOk\":%s,\"ecgOn\":%s,"
      "\"ecgPkts\":%lu,\"ecgDrop\":%lu"
    "}",
    NODE_ID, ts,
    uplink_ok ? "true" : "false",
    uplink_ok ? "true" : "false",
    (int)wifiRssiDbmOrNeg127(),
    bleConnected ? "true" : "false",
    ecgEnabled ? "true" : "false",
    (unsigned long)ecgPacketCount,
    (unsigned long)ecgDropCount
  );
  mqtt.publish(TOPIC_GW, gw);
}

// ECG bundling
static void flushEcgBundle() {
  if (!mqtt.connected()) return;
  if (!ecgEnabled) return;

  static bool hasStash = false;
  static EcgPkt stash;

  uint8_t out[ECG_BUNDLE_MAX];
  size_t outLen = 0;

  out[outLen++] = 'E'; out[outLen++] = 'C'; out[outLen++] = 'G'; out[outLen++] = '1';

  uint32_t t = (uint32_t)millis();
  out[outLen++] = (uint8_t)(t & 0xFF);
  out[outLen++] = (uint8_t)((t >> 8) & 0xFF);
  out[outLen++] = (uint8_t)((t >> 16) & 0xFF);
  out[outLen++] = (uint8_t)((t >> 24) & 0xFF);

  size_t countPos = outLen;
  out[outLen++] = 0;
  uint8_t count = 0;

  if (hasStash) {
    size_t need = 2 + stash.len;
    if (outLen + need <= sizeof(out)) {
      out[outLen++] = (uint8_t)(stash.len & 0xFF);
      out[outLen++] = (uint8_t)((stash.len >> 8) & 0xFF);
      memcpy(&out[outLen], stash.data, stash.len);
      outLen += stash.len;
      count++;
      hasStash = false;
    } else return;
  }

  while (count < 255) {
    EcgPkt pkt;
    if (xQueueReceive(ecgQueue, &pkt, 0) != pdTRUE) break;

    size_t need = 2 + pkt.len;
    if (outLen + need > sizeof(out)) { stash = pkt; hasStash = true; break; }

    out[outLen++] = (uint8_t)(pkt.len & 0xFF);
    out[outLen++] = (uint8_t)((pkt.len >> 8) & 0xFF);
    memcpy(&out[outLen], pkt.data, pkt.len);
    outLen += pkt.len;
    count++;

    if (outLen > (ECG_BUNDLE_MAX - 64)) break;
  }

  if (count == 0) return;
  out[countPos] = count;

  mqtt.publish(TOPIC_ECG, out, outLen);
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== FIREFIGHTER A | ESP32 | MQTT | BLE HR+ECG + MQ2 + DHT ===");

  pinMode(MQ2_DPIN, INPUT);
  pinMode(MQ2_APIN, INPUT);

  analogReadResolution(12);
  analogSetPinAttenuation(MQ2_APIN, ADC_11db);

  dht.begin();
  ecgQueue = xQueueCreate(ECG_QUEUE_LEN, sizeof(EcgPkt));

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.onEvent(onWiFiEvent);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(MQTT_BUF_SIZE);

  BLEDevice::init("");
  BLEDevice::setPower(ESP_PWR_LVL_P6);
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  startNextWifiAttempt();
}

void loop() {
  ensureWiFi();
  ensureMQTT();
  mqtt.loop();

  bleTick();

  bool uplink_ok = (WiFi.status() == WL_CONNECTED && mqtt.connected());

  // ECG only when uplink OK
  setEcgEnabled(uplink_ok);

  uint32_t now = millis();

  if (uplink_ok && (now - lastPub >= PUB_INTERVAL_MS)) {
    lastPub = now;
    publishEnvBioGw();
  }

  if (uplink_ok && (now - lastEcgFlush >= ECG_FLUSH_MS)) {
    lastEcgFlush = now;
    flushEcgBundle();
  }

  delay(2);
}
