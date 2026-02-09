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

# ===================== CONFIG ===================== #
MQTT_BROKER_HOST = "localhost"
MQTT_BROKER_PORT = 1883

MQTT_TOPICS = [
    ("ngsi/Environment/+/+", 0),
    ("ngsi/Biomedical/+/+", 0),
    ("ngsi/Location/+/+", 0),
    ("ngsi/Gateway/+", 0),
    ("raw/ECG/+/+", 0),
]

CONTEXT_BROKER_BASE_URL = "http://192.168.2.12:1026"
FIWARE_SERVICE = None
FIWARE_SERVICEPATH = None

SQLITE_DB_PATH = "edge_data.db"
ENABLE_SQLITE = True
ORION_TIMEOUT_SEC = 5

ALERTS_TOPIC_PREFIX = "edge/alerts"
STATUS_TOPIC_PREFIX = "edge/status"

TEAM_ID = "Team_A"
REAL_FF_ID = "FF_A"
FAKE_FFS = ["FF_B", "FF_C", "FF_D"]

FAKE_LOC_PUB_HZ = 1.0
FAKE_LOC_ACCURACY_M = 8
MAX_SPEED_MPS = 1.6
MAX_ACCEL_MPS2 = 0.6
KEEP_WITHIN_M = 250

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

def store_measurement(entity_id, entity_type, topic, payload):
    if not ENABLE_SQLITE:
        return
    conn = sqlite3.connect(SQLITE_DB_PATH)
    c = conn.cursor()
    c.execute(
        "INSERT INTO measurements (entity_id, entity_type, topic, payload_json, ts_utc) VALUES (?, ?, ?, ?, ?)",
        (entity_id, entity_type, topic, json.dumps(payload), datetime.now(timezone.utc).isoformat()),
    )
    conn.commit()
    conn.close()

def store_raw_ecg(team_id, ff_id, topic, payload_bytes):
    if not ENABLE_SQLITE:
        return
    conn = sqlite3.connect(SQLITE_DB_PATH)
    c = conn.cursor()
    c.execute(
        "INSERT INTO raw_ecg VALUES (NULL, ?, ?, ?, ?, ?, ?)",
        (
            team_id,
            ff_id,
            topic,
            base64.b64encode(payload_bytes).decode(),
            len(payload_bytes),
            datetime.now(timezone.utc).isoformat(),
        ),
    )
    conn.commit()
    conn.close()

# ===================== NGSI ===================== #
def _attr_type(v):
    if isinstance(v, bool):
        return "Boolean"
    if isinstance(v, (int, float)):
        return "Number"
    return "Text"

def _topic_to_entity(topic):
    p = topic.split("/")
    if len(p) == 4 and p[0] == "ngsi":
        _, kind, team, ff = p
        if kind == "Environment":
            return f"EnvNode:{team}:{ff}", "Environment", team, ff
        if kind == "Biomedical":
            return f"Wearable:{team}:{ff}", "Biomedical", team, ff
        if kind == "Location":
            return f"Phone:{team}:{ff}", "Location", team, ff
    if len(p) == 3 and p[1] == "Gateway":
        return f"Gateway:{p[2]}", "Gateway", None, p[2]
    return None, None, None, None

def build_ngsi_v2_entity(eid, etype, payload):
    ent = {"id": eid, "type": etype}
    for k, v in payload.items():
        if k in ("id", "type"):
            continue
        ent[k] = {"type": _attr_type(v), "value": v}
    ent.setdefault("timestamp", {"type": "Number", "value": int(time.time())})
    return ent

def send_to_orion(ent):
    try:
        r = requests.post(
            f"{CONTEXT_BROKER_BASE_URL}/v2/op/update",
            json={"actionType": "append", "entities": [ent]},
            timeout=ORION_TIMEOUT_SEC,
        )
        if r.status_code not in (200, 201, 204):
            logging.warning("Orion %s: %s", r.status_code, r.text[:200])
    except Exception as e:
        logging.error("Orion error: %s", e)

# ===================== RULE ENGINE ===================== #
@dataclass
class Thresholds:
    hr_warn: int = 160
    hr_danger: int = 180
    sep_warn: float = 60
    sep_danger: float = 110

TH = Thresholds()

last_seen = {}
last_env = {}
last_bio = {}
last_loc = {}

def _nowz():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

