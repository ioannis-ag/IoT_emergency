#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_now.h>
#include <esp_wifi.h>

// BLE (Arduino-ESP32 BLE library)
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include "esp_bt.h"

// ---------------- IDs ----------------
static constexpr char NODE_ID = 'A';

// ---------------- WiFi/MQTT ----------------
static const char* WIFI_SSID = "VODAFONE_H268Q-4057";
static const char* WIFI_PASS = "SFzDE5ZxHyQPQ2FQ";

static const char* MQTT_HOST = "192.168.2.13";
static const int   MQTT_PORT = 1883;

// ---------------- ESP-NOW peer (B STA MAC) ----------------
static const uint8_t ESP_B_MAC[6] = { 0x78, 0x1C, 0x3C, 0xF5, 0x97, 0x70 };

// ---------------- Sensors ----------------
static const int MQ2_APIN = 34;

// ---------------- Failover timing ----------------
static const unsigned long FAILOVER_AFTER_MS = 8000;
static const unsigned long RECOVER_AFTER_MS  = 8000;
static const unsigned long GW_STALE_MS       = 4000;
static const unsigned long PUB_INTERVAL_MS   = 2000;

// ---------------- BLE Polar HR ----------------
static const char* POLAR_PREFIX = "Polar H10";
static BLEUUID HRS_SERVICE((uint16_t)0x180D);
static BLEUUID HRM_CHAR  ((uint16_t)0x2A37);

static const uint32_t BLE_SCAN_SEC          = 3;      // short scan
static const uint32_t BLE_RETRY_MS          = 8000;   // retry scan/connect every 8s if disconnected
static const uint32_t BLE_CONNECT_TIMEOUT_MS= 4000;

// ---------------- Protocol ----------------
enum MsgType : uint8_t { MSG_BEACON=1, MSG_FWD_REQUEST=2, MSG_DATA_FORWARD=3 };

#pragma pack(push, 1)
struct BeaconMsg { uint8_t type; char node_id; uint8_t uplink_ok; int8_t rssi; };
struct ForwardRequest { uint8_t type; char from_id; };
struct ForwardData { uint8_t type; char src_id; uint32_t seq; uint16_t mq2; uint16_t bpm; uint8_t failover; };
#pragma pack(pop)

// ---------------- Globals ----------------
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

// BLE state
static BLEAdvertisedDevice* polarDevice = nullptr;
static BLEClient* polarClient = nullptr;
static BLERemoteCharacteristic* hrmChr = nullptr;

static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint16_t latestBpm = 0;
static volatile bool gotHr = false;

static uint32_t lastBleTry = 0;
static bool bleConnected = false;

// ---------------- Helpers ----------------
String topicFor(char id){ String t="ff/"; t+=id; t+="/telemetry"; return t; }

static bool enableCCCD(BLERemoteCharacteristic* chr, uint16_t value) {
  BLERemoteDescriptor* cccd = chr->getDescriptor(BLEUUID((uint16_t)0x2902));
  if (!cccd) return false;
  uint8_t v[2];
  v[0] = (uint8_t)(value & 0xFF);
  v[1] = (uint8_t)((value >> 8) & 0xFF);
  return cccd->writeValue(v, 2, true);
}

// ---------------- WiFi/MQTT (non-blocking) ----------------
void ensureWiFi(){
  if(WiFi.status()==WL_CONNECTED) return;

  static uint32_t lastTry=0;
  uint32_t now=millis();
  if(now-lastTry < 2000) return;
  lastTry=now;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.printf("[A] WiFi begin SSID=%s\n", WIFI_SSID);
}

void ensureMQTT(){
  if(WiFi.status()!=WL_CONNECTED) return;
  if(mqtt.connected()) return;

  static uint32_t lastTry=0;
  uint32_t now=millis();
  if(now-lastTry < 2000) return;
  lastTry=now;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  char cid[40];
  uint64_t mac = ESP.getEfuseMac();
  snprintf(cid, sizeof(cid), "nodeA-%06lX%06lX",
           (unsigned long)(mac >> 24), (unsigned long)(mac & 0xFFFFFF));

  bool ok=mqtt.connect(cid);
  Serial.printf("[A] MQTT %s state=%d\n", ok?"OK":"FAIL", mqtt.state());
}

