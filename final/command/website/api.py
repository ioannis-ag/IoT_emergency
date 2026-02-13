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

# ✅ Support both names (so compose mistakes don't break you)
ORION_BASE = os.getenv("ORION_BASE") or os.getenv("ORION_URL") or "http://fiware-orion:1026"
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
_location_cache: Dict[Tuple[str, str], Dict[str, Any]] = {}
_location_lock = asyncio.Lock()

_mqtt_loc_client = None
_mqtt_loc_loop = None


def _parse_team_ff_from_topic(topic: str) -> Tuple[Optional[str], Optional[str]]:
    try:
        parts = [p for p in (topic or "").split("/") if p]
        if len(parts) >= 4 and parts[0] == "ngsi" and parts[1] == "Location":
            return parts[2], parts[3]
    except Exception:
        pass
    return None, None


def _payload_to_latlon(payload: bytes) -> Tuple[Optional[float], Optional[float], Optional[str]]:
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
        try:
            obs = _iso(datetime.fromtimestamp(float(d["tst"]), tz=timezone.utc))
        except Exception:
            obs = None

    return lat, lon, obs


def _cache_set(team: str, ff: str, lat: float, lon: float, observed_at: Optional[str]) -> None:
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

        try:
            _mqtt_loc_loop.call_soon_threadsafe(_apply)
        except Exception:
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
    try:
        _start_mqtt_location_cache()
    except Exception:
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
# ORION HELPERS (keyValues)
# -----------------------------------------------------------------------------
def _orion_get_keyvalues(entity_id: str, timeout: float = 3.0) -> Optional[Dict[str, Any]]:
    try:
        r = requests.get(
            f"{ORION_BASE.rstrip('/')}/v2/entities/{entity_id}",
            params={"options": "keyValues"},
            timeout=timeout,
        )
        if r.status_code == 200:
            d = r.json()
            return d if isinstance(d, dict) else None
    except Exception:
        return None
    return None


def _merge_if_missing_num(dst: Dict[str, Any], key: str, val: Any) -> None:
    if key in dst and isinstance(dst.get(key), (int, float)) and float(dst[key]) != 0.0:
        return
    f = _safe_float(val)
    if f is None:
        return
    dst[key] = f


def _merge_if_missing_str(dst: Dict[str, Any], key: str, val: Any) -> None:
    if key in dst and isinstance(dst.get(key), str) and dst.get(key):
        return
    if isinstance(val, str) and val:
        dst[key] = val


def _ensure_member(members: Dict[str, Dict[str, Any]], team: str, ff: str) -> Dict[str, Any]:
    return members.setdefault(ff, {"teamId": team, "ffId": ff})


def _merge_orion_for_ff(members: Dict[str, Dict[str, Any]], team: str, ff: str) -> None:
    """
    Pulls:
      - Wearable:Team:FF -> hrBpm, rrMs, stressIndex, fatigueIndex, riskScore, etc
      - EnvNode:Team:FF  -> tempC, humidityPct, mq2Raw, coPpm
      - Phone:Team:FF    -> lat, lon
    Merges into members[ff] if missing.
    """
    m = _ensure_member(members, team, ff)

    w = _orion_get_keyvalues(f"Wearable:{team}:{ff}")
    if w:
        for k in [
            "hrBpm", "rrMs", "rmssdMs", "sdnnMs", "pnn50Pct",
            "stressIndex", "fatigueIndex", "riskScore",
            "heatRisk", "heatIndexC", "gasRisk", "separationRisk",
        ]:
            _merge_if_missing_num(m, k, w.get(k))
        _merge_if_missing_str(m, "observedAt", w.get("observedAt") or w.get("timestamp"))

    e = _orion_get_keyvalues(f"EnvNode:{team}:{ff}")
    if e:
        for k in ["tempC", "humidityPct", "mq2Raw", "mq2Digital", "coPpm", "wifiRssiDbm"]:
            _merge_if_missing_num(m, k, e.get(k))
        _merge_if_missing_str(m, "envObservedAt", e.get("observedAt"))

    p = _orion_get_keyvalues(f"Phone:{team}:{ff}")
    if p:
        _merge_if_missing_num(m, "lat", p.get("lat"))
        _merge_if_missing_num(m, "lon", p.get("lon") if "lon" in p else p.get("lng"))
        _merge_if_missing_str(m, "locObservedAt", p.get("observedAt"))


