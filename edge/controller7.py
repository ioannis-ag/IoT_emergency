#!/usr/bin/env python3
import json
import logging
import math
import random
import threading
import time
from datetime import datetime, timezone
from typing import Any, Dict, Tuple, List, Optional

import paho.mqtt.client as mqtt
import requests

# =========================================================
# CONFIG
# =========================================================
MQTT_BROKER_HOST = "localhost"
MQTT_BROKER_PORT = 1883

MQTT_TOPICS = [
    ("ngsi/Environment/+/+", 0),
    ("ngsi/Biomedical/+/+", 0),
    ("ngsi/Location/+/+", 0),
    ("ngsi/Gateway/+", 0),
    ("raw/ECG/+/+", 0),
]

ORION_URL = "http://192.168.2.12:1026/v2/op/update"
ORION_TIMEOUT = 5

TEAM_ID = "Team_A"
REAL_FF_ID = "FF_A"

STATUS_TOPIC_PREFIX = "edge/status"
ALERTS_TOPIC_PREFIX = "edge/alerts"
INCIDENT_TOPIC_PREFIX = "edge/incident"   # NEW

FALLBACK_LAT = 38.2466
FALLBACK_LON = 21.7346

FAKE_LOC_ACCURACY_M = 6
FAKE_LOC_HZ = 1.0

# Human movement limits (walking / brisk)
MAX_SPEED_MPS = 1.35      # ~4.9 km/h
MAX_ACCEL_MPS2 = 0.35

# Alert behavior
PERSIST_SEC = 6.0
ALERT_COOLDOWN_SEC = 35.0
ESCALATION_BYPASS_SEC = 8.0

# Incident model
INCIDENT_RADIUS_M = 55.0
INCIDENT_LEAD_START_M = 180.0   # starts far ahead of A
INCIDENT_LEAD_MIN_M = 35.0      # eventually near A
INCIDENT_LEAD_SHRINK_MPS = 0.30 # lead shrinks over time => "A approaches"
INCIDENT_MIN_MOVE_M = 2.0       # ignore tiny changes in incident center (stability)

# Staging offsets relative to incident
STAGE_OFFSET_M = 18.0           # B/D stay outside radius by this margin
C_PUSH_IN_M = 10.0              # C may go inside by this amount (adventurous)

FF_NAMES = {
    "FF_A": "Alex (Leader)",
    "FF_B": "Maria",
    "FF_C": "Nikos (Crisis)",
    "FF_D": "Eleni",
}

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")

# =========================================================
# ORION HARD FILTER
# =========================================================
ORION_ALLOWED_ATTRS = {
    "lat", "lon", "accuracyM",
    "tempC", "humidityPct", "mq2Raw", "mq2Digital", "coPpm",
    "hrBpm", "wearableOk",
    "rssi", "battery", "failover", "via",
    "observedAt", "timestamp", "tst",
}

def filter_orion_payload(payload: Dict[str, Any]) -> Dict[str, Any]:
    clean = {}
    for k, v in payload.items():
        if k not in ORION_ALLOWED_ATTRS:
            continue
        if v is None:
            continue
        if isinstance(v, (list, dict)):
            continue
        if isinstance(v, (int, float)):
            if not math.isfinite(float(v)):
                continue
            clean[k] = v
            continue
        if isinstance(v, str):
            s = "".join(ch for ch in v if ch.isprintable()).strip()
            if s:
                clean[k] = s
    return clean

def ngsi_type(v):
    if isinstance(v, bool):
        return "Boolean"
    if isinstance(v, (int, float)):
        return "Number"
    return "Text"

def build_ngsi_entity(eid: str, etype: str, payload: Dict[str, Any]) -> Dict[str, Any]:
    ent = {"id": eid, "type": etype}
    for k, v in payload.items():
        ent[k] = {"type": ngsi_type(v), "value": v}
    ent["timestamp"] = {"type": "Number", "value": int(time.time())}
    return ent

