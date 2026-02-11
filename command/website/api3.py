from __future__ import annotations

import asyncio
import json
import os
import time
from datetime import datetime, timedelta, timezone
from typing import Any, Dict, Optional, List, Tuple

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

# Orion inside Docker doesn't resolve in your setup; default to host IP that works for you.
ORION_BASE = os.getenv("ORION_BASE", "http://192.168.2.12:1026")
WEATHER_ENTITY_ID = os.getenv("WEATHER_ENTITY_ID", "Weather:Patra")

# MQTT
MQTT_HOST = os.getenv("MQTT_HOST", "192.168.2.12")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))

# Measurements (Influx)
MEAS_ENV = os.getenv("MEAS_ENV", "Environment")
MEAS_BIO = os.getenv("MEAS_BIO", "Biomedical")
MEAS_LOC = os.getenv("MEAS_LOC", "Location")
MEAS_ALERTS = os.getenv("MEAS_ALERTS", "Alerts")
MEAS_WEATHER = os.getenv("MEAS_WEATHER", "WeatherObserved")

# Location cache behavior
LOC_CACHE_TTL_SEC = int(os.getenv("LOC_CACHE_TTL_SEC", "3600"))  # keep last known for 1h by default


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


# -----------------------------------------------------------------------------
# LIVE MQTT LOCATION CACHE
# -----------------------------------------------------------------------------
# Keyed by (teamId, ffId) -> {"lat": float, "lon": float, "observedAt": str, "ts": float}
_location_cache: Dict[Tuple[str, str], Dict[str, Any]] = {}
_location_lock = asyncio.Lock()

_mqtt_loc_client = None
_mqtt_loc_loop = None


def _parse_team_ff_from_topic(topic: str) -> Tuple[Optional[str], Optional[str]]:
    # expected: ngsi/Location/<team>/<ff>
    try:
        parts = [p for p in (topic or "").split("/") if p]
        if len(parts) >= 4 and parts[0] == "ngsi" and parts[1] == "Location":
            return parts[2], parts[3]
    except Exception:
        pass
    return None, None


def _payload_to_latlon(payload: bytes) -> Tuple[Optional[float], Optional[float], Optional[str]]:
    """
    Supports your OwnTracks-like payload:
      {"lat": 38.254727, "lon": 21.742054, "tst": 1770842887, ...}
    Also supports:
      {"observedAt":"...Z"} etc.
    """
    try:
        d = json.loads(payload.decode("utf-8", "replace"))
    except Exception:
        return None, None, None

    lat = _safe_float(d.get("lat"))
    lon = _safe_float(d.get("lon") if "lon" in d else d.get("lng"))

    obs = None
    if isinstance(d.get("observedAt"), str) and d.get("observedAt"):
        obs = d["observedAt"]
    elif isinstance(d.get("timestamp"), str) and d.get("timestamp"):
        obs = d["timestamp"]
    elif isinstance(d.get("tst"), (int, float)):
        # OwnTracks 'tst' is epoch seconds
        try:
            obs = _iso(datetime.fromtimestamp(float(d["tst"]), tz=timezone.utc))
        except Exception:
            obs = None

    return lat, lon, obs


def _cache_set(team: str, ff: str, lat: float, lon: float, observed_at: Optional[str]) -> None:
    # Called from MQTT thread via loop.call_soon_threadsafe, so do not use await here.
    key = (team, ff)
    _location_cache[key] = {
        "teamId": team,
        "ffId": ff,
        "lat": float(lat),
        "lon": float(lon),
        "observedAt": observed_at or _iso(_now_utc()),
        "ts": time.time(),
        "source": "mqtt",
    }


def _cache_prune(now_ts: Optional[float] = None) -> None:
    now_ts = now_ts or time.time()
    ttl = max(10, LOC_CACHE_TTL_SEC)
    dead = [k for k, v in _location_cache.items() if (now_ts - float(v.get("ts", 0))) > ttl]
    for k in dead:
        _location_cache.pop(k, None)


