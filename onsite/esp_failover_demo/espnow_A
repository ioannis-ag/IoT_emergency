#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_now.h>
#include <esp_wifi.h>   // for wifi_tx_info_t
static constexpr char NODE_ID = 'A';

static const char* WIFI_SSID = "VODAFONE_H268Q-4057";
static const char* WIFI_PASS = "SFzDE5ZxHyQPQ2FQ";

static const char* MQTT_HOST = "192.168.2.13";
static const int   MQTT_PORT = 1883;

// IMPORTANT: set this to B's *WiFi.macAddress()* (STA MAC)
static const uint8_t ESP_B_MAC[6] = { 0x78, 0x1C, 0x3C, 0xF5, 0x97, 0x70 };


static const int MQ2_APIN = 34;

static const bool SIMULATE_UPLINK = true;
static const unsigned long SIM_OK_MS   = 20000;
static const unsigned long SIM_DOWN_MS = 30000;

static const unsigned long FAILOVER_AFTER_MS = 8000;
static const unsigned long RECOVER_AFTER_MS  = 8000;

static const unsigned long GW_STALE_MS     = 4000;
static const unsigned long PUB_INTERVAL_MS = 2000;

enum MsgType : uint8_t { MSG_BEACON=1, MSG_FWD_REQUEST=2, MSG_DATA_FORWARD=3 };

#pragma pack(push, 1)
struct BeaconMsg { uint8_t type; char node_id; uint8_t uplink_ok; int8_t rssi; };
struct ForwardRequest { uint8_t type; char from_id; };
struct ForwardData { uint8_t type; char src_id; uint32_t seq; uint16_t mq2; uint16_t bpm; uint8_t failover; };
#pragma pack(pop)

WiFiClient wifi;
PubSubClient mqtt(wifi);

bool uplink_ok_real=false;
bool uplink_ok_effective=false;

unsigned long downSince=0, upSince=0;
bool usingGateway=false;

bool gwUplinkOk=false;
unsigned long gwLastSeen=0;

unsigned long lastPubMs=0;
uint32_t seq=0;

String topicFor(char id){ String t="ff/"; t+=id; t+="/telemetry"; return t; }

bool simulateDownNow(){
  if(!SIMULATE_UPLINK) return false;
  unsigned long cycle=SIM_OK_MS+SIM_DOWN_MS;
  unsigned long m=millis()%cycle;
  return (m>=SIM_OK_MS);
}

bool gatewayFreshAndUp(){ return (millis()-gwLastSeen)<GW_STALE_MS && gwUplinkOk; }

void ensureWiFi(){
  if(WiFi.status()==WL_CONNECTED) return;
  Serial.printf("[A] WiFi connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<15000){ delay(250); Serial.print("."); }
  Serial.println();
  if(WiFi.status()==WL_CONNECTED) Serial.printf("[A] WiFi OK IP=%s RSSI=%d CH=%d\n",
    WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
  else Serial.println("[A] WiFi FAIL");
}

void ensureMQTT(){
  if(WiFi.status()!=WL_CONNECTED) return;
  if(mqtt.connected()) return;
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  String cid="nodeA-"+String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[A] MQTT connecting as %s\n", cid.c_str());
  bool ok=mqtt.connect(cid.c_str());
  Serial.printf("[A] MQTT %s state=%d\n", ok?"OK":"FAIL", mqtt.state());
}
void onEspNowSend(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  const uint8_t* mac = info->des_addr;
  Serial.printf("[ESP-NOW] send -> %02X:%02X:%02X:%02X:%02X:%02X %s\n",
    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],
    status==ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
}

void sendForwardRequest(){
  ForwardRequest r{MSG_FWD_REQUEST, NODE_ID};
  esp_err_t e=esp_now_send(ESP_B_MAC,(uint8_t*)&r,sizeof(r));
  Serial.printf("[A] ESP-NOW send FWD_REQUEST -> B %s (%d)\n", (e==ESP_OK)?"OK":"FAIL", (int)e);
}

void sendForwardData(uint16_t mq2, uint16_t bpm){
  ForwardData d{MSG_DATA_FORWARD, NODE_ID, seq, mq2, bpm, 1};
  esp_err_t e=esp_now_send(ESP_B_MAC,(uint8_t*)&d,sizeof(d));
  Serial.printf("[A] ESP-NOW send DATA_FORWARD -> B %s (%d)\n", (e==ESP_OK)?"OK":"FAIL", (int)e);
}

void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len){
  if(len<1) return;
  uint8_t type=data[0];

  if(type==MSG_BEACON && len==(int)sizeof(BeaconMsg)){
    BeaconMsg b; memcpy(&b,data,sizeof(b));
    if(b.node_id=='B'){
      gwUplinkOk=(b.uplink_ok==1);
      gwLastSeen=millis();
      Serial.printf("[A] Beacon from B uplink=%d rssi=%d mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
        b.uplink_ok,b.rssi,
        info->src_addr[0],info->src_addr[1],info->src_addr[2],
        info->src_addr[3],info->src_addr[4],info->src_addr[5]);
    }
  }
}

void addPeerB(){
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, ESP_B_MAC, 6);
  peer.channel = WiFi.channel();   // LOCK channel
  peer.encrypt = false;

  esp_err_t p=esp_now_add_peer(&peer);
  Serial.printf("[A] add peer B %s (%d)\n", (p==ESP_OK)?"OK":"FAIL", (int)p);
}

