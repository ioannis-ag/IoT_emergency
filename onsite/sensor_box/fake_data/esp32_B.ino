#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include <math.h>

// ---------------- Identity ----------------
static constexpr char NODE_ID_CHAR = 'B';
static const char*    NODE_ID      = "B";
static const char*    TEAM_ID      = "Team_A";
static const char*    FIREFIGHTER_ID = "FF_B";

// ---------------- Wi-Fi credentials rotation ----------------
struct WifiCred { const char* ssid; const char* pass; };
static WifiCred WIFI_LIST[] = {
  { "VODAFONE_H268Q-4057", "SFzDE5ZxHyQPQ2FQ" },
  { "FirefighterA",        "firefighterA"     },
};
static constexpr int WIFI_LIST_N = sizeof(WIFI_LIST) / sizeof(WIFI_LIST[0]);

// ---------------- MQTT ----------------
static const char* MQTT_HOST = "192.168.2.13";
static const int   MQTT_PORT = 1883;

// ---------------- Topics (match edge controller patterns) ----------------
static String TOPIC_ENV = String("ngsi/Environment/") + TEAM_ID + "/" + FIREFIGHTER_ID;
static String TOPIC_BIO = String("ngsi/Biomedical/")  + TEAM_ID + "/" + FIREFIGHTER_ID;
static String TOPIC_GW  = String("ngsi/Gateway/")     + NODE_ID;
static String TOPIC_ECG = String("raw/ECG/")          + TEAM_ID + "/" + FIREFIGHTER_ID;


// ---------------- Multi-rate publishing ----------------
static const uint32_t ENV_INTERVAL_MS = 2000;
static const uint32_t BIO_INTERVAL_MS = 1000;
static const uint32_t GW_INTERVAL_MS  = 5000;

// ---------------- Synthetic ECG (binary) ----------------
static const bool     ENABLE_FAKE_ECG      = true;
static const uint32_t ECG_PUB_INTERVAL_MS  = 154;     // publish small bundles frequently
static const int      ECG_HZ               = 130;    // conceptual sampling rate for waveform generation
static const int      ECG_SAMPLES_PER_BUNDLE = 20;   // samples packed into each MQTT message (keeps payload small)

// ---------------- Connectivity timing ----------------
static const uint32_t MQTT_RETRY_MS           = 2000;
static const uint32_t WIFI_ATTEMPT_WINDOW_MS  = 15000;

// ---------------- Globals ----------------
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

// ---------------- Time (UTC via NTP) ----------------
static bool timeInited = false;
static uint32_t lastTimeTry = 0;

// ---------------- Random utilities ----------------
static float frand(float a, float b) {
  return a + (b - a) * (float)random(0, 10000) / 10000.0f;
}
static float clampf(float x, float lo, float hi) { return (x < lo) ? lo : (x > hi) ? hi : x; }
static int   clampi(int x, int lo, int hi) { return (x < lo) ? lo : (x > hi) ? hi : x; }

static inline int16_t wifiRssiDbmOrNeg127() {
  return (WiFi.status() == WL_CONNECTED) ? (int16_t)WiFi.RSSI() : (int16_t)-127;
}

// ---------------- UTC ISO timestamp ----------------
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

// ---------------- Wi-Fi event-driven state ----------------
static void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:      wifiConnecting = true; break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:     wifiGotIp = true; wifiConnecting = false; break;
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

// ---------------- MQTT reconnect ----------------
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

// ============================================================
// Realistic physiology + environment model  (MUST be above any use)
// ============================================================

enum IncidentType : uint8_t {
  INC_NONE = 0,
  INC_SMOKE_CO = 1,
  INC_HEAT = 2,
  INC_TACHY = 3,
  INC_WEARABLE_DROP = 4,
  INC_RADIO_SILENCE = 5
};

struct Incident {
  bool active = false;
  IncidentType type = INC_NONE;
  uint32_t startMs = 0;
  uint32_t endMs = 0;
  uint32_t rampMs = 0;
  float severity = 0.0f;   // 0..1
  uint32_t id = 0;
};

static Incident gInc;