def send_to_orion(entity: Dict[str, Any]):
    try:
        r = requests.post(
            ORION_URL,
            json={"actionType": "append", "entities": [entity]},
            timeout=ORION_TIMEOUT,
        )
        if r.status_code not in (200, 201, 204):
            logging.warning("Orion %s: %s", r.status_code, r.text[:200])
    except Exception as e:
        logging.error("Orion error: %s", e)

def topic_to_entity(topic: str):
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

# =========================================================
# STATE
# =========================================================
last_loc: Dict[Tuple[str, str], Dict[str, Any]] = {}
last_env: Dict[Tuple[str, str], Dict[str, Any]] = {}
last_bio: Dict[Tuple[str, str], Dict[str, Any]] = {}
last_seen: Dict[Tuple[str, str], float] = {}

# Track A motion to estimate heading (local meters)
_a_hist: List[Tuple[float, float, float]] = []  # [(t, lat, lon)] recent

# Incident state
incident = {
    "incidentId": "INC_1",
    "lat": None,
    "lon": None,
    "radiusM": INCIDENT_RADIUS_M,
    "leadM": INCIDENT_LEAD_START_M,
    "last_pub_ts": 0.0,
    "last_center_lat": None,
    "last_center_lon": None,
}

# Alert de-bounce + cooldown
_cond_start: Dict[Tuple[str, str, str], float] = {}
_last_alert_sig: Dict[Tuple[str, str], str] = {}
_last_alert_ts: Dict[Tuple[str, str], float] = {}

def nowz() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

def haversine_m(lat1, lon1, lat2, lon2) -> float:
    R = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dphi/2)**2 + math.cos(p1)*math.cos(p2)*math.sin(dl/2)**2
    return 2*R*math.asin(math.sqrt(a))

def meters_to_deg_lat(m): return m / 111_320.0
def meters_to_deg_lon(m, lat): return m / (111_320.0 * math.cos(math.radians(lat)) + 1e-9)

def clamp(v, lo, hi): return max(lo, min(hi, v))

def latlon_add_m(lat, lon, east_m, north_m):
    lat2 = lat + meters_to_deg_lat(north_m)
    lon2 = lon + meters_to_deg_lon(east_m, lat)
    return lat2, lon2

def latlon_to_local_m(lat, lon, ref_lat, ref_lon):
    # x=east, y=north
    x = (lon - ref_lon) * (111_320.0 * math.cos(math.radians(ref_lat)))
    y = (lat - ref_lat) * 111_320.0
    return x, y

def local_m_to_latlon(x, y, ref_lat, ref_lon):
    lat = ref_lat + meters_to_deg_lat(y)
    lon = ref_lon + meters_to_deg_lon(x, ref_lat)
    return lat, lon

# =========================================================
# INCIDENT MODEL
# =========================================================
def estimate_heading_unit(lat, lon) -> Tuple[float, float]:
    """
    Estimate A's direction of travel as a unit vector in local meters (east, north).
    If not enough history or too small motion, return a stable default.
    """
    # Keep last ~10 seconds
    tnow = time.time()
    while _a_hist and (tnow - _a_hist[0][0]) > 12.0:
        _a_hist.pop(0)

    if len(_a_hist) < 2:
        # Default: NE-ish
        return (0.7, 0.7)

    t0, lat0, lon0 = _a_hist[0]
    t1, lat1, lon1 = _a_hist[-1]
    dt = max(0.1, t1 - t0)

    # local delta in meters using current lat as ref
    dx, dy = latlon_to_local_m(lat1, lon1, lat0, lon0)
    dist = math.hypot(dx, dy)

    if dist < 2.5:  # basically stationary / jitter
        return (0.7, 0.7)

    ux = dx / dist
    uy = dy / dist
    return (ux, uy)

