#!/usr/bin/env python3
import base64
import json
import logging
import sqlite3
import time
from dataclasses import dataclass
from datetime import datetime, timezone, timedelta
from typing import Any, Dict, Optional, Tuple

import paho.mqtt.client as mqtt
import requests
// ADDED LOCATION DATA + RULE ENGINE ALERTS
# ===================== CONFIG ===================== #
MQTT_BROKER_HOST = "localhost"
MQTT_BROKER_PORT = 1883

MQTT_TOPICS = [
    ("ngsi/Environment/+/+", 0),
    ("ngsi/Biomedical/+/+", 0),
    ("ngsi/Location/+/+", 0),     # <-- OwnTracks should publish here
    ("ngsi/Gateway/+", 0),
    ("raw/ECG/+/+", 0),
]

CONTEXT_BROKER_BASE_URL = "http://192.168.2.12:1026"
FIWARE_SERVICE = None
FIWARE_SERVICEPATH = None

SQLITE_DB_PATH = "edge_data.db"
ENABLE_SQLITE = True
ORION_TIMEOUT_SEC = 5

# Frontend MQTT outputs
ALERTS_TOPIC_PREFIX = "edge/alerts"       # edge/alerts/<team>/<ff>
STATUS_TOPIC_PREFIX = "edge/status"       # edge/status/<team>/<ff>

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
    """
    Topics:
      ngsi/Environment/<team>/<ff> -> EnvNode:<team>:<ff> , type Environment
      ngsi/Biomedical/<team>/<ff>  -> Wearable:<team>:<ff>, type Biomedical
      ngsi/Location/<team>/<ff>    -> Phone:<team>:<ff>   , type Location
      ngsi/Gateway/<node>          -> Gateway:<node>      , type Gateway
      raw/ECG/<team>/<ff>          -> handled separately
    """
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

        # flatten if someone accidentally sends dicts
        if isinstance(v, dict):
            for sk, sv in v.items():
                ent[f"{k}_{sk}"] = {"type": _attr_type(sv), "value": sv}
            continue

        if k in ("observedAt", "forwardedAt") and isinstance(v, str) and _looks_like_iso8601_utc(v):
            ent[k] = {"type": "DateTime", "value": v}
            continue

        ent[k] = {"type": _attr_type(v), "value": v}

    # Ensure at least a timestamp exists
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
    # Environment
    mq2_warn: int = 1800
    mq2_danger: int = 2600
    temp_warn: float = 50.0
    temp_danger: float = 60.0
    co_warn: float = 35.0       # ppm (if you have coPpm)
    co_danger: float = 100.0

    # Health
    hr_warn: int = 160
    hr_danger: int = 180

    # Freshness
    stale_warn_sec: int = 15
    stale_danger_sec: int = 45

TH = Thresholds()

# Keep last-seen per firefighter
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
    # fallback
    return datetime.now(timezone.utc)

def _mk_alert(team: str, ff: str, severity: str, category: str, reason: str, value: Any = None) -> Dict[str, Any]:
    out = {
        "teamId": team,
        "ffId": ff,
        "severity": severity,          # info/warn/danger/offline
        "category": category,          # environment/health/connectivity/location
        "reason": reason,
        "value": value,
        "observedAt": _nowz(),
    }
    return out

def _severity_rank(s: str) -> int:
    return {"info": 0, "warn": 1, "danger": 2, "offline": 2}.get(s, 0)

def evaluate_rules(team: str, ff: str) -> Tuple[Dict[str, Any], list]:
    """
    Returns (status_summary, alerts[])
    status_summary is for frontend and optional Orion Firefighter entity.
    """
    key = (team, ff)
    alerts = []
    now = datetime.now(timezone.utc)

    # Freshness checks
    seen = last_seen.get(key)
    if seen:
        age = (now - seen).total_seconds()
        if age >= TH.stale_danger_sec:
            alerts.append(_mk_alert(team, ff, "offline", "connectivity", f"Data stale ({int(age)}s)", int(age)))
        elif age >= TH.stale_warn_sec:
            alerts.append(_mk_alert(team, ff, "warn", "connectivity", f"Data delayed ({int(age)}s)", int(age)))

    # Env checks
    env = last_env.get(key, {})
    mq2 = env.get("mq2Raw")
    temp = env.get("tempC")
    co = env.get("coPpm")

    if isinstance(mq2, int):
        if mq2 >= TH.mq2_danger:
            alerts.append(_mk_alert(team, ff, "danger", "environment", "Very high MQ2", mq2))
        elif mq2 >= TH.mq2_warn:
            alerts.append(_mk_alert(team, ff, "warn", "environment", "High MQ2", mq2))

    if isinstance(temp, (int, float)):
        if temp >= TH.temp_danger:
            alerts.append(_mk_alert(team, ff, "danger", "environment", "Very high temperature", temp))
        elif temp >= TH.temp_warn:
            alerts.append(_mk_alert(team, ff, "warn", "environment", "High temperature", temp))

    if isinstance(co, (int, float)):
        if co >= TH.co_danger:
            alerts.append(_mk_alert(team, ff, "danger", "environment", "Dangerous CO level", co))
        elif co >= TH.co_warn:
            alerts.append(_mk_alert(team, ff, "warn", "environment", "Elevated CO level", co))

    # Health checks
    bio = last_bio.get(key, {})
    hr = bio.get("hrBpm")
    wearable_ok = bio.get("wearableOk")

    if wearable_ok is False:
        alerts.append(_mk_alert(team, ff, "warn", "health", "Wearable not OK", False))

    if isinstance(hr, int):
        if hr >= TH.hr_danger:
            alerts.append(_mk_alert(team, ff, "danger", "health", "Very high heart rate", hr))
        elif hr >= TH.hr_warn:
            alerts.append(_mk_alert(team, ff, "warn", "health", "High heart rate", hr))

    # Failover signal (informational but useful)
    # If any latest payload had failover true
    if env.get("failover") is True or bio.get("failover") is True:
        via = env.get("via") or bio.get("via")
        alerts.append(_mk_alert(team, ff, "info", "connectivity", f"Failover active via {via}", via))

    # Build summary status
    worst = "info"
    for a in alerts:
        if _severity_rank(a["severity"]) > _severity_rank(worst):
            worst = a["severity"]

    # Include last known location if any
    loc = last_loc.get(key, {})
    summary = {
        "teamId": team,
        "ffId": ff,
        "status": "normal" if worst == "info" else ("warn" if worst == "warn" else ("danger" if worst == "danger" else "offline")),
        "severity": worst,
        "observedAt": _nowz(),
        "lastSeenSec": int((now - seen).total_seconds()) if seen else None,
        "lat": loc.get("lat"),
        "lon": loc.get("lon"),
        "accuracyM": loc.get("accuracyM"),
        "hrBpm": hr,
        "tempC": temp,
        "mq2Raw": mq2,
        "coPpm": co,
    }
    return summary, alerts