// ---- Forward declarations (prevents Arduino from generating broken prototypes)
static float incidentEnvelope(const Incident& inc, uint32_t nowMs);
static const char* incidentTypeToStr(IncidentType t);
static const char* incidentSeverityToStr(float s);
static void endIncident();
static IncidentType pickIncidentType();
static void scheduleNextIncident(uint32_t nowMs);
static void maybeStartIncident(uint32_t nowMs);
static void updateIncident(uint32_t nowMs);

// Baselines (tuned for active firefighter conditions).
static float baseTempC = 34.0f;
static float baseHumPct = 35.0f;
static float baseCoPpm = 4.0f;
static int   baseMq2Raw = 900;
static float baseHrBpm = 108.0f;

// Random-walk drift terms (slow sensor drift / environment variability).
static float rwTemp = 0.0f, rwHum = 0.0f, rwCo = 0.0f, rwMq2 = 0.0f, rwHr = 0.0f;

// Internal state that creates correlation and lag.
static float exertion = 0.35f;        // 0..1 (affects HR, temp, respiration)
static float exertionTarget = 0.35f;  // slowly approached by exertion
static float stressIndex = 0.25f;     // 0..1 (computed for backend)
static bool  wearableOk = true;

// Extra backend-friendly signals.
static int   batteryPct = 94;
static float motionLevel = 0.45f;     // 0..1
static const char* activity = "walk"; // walk/run/still

// Scheduling: next incident starts after a random gap, not periodic.
static uint32_t nextIncidentAtMs = 0;

// Offline simulation separate from incident model to produce staleness alerts.
static bool radioSilenceActive = false;
static uint32_t radioSilenceUntilMs = 0;

// A small helper to update random walk smoothly.
static void rwStep(float& v, float step, float limitAbs) {
  v += frand(-step, step);
  v = clampf(v, -limitAbs, limitAbs);
}

// A smooth 0..1 envelope for an incident with ramp up/down.
static float incidentEnvelope(const Incident& inc, uint32_t nowMs) {
  if (!inc.active) return 0.0f;
  if (nowMs <= inc.startMs) return 0.0f;
  if (nowMs >= inc.endMs) return 0.0f;

  uint32_t ramp = inc.rampMs;
  if (ramp < 500) ramp = 500;

  uint32_t t = nowMs - inc.startMs;
  uint32_t dur = inc.endMs - inc.startMs;

  if (t < ramp) {
    return (float)t / (float)ramp;
  }
  if (t > dur - ramp) {
    uint32_t td = (inc.endMs - nowMs);
    return (float)td / (float)ramp;
  }
  return 1.0f;
}

static const char* incidentTypeToStr(IncidentType t) {
  switch (t) {
    case INC_SMOKE_CO:       return "smoke_co";
    case INC_HEAT:           return "heat";
    case INC_TACHY:          return "tachy";
    case INC_WEARABLE_DROP:  return "wearable_drop";
    case INC_RADIO_SILENCE:  return "radio_silence";
    default:                 return "none";
  }
}

static const char* incidentSeverityToStr(float s) {
  if (s >= 0.72f) return "danger";
  if (s >= 0.42f) return "warn";
  return "info";
}

static void endIncident() {
  gInc.active = false;
  gInc.type = INC_NONE;
  gInc.startMs = gInc.endMs = gInc.rampMs = 0;
  gInc.severity = 0.0f;
}

static IncidentType pickIncidentType() {
  float r = frand(0.0f, 1.0f);
  if (r < 0.36f) return INC_SMOKE_CO;
  if (r < 0.62f) return INC_HEAT;
  if (r < 0.80f) return INC_TACHY;
  if (r < 0.92f) return INC_WEARABLE_DROP;
  return INC_RADIO_SILENCE;
}

static void scheduleNextIncident(uint32_t nowMs) {
  uint32_t gapMs = (uint32_t)random(45000, 210000); // 45s..210s between incidents
  nextIncidentAtMs = nowMs + gapMs;
}

