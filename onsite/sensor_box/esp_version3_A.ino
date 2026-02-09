#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

static const uint8_t ESP_A_MAC[6] = { /* PUT NODE A STA MAC HERE */ };

enum EspNowMsgType : uint8_t {
  MSG_BEACON_REQ = 1,
  MSG_CAPSULE    = 2
};

#pragma pack(push, 1)
struct BeaconReq {
  uint8_t  type;
  char     dst_id;
  uint32_t seq;
  uint32_t ms;
};

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
  int16_t  hum_x100;

  int8_t   rssi;
  uint8_t  uplink_ok;
  uint8_t  ble_ok;
  uint8_t  ecg_on;
};
#pragma pack(pop)

static uint32_t seq = 0;
static uint32_t lastBeacon = 0;
static const uint32_t BEACON_PERIOD_MS = 2000;

static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len < 1) return;
  if (data[0] != MSG_CAPSULE) return;
  if (len < (int)sizeof(CapsuleMsg)) return;

  const CapsuleMsg* c = (const CapsuleMsg*)data;

  Serial.printf("[CAPSULE] from %c seq=%lu bpm=%u rmssd=%u sdnn=%u mq2=%u temp=%.2fC hum=%.2f%% rssi=%d uplink=%u ble=%u ecg=%u\n",
    c->src_id,
    (unsigned long)c->seq,
    (unsigned)c->bpm,
    (unsigned)c->rmssd_ms,
    (unsigned)c->sdnn_ms,
    (unsigned)c->mq2_adc,
    (c->temp_c_x100 == (int16_t)0x7FFF) ? NAN : (c->temp_c_x100 / 100.0f),
    (c->hum_x100  == (int16_t)0x7FFF) ? NAN : (c->hum_x100  / 100.0f),
    (int)c->rssi,
    (unsigned)c->uplink_ok,
    (unsigned)c->ble_ok,
    (unsigned)c->ecg_on
  );
}

static void sendBeaconReq() {
  BeaconReq b{};
  b.type = MSG_BEACON_REQ;
  b.dst_id = 'A';
  b.seq = ++seq;
  b.ms = millis();

  esp_err_t e = esp_now_send(ESP_A_MAC, (uint8_t*)&b, sizeof(b));
  Serial.printf("[BEACON] ->A %s (%d)\n", (e == ESP_OK) ? "OK" : "FAIL", (int)e);
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== NODE B (ESP32) | ESP-NOW beacon requester ===");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);

  esp_err_t e = esp_now_init();
  Serial.printf("[ESP-NOW] init %s (%d)\n", (e == ESP_OK) ? "OK" : "FAIL", (int)e);

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, ESP_A_MAC, 6);
  peer.channel = 0;     // auto
  peer.encrypt = false;

  esp_err_t p = esp_now_add_peer(&peer);
  Serial.printf("[ESP-NOW] add peer(A) %s (%d)\n", (p == ESP_OK) ? "OK" : "FAIL", (int)p);

  esp_now_register_recv_cb(onRecv);
}

void loop() {
  uint32_t now = millis();

  // For now: beacon periodically.
  // Later you can gate this by "only beacon if haven't received MQTT/telemetry from A for X seconds".
  if (now - lastBeacon >= BEACON_PERIOD_MS) {
    lastBeacon = now;
    sendBeaconReq();
  }

  delay(20);
}