def update_incident_from_a(a_lat: float, a_lon: float, dt: float):
    """
    Incident is placed ahead of A along heading.
    The lead distance shrinks over time -> "A approaches".
    """
    # Shrink lead
    lead = float(incident["leadM"])
    lead = max(INCIDENT_LEAD_MIN_M, lead - INCIDENT_LEAD_SHRINK_MPS * dt)
    incident["leadM"] = lead

    ux, uy = estimate_heading_unit(a_lat, a_lon)

    # Place incident ahead (east/north)
    center_lat, center_lon = latlon_add_m(a_lat, a_lon, ux * lead, uy * lead)

    # Debounce tiny center movements for stability (optional)
    last_lat = incident["last_center_lat"]
    last_lon = incident["last_center_lon"]
    if last_lat is not None and last_lon is not None:
        moved = haversine_m(last_lat, last_lon, center_lat, center_lon)
        if moved < INCIDENT_MIN_MOVE_M:
            center_lat, center_lon = last_lat, last_lon

    incident["lat"] = center_lat
    incident["lon"] = center_lon
    incident["last_center_lat"] = center_lat
    incident["last_center_lon"] = center_lon

def publish_incident(client: mqtt.Client):
    if incident["lat"] is None or incident["lon"] is None:
        return
    pkt = {
        "teamId": TEAM_ID,
        "incidentId": incident["incidentId"],
        "lat": float(incident["lat"]),
        "lon": float(incident["lon"]),
        "radiusM": float(incident["radiusM"]),
        "leadM": float(incident["leadM"]),
        "observedAt": nowz(),
        "type": "Fire",
        "severity": "danger",
    }
    client.publish(f"{INCIDENT_TOPIC_PREFIX}/{TEAM_ID}", json.dumps(pkt), retain=True)

# =========================================================
# UI SUMMARY (matches front-end expectations)
# =========================================================
def _severity_rank(s: str) -> int:
    return {"ok": 0, "warn": 1, "danger": 2}.get(s, 0)