# -----------------------------------------------------------------------------
# API
# -----------------------------------------------------------------------------
@app.get("/api/health")
def health():
    return {"ok": True, "ts": _iso(_now_utc()), "orionBase": ORION_BASE}


@app.get("/api/latest")
def latest(team: str = Query(...), minutes: int = Query(60, ge=1, le=240)):
    """
    Primary: Influx (Environment/Biomedical/Location)
    Always merge: MQTT cached coords for ngsi/Location/+/+ if missing
    Fallback/merge: Orion (Wearable/EnvNode/Phone) if Influx empty or missing important fields
    """
    members: Dict[str, Dict[str, Any]] = {}

    # ---- 1) Try Influx (if configured) ----
    influx_ok = bool(INFLUX_TOKEN and INFLUX_ORG and INFLUX_BUCKET)
    if influx_ok:
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
        try:
            with InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG) as client:
                tables = client.query_api().query(flux, org=INFLUX_ORG)
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
        except Exception:
            # Don't fail the endpoint if Influx is flaky; we'll fallback to Orion.
            pass

    # ---- 2) Merge MQTT cached locations ----
    try:
        _cache_prune()
        for (t, ff), v in list(_location_cache.items()):
            if t != team:
                continue
            m = _ensure_member(members, team, ff)
            lat_ok = isinstance(m.get("lat"), (int, float)) and float(m.get("lat")) != 0.0
            lon_ok = isinstance(m.get("lon"), (int, float)) and float(m.get("lon")) != 0.0
            if not lat_ok or not lon_ok:
                m["lat"] = float(v.get("lat"))
                m["lon"] = float(v.get("lon"))
                m["locObservedAt"] = v.get("observedAt") or _iso(_now_utc())
                m["locSource"] = "mqtt"
    except Exception:
        pass

    # ---- 3) Orion fallback/merge (critical for humidity/env) ----
    # If members is empty OR any member missing humidity/temp/hr, merge from Orion.
    # Also: ensure FF_A..FF_D exist (your UI expects them).
    for ff in ["FF_A", "FF_B", "FF_C", "FF_D"]:
        _ensure_member(members, team, ff)

    need_orion = (len(members) == 0)
    if not need_orion:
        # if any "key" fields missing, still merge
        for ff, m in members.items():
            if any(k not in m for k in ("hrBpm", "tempC", "humidityPct")):
                need_orion = True
                break

    if need_orion:
        for ff in list(members.keys()):
            _merge_orion_for_ff(members, team, ff)

    # optional: drop completely empty placeholder members if you prefer.
    # but keeping FF_A..FF_D helps the UI
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
    # 1) ORION (primary)
    try:
        r = requests.get(
            f"{ORION_BASE.rstrip('/')}/v2/entities/{WEATHER_ENTITY_ID}",
            params={"options": "keyValues"},
            timeout=3,
        )
        if r.status_code == 200:
            d = r.json() or {}
            temp_c = _safe_float(d.get("temperature"))
            wind_ms = _safe_float(d.get("windSpeed"))
            wind_dir = _safe_float(d.get("windDirection"))
            hum = _safe_float(d.get("humidityPct"))
            if hum is None:
                hum = _safe_float(d.get("humidity"))
            if hum is None:
                hum = _safe_float(d.get("relativeHumidity"))
            if hum is None:
                hum = _safe_float(d.get("rh"))
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

    return {"ok": False, "reason": "Weather fetch failed", "risk": "LOW"}


@app.post("/api/action")
def action(payload: Dict[str, Any]):
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
            f"{ORION_BASE.rstrip('/')}/v2/op/update",
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
    since_ts = int(time.time()) - int(minutes * 60)

    try:
        r = requests.get(
            f"{ORION_BASE.rstrip('/')}/v2/entities",
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

    out.sort(key=lambda x: int(x.get("timestamp") or 0), reverse=True)
    return {"team": team, "actions": out[:limit]}


@app.get("/api/alerts")
def alerts(
    team: str = Query(...),
    minutes: int = Query(180, ge=1, le=1440),
    limit: int = Query(200, ge=1, le=2000),
):
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
            tables = client.query_api().query(flux, org=INFLUX_ORG)
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
    return {"team": team, "alerts": rows[:limit]}


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
