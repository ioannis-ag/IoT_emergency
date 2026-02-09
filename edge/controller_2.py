#!/usr/bin/env python3
import base64
import json
import logging
import sqlite3
import time
from datetime import datetime, timezone
from typing import Any, Dict, Optional, Tuple

import paho.mqtt.client as mqtt
import requests

# ===================== CONFIG ===================== #
MQTT_BROKER_HOST = "localhost"
MQTT_BROKER_PORT = 1883

# Subscribe to your chosen topic scheme
MQTT_TOPICS = [
    ("ngsi/Environment/+/+", 0),
    ("ngsi/Biomedical/+/+", 0),
    ("ngsi/Gateway/+", 0),
    ("raw/ECG/+/+", 0),          # recommended: raw/ECG/<team>/<ff>
    # If you ever decide raw/ECG/A style, add ("raw/ECG/+", 0)
]

CONTEXT_BROKER_BASE_URL = "http://192.168.2.12:1026"
FIWARE_SERVICE = None
FIWARE_SERVICEPATH = None

SQLITE_DB_PATH = "edge_data.db"
ENABLE_SQLITE = True

# If Orion is down, keep last send error logs less spammy:
ORION_TIMEOUT_SEC = 5

# Rule engine outputs
ALERTS_MQTT_TOPIC = "edge/alerts"          # local frontend can subscribe
ALERT_ENTITY_PREFIX = "Alert"              # Orion entity: Alert:<team>:<ff> or Alert:Gateway:<node>

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")


# ===================== SQLITE ===================== #
def init_db():
    if not ENABLE_SQLITE:
        return
    conn = sqlite3.connect(SQLITE_DB_PATH)
    c = conn.cursor()

    # JSON measurements
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

    # raw ECG (binary)
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
    # bool must be checked before int/float in Python
    if isinstance(v, bool):
        return "Boolean"
    if isinstance(v, (int, float)):
        return "Number"
    return "Text"


def _looks_like_iso8601_utc(s: str) -> bool:
    # super lightweight check: your ESP32 uses "YYYY-MM-DDTHH:MM:SSZ"
    return isinstance(s, str) and len(s) >= 20 and s.endswith("Z") and "T" in s


def _topic_to_entity(topic: str) -> Tuple[Optional[str], Optional[str], Optional[str], Optional[str]]:
    """
    Returns (entity_id, entity_type, team_id, ff_or_node)
    Based on topic contract.

    Topics:
      ngsi/Environment/<team>/<ff>   -> EnvNode:<team>:<ff> , type Environment
      ngsi/Biomedical/<team>/<ff>    -> Wearable:<team>:<ff>, type Biomedical
      ngsi/Gateway/<node>            -> Gateway:<node>      , type Gateway
      raw/ECG/<team>/<ff>            -> handled separately (binary)
    """
    parts = topic.split("/")
    if len(parts) < 3:
        return (None, None, None, None)

    root = parts[0]
    if root == "ngsi":
        if len(parts) != 4:
            return (None, None, None, None)
        _, kind, team_id, ff_id = parts

        if kind == "Environment":
            return (f"EnvNode:{team_id}:{ff_id}", "Environment", team_id, ff_id)
        if kind == "Biomedical":
            return (f"Wearable:{team_id}:{ff_id}", "Biomedical", team_id, ff_id)

        return (None, None, None, None)

    if root == "raw":
        # raw/ECG/<team>/<ff>
        return (None, None, None, None)

    if topic.startswith("ngsi/Gateway/"):
        # Some brokers may present as parts = ["ngsi","Gateway","A"] due to subscribe pattern.
        pass

    # Explicit gateway form:
    if len(parts) == 3 and parts[0] == "ngsi" and parts[1] == "Gateway":
        node = parts[2]
        return (f"Gateway:{node}", "Gateway", None, node)

    return (None, None, None, None)


def build_ngsi_v2_entity(entity_id: str, entity_type: str, payload: Dict[str, Any]) -> Dict[str, Any]:
    """
    Converts a flat JSON payload to NGSI v2 entity format:
      { id, type, attr: {type,value}, ... }

    Special handling:
      - observedAt: if ISO8601Z -> DateTime
      - forwardedAt: if ISO8601Z -> DateTime
    """
    entity: Dict[str, Any] = {"id": entity_id, "type": entity_type}

    for key, value in payload.items():
        if key in ("id", "type"):
            continue

        # We do NOT want nested dicts; if they exist, flatten them
        if isinstance(value, dict):
            for sub_key, sub_val in value.items():
                attr_name = f"{key}_{sub_key}"
                entity[attr_name] = {"value": sub_val, "type": _attr_type(sub_val)}
            continue

        # Dates
        if key in ("observedAt", "forwardedAt") and isinstance(value, str) and _looks_like_iso8601_utc(value):
            entity[key] = {"value": value, "type": "DateTime"}
            continue

        entity[key] = {"value": value, "type": _attr_type(value)}

    # If no timestamp-like field exists at all, add one
    if "observedAt" not in payload and "timestamp" not in payload and "tst" not in payload:
        entity["timestamp"] = {"value": int(time.time()), "type": "Number"}

    return entity


def send_to_context_broker_op_update(entity: Dict[str, Any]) -> bool:
    """
    Uses /v2/op/update with actionType=append (upsert-ish behavior).
    """
    url = f"{CONTEXT_BROKER_BASE_URL}/v2/op/update"
    headers = {"Content-Type": "application/json"}
    if FIWARE_SERVICE:
        headers["Fiware-Service"] = FIWARE_SERVICE
    if FIWARE_SERVICEPATH:
        headers["Fiware-ServicePath"] = FIWARE_SERVICEPATH

    payload = {"actionType": "append", "entities": [entity]}

    try:
        r = requests.post(url, headers=headers, json=payload, timeout=ORION_TIMEOUT_SEC)
        if r.status_code not in (200, 201, 204):
            logging.warning("Orion responded %s: %s", r.status_code, r.text[:300])
            return False
        return True
    except Exception as e:
        logging.error("❌ Error sending to Orion: %s", e)
        return False