def build_ui_summary(team: str, ff: str):
    key = (team, ff)
    loc = last_loc.get(key, {})
    env = last_env.get(key, {})
    bio = last_bio.get(key, {})

    lat = loc.get("lat")
    lon = loc.get("lon")
    acc = loc.get("accuracyM")

    hr = bio.get("hrBpm")
    temp = env.get("tempC")
    mq2 = env.get("mq2Raw")

    now = time.time()
    seen = last_seen.get(key)
    last_seen_sec = int(now - seen) if seen else None

    # ---- Pulse levels ----
    pulse_level = pulse_hint = "—"
    if isinstance(hr, (int, float)):
        hr_i = int(hr)
        if hr_i < 130:
            pulse_level, pulse_hint = "Normal", f"{hr_i} bpm"
        elif hr_i < 160:
            pulse_level, pulse_hint = "Elevated", f"{hr_i} bpm"
        elif hr_i < 180:
            pulse_level, pulse_hint = "High", f"{hr_i} bpm"
        else:
            pulse_level, pulse_hint = "Critical", f"{hr_i} bpm"

    # ---- Heat levels ----
    heat_level = heat_hint = "—"
    if isinstance(temp, (int, float)):
        t = float(temp)
        if t < 45:
            heat_level, heat_hint = "OK", f"{t:.1f} °C"
        elif t < 50:
            heat_level, heat_hint = "Warm", f"{t:.1f} °C"
        elif t < 60:
            heat_level, heat_hint = "Hot", f"{t:.1f} °C"
        else:
            heat_level, heat_hint = "Danger", f"{t:.1f} °C"

    # ---- Smoke levels ----
    smoke_level = smoke_hint = "—"
    if isinstance(mq2, (int, float)):
        m = int(mq2)
        if m < 1800:
            smoke_level, smoke_hint = "Low", str(m)
        elif m < 2600:
            smoke_level, smoke_hint = "Moderate", str(m)
        elif m < 3200:
            smoke_level, smoke_hint = "High", str(m)
        else:
            smoke_level, smoke_hint = "Extreme", str(m)

    # ---- Distance to leader ----
    dist = None
    anchor = last_loc.get((team, REAL_FF_ID), {})
    al, ao = anchor.get("lat"), anchor.get("lon")
    if all(isinstance(x, (int, float)) for x in (lat, lon, al, ao)):
        dist = haversine_m(lat, lon, al, ao)

    # ---- Distance to incident ----
    dist_inc = None
    if incident["lat"] is not None and incident["lon"] is not None and isinstance(lat, (int, float)) and isinstance(lon, (int, float)):
        dist_inc = haversine_m(float(lat), float(lon), float(incident["lat"]), float(incident["lon"]))

    # ---- Determine base severity (from bio/env) ----
    severity = "ok"
    if pulse_level in ("High", "Critical") or heat_level in ("Hot", "Danger") or smoke_level in ("High", "Extreme"):
        severity = "warn"
    if pulse_level == "Critical" or heat_level == "Danger" or smoke_level == "Extreme":
        severity = "danger"

    # Location separation influences severity a bit
    if isinstance(dist, (int, float)):
        if dist >= 140:
            severity = "danger"
        elif dist >= 90 and _severity_rank(severity) < _severity_rank("warn"):
            severity = "warn"

    # Incident proximity influences severity (strong)
    if isinstance(dist_inc, (int, float)):
        r = float(incident["radiusM"])
        if dist_inc <= r:
            severity = "danger"
        elif dist_inc <= (r + 30) and _severity_rank(severity) < _severity_rank("warn"):
            severity = "warn"

    # =====================================================
    # Calm rule engine: require persistence before alert
    # =====================================================
    alerts: List[Dict[str, Any]] = []

    def _persist_ok(cond_key: str, cond_true: bool) -> bool:
        ck = (team, ff, cond_key)
        tnow = time.time()
        if cond_true:
            if ck not in _cond_start:
                _cond_start[ck] = tnow
            return (tnow - _cond_start[ck]) >= PERSIST_SEC
        else:
            _cond_start.pop(ck, None)
            return False

    def add_alert(sev, cat, title, detail):
        alerts.append({
            "teamId": team,
            "ffId": ff,
            "name": FF_NAMES.get(ff, ff),
            "severity": sev,          # ok/warn/danger
            "category": cat,          # health/environment/location/incident
            "title": title,
            "detail": detail,
            "observedAt": nowz(),
        })

    # ---- Health alerts ----
    if _persist_ok("pulse_crit", pulse_level == "Critical"):
        add_alert("danger", "health", "Pulse Critical", pulse_hint)
    elif _persist_ok("pulse_high", pulse_level == "High"):
        add_alert("warn", "health", "Pulse High", pulse_hint)

    # ---- Environment alerts ----
    if _persist_ok("heat_danger", heat_level == "Danger"):
        add_alert("danger", "environment", "Heat Danger", heat_hint)
    elif _persist_ok("heat_hot", heat_level == "Hot"):
        add_alert("warn", "environment", "Heat Rising", heat_hint)

    if _persist_ok("smoke_extreme", smoke_level == "Extreme"):
        add_alert("danger", "environment", "Smoke Extreme", smoke_hint)
    elif _persist_ok("smoke_high", smoke_level == "High"):
        add_alert("warn", "environment", "Smoke High", smoke_hint)

    # ---- Location alerts ----
    if isinstance(dist, (int, float)):
        if _persist_ok("sep_danger", dist >= 140):
            add_alert("danger", "location", "Separated", f"{int(dist)} m from leader")
        elif _persist_ok("sep_warn", dist >= 95):
            add_alert("warn", "location", "Drifting", f"{int(dist)} m from leader")

    # ---- Incident alerts ----
    if isinstance(dist_inc, (int, float)):
        r = float(incident["radiusM"])
        if _persist_ok("inc_inside", dist_inc <= r):
            add_alert("danger", "incident", "Inside Fire Zone", f"{int(dist_inc)} m from center (r={int(r)}m)")
        elif _persist_ok("inc_near", dist_inc <= (r + 30)):
            add_alert("warn", "incident", "Near Fire Zone", f"{int(dist_inc)} m from center (r={int(r)}m)")

    worst = "ok"
    for a in alerts:
        if _severity_rank(a["severity"]) > _severity_rank(worst):
            worst = a["severity"]

    summary = {
        "teamId": team,
        "ffId": ff,
        "name": FF_NAMES.get(ff, ff),

        "severity": severity,
        "worst": worst,
        "observedAt": nowz(),
        "lastSeenSec": last_seen_sec,

        "lat": lat,
        "lon": lon,
        "accuracyM": acc,

        "pulse": int(hr) if isinstance(hr, (int, float)) else None,
        "temp": float(temp) if isinstance(temp, (int, float)) else None,
        "gas": int(mq2) if isinstance(mq2, (int, float)) else None,

        "pulseLevel": pulse_level,
        "pulseHint": pulse_hint,
        "heatLevel": heat_level,
        "heatHint": heat_hint,
        "smokeLevel": smoke_level,
        "smokeHint": smoke_hint,

        "distanceToLeaderM": dist,
        "distanceToIncidentM": dist_inc,   # NEW for UI
    }

    alerts_packet = {
        "teamId": team,
        "ffId": ff,
        "name": FF_NAMES.get(ff, ff),
        "worst": worst,
        "alerts": alerts,
        "observedAt": nowz(),
    }

    return summary, alerts_packet