// ---------------- ESP-NOW ----------------
void onEspNowSend(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  const uint8_t* mac = info->des_addr;
  Serial.printf("[ESP-NOW] send -> %02X:%02X:%02X:%02X:%02X:%02X %s\n",
    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],
    status==ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
}

void sendForwardRequest(){
  ForwardRequest r{MSG_FWD_REQUEST, NODE_ID};
  esp_err_t e=esp_now_send(ESP_B_MAC,(uint8_t*)&r,sizeof(r));
  Serial.printf("[A] ESP-NOW FWD_REQUEST -> B %s (%d)\n", (e==ESP_OK)?"OK":"FAIL", (int)e);
}

void sendForwardData(uint16_t mq2, uint16_t bpm){
  ForwardData d{MSG_DATA_FORWARD, NODE_ID, seq, mq2, bpm, 1};
  esp_err_t e=esp_now_send(ESP_B_MAC,(uint8_t*)&d,sizeof(d));
  Serial.printf("[A] ESP-NOW DATA_FORWARD -> B %s (%d)\n", (e==ESP_OK)?"OK":"FAIL", (int)e);
}

void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len){
  if(len<1) return;
  uint8_t type=data[0];

  if(type==MSG_BEACON && len==(int)sizeof(BeaconMsg)){
    BeaconMsg b; memcpy(&b,data,sizeof(b));
    if(b.node_id=='B'){
      gwUplinkOk=(b.uplink_ok==1);
      gwLastSeen=millis();
      Serial.printf("[A] Beacon from B uplink=%d rssi=%d\n", b.uplink_ok, b.rssi);
    }
  }
}

bool gatewayFreshAndUp(){ return (millis()-gwLastSeen)<GW_STALE_MS && gwUplinkOk; }

static int lastPeerChannel = -1;
void ensurePeerChannel(){
  if(WiFi.status()!=WL_CONNECTED) return;
  int ch = WiFi.channel();
  if(ch == lastPeerChannel) return;

  // remove + re-add peer with new channel
  esp_now_del_peer(ESP_B_MAC);

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, ESP_B_MAC, 6);
  peer.channel = ch;
  peer.encrypt = false;

  esp_err_t p = esp_now_add_peer(&peer);
  Serial.printf("[A] Peer(B) channel=%d add %s (%d)\n", ch, (p==ESP_OK)?"OK":"FAIL", (int)p);
  lastPeerChannel = ch;
}

void setupEspNow(){
  esp_err_t e=esp_now_init();
  Serial.printf("[A] ESP-NOW init %s (%d)\n", (e==ESP_OK)?"OK":"FAIL", (int)e);
  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_register_send_cb(onEspNowSend);
}

// ---------------- BLE HR callback ----------------
static void hrCb(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len < 2) return;
  uint8_t flags = data[0];
  bool hr16 = (flags & 0x01) != 0;

  uint16_t bpm;
  if (!hr16) bpm = data[1];
  else {
    if (len < 3) return;
    bpm = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
  }

  portENTER_CRITICAL(&g_mux);
  latestBpm = bpm;
  gotHr = true;
  portEXIT_CRITICAL(&g_mux);
}

// ---------------- BLE scanning ----------------
class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (!dev.haveName()) return;
    std::string n = dev.getName();
    if (n.rfind(POLAR_PREFIX, 0) != 0) return; // startswith

    // capture first match
    polarDevice = new BLEAdvertisedDevice(dev);
    BLEDevice::getScan()->stop();
  }
};