# ===================== RULE ENGINE (PLACEHOLDER) ===================== #
def rule_engine_evaluate(entity_id: str, entity_type: str, payload: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """
    Return an alert dict or None.
    Placeholder rules (safe defaults):
      - Environment: mq2Raw high => warn
      - Biomedical: hrBpm too high => warn
      - Gateway: uplinkEffective false => offline
    You can replace with your real logic later.
    """
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    if entity_type == "Environment":
        mq2 = payload.get("mq2Raw")
        if isinstance(mq2, (int, float)) and mq2 >= 2500:
            return {
                "severity": "warn",
                "reason": "High MQ2 reading",
                "entityId": entity_id,
                "entityType": entity_type,
                "value": mq2,
                "observedAt": payload.get("observedAt", now),
            }

    if entity_type == "Biomedical":
        hr = payload.get("hrBpm")
        if isinstance(hr, (int, float)) and hr >= 170:
            return {
                "severity": "warn",
                "reason": "High heart rate",
                "entityId": entity_id,
                "entityType": entity_type,
                "value": hr,
                "observedAt": payload.get("observedAt", now),
            }

    if entity_type == "Gateway":
        uplink = payload.get("uplinkEffective")
        if uplink is False:
            return {
                "severity": "offline",
                "reason": "Gateway uplink lost",
                "entityId": entity_id,
                "entityType": entity_type,
                "observedAt": payload.get("observedAt", now),
            }

    return None


def build_alert_entity(alert: Dict[str, Any]) -> Dict[str, Any]:
    """
    Create an NGSI entity for alerts so Grafana/Influx can pick it up too.
    """
    eid = alert.get("entityId", "unknown")
    # Make a stable alert entity id per source entity:
    alert_entity_id = f"{ALERT_ENTITY_PREFIX}:{eid}"

    ent = {
        "id": alert_entity_id,
        "type": "Alert",
        "sourceEntityId": {"type": "Text", "value": eid},
        "sourceEntityType": {"type": "Text", "value": alert.get("entityType", "unknown")},
        "severity": {"type": "Text", "value": alert.get("severity", "info")},
        "reason": {"type": "Text", "value": alert.get("reason", "")},
        "value": {"type": _attr_type(alert.get("value")), "value": alert.get("value")} if "value" in alert else {"type": "Text", "value": ""},
        "observedAt": {"type": "DateTime", "value": alert.get("observedAt")},
    }
    return ent


# ===================== MQTT CALLBACKS ===================== #
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        logging.info("✅ Connected to MQTT broker")
        for topic, qos in MQTT_TOPICS:
            client.subscribe(topic, qos)
            logging.info("Subscribed to topic: %s (qos=%s)", topic, qos)
    else:
        logging.error("❌ Failed to connect to MQTT broker. rc=%s", rc)


def on_message(client, userdata, msg):
    topic = msg.topic

    # ---- RAW ECG (binary) ----
    if topic.startswith("raw/ECG/"):
        parts = topic.split("/")
        if len(parts) == 4:
            _, _, team_id, ff_id = parts
        else:
            team_id, ff_id = ("unknown", "unknown")

        payload_bytes = msg.payload  # bytes
        store_raw_ecg(team_id, ff_id, topic, payload_bytes)

        # Optional: also publish a small JSON "ecg meta" event to frontend
        meta = {
            "teamId": team_id,
            "ffId": ff_id,
            "topic": topic,
            "bytes": len(payload_bytes),
            "observedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        }
        client.publish("edge/ecg_meta", json.dumps(meta), qos=0, retain=False)

        logging.info("📦 RAW ECG stored: team=%s ff=%s bytes=%d", team_id, ff_id, len(payload_bytes))
        return

    # ---- JSON telemetry ----
    try:
        payload_str = msg.payload.decode("utf-8")
    except UnicodeDecodeError:
        logging.error("Received non-UTF8 payload on topic %s (not raw/ECG). Dropping.", topic)
        return

    try:
        payload = json.loads(payload_str)
    except json.JSONDecodeError:
        logging.warning("Payload not valid JSON on %s; storing skipped.", topic)
        return

    entity_id, entity_type, team_id, ff_or_node = _topic_to_entity(topic)
    if not entity_id or not entity_type:
        logging.warning("Unknown topic pattern (ignored): %s", topic)
        return

    # Store
    store_measurement(entity_id, entity_type, topic, payload)

    # Convert to NGSI v2 and send to Orion
    entity = build_ngsi_v2_entity(entity_id, entity_type, payload)
    ok = send_to_context_broker_op_update(entity)
    if ok:
        logging.info("*** Sent %s (%s) to Orion", entity_id, entity_type)
    else:
        logging.warning("!!! Failed sending %s (%s) to Orion", entity_id, entity_type)

    # ---- Rule Engine ----
    alert = rule_engine_evaluate(entity_id, entity_type, payload)
    if alert:
        # publish to local frontend via MQTT
        client.publish(ALERTS_MQTT_TOPIC, json.dumps(alert), qos=0, retain=False)

        # also store in Orion as Alert entity
        alert_entity = build_alert_entity(alert)
        send_to_context_broker_op_update(alert_entity)

        logging.info("<!> ALERT: %s", alert)


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
            logging.error("MQTT connection error: %s. Reconnecting in 5 seconds...", e)
            time.sleep(5)


if __name__ == "__main__":
    main()