def publish_ui(client: mqtt.Client, team: str, ff: str):
    summary, alerts_packet = build_ui_summary(team, ff)
    client.publish(f"{STATUS_TOPIC_PREFIX}/{team}/{ff}", json.dumps(summary), retain=True)

    key = (team, ff)
    sig = json.dumps([(a["severity"], a["category"], a["title"], a["detail"]) for a in alerts_packet["alerts"]])

    ts = time.time()
    last_ts = _last_alert_ts.get(key, 0.0)
    can_publish = (ts - last_ts) >= ALERT_COOLDOWN_SEC

    prev_sig = _last_alert_sig.get(key)
    prev_worst = "ok"
    if prev_sig:
        try:
            prev = json.loads(prev_sig)
            for sev, *_ in prev:
                if _severity_rank(sev) > _severity_rank(prev_worst):
                    prev_worst = sev
        except Exception:
            prev_worst = "ok"

    if alerts_packet["worst"] == "danger" and (ts - last_ts) >= ESCALATION_BYPASS_SEC:
        can_publish = True

    if sig != prev_sig and can_publish:
        client.publish(f"{ALERTS_TOPIC_PREFIX}/{team}/{ff}", json.dumps(alerts_packet), retain=True)
        _last_alert_sig[key] = sig
        _last_alert_ts[key] = ts

# =========================================================
# FAKE LOCATION SIM (goal-seeking, smooth, no teleport)
# =========================================================
class GoalWalker:
    """
    Smoothly walks toward a moving goal with acceleration/speed limits.
    """
    def __init__(self, ff_id: str):
        self.ff = ff_id
        self.lat: Optional[float] = None
        self.lon: Optional[float] = None
        self.vx = 0.0  # m/s east
        self.vy = 0.0  # m/s north
        self._noise_phase = random.random() * 2 * math.pi

    def attach_near(self, lat, lon, r_m: float):
        ang = random.random() * 2 * math.pi
        self.lat, self.lon = latlon_add_m(lat, lon, math.cos(ang)*r_m, math.sin(ang)*r_m)
        self.vx = self.vy = 0.0

    def step_to_goal(self, dt: float, goal_lat: float, goal_lon: float):
        assert self.lat is not None and self.lon is not None

        # Work in local meters around current position
        gx, gy = latlon_to_local_m(goal_lat, goal_lon, self.lat, self.lon)
        dist = math.hypot(gx, gy) + 1e-6
        ux, uy = gx/dist, gy/dist

        # "Arrival": slow down near goal
        desired_speed = MAX_SPEED_MPS
        if dist < 10.0:
            desired_speed = MAX_SPEED_MPS * (dist / 10.0)

        desired_vx = ux * desired_speed
        desired_vy = uy * desired_speed

        # Steering acceleration toward desired velocity
        ax = (desired_vx - self.vx) * 1.2
        ay = (desired_vy - self.vy) * 1.2

        # Add gentle lateral noise so paths aren't perfectly straight
        self._noise_phase += 0.20 * dt
        nx = math.cos(self._noise_phase)
        ny = math.sin(self._noise_phase)
        ax += nx * 0.06
        ay += ny * 0.06

        # Clamp acceleration
        a = math.hypot(ax, ay)
        if a > MAX_ACCEL_MPS2:
            s = MAX_ACCEL_MPS2 / a
            ax *= s
            ay *= s

        # Integrate velocity
        self.vx += ax * dt
        self.vy += ay * dt

        # Clamp speed
        v = math.hypot(self.vx, self.vy)
        if v > MAX_SPEED_MPS:
            s = MAX_SPEED_MPS / v
            self.vx *= s
            self.vy *= s

        # Integrate position
        self.lat, self.lon = latlon_add_m(self.lat, self.lon, self.vx * dt, self.vy * dt)

        # tiny jitter (very small)
        j = 0.03
        self.lat, self.lon = latlon_add_m(self.lat, self.lon, (random.random()-0.5)*j, (random.random()-0.5)*j)

        return {
            "teamId": TEAM_ID,
            "ffId": self.ff,
            "lat": self.lat,
            "lon": self.lon,
            "accuracyM": FAKE_LOC_ACCURACY_M,
            "observedAt": nowz(),
            "source": "fake",
        }

