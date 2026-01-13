#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_now.h>
#include <esp_wifi.h>   // for wifi_tx_info_t

static constexpr char NODE_ID = 'B';

static const char* WIFI_SSID = "VODAFONE_H268Q-4057";
static const char* WIFI_PASS = "SFzDE5ZxHyQPQ2FQ";

static const char* MQTT_HOST = "192.168.2.13";
static const int   MQTT_PORT = 1883;

// IMPORTANT: set this to A's *WiFi.macAddress()* (STA MAC)
static const uint8_t ESP_A_MAC[6] = { 0x78, 0x1C, 0x3C, 0xF5, 0xC5, 0xDC };


enum MsgType : uint8_t { MSG_BEACON=1, MSG_FWD_REQUEST=2, MSG_DATA_FORWARD=3 };

#pragma pack(push, 1)
struct BeaconMsg { uint8_t type; char node_id; uint8_t uplink_ok; int8_t rssi; };
struct ForwardRequest { uint8_t type; char from_id; };
struct ForwardData { uint8_t type; char src_id; uint32_t seq; uint16_t mq2; uint16_t bpm; uint8_t failover; };
#pragma pack(pop)

WiFiClient wifi;
PubSubClient mqtt(wifi);

bool uplink_ok=false;
unsigned long lastBeaconMs=0, lastOwnPubMs=0;
uint32_t ownSeq=0;

String topicFor(char id){ String t="ff/"; t+=id; t+="/telemetry"; return t; }

void ensureWiFi(){
  if(WiFi.status()==WL_CONNECTED) return;
  Serial.printf("[B] WiFi connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<15000){ delay(250); Serial.print("."); }
  Serial.println();
  if(WiFi.status()==WL_CONNECTED) Serial.printf("[B] WiFi OK IP=%s RSSI=%d CH=%d\n",
    WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
  else Serial.println("[B] WiFi FAIL");
}

void ensureMQTT(){
  if(WiFi.status()!=WL_CONNECTED) return;
  if(mqtt.connected()) return;
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  String cid="gwB-"+String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[B] MQTT connecting as %s\n", cid.c_str());
  bool ok=mqtt.connect(cid.c_str());
  Serial.printf("[B] MQTT %s state=%d\n", ok?"OK":"FAIL", mqtt.state());
}

void onEspNowSend(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  const uint8_t* mac = info->des_addr;
  Serial.printf("[ESP-NOW] send -> %02X:%02X:%02X:%02X:%02X:%02X %s\n",
    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],
    status==ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
}


void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len){
  if(len<1) return;
  uint8_t type=data[0];

  if(type==MSG_FWD_REQUEST && len==(int)sizeof(ForwardRequest)){
    ForwardRequest r; memcpy(&r, data, sizeof(r));
    Serial.printf("[B] ESP-NOW FWD_REQUEST from %c mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
      r.from_id,
      info->src_addr[0],info->src_addr[1],info->src_addr[2],
      info->src_addr[3],info->src_addr[4],info->src_addr[5]);
    return;
  }

  if(type==MSG_DATA_FORWARD && len==(int)sizeof(ForwardData)){
    ForwardData d; memcpy(&d, data, sizeof(d));
    Serial.printf("[B] ESP-NOW DATA_FORWARD src=%c seq=%lu mq2=%u bpm=%u uplink_ok=%d\n",
      d.src_id,(unsigned long)d.seq,d.mq2,d.bpm,uplink_ok);

    if(!uplink_ok) { Serial.println("[B] DROP forwarded data (uplink down)"); return; }

    char payload[220];
    snprintf(payload,sizeof(payload),
      "{\"node\":\"%c\",\"seq\":%lu,\"mq2\":%u,\"bpm\":%u,\"failover\":true,\"via\":\"B\",\"gw_rssi\":%d}",
      d.src_id,(unsigned long)d.seq,d.mq2,d.bpm,(WiFi.status()==WL_CONNECTED)?WiFi.RSSI():-127);

    String topic=topicFor(d.src_id);
    bool ok=mqtt.publish(topic.c_str(), payload);
    Serial.printf("[B] MQTT forward publish %s ok=%d\n", topic.c_str(), ok);
  }
}

void addPeer(const uint8_t mac[6]){
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = WiFi.channel();   // LOCK to current WiFi channel
  peer.encrypt = false;

  esp_err_t p = esp_now_add_peer(&peer);
  Serial.printf("[B] add peer %02X:%02X:%02X:%02X:%02X:%02X %s (%d)\n",
    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],
    (p==ESP_OK)?"OK":"FAIL", (int)p);
}

void setup(){
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  delay(50);

  Serial.printf("[B] WiFi  MAC %s\n", WiFi.macAddress().c_str());

  ensureWiFi();
  ensureMQTT();

  esp_err_t e=esp_now_init();
  Serial.printf("[B] ESP-NOW init %s (%d)\n", (e==ESP_OK)?"OK":"FAIL", (int)e);
  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_register_send_cb(onEspNowSend);


  addPeer(ESP_A_MAC);

  Serial.println("[B] Ready");
}

void loop(){
  ensureWiFi();
  ensureMQTT();
  mqtt.loop();

  uplink_ok=(WiFi.status()==WL_CONNECTED && mqtt.connected());

  unsigned long now=millis();

  // UNICAST beacon to A
  if(now-lastBeaconMs>=1000){
    lastBeaconMs=now;
    BeaconMsg b{MSG_BEACON, NODE_ID, (uint8_t)(uplink_ok?1:0),
                (int8_t)((WiFi.status()==WL_CONNECTED)?WiFi.RSSI():-127)};
    esp_err_t r=esp_now_send(ESP_A_MAC,(uint8_t*)&b,sizeof(b));
    Serial.printf("[B] Beacon uplink=%d rssi=%d send=%s (%d)\n",
      b.uplink_ok, b.rssi, (r==ESP_OK)?"OK":"FAIL", (int)r);
  }

  // B publishes its own telemetry too
  if(uplink_ok && now-lastOwnPubMs>=3000){
    lastOwnPubMs=now;
    uint16_t mq2=1500+(ownSeq%50);
    uint16_t bpm=75+(ownSeq%6);
    ownSeq++;

    char payload[200];
    snprintf(payload,sizeof(payload),
      "{\"node\":\"B\",\"seq\":%lu,\"mq2\":%u,\"bpm\":%u,\"failover\":false,\"via\":\"self\",\"rssi\":%d}",
      (unsigned long)ownSeq,mq2,bpm,(WiFi.status()==WL_CONNECTED)?WiFi.RSSI():-127);

    String topic=topicFor('B');
    bool ok=mqtt.publish(topic.c_str(), payload);
    Serial.printf("[B] MQTT own publish %s ok=%d\n", topic.c_str(), ok);
  }
}
