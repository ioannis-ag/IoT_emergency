from __future__ import annotations

import os
import time
import base64
import threading
from datetime import datetime, timedelta, timezone
from typing import Any, Dict, Optional, List

import requests
from fastapi import FastAPI, HTTPException, Query, WebSocket, WebSocketDisconnect
from influxdb_client import InfluxDBClient

try:
    import paho.mqtt.client as mqtt
except Exception:  # pragma: no cover
    mqtt = None

app = FastAPI()

# ---- ENV ----
INFLUX_URL = os.getenv("INFLUX_URL", "http://influxdb:8086")
INFLUX_TOKEN = os.getenv("INFLUX_TOKEN", "")
INFLUX_ORG = os.getenv("INFLUX_ORG", "")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET", "")

WEATHER_AGENT_URL = os.getenv("WEATHER_AGENT_URL", "http://weather-agent:8000")
ORION_BASE = os.getenv("ORION_BASE", "http://orion:1026")

# MQTT for raw ECG (command-center side)
MQTT_HOST = os.getenv("MQTT_HOST", "192.168.2.12")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))

# Measurements (Influx)
MEAS_ENV = os.getenv("MEAS_ENV", "Environment")
MEAS_BIO = os.getenv("MEAS_BIO", "Biomedical")
MEAS_LOC = os.getenv("MEAS_LOC", "Location")
MEAS_ALERTS = os.getenv("MEAS_ALERTS", "Alerts")

def _now_utc() -> datetime:
    return datetime.now(timezone.utc)

def _iso(dt: datetime) -> str:
    return dt.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")

def _safe_float(x) -> Optional[float]:
    try:
        if x is None:
            return None
        return float(x)
    except Exception:
        return None

def _need_influx():
    if not (INFLUX_TOKEN and INFLUX_ORG and INFLUX_BUCKET):
        raise HTTPException(500, "Missing INFLUX_TOKEN/INFLUX_ORG/INFLUX_BUCKET in environment")

def _influx_client() -> InfluxDBClient:
    _need_influx()
    return InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)

@app.get("/api/health")
def health():
    return {"ok": True, "ts": _iso(_now_utc())}

@app.get("/api/latest")
def latest(team: str = Query(...), minutes: int = Query(60, ge=1, le=240)):
    """
    Returns:
      { team: "Team_A", members: [ {teamId, ffId, hrBpm, tempC, mq2Raw, lat, lon, stressIndex, ...}, ... ] }

    Expects Influx points tagged with: teamId, ffId
    across measurements Environment/Biomedical/Location.
    """
    _need_influx()

    since = _now_utc() - timedelta(minutes=minutes)
    start = since.isoformat()

    # include extra fields (risk + env + hrv)
    wanted_fields = [
        "hrBpm", "rrMs",
        "tempC", "humidityPct", "mq2Raw", "mq2Digital", "coPpm",
        "stressIndex", "fatigueIndex", "riskScore",
        "heatRisk", "heatIndexC", "gasRisk", "separationRisk",
        "rmssdMs", "sdnnMs", "pnn50Pct",
        "lat", "lon",
    ]

    field_filter = " or ".join([f'r["_field"] == "{f}"' for f in wanted_fields])

    flux = f'''
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: {start})
  |> filter(fn: (r) => r["teamId"] == "{team}")
  |> filter(fn: (r) =>
      r["_measurement"] == "{MEAS_ENV}" or
      r["_measurement"] == "{MEAS_BIO}" or
      r["_measurement"] == "{MEAS_LOC}"
  )
  |> filter(fn: (r) => {field_filter})
  |> keep(columns: ["_time","_measurement","_field","_value","ffId","teamId"])
  |> sort(columns: ["_time"], desc: false)
'''

    members: Dict[str, Dict[str, Any]] = {}

    with _influx_client() as client:
        q = client.query_api()
        tables = q.query(flux, org=INFLUX_ORG)

    for table in tables:
        for rec in table.records:
            ff = rec.values.get("ffId")
            if not ff:
                continue

            m = members.setdefault(ff, {"teamId": team, "ffId": ff})
            field = rec.get_field()
            val = rec.get_value()
            t = rec.get_time()
            if t:
                m["observedAt"] = _iso(t)

            fval = _safe_float(val)
            if fval is None:
                continue

            m[field] = fval

    return {"team": team, "members": list(members.values())}

@app.get("/api/weather")
def weather(lat: float = Query(...), lon: float = Query(...)):
    """Proxies weather-agent: GET /weather?lat=..&lon=.."""
    try:
        r = requests.get(f"{WEATHER_AGENT_URL}/weather", params={"lat": lat, "lon": lon}, timeout=8)
    except Exception as e:
        raise HTTPException(502, f"Weather agent unreachable: {e}")

    if r.status_code != 200:
        raise HTTPException(502, f"Weather agent error: {r.status_code} {r.text}")

    return r.json()

