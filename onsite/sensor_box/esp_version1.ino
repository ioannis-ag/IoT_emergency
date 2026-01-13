#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>

#include "esp_bt.h"   // for esp_bt_controller_mem_release

//THIS VERSION IS A DEMO FOR GETTING THE ECG DATA STREAM FROM THE POLAR H10
//THIS VERSION DOES NOT UTILISE ESP-NOW FOR REDUNDANCY

// ---------------- WiFi ----------------
static const char* WIFI_SSID     = "VODAFONE_H268Q-4057";
static const char* WIFI_PASS     = "SFzDE5ZxHyQPQ2FQ";

// ---------------- MQTT ----------------
static const char* MQTT_HOST = "192.168.2.13";
static const int   MQTT_PORT = 1883;

static const char* TOPIC_JSON = "iot/esp32/telemetry";
static const char* TOPIC_ECG  = "iot/esp32/polar/ecg";

// ---------------- MQ-2 ----------------
static const int MQ2_APIN = 34;   // ADC1
static const int MQ2_DPIN = 16;   // digital

// ---------------- Polar BLE ----------------
static const char* POLAR_PREFIX = "Polar H10";

// Polar PMD UUIDs
static BLEUUID PMD_SERVICE("FB005C80-02E7-F387-1CAD-8ACD2D8DF0C8");
static BLEUUID PMD_CP     ("FB005C81-02E7-F387-1CAD-8ACD2D8DF0C8");
static BLEUUID PMD_DATA   ("FB005C82-02E7-F387-1CAD-8ACD2D8DF0C8");

// Standard Heart Rate Service/Characteristic
static BLEUUID HRS_SERVICE((uint16_t)0x180D);
static BLEUUID HRM_CHAR  ((uint16_t)0x2A37);

// ---------------- Timing ----------------
static const uint32_t JSON_INTERVAL_MS = 2000;
static const uint32_t ECG_FLUSH_MS     = 50;

static const uint32_t WIFI_RETRY_MS    = 2000;
static const uint32_t MQTT_RETRY_MS    = 2000;

static const uint32_t BLE_SCAN_SEC     = 5;

// ---------------- MQTT sizing ----------------
// Keep these modest to avoid pulling in extra overhead.
// PubSubClient buffer must be >= your biggest publish payload.
static const uint16_t MQTT_BUF_SIZE    = 512;
static const uint16_t ECG_BUNDLE_MAX   = 450; // keep < MQTT_BUF_SIZE

// ---------------- ECG queue ----------------
static const int ECG_QUEUE_LEN = 24;
static const int ECG_PKT_MAX   = 260; // enough for MTU=247 notifications

struct EcgPkt {
  uint16_t len;
  uint8_t  data[ECG_PKT_MAX];
};

static QueueHandle_t ecgQueue;

// ---------------- Globals ----------------
static WiFiClient espClient;
static PubSubClient mqtt(espClient);

static BLEAdvertisedDevice* polarDevice = nullptr;
static BLEClient* polarClient = nullptr;

static BLERemoteCharacteristic* pmdCpChr   = nullptr;
static BLERemoteCharacteristic* pmdDataChr = nullptr;

// Heart rate state
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint16_t latestBpm = 0;
static volatile bool gotHr = false;

// Stats
static volatile uint32_t ecgPacketCount = 0;
static volatile uint32_t ecgDropCount   = 0;

// CP capture (PMD control point)
static volatile bool gotCp = false;
static uint8_t cpBuf[128];
static size_t  cpLen = 0;

// Reconnect throttles
static uint32_t lastWifiTry = 0;
static uint32_t lastMqttTry = 0;

// Publish throttles
static uint32_t lastJsonPub  = 0;
static uint32_t lastEcgFlush = 0;

