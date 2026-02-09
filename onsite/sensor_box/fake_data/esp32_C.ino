#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include <math.h>

// ===================== IDENTITY =====================
static constexpr char NODE_ID_CHAR = 'C';
static const char* NODE_ID         = "C";
static const char* TEAM_ID         = "Team_A";
static const char* FIREFIGHTER_ID  = "FF_C";

// ===================== WIFI  =====================
struct WifiCred { const char* ssid; const char* pass; };
static WifiCred WIFI_LIST[] = {
  { "VODAFONE_H268Q-4057", "SFzDE5ZxHyQPQ2FQ" },
  { "FirefighterA",        "firefighterA" },
};
static constexpr int WIFI_LIST_N = sizeof(WIFI_LIST) / sizeof(WIFI_LIST[0]);

// ===================== MQTT  =====================
static const char* MQTT_HOST = "192.168.2.13";
static const int   MQTT_PORT = 1883;

// Topics 
static String TOPIC_ENV = String("ngsi/Environment/") + TEAM_ID + "/" + FIREFIGHTER_ID;
static String TOPIC_BIO = String("ngsi/Biomedical/")  + TEAM_ID + "/" + FIREFIGHTER_ID;
static String TOPIC_GW  = String("ngsi/Gateway/")     + NODE_ID;

// ===================== TIMING =====================
static const uint32_t PUB_INTERVAL_MS = 2000;
static const uint32_t MQTT_RETRY_MS   = 2000;
static const uint32_t WIFI_ATTEMPT_WINDOW_MS = 15000;

// ===================== GLOBALS =====================
static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

static volatile bool wifiGotIp = false;
static volatile bool wifiConnecting = false;
static int wifiIndex = -1;
static uint32_t wifiAttemptStart = 0;
static uint32_t lastMqttTry = 0;
static uint32_t lastPub = 0;

static bool timeInited = false;
static uint32_t lastTimeTry = 0;

// ===================== SIM STATE =====================
struct Incident {
  bool active = false;
  uint32_t untilMs = 0;
  float intensity = 0;  // 0..1
};
static Incident incHeat, incSmoke, incCO, incTachy, incWearableDrop, incStale;

static float tempBase = 33.0f;   // room+gear heat
static float humBase  = 35.0f;
static float coBase   = 5.0f;    // ppm baseline
static int   mq2Base  = 900;     // "clean air" raw baseline (tunable)
static int   hrBase   = 105;     // active working HR baseline
static int   rrBaseMs = 600;     // ~100 bpm

static float tempRW = 0.0f, humRW = 0.0f, coRW = 0.0f;
static float hrRW   = 0.0f;
static float mq2RW  = 0.0f;

static bool wearableOk = true;

// ===================== HELPERS =====================
static inline int16_t wifiRssiDbmOrNeg127() {
  return (WiFi.status() == WL_CONNECTED) ? (int16_t)WiFi.RSSI() : (int16_t)-127;
}

static void ensureTimeUtc() {
  if (timeInited) return;
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t now = millis();
  if (now - lastTimeTry < 10000) return;
  lastTimeTry = now;

  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  time_t t = time(nullptr);
  if (t > 1700000000) {
    timeInited = true;
  }
}

