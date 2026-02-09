#!/usr/bin/env python3
import base64
import json
import logging
import math
import random
import sqlite3
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Dict, Optional, Tuple

import paho.mqtt.client as mqtt
import requests

# =========================================================
# Smart Firefighter Edge Controller
#  - Ingest telemetry (Environment/Biomedical/Location)
#  - Push to Orion (NGSIv2 op/update)
#  - Rule engine produces:
#      edge/status/<team>/<ff>  (retained)
#      edge/alerts/<team>/<ff>  (events)
#  - NEW: Fake location generator for FF_B/FF_C/FF_D
#      * Smooth motion (no teleport)
#      * Near FF_A, but FF_C drifts further (crisis/off-route)
# =========================================================

# ===================== CONFIG ===================== #
MQTT_BROKER_HOST = "localhost"
MQTT_BROKER_PORT = 1883

TEAM_ID = "Team_A"
REAL_FF_ID = "FF_A"             # OwnTracks/real phone publishes here
FAKE_FFS = ["FF_B", "FF_C", "FF_D"]

MQTT_TOPICS = [
    ("ngsi/Environment/+/+", 0),
    ("ngsi/Biomedical/+/+", 0),
    ("ngsi/Location/+/+", 0),     # OwnTracks publishes here (FF_A)
    ("ngsi/Gateway/+", 0),
    ("raw/ECG/+/+", 0),
]

CONTEXT_BROKER_BASE_URL = "http://192.168.2.12:1026"
FIWARE_SERVICE = None
FIWARE_SERVICEPATH = None

SQLITE_DB_PATH = "edge_data.db"
ENABLE_SQLITE = True
ORION_TIMEOUT_SEC = 5

ALERTS_TOPIC_PREFIX = "edge/alerts"       # edge/alerts/<team>/<ff>
STATUS_TOPIC_PREFIX = "edge/status"       # edge/status/<team>/<ff>

# Fake location publishing
FAKE_LOC_PUB_HZ = 1.0
FAKE_LOC_ACCURACY_M = 8
MAX_SPEED_MPS = 1.6
MAX_ACCEL_MPS2 = 0.6
KEEP_WITHIN_M = 250

# Patras fallback anchor (until FF_A arrives)
FALLBACK_ANCHOR_LAT = 38.2466
FALLBACK_ANCHOR_LON = 21.7346

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")

# ===================== SQLITE ===================== #
def init_db():
    if not ENABLE_SQLITE:
        return
    conn = sqlite3.connect(SQLITE_DB_PATH)
    c = conn.cursor()
    c.execute("""
        CREATE TABLE IF NOT EXISTS measurements (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            entity_id TEXT,
            entity_type TEXT,
            topic TEXT,
            payload_json TEXT,
            ts_utc TEXT
        );
    """)
    c.execute("""
        CREATE TABLE IF NOT EXISTS raw_ecg (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            team_id TEXT,
            ff_id TEXT,
            topic TEXT,
            payload_b64 TEXT,
            payload_len INTEGER,
            ts_utc TEXT
        );
    """)
    conn.commit()
    conn.close()

def store_measurement(entity_id: str, entity_type: str, topic: str, payload: Dict[str, Any]):
    if not ENABLE_SQLITE:
        return
    conn = sqlite3.connect(SQLITE_DB_PATH)
    c = conn.cursor()
    ts_utc = datetime.now(timezone.utc).isoformat()
    c.execute(
        "INSERT INTO measurements (entity_id, entity_type, topic, payload_json, ts_utc) VALUES (?, ?, ?, ?, ?);",
        (entity_id, entity_type, topic, json.dumps(payload, separators=(",", ":")), ts_utc),
    )
    conn.commit()
    conn.close()

def store_raw_ecg(team_id: str, ff_id: str, topic: str, payload_bytes: bytes):
    if not ENABLE_SQLITE:
        return
    conn = sqlite3.connect(SQLITE_DB_PATH)
    c = conn.cursor()
    ts_utc = datetime.now(timezone.utc).isoformat()
    payload_b64 = base64.b64encode(payload_bytes).decode("ascii")
    c.execute(
        "INSERT INTO raw_ecg (team_id, ff_id, topic, payload_b64, payload_len, ts_utc) VALUES (?, ?, ?, ?, ?, ?);",
        (team_id, ff_id, topic, payload_b64, len(payload_bytes), ts_utc),
    )
    conn.commit()
    conn.close()

