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
static const uint32_t PHASE_NORMAL_MS  = 60000;   // 0..60s
static const uint32_t PHASE_WARN_MS    = 30000;   // 60..90s
static const uint32_t PHASE_DANGER_MS  = 240000;  // 90..330s
static const uint32_t PHASE_RECOVER_MS = 90000;   // 330..420s

static const uint32_t DEMO_CYCLE_MS =
  PHASE_NORMAL_MS + PHASE_WARN_MS + PHASE_DANGER_MS + PHASE_RECOVER_MS;

// =========================================================
// ECG PLAYBACK + BPM/RR FROM SEGMENT
// =========================================================
// IMPORTANT: set this to the true sampling rate of your segment.
// Common: 250 or 360. If wrong => BPM/RR wrong.
static const uint16_t ECG_FS_HZ = 360;

// How many samples per MQTT payload
static const uint16_t ECG_SAMPLES_PER_BUNDLE = 20;

// If true: always stream ECG (loop the segment forever)
// If false: stream only during danger window
static const bool STREAM_ECG_ALWAYS = false;

// Derived bundle interval to match ECG_FS_HZ
static uint32_t ecgBundleIntervalMs() {
  // round(1000 * N / fs)
  return (uint32_t)((1000UL * (uint32_t)ECG_SAMPLES_PER_BUNDLE + (ECG_FS_HZ / 2)) / ECG_FS_HZ);
}

static bool     ecgActive   = false;
static uint32_t ecgIdx      = 0;       // index into ECG_SEGMENT_DATA
static uint32_t ecgSampleNo = 0;       // global playback sample counter
static uint32_t lastEcgPub  = 0;

static uint32_t ecgPkts = 0;

// --- Simple R-peak detector state (lightweight demo-grade) ---
struct RPeakDetector {
  float mean = 0.0f;     // baseline estimate
  float env  = 0.0f;     // envelope of |signal|
  bool  inPeak = false;
  float peakMax = 0.0f;
  uint32_t peakMaxIdx = 0;

  bool hasLast = false;
  uint32_t lastRIdx = 0;

  int rrMs = 0;
  int hrBpm = 0;
  float hrSmooth = 90.0f;
};

static RPeakDetector rdet;

static float clampf(float x, float lo, float hi) { return (x < lo) ? lo : (x > hi) ? hi : x; }
static int   clampi(int x, int lo, int hi) { return (x < lo) ? lo : (x > hi) ? hi : x; }
static float frand(float a, float b) { return a + (b - a) * (float)random(0, 10000) / 10000.0f; }

static void rpeakFeedSample(int16_t s) {
  // Baseline removal
  const float aMean = 0.01f;
  rdet.mean += aMean * ((float)s - rdet.mean);
  float x = (float)s - rdet.mean;

  // Envelope of absolute signal
  float ax = fabsf(x);
  const float aEnv = 0.05f;
  rdet.env += aEnv * (ax - rdet.env);

  // Adaptive threshold (add small constant to avoid too-low threshold)
  float thr = rdet.env * 1.6f + 50.0f;

  // Refractory period ~220ms
  uint32_t refractorySamples = (uint32_t)((ECG_FS_HZ * 220UL) / 1000UL);

  // Peak state machine: enter peak when above thr, exit when falls below half thr.
  if (!rdet.inPeak) {
    if (ax > thr) {
      if (!rdet.hasLast || (ecgSampleNo - rdet.lastRIdx) > refractorySamples) {
        rdet.inPeak = true;
        rdet.peakMax = ax;
        rdet.peakMaxIdx = ecgSampleNo;
      }
    }
  } else {
    if (ax > rdet.peakMax) {
      rdet.peakMax = ax;
      rdet.peakMaxIdx = ecgSampleNo;
    }
    if (ax < (thr * 0.5f)) {
      // end peak -> accept rdet.peakMaxIdx as R
      rdet.inPeak = false;

      uint32_t rIdx = rdet.peakMaxIdx;
      if (rdet.hasLast) {
        uint32_t rrSamples = rIdx - rdet.lastRIdx;
        int rrMs = (int)((rrSamples * 1000UL) / ECG_FS_HZ);

        // sanity limits
        if (rrMs >= 250 && rrMs <= 2000) {
          int hr = (rrMs > 0) ? (int)(60000 / rrMs) : 0;
          hr = clampi(hr, 30, 240);

          // smooth HR a bit
          rdet.hrSmooth = 0.85f * rdet.hrSmooth + 0.15f * (float)hr;
          rdet.hrBpm = (int)lround(rdet.hrSmooth);
          rdet.rrMs = rrMs;
        }
      }
      rdet.lastRIdx = rIdx;
      rdet.hasLast = true;
    }
  }
}

// Publish ECG bundle and feed detector sample-by-sample
static void publishEcgBundle(uint32_t nowMs) {
  if (!ecgActive) return;

  // Payload format:
  // [4] "ECG1"
  // [4] uint32 millis
  // [2] uint16 sampleCount
  // [2] uint16 hrBpm (our computed / smoothed)
  // [1] uint8  stress*255 (derived elsewhere)
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

  uint16_t hr = (uint16_t)clampi(rdet.hrBpm, 0, 250);
  buf[p++] = (uint8_t)(hr & 0xFF);
  buf[p++] = (uint8_t)((hr >> 8) & 0xFF);

  // stress byte will be filled by caller via global stressIndex (set in synth)
  extern float stressIndex;
  uint8_t st = (uint8_t)clampi((int)lround(clampf(stressIndex, 0.0f, 1.0f) * 255.0f), 0, 255);
  buf[p++] = st;

  for (uint16_t i = 0; i < n; i++) {
    int16_t s = ECG_SEGMENT_DATA[ecgIdx++];
    if (ecgIdx >= ECG_SEGMENT_LEN) ecgIdx = 0; // LOOP segment
    // feed detector
    rpeakFeedSample(s);

    buf[p++] = (uint8_t)(s & 0xFF);
    buf[p++] = (uint8_t)((s >> 8) & 0xFF);

    ecgSampleNo++;
  }

  mqtt.publish(TOPIC_ECG.c_str(), buf, p);
  ecgPkts++;
}

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
float stressIndex = 0.0f;         // NOTE: no longer “preset”, we compute it each cycle
static bool wearableOk = true;

