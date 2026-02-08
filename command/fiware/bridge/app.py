import os
from datetime import datetime, timezone
from flask import Flask, request, jsonify
from influxdb_client import InfluxDBClient, Point, WritePrecision

app = Flask(__name__)

INFLUX_URL = os.getenv("INFLUX_URL", "http://influxdb:8086")
INFLUX_TOKEN = os.getenv("INFLUX_TOKEN")
INFLUX_ORG = os.getenv("INFLUX_ORG")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET")
MEASUREMENT = os.getenv("INFLUX_MEASUREMENT", "wearable")

if not all([INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET]):
    raise RuntimeError("Missing INFLUX_TOKEN / INFLUX_ORG / INFLUX_BUCKET env vars")

client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
write_api = client.write_api()

def attr_value(x):
    if isinstance(x, dict) and "value" in x:
        return x["value"]
    return None

@app.route("/health", methods=["GET"])
def health():
    return jsonify({"ok": True}), 200

@app.route("/notify", methods=["POST"])
def notify():
    payload = request.get_json(silent=True) or {}
    data = payload.get("data", [])

    if not isinstance(data, list) or not data:
        return jsonify({"ok": True, "written": 0}), 200

    points = []
    for ent in data:
        ent_id = ent.get("id", "unknown_device")
        ent_type = ent.get("type", "unknown")

        p = Point(MEASUREMENT).tag("entity_id", ent_id).tag("entity_type", ent_type)

        # Use tst if available (epoch seconds), else now
        tst = attr_value(ent.get("tst"))
        if isinstance(tst, (int, float)):
            ts = datetime.fromtimestamp(float(tst), tz=timezone.utc)
        else:
            ts = datetime.now(timezone.utc)

        # Store numeric attributes as fields
        for k, v in ent.items():
            if k in ("id", "type"):
                continue
            val = attr_value(v)
            if isinstance(val, bool):
                p.field(k, int(val))
            elif isinstance(val, (int, float)):
                p.field(k, float(val))
            elif isinstance(val, str) and k in ("conn", "tid", "t"):
                p.field(k, val)

        p.time(ts, WritePrecision.NS)
        points.append(p)

    write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=points)
    return jsonify({"ok": True, "written": len(points)}), 200

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8666)
