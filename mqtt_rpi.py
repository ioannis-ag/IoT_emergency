#!/usr/bin/env python3
import json
import logging
import sqlite3
import time
from datetime import datetime

import paho.mqtt.client as mqtt
import requests

# ----------------- CONFIG ----------------- #
# MQTT broker (running on the Pi)
MQTT_BROKER_HOST = "localhost"
MQTT_BROKER_PORT = 1883
MQTT_TOPICS = [("iot/#", 0)]  # subscribe to everything under iot/...

# Orion Context Broker (NGSI-v2)
CONTEXT_BROKER_BASE_URL = "http://192.168.2.12:1026"
FIWARE_SERVICE = None       # e.g. "firefighters" or None
FIWARE_SERVICEPATH = None   # e.g. "/" or None

# SQLite local DB
SQLITE_DB_PATH = "edge_data.db"

# InfluxDB v2 (HISTORICAL TIME SERIES)
INFLUXDB_BASE_URL = "http://192.168.2.12:8086"
INFLUXDB_ORG = "IoT"          # <--- CHECK THIS
INFLUXDB_BUCKET = "iot"       # bucket name in Influx
INFLUXDB_TOKEN = "vSYm7PnfhFsfuExZlUpPIib07AyWBfnnxyipDVRdta6QDIviBhjt3_cH6SW06O31vZnlVhhqr7Hy4p3gPpmkEA=="  # <--- CHECK THIS

# Logging
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s"
)

# ----------------- SQLITE SETUP ----------------- #

def init_db():
    conn = sqlite3.connect(SQLITE_DB_PATH)
    c = conn.cursor()
    c.execute(
        """
        CREATE TABLE IF NOT EXISTS measurements (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT,
            topic TEXT,
            payload_json TEXT,
            ts_utc TEXT
        );
        """
    )
    conn.commit()
    conn.close()


def store_message_in_db(device_id: str, topic: str, payload_json: str):
    conn = sqlite3.connect(SQLITE_DB_PATH)
    c = conn.cursor()
    ts_utc = datetime.utcnow().isoformat()
    c.execute(
        "INSERT INTO measurements (device_id, topic, payload_json, ts_utc) "
        "VALUES (?, ?, ?, ?);",
        (device_id, topic, payload_json, ts_utc)
    )
    conn.commit()
    conn.close()

# ----------------- ORION HELPERS ----------------- #

def build_ngsi_v2_entity(payload: dict) -> dict:
    """
    Build an NGSI-v2 entity from the MQTT JSON payload.

    Priority for entity id:
    - device_id (if present at top level)
    - node_id
    - firefighter_id
    """
    device_id = (
        payload.get("device_id")
        or payload.get("node_id")
        or payload.get("firefighter_id")
        or "unknown_device"
    )
    entity_type = payload.get("type", "WearableGateway")

    entity = {
        "id": device_id,
        "type": entity_type,
    }

    # Flatten nested dicts like polar/environment/metrics into attributes
    for key, value in payload.items():
        if key in ("device_id", "type"):
            continue

        if isinstance(value, dict):
            prefix = key
            for sub_key, sub_val in value.items():
                attr_name = f"{prefix}_{sub_key}"
                if isinstance(sub_val, (int, float)):
                    attr_type = "Number"
                elif isinstance(sub_val, bool):
                    attr_type = "Boolean"
                else:
                    attr_type = "Text"
                entity[attr_name] = {"value": sub_val, "type": attr_type}
        else:
            if isinstance(value, (int, float)):
                attr_type = "Number"
            elif isinstance(value, bool):
                attr_type = "Boolean"
            else:
                attr_type = "Text"
            entity[key] = {"value": value, "type": attr_type}

    return entity


def send_to_context_broker(payload: dict):
    entity = build_ngsi_v2_entity(payload)

    url = f"{CONTEXT_BROKER_BASE_URL}/v2/entities?options=upsert"
    headers = {
        "Content-Type": "application/json"
    }
    if FIWARE_SERVICE:
        headers["Fiware-Service"] = FIWARE_SERVICE
    if FIWARE_SERVICEPATH:
        headers["Fiware-ServicePath"] = FIWARE_SERVICEPATH

    try:
        r = requests.post(url, headers=headers, data=json.dumps(entity), timeout=5)
        if r.status_code not in (200, 201, 204):
            logging.warning(
                "Context broker responded with status %s: %s",
                r.status_code,
                r.text
            )
        else:
            logging.info("Sent entity %s to context broker", entity.get("id"))
    except Exception as e:
        logging.error("Error sending to context broker: %s", e)

# ----------------- INFLUXDB v2 HELPERS ----------------- #

def _escape_tag(value: str) -> str:
    return (
        str(value)
        .replace(" ", "\\ ")
        .replace(",", "\\,")
        .replace("=", "\\=")
    )