def _start_mqtt_location_cache():
    """
    Starts a background MQTT client (threaded via paho loop_start)
    that subscribes to ngsi/Location/+/+ and keeps last lat/lon per FF.
    """
    global _mqtt_loc_client, _mqtt_loc_loop
    if mqtt is None:
        return
    if _mqtt_loc_client is not None:
        return

    _mqtt_loc_loop = asyncio.get_event_loop()

    client = mqtt.Client()

    def on_connect(c, userdata, flags, rc):
        if rc == 0:
            c.subscribe("ngsi/Location/+/+", qos=0)

    def on_message(c, userdata, msg):
        topic = msg.topic or ""
        team, ff = _parse_team_ff_from_topic(topic)
        if not team or not ff:
            return

        lat, lon, obs = _payload_to_latlon(bytes(msg.payload))
        if lat is None or lon is None:
            return

        def _apply():
            try:
                _cache_prune()
                _cache_set(team, ff, lat, lon, obs)
            except Exception:
                pass

        # marshal into the FastAPI loop safely
        try:
            _mqtt_loc_loop.call_soon_threadsafe(_apply)
        except Exception:
            # if loop isn't available, still store (best effort)
            try:
                _cache_prune()
                _cache_set(team, ff, lat, lon, obs)
            except Exception:
                pass

    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.loop_start()

    _mqtt_loc_client = client


@app.on_event("startup")
async def _on_startup():
    # Start MQTT location cache so /api/latest can always provide last-known coords.
    try:
        _start_mqtt_location_cache()
    except Exception:
        # don't fail startup if mqtt isn't reachable
        pass


@app.on_event("shutdown")
async def _on_shutdown():
    global _mqtt_loc_client
    try:
        if _mqtt_loc_client is not None:
            _mqtt_loc_client.loop_stop()
            _mqtt_loc_client.disconnect()
    except Exception:
        pass
    _mqtt_loc_client = None


# -----------------------------------------------------------------------------
# API
# -----------------------------------------------------------------------------
@app.get("/api/health")
def health():
    return {"ok": True, "ts": _iso(_now_utc())}


@app.get("/api/latest")
def latest(team: str = Query(...), minutes: int = Query(60, ge=1, le=240)):
    """
    Returns:
      { team: "Team_A", members: [ {teamId, ffId, hrBpm, tempC, mq2Raw, lat, lon, ...}, ... ] }

    Primary source: Influx points tagged with teamId, ffId across
      Environment / Biomedical / Location.

    IMPORTANT FIX:
      If Influx doesn't have lat/lon for some FF (e.g., FF_A),
      we merge last-known coords from MQTT topic cache:
        ngsi/Location/<team>/<ff>
    """
    _need_influx()

    since = _now_utc() - timedelta(minutes=minutes)
    start = since.isoformat()

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

    try:
        with _influx_client() as client:
            q = client.query_api()
            tables = q.query(flux, org=INFLUX_ORG)
    except Exception as e:
        raise HTTPException(502, f"Influx query failed: {type(e).__name__}: {str(e)[:200]}")

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

    # ---- Merge MQTT cached locations (fixes FF_A missing lat/lon) ----
    try:
        _cache_prune()
        for (t, ff), v in list(_location_cache.items()):
            if t != team:
                continue

            m = members.setdefault(ff, {"teamId": team, "ffId": ff})

            # Only set lat/lon if missing or invalid in Influx result
            lat_ok = isinstance(m.get("lat"), (int, float)) and float(m.get("lat")) != 0.0
            lon_ok = isinstance(m.get("lon"), (int, float)) and float(m.get("lon")) != 0.0

            if not lat_ok or not lon_ok:
                m["lat"] = float(v.get("lat"))
                m["lon"] = float(v.get("lon"))
                m["locObservedAt"] = v.get("observedAt") or _iso(_now_utc())
                m["locSource"] = "mqtt"
    except Exception:
        pass

    return {"team": team, "members": list(members.values())}


