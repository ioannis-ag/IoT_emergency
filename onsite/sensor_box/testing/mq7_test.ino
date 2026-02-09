#include <WiFi.h>
#include <PubSubClient.h>

// ---------- WiFi ----------
static const char* WIFI_SSID = "VODAFONE_H268Q-4057";
static const char* WIFI_PASS = "SFzDE5ZxHyQPQ2FQ";
// ---------- MQTT ----------
const char* MQTT_HOST = "192.168.2.13";
const int   MQTT_PORT = 1883;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ---------- Sensors ----------
#define MQ7_PIN 4   // MQ-7 AOUT -> GPIO4
#define MQ2_PIN 5   // MQ-2 AOUT -> GPIO5

// Publish interval
static unsigned long lastPub = 0;
static const unsigned long PUB_MS = 1000;

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

void connectMQTT() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  while (!mqtt.connected()) {
    Serial.print("MQTT connecting...");
    String clientId = "esp32s3-mq-" + String((uint32_t)ESP.getEfuseMac(), HEX);

    // No username/password (set here if your broker requires it)
    if (mqtt.connect(clientId.c_str())) {
      Serial.println("OK");
    } else {
      Serial.print("failed rc=");
      Serial.print(mqtt.state());
      Serial.println(" retry in 2s");
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  // ADC setup
  analogReadResolution(12);        // 0..4095
  analogSetAttenuation(ADC_11db);  // ~0..3.3V range

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
  if (now - lastPub >= PUB_MS) {
    lastPub = now;

    int mq7_raw = analogRead(MQ7_PIN);
    int mq2_raw = analogRead(MQ2_PIN);

    float mq7_v = mq7_raw * (3.3f / 4095.0f);
    float mq2_v = mq2_raw * (3.3f / 4095.0f);

    // Publish (simple text payloads)
    mqtt.publish("sensors/mq7/raw", String(mq7_raw).c_str(), true);
    mqtt.publish("sensors/mq7/voltage", String(mq7_v, 3).c_str(), true);

    mqtt.publish("sensors/mq2/raw", String(mq2_raw).c_str(), true);
    mqtt.publish("sensors/mq2/voltage", String(mq2_v, 3).c_str(), true);

    // Also print locally
    Serial.print("MQ7 raw=");
    Serial.print(mq7_raw);
    Serial.print(" V=");
    Serial.print(mq7_v, 3);
    Serial.print(" | MQ2 raw=");
    Serial.print(mq2_raw);
    Serial.print(" V=");
    Serial.println(mq2_v, 3);
  }
}