// ---------------- Helpers ----------------
static bool enableCCCD(BLERemoteCharacteristic* chr, uint16_t value) {
  BLERemoteDescriptor* cccd = chr->getDescriptor(BLEUUID((uint16_t)0x2902));
  if (!cccd) return false;
  uint8_t v[2];
  v[0] = (uint8_t)(value & 0xFF);
  v[1] = (uint8_t)((value >> 8) & 0xFF);
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

// ---------------- WiFi/MQTT ----------------
static void connectWiFiNonBlocking() {
  if (WiFi.status() == WL_CONNECTED) return;

  uint32_t now = millis();
  if ((now - lastWifiTry) < WIFI_RETRY_MS) return;
  lastWifiTry = now;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // helps coexistence
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static void connectMQTTNonBlocking() {
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t now = millis();
  if ((now - lastMqttTry) < MQTT_RETRY_MS) return;
  lastMqttTry = now;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  // Small, no-String clientId
  uint64_t mac = ESP.getEfuseMac();
  char clientId[32];
  // esp32-polar-XXXXXXXXXXXX
  snprintf(clientId, sizeof(clientId), "esp32-polar-%06lX%06lX",
           (unsigned long)(mac >> 24), (unsigned long)(mac & 0xFFFFFF));

  mqtt.connect(clientId);
}

// ---------------- BLE Callbacks ----------------
static void cpCb(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  size_t n = len;
  if (n > sizeof(cpBuf)) n = sizeof(cpBuf);
  memcpy(cpBuf, data, n);
  cpLen = n;
  gotCp = true;
}

static void hrCb(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len < 2) return;

  uint8_t flags = data[0];
  bool hr16 = (flags & 0x01) != 0;

  uint16_t bpm;
  if (!hr16) {
    bpm = data[1];
  } else {
    if (len < 3) return;
    bpm = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
  }

  portENTER_CRITICAL(&g_mux);
  latestBpm = bpm;
  gotHr = true;
  portEXIT_CRITICAL(&g_mux);
}

static void ecgCb(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len < 10) return;
  if (data[0] != 0x00) return; // ECG stream type

  ecgPacketCount++;

  EcgPkt pkt;
  uint16_t n = (len > (size_t)ECG_PKT_MAX) ? (uint16_t)ECG_PKT_MAX : (uint16_t)len;
  pkt.len = n;
  memcpy(pkt.data, data, n);

  // If full, drop oldest to keep “freshest”
  if (uxQueueSpacesAvailable(ecgQueue) == 0) {
    EcgPkt dump;
    if (xQueueReceive(ecgQueue, &dump, 0) == pdTRUE) {
      ecgDropCount++;
    }
  }
  xQueueSend(ecgQueue, &pkt, 0);
}

// ---------------- BLE Scan ----------------
class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (!dev.haveName()) return;
    String name = dev.getName();
    if (!name.startsWith(POLAR_PREFIX)) return;


    polarDevice = new BLEAdvertisedDevice(dev);
    BLEDevice::getScan()->stop();
  }
};

// ---------------- Polar connect + start ECG ----------------
static bool startPolarECG(BLEClient* client) {
  // Heart Rate notify
  BLERemoteService* hrs = client->getService(HRS_SERVICE);
  if (!hrs) return false;

  BLERemoteCharacteristic* hrm = hrs->getCharacteristic(HRM_CHAR);
  if (!hrm) return false;

  hrm->registerForNotify(hrCb);
  if (!enableCCCD(hrm, 0x0001)) return false;

  // PMD
  BLERemoteService* pmd = client->getService(PMD_SERVICE);
  if (!pmd) return false;

  pmdCpChr   = pmd->getCharacteristic(PMD_CP);
  pmdDataChr = pmd->getCharacteristic(PMD_DATA);
  if (!pmdCpChr || !pmdDataChr) return false;

  pmdCpChr->registerForNotify(cpCb);
  if (!enableCCCD(pmdCpChr, 0x0002)) return false;  // indications for CP

  pmdDataChr->registerForNotify(ecgCb);
  if (!enableCCCD(pmdDataChr, 0x0001)) return false; // notifications for data

  // Get settings
  gotCp = false;
  uint8_t getSettings[2] = { 0x01, 0x00 };
  pmdCpChr->writeValue(getSettings, sizeof(getSettings), true);
  if (!waitCp(3000)) return false;

  // Validate response
  if (cpLen < 5) return false;
  if (cpBuf[0] != 0xF0 || cpBuf[1] != 0x01) return false;
  if (cpBuf[3] != 0x00) return false; // error

  // Build START (copy payload from index 5)
  uint8_t startCmd[64];
  size_t startLen = 0;
  startCmd[startLen++] = 0x02; // START
  startCmd[startLen++] = 0x00; // ECG

  for (size_t i = 5; i < cpLen && startLen < sizeof(startCmd); i++) {
    startCmd[startLen++] = cpBuf[i];
  }

  gotCp = false;
  pmdCpChr->writeValue(startCmd, startLen, true);
  if (!waitCp(3000)) return false;

  // START response check: F0 02 ... err
  if (cpLen >= 4 && cpBuf[0] == 0xF0 && cpBuf[1] == 0x02 && cpBuf[3] != 0x00) {
    return false;
  }

  return true;
}

static bool connectPolarOnce() {
  polarDevice = nullptr;

  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), true);
  scan->setActiveScan(true);
  scan->start(BLE_SCAN_SEC, false);

  if (!polarDevice) return false;

  polarClient = BLEDevice::createClient();
  if (!polarClient->connect(polarDevice)) return false;

  polarClient->setMTU(247);
  return startPolarECG(polarClient);
}