static uint32_t incidentId = 0;
static const char* incidentType = "none";
static const char* incidentSeverity = "info";
static bool incidentActive = false;
static int incidentTtlSec = 0;

static inline int16_t wifiRssiDbmOrNeg127() {
  return (WiFi.status() == WL_CONNECTED) ? (int16_t)WiFi.RSSI() : (int16_t)-127;
}
static inline uint32_t demoTms(uint32_t nowMs) {
  return (nowMs - demoT0) % DEMO_CYCLE_MS;
}

static float smoothstep(float x) {
  x = clampf(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
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
// - environment follows phase timeline
// - stressIndex is DERIVED from (HR + motion + incident intensity), not a fixed constant
// - HR/RR come from ECG detector when ECG is active; fallback if not yet detected
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

  // Environment
  float tt = (float)tms / 1000.0f;
  float baseTemp = 37.5f + 0.20f * sinf(tt * 0.008f + 0.8f);
  float baseHum  = 35.0f + 0.45f * sinf(tt * 0.006f + 1.1f);
  float baseCo   = 4.0f  + 0.20f * sinf(tt * 0.007f + 0.3f);
  float baseMq2  = 850.0f + 18.0f * sinf(tt * 0.006f + 0.2f);

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

  // HR/RR: prefer detector values when available
  int hrBpm = rdet.hrBpm;
  int rrMs  = rdet.rrMs;

  // Fallback if detector hasn't locked yet
  if (hrBpm <= 0 || rrMs <= 0) {
    // Simple fallback tied to phase (only until we have peaks)
    float hrFallback = 90.0f + 15.0f * worsen + 85.0f * danger - 60.0f * recover + frand(-2.0f, 2.0f);
    hrFallback = clampf(hrFallback, 60.0f, 210.0f);
    hrBpm = (int)lround(hrFallback);
    rrMs = (int)lround(clampf(60000.0f / hrFallback, 260.0f, 1300.0f));
  }

  // DERIVED stressIndex (not hardcoded): from HR + motion + incident intensity
  float hrNorm = clampf(((float)hrBpm - 70.0f) / (190.0f - 70.0f), 0.0f, 1.0f);
  float incLoad = clampf(0.25f * worsen + 0.85f * danger - 0.55f * recover, 0.0f, 1.0f);
  float s = 0.12f + 0.55f * hrNorm + 0.20f * motionLevel + 0.25f * incLoad;
  if (!wearableOk) s += 0.10f;
  stressIndex = clampf(s + frand(-0.03f, 0.03f), 0.0f, 1.0f);

  outTempC = temp;
  outHumPct = hum;
  outMq2Raw = (int)lround(mq2);
  outCoPpm = co;
  outHrBpm = hrBpm;
  outRrMs = rrMs;

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

  char env[950];
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

  char bio[750];
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

  char gw[560];
  snprintf(gw, sizeof(gw),
    "{"
      "\"nodeId\":\"%s\",\"observedAt\":\"%s\","
      "\"uplinkReal\":%s,\"uplinkEffective\":%s,"
      "\"wifiRssiDbm\":%d,"
      "\"bleOk\":%s,\"ecgOn\":%s,"
      "\"ecgPkts\":%lu,\"ecgDrop\":0"
    "}",
    NODE_ID, ts,
    uplink_ok ? "true" : "false",
    uplink_ok ? "true" : "false",
    (int)wifiRssiDbmOrNeg127(),
    wearableOk ? "true" : "false",
    ecgActive ? "true" : "false",
    (unsigned long)ecgPkts
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

  // init ECG detector smoothing
  rdet.hrSmooth = 90.0f + frand(-3.0f, 3.0f);

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

  // cycle wrap => new incident id + reset ECG state (optional)
  if (tms < lastTms) {
    incidentId = (uint32_t)random(100000, 999999);
    // if you want HR to re-lock each cycle:
    // rdet = RPeakDetector{};
    // ecgIdx = 0; ecgSampleNo = 0;
  }
  lastTms = tms;

  updateKinematics(nowMs);

  // Determine crisis window
  const uint32_t dangerStart = PHASE_NORMAL_MS + PHASE_WARN_MS;
  const uint32_t dangerEnd   = dangerStart + PHASE_DANGER_MS;
  bool inDanger = (tms >= dangerStart && tms < dangerEnd);

  // ECG streaming policy
  if (STREAM_ECG_ALWAYS) ecgActive = true;
  else ecgActive = inDanger;

  // Keep publishing other signals
  float tempC, humPct, coPpm;
  int mq2Raw, hrBpm, rrMs;
  synthDemoSignals(nowMs, tempC, humPct, mq2Raw, coPpm, hrBpm, rrMs);

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

  // Publish ECG bundles at the correct rate for ECG_FS_HZ
  uint32_t interval = ecgBundleIntervalMs();
  if (ecgActive && (nowMs - lastEcgPub >= interval)) {
    lastEcgPub = nowMs;
    publishEcgBundle(nowMs);
  }

  delay(2);
}
