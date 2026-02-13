#!/usr/bin/env python3
"""
risk_processor.py
Polls Influx for recent Biomedical/Environment/Location data,
computes stress/fatigue/heat/gas/separation risk per firefighter,
and writes the latest indexes back to Orion (Wearable:* Biomedical entity).

Architecture:
MQTT -> Edge Controller -> Orion -> (bridge) -> Influx -> (this) -> Orion -> (bridge) -> Influx

NOT MEDICAL. Demo-only heuristics.
"""

from __future__ import annotations

import os
import math
import time
from datetime import datetime, timedelta, timezone
from typing import Any, Dict, List, Optional, Tuple

import requests
from influxdb_client import InfluxDBClient


# ----------------- ENV / CONFIG -----------------
INFLUX_URL = os.getenv("INFLUX_URL", "http://192.168.2.12:8086")
INFLUX_TOKEN = os.getenv("INFLUX_TOKEN", "R70UIeGKZGk-A9odIdXImPVGutlsJEbrrh0qJ9b0YoOmZb_-tr0g1kju3jgnWanoj4sDbUsYKe52Rr_GOkl81Q==")
INFLUX_ORG = os.getenv("INFLUX_ORG", "IoT")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET", "IoT")

ORION_BASE = os.getenv("ORION_BASE", "http://192.168.2.12:1026")
ORION_UPDATE_URL = os.getenv("ORION_UPDATE_URL", f"{ORION_BASE}/v2/op/update")
ORION_TIMEOUT = float(os.getenv("ORION_TIMEOUT", "5"))

TEAM_ID = os.getenv("TEAM_ID", "Team_A")
LEADER_FF_ID = os.getenv("LEADER_FF_ID", "FF_A")

POLL_SEC = float(os.getenv("POLL_SEC", "5"))
LOOKBACK_MIN = int(os.getenv("LOOKBACK_MIN", "3"))  # recent window for RR + latest readings

# measurement names in Influx (your api.py uses these defaults)
MEAS_ENV = os.getenv("MEAS_ENV", "Environment")
MEAS_BIO = os.getenv("MEAS_BIO", "Biomedical")
MEAS_LOC = os.getenv("MEAS_LOC", "Location")

# demo baselines (tune later)
BASELINE_HR_BPM = float(os.getenv("BASELINE_HR_BPM", "75"))
BASELINE_RMSSD_MS = float(os.getenv("BASELINE_RMSSD_MS", "45"))
HR_MAX_BPM = float(os.getenv("HR_MAX_BPM", "190"))

# RR plausibility
RR_MIN_MS = float(os.getenv("RR_MIN_MS", "300"))
RR_MAX_MS = float(os.getenv("RR_MAX_MS", "2000"))

# weights for overall risk
W_STRESS = float(os.getenv("W_STRESS", "0.40"))
W_FATIGUE = float(os.getenv("W_FATIGUE", "0.20"))
W_HEAT = float(os.getenv("W_HEAT", "0.20"))
W_GAS = float(os.getenv("W_GAS", "0.10"))
W_SEP = float(os.getenv("W_SEP", "0.10"))


# ----------------- helpers -----------------
def now_utc() -> datetime:
    return datetime.now(timezone.utc)

def iso(dt: datetime) -> str:
    return dt.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")

def clamp01(x: float) -> float:
    return float(max(0.0, min(1.0, x)))

def safe_float(x) -> Optional[float]:
    try:
        if x is None:
            return None
        return float(x)
    except Exception:
        return None

def haversine_m(lat1, lon1, lat2, lon2) -> float:
    R = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * R * math.asin(math.sqrt(a))

def heat_index_c(temp_c: float, rh_pct: float) -> float:
    # NOAA-ish regression, returned °C. Works best in warm conditions.
    T = temp_c * 9.0 / 5.0 + 32.0
    R = rh_pct
    HI = (
        -42.379
        + 2.04901523 * T
        + 10.14333127 * R
        - 0.22475541 * T * R
        - 0.00683783 * T * T
        - 0.05481717 * R * R
        + 0.00122874 * T * T * R
        + 0.00085282 * T * R * R
        - 0.00000199 * T * T * R * R
    )
    if T < 80 or R < 40:
        HI = 0.7 * HI + 0.3 * T
    return (HI - 32.0) * 5.0 / 9.0

