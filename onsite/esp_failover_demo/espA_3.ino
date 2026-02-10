#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_now.h>
#include <esp_wifi.h>

static const char* WIFI_SSID = "FirefighterA";
static const char* WIFI_PASS = "firefighterA";

static const char* MQTT_HOST = "141.237.94.127";
static const int   MQTT_PORT = 1883;

static const char* TOPIC_ENV_A = "ngsi/Environment/Team_A/FF_A";
static const char* TOPIC_BIO_A = "ngsi/Biomedical/Team_A/FF_A";
static const char* TOPIC_GW_A  = "ngsi/Gateway/A";

static uint8_t ESP_B_MAC[6] = { 0x78, 0x1C, 0x3C, 0xF5, 0x97, 0x70 }; // <-- CHANGE

// Demo goal: fixed channel for ESP-NOW
static const uint8_t FIXED_CHANNEL = 6;

// Hold time (increase as you like)
static const uint32_t FAILOVER_HOLD_MS = 120000; // 2 minutes

static const uint32_t PUB_INTERVAL_MS = 2000;
static const uint32_t STATUS_MS       = 2000;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ---------- ESPNOW protocol ----------
static const uint32_t ESPNOW_MAGIC = 0xA0B0C0D0;

enum MsgType : uint8_t { MSG_DATA = 1, MSG_ACK = 2 };
enum TopicKind : uint8_t { TK_ENV = 1, TK_BIO = 2, TK_GW = 3 };

#pragma pack(push, 1)
struct NowMsg {
  uint32_t magic;
  uint8_t  type;      // MSG_DATA or MSG_ACK
  uint8_t  kind;      // TK_*
  uint16_t _pad;
  uint32_t seq;
  char payload[200];  // JSON (DATA only)
};
#pragma pack(pop)

static volatile uint32_t g_lastAckSeq = 0;
static uint32_t g_seq = 0;

static uint32_t g_holdUntil = 0;
static bool g_inHold = false;
static uint8_t g_lastGoodChannel = 0;

static int getRadioChannel() {
  uint8_t primary = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&primary, &second) == ESP_OK) return (int)primary;
  return -1;
}

static void lockRadioChannel(uint8_t ch) {
  if (ch == 0) return;
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

static void enterFailoverHold() {
  g_inHold = true;
  g_holdUntil = millis() + FAILOVER_HOLD_MS;

  // IMPORTANT: stop WiFi state machine so it can't scan/hop channels
  WiFi.disconnect(true, true);
  delay(50);

  // Lock to fixed channel for ESP-NOW demo
  lockRadioChannel(FIXED_CHANNEL);

  Serial.printf("[A] FAILOVER HOLD START: WiFi stopped, channel locked to %u for %lus\n",
                FIXED_CHANNEL, (unsigned long)(FAILOVER_HOLD_MS / 1000));
}

static void exitFailoverHold() {
  g_inHold = false;
  g_holdUntil = 0;
  Serial.println("[A] FAILOVER HOLD END: resuming WiFi connect");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static void onWiFiEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    g_lastGoodChannel = (uint8_t)WiFi.channel();
    Serial.printf("[A] GOT_IP ip=%s channel=%u\n", WiFi.localIP().toString().c_str(), g_lastGoodChannel);
    // Once we have WiFi again, we can end hold (if any)
    if (g_inHold) exitFailoverHold();
  }
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("[A] DISCONNECTED (reason unknown) radioCh=%d\n", getRadioChannel());
    // Only enter hold if we previously were connected (channel known)
    // For demo, we always enter hold when disconnected.
    if (!g_inHold) enterFailoverHold();
  }
}

// IDF v5 signature
static void onEspNowSent(const wifi_tx_info_t*, esp_now_send_status_t status) {
  Serial.printf("[A][ESPNOW] tx=%s\n", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// IDF v5 signature
static void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != (int)sizeof(NowMsg)) return;
  NowMsg m{};
  memcpy(&m, data, sizeof(m));
  if (m.magic != ESPNOW_MAGIC) return;

  if (m.type == MSG_ACK) {
    g_lastAckSeq = m.seq;
    Serial.printf("[A][ESPNOW] ACK from %02X:%02X:%02X:%02X:%02X:%02X seq=%lu\n",
                  info->src_addr[0], info->src_addr[1], info->src_addr[2],
                  info->src_addr[3], info->src_addr[4], info->src_addr[5],
                  (unsigned long)m.seq);
  }
}

static bool initEspNow() {
  // Must be in STA mode
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  // Lock channel before init for stability
  lockRadioChannel(FIXED_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[A][ESPNOW] init FAIL");
    return false;
  }

  esp_now_register_send_cb(onEspNowSent);
  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, ESP_B_MAC, 6);
  peer.channel = FIXED_CHANNEL;  // FIXED!
  peer.encrypt = false;

  if (!esp_now_is_peer_exist(ESP_B_MAC)) {
    if (esp_now_add_peer(&peer) != ESP_OK) {
      Serial.println("[A][ESPNOW] add_peer FAIL");
      return false;
    }
  }

  Serial.printf("[A][ESPNOW] ready (fixed ch=%u)\n", FIXED_CHANNEL);
  return true;
}