static void maybeStartIncident(uint32_t nowMs) {
  if (gInc.active) return;
  if (nextIncidentAtMs == 0) scheduleNextIncident(nowMs);
  if ((int32_t)(nowMs - nextIncidentAtMs) < 0) return;

  IncidentType t = pickIncidentType();
  uint32_t durMs = (uint32_t)random(90000, 150000); // 1.5..2.5 minutes (center ~2 min)
  uint32_t rampMs = (uint32_t)random(8000, 20000);  // 8..20s ramp feels realistic
  float sev = frand(0.35f, 1.0f);

  gInc.active = true;
  gInc.type = t;
  gInc.startMs = nowMs;
  gInc.endMs = nowMs + durMs;
  gInc.rampMs = rampMs;
  gInc.severity = sev;
  gInc.id = (uint32_t)random(100000, 999999);

  scheduleNextIncident(nowMs);
}

static void updateIncident(uint32_t nowMs) {
  if (gInc.active && (int32_t)(nowMs - gInc.endMs) >= 0) {
    endIncident();
  }

  wearableOk = true;

  if (gInc.active) {
    if (gInc.type == INC_WEARABLE_DROP) wearableOk = false;
    if (gInc.type == INC_RADIO_SILENCE) {
      radioSilenceActive = true;
      uint32_t extra = (uint32_t)random(20000, 60000);
      radioSilenceUntilMs = nowMs + extra;
      endIncident();
    }
  }

  if (radioSilenceActive && (int32_t)(nowMs - radioSilenceUntilMs) >= 0) {
    radioSilenceActive = false;
  }
}

static void updateActivity(uint32_t nowMs) {
  float t = nowMs / 1000.0f;

  float motionBase = 0.45f + 0.18f * sinf(t * 0.05f + 0.8f);
  motionLevel = clampf(motionBase + frand(-0.06f, 0.06f), 0.05f, 1.0f);

  if (motionLevel < 0.22f) activity = "still";
  else if (motionLevel < 0.62f) activity = "walk";
  else activity = "run";

  if ((nowMs % 7000) < 20) {
    int drop = (int)random(0, 2);
    batteryPct = clampi(batteryPct - drop, 20, 100);
  }
}

static void updateExertionAndStress(uint32_t nowMs) {
  float envLoad = 0.0f;
  float e = incidentEnvelope(gInc, nowMs);

  if (gInc.active) {
    if (gInc.type == INC_HEAT)      envLoad += 0.55f * gInc.severity * e;
    if (gInc.type == INC_SMOKE_CO)  envLoad += 0.45f * gInc.severity * e;
    if (gInc.type == INC_TACHY)     envLoad += 0.35f * gInc.severity * e;
  }

  float motionLoad = 0.15f + 0.55f * motionLevel;
  exertionTarget = clampf(0.15f + motionLoad + envLoad, 0.05f, 1.0f);

  float dt = 1.0f / 30.0f;
  float tau = 10.0f;
  float alpha = dt / (tau + dt);
  exertion = exertion + alpha * (exertionTarget - exertion);

  float s = 0.12f + 0.75f * exertion;
  if (!wearableOk) s += 0.10f;
  stressIndex = clampf(s + frand(-0.03f, 0.03f), 0.0f, 1.0f);
}

