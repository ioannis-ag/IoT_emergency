from __future__ import annotations

import os
from datetime import datetime, timedelta, timezone
from typing import Any, Dict, Optional

import requests
from fastapi import FastAPI, HTTPException, Query
from influxdb_client import InfluxDBClient

app = FastAPI()

# ---- ENV ----
INFLUX_URL = os.getenv("INFLUX_URL", "http://influxdb:8086")
INFLUX_TOKEN = os.getenv("INFLUX_TOKEN", "")
INFLUX_ORG = os.getenv("INFLUX_ORG", "")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET", "")

WEATHER_AGENT_URL = os.getenv("WEATHER_AGENT_URL", "http://weather-agent:8000")

# If your measurements differ, change here
MEAS_ENV = os.getenv("MEAS_ENV", "Environment")
MEAS_BIO = os.getenv("MEAS_BIO", "Biomedical")
MEAS_LOC = os.getenv("MEAS_LOC", "Location")


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
        raise HTTPException(
            500,
            "Missing INFLUX_TOKEN/INFLUX_ORG/INFLUX_BUCKET in environment"
        )


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
      { team: "Team_A", members: [ {teamId, ffId, hrBpm, tempC, mq2Raw, lat, lon, stressIndex, observedAt}, ... ] }

    This expects your Influx points to have tags: teamId, ffId
    and fields among: hrBpm, tempC, mq2Raw, stressIndex, lat, lon
    across measurements Environment/Biomedical/Location.
    """
    _need_influx()

    since = _now_utc() - timedelta(minutes=minutes)
    start = since.isoformat()

    flux = f'''
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: {start})
  |> filter(fn: (r) => r["teamId"] == "{team}")
  |> filter(fn: (r) =>
      r["_measurement"] == "{MEAS_ENV}" or
      r["_measurement"] == "{MEAS_BIO}" or
      r["_measurement"] == "{MEAS_LOC}"
  )
  |> filter(fn: (r) =>
      r["_field"] == "hrBpm" or
      r["_field"] == "rrMs" or
      r["_field"] == "tempC" or
      r["_field"] == "humidityPct" or
      r["_field"] == "mq2Raw" or
      r["_field"] == "coPpm" or
      r["_field"] == "stressIndex" or
      r["_field"] == "fatigueIndex" or
      r["_field"] == "riskScore" or
      r["_field"] == "heatRisk" or
      r["_field"] == "gasRisk" or
      r["_field"] == "heatIndexC" or
      r["_field"] == "rmssdMs" or
      r["_field"] == "sdnnMs" or
      r["_field"] == "pnn50Pct" or
      r["_field"] == "separationRisk" or
      r["_field"] == "lat" or
      r["_field"] == "lon"
  )

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

            ALLOWED = {
                "hrBpm", "rrMs",
                "tempC", "humidityPct",
                "mq2Raw", "coPpm",
                "stressIndex", "fatigueIndex", "riskScore",
                "heatRisk", "gasRisk", "heatIndexC",
                "rmssdMs", "sdnnMs", "pnn50Pct",
                "separationRisk",
                "lat", "lon",
            }

            if field in ALLOWED:
                m[field] = fval

    return {"team": team, "members": list(members.values())}


@app.get("/api/weather")
def weather(lat: float = Query(...), lon: float = Query(...)):
    """
    Proxies weather-agent:
      GET http://weather-agent:8000/weather?lat=..&lon=..
    Must return JSON.
    """
    try:
        r = requests.get(
            f"{WEATHER_AGENT_URL}/weather",
            params={"lat": lat, "lon": lon},
            timeout=8,
        )
    except Exception as e:
        raise HTTPException(502, f"Weather agent unreachable: {e}")

    if r.status_code != 200:
        raise HTTPException(502, f"Weather agent error: {r.status_code} {r.text}")

    return r.json()