static void mqttEnsure() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;

  String cid = String("ESP_A_") + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(cid.c_str())) {
    Serial.println("[A] MQTT connected");
  } else {
    Serial.printf("[A] MQTT failed rc=%d\n", mqtt.state());
  }
}

static void nowSendJson(TopicKind kind, const String& json) {
  NowMsg m{};
  m.magic = ESPNOW_MAGIC;
  m.type = MSG_DATA;
  m.kind = (uint8_t)kind;
  m.seq = ++g_seq;
  strlcpy(m.payload, json.c_str(), sizeof(m.payload));

  // Stay locked
  lockRadioChannel(FIXED_CHANNEL);

  esp_err_t e = esp_now_send(ESP_B_MAC, (const uint8_t*)&m, sizeof(m));
  if (e != ESP_OK) Serial.printf("[A][ESPNOW] send err=%d\n", (int)e);
}

static String isoNowUtcLite() {
  uint32_t ms = millis();
  char buf[32];
  snprintf(buf, sizeof(buf), "t%lu", (unsigned long)(ms / 1000));
  return String(buf);
}

static void publishTelemetry() {
  int hr = 80 + (int)((millis() / 1000) % 5);
  int rr = 60000 / hr;

  bool mqttOk = (WiFi.status() == WL_CONNECTED && mqtt.connected());
  bool failover = !mqttOk;

  String t = isoNowUtcLite();
  String bio = String("{\"teamId\":\"Team_A\",\"ffId\":\"FF_A\",\"originNodeId\":\"A\",")
    + "\"via\":\"" + String(failover ? "espnow" : "self") + "\","
    + "\"failover\":" + String(failover ? "true" : "false") + ","
    + "\"forwardHopCount\":0,"
    + "\"observedAt\":\"" + t + "\","
    + "\"hrBpm\":" + String(hr) + ",\"rrMs\":" + String(rr) + ",\"wearableOk\":true,\"source\":\"demo\"}";

  String env = String("{\"teamId\":\"Team_A\",\"ffId\":\"FF_A\",\"nodeId\":\"A\",\"originNodeId\":\"A\",")
    + "\"via\":\"" + String(failover ? "espnow" : "self") + "\","
    + "\"failover\":" + String(failover ? "true" : "false") + ","
    + "\"forwardHopCount\":0,"
    + "\"observedAt\":\"" + t + "\","
    + "\"mq2Raw\":1234,\"source\":\"demo\"}";

  String gw = String("{\"nodeId\":\"A\",\"observedAt\":\"") + t
    + "\",\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false")
    + ",\"radioChannel\":" + String(getRadioChannel())
    + ",\"fixedChannel\":" + String(FIXED_CHANNEL)
    + ",\"holdActive\":" + String(g_inHold ? "true" : "false")
    + ",\"mqttConnected\":" + String(mqtt.connected() ? "true" : "false")
    + ",\"lastAckSeq\":" + String((unsigned long)g_lastAckSeq) + "}";

  if (mqttOk) {
    mqtt.publish(TOPIC_BIO_A, bio.c_str());
    mqtt.publish(TOPIC_ENV_A, env.c_str());
    mqtt.publish(TOPIC_GW_A,  gw.c_str());
  } else {
    nowSendJson(TK_BIO, bio);
    nowSendJson(TK_ENV, env);
    nowSendJson(TK_GW,  gw);
  }
}

static uint32_t lastPub = 0;
static uint32_t lastStatus = 0;

void setup() {
  Serial.begin(115200);
  delay(150);

  Serial.println("\n=== ESP_A boot ===");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.onEvent(onWiFiEvent);

  Serial.printf("[A] MAC (STA) = %s\n", WiFi.macAddress().c_str());

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);

  initEspNow();

  // Start WiFi initially (normal mode)
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.println("[A] Ready.");
}

void loop() {
  // Hold window management
  if (g_inHold) {
    lockRadioChannel(FIXED_CHANNEL);
    if (millis() > g_holdUntil) exitFailoverHold();
  } else {
    mqttEnsure();
    mqtt.loop();
  }

  uint32_t now = millis();
  if (now - lastStatus >= STATUS_MS) {
    lastStatus = now;
    Serial.printf("[A] WiFi=%d SSID=%s ip=%s radioCh=%d fixed=%u hold=%s MQTT=%s lastAck=%lu\n",
                  (int)WiFi.status(),
                  (WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "-"),
                  (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-"),
                  getRadioChannel(),
                  FIXED_CHANNEL,
                  (g_inHold ? "YES" : "NO"),
                  (mqtt.connected() ? "UP" : "DOWN"),
                  (unsigned long)g_lastAckSeq);
  }

  if (now - lastPub >= PUB_INTERVAL_MS) {
    lastPub = now;
    publishTelemetry();
  }

  delay(2);
}
