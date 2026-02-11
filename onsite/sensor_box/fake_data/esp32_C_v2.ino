#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include <math.h>

#include "ecg_segment_ff_c.h"

// =========================================================
// IDENTITY
// =========================================================
static constexpr char NODE_ID_CHAR = 'C';
static const char*    NODE_ID      = "C";
static const char*    TEAM_ID      = "Team_A";
static const char*    FIREFIGHTER_ID = "FF_C";

// =========================================================
// Wi-Fi credentials rotation
// =========================================================
struct WifiCred { const char* ssid; const char* pass; };
static WifiCred WIFI_LIST[] = {
  { "VODAFONE_H268Q-4057", "SFzDE5ZxHyQPQ2FQ" },
  { "FirefighterA",        "firefighterA"     },
};
static constexpr int WIFI_LIST_N = sizeof(WIFI_LIST) / sizeof(WIFI_LIST[0]);

// =========================================================
// MQTT
// =========================================================
static const char* MQTT_HOST = "192.168.2.13";
static const int   MQTT_PORT = 1883;

// Topics
static String TOPIC_ENV = String("ngsi/Environment/") + TEAM_ID + "/" + FIREFIGHTER_ID;
static String TOPIC_BIO = String("ngsi/Biomedical/")  + TEAM_ID + "/" + FIREFIGHTER_ID;
static String TOPIC_GW  = String("ngsi/Gateway/")     + NODE_ID;
static String TOPIC_ECG = String("raw/ECG/")          + TEAM_ID + "/" + FIREFIGHTER_ID;

// =========================================================
// MULTI-RATE PUBLISHING
// =========================================================
static const uint32_t ENV_INTERVAL_MS = 2000;
static const uint32_t BIO_INTERVAL_MS = 1000;
static const uint32_t GW_INTERVAL_MS  = 5000;

// =========================================================
// DEMO CYCLE (REPEATING) + CRISIS WINDOW
// =========================================================
// Keep crisis starting at ~90s like your old code, but longer.
static const uint32_t PHASE_NORMAL_MS  = 60000;   // 0..60s
static const uint32_t PHASE_WARN_MS    = 30000;   // 60..90s
static const uint32_t PHASE_DANGER_MS  = 240000;  // 90..330s  (4 minutes crisis)
static const uint32_t PHASE_RECOVER_MS = 90000;   // 330..420s (1.5 min recovery)

static const uint32_t DEMO_CYCLE_MS =
  PHASE_NORMAL_MS + PHASE_WARN_MS + PHASE_DANGER_MS + PHASE_RECOVER_MS;

// =========================================================
// ECG REPLAY (FROM PHYSIONET SEGMENT) - ONLY DURING CRISIS
// =========================================================
static const uint32_t ECG_REPLAY_PUB_MS = 50;
static const uint16_t ECG_REPLAY_SAMPLES_PER_BUNDLE = 20;

static bool     replayActive = false;
static uint32_t replayEndMs  = 0;
static uint32_t replayIdx    = 0;

// =========================================================
// CONNECTIVITY TIMING
// =========================================================
static const uint32_t MQTT_RETRY_MS          = 2000;
static const uint32_t WIFI_ATTEMPT_WINDOW_MS = 15000;

// =========================================================
// GLOBALS
// =========================================================
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
static uint32_t lastTms = 0;

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

// HR realism state (no obvious oscillation in normal)
static float hrSmoothed = 92.0f;
static float hrDrift = 0.0f;
static uint32_t lastSynthMs = 0;

// =========================================================
// MATH HELPERS
// =========================================================
static float frand(float a, float b) { return a + (b - a) * (float)random(0, 10000) / 10000.0f; }
static float clampf(float x, float lo, float hi) { return (x < lo) ? lo : (x > hi) ? hi : x; }
static int   clampi(int x, int lo, int hi) { return (x < lo) ? lo : (x > hi) ? hi : x; }

