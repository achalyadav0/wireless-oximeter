import os
import json
from datetime import datetime, timezone
from threading import Lock

import paho.mqtt.client as mqtt


BROKER_HOST = "localhost"
BROKER_PORT = 1883
MQTT_TOPIC = "oximeter/+/ppg"


lock = Lock()

devices = {}
DATA_FILE = "latest_devices.json"

def on_connect(client, userdata, flags, reason_code, properties):
    print(f"Connected to MQTT broker: {BROKER_HOST}:{BROKER_PORT}")

    client.subscribe(MQTT_TOPIC)

    print(f"Subscribed to: {MQTT_TOPIC}")


def on_message(client, userdata, message):
    try:
        data = json.loads(message.payload.decode("utf-8"))
    except json.JSONDecodeError:
        print("Invalid JSON received")
        return

    device_id = data.get("device_id")

    if not device_id:
        print("Missing device_id")
        return

    if "red" not in data or "ir" not in data:
        print(f"Invalid PPG packet from {device_id}")
        return

    with lock:
        devices[device_id] = {
            "device_id": device_id,
            "timestamp_ms": data.get("timestamp_ms"),
            "sample_rate": data.get("sample_rate"),
            "sample_count": data.get("sample_count"),
            "red": data.get("red", []),
            "ir": data.get("ir", []),
            "received_at": datetime.now(timezone.utc).isoformat(),
        }
        with open(DATA_FILE, "w") as file:

            json.dump(devices, file)

    print(
        f"PPG received: {device_id} "
        f"({data.get('sample_count', 0)} samples)"
    )


client = mqtt.Client(
    callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
    client_id="wireless-oximeter-dashboard",
)

client.on_connect = on_connect
client.on_message = on_message


print(f"Connecting to MQTT broker {BROKER_HOST}:{BROKER_PORT}")

client.connect(
    BROKER_HOST,
    BROKER_PORT,
    keepalive=60,
)

client.loop_forever()