# ===================== NGSI HELPERS ===================== #
def _attr_type(v: Any) -> str:
    if isinstance(v, bool):
        return "Boolean"
    if isinstance(v, (int, float)):
        return "Number"
    return "Text"

def _looks_like_iso8601_utc(s: str) -> bool:
    return isinstance(s, str) and len(s) >= 20 and s.endswith("Z") and "T" in s

def _topic_to_entity(topic: str) -> Tuple[Optional[str], Optional[str], Optional[str], Optional[str]]:
    parts = topic.split("/")
    if len(parts) >= 3 and parts[0] == "ngsi" and parts[1] == "Gateway":
        node = parts[2]
        return (f"Gateway:{node}", "Gateway", None, node)

    if len(parts) == 4 and parts[0] == "ngsi":
        _, kind, team_id, ff_id = parts
        if kind == "Environment":
            return (f"EnvNode:{team_id}:{ff_id}", "Environment", team_id, ff_id)
        if kind == "Biomedical":
            return (f"Wearable:{team_id}:{ff_id}", "Biomedical", team_id, ff_id)
        if kind == "Location":
            return (f"Phone:{team_id}:{ff_id}", "Location", team_id, ff_id)

    return (None, None, None, None)

def build_ngsi_v2_entity(entity_id: str, entity_type: str, payload: Dict[str, Any]) -> Dict[str, Any]:
    ent: Dict[str, Any] = {"id": entity_id, "type": entity_type}
    for k, v in payload.items():
        if k in ("id", "type"):
            continue
        if isinstance(v, dict):
            for sk, sv in v.items():
                ent[f"{k}_{sk}"] = {"type": _attr_type(sv), "value": sv}
            continue
        if k in ("observedAt", "forwardedAt") and isinstance(v, str) and _looks_like_iso8601_utc(v):
            ent[k] = {"type": "DateTime", "value": v}
            continue
        ent[k] = {"type": _attr_type(v), "value": v}
    if "observedAt" not in payload and "timestamp" not in payload and "tst" not in payload:
        ent["timestamp"] = {"type": "Number", "value": int(time.time())}
    return ent

def send_to_orion_op_update(entity: Dict[str, Any]) -> bool:
    url = f"{CONTEXT_BROKER_BASE_URL}/v2/op/update"
    headers = {"Content-Type": "application/json"}
    if FIWARE_SERVICE:
        headers["Fiware-Service"] = FIWARE_SERVICE
    if FIWARE_SERVICEPATH:
        headers["Fiware-ServicePath"] = FIWARE_SERVICEPATH
    body = {"actionType": "append", "entities": [entity]}
    try:
        r = requests.post(url, headers=headers, json=body, timeout=ORION_TIMEOUT_SEC)
        if r.status_code not in (200, 201, 204):
            logging.warning("Orion %s: %s", r.status_code, r.text[:300])
            return False
        return True
    except Exception as e:
        logging.error("❌ Orion send error: %s", e)
        return False

# ===================== RULE ENGINE ===================== #
@dataclass
class Thresholds:
    mq2_warn: int = 1800
    mq2_danger: int = 2600
    temp_warn: float = 50.0
    temp_danger: float = 60.0
    co_warn: float = 35.0
    co_danger: float = 100.0
    hr_warn: int = 160
    hr_danger: int = 180
    separation_warn_m: float = 60.0
    separation_danger_m: float = 110.0
    stale_warn_sec: int = 15
    stale_danger_sec: int = 45

TH = Thresholds()

last_seen: Dict[Tuple[str, str], datetime] = {}
last_env: Dict[Tuple[str, str], Dict[str, Any]] = {}
last_bio: Dict[Tuple[str, str], Dict[str, Any]] = {}
last_loc: Dict[Tuple[str, str], Dict[str, Any]] = {}

def _nowz() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

def _parse_observed_at(payload: Dict[str, Any]) -> datetime:
    oa = payload.get("observedAt")
    if isinstance(oa, str) and oa.endswith("Z"):
        try:
            return datetime.fromisoformat(oa.replace("Z", "+00:00"))
        except Exception:
            pass
    return datetime.now(timezone.utc)

def _mk_alert(team: str, ff: str, severity: str, category: str, reason: str, value: Any = None) -> Dict[str, Any]:
    return {"teamId": team, "ffId": ff, "severity": severity, "category": category, "reason": reason, "value": value, "observedAt": _nowz()}

def _severity_rank(s: str) -> int:
    return {"info": 0, "warn": 1, "danger": 2, "offline": 2}.get(s, 0)

