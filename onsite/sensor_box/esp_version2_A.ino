#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include "esp_bt.h"

#include <ArduinoJson.h>
#include <DHT.h>
#include <time.h>

// ============================================================
// CONFIG (EDIT THESE)
// ============================================================

static constexpr char NODE_ID_CHAR = 'A';
static const char*   NODE_ID_TEXT = "A";
static const char*   TEAM_ID      = "Team_A";
static const char*   FF_ID        = "FF_03";

// self-published semantics
static const char* VIA_TEXT = "self";
static const bool  FAILOVER_BOOL = false;
static const int   FORWARD_HOPS  = 0;

// Wi-Fi failover list
struct WifiCred { const char* ssid; const char* pass; };
static WifiCred WIFI_LIST[] = {
  { "VODAFONE_H268Q-4057", "SFzDE5ZxHyQPQ2FQ" },
  { "FirefighterA",        "firefighterA" },
};
static constexpr int WIFI_LIST_N = sizeof(WIFI_LIST) / sizeof(WIFI_LIST[0]);

// MQTT
static const char* MQTT_HOST = "192.168.2.13";
static const int   MQTT_PORT = 1883;

// Topics map to entity identity (edge controller will translate to Orion)
static const char* TOPIC_ENV  = "ngsi/EnvNode/Team_A/FF_03";
static const char* TOPIC_WEAR = "ngsi/Wearable/Team_A/FF_03";
static const char* TOPIC_GW   = "ngsi/Gateway/A";

// ECG binary stream
static const char* TOPIC_ECG  = "ff/A/polar/ecg";

// ESP-NOW peer (Node B STA MAC)
static const uint8_t ESP_B_MAC[6] = { 0x78, 0x1C, 0x3C, 0xF5, 0x97, 0x70 };

// Sensors (ESP32 classic)
static const int MQ2_APIN = 34;     // ADC (input-only ok)
static const int MQ2_DPIN = 16;

static const int DHT_PIN  = 14;
static const int DHT_TYPE = DHT22;

// Timing
static const uint32_t PUB_INTERVAL_MS  = 2000;
static const uint32_t ECG_FLUSH_MS     = 50;
static const uint32_t CAPSULE_MS       = 2000;

static const uint32_t WIFI_RETRY_MS    = 2000;
static const uint32_t MQTT_RETRY_MS    = 2000;

static const unsigned long FAILOVER_AFTER_MS = 8000;
static const unsigned long RECOVER_AFTER_MS  = 8000;

// Polar BLE
static const char* POLAR_PREFIX = "Polar H10";

static BLEUUID PMD_SERVICE("FB005C80-02E7-F387-1CAD-8ACD2D8DF0C8");
static BLEUUID PMD_CP     ("FB005C81-02E7-F387-1CAD-8ACD2D8DF0C8");
static BLEUUID PMD_DATA   ("FB005C82-02E7-F387-1CAD-8ACD2D8DF0C8");

static BLEUUID HRS_SERVICE((uint16_t)0x180D);
static BLEUUID HRM_CHAR  ((uint16_t)0x2A37);

static const uint32_t BLE_SCAN_SEC   = 3;
static const uint32_t BLE_RETRY_MS   = 8000;

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

enum MsgType : uint8_t { MSG_CAPSULE = 10 };

#pragma pack(push, 1)
struct CapsuleMsg {
  uint8_t  type;
  char     src_id;
  uint32_t seq;
  uint32_t ms;

  uint16_t bpm;
  uint16_t rmssd_ms;
  uint16_t sdnn_ms;

  uint16_t mq2_adc;
  uint8_t  mq2_dig;

  int16_t  temp_c_x100;
  int8_t   rssi;

  uint8_t  uplink_ok;
  uint8_t  ecg_on;
  uint8_t  ble_ok;
};
#pragma pack(pop)

// ============================================================
// GLOBALS
// ============================================================

static WiFiClient wifi;
static PubSubClient mqtt(wifi);
static DHT dht(DHT_PIN, DHT_TYPE);

static bool uplink_ok_real = false;
static bool uplink_ok_effective = false;

static unsigned long downSince = 0;
static unsigned long upSince   = 0;
static bool degraded_mode      = false;

static uint32_t lastWifiTry = 0;
static int wifiIndex = -1;

static uint32_t lastMqttTry = 0;

static uint32_t lastPub = 0;
static uint32_t lastEcgFlush = 0;
static uint32_t lastCapsule = 0;

static uint32_t seq = 0;

// BLE state
static BLEAdvertisedDevice* polarDevice = nullptr;
static BLEClient* polarClient = nullptr;

static BLERemoteCharacteristic* hrmChr = nullptr;
static BLERemoteCharacteristic* pmdCpChr = nullptr;
static BLERemoteCharacteristic* pmdDataChr = nullptr;

static volatile bool bleConnected = false;
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
static bool ecgEnabled = false;

// NTP
static bool timeInited = false;
static uint32_t lastTimeTry = 0;