static void iso8601UtcNow(char* out, size_t outSz) {
  time_t t = time(nullptr);
  if (t <= 100000) { snprintf(out, outSz, "1970-01-01T00:00:00Z"); return; }
  struct tm tm{};
  gmtime_r(&t, &tm);
  strftime(out, outSz, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static float clampf(float x, float a, float b) { return (x < a) ? a : (x > b) ? b : x; }
static int clampi(int x, int a, int b) { return (x < a) ? a : (x > b) ? b : x; }

static float frand(float a, float b) {
  return a + (b - a) * (float)random(0, 10000) / 10000.0f;
}

static void maybeStartIncident(Incident& inc, float chancePerPub, uint32_t minDurMs, uint32_t maxDurMs) {
  if (inc.active) return;
  // chancePerPub ~ probability each publish tick
  if (frand(0, 1) < chancePerPub) {
    inc.active = true;
    inc.untilMs = millis() + (uint32_t)random(minDurMs, maxDurMs);
    inc.intensity = frand(0.4f, 1.0f);
  }
}

static void updateIncident(Incident& inc) {
  if (!inc.active) return;
  if ((int32_t)(millis() - inc.untilMs) >= 0) {
    inc.active = false;
    inc.intensity = 0;
  }
}

// ===================== WIFI (event-driven) =====================
static void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      wifiConnecting = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiGotIp = true;
      wifiConnecting = false;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiGotIp = false;
      wifiConnecting = false;
      break;
    default:
      break;
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

  if (wifiAttemptStart == 0 || (millis() - wifiAttemptStart) >= WIFI_ATTEMPT_WINDOW_MS) {
    startNextWifiAttempt();
  }
}

// ===================== MQTT =====================
static void ensureMQTT() {
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t now = millis();
  if (now - lastMqttTry < MQTT_RETRY_MS) return;
  lastMqttTry = now;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  uint64_t mac = ESP.getEfuseMac();
  char cid[64];
  snprintf(cid, sizeof(cid), "sim-%s-%s-node%c-%06lX%06lX",
           TEAM_ID, FIREFIGHTER_ID, NODE_ID_CHAR,
           (unsigned long)(mac >> 24), (unsigned long)(mac & 0xFFFFFF));

  mqtt.connect(cid);
}

// ===================== REALISTIC SIGNAL GENERATORS =====================
static void randomWalkStep(float& rw, float step, float limitAbs) {
  rw += frand(-step, step);
  rw = clampf(rw, -limitAbs, limitAbs);
}

static void synthTick(float& outTempC, float& outHumPct, int& outMq2Raw, float& outCoPpm,
                      int& outHrBpm, int& outRrMs) {
  // Update incidents (start chances tuned to feel “rare but real”)
  maybeStartIncident(incHeat,         0.010f, 15000, 60000);   // ~1% per publish
  maybeStartIncident(incSmoke,        0.012f, 15000, 70000);
  maybeStartIncident(incCO,           0.008f, 15000, 60000);
  maybeStartIncident(incTachy,        0.010f, 12000, 50000);
  maybeStartIncident(incWearableDrop, 0.006f,  8000, 25000);
  maybeStartIncident(incStale,        0.004f, 20000, 60000);   // offline simulation

  updateIncident(incHeat);
  updateIncident(incSmoke);
  updateIncident(incCO);
  updateIncident(incTachy);
  updateIncident(incWearableDrop);
  updateIncident(incStale);

  // Random walks (slow drift)
  randomWalkStep(tempRW, 0.08f, 3.0f);
  randomWalkStep(humRW,  0.15f, 8.0f);
  randomWalkStep(coRW,   0.40f, 8.0f);
  randomWalkStep(hrRW,   0.60f, 25.0f);
  randomWalkStep(mq2RW,  20.0f, 600.0f);

  float t = millis() / 1000.0f;

  // Base oscillations (breathing/effort cycles)
  float temp = tempBase + tempRW + 1.2f * sinf(t * 0.07f);
  float hum  = humBase  + humRW  + 3.0f * sinf(t * 0.05f + 1.2f);
  float co   = coBase   + coRW   + 1.0f * sinf(t * 0.09f + 0.6f);
  float mq2  = (float)mq2Base + mq2RW + 60.0f * sinf(t * 0.06f);

  float hr   = (float)hrBase + hrRW + 8.0f * sinf(t * 0.20f);
  float rrMs = 60000.0f / clampf(hr, 55.0f, 200.0f);

  // Apply incidents with ramps
  if (incHeat.active)  temp += 25.0f * incHeat.intensity;             // can exceed 60
  if (incSmoke.active) mq2  += 1900.0f * incSmoke.intensity;          // can exceed 2600
  if (incCO.active)    co   += 140.0f * incCO.intensity;              // can exceed 100
  if (incTachy.active) hr   += 95.0f * incTachy.intensity;            // can exceed 180
  wearableOk = !incWearableDrop.active;

  // Clip to plausible ranges
  temp = clampf(temp, 18.0f, 95.0f);
  hum  = clampf(hum,  10.0f, 90.0f);
  co   = clampf(co,    0.0f, 250.0f);
  mq2  = clampf(mq2,  200.0f, 4095.0f);
  hr   = clampf(hr,   45.0f, 210.0f);

  // Add small measurement noise
  temp += frand(-0.15f, 0.15f);
  hum  += frand(-0.35f, 0.35f);
  co   += frand(-0.8f,  0.8f);
  hr   += frand(-1.5f,  1.5f);

  outTempC = temp;
  outHumPct = hum;
  outCoPpm = co;
  outMq2Raw = (int)lround(mq2);
  outHrBpm = (int)lround(hr);
  outRrMs  = (int)lround(60000.0f / clampf(hr, 55.0f, 200.0f));
}

// ===================== PUBLISHERS =====================
static void publishAll() {
  if (!mqtt.connected()) return;

  ensureTimeUtc();
  char ts[32];
  iso8601UtcNow(ts, sizeof(ts));

  // If “stale incident” active: simulate radio silence (edge should raise delayed/offline)
  if (incStale.active) return;

  float tempC, humPct, coPpm;
  int mq2Raw, hrBpm, rrMs;
  synthTick(tempC, humPct, mq2Raw, coPpm, hrBpm, rrMs);

  char tbuf[16], hbuf[16], cobuf[16];
  dtostrf(tempC, 0, 2, tbuf);
  dtostrf(humPct,0, 2, hbuf);
  dtostrf(coPpm, 0, 1, cobuf);

  // ENV
  char env[560];
  snprintf(env, sizeof(env),
    "{"
      "\"teamId\":\"%s\",\"ffId\":\"%s\",\"nodeId\":\"%s\","
      "\"originNodeId\":\"%s\",\"via\":\"self\",\"failover\":false,\"forwardHopCount\":0,"
      "\"observedAt\":\"%s\","
      "\"tempC\":%s,\"humidityPct\":%s,"
      "\"mq2Raw\":%d,\"mq2Digital\":%d,"
      "\"coPpm\":%s,"
      "\"wifiRssiDbm\":%d,"
      "\"source\":\"sim\""
    "}",
    TEAM_ID, FIREFIGHTER_ID, NODE_ID,
    NODE_ID, ts,
    tbuf, hbuf,
    mq2Raw, (mq2Raw > 1800) ? 1 : 0,
    cobuf,
    (int)wifiRssiDbmOrNeg127()
  );
  mqtt.publish(TOPIC_ENV.c_str(), env);

  // BIO
  char bio[420];
  snprintf(bio, sizeof(bio),
    "{"
      "\"teamId\":\"%s\",\"ffId\":\"%s\","
      "\"originNodeId\":\"%s\",\"via\":\"self\",\"failover\":false,\"forwardHopCount\":0,"
      "\"observedAt\":\"%s\","
      "\"hrBpm\":%d,\"rrMs\":%d,"
      "\"wearableOk\":%s,"
      "\"source\":\"sim\""
    "}",
    TEAM_ID, FIREFIGHTER_ID,
    NODE_ID, ts,
    hrBpm, rrMs,
    wearableOk ? "true" : "false"
  );
  mqtt.publish(TOPIC_BIO.c_str(), bio);

  // GW
  bool uplink_ok = (WiFi.status() == WL_CONNECTED && mqtt.connected());
  char gw[320];
  snprintf(gw, sizeof(gw),
    "{"
      "\"nodeId\":\"%s\",\"observedAt\":\"%s\","
      "\"uplinkReal\":%s,\"uplinkEffective\":%s,"
      "\"wifiRssiDbm\":%d,"
      "\"bleOk\":true,\"ecgOn\":false,"
      "\"ecgPkts\":0,\"ecgDrop\":0"
    "}",
    NODE_ID, ts,
    uplink_ok ? "true" : "false",
    uplink_ok ? "true" : "false",
    (int)wifiRssiDbmOrNeg127()
  );
  mqtt.publish(TOPIC_GW.c_str(), gw);
}

// ===================== SETUP / LOOP =====================
void setup() {
  Serial.begin(115200);
  delay(100);

  randomSeed((uint32_t)ESP.getEfuseMac() ^ (uint32_t)micros());

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.onEvent(onWiFiEvent);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);

  startNextWifiAttempt();
}

void loop() {
  ensureWiFi();
  ensureMQTT();
  mqtt.loop();

  uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
    if (now - lastPub >= PUB_INTERVAL_MS) {
      lastPub = now;
      publishAll();
    }
  }

  delay(2);
}
