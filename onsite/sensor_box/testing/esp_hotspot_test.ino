#include <WiFi.h>
#include <PubSubClient.h>

// -------- iPhone hotspot --------
static const char* WIFI_SSID = "FirefighterA";
static const char* WIFI_PASS = "firefighterA";

// -------- Remote MQTT --------
static const char* MQTT_HOST = "141.237.94.127";
static const int   MQTT_PORT = 1883;

// -------- MQTT topic --------
static const char* TOPIC = "ff/A/demo";

WiFiClient wifi;
PubSubClient mqtt(wifi);

unsigned long lastPub = 0;
uint32_t seq = 0;

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("[A] Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("[A] WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
}

void connectMQTT() {
  if (mqtt.connected()) return;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  char clientId[32];
  uint64_t mac = ESP.getEfuseMac();
  snprintf(clientId, sizeof(clientId),
           "espA-demo-%06lX", (unsigned long)(mac & 0xFFFFFF));

  Serial.print("[A] Connecting MQTT...");
  if (mqtt.connect(clientId)) {
    Serial.println("OK");
  } else {
    Serial.print("FAIL rc=");
    Serial.println(mqtt.state());
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("\n=== ESP A HOTSPOT + MQTT DEMO ===");

  connectWiFi();
  connectMQTT();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (!mqtt.connected()) {
    connectMQTT();
  }

  mqtt.loop();

  unsigned long now = millis();
  if (now - lastPub >= 2000 && mqtt.connected()) {
    lastPub = now;
    seq++;

    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"node\":\"A\",\"seq\":%lu,\"rssi\":%d}",
             (unsigned long)seq, WiFi.RSSI());

    bool ok = mqtt.publish(TOPIC, payload);
    Serial.printf("[A] Publish #%lu %s\n", (unsigned long)seq, ok ? "OK" : "FAIL");
  }

  delay(10);
}