void setupEspNow(){
  esp_err_t e=esp_now_init();
  Serial.printf("[A] ESP-NOW init %s (%d)\n", (e==ESP_OK)?"OK":"FAIL", (int)e);
  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_register_send_cb(onEspNowSend);
  addPeerB();
}

void setup(){
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  delay(50);

  Serial.printf("[A] WiFi  MAC %s\n", WiFi.macAddress().c_str());

  pinMode(MQ2_APIN, INPUT);
  analogReadResolution(12);

  ensureWiFi();
  ensureMQTT();

  setupEspNow();

  Serial.printf("[A] SIM cycle OK=%lus DOWN=%lus\n", SIM_OK_MS/1000, SIM_DOWN_MS/1000);
}

void loop(){
  ensureWiFi();
  ensureMQTT();
  mqtt.loop();

  uplink_ok_real=(WiFi.status()==WL_CONNECTED && mqtt.connected());
  bool simDown=simulateDownNow();
  uplink_ok_effective=uplink_ok_real && !simDown;

  unsigned long now=millis();

  if(!uplink_ok_effective){
    if(downSince==0) downSince=now;
    upSince=0;
  } else {
    if(upSince==0) upSince=now;
    downSince=0;
  }

  if(!usingGateway){
    if(downSince && (now-downSince)>=FAILOVER_AFTER_MS && gatewayFreshAndUp()){
      Serial.println("[A] >>> FAILOVER start forwarding to B");
      usingGateway=true;
      sendForwardRequest();
    }
  } else {
    if(upSince && (now-upSince)>=RECOVER_AFTER_MS){
      Serial.println("[A] <<< RECOVER stop forwarding, publish self");
      usingGateway=false;
    }
    if(!gatewayFreshAndUp()){
      Serial.println("[A] !!! gateway stale/down");
    }
  }

  if(now-lastPubMs>=PUB_INTERVAL_MS){
    lastPubMs=now;

    uint16_t mq2=(uint16_t)analogRead(MQ2_APIN);
    uint16_t bpm=80+(seq%10);
    seq++;

    Serial.printf("[A] tick seq=%lu real=%d simDown=%d eff=%d usingGw=%d gwFresh=%d\n",
      (unsigned long)seq, uplink_ok_real, simDown, uplink_ok_effective, usingGateway, gatewayFreshAndUp());

    if(uplink_ok_effective && !usingGateway){
      char payload[240];
      snprintf(payload,sizeof(payload),
        "{\"node\":\"A\",\"seq\":%lu,\"mq2\":%u,\"bpm\":%u,\"failover\":false,\"via\":\"self\","
        "\"uplink_real\":%s,\"uplink_effective\":%s,\"sim_down\":%s,\"rssi\":%d}",
        (unsigned long)seq,mq2,bpm,
        uplink_ok_real?"true":"false",
        uplink_ok_effective?"true":"false",
        simDown?"true":"false",
        (WiFi.status()==WL_CONNECTED)?WiFi.RSSI():-127);

      bool ok=mqtt.publish(topicFor('A').c_str(), payload);
      Serial.printf("[A] MQTT own publish ok=%d\n", ok);
    } else {
      if(usingGateway && gatewayFreshAndUp()){
        sendForwardData(mq2,bpm);
      } else {
        Serial.println("[A] NO UPLINK (would buffer here)");
      }
    }
  }
}
