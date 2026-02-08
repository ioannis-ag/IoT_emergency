import os
import time
import requests
from datetime import datetime, timezone

ORION_URL = os.getenv("ORION_URL", "http://localhost:1026")
ENTITY_ID = os.getenv("WEATHER_ENTITY_ID", "Weather:Athens")
CITY = os.getenv("WEATHER_CITY", "Athens,GR")
POLL_SECONDS = int(os.getenv("POLL_SECONDS", "60"))

LAT = os.getenv("LAT", "37.9838")   # Athens default
LON = os.getenv("LON", "23.7275")

OPEN_METEO_URL = "https://api.open-meteo.com/v1/forecast"

def fetch_weather():
    params = {
        "latitude": LAT,
        "longitude": LON,
        "current_weather": True
    }
    r = requests.get(OPEN_METEO_URL, params=params, timeout=10)
    r.raise_for_status()
    return r.json()

def to_ngsi(weather_json):
    current = weather_json.get("current_weather", {})
    now = datetime.utcnow().replace(microsecond=0).isoformat() + "Z"

    return {
        "id": ENTITY_ID,
        "type": "WeatherObserved",
        "temperature": {"type": "Number", "value": current.get("temperature")},
        "windSpeed": {"type": "Number", "value": current.get("windspeed")},
        "windDirection": {"type": "Number", "value": current.get("winddirection")},
        "timestamp": {"type": "DateTime", "value": now}
    }

def upsert_to_orion(entity):
    payload = {"actionType": "append", "entities": [entity]}
    r = requests.post(f"{ORION_URL}/v2/op/update", json=payload, timeout=10)

    if r.status_code >= 400:
        print("---- ORION 400 DETAILS ----", flush=True)
        print("Status:", r.status_code, flush=True)
        print("Body:", r.text, flush=True)
        print("Payload sent:", payload, flush=True)
        print("--------------------------", flush=True)

    r.raise_for_status()

def main():
    while True:
        try:
            w = fetch_weather()
            e = to_ngsi(w)
            upsert_to_orion(e)
            print(f"[OK] Updated {ENTITY_ID} at {e['timestamp']['value']}", flush=True)
        except Exception as ex:
            print(f"[ERR] {ex}", flush=True)
        time.sleep(POLL_SECONDS)

if __name__ == "__main__":
    main()