bool connectPolarHR_once(){
  polarDevice = nullptr;

  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), true);
  scan->setActiveScan(true);
  scan->start(BLE_SCAN_SEC, false);

  if(!polarDevice){
    Serial.println("[A] BLE scan: no Polar found");
    return false;
  }

  polarClient = BLEDevice::createClient();
  // NOTE: connect() can block; keep it short by having scan already stopped
  bool ok = polarClient->connect(polarDevice);
  if(!ok){
    Serial.println("[A] BLE connect failed");
    return false;
  }

  BLERemoteService* hrs = polarClient->getService(HRS_SERVICE);
  if(!hrs){
    Serial.println("[A] HRS service missing");
    polarClient->disconnect();
    return false;
  }

  hrmChr = hrs->getCharacteristic(HRM_CHAR);
  if(!hrmChr){
    Serial.println("[A] HRM char missing");
    polarClient->disconnect();
    return false;
  }

  hrmChr->registerForNotify(hrCb);
  if(!enableCCCD(hrmChr, 0x0001)){
    Serial.println("[A] CCCD enable failed");
    polarClient->disconnect();
    return false;
  }

  Serial.println("[A] Polar HR notifications enabled");
  return true;
}

void bleTick(){
  // If connected but dropped, mark it
  if(bleConnected && polarClient && !polarClient->isConnected()){
    Serial.println("[A] BLE disconnected");
    bleConnected = false;
    gotHr = false;
  }

  if(bleConnected) return;

  uint32_t now = millis();
  if(now - lastBleTry < BLE_RETRY_MS) return;
  lastBleTry = now;

  Serial.println("[A] BLE trying to connect Polar (HR only)...");
  bleConnected = connectPolarHR_once();
}

// ---------------- Arduino ----------------
void setup(){
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  delay(50);

  Serial.printf("[A] WiFi MAC %s\n", WiFi.macAddress().c_str());

  pinMode(MQ2_APIN, INPUT);
  analogReadResolution(12);

  // Start WiFi/MQTT
  ensureWiFi();

  // ESP-NOW
  setupEspNow();

  // BLE init (BLE only, free classic BT RAM)
  BLEDevice::init("");
  BLEDevice::setPower(ESP_PWR_LVL_P6); // not max; keeps RF calmer sometimes
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  // Small MQTT buffers help stability
  mqtt.setBufferSize(256);
}

void loop(){
  ensureWiFi();
  ensureMQTT();
  mqtt.loop();

  // Keep peer channel synced to AP channel
  ensurePeerChannel();

  // BLE state machine
  bleTick();

  // Uplink logic
  uplink_ok_real = (WiFi.status()==WL_CONNECTED && mqtt.connected());
  uplink_ok_effective = uplink_ok_real; // no simulated down here

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

  // Publish tick
  if(now-lastPubMs>=PUB_INTERVAL_MS){
    lastPubMs=now;

    uint16_t mq2=(uint16_t)analogRead(MQ2_APIN);

    uint16_t bpm;
    bool hrOk;
    portENTER_CRITICAL(&g_mux);
    bpm = latestBpm;
    hrOk = gotHr;
    portEXIT_CRITICAL(&g_mux);

    seq++;

    // If we don't have HR yet, publish bpm=0 and hrok=false (so dashboard shows “no sensor”)
    if(uplink_ok_effective && !usingGateway){
      char payload[220];
      snprintf(payload,sizeof(payload),
        "{\"node\":\"A\",\"seq\":%lu,\"mq2\":%u,\"bpm\":%u,\"hrok\":%s,"
        "\"failover\":false,\"via\":\"self\",\"rssi\":%d,\"ble\":%s}",
        (unsigned long)seq,mq2,bpm,
        hrOk?"true":"false",
        (WiFi.status()==WL_CONNECTED)?WiFi.RSSI():-127,
        bleConnected?"true":"false");

      bool ok=mqtt.publish(topicFor('A').c_str(), payload);
      Serial.printf("[A] MQTT publish ok=%d bpm=%u hrok=%d\n", ok, bpm, hrOk);
    } else {
      if(usingGateway && gatewayFreshAndUp()){
        sendForwardData(mq2, bpm);
      } else {
        Serial.println("[A] NO UPLINK (would buffer here)");
      }
    }
  }

  delay(2); // yields to BLE/WiFi tasks
}