static float smoothstep(float x) {
  x = clampf(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}
static float alphaFromTau(float dtSec, float tauSec) {
  if (tauSec <= 0.0001f) return 1.0f;
  float a = 1.0f - expf(-dtSec / tauSec);
  return clampf(a, 0.0f, 1.0f);
}

static inline int16_t wifiRssiDbmOrNeg127() {
  return (WiFi.status() == WL_CONNECTED) ? (int16_t)WiFi.RSSI() : (int16_t)-127;
}

static inline uint32_t demoTms(uint32_t nowMs) {
  return (nowMs - demoT0) % DEMO_CYCLE_MS;
}

// =========================================================
// UTC TIME HELPERS
// =========================================================
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

// =========================================================
// Wi-Fi EVENT HANDLER
// =========================================================
static void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:        wifiConnecting = true; break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:       wifiGotIp = true; wifiConnecting = false; break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: wifiGotIp = false; wifiConnecting = false; break;
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

// =========================================================
// MQTT RECONNECT
// =========================================================
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

// =========================================================
// ECG REPLAY CONTROL (segment loops while active)
// =========================================================
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

  // USE THE PHYSIONET SEGMENT SAMPLES HERE
  for (uint16_t i = 0; i < n; i++) {
    int16_t s = ECG_SEGMENT_DATA[replayIdx++];
    if (replayIdx >= ECG_SEGMENT_LEN) replayIdx = 0; // loop segment to make crisis longer
    buf[p++] = (uint8_t)(s & 0xFF);
    buf[p++] = (uint8_t)((s >> 8) & 0xFF);
  }

  mqtt.publish(TOPIC_ECG.c_str(), buf, p);
}

// =========================================================
// KINEMATICS (slow changes)
// =========================================================
static void updateKinematics(uint32_t nowMs) {
  float t = nowMs / 1000.0f;
  float base = 0.45f + 0.10f * sinf(t * 0.02f + 0.8f);
  motionLevel = clampf(base + frand(-0.05f, 0.05f), 0.05f, 1.0f);

  if (motionLevel < 0.22f) activity = "still";
  else if (motionLevel < 0.62f) activity = "walk";
  else activity = "run";

  if ((nowMs % 7000) < 20) {
    int drop = (int)random(0, 2);
    batteryPct = clampi(batteryPct - drop, 20, 100);
  }
}