def hrv_from_rr(rr_ms_list: List[float]) -> Optional[Dict[str, float]]:
    rr = [float(x) for x in rr_ms_list if x is not None and math.isfinite(float(x))]
    rr = [x for x in rr if RR_MIN_MS <= x <= RR_MAX_MS]
    if len(rr) < 5:
        return None

    rr_sorted = sorted(rr)
    med = rr_sorted[len(rr_sorted) // 2]
    bpm = 60.0 / (med / 1000.0) if med > 0 else None

    mean = sum(rr) / len(rr)
    sdnn = math.sqrt(sum((x - mean) ** 2 for x in rr) / (len(rr) - 1)) if len(rr) > 1 else None

    dif = [rr[i + 1] - rr[i] for i in range(len(rr) - 1)]
    rmssd = math.sqrt(sum(d * d for d in dif) / len(dif)) if len(dif) >= 1 else None
    pnn50 = 100.0 * (sum(1 for d in dif if abs(d) > 50.0) / len(dif)) if dif else None

    out = {}
    if bpm is not None: out["bpm"] = float(bpm)
    if sdnn is not None: out["sdnn_ms"] = float(sdnn)
    if rmssd is not None: out["rmssd_ms"] = float(rmssd)
    if pnn50 is not None: out["pnn50_pct"] = float(pnn50)
    out["n_rr"] = float(len(rr))
    return out

def stress_index(bpm: Optional[float], rmssd_ms: Optional[float], pnn50_pct: Optional[float]) -> Optional[float]:
    if bpm is None or rmssd_ms is None:
        return None

    hr_norm = clamp01((bpm - BASELINE_HR_BPM) / max(1.0, (HR_MAX_BPM - BASELINE_HR_BPM)))
    rmssd_norm = clamp01(rmssd_ms / max(1.0, BASELINE_RMSSD_MS))
    rmssd_stress = clamp01(1.0 - rmssd_norm)

    if pnn50_pct is not None:
        pnn50_norm = clamp01(pnn50_pct / 30.0)
        pnn50_stress = clamp01(1.0 - pnn50_norm)
        score = 0.45 * hr_norm + 0.35 * rmssd_stress + 0.20 * pnn50_stress
    else:
        score = 0.55 * hr_norm + 0.45 * rmssd_stress

    return clamp01(score)

def fatigue_index(bpm: Optional[float], rmssd_ms: Optional[float], stress01: Optional[float]) -> Optional[float]:
    if bpm is None or rmssd_ms is None:
        return None
    hr_norm = clamp01((bpm - BASELINE_HR_BPM) / max(1.0, (HR_MAX_BPM - BASELINE_HR_BPM)))
    rmssd_norm = clamp01(rmssd_ms / max(1.0, BASELINE_RMSSD_MS))
    hrv_drop = clamp01(1.0 - rmssd_norm)

    # demo: fatigue = HR load + HRV drop + some stress
    s = 0.0 if stress01 is None else float(stress01)
    return clamp01(0.50 * hr_norm + 0.35 * hrv_drop + 0.15 * s)

def gas_risk(mq2_raw: Optional[float], co_ppm: Optional[float]) -> Optional[float]:
    # demo threshold mapping (tune later)
    parts = []
    if mq2_raw is not None:
        # ~1800 moderate, ~2600 high, ~3200 extreme
        x = clamp01((mq2_raw - 1800.0) / 1400.0)
        parts.append(x)
    if co_ppm is not None:
        # ~35 moderate, ~100 high, ~150 extreme
        x = clamp01((co_ppm - 35.0) / 115.0)
        parts.append(x)
    if not parts:
        return None
    return clamp01(sum(parts) / len(parts))

def heat_risk(bpm: Optional[float], temp_c: Optional[float], rh_pct: Optional[float]) -> Tuple[Optional[float], Optional[float]]:
    if bpm is None:
        return None, None
    hi_c = None
    if temp_c is not None and rh_pct is not None:
        hi_c = float(heat_index_c(float(temp_c), float(rh_pct)))

    hr_norm = clamp01((bpm - BASELINE_HR_BPM) / max(1.0, (HR_MAX_BPM - BASELINE_HR_BPM)))
    if hi_c is None:
        return hr_norm, None

    # ~32C low, ~46C high
    hi_norm = clamp01((hi_c - 32.0) / 14.0)
    return clamp01(0.55 * hr_norm + 0.45 * hi_norm), hi_c

def overall_risk(
    stress01: Optional[float],
    fatigue01: Optional[float],
    heat01: Optional[float],
    gas01: Optional[float],
    sep01: Optional[float],
) -> Optional[float]:
    vals = {
        "stress": stress01,
        "fatigue": fatigue01,
        "heat": heat01,
        "gas": gas01,
        "sep": sep01,
    }
    # treat missing parts as 0 for demo
    s = float(vals["stress"] or 0.0)
    f = float(vals["fatigue"] or 0.0)
    h = float(vals["heat"] or 0.0)
    g = float(vals["gas"] or 0.0)
    p = float(vals["sep"] or 0.0)
    return clamp01(W_STRESS * s + W_FATIGUE * f + W_HEAT * h + W_GAS * g + W_SEP * p)

def ngsi_type(v: Any) -> str:
    if isinstance(v, bool):
        return "Boolean"
    if isinstance(v, (int, float)):
        return "Number"
    return "Text"

def build_orion_update_entity(team: str, ff: str, attrs: Dict[str, Any]) -> Dict[str, Any]:
    # We update the Biomedical entity that already exists:
    eid = f"Wearable:{team}:{ff}"
    ent: Dict[str, Any] = {"id": eid, "type": "Biomedical"}
    for k, v in attrs.items():
        ent[k] = {"type": ngsi_type(v), "value": v}
    ent["observedAt"] = {"type": "DateTime", "value": iso(now_utc())}
    ent["timestamp"] = {"type": "Number", "value": int(time.time())}
    return ent

def orion_append(entities: List[Dict[str, Any]]) -> None:
    if not entities:
        return
    try:
        r = requests.post(
            ORION_UPDATE_URL,
            json={"actionType": "append", "entities": entities},
            timeout=ORION_TIMEOUT,
        )
        if r.status_code not in (200, 201, 204):
            print(f"[ORION] {r.status_code}: {r.text[:200]}")
    except Exception as e:
        print(f"[ORION] error: {e}")


# ----------------- Influx read -----------------
def need_influx():
    if not (INFLUX_TOKEN and INFLUX_ORG and INFLUX_BUCKET):
        raise RuntimeError("Missing INFLUX_TOKEN/INFLUX_ORG/INFLUX_BUCKET")

def influx_client() -> InfluxDBClient:
    need_influx()
    return InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)