def _weather_risk_from_wind(wind_ms: Optional[float]) -> str:
    if wind_ms is None:
        return "LOW"
    if wind_ms >= 12:
        return "HIGH"
    if wind_ms >= 7:
        return "MEDIUM"
    return "LOW"


@app.get("/api/weather")
def weather(lat: float = Query(...), lon: float = Query(...)):
    """
    Prefer ORION (because your agent updates Orion reliably).
    If ORION fails, optionally try Influx WeatherObserved.
    """
    # 1) ORION (primary)
    try:
        r = requests.get(
            f"{ORION_BASE}/v2/entities/{WEATHER_ENTITY_ID}",
            params={"options": "keyValues"},
            timeout=3,
        )
        if r.status_code == 200:
            d = r.json() or {}
            temp_c = _safe_float(d.get("temperature"))
            wind_ms = _safe_float(d.get("windSpeed"))
            wind_dir = _safe_float(d.get("windDirection"))
            hum = _safe_float(d.get("humidity"))
            obs = d.get("timestamp") or d.get("observedAt")

            out = {"ok": True, "source": "orion", "risk": _weather_risk_from_wind(wind_ms)}
            if temp_c is not None:
                out["tempC"] = temp_c
            if wind_ms is not None:
                out["windMs"] = wind_ms
            if wind_dir is not None:
                out["windDirDeg"] = wind_dir
            if hum is not None:
                out["humidityPct"] = hum
            if isinstance(obs, str) and obs:
                out["observedAt"] = obs
            return out
    except Exception:
        pass

    # 2) Influx fallback (best-effort)
    if not (INFLUX_TOKEN and INFLUX_ORG and INFLUX_BUCKET):
        return {"ok": False, "reason": "Weather fetch failed: Orion unreachable + missing Influx env", "risk": "LOW"}

    flux = f"""from(bucket: "{INFLUX_BUCKET}")
  |> range(start: -6h)
  |> filter(fn: (r) => r["_measurement"] == "{MEAS_WEATHER}")
  |> filter(fn: (r) =>
      r["_field"] == "temperature" or
      r["_field"] == "windSpeed" or
      r["_field"] == "windDirection" or
      r["_field"] == "humidityPct" or
      r["_field"] == "humidity"
  )
  |> last()
  |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
  |> sort(columns: ["_time"], desc: true)
  |> limit(n: 1)
"""
    try:
        with InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG) as client:
            tables = client.query_api().query(flux, org=INFLUX_ORG)
    except Exception as e:
        return {"ok": False, "reason": f"Weather fetch failed: {type(e).__name__}", "risk": "LOW"}

    row = None
    for t in tables:
        for rec in t.records:
            row = rec.values
            break
        if row:
            break

    if not row:
        return {"ok": False, "reason": "No weather data (Orion failed, Influx empty)", "risk": "LOW"}

    temp_c = _safe_float(row.get("temperature"))
    wind_ms = _safe_float(row.get("windSpeed"))
    wind_dir = _safe_float(row.get("windDirection"))
    hum = _safe_float(row.get("humidityPct"))
    if hum is None:
        hum = _safe_float(row.get("humidity"))

    observed_at = None
    t = row.get("_time")
    if isinstance(t, datetime):
        observed_at = _iso(t)

    out = {"ok": True, "source": "influx", "risk": _weather_risk_from_wind(wind_ms)}
    if temp_c is not None:
        out["tempC"] = temp_c
    if wind_ms is not None:
        out["windMs"] = wind_ms
    if wind_dir is not None:
        out["windDirDeg"] = wind_dir
    if hum is not None:
        out["humidityPct"] = hum
    if observed_at:
        out["observedAt"] = observed_at
    return out