@app.post("/api/action")
def action(payload: Dict[str, Any]):
    """
    Stores a command action to Orion (so it can be logged / bridged / displayed).
    payload: {teamId, ffId?, action, note?}
    """
    team = payload.get("teamId")
    if not team:
        raise HTTPException(400, "teamId is required")
    ff = payload.get("ffId") or "ALL"
    action_name = payload.get("action") or "UNKNOWN"
    note = payload.get("note") or ""
    ts = int(time.time())

    eid = f"CommandAction:{team}:{ff}:{ts}"
    ent = {
        "id": eid,
        "type": "CommandAction",
        "teamId": {"type": "Text", "value": team},
        "ffId": {"type": "Text", "value": ff},
        "action": {"type": "Text", "value": str(action_name)},
        "note": {"type": "Text", "value": str(note)[:500]},
        "observedAt": {"type": "Text", "value": _iso(_now_utc())},
        "timestamp": {"type": "Number", "value": ts},
    }

    try:
        r = requests.post(
            f"{ORION_BASE}/v2/op/update",
            json={"actionType": "append", "entities": [ent]},
            timeout=5,
        )
    except Exception as e:
        raise HTTPException(502, f"Orion unreachable: {e}")

    if r.status_code not in (200, 201, 204):
        raise HTTPException(502, f"Orion error: {r.status_code} {r.text[:200]}")

    return {"ok": True, "id": eid}

@app.get("/api/alerts")
def alerts(team: str = Query(...), minutes: int = Query(180, ge=1, le=1440), limit: int = Query(200, ge=1, le=2000)):
    """
    Returns alerts from Influx (if you persist them).
    Expected tags: teamId, ffId. Expected fields: severity,title,detail (or similar).
    If you don't have this measurement, you'll get an empty list.
    """
    _need_influx()
    since = _now_utc() - timedelta(minutes=minutes)
    start = since.isoformat()

    flux = f'''
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: {start})
  |> filter(fn: (r) => r["_measurement"] == "{MEAS_ALERTS}")
  |> filter(fn: (r) => r["teamId"] == "{team}")
  |> keep(columns: ["_time","_field","_value","ffId","teamId"])
  |> sort(columns: ["_time"], desc: true)
'''

    # Build latest alert rows by grouping on time+ffId if your alerts are written that way.
    # If your structure differs, this still returns best-effort.
    rows: List[Dict[str, Any]] = []
    acc: Dict[str, Dict[str, Any]] = {}

    with _influx_client() as client:
        q = client.query_api()
        tables = q.query(flux, org=INFLUX_ORG)

    for table in tables:
        for rec in table.records:
            ff = rec.values.get("ffId") or ""
            t = rec.get_time()
            if not t:
                continue
            k = f"{ff}|{t.isoformat()}"
            row = acc.setdefault(k, {"teamId": team, "ffId": ff, "observedAt": _iso(t)})
            field = rec.get_field()
            val = rec.get_value()
            if isinstance(val, (int, float)):
                row[field] = float(val)
            else:
                row[field] = str(val)

    # take most recent first
    rows = list(acc.values())
    rows.sort(key=lambda r: r.get("observedAt",""), reverse=True)
    rows = rows[:limit]

    # normalize keys if present
    for r in rows:
        if "severity" not in r and "worst" in r:
            r["severity"] = r.get("worst")
        if "title" not in r and "category" in r:
            r["title"] = r.get("category")

    return {"team": team, "alerts": rows}

# ---------------- WebSocket ECG bridge ----------------
# The browser decodes the ECG bundle. This endpoint just forwards MQTT bytes.
@app.websocket("/ws/ecg")
async def ws_ecg(ws: WebSocket):
    """Stream raw ECG bytes from MQTT topic raw/ECG/<team>/<ff> over WebSocket."""
    # NOTE: accept exactly once
    await ws.accept()

    team = ws.query_params.get("team")
    ff = ws.query_params.get("ff")

    if not team or not ff:
        await ws.send_text("ERROR: missing team or ff query params")
        await ws.close(code=1008)
        return

    if mqtt is None:
        await ws.send_text("ERROR: paho-mqtt not installed in api container")
        await ws.close(code=1011)
        return

    import asyncio
    import threading

    topic = f"raw/ECG/{team}/{ff}"

    # Small async queue to avoid memory blowups if UI is slow
    q: asyncio.Queue[bytes] = asyncio.Queue(maxsize=64)
    stop = threading.Event()

    def mqtt_worker():
        try:
            client = mqtt.Client()
            def on_connect(c, u, flags, rc, properties=None):
                try:
                    c.subscribe(topic, qos=0)
                except Exception:
                    pass

            def on_message(c, u, msg):
                if stop.is_set():
                    return
                payload = msg.payload or b""
                try:
                    # drop if queue full
                    q.put_nowait(payload)
                except Exception:
                    pass

            client.on_connect = on_connect
            client.on_message = on_message

            # Prefer env vars if present; fallback to localhost
            host = os.environ.get("MQTT_HOST", "localhost")
            port = int(os.environ.get("MQTT_PORT", "1883"))
            client.connect(host, port, keepalive=30)
            client.loop_start()

            # wait until stop
            stop.wait()

            try:
                client.loop_stop()
            except Exception:
                pass
            try:
                client.disconnect()
            except Exception:
                pass
        except Exception:
            # If MQTT fails, we just stop producing; WS side will idle
            return

    t = threading.Thread(target=mqtt_worker, daemon=True)
    t.start()

    try:
        while True:
            payload = await q.get()
            # Send raw bytes exactly as received
            await ws.send_bytes(payload)
    except WebSocketDisconnect:
        pass
    except Exception:
        # Don't crash the server on unexpected WS errors
        try:
            await ws.close(code=1011)
        except Exception:
            pass
    finally:
        stop.set()