def haversine(lat1, lon1, lat2, lon2):
    R = 6371000
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dphi/2)**2 + math.cos(p1)*math.cos(p2)*math.sin(dl/2)**2
    return 2*R*math.asin(math.sqrt(a))

def evaluate(team, ff):
    alerts = []
    key = (team, ff)

    loc = last_loc.get(key, {})
    anchor = last_loc.get((team, REAL_FF_ID), {})
    if loc and anchor:
        d = haversine(loc["lat"], loc["lon"], anchor["lat"], anchor["lon"])
        if d > TH.sep_danger:
            alerts.append({"teamId": team, "ffId": ff, "severity": "danger", "category": "location", "reason": f"Separated ({int(d)}m)", "observedAt": _nowz()})
        elif d > TH.sep_warn:
            alerts.append({"teamId": team, "ffId": ff, "severity": "warn", "category": "location", "reason": f"Drifting ({int(d)}m)", "observedAt": _nowz()})

    summary = {
        "teamId": team,
        "ffId": ff,
        "status": "danger" if any(a["severity"] == "danger" for a in alerts) else "normal",
        "observedAt": _nowz(),
        **loc,
    }
    return summary, alerts

# ===================== FAKE LOCATION ===================== #
def meters_to_lat(m): return m / 111320
def meters_to_lon(m, lat): return m / (111320 * math.cos(math.radians(lat)))

class FakeSim:
    def __init__(self, ff, crisis=False):
        self.ff = ff
        self.crisis = crisis
        self.lat = FALLBACK_ANCHOR_LAT
        self.lon = FALLBACK_ANCHOR_LON
        self.vx = self.vy = 0

    def step(self, dt, al, ao):
        drift = 1.0 if self.crisis else 0.3
        self.vx += random.uniform(-0.3, 0.3) * drift
        self.vy += random.uniform(-0.3, 0.3) * drift
        self.lat += meters_to_lat(self.vy * dt)
        self.lon += meters_to_lon(self.vx * dt, self.lat)
        return {
            "teamId": TEAM_ID,
            "ffId": self.ff,
            "lat": self.lat,
            "lon": self.lon,
            "accuracyM": FAKE_LOC_ACCURACY_M,
            "observedAt": _nowz(),
            "source": "fake",
        }

sims = {
    "FF_B": FakeSim("FF_B"),
    "FF_C": FakeSim("FF_C", crisis=True),
    "FF_D": FakeSim("FF_D"),
}

def fake_loop(client):
    while True:
        anchor = last_loc.get((TEAM_ID, REAL_FF_ID), {"lat": FALLBACK_ANCHOR_LAT, "lon": FALLBACK_ANCHOR_LON})
        for ff, sim in sims.items():
            p = sim.step(1.0, anchor["lat"], anchor["lon"])
            client.publish(f"ngsi/Location/{TEAM_ID}/{ff}", json.dumps(p), retain=True)
            last_loc[(TEAM_ID, ff)] = {"lat": p["lat"], "lon": p["lon"], "accuracyM": p["accuracyM"]}
            summary, alerts = evaluate(TEAM_ID, ff)
            client.publish(f"{STATUS_TOPIC_PREFIX}/{TEAM_ID}/{ff}", json.dumps(summary), retain=True)
            for a in alerts:
                client.publish(f"{ALERTS_TOPIC_PREFIX}/{TEAM_ID}/{ff}", json.dumps(a))
        time.sleep(1)

# ===================== MQTT ===================== #
def on_connect(client, userdata, flags, rc):
    for t, q in MQTT_TOPICS:
        client.subscribe(t, q)

def on_message(client, userdata, msg):
    if msg.topic.startswith("raw/ECG"):
        return

    try:
        payload = json.loads(msg.payload.decode())
    except Exception:
        return

    eid, etype, team, ff = _topic_to_entity(msg.topic)
    if not eid:
        return

    store_measurement(eid, etype, msg.topic, payload)
    send_to_orion(build_ngsi_v2_entity(eid, etype, payload))

    if etype == "Location":
        last_loc[(team, ff)] = {"lat": payload["lat"], "lon": payload["lon"], "accuracyM": payload.get("accuracyM", 10)}

# ===================== MAIN ===================== #
def main():
    init_db()
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_BROKER_HOST, MQTT_BROKER_PORT)
    threading.Thread(target=fake_loop, args=(client,), daemon=True).start()
    client.loop_forever()

if __name__ == "__main__":
    main()
