#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include <math.h>

#include "ecg_segment_ff_c.h"

// Identity
static constexpr char NODE_ID_CHAR = 'C';
static const char*    NODE_ID      = "C";
static const char*    TEAM_ID      = "Team_A";
static const char*    FIREFIGHTER_ID = "FF_C";

// Wi-Fi credentials rotation
struct WifiCred { const char* ssid; const char* pass; };
static WifiCred WIFI_LIST[] = {
  { "VODAFONE_H268Q-4057", "SFzDE5ZxHyQPQ2FQ" },
  { "FirefighterA",        "firefighterA"     },
};
static constexpr int WIFI_LIST_N = sizeof(WIFI_LIST) / sizeof(WIFI_LIST[0]);

// MQTT
static const char* MQTT_HOST = "192.168.2.13";
static const int   MQTT_PORT = 1883;

// Topics
static String TOPIC_ENV = String("ngsi/Environment/") + TEAM_ID + "/" + FIREFIGHTER_ID;
static String TOPIC_BIO = String("ngsi/Biomedical/")  + TEAM_ID + "/" + FIREFIGHTER_ID;
static String TOPIC_GW  = String("ngsi/Gateway/")     + NODE_ID;
static String TOPIC_ECG = String("raw/ECG/")          + TEAM_ID + "/" + FIREFIGHTER_ID;

// Multi-rate publishing
static const uint32_t ENV_INTERVAL_MS = 2000;
static const uint32_t BIO_INTERVAL_MS = 1000;
static const uint32_t GW_INTERVAL_MS  = 5000;

// ECG replay
static const uint32_t ECG_REPLAY_PUB_MS = 50;
static const uint16_t ECG_REPLAY_SAMPLES_PER_BUNDLE = 20;
static bool     replayActive = false;
static uint32_t replayEndMs  = 0;
static uint32_t replayIdx    = 0;

// Connectivity timing
static const uint32_t MQTT_RETRY_MS          = 2000;
static const uint32_t WIFI_ATTEMPT_WINDOW_MS = 15000;

// Globals
static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

static volatile bool wifiGotIp = false;
static volatile bool wifiConnecting = false;
static int wifiIndex = -1;
static uint32_t wifiAttemptStart = 0;

static uint32_t lastMqttTry = 0;
static uint32_t lastEnv = 0;
static uint32_t lastBio = 0;
static uint32_t lastGw  = 0;
static uint32_t lastEcg = 0;

// Time (UTC via NTP)
static bool timeInited = false;
static uint32_t lastTimeTry = 0;

// Demo timeline
static uint32_t demoT0 = 0;

// Backend-friendly state
static int   batteryPct  = 94;
static float motionLevel = 0.45f;
static const char* activity = "walk";
static float stressIndex = 0.25f;
static bool wearableOk = true;

static uint32_t incidentId = 0;
static const char* incidentType = "none";
static const char* incidentSeverity = "info";
static bool incidentActive = false;
static int incidentTtlSec = 0;

// Math helpers
static float frand(float a, float b) { return a + (b - a) * (float)random(0, 10000) / 10000.0f; }
static float clampf(float x, float lo, float hi) { return (x < lo) ? lo : (x > hi) ? hi : x; }
static int   clampi(int x, int lo, int hi) { return (x < lo) ? lo : (x > hi) ? hi : x; }

// Wi-Fi RSSI helper
static inline int16_t wifiRssiDbmOrNeg127() {
  return (WiFi.status() == WL_CONNECTED) ? (int16_t)WiFi.RSSI() : (int16_t)-127;
}