// ---------------- Publishing ----------------
static void publishTelemetryJson() {
  if (!mqtt.connected()) return;

  int mq2_raw = analogRead(MQ2_APIN);
  int mq2_dig = digitalRead(MQ2_DPIN);

  uint16_t bpm;
  bool hrOk;
  portENTER_CRITICAL(&g_mux);
  bpm = latestBpm;
  hrOk = gotHr;
  portEXIT_CRITICAL(&g_mux);

  // Keep JSON tiny
  // {"mq2":1234,"mq2d":0,"bpm":72,"hrok":1,"rssi":-55,"ecg":12345,"drop":12}
  char payload[160];
  snprintf(payload, sizeof(payload),
           "{\"mq2\":%d,\"mq2d\":%d,\"bpm\":%u,\"hrok\":%d,\"rssi\":%d,\"ecg\":%lu,\"drop\":%lu}",
           mq2_raw, mq2_dig,
           (unsigned)bpm, hrOk ? 1 : 0,
           WiFi.RSSI(),
           (unsigned long)ecgPacketCount,
           (unsigned long)ecgDropCount);

  mqtt.publish(TOPIC_JSON, payload);
}

static void flushEcgBundle() {
  if (!mqtt.connected()) return;

  static bool hasStash = false;
  static EcgPkt stash;

  uint8_t out[ECG_BUNDLE_MAX];
  size_t outLen = 0;

  // Header: "ECG1" + uptime(u32 LE) + count(u8)
  out[outLen++] = 'E'; out[outLen++] = 'C'; out[outLen++] = 'G'; out[outLen++] = '1';

  uint32_t t = (uint32_t)millis();
  out[outLen++] = (uint8_t)(t & 0xFF);
  out[outLen++] = (uint8_t)((t >> 8) & 0xFF);
  out[outLen++] = (uint8_t)((t >> 16) & 0xFF);
  out[outLen++] = (uint8_t)((t >> 24) & 0xFF);

  size_t countPos = outLen;
  out[outLen++] = 0; // placeholder
  uint8_t count = 0;

  // First, if we have a stashed packet from last time, try to include it
  if (hasStash) {
    size_t need = 2 + stash.len;
    if (outLen + need <= sizeof(out)) {
      out[outLen++] = (uint8_t)(stash.len & 0xFF);
      out[outLen++] = (uint8_t)((stash.len >> 8) & 0xFF);
      memcpy(&out[outLen], stash.data, stash.len);
      outLen += stash.len;
      count++;
      hasStash = false;
    } else {
      // can't even fit stash -> give up for now
      return;
    }
  }

  // Pull from queue until full
  while (count < 255) {
    EcgPkt pkt;
    if (xQueueReceive(ecgQueue, &pkt, 0) != pdTRUE) break;

    size_t need = 2 + pkt.len;
    if (outLen + need > sizeof(out)) {
      // keep this packet for next flush
      stash = pkt;
      hasStash = true;
      break;
    }

    out[outLen++] = (uint8_t)(pkt.len & 0xFF);
    out[outLen++] = (uint8_t)((pkt.len >> 8) & 0xFF);
    memcpy(&out[outLen], pkt.data, pkt.len);
    outLen += pkt.len;
    count++;

    // optional CPU cap
    if (outLen > (ECG_BUNDLE_MAX - 64)) break;
  }

  if (count == 0) return;
  out[countPos] = count;

  mqtt.publish(TOPIC_ECG, out, outLen);
}

// ---------------- Arduino setup/loop ----------------
void setup() {
  // No Serial

  pinMode(MQ2_DPIN, INPUT);
  analogReadResolution(12);

  ecgQueue = xQueueCreate(ECG_QUEUE_LEN, sizeof(EcgPkt));

  // MQTT buffer size
  mqtt.setBufferSize(MQTT_BUF_SIZE);

  // WiFi start
  connectWiFiNonBlocking();

  // BLE init
  BLEDevice::init("");
  BLEDevice::setPower(ESP_PWR_LVL_P9);

  // Release BT Classic memory (BLE only)
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  // Try to connect polar once at boot (non-fatal if not found)
  connectPolarOnce();
}

void loop() {
  // Keep connectivity
  connectWiFiNonBlocking();
  connectMQTTNonBlocking();
  mqtt.loop();

  uint32_t now = millis();

  // JSON telemetry
  if (mqtt.connected() && (now - lastJsonPub >= JSON_INTERVAL_MS)) {
    lastJsonPub = now;
    publishTelemetryJson();
  }

  // ECG bundles
  if (mqtt.connected() && (now - lastEcgFlush >= ECG_FLUSH_MS)) {
    lastEcgFlush = now;
    flushEcgBundle();
  }

  // Light yield (don’t block BLE)
  delay(2);
}