def build_alert_entity(alert: Dict[str, Any]) -> Dict[str, Any]:
    # Stable ID per alert category per firefighter (so it updates instead of exploding)
    aid = f"Alert:{alert['teamId']}:{alert['ffId']}:{alert['category']}:{alert['reason'][:24].replace(' ', '_')}"
    ent = {
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
    return ent

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
    # optional numeric hints (Grafana)
    for k in ("lastSeenSec", "lat", "lon", "accuracyM", "hrBpm", "tempC", "mq2Raw", "coPpm"):
        v = summary.get(k)
        if isinstance(v, (int, float)):
            ent[k] = {"type": "Number", "value": float(v)}
    return ent

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

    # --- RAW ECG (binary) ---
    if topic.startswith("raw/ECG/"):
        parts = topic.split("/")
        if len(parts) == 4:
            _, _, team_id, ff_id = parts
        else:
            team_id, ff_id = ("unknown", "unknown")

        payload_bytes = msg.payload
        store_raw_ecg(team_id, ff_id, topic, payload_bytes)

        # small meta event for frontend
        meta = {
            "teamId": team_id,
            "ffId": ff_id,
            "bytes": len(payload_bytes),
            "observedAt": _nowz(),
        }
        client.publish("edge/ecg_meta", json.dumps(meta), qos=0, retain=False)
        return

    # --- JSON telemetry ---
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except Exception:
        logging.warning("Non-JSON on %s (ignored)", topic)
        return

    entity_id, entity_type, team_id, ff_or_node = _topic_to_entity(topic)
    if not entity_id or not entity_type:
        logging.warning("Unknown topic pattern: %s", topic)
        return

    # Store locally
    store_measurement(entity_id, entity_type, topic, payload)

    # Send to Orion
    entity = build_ngsi_v2_entity(entity_id, entity_type, payload)
    send_to_orion_op_update(entity)

    # Update in-memory state for rule engine
    if entity_type in ("Environment", "Biomedical", "Location") and team_id and ff_or_node:
        key = (team_id, ff_or_node)
        last_seen[key] = _parse_observed_at(payload)

        if entity_type == "Environment":
            last_env[key] = payload
        elif entity_type == "Biomedical":
            last_bio[key] = payload
        elif entity_type == "Location":
            # Normalize OwnTracks keys into our standard fields (if needed)
            # OwnTracks uses acc (meters). We'll accept either accuracyM or acc.
            if "accuracyM" not in payload and "acc" in payload:
                payload["accuracyM"] = payload.get("acc")
            last_loc[key] = {
                "lat": payload.get("lat"),
                "lon": payload.get("lon"),
                "accuracyM": payload.get("accuracyM"),
            }

        # Run rule engine and publish results
        summary, alerts = evaluate_rules(team_id, ff_or_node)

        # Frontend-friendly status
        client.publish(f"{STATUS_TOPIC_PREFIX}/{team_id}/{ff_or_node}", json.dumps(summary), qos=0, retain=True)

        # Alerts (send each; retain false so UI can treat as events)
        for a in alerts:
            client.publish(f"{ALERTS_TOPIC_PREFIX}/{team_id}/{ff_or_node}", json.dumps(a), qos=0, retain=False)
            # also store in Orion as Alert entity (optional but useful for Grafana)
            send_to_orion_op_update(build_alert_entity(a))

        # Also store consolidated status in Orion (optional but VERY useful)
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
            client.loop_forever()
        except Exception as e:
            logging.error("MQTT error: %s. Reconnecting in 5s...", e)
            time.sleep(5)

if __name__ == "__main__":
    main()