def fetch_recent(team: str, lookback_min: int) -> Dict[str, Dict[str, Any]]:
    """
    Returns per-ff:
      {
        "FF_A": {
          "hrBpm": last,
          "rrMs_list": [...],
          "tempC": last,
          "humidityPct": last,
          "mq2Raw": last,
          "coPpm": last,
          "lat": last,
          "lon": last,
          "activity": last (if present),
        }, ...
      }
    """
    start = (now_utc() - timedelta(minutes=lookback_min)).isoformat()

    # Pull a few important fields across 3 measurements.
    # We keep rrMs too (as multiple samples).
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
      r["_field"] == "lat" or
      r["_field"] == "lon" or
      r["_field"] == "activity"
  )
  |> keep(columns: ["_time","_measurement","_field","_value","ffId","teamId"])
  |> sort(columns: ["_time"], desc: false)
'''

    per_ff: Dict[str, Dict[str, Any]] = {}

    with influx_client() as client:
        q = client.query_api()
        tables = q.query(flux, org=INFLUX_ORG)

    for table in tables:
        for rec in table.records:
            ff = rec.values.get("ffId")
            if not ff:
                continue
            d = per_ff.setdefault(ff, {"rrMs_list": []})

            field = rec.get_field()
            val = rec.get_value()

            if field == "rrMs":
                f = safe_float(val)
                if f is not None:
                    d["rrMs_list"].append(float(f))
                continue

            # for non-rr fields we keep the LAST seen in the window
            f = safe_float(val)
            if field in ("lat", "lon", "tempC", "humidityPct", "mq2Raw", "coPpm", "hrBpm"):
                if f is not None:
                    d[field] = float(f)
            elif field == "activity":
                if isinstance(val, str):
                    d["activity"] = val

    return per_ff


# ----------------- main loop -----------------
def compute_and_publish(team: str) -> None:
    data = fetch_recent(team, LOOKBACK_MIN)
    if not data:
        return

    # leader loc for separation
    leader = data.get(LEADER_FF_ID, {})
    leader_lat = safe_float(leader.get("lat"))
    leader_lon = safe_float(leader.get("lon"))

    updates: List[Dict[str, Any]] = []

    for ff, d in data.items():
        rr_list = d.get("rrMs_list") or []
        hrv = hrv_from_rr(rr_list)

        bpm = safe_float(d.get("hrBpm"))
        if hrv and hrv.get("bpm") is not None:
            bpm = float(hrv["bpm"])  # prefer RR-derived HR when available

        rmssd = hrv.get("rmssd_ms") if hrv else None
        sdnn = hrv.get("sdnn_ms") if hrv else None
        pnn50 = hrv.get("pnn50_pct") if hrv else None

        stx = stress_index(bpm, rmssd, pnn50)
        ftx = fatigue_index(bpm, rmssd, stx)

        temp_c = safe_float(d.get("tempC"))
        rh_pct = safe_float(d.get("humidityPct"))
        heat01, hi_c = heat_risk(bpm, temp_c, rh_pct)

        mq2 = safe_float(d.get("mq2Raw"))
        co = safe_float(d.get("coPpm"))
        gas01 = gas_risk(mq2, co)

        # separation risk vs leader
        sep01 = None
        lat = safe_float(d.get("lat"))
        lon = safe_float(d.get("lon"))
        if None not in (leader_lat, leader_lon, lat, lon) and ff != LEADER_FF_ID:
            dist = haversine_m(float(lat), float(lon), float(leader_lat), float(leader_lon))
            # 0 until ~60m, 1 at ~140m
            sep01 = clamp01((dist - 60.0) / 80.0)

        risk01 = overall_risk(stx, ftx, heat01, gas01, sep01)

        print(
            f"[{team}/{ff}] bpm={bpm} rmssd={rmssd} "
            f"stx={stx} ftx={ftx} heat={heat01} gas={gas01} sep={sep01} risk={risk01}"
        )

        attrs: Dict[str, Any] = {}
        # we write only what we computed / what is meaningful
        if stx is not None: attrs["stressIndex"] = round(float(stx), 3)
        if ftx is not None: attrs["fatigueIndex"] = round(float(ftx), 3)
        if heat01 is not None: attrs["heatRisk"] = round(float(heat01), 3)
        if gas01 is not None: attrs["gasRisk"] = round(float(gas01), 3)
        if sep01 is not None: attrs["separationRisk"] = round(float(sep01), 3)
        if risk01 is not None: attrs["riskScore"] = round(float(risk01), 3)

        if rmssd is not None: attrs["rmssdMs"] = round(float(rmssd), 1)
        if sdnn is not None: attrs["sdnnMs"] = round(float(sdnn), 1)
        if pnn50 is not None: attrs["pnn50Pct"] = round(float(pnn50), 1)
        if hi_c is not None: attrs["heatIndexC"] = round(float(hi_c), 1)

        if not attrs:
            continue

        updates.append(build_orion_update_entity(team, ff, attrs))
    print(f"[ORION] pushing {len(updates)} updates -> {ORION_UPDATE_URL}")

    orion_append(updates)

def main():
    print(f"[risk_processor] team={TEAM_ID} lookback={LOOKBACK_MIN}min poll={POLL_SEC}s")
    while True:
        try:
            compute_and_publish(TEAM_ID)
        except Exception as e:
            print(f"[risk_processor] loop error: {e}")
        time.sleep(POLL_SEC)

if __name__ == "__main__":
    main()