// ESP-NOW peer channel sync
static int lastPeerChannel = -1;

// ============================================================
// HELPERS
// ============================================================

static inline int8_t wifiRssiOrNeg127() {
  return (WiFi.status() == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : (int8_t)-127;
}

static uint16_t readMq2Adc() {
  return (uint16_t)analogRead(MQ2_APIN);
}

static uint8_t readMq2Dig() {
  return (uint8_t)(digitalRead(MQ2_DPIN) ? 1 : 0);
}

static bool readDht(float& tempC, float& humPct) {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) return false;
  tempC = t;
  humPct = h;
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
// ESP-NOW
// ============================================================

static void setupEspNow() {
  esp_err_t e = esp_now_init();
  Serial.printf("[ESP-NOW] init %s (%d)\n", (e == ESP_OK) ? "OK" : "FAIL", (int)e);
}

static void ensurePeerChannel() {
  if (WiFi.status() != WL_CONNECTED) return;
  int ch = WiFi.channel();
  if (ch == lastPeerChannel) return;

  esp_now_del_peer(ESP_B_MAC);

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, ESP_B_MAC, 6);
  peer.channel = ch;
  peer.encrypt = false;

  esp_err_t p = esp_now_add_peer(&peer);
  Serial.printf("[ESP-NOW] peer(B) channel=%d add %s (%d)\n", ch, (p == ESP_OK) ? "OK" : "FAIL", (int)p);
  lastPeerChannel = ch;
}

static void sendCapsule() {
  CapsuleMsg c{};
  c.type = MSG_CAPSULE;
  c.src_id = NODE_ID_CHAR;
  c.seq = ++seq;
  c.ms = millis();

  uint16_t bpm; bool hrOk;
  portENTER_CRITICAL(&g_mux);
  bpm = latestBpm; hrOk = gotHr;
  portEXIT_CRITICAL(&g_mux);
  c.bpm = hrOk ? bpm : 0;

  uint16_t rmssd, sdnn;
  computeHrv(rmssd, sdnn);
  c.rmssd_ms = rmssd;
  c.sdnn_ms  = sdnn;

  c.mq2_adc = readMq2Adc();
  c.mq2_dig = readMq2Dig();

  float tC, hPct;
  bool dhtOk = readDht(tC, hPct);
  if (dhtOk) {
    int v = (int)lroundf(tC * 100.0f);
    c.temp_c_x100 = (int16_t)constrain(v, -32768, 32767);
  } else {
    c.temp_c_x100 = (int16_t)0x7FFF;
  }

  c.rssi = wifiRssiOrNeg127();
  c.uplink_ok = uplink_ok_real ? 1 : 0;
  c.ecg_on    = ecgEnabled ? 1 : 0;
  c.ble_ok    = bleConnected ? 1 : 0;

  esp_err_t e = esp_now_send(ESP_B_MAC, (uint8_t*)&c, sizeof(c));
  Serial.printf("[CAPSULE] send->B %s (%d)\n", (e == ESP_OK) ? "OK" : "FAIL", (int)e);
}

// ============================================================
// WIFI / MQTT (NON-BLOCKING + SSID FAILOVER)
// ============================================================

static void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  uint32_t now = millis();
  if (now - lastWifiTry < WIFI_RETRY_MS) return;
  lastWifiTry = now;

  wifiIndex = (wifiIndex + 1) % WIFI_LIST_N;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_LIST[wifiIndex].ssid, WIFI_LIST[wifiIndex].pass);

  Serial.printf("[WiFi] begin ssid=%s\n", WIFI_LIST[wifiIndex].ssid);
}

static void ensureMQTT() {
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t now = millis();
  if (now - lastMqttTry < MQTT_RETRY_MS) return;
  lastMqttTry = now;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  char cid[48];
  uint64_t mac = ESP.getEfuseMac();
  snprintf(cid, sizeof(cid), "node%c-%06lX%06lX",
           NODE_ID_CHAR, (unsigned long)(mac >> 24), (unsigned long)(mac & 0xFFFFFF));

  bool ok = mqtt.connect(cid);
  Serial.printf("[MQTT] connect %s state=%d\n", ok ? "OK" : "FAIL", mqtt.state());
}

// ============================================================
// BLE (HR always; ECG gated)
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
// MQTT publishers (simple, flat JSON; topics map to entity identity)
// ============================================================