def write_to_influx(payload: dict, topic: str = None):
    """
    Write fused payload to InfluxDB v2.

    - measurement "polar" for heart-rate data
    - measurement "environment" for env data (incl. MQ2)
    - measurement "metrics" for stress / heat strain metrics
    - measurement "sensor_data" for extra numeric top-level fields
    """
    if not INFLUXDB_TOKEN or INFLUXDB_ORG == "YOUR_ORG_HERE":
        return  # not configured yet

    lines = []

    # Common tags
    node_id = payload.get("node_id", "unknown_node")
    firefighter_id = payload.get("firefighter_id", "unknown_ff")
    team_id = payload.get("team_id", "unknown_team")
    role = payload.get("role", "unknown_role")

    common_tags = [
        f"node_id={_escape_tag(node_id)}",
        f"firefighter_id={_escape_tag(firefighter_id)}",
        f"team_id={_escape_tag(team_id)}",
        f"role={_escape_tag(role)}",
    ]
    if topic:
        common_tags.append(f"topic={_escape_tag(topic)}")

    # --- Polar vitals ---
    polar = payload.get("polar")
    if isinstance(polar, dict):
        tags = common_tags + [f"device_id={_escape_tag(polar.get('device_id', 'unknown_polar'))}"]
        fields = []
        for key in ("heart_rate_bpm", "rr_interval_ms"):
            val = polar.get(key)
            if isinstance(val, (int, float)):
                fields.append(f"{key}={float(val)}")

        if fields:
            line = "polar," + ",".join(tags) + " " + ",".join(fields)
            lines.append(line)

    # --- Environment (including MQ2 fields) ---
    env = payload.get("environment")
    if isinstance(env, dict):
        tags = common_tags + [f"device_id={_escape_tag(env.get('device_id', 'unknown_env'))}"]
        fields = []
        for key in ("temp_c", "humidity_pct", "mq2_analog", "mq2_digital", "co_ppm"):
            val = env.get(key)
            if isinstance(val, (int, float)):
                fields.append(f"{key}={float(val)}")

        if fields:
            line = "environment," + ",".join(tags) + " " + ",".join(fields)
            lines.append(line)

    # --- Metrics (stress / heat strain etc.) ---
    metrics = payload.get("metrics")
    if isinstance(metrics, dict):
        tags = common_tags
        fields = []
        for key, val in metrics.items():
            if isinstance(val, (int, float)):
                fields.append(f"{key}={float(val)}")
        if fields:
            line = "metrics," + ",".join(tags) + " " + ",".join(fields)
            lines.append(line)

    # --- Any extra numeric top-level fields (optional) ---
    extra_fields = []
    for key, value in payload.items():
        if key in ("device_id", "type", "polar", "environment", "metrics"):
            continue
        if isinstance(value, (int, float)):
            extra_fields.append(f"{key}={float(value)}")
    if extra_fields:
        line = "sensor_data," + ",".join(common_tags) + " " + ",".join(extra_fields)
        lines.append(line)

    if not lines:
        return

    body = "\n".join(lines)

    params = {
        "org": INFLUXDB_ORG,
        "bucket": INFLUXDB_BUCKET,
        "precision": "s",
    }
    headers = {
        "Authorization": f"Token {INFLUXDB_TOKEN}",
        "Content-Type": "text/plain; charset=utf-8",
        "Accept": "application/json",
    }

    try:
        r = requests.post(
            f"{INFLUXDB_BASE_URL}/api/v2/write",
            params=params,
            headers=headers,
            data=body,
            timeout=5,
        )
        if r.status_code != 204:
            logging.warning("InfluxDB write failed: %s %s", r.status_code, r.text)
        else:
            logging.info("Wrote to InfluxDB:\n%s", body)
    except Exception as e:
        logging.error("Error writing to InfluxDB: %s", e)

# ----------------- MQTT CALLBACKS ----------------- #

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        logging.info("Connected to MQTT broker")
        for topic, qos in MQTT_TOPICS:
            client.subscribe(topic, qos)
            logging.info("Subscribed to topic: %s (qos=%s)", topic, qos)
    else:
        logging.error("Failed to connect to MQTT broker. rc=%s", rc)


def on_message(client, userdata, msg):
    try:
        payload_str = msg.payload.decode("utf-8")
    except UnicodeDecodeError:
        logging.error("Received non-UTF8 payload on topic %s", msg.topic)
        return

    logging.info("MQTT message on %s: %s", msg.topic, payload_str)

    # Try parse JSON
    try:
        payload = json.loads(payload_str)
    except json.JSONDecodeError:
        logging.warning("Payload is not valid JSON, storing raw only.")
        device_id = "unknown_device"
        store_message_in_db(device_id, msg.topic, payload_str)
        return

    # Decide a reasonable device id to store in SQLite
    device_id = (
        payload.get("device_id")
        or payload.get("node_id")
        or payload.get("firefighter_id")
        or "unknown_device"
    )

    # 1) Store raw JSON in local SQLite
    store_message_in_db(device_id, msg.topic, json.dumps(payload))

    # 2) Send current state to Orion Context Broker
    send_to_context_broker(payload)

    # 3) Store time-series point(s) in InfluxDB
    write_to_influx(payload, topic=msg.topic)

# ----------------- MAIN ----------------- #

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