// UTC time helpers
static void ensureTimeUtc() {
  if (timeInited) return;
  if (WiFi.status() != WL_CONNECTED) return;
  uint32_t now = millis();
  if (now - lastTimeTry < 10000) return;
  lastTimeTry = now;
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  time_t t = time(nullptr);
  if (t > 1700000000) timeInited = true;
}
static void iso8601UtcNow(char* out, size_t outSz) {
  time_t t = time(nullptr);
  if (t <= 100000) { snprintf(out, outSz, "1970-01-01T00:00:00Z"); return; }
  struct tm tm{};
  gmtime_r(&t, &tm);
  strftime(out, outSz, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

// Wi-Fi event-driven state
static void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:          wifiConnecting = true; break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:         wifiGotIp = true; wifiConnecting = false; break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:   wifiGotIp = false; wifiConnecting = false; break;
    default: break;
  }
}
static void startNextWifiAttempt() {
  wifiIndex = (wifiIndex + 1) % WIFI_LIST_N;
  wifiAttemptStart = millis();
  WiFi.disconnect(true);
  delay(50);
  WiFi.begin(WIFI_LIST[wifiIndex].ssid, WIFI_LIST[wifiIndex].pass);
}
static void ensureWiFi() {
  if (wifiGotIp) return;
  if (wifiConnecting && (millis() - wifiAttemptStart) < WIFI_ATTEMPT_WINDOW_MS) return;
  if (wifiAttemptStart == 0 || (millis() - wifiAttemptStart) >= WIFI_ATTEMPT_WINDOW_MS) startNextWifiAttempt();
}

// MQTT reconnect
static void ensureMQTT() {
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t now = millis();
  if (now - lastMqttTry < MQTT_RETRY_MS) return;
  lastMqttTry = now;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  uint64_t mac = ESP.getEfuseMac();
  char cid[80];
  snprintf(cid, sizeof(cid), "sim-%s-%s-node%c-%06lX%06lX",
           TEAM_ID, FIREFIGHTER_ID, NODE_ID_CHAR,
           (unsigned long)(mac >> 24), (unsigned long)(mac & 0xFFFFFF));

  mqtt.connect(cid);
}

// ECG replay control
static void startEcgReplay(uint32_t nowMs, uint32_t durationMs) {
  replayActive = true;
  replayEndMs = nowMs + durationMs;
  replayIdx = 0;
}
static void stopEcgReplay() { replayActive = false; }

static void publishReplayBundle(uint32_t nowMs, int hrBpm, float stress) {
  if (!mqtt.connected()) return;
  if (!replayActive) return;

  if ((int32_t)(nowMs - replayEndMs) >= 0) { stopEcgReplay(); return; }

  const uint16_t n = ECG_REPLAY_SAMPLES_PER_BUNDLE;
  uint8_t buf[4 + 4 + 2 + 2 + 1 + (n * 2)];
  uint16_t p = 0;

  buf[p++] = 'E'; buf[p++] = 'C'; buf[p++] = 'G'; buf[p++] = '1';

  uint32_t ms = nowMs;
  buf[p++] = (uint8_t)(ms & 0xFF);
  buf[p++] = (uint8_t)((ms >> 8) & 0xFF);
  buf[p++] = (uint8_t)((ms >> 16) & 0xFF);
  buf[p++] = (uint8_t)((ms >> 24) & 0xFF);

  buf[p++] = (uint8_t)(n & 0xFF);
  buf[p++] = (uint8_t)((n >> 8) & 0xFF);

  uint16_t hr = (uint16_t)clampi(hrBpm, 0, 250);
  buf[p++] = (uint8_t)(hr & 0xFF);
  buf[p++] = (uint8_t)((hr >> 8) & 0xFF);

  uint8_t st = (uint8_t)clampi((int)lround(clampf(stress, 0.0f, 1.0f) * 255.0f), 0, 255);
  buf[p++] = st;

  for (uint16_t i = 0; i < n; i++) {
    int16_t s = ECG_SEGMENT_DATA[replayIdx++];
    if (replayIdx >= ECG_SEGMENT_LEN) replayIdx = 0;
    buf[p++] = (uint8_t)(s & 0xFF);
    buf[p++] = (uint8_t)((s >> 8) & 0xFF);
  }

  mqtt.publish(TOPIC_ECG.c_str(), buf, p);
}