def haversine_m(lat1, lon1, lat2, lon2) -> float:
    R = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dphi/2)**2 + math.cos(p1)*math.cos(p2)*math.sin(dl/2)**2
    return 2*R*math.asin(math.sqrt(a))

def evaluate_rules(team: str, ff: str) -> Tuple[Dict[str, Any], list]:
    key = (team, ff)
    alerts = []
    now = datetime.now(timezone.utc)

    seen = last_seen.get(key)
    if seen:
        age = (now - seen).total_seconds()
        if age >= TH.stale_danger_sec:
            alerts.append(_mk_alert(team, ff, "offline", "connectivity", f"Data stale ({int(age)}s)", int(age)))
        elif age >= TH.stale_warn_sec:
            alerts.append(_mk_alert(team, ff, "warn", "connectivity", f"Data delayed ({int(age)}s)", int(age)))

    env = last_env.get(key, {})
    mq2 = env.get("mq2Raw")
    temp = env.get("tempC")
    co = env.get("coPpm")

    if isinstance(mq2, int):
        alerts.append(_mk_alert(team, ff, "danger" if mq2 >= TH.mq2_danger else "warn", "environment",
                                "Very high MQ2" if mq2 >= TH.mq2_danger else "High MQ2", mq2)) if mq2 >= TH.mq2_warn else None
    if isinstance(temp, (int, float)):
        alerts.append(_mk_alert(team, ff, "danger" if temp >= TH.temp_danger else "warn", "environment",
                                "Very high temperature" if temp >= TH.temp_danger else "High temperature", float(temp))) if temp >= TH.temp_warn else None
    if isinstance(co, (int, float)):
        alerts.append(_mk_alert(team, ff, "danger" if co >= TH.co_danger else "warn", "environment",
                                "Dangerous CO level" if co >= TH.co_danger else "Elevated CO level", float(co))) if co >= TH.co_warn else None

    bio = last_bio.get(key, {})
    hr = bio.get("hrBpm")
    wearable_ok = bio.get("wearableOk")
    if wearable_ok is False:
        alerts.append(_mk_alert(team, ff, "warn", "health", "Wearable not OK", False))
    if isinstance(hr, int):
        alerts.append(_mk_alert(team, ff, "danger" if hr >= TH.hr_danger else "warn", "health",
                                "Very high heart rate" if hr >= TH.hr_danger else "High heart rate", hr)) if hr >= TH.hr_warn else None

    loc = last_loc.get(key, {})
    lat, lon = loc.get("lat"), loc.get("lon")
    anchor = last_loc.get((team, REAL_FF_ID), {})
    al, ao = anchor.get("lat", FALLBACK_ANCHOR_LAT), anchor.get("lon", FALLBACK_ANCHOR_LON)

    if isinstance(lat, (int, float)) and isinstance(lon, (int, float)):
        d = haversine_m(lat, lon, al, ao)
        if d >= TH.separation_danger_m:
            alerts.append(_mk_alert(team, ff, "danger", "location", f"Separated from team ({int(d)}m)", int(d)))
        elif d >= TH.separation_warn_m:
            alerts.append(_mk_alert(team, ff, "warn", "location", f"Drifting from team ({int(d)}m)", int(d)))

    worst = "info"
    for a in alerts:
        if _severity_rank(a["severity"]) > _severity_rank(worst):
            worst = a["severity"]

    summary = {
        "teamId": team,
        "ffId": ff,
        "status": "normal" if worst == "info" else ("warn" if worst == "warn" else ("danger" if worst == "danger" else "offline")),
        "severity": worst,
        "observedAt": _nowz(),
        "lastSeenSec": int((now - seen).total_seconds()) if seen else None,
        "lat": lat,
        "lon": lon,
        "accuracyM": loc.get("accuracyM"),
        "hrBpm": hr,
        "tempC": temp,
        "mq2Raw": mq2,
        "coPpm": co,
    }
    return summary, alerts

def build_alert_entity(alert: Dict[str, Any]) -> Dict[str, Any]:
    aid = f"Alert:{alert['teamId']}:{alert['ffId']}:{alert['category']}:{alert['reason'][:24].replace(' ', '_')}"
    return {
        "id": aid,
        "type": "Alert",
        "teamId": {"type": "Text", "value": alert["teamId"]},
        "ffId": {"type": "Text", "value": alert["ffId"]},
        "severity": {"type": "Text", "value": alert["severity"]},
        "category": {"type": "Text", "value": alert["category"]},
        "reason": {"type": "Text", "value": alert["reason"]},
        "value": {"type": _attr_type(alert.get("value")), "value": alert.get("value")},
        "observedAt": {"type": "DateTime", "value": alert.get("observedAt", _nowz())},
    }