static void synthSignals(uint32_t nowMs,
                         float& outTempC, float& outHumPct, int& outMq2Raw, float& outCoPpm,
                         int& outHrBpm, int& outRrMs) {
  float t = nowMs / 1000.0f;

  rwStep(rwTemp, 0.03f, 2.5f);
  rwStep(rwHum,  0.10f, 10.0f);
  rwStep(rwCo,   0.20f, 10.0f);
  rwStep(rwMq2,  18.0f, 700.0f);
  rwStep(rwHr,   0.45f, 22.0f);

  float temp = baseTempC + rwTemp + 0.8f * sinf(t * 0.06f);
  float hum  = baseHumPct + rwHum + 2.8f * sinf(t * 0.05f + 1.1f);
  float co   = baseCoPpm + rwCo + 0.8f * sinf(t * 0.08f + 0.4f);
  float mq2  = (float)baseMq2Raw + rwMq2 + 55.0f * sinf(t * 0.055f + 0.2f);

  float e = incidentEnvelope(gInc, nowMs);
  if (gInc.active) {
    if (gInc.type == INC_HEAT) {
      temp += 28.0f * gInc.severity * e;
      hum  -= 6.0f  * gInc.severity * e;
    }
    if (gInc.type == INC_SMOKE_CO) {
      mq2  += 2000.0f * gInc.severity * e;
      co   += 135.0f  * gInc.severity * e;
    }
    if (gInc.type == INC_TACHY) {
      // tachy is applied mainly via exertion/HR, but add slight environmental co-variation
      co += 8.0f * gInc.severity * e;
    }
  }

  float hr = baseHrBpm + rwHr;
  hr += 75.0f * exertion;
  hr += 7.0f  * sinf(t * 0.22f + 0.9f);

  if (gInc.active && gInc.type == INC_TACHY) {
    hr += 85.0f * gInc.severity * e;
  }

  temp = clampf(temp, 18.0f, 95.0f);
  hum  = clampf(hum,  8.0f,  95.0f);
  co   = clampf(co,   0.0f,  260.0f);
  mq2  = clampf(mq2,  200.0f, 4095.0f);
  hr   = clampf(hr,   45.0f,  210.0f);

  temp += frand(-0.12f, 0.12f);
  hum  += frand(-0.30f, 0.30f);
  co   += frand(-0.7f,  0.7f);
  hr   += frand(-1.2f,  1.2f);

  float rrMsF = 60000.0f / clampf(hr, 55.0f, 200.0f);
  float hrvJitter = (1.0f - stressIndex) * frand(-18.0f, 18.0f) + frand(-6.0f, 6.0f);
  rrMsF = clampf(rrMsF + hrvJitter, 260.0f, 1300.0f);

  outTempC = temp;
  outHumPct = hum;
  outCoPpm = co;
  outMq2Raw = (int)lround(mq2);
  outHrBpm  = (int)lround(hr);
  outRrMs   = (int)lround(rrMsF);
}

// ============================================================
// Synthetic ECG waveform generator (binary payload)
// ============================================================

// Simple ECG-like waveform: baseline + P/QRS/T components + noise.
// This is not clinical-grade; it produces realistic-looking variability for demos and backend load.
static uint32_t ecgSampleIndex = 0;
static float ecgPhase = 0.0f;

static int16_t synthEcgSample(uint32_t nowMs, int hrBpm, float stress) {
  float hz = (float)ECG_HZ;
  float dt = 1.0f / hz;

  float bpm = clampf((float)hrBpm, 55.0f, 190.0f);
  float beatHz = bpm / 60.0f;

  ecgPhase += beatHz * dt;
  if (ecgPhase >= 1.0f) ecgPhase -= 1.0f;

  float x = ecgPhase;

  float p  = 0.12f * expf(-powf((x - 0.18f) / 0.035f, 2.0f));
  float q  = -0.25f * expf(-powf((x - 0.285f) / 0.010f, 2.0f));
  float r  = 1.15f * expf(-powf((x - 0.300f) / 0.008f, 2.0f));
  float s  = -0.35f * expf(-powf((x - 0.318f) / 0.012f, 2.0f));
  float tw = 0.35f * expf(-powf((x - 0.55f) / 0.060f, 2.0f));

  float baseline = 0.03f * sinf((float)nowMs * 0.002f) + 0.02f * sinf((float)nowMs * 0.0007f);
  float noise = frand(-0.03f, 0.03f) + stress * frand(-0.02f, 0.05f);

  float y = (p + q + r + s + tw) + baseline + noise;

  float gain = 1200.0f;
  float v = y * gain;
  v = clampf(v, -1800.0f, 1800.0f);
  return (int16_t)lround(v);
}

static void publishFakeEcg(uint32_t nowMs, int hrBpm, float stress) {
  if (!ENABLE_FAKE_ECG) return;
  if (!mqtt.connected()) return;

  // Payload format:
  // [4] "ECG1"
  // [4] uint32 millis
  // [2] uint16 sampleCount
  // [2] uint16 hrBpm
  // [1] uint8  stress*255
  // [N*2] int16 samples little-endian
  const uint16_t n = ECG_SAMPLES_PER_BUNDLE;
  const uint16_t maxBytes = 4 + 4 + 2 + 2 + 1 + (n * 2);
  uint8_t buf[maxBytes];
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
    int16_t s = synthEcgSample(nowMs, hrBpm, stress);
    buf[p++] = (uint8_t)(s & 0xFF);
    buf[p++] = (uint8_t)((s >> 8) & 0xFF);
    ecgSampleIndex++;
  }

  mqtt.publish(TOPIC_ECG.c_str(), buf, p);
}