# Create walkers
walker_B = GoalWalker("FF_B")
walker_C = GoalWalker("FF_C")
walker_D = GoalWalker("FF_D")

def incident_targets(a_lat: float, a_lon: float) -> Dict[str, Tuple[float, float]]:
    """
    Compute targets for B, C, D relative to incident and A.
    B/D: stay outside radius on A-facing side, with slight lateral separation.
    C: goes closer; sometimes slightly inside the radius.
    """
    ic_lat = float(incident["lat"])
    ic_lon = float(incident["lon"])
    r = float(incident["radiusM"])

    # Vector from incident -> A in local meters (A-facing direction)
    vx, vy = latlon_to_local_m(a_lat, a_lon, ic_lat, ic_lon)
    dist = math.hypot(vx, vy) + 1e-6
    ux, uy = vx/dist, vy/dist  # unit from incident toward A

    # A-facing point just outside perimeter
    base_out_x = ux * (r + STAGE_OFFSET_M)
    base_out_y = uy * (r + STAGE_OFFSET_M)

    # Perp unit for lateral spacing
    px, py = -uy, ux

    # B and D: outside radius, slight lateral offsets, no crossing
    b_x = base_out_x + px * 16.0
    b_y = base_out_y + py * 16.0

    d_x = base_out_x - px * 14.0
    d_y = base_out_y - py * 14.0

    # C: closer (and a little adventurous)
    # oscillate between just outside and slightly inside
    t = time.time()
    pulse = 0.5 + 0.5 * math.sin(t * 0.18)  # slow oscillation [0..1]
    desired_r = (r + 6.0) - pulse * (C_PUSH_IN_M + 6.0)  # ranges about (r+6) to (r-10)
    c_x = ux * desired_r + px * 6.0
    c_y = uy * desired_r + py * 6.0

    # Convert these local meters offsets (relative to incident) into lat/lon
    B_lat, B_lon = local_m_to_latlon(b_x, b_y, ic_lat, ic_lon)
    C_lat, C_lon = local_m_to_latlon(c_x, c_y, ic_lat, ic_lon)
    D_lat, D_lon = local_m_to_latlon(d_x, d_y, ic_lat, ic_lon)

    return {"FF_B": (B_lat, B_lon), "FF_C": (C_lat, C_lon), "FF_D": (D_lat, D_lon)}

