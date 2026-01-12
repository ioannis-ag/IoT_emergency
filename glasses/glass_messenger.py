# repl_namglasses.py
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
        print("Connected.")
        print("Type text to send to the RPi. Type 'quit' or 'exit' to stop.\n")

        while True:
            try:
                user_input = input("> ")
            except (KeyboardInterrupt, EOFError):
                print("\nExiting…")
                break

            if user_input.lower() in {"quit", "exit", "q"}:
                print("Bye!")
                break

            # Turn "\n" that you type into real newlines
            text = user_input.replace("\\n", "\n")

            sdk.display_text(text, duration_sec=0, x=0, y=0, clear=True)
            print("Sent.")


if __name__ == "__main__":
    main()
