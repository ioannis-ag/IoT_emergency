#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_now.h>
#include <esp_wifi.h>

static const char* WIFI_SSID = "FirefighterB";
static const char* WIFI_PASS = "firefighterB";

static const char* MQTT_HOST = "141.237.94.127";
static const int   MQTT_PORT = 1883;

// B publishes its own telemetry too
static const char* TOPIC_ENV_B = "ngsi/Environment/Team_A/FF_B";
static const char* TOPIC_BIO_B = "ngsi/Biomedical/Team_A/FF_B";
static const char* TOPIC_GW_B  = "ngsi/Gateway/B";

// A forwarded topics
static const char* TOPIC_ENV_A = "ngsi/Environment/Team_A/FF_A";
static const char* TOPIC_BIO_A = "ngsi/Biomedical/Team_A/FF_A";
static const char* TOPIC_GW_A  = "ngsi/Gateway/A";

static uint8_t ESP_A_MAC[6] = { 0x78, 0x1C, 0x3C, 0xF5, 0xC5, 0xDC }; // <-- CHANGE
static const uint8_t FIXED_CHANNEL = 6;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

static const uint32_t ESPNOW_MAGIC = 0xA0B0C0D0;

enum MsgType : uint8_t { MSG_DATA = 1, MSG_ACK = 2 };
enum TopicKind : uint8_t { TK_ENV = 1, TK_BIO = 2, TK_GW = 3 };

#pragma pack(push, 1)
struct NowMsg {
  uint32_t magic;
  uint8_t  type;
  uint8_t  kind;
  uint16_t _pad;
  uint32_t seq;
  char payload[200];
};
#pragma pack(pop)

static int getRadioChannel() {
  uint8_t primary = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&primary, &second) == ESP_OK) return (int)primary;
  return -1;
}

static void lockRadioChannel(uint8_t ch) {
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

static void sendAck(uint32_t seq) {
  NowMsg a{};
  a.magic = ESPNOW_MAGIC;
  a.type = MSG_ACK;
  a.seq = seq;

  lockRadioChannel(FIXED_CHANNEL);
  esp_now_send(ESP_A_MAC, (const uint8_t*)&a, sizeof(a));
}

static void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != (int)sizeof(NowMsg)) return;

  NowMsg m{};
  memcpy(&m, data, sizeof(m));
  if (m.magic != ESPNOW_MAGIC) return;

  if (m.type == MSG_DATA) {
    sendAck(m.seq);

    const char* topic =
      (m.kind == TK_ENV) ? TOPIC_ENV_A :
      (m.kind == TK_BIO) ? TOPIC_BIO_A :
      (m.kind == TK_GW)  ? TOPIC_GW_A  : nullptr;

    Serial.printf("[B][ESPNOW] rx kind=%u seq=%lu from %02X:%02X:%02X:%02X:%02X:%02X\n",
                  (unsigned)m.kind, (unsigned long)m.seq,
                  info->src_addr[0], info->src_addr[1], info->src_addr[2],
                  info->src_addr[3], info->src_addr[4], info->src_addr[5]);

    if (topic && WiFi.status() == WL_CONNECTED && mqtt.connected()) {
      m.payload[sizeof(m.payload) - 1] = '\0';
      mqtt.publish(topic, m.payload);
    }
  }
}

static bool initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  lockRadioChannel(FIXED_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[B][ESPNOW] init FAIL");
    return false;
  }

  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, ESP_A_MAC, 6);
  peer.channel = FIXED_CHANNEL;
  peer.encrypt = false;

  if (!esp_now_is_peer_exist(ESP_A_MAC)) {
    if (esp_now_add_peer(&peer) != ESP_OK) {
      Serial.println("[B][ESPNOW] add_peer FAIL");
      return false;
    }
  }

  Serial.printf("[B][ESPNOW] ready (fixed ch=%u)\n", FIXED_CHANNEL);
  return true;
}

static void mqttEnsure() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;

  String cid = String("ESP_B_") + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(cid.c_str())) {
    Serial.println("[B] MQTT connected");
  } else {
    Serial.printf("[B] MQTT failed rc=%d\n", mqtt.state());
  }
}

static String isoNowUtcLite() {
  uint32_t ms = millis();
  char buf[32];
  snprintf(buf, sizeof(buf), "t%lu", (unsigned long)(ms / 1000));
  return String(buf);
}

static uint32_t lastPub = 0;
static const uint32_t B_PUB_MS = 2000;

static void publishBData() {
  if (!(WiFi.status() == WL_CONNECTED && mqtt.connected())) return;

  int hr = 72 + (int)((millis() / 1000) % 5);
  int rr = 60000 / hr;

  String t = isoNowUtcLite();
  String bio = String("{\"teamId\":\"Team_A\",\"ffId\":\"FF_B\",\"originNodeId\":\"B\",")
    + "\"via\":\"self\",\"failover\":false,\"forwardHopCount\":0,\"observedAt\":\"" + t
    + "\",\"hrBpm\":" + String(hr) + ",\"rrMs\":" + String(rr) + ",\"wearableOk\":true,\"source\":\"demo\"}";

  String env = String("{\"teamId\":\"Team_A\",\"ffId\":\"FF_B\",\"nodeId\":\"B\",\"originNodeId\":\"B\",")
    + "\"via\":\"self\",\"failover\":false,\"forwardHopCount\":0,\"observedAt\":\"" + t
    + "\",\"mq2Raw\":1234,\"source\":\"demo\"}";

  String gw = String("{\"nodeId\":\"B\",\"observedAt\":\"") + t
    + "\",\"wifiConnected\":true,\"radioChannel\":" + String(getRadioChannel())
    + ",\"fixedChannel\":" + String(FIXED_CHANNEL)
    + ",\"mqttConnected\":" + String(mqtt.connected() ? "true" : "false") + "}";

  mqtt.publish(TOPIC_BIO_B, bio.c_str());
  mqtt.publish(TOPIC_ENV_B, env.c_str());
  mqtt.publish(TOPIC_GW_B,  gw.c_str());
}

static uint32_t lastStatus = 0;
static const uint32_t STATUS_MS = 2000;

void setup() {
  Serial.begin(115200);
  delay(150);

  Serial.println("\n=== ESP_B boot ===");
  Serial.printf("[B] MAC (STA) = %s\n", WiFi.macAddress().c_str());

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);

  initEspNow();

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.println("[B] Ready.");
}

void loop() {
  lockRadioChannel(FIXED_CHANNEL); // keep it pinned
  mqttEnsure();
  mqtt.loop();

  uint32_t now = millis();
  if (now - lastStatus >= STATUS_MS) {
    lastStatus = now;
    Serial.printf("[B] WiFi=%d SSID=%s ip=%s radioCh=%d fixed=%u MQTT=%s\n",
                  (int)WiFi.status(),
                  (WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "-"),
                  (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-"),
                  getRadioChannel(),
                  FIXED_CHANNEL,
                  (mqtt.connected() ? "UP" : "DOWN"));
  }

  if (now - lastPub >= B_PUB_MS) {
    lastPub = now;
    publishBData();
  }

  delay(2);
}