@app.post("/api/action")
def action(payload: Dict[str, Any]):
    """
    Stores a command action to Orion.
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


@app.get("/api/actions")
def actions(
    team: str = Query(...),
    minutes: int = Query(180, ge=1, le=1440),
    limit: int = Query(200, ge=1, le=2000),
):
    """
    Pull recent CommandAction entities from Orion (so actions are visible to any frontend).
    Returns:
      { team, actions: [ {id, teamId, ffId, action, note, observedAt, timestamp} ... ] }
    """
    since_ts = int(time.time()) - int(minutes * 60)

    # Orion v2 supports filtering by type and q (simple comparisons).
    # We store timestamp (Number) so we can filter on it.
    try:
        r = requests.get(
            f"{ORION_BASE}/v2/entities",
            params={
                "type": "CommandAction",
                "options": "keyValues",
                "limit": str(min(limit, 1000)),
                "q": f"teamId=={team};timestamp>{since_ts}",
            },
            timeout=5,
        )
    except Exception as e:
        raise HTTPException(502, f"Orion unreachable: {e}")

    if r.status_code != 200:
        raise HTTPException(502, f"Orion error: {r.status_code} {r.text[:200]}")

    arr = r.json() or []
    out: List[Dict[str, Any]] = []
    for e in arr:
        try:
            if e.get("teamId") != team:
                continue
            out.append(
                {
                    "id": e.get("id"),
                    "teamId": e.get("teamId"),
                    "ffId": e.get("ffId", "ALL"),
                    "action": e.get("action"),
                    "note": e.get("note", ""),
                    "observedAt": e.get("observedAt"),
                    "timestamp": e.get("timestamp"),
                }
            )
        except Exception:
            pass

    # sort newest first
    out.sort(key=lambda x: int(x.get("timestamp") or 0), reverse=True)
    out = out[:limit]
    return {"team": team, "actions": out}


@app.get("/api/alerts")
def alerts(
    team: str = Query(...),
    minutes: int = Query(180, ge=1, le=1440),
    limit: int = Query(200, ge=1, le=2000),
):
    """
    Returns alerts from Influx (if you persist them).
    Expected tags: teamId, ffId.
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

    acc: Dict[str, Dict[str, Any]] = {}

    try:
        with _influx_client() as client:
            q = client.query_api()
            tables = q.query(flux, org=INFLUX_ORG)
    except Exception as e:
        raise HTTPException(502, f"Influx query failed: {type(e).__name__}: {str(e)[:200]}")

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

    rows: List[Dict[str, Any]] = list(acc.values())
    rows.sort(key=lambda r: r.get("observedAt", ""), reverse=True)
    rows = rows[:limit]

    for r in rows:
        if "severity" not in r and "worst" in r:
            r["severity"] = r.get("worst")
        if "title" not in r and "category" in r:
            r["title"] = r.get("category")

    return {"team": team, "alerts": rows}


# -----------------------------------------------------------------------------
# WebSocket ECG bridge
# -----------------------------------------------------------------------------
@app.websocket("/ws/ecg")
async def ws_ecg(ws: WebSocket, team: str = Query(...), ff: str = Query(...)):
    if mqtt is None:
        await ws.accept()
        await ws.send_text("ERROR: paho-mqtt not installed in api container")
        await ws.close()
        return

    await ws.accept()

    topic = f"raw/ECG/{team}/{ff}"
    loop = asyncio.get_running_loop()
    aq: asyncio.Queue[bytes] = asyncio.Queue(maxsize=50)

    def on_connect(client, userdata, flags, rc):
        if rc == 0:
            client.subscribe(topic, qos=0)

    def on_message(client, userdata, msg):
        payload = bytes(msg.payload)

        def _put():
            try:
                if aq.full():
                    try:
                        aq.get_nowait()
                    except Exception:
                        pass
                aq.put_nowait(payload)
            except Exception:
                pass

        loop.call_soon_threadsafe(_put)

    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.loop_start()

    try:
        while True:
            try:
                payload = await asyncio.wait_for(aq.get(), timeout=5.0)
                await ws.send_bytes(payload)
            except asyncio.TimeoutError:
                # keepalive tick so proxies don't kill idle WS
                await ws.send_bytes(b"")
    except WebSocketDisconnect:
        pass
    finally:
        try:
            client.loop_stop()
            client.disconnect()
        except Exception:
            pass
        try:
            await ws.close()
        except Exception:
            pass