// ============================================================
// Publishing (JSON)
// ============================================================

static void publishEnv(uint32_t nowMs,
                       float tempC, float humPct, int mq2Raw, float coPpm,
                       int hrBpm) {
  if (!mqtt.connected()) return;

  ensureTimeUtc();
  char ts[32];
  iso8601UtcNow(ts, sizeof(ts));

  bool incidentActive = gInc.active;
  float envE = incidentEnvelope(gInc, nowMs);
  int ttlSec = 0;
  if (incidentActive) ttlSec = (int)((gInc.endMs - nowMs) / 1000);

  char tbuf[16], hbuf[16], cobuf[16];
  dtostrf(tempC, 0, 2, tbuf);
  dtostrf(humPct, 0, 2, hbuf);
  dtostrf(coPpm,  0, 1, cobuf);

  const char* itype = incidentActive ? incidentTypeToStr(gInc.type) : "none";
  const char* isev  = incidentActive ? incidentSeverityToStr(gInc.severity) : "info";

  int mq2Dig = (mq2Raw > 1800) ? 1 : 0;

  char env[820];
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
    incidentActive ? "true" : "false", itype, (unsigned long)(incidentActive ? gInc.id : 0UL), isev, ttlSec,
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

  bool incidentActive = gInc.active;
  int ttlSec = 0;
  if (incidentActive) ttlSec = (int)((gInc.endMs - nowMs) / 1000);

  const char* itype = incidentActive ? incidentTypeToStr(gInc.type) : "none";
  const char* isev  = incidentActive ? incidentSeverityToStr(gInc.severity) : "info";

  char bio[640];
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
    incidentActive ? "true" : "false", itype, (unsigned long)(incidentActive ? gInc.id : 0UL), isev, ttlSec
  );

  mqtt.publish(TOPIC_BIO.c_str(), bio);
}

static void publishGw(uint32_t nowMs) {
  if (!mqtt.connected()) return;

  ensureTimeUtc();
  char ts[32];
  iso8601UtcNow(ts, sizeof(ts));

  bool uplink_ok = (WiFi.status() == WL_CONNECTED && mqtt.connected());

  char gw[420];
  snprintf(gw, sizeof(gw),
    "{"
      "\"nodeId\":\"%s\",\"observedAt\":\"%s\","
      "\"uplinkReal\":%s,\"uplinkEffective\":%s,"
      "\"wifiRssiDbm\":%d,"
      "\"bleOk\":%s,\"ecgOn\":%s,"
      "\"ecgPkts\":%u,\"ecgDrop\":%u,"
      "\"radioSilence\":%s"
    "}",
    NODE_ID, ts,
    uplink_ok ? "true" : "false",
    uplink_ok ? "true" : "false",
    (int)wifiRssiDbmOrNeg127(),
    wearableOk ? "true" : "false",
    (ENABLE_FAKE_ECG ? "true" : "false"),
    0u, 0u,
    radioSilenceActive ? "true" : "false"
  );

  mqtt.publish(TOPIC_GW.c_str(), gw);
}

// ============================================================
// Setup / loop
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(100);

  randomSeed((uint32_t)ESP.getEfuseMac() ^ (uint32_t)micros());

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.onEvent(onWiFiEvent);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);

  scheduleNextIncident(millis());
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

  maybeStartIncident(nowMs);
  updateIncident(nowMs);
  updateActivity(nowMs);
  updateExertionAndStress(nowMs);

  float tempC, humPct, coPpm;
  int mq2Raw, hrBpm, rrMs;
  synthSignals(nowMs, tempC, humPct, mq2Raw, coPpm, hrBpm, rrMs);

  if (radioSilenceActive) {
    delay(2);
    return;
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

  if (ENABLE_FAKE_ECG && (nowMs - lastEcg >= ECG_PUB_INTERVAL_MS)) {
    lastEcg = nowMs;
    publishFakeEcg(nowMs, hrBpm, stressIndex);
  }

  delay(2);
}
