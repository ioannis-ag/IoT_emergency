import os
import logging
from datetime import datetime, timezone
from flask import Flask, request, jsonify

from influxdb_client import InfluxDBClient, Point, WritePrecision
from influxdb_client.client.write_api import SYNCHRONOUS

# ------------------ LOGGING ------------------
logging.basicConfig(
    level=os.getenv("LOG_LEVEL", "INFO"),
    format="%(asctime)s [%(levelname)s] %(message)s",
)
log = logging.getLogger("orion-influx-bridge")

# ------------------ FLASK ------------------
app = Flask(__name__)

# ------------------ INFLUX CONFIG ------------------
INFLUX_URL = os.getenv("INFLUX_URL", "http://influxdb:8086")
INFLUX_TOKEN = os.getenv("INFLUX_TOKEN")
INFLUX_ORG = os.getenv("INFLUX_ORG")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET")

# If set, forces one measurement name; if empty, use entity_type as measurement
MEASUREMENT = os.getenv("INFLUX_MEASUREMENT", "").strip()

if not all([INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET]):
    raise RuntimeError("Missing INFLUX_TOKEN / INFLUX_ORG / INFLUX_BUCKET env vars")

client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
# Synchronous writes so you SEE errors immediately
write_api = client.write_api(write_options=SYNCHRONOUS)

# ------------------ HELPERS ------------------
def attr_value(x):
    """Orion normalized attrs look like {'type':..., 'value':..., 'metadata':...}"""
    if isinstance(x, dict) and "value" in x:
        return x["value"]
    return None

def parse_time(ent: dict) -> datetime:
    # Prefer observedAt NGSI DateTime attr
    obs = attr_value(ent.get("observedAt"))
    if isinstance(obs, str):
        try:
            if obs.endswith("Z"):
                return datetime.fromisoformat(obs.replace("Z", "+00:00"))
            return datetime.fromisoformat(obs)
        except Exception:
            pass

    # Fallback: epoch seconds if present
    tst = attr_value(ent.get("tst"))
    if isinstance(tst, (int, float)):
        return datetime.fromtimestamp(float(tst), tz=timezone.utc)

    return datetime.now(timezone.utc)

TAG_KEYS = ("teamId", "ffId", "nodeId", "originNodeId", "via", "source")
KEEP_STR_FIELDS = ("severity", "reason", "status", "via", "source")

def build_point(ent: dict) -> tuple[Point, int]:
    ent_id = ent.get("id", "unknown")
    ent_type = ent.get("type", "unknown")

    measurement = MEASUREMENT if MEASUREMENT else ent_type

    p = Point(measurement).tag("entity_id", ent_id).tag("entity_type", ent_type)

    # Promote key identifiers to tags if present
    for k in TAG_KEYS:
        v = attr_value(ent.get(k))
        if isinstance(v, str) and v:
            p.tag(k, v)

    # Time
    ts = parse_time(ent)
    p.time(ts, WritePrecision.NS)

    # Fields
    field_count = 0
    for k, v in ent.items():
        if k in ("id", "type"):
            continue

        val = attr_value(v)

        # Booleans
        if isinstance(val, bool):
            p.field(k, int(val))
            field_count += 1
            continue

        # Numbers
        if isinstance(val, (int, float)):
            p.field(k, float(val))
            field_count += 1
            continue

        # Useful strings (limited)
        if isinstance(val, str) and k in KEEP_STR_FIELDS and val:
            p.field(k, val)
            field_count += 1
            continue

        # If Orion sends a value that is a dict/list (rare in normalized), skip it.
        # (If you ever need it, stringify intentionally.)

    # IMPORTANT: ensure at least ONE field so Influx never discards a point
    if field_count == 0:
        p.field("_ingest", 1)
        field_count = 1

    return p, field_count

# ------------------ ROUTES ------------------
@app.route("/health", methods=["GET"])
def health():
    return jsonify({"ok": True}), 200

@app.route("/notify", methods=["POST"])
def notify():
    payload = request.get_json(silent=True) or {}
    data = payload.get("data", [])

    if not isinstance(data, list) or not data:
        # Still OK; Orion sometimes sends empty batches
        return jsonify({"ok": True, "written": 0}), 200

    points = []
    total_fields = 0

    # Build points
    for ent in data:
        try:
            p, nfields = build_point(ent)
            points.append(p)
            total_fields += nfields
        except Exception as e:
            log.exception("Failed to build point for entity id=%s type=%s: %s", ent.get("id"), ent.get("type"), e)

    # Write
    try:
        if points:
            write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=points)

        # Log a compact summary (this is what you were missing)
        types = {}
        for ent in data:
            t = ent.get("type", "unknown")
            types[t] = types.get(t, 0) + 1

        log.info(
            "notify: entities=%d points=%d fields=%d bucket=%s types=%s",
            len(data), len(points), total_fields, INFLUX_BUCKET, types
        )

        return jsonify({"ok": True, "written": len(points)}), 200

    except Exception as e:
        # IMPORTANT: return 500 so Orion shows notification failure (and you see it)
        log.exception("Influx write failed: %s", e)
        return jsonify({"ok": False, "error": str(e)}), 500

if __name__ == "__main__":
    log.info("Starting bridge on 0.0.0.0:8666 -> Influx %s org=%s bucket=%s measurement=%s",
             INFLUX_URL, INFLUX_ORG, INFLUX_BUCKET, (MEASUREMENT or "<entity_type>"))
    app.run(host="0.0.0.0", port=8666)