def build_firefighter_status_entity(summary: Dict[str, Any]) -> Dict[str, Any]:
    eid = f"Firefighter:{summary['teamId']}:{summary['ffId']}"
    ent = {
        "id": eid,
        "type": "Firefighter",
        "teamId": {"type": "Text", "value": summary["teamId"]},
        "ffId": {"type": "Text", "value": summary["ffId"]},
        "status": {"type": "Text", "value": summary["status"]},
        "severity": {"type": "Text", "value": summary["severity"]},
        "observedAt": {"type": "DateTime", "value": summary["observedAt"]},
    }
    for k in ("lastSeenSec", "lat", "lon", "accuracyM", "hrBpm", "tempC", "mq2Raw", "coPpm"):
        v = summary.get(k)
        if isinstance(v, (int, float)):
            ent[k] = {"type": "Number", "value": float(v)}
    return ent

# ===================== FAKE LOCATION SIM ===================== #
def meters_to_deg_lat(m: float) -> float:
    return m / 111_320.0

def meters_to_deg_lon(m: float, at_lat: float) -> float:
    return m / (111_320.0 * math.cos(math.radians(at_lat)) + 1e-9)

class FakeLocationSim:
    def __init__(self, ff_id: str, crisis: bool):
        self.ff_id = ff_id
        self.crisis = crisis
        self.lat = FALLBACK_ANCHOR_LAT
        self.lon = FALLBACK_ANCHOR_LON

        off_m = 8 if not crisis else 35
        angle = random.random() * 2*math.pi
        self.lat += meters_to_deg_lat(off_m*math.cos(angle))
        self.lon += meters_to_deg_lon(off_m*math.sin(angle), self.lat)

        self.vx = 0.0  # east m/s
        self.vy = 0.0  # north m/s
        self.bias_angle = random.random() * 2*math.pi

    def step(self, dt: float, anchor_lat: float, anchor_lon: float) -> Dict[str, Any]:
        dy = (self.lat - anchor_lat) * 111_320.0
        dx = (self.lon - anchor_lon) * (111_320.0 * math.cos(math.radians(anchor_lat)) + 1e-9)
        dist = math.hypot(dx, dy)

        self.bias_angle += random.uniform(-0.08, 0.08)
        base_ax = math.cos(self.bias_angle)
        base_ay = math.sin(self.bias_angle)

        drift = 1.0 if self.crisis else 0.25

        leash = 0.0
        if dist > KEEP_WITHIN_M:
            leash = min(1.0, (dist - KEEP_WITHIN_M) / 120.0)

        ax = drift * 0.45 * base_ax
        ay = drift * 0.45 * base_ay

        if dist > 1.0:
            away_x = dx / dist
            away_y = dy / dist
            if self.crisis:
                ax += 0.35 * away_x
                ay += 0.35 * away_y
            else:
                ax -= 0.10 * away_x
                ay -= 0.10 * away_y

        ax -= leash * 1.1 * (dx / (dist + 1e-6))
        ay -= leash * 1.1 * (dy / (dist + 1e-6))

        a = math.hypot(ax, ay)
        if a > 1e-6:
            scale = min(1.0, MAX_ACCEL_MPS2 / a)
            ax *= scale
            ay *= scale

        self.vx += ax * dt
        self.vy += ay * dt

        v = math.hypot(self.vx, self.vy)
        if v > MAX_SPEED_MPS:
            s = MAX_SPEED_MPS / v
            self.vx *= s
            self.vy *= s

        dx_step = self.vx * dt
        dy_step = self.vy * dt

        self.lat += meters_to_deg_lat(dy_step)
        self.lon += meters_to_deg_lon(dx_step, self.lat)

        return {
            "teamId": TEAM_ID,
            "ffId": self.ff_id,
            "lat": float(self.lat),
            "lon": float(self.lon),
            "accuracyM": FAKE_LOC_ACCURACY_M,
            "observedAt": _nowz(),
            "source": "fake",
        }

_stop_sim = threading.Event()
_sims = {
    "FF_B": FakeLocationSim("FF_B", crisis=False),
    "FF_C": FakeLocationSim("FF_C", crisis=True),
    "FF_D": FakeLocationSim("FF_D", crisis=False),
}

