# test_namglasses.py
import time
import datetime as dt
from namglasses_sdk import NamGlassesSDK

BROKER = "labserver.sense-campus.gr"
PORT = 1883
DOWN_TOPIC = "namglasses_downlink"
UP_TOPIC = "namglasses_uplink"

def main():
    print("Connecting to MQTT…")
    with NamGlassesSDK(
        broker=BROKER,
        port=PORT,
        downlink_topic=DOWN_TOPIC,
        uplink_topic=UP_TOPIC,
        qos=1,
    ) as sdk:
        # 1) Ping
        print("→ Sending ping…")
        pong = sdk.ping()
        print("✓ Pong:", pong)

        # 2) Status
        print("→ Requesting status…")
        status = sdk.get_status()
        cpu = status.get("cpu_percent")
        mem = status.get("mem_percent")
        temp = status.get("temp_c")
        print(f"✓ Status: CPU={cpu}%  MEM={mem}%  TEMP={temp}°C")

        # 3) OLED text
        print("→ Displaying text on OLED for 2s…")
        sdk.display_text("Chad Mike", duration_sec=2.0, x=0, y=0, clear=True)
        print("✓ Text displayed")

        # 4) Photo
        print("→ Taking photo… (this may take a few seconds)")
        try:
            img_bytes = sdk.take_photo(timeout=25.0)
            ts = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
            out_path = f"namglasses_photo_{ts}.jpg"
            with open(out_path, "wb") as f:
                f.write(img_bytes)
            print(f"✓ Photo saved to: {out_path}")
        except Exception as e:
            print(f"✗ Photo failed: {e}")

        print("\nAll checks done!")

if __name__ == "__main__":
    main()