// =========================================================
// DEMO SIGNAL SYNTHESIS
// (key idea: HR is stable in normal, and forced tachy in danger
// so it matches "tachy segment playing")
// =========================================================
static void synthDemoSignals(uint32_t nowMs,
                             float& outTempC, float& outHumPct, int& outMq2Raw, float& outCoPpm,
                             int& outHrBpm, int& outRrMs) {
  uint32_t tms = demoTms(nowMs);

  const uint32_t T1 = PHASE_NORMAL_MS;
  const uint32_t T2 = PHASE_NORMAL_MS + PHASE_WARN_MS;
  const uint32_t T3 = PHASE_NORMAL_MS + PHASE_WARN_MS + PHASE_DANGER_MS;
  const uint32_t T4 = DEMO_CYCLE_MS;

  float worsen = 0.0f;
  float danger = 0.0f;
  float recover = 0.0f;

  if (tms < T1) {
    worsen = 0.0f; danger = 0.0f; recover = 0.0f;
  } else if (tms < T2) {
    worsen = smoothstep((tms - T1) / (float)(T2 - T1));
    danger = 0.0f; recover = 0.0f;
  } else if (tms < T3) {
    worsen = 1.0f;
    danger = smoothstep((tms - T2) / (float)(T3 - T2));
    recover = 0.0f;
  } else {
    worsen = 1.0f;
    danger = 1.0f;
    recover = smoothstep((tms - T3) / (float)(T4 - T3));
  }

  // Environment (gentle)
  float t = (float)tms / 1000.0f;
  float baseTemp = 37.5f + 0.20f * sinf(t * 0.008f + 0.8f);
  float baseHum  = 35.0f + 0.45f * sinf(t * 0.006f + 1.1f);
  float baseCo   = 4.0f  + 0.20f * sinf(t * 0.007f + 0.3f);
  float baseMq2  = 850.0f + 18.0f * sinf(t * 0.006f + 0.2f);

  float temp = baseTemp + 2.0f * worsen + 10.0f * danger - 7.0f * recover;
  float hum  = baseHum  - 0.5f * worsen - 3.0f  * danger + 2.0f  * recover;
  float mq2  = baseMq2 + 650.0f * worsen + 1800.0f * danger - 1500.0f * recover;
  float co   = baseCo  + 8.0f   * worsen + 120.0f  * danger - 100.0f  * recover;

  temp += frand(-0.12f, 0.12f);
  hum  += frand(-0.30f, 0.30f);
  mq2  += frand(-20.0f, 20.0f);
  co   += frand(-0.6f, 0.6f);

  temp = clampf(temp, 25.0f, 90.0f);
  hum  = clampf(hum,  8.0f,  90.0f);
  mq2  = clampf(mq2,  200.0f, 4095.0f);
  co   = clampf(co,   0.0f,  260.0f);

  // Stress
  float stress = 0.12f + 0.25f * worsen + 0.80f * danger - 0.55f * recover;
  stress += 0.10f * (motionLevel > 0.65f ? 1.0f : 0.0f);
  stressIndex = clampf(stress + frand(-0.03f, 0.03f), 0.0f, 1.0f);

  // ---- HR MODEL ----
  // Stable in normal (slow drift), ramps to tachy during danger (so Grafana shows crisis).
  uint32_t prev = lastSynthMs;
  lastSynthMs = nowMs;
  float dtSec = (prev == 0) ? 0.05f : (float)(nowMs - prev) / 1000.0f;
  dtSec = clampf(dtSec, 0.01f, 0.25f);

  // slow drift update every ~2s
  if ((nowMs % 2000) < 30) {
    hrDrift += frand(-0.35f, 0.35f);
    hrDrift = clampf(hrDrift, -4.0f, 5.0f);
  }

  float hrRest = 92.0f + hrDrift;

  // Force tachy-like HR when danger is active (matches tachy segment playing)
  // Tune here if you want higher peaks:
  float hrTachyTarget = 185.0f + 18.0f * danger + frand(-3.0f, 3.0f); // ~185..206

  // Blend: normal -> warn -> danger -> recovery
  float hrTarget = hrRest;
  hrTarget += 10.0f * worsen; // warn ramp
  hrTarget = (danger > 0.001f) ? (hrRest * (1.0f - danger) + hrTachyTarget * danger) : hrTarget;

  // during recovery, return toward rest smoothly
  if (tms >= T3) {
    hrTarget = hrTachyTarget * (1.0f - recover) + hrRest * recover;
  }

  hrTarget = clampf(hrTarget, 60.0f, 210.0f);

  // smoothing (fast in danger, slow in normal)
  float tau = 10.0f - 7.0f * danger; // normal ~10s, danger ~3s
  float a = alphaFromTau(dtSec, tau);
  hrSmoothed += a * (hrTarget - hrSmoothed);

  float hr = clampf(hrSmoothed, 60.0f, 210.0f);

  // RR from HR (less variability when stressed)
  float rr = 60000.0f / clampf(hr, 55.0f, 200.0f);
  float hrvJitter = (1.0f - stressIndex) * frand(-12.0f, 12.0f) + frand(-4.0f, 4.0f);
  rr = clampf(rr + hrvJitter, 260.0f, 1300.0f);

  outTempC = temp;
  outHumPct = hum;
  outMq2Raw = (int)lround(mq2);
  outCoPpm = co;
  outHrBpm = (int)lround(hr);
  outRrMs = (int)lround(rr);

  // Incident state
  if (tms < T1) {
    incidentActive = false;
    incidentType = "none";
    incidentSeverity = "info";
    incidentTtlSec = 0;
  } else if (tms < T2) {
    incidentActive = true;
    incidentType = "smoke_co";
    incidentSeverity = "warn";
    incidentTtlSec = (int)((T2 - tms) / 1000);
  } else if (tms < T3) {
    incidentActive = true;
    incidentType = "smoke_co";
    incidentSeverity = "danger";
    incidentTtlSec = (int)((T3 - tms) / 1000);
  } else {
    incidentActive = true;
    incidentType = "recovery";
    incidentSeverity = "info";
    incidentTtlSec = (int)((T4 - tms) / 1000);
  }
}

// =========================================================
// PUBLISHERS
// =========================================================
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

// =========================================================
// SETUP / LOOP
// =========================================================
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

  // init HR
  hrSmoothed = 92.0f + frand(-2.0f, 2.0f);
  hrDrift = frand(-1.0f, 1.0f);
  lastSynthMs = 0;

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
  uint32_t tms = demoTms(nowMs);

  // cycle wrap => new incident id + clean ECG state
  if (tms < lastTms) {
    stopEcgReplay();
    replayIdx = 0;
    incidentId = (uint32_t)random(100000, 999999);
    hrDrift = frand(-1.0f, 1.0f);
  }
  lastTms = tms;

  updateKinematics(nowMs);

  float tempC, humPct, coPpm;
  int mq2Raw, hrBpm, rrMs;
  synthDemoSignals(nowMs, tempC, humPct, mq2Raw, coPpm, hrBpm, rrMs);

  // Crisis window: start at 90s, lasts PHASE_DANGER_MS
  const uint32_t dangerStart = PHASE_NORMAL_MS + PHASE_WARN_MS;
  const uint32_t dangerEnd   = dangerStart + PHASE_DANGER_MS;

  // START/STOP RAW ECG (segment) ONLY DURING CRISIS
  if (tms >= dangerStart && tms < dangerEnd) {
    if (!replayActive) {
      uint32_t remaining = dangerEnd - tms;
      startEcgReplay(nowMs, remaining);
    }
  } else {
    if (replayActive) stopEcgReplay();
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
