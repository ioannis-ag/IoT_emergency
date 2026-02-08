#!/usr/bin/env python3
import json
import logging
import sqlite3
import time
from datetime import datetime

import paho.mqtt.client as mqtt
import requests

# ----------------- CONFIG ----------------- #
MQTT_BROKER_HOST = "localhost"
MQTT_BROKER_PORT = 1883
MQTT_TOPICS = [("ff/#", 0)]  # subscribe to ff telemetry

CONTEXT_BROKER_BASE_URL = "http://192.168.2.12:1026"
FIWARE_SERVICE = None
FIWARE_SERVICEPATH = None

SQLITE_DB_PATH = "edge_data.db"
ENABLE_SQLITE = True

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")

# ----------------- SQLITE ----------------- #
def init_db():
    if not ENABLE_SQLITE:
        return
    conn = sqlite3.connect(SQLITE_DB_PATH)
    c = conn.cursor()
    c.execute("""
        CREATE TABLE IF NOT EXISTS measurements (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT,
            topic TEXT,
            payload_json TEXT,
            ts_utc TEXT
        );
    """)
    conn.commit()
    conn.close()

def store_message_in_db(device_id: str, topic: str, payload_json: str):
    if not ENABLE_SQLITE:
        return
    conn = sqlite3.connect(SQLITE_DB_PATH)
    c = conn.cursor()
    ts_utc = datetime.utcnow().isoformat()
    c.execute(
        "INSERT INTO measurements (device_id, topic, payload_json, ts_utc) VALUES (?, ?, ?, ?);",
        (device_id, topic, payload_json, ts_utc),
    )
    conn.commit()
    conn.close()

# ----------------- ORION HELPERS ----------------- #
def _attr_type(v):
    # IMPORTANT: bool must be checked before int/float in Python
    if isinstance(v, bool):
        return "Boolean"
    if isinstance(v, (int, float)):
        return "Number"
    return "Text"

def _entity_id_from_topic_or_payload(topic: str, payload: dict) -> str:
    # expected topic: ff/A/telemetry
    parts = topic.split("/")
    if len(parts) >= 2 and parts[0] == "ff":
        return f"ff_{parts[1]}"  # ff_A, ff_B, ...
    # fallback to payload keys
    return (
        payload.get("device_id")
        or payload.get("node_id")
        or payload.get("node")          # <--- FIX for your ff payloads
        or payload.get("firefighter_id")
        or "unknown_device"
    )

def build_ngsi_v2_entity(topic: str, payload: dict) -> dict:
    entity_id = _entity_id_from_topic_or_payload(topic, payload)
    entity_type = payload.get("type", "WearableGateway")

    # ensure a timestamp attribute exists (optional but helps)
    if "timestamp" not in payload and "tst" not in payload:
        payload["timestamp"] = int(time.time())

    entity = {"id": entity_id, "type": entity_type}

    for key, value in payload.items():
        if key in ("id", "type"):
            continue

        if isinstance(value, dict):
            # flatten nested dicts
            for sub_key, sub_val in value.items():
                attr_name = f"{key}_{sub_key}"
                entity[attr_name] = {"value": sub_val, "type": _attr_type(sub_val)}
        else:
            entity[key] = {"value": value, "type": _attr_type(value)}

    return entity

def send_to_context_broker(entity: dict):
    url = f"{CONTEXT_BROKER_BASE_URL}/v2/entities?options=upsert"
    headers = {"Content-Type": "application/json"}
    if FIWARE_SERVICE:
        headers["Fiware-Service"] = FIWARE_SERVICE
    if FIWARE_SERVICEPATH:
        headers["Fiware-ServicePath"] = FIWARE_SERVICEPATH

    try:
        r = requests.post(url, headers=headers, json=entity, timeout=5)
        if r.status_code not in (200, 201, 204):
            logging.warning("Orion responded %s: %s", r.status_code, r.text)
        else:
            logging.info("✅ Sent entity %s to Orion", entity.get("id"))
    except Exception as e:
        logging.error("❌ Error sending to Orion: %s", e)

# ----------------- MQTT CALLBACKS ----------------- #
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        logging.info("✅ Connected to MQTT broker")
        for topic, qos in MQTT_TOPICS:
            client.subscribe(topic, qos)
            logging.info("Subscribed to topic: %s (qos=%s)", topic, qos)
    else:
        logging.error("❌ Failed to connect to MQTT broker. rc=%s", rc)

def on_message(client, userdata, msg):
    try:
        payload_str = msg.payload.decode("utf-8")
    except UnicodeDecodeError:
        logging.error("Received non-UTF8 payload on topic %s", msg.topic)
        return

    logging.info("MQTT message on %s: %s", msg.topic, payload_str)

    try:
        payload = json.loads(payload_str)
    except json.JSONDecodeError:
        logging.warning("Payload not valid JSON; storing raw only.")
        store_message_in_db("unknown_device", msg.topic, payload_str)
        return

    entity = build_ngsi_v2_entity(msg.topic, payload)

    store_message_in_db(entity["id"], msg.topic, json.dumps(payload))
    send_to_context_broker(entity)

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