// Smooth ramp helper for demo phases
static float smoothstep(float x) {
  x = clampf(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

// Update motion/activity/battery slowly for realism
static void updateKinematics(uint32_t nowMs) {
  float t = nowMs / 1000.0f;
  float base = 0.45f + 0.18f * sinf(t * 0.05f + 0.8f);
  motionLevel = clampf(base + frand(-0.06f, 0.06f), 0.05f, 1.0f);

  if (motionLevel < 0.22f) activity = "still";
  else if (motionLevel < 0.62f) activity = "walk";
  else activity = "run";

  if ((nowMs % 7000) < 20) {
    int drop = (int)random(0, 2);
    batteryPct = clampi(batteryPct - drop, 20, 100);
  }
}

// Demo phase synthesis
static void synthDemoSignals(uint32_t nowMs,
                             float& outTempC, float& outHumPct, int& outMq2Raw, float& outCoPpm,
                             int& outHrBpm, int& outRrMs) {
  uint32_t tms = nowMs - demoT0;
  float t = tms / 1000.0f;

  float baseTemp = 38.0f + 1.3f * sinf(t * 0.05f);
  float baseHum  = 34.0f + 2.0f * sinf(t * 0.04f + 1.1f);
  float baseCo   = 6.0f  + 1.0f * sinf(t * 0.07f + 0.3f);
  float baseMq2  = 950.0f + 60.0f * sinf(t * 0.06f + 0.2f);
  float baseHr   = 120.0f + 7.0f * sinf(t * 0.20f + 0.9f);

  float worsen = 0.0f;
  float danger = 0.0f;
  float recover = 0.0f;

  if (tms < 60000) {
    worsen = 0.0f;
    danger = 0.0f;
    recover = 0.0f;
  } else if (tms < 90000) {
    worsen = smoothstep((tms - 60000) / 30000.0f);
    danger = 0.0f;
    recover = 0.0f;
  } else if (tms < 210000) {
    worsen = 1.0f;
    danger = smoothstep((tms - 90000) / 120000.0f);
    recover = 0.0f;
  } else {
    worsen = 1.0f;
    danger = 1.0f;
    recover = smoothstep((tms - 210000) / 90000.0f);
  }

  float temp = baseTemp + 9.0f * worsen + 20.0f * danger - 18.0f * recover;
  float hum  = baseHum  - 2.0f * worsen - 6.0f  * danger + 5.0f  * recover;

  float mq2  = baseMq2 + 1050.0f * worsen + 1900.0f * danger - 1800.0f * recover;
  float co   = baseCo  + 16.0f   * worsen + 130.0f  * danger - 120.0f  * recover;

  float hr   = baseHr  + 25.0f   * worsen + 75.0f   * danger - 70.0f   * recover;

  temp += frand(-0.15f, 0.15f);
  hum  += frand(-0.35f, 0.35f);
  mq2  += frand(-25.0f, 25.0f);
  co   += frand(-0.8f, 0.8f);
  hr   += frand(-1.2f, 1.2f);

  temp = clampf(temp, 25.0f, 90.0f);
  hum  = clampf(hum,  8.0f,  90.0f);
  mq2  = clampf(mq2,  200.0f, 4095.0f);
  co   = clampf(co,   0.0f,  260.0f);
  hr   = clampf(hr,   60.0f, 210.0f);

  float stress = 0.18f + 0.35f * worsen + 0.75f * danger - 0.70f * recover;
  stress += 0.10f * (motionLevel > 0.65f ? 1.0f : 0.0f);
  stressIndex = clampf(stress + frand(-0.03f, 0.03f), 0.0f, 1.0f);

  float rr = 60000.0f / clampf(hr, 55.0f, 200.0f);
  float hrvJitter = (1.0f - stressIndex) * frand(-16.0f, 16.0f) + frand(-6.0f, 6.0f);
  rr = clampf(rr + hrvJitter, 260.0f, 1300.0f);

  outTempC = temp;
  outHumPct = hum;
  outMq2Raw = (int)lround(mq2);
  outCoPpm = co;
  outHrBpm = (int)lround(hr);
  outRrMs = (int)lround(rr);

  if (tms < 60000) {
    incidentActive = false;
    incidentType = "none";
    incidentSeverity = "info";
    incidentTtlSec = 0;
  } else if (tms < 90000) {
    incidentActive = true;
    incidentType = "smoke_co";
    incidentSeverity = "warn";
    incidentTtlSec = (int)((90000 - tms) / 1000);
  } else if (tms < 210000) {
    incidentActive = true;
    incidentType = "smoke_co";
    incidentSeverity = "danger";
    incidentTtlSec = (int)((210000 - tms) / 1000);
  } else {
    incidentActive = true;
    incidentType = "recovery";
    incidentSeverity = "info";
    incidentTtlSec = 0;
  }
}

// Publish JSON Environment
static void publishEnv(uint32_t nowMs,
                       float tempC, float humPct, int mq2Raw, float coPpm,
                       int hrBpm) {
  if (!mqtt.connected()) return;

  ensureTimeUtc();
  char ts[32];
  iso8601UtcNow(ts, sizeof(ts));

  char tbuf[16], hbuf[16], cobuf[16];
  dtostrf(tempC, 0, 2, tbuf);
  dtostrf(humPct, 0, 2, hbuf);
  dtostrf(coPpm,  0, 1, cobuf);

  int mq2Dig = (mq2Raw > 1800) ? 1 : 0;

  char env[900];
  snprintf(env, sizeof(env),
    "{"
      "\"teamId\":\"%s\",\"ffId\":\"%s\",\"nodeId\":\"%s\","
      "\"originNodeId\":\"%s\",\"via\":\"self\",\"failover\":false,\"forwardHopCount\":0,"
      "\"observedAt\":\"%s\","
      "\"tempC\":%s,\"humidityPct\":%s,"
      "\"mq2Raw\":%d,\"mq2Digital\":%d,"
      "\"coPpm\":%s,"
      "\"wifiRssiDbm\":%d,"
      "\"batteryPct\":%d,"
      "\"activity\":\"%s\",\"motionLevel\":%.2f,"
      "\"incidentActive\":%s,\"incidentType\":\"%s\",\"incidentId\":%lu,\"incidentSeverity\":\"%s\",\"incidentTtlSec\":%d,"
      "\"stressIndex\":%.2f,"
      "\"hrBpmHint\":%d,"
      "\"source\":\"sim\""
    "}",
    TEAM_ID, FIREFIGHTER_ID, NODE_ID,
    NODE_ID, ts,
    tbuf, hbuf,
    mq2Raw, mq2Dig,
    cobuf,
    (int)wifiRssiDbmOrNeg127(),
    batteryPct,
    activity, (double)motionLevel,
    incidentActive ? "true" : "false", incidentType, (unsigned long)incidentId, incidentSeverity, incidentTtlSec,
    (double)stressIndex,
    hrBpm
  );

  mqtt.publish(TOPIC_ENV.c_str(), env);
}

// Publish JSON Biomedical
static void publishBio(uint32_t nowMs, int hrBpm, int rrMs) {
  if (!mqtt.connected()) return;

  ensureTimeUtc();
  char ts[32];
  iso8601UtcNow(ts, sizeof(ts));

  char bio[700];
  snprintf(bio, sizeof(bio),
    "{"
      "\"teamId\":\"%s\",\"ffId\":\"%s\","
      "\"originNodeId\":\"%s\",\"via\":\"self\",\"failover\":false,\"forwardHopCount\":0,"
      "\"observedAt\":\"%s\","
      "\"hrBpm\":%d,\"rrMs\":%d,"
      "\"wearableOk\":%s,"
      "\"stressIndex\":%.2f,"
      "\"activity\":\"%s\",\"motionLevel\":%.2f,"
      "\"incidentActive\":%s,\"incidentType\":\"%s\",\"incidentId\":%lu,\"incidentSeverity\":\"%s\",\"incidentTtlSec\":%d,"
      "\"source\":\"sim\""
    "}",
    TEAM_ID, FIREFIGHTER_ID,
    NODE_ID, ts,
    hrBpm, rrMs,
    wearableOk ? "true" : "false",
    (double)stressIndex,
    activity, (double)motionLevel,
    incidentActive ? "true" : "false", incidentType, (unsigned long)incidentId, incidentSeverity, incidentTtlSec
  );

  mqtt.publish(TOPIC_BIO.c_str(), bio);
}

// Publish JSON Gateway
static void publishGw(uint32_t nowMs) {
  if (!mqtt.connected()) return;

  ensureTimeUtc();
  char ts[32];
  iso8601UtcNow(ts, sizeof(ts));

  bool uplink_ok = (WiFi.status() == WL_CONNECTED && mqtt.connected());

  char gw[520];
  snprintf(gw, sizeof(gw),
    "{"
      "\"nodeId\":\"%s\",\"observedAt\":\"%s\","
      "\"uplinkReal\":%s,\"uplinkEffective\":%s,"
      "\"wifiRssiDbm\":%d,"
      "\"bleOk\":%s,\"ecgOn\":%s,"
      "\"ecgPkts\":0,\"ecgDrop\":0"
    "}",
    NODE_ID, ts,
    uplink_ok ? "true" : "false",
    uplink_ok ? "true" : "false",
    (int)wifiRssiDbmOrNeg127(),
    wearableOk ? "true" : "false",
    replayActive ? "true" : "false"
  );

  mqtt.publish(TOPIC_GW.c_str(), gw);
}

// Setup / loop
void setup() {
  Serial.begin(115200);
  delay(100);

  randomSeed((uint32_t)ESP.getEfuseMac() ^ (uint32_t)micros());

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.onEvent(onWiFiEvent);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);

  incidentId = (uint32_t)random(100000, 999999);
  demoT0 = millis();

  startNextWifiAttempt();
}

void loop() {
  ensureWiFi();
  ensureMQTT();
  mqtt.loop();

  if (WiFi.status() != WL_CONNECTED || !mqtt.connected()) {
    delay(2);
    return;
  }

  uint32_t nowMs = millis();
  updateKinematics(nowMs);

  float tempC, humPct, coPpm;
  int mq2Raw, hrBpm, rrMs;
  synthDemoSignals(nowMs, tempC, humPct, mq2Raw, coPpm, hrBpm, rrMs);

  uint32_t tms = nowMs - demoT0;

  if (tms >= 90000 && tms < 210000) {
    if (!replayActive) startEcgReplay(nowMs, 120000);
  } else {
    if (replayActive && tms >= 210000) stopEcgReplay();
  }

  if (nowMs - lastBio >= BIO_INTERVAL_MS) {
    lastBio = nowMs;
    publishBio(nowMs, hrBpm, rrMs);
  }

  if (nowMs - lastEnv >= ENV_INTERVAL_MS) {
    lastEnv = nowMs;
    publishEnv(nowMs, tempC, humPct, mq2Raw, coPpm, hrBpm);
  }

  if (nowMs - lastGw >= GW_INTERVAL_MS) {
    lastGw = nowMs;
    publishGw(nowMs);
  }

  if (replayActive && (nowMs - lastEcg >= ECG_REPLAY_PUB_MS)) {
    lastEcg = nowMs;
    publishReplayBundle(nowMs, hrBpm, stressIndex);
  }

  delay(2);
}