def fake_location_publisher(mqtt_client: mqtt.Client):
    period = 1.0 / max(0.1, FAKE_LOC_PUB_HZ)
    last_t = time.time()
    while not _stop_sim.is_set():
        t = time.time()
        dt = max(0.05, min(1.5, t - last_t))
        last_t = t

        anchor = last_loc.get((TEAM_ID, REAL_FF_ID), {})
        al = anchor.get("lat", FALLBACK_ANCHOR_LAT)
        ao = anchor.get("lon", FALLBACK_ANCHOR_LON)

        for ff, sim in _sims.items():
            payload = sim.step(dt, al, ao)
            topic = f"ngsi/Location/{TEAM_ID}/{ff}"
            mqtt_client.publish(topic, json.dumps(payload), qos=0, retain=True)

            key = (TEAM_ID, ff)
            last_seen[key] = _parse_observed_at(payload)
            last_loc[key] = {"lat": payload["lat"], "lon": payload["lon"], "accuracyM": payload["accuracyM"]}

            summary, alerts = evaluate_rules(TEAM_ID, ff)
            mqtt_client.publish(f"{STATUS_TOPIC_PREFIX}/{TEAM_ID}/{ff}", json.dumps(summary), qos=0, retain=True)
            for a in alerts:
                mqtt_client.publish(f"{ALERTS_TOPIC_PREFIX}/{TEAM_ID}/{ff}", json.dumps(a), qos=0, retain=False)

        time.sleep(period)

# ===================== MQTT CALLBACKS ===================== #
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        logging.info("✅ Connected to MQTT broker")
        for topic, qos in MQTT_TOPICS:
            client.subscribe(topic, qos)
            logging.info("Subscribed: %s", topic)
    else:
        logging.error("❌ MQTT connect failed rc=%s", rc)

def on_message(client, userdata, msg):
    topic = msg.topic

    if topic.startswith("raw/ECG/"):
        parts = topic.split("/")
        team_id, ff_id = (parts[2], parts[3]) if len(parts) == 4 else ("unknown", "unknown")
        payload_bytes = msg.payload
        store_raw_ecg(team_id, ff_id, topic, payload_bytes)
        meta = {"teamId": team_id, "ffId": ff_id, "bytes": len(payload_bytes), "observedAt": _nowz()}
        client.publish("edge/ecg_meta", json.dumps(meta), qos=0, retain=False)
        return

    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except Exception:
        logging.warning("Non-JSON on %s (ignored)", topic)
        return

    entity_id, entity_type, team_id, ff_or_node = _topic_to_entity(topic)
    if not entity_id or not entity_type:
        logging.warning("Unknown topic pattern: %s", topic)
        return

    store_measurement(entity_id, entity_type, topic, payload)

    entity = build_ngsi_v2_entity(entity_id, entity_type, payload)
    send_to_orion_op_update(entity)

    if entity_type in ("Environment", "Biomedical", "Location") and team_id and ff_or_node:
        key = (team_id, ff_or_node)
        last_seen[key] = _parse_observed_at(payload)

        if entity_type == "Environment":
            last_env[key] = payload
        elif entity_type == "Biomedical":
            last_bio[key] = payload
        elif entity_type == "Location":
            if "accuracyM" not in payload and "acc" in payload:
                payload["accuracyM"] = payload.get("acc")
            last_loc[key] = {"lat": payload.get("lat"), "lon": payload.get("lon"), "accuracyM": payload.get("accuracyM")}

        summary, alerts = evaluate_rules(team_id, ff_or_node)
        client.publish(f"{STATUS_TOPIC_PREFIX}/{team_id}/{ff_or_node}", json.dumps(summary), qos=0, retain=True)

        for a in alerts:
            client.publish(f"{ALERTS_TOPIC_PREFIX}/{team_id}/{ff_or_node}", json.dumps(a), qos=0, retain=False)
            send_to_orion_op_update(build_alert_entity(a))

        send_to_orion_op_update(build_firefighter_status_entity(summary))

# ===================== MAIN ===================== #
def main():
    init_db()
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message

    while True:
        try:
            client.connect(MQTT_BROKER_HOST, MQTT_BROKER_PORT, keepalive=60)
            threading.Thread(target=fake_location_publisher, args=(client,), daemon=True).start()
            client.loop_forever()
        except Exception as e:
            logging.error("MQTT error: %s. Reconnecting in 5s...", e)
            time.sleep(5)

if __name__ == "__main__":
    main()