def fake_loop(client):
    logging.info("Fake sim: waiting for real FF_A location ...")
    while (TEAM_ID, REAL_FF_ID) not in last_loc:
        time.sleep(0.5)

    # Init anchor from A
    al = last_loc[(TEAM_ID, REAL_FF_ID)].get("lat", FALLBACK_LAT)
    ao = last_loc[(TEAM_ID, REAL_FF_ID)].get("lon", FALLBACK_LON)
    logging.info("Fake sim: anchor acquired (%.6f, %.6f)", al, ao)

    # Initialize incident once and walkers near A
    update_incident_from_a(al, ao, dt=1.0)
    publish_incident(client)

    walker_B.attach_near(al, ao, 30.0)
    walker_C.attach_near(al, ao, 45.0)
    walker_D.attach_near(al, ao, 28.0)

    period = 1.0 / FAKE_LOC_HZ
    last_t = time.time()

    while True:
        t = time.time()
        dt = max(0.10, min(1.0, t - last_t))
        last_t = t

        # Update A (for heading estimation)
        a = last_loc.get((TEAM_ID, REAL_FF_ID))
        if a and isinstance(a.get("lat"), (int, float)) and isinstance(a.get("lon"), (int, float)):
            al, ao = float(a["lat"]), float(a["lon"])
            _a_hist.append((time.time(), al, ao))

        # Update incident based on A
        if isinstance(al, float) and isinstance(ao, float):
            update_incident_from_a(al, ao, dt)

        # Publish incident periodically (or always since retained; cheap)
        publish_incident(client)

        # Compute targets around incident
        if incident["lat"] is not None and incident["lon"] is not None:
            targets = incident_targets(al, ao)

            # Step walkers smoothly toward targets
            for ff, (glat, glon) in targets.items():
                if ff == "FF_B":
                    p = walker_B.step_to_goal(dt, glat, glon)
                elif ff == "FF_C":
                    p = walker_C.step_to_goal(dt, glat, glon)
                else:
                    p = walker_D.step_to_goal(dt, glat, glon)

                client.publish(f"ngsi/Location/{TEAM_ID}/{ff}", json.dumps(p), retain=True)
                last_loc[(TEAM_ID, ff)] = {"lat": p["lat"], "lon": p["lon"], "accuracyM": p["accuracyM"]}
                last_seen[(TEAM_ID, ff)] = time.time()
                publish_ui(client, TEAM_ID, ff)

        time.sleep(period)

# =========================================================
# MQTT
# =========================================================
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        logging.info("✅ Connected to MQTT broker")
        for tpc, q in MQTT_TOPICS:
            client.subscribe(tpc, q)
    else:
        logging.error("❌ MQTT connect failed rc=%s", rc)

def on_message(client, userdata, msg):
    if msg.topic.startswith("raw/ECG/"):
        return

    try:
        payload = json.loads(msg.payload.decode())
    except Exception:
        return

    eid, etype, team, ff = topic_to_entity(msg.topic)
    if not eid:
        return

    # Orion
    filtered = filter_orion_payload(payload)
    if filtered:
        send_to_orion(build_ngsi_entity(eid, etype, filtered))

    # State + UI
    if etype == "Location" and team and ff:
        if "accuracyM" not in payload and "acc" in payload:
            payload["accuracyM"] = payload.get("acc")

        if isinstance(payload.get("lat"), (int, float)) and isinstance(payload.get("lon"), (int, float)):
            last_loc[(team, ff)] = {
                "lat": float(payload.get("lat")),
                "lon": float(payload.get("lon")),
                "accuracyM": payload.get("accuracyM"),
            }
            last_seen[(team, ff)] = time.time()

            # Track A movement for heading
            if ff == REAL_FF_ID and team == TEAM_ID:
                _a_hist.append((time.time(), float(payload.get("lat")), float(payload.get("lon"))))

            publish_ui(client, team, ff)

    elif etype == "Environment" and team and ff:
        last_env[(team, ff)] = payload
        last_seen[(team, ff)] = time.time()
        publish_ui(client, team, ff)

    elif etype == "Biomedical" and team and ff:
        last_bio[(team, ff)] = payload
        last_seen[(team, ff)] = time.time()
        publish_ui(client, team, ff)

# =========================================================
# MAIN
# =========================================================
def main():
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_BROKER_HOST, MQTT_BROKER_PORT, keepalive=60)

    threading.Thread(target=fake_loop, args=(client,), daemon=True).start()
    client.loop_forever()

if __name__ == "__main__":
    main()