static void publishEnvWearGw() {
  if (!mqtt.connected()) return;

  ensureTimeUtc();

  char ts[32];
  iso8601UtcNow(ts, sizeof(ts));

  uint16_t mq2_adc = readMq2Adc();
  uint8_t  mq2_dig = readMq2Dig();

  float tempC = NAN, humPct = NAN;
  bool dhtOk = readDht(tempC, humPct);

  uint16_t bpm; bool hrOk;
  uint16_t rr;  bool rrOk;
  portENTER_CRITICAL(&g_mux);
  bpm = latestBpm; hrOk = gotHr;
  rr  = latestRrMs; rrOk = gotRr;
  portEXIT_CRITICAL(&g_mux);

  // ENV
  StaticJsonDocument<384> env;
  env["teamId"] = TEAM_ID;
  env["ffId"] = FF_ID;
  env["nodeId"] = NODE_ID_TEXT;
  env["originNodeId"] = NODE_ID_TEXT;
  env["via"] = VIA_TEXT;
  env["failover"] = FAILOVER_BOOL;
  env["forwardHopCount"] = FORWARD_HOPS;
  env["observedAt"] = ts;
  env["tempC"] = dhtOk ? tempC : nullptr;
  env["humidityPct"] = dhtOk ? humPct : nullptr;
  env["mq2Raw"] = mq2_adc;
  env["mq2Digital"] = (int)mq2_dig;
  env["rssi"] = (int)wifiRssiOrNeg127();
  env["source"] = "esp32";

  char outEnv[384];
  size_t nEnv = serializeJson(env, outEnv, sizeof(outEnv));
  mqtt.publish(TOPIC_ENV, outEnv, nEnv);

  // WEAR
  StaticJsonDocument<320> wear;
  wear["teamId"] = TEAM_ID;
  wear["ffId"] = FF_ID;
  wear["originNodeId"] = NODE_ID_TEXT;
  wear["via"] = VIA_TEXT;
  wear["failover"] = FAILOVER_BOOL;
  wear["forwardHopCount"] = FORWARD_HOPS;
  wear["observedAt"] = ts;
  wear["hrBpm"] = hrOk ? (int)bpm : nullptr;
  wear["rrMsLatest"] = rrOk ? (int)rr : nullptr;
  wear["wearableOk"] = (bool)(bleConnected && hrOk);
  wear["source"] = "ble";

  char outWear[320];
  size_t nWear = serializeJson(wear, outWear, sizeof(outWear));
  mqtt.publish(TOPIC_WEAR, outWear, nWear);

  // GW
  StaticJsonDocument<256> gw;
  gw["nodeId"] = NODE_ID_TEXT;
  gw["observedAt"] = ts;
  gw["uplinkReal"] = (bool)uplink_ok_real;
  gw["uplinkEffective"] = (bool)uplink_ok_effective;
  gw["wifiRssi"] = (int)wifiRssiOrNeg127();
  gw["bleOk"] = (bool)bleConnected;
  gw["ecgOn"] = (bool)ecgEnabled;
  gw["degraded"] = (bool)degraded_mode;

  char outGw[256];
  size_t nGw = serializeJson(gw, outGw, sizeof(outGw));
  mqtt.publish(TOPIC_GW, outGw, nGw);
}

// ECG bundling stays unchanged
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
  Serial.println("\n=== NODE A (ESP32) | BLE HR+ECG + MQTT + ESP-NOW capsule ===");

  pinMode(MQ2_DPIN, INPUT);
  pinMode(MQ2_APIN, INPUT);

  // Better MQ2 dynamic range on classic ESP32
  analogReadResolution(12);
  analogSetPinAttenuation(MQ2_APIN, ADC_11db);

  dht.begin();

  ecgQueue = xQueueCreate(ECG_QUEUE_LEN, sizeof(EcgPkt));

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(MQTT_BUF_SIZE);

  setupEspNow();

  BLEDevice::init("");
  BLEDevice::setPower(ESP_PWR_LVL_P6);
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  ensureWiFi();
}

void loop() {
  ensureWiFi();
  ensureMQTT();
  mqtt.loop();

  ensureTimeUtc();
  ensurePeerChannel();
  bleTick();

  uplink_ok_real = (WiFi.status() == WL_CONNECTED && mqtt.connected());
  uplink_ok_effective = uplink_ok_real;

  unsigned long now = millis();

  if (!uplink_ok_effective) {
    if (downSince == 0) downSince = now;
    upSince = 0;
  } else {
    if (upSince == 0) upSince = now;
    downSince = 0;
  }

  if (!degraded_mode) {
    if (downSince && (now - downSince) >= FAILOVER_AFTER_MS) {
      degraded_mode = true;
      Serial.println("[MODE] >>> DEGRADED (capsule only, stop ECG)");
      setEcgEnabled(false);
    }
  } else {
    if (upSince && (now - upSince) >= RECOVER_AFTER_MS) {
      degraded_mode = false;
      Serial.println("[MODE] <<< RECOVERED (uplink stable, ECG allowed)");
    }
  }

  bool wantEcg = (uplink_ok_effective && !degraded_mode);
  setEcgEnabled(wantEcg);

  if (uplink_ok_effective && (now - lastPub >= PUB_INTERVAL_MS)) {
    lastPub = now;
    publishEnvWearGw();
  }

  if (uplink_ok_effective && (now - lastEcgFlush >= ECG_FLUSH_MS)) {
    lastEcgFlush = now;
    flushEcgBundle();
  }

  if (!uplink_ok_effective && (now - lastCapsule >= CAPSULE_MS)) {
    lastCapsule = now;
    sendCapsule();
  }

  delay(2);
}
