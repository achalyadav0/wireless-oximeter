import json

from app.dashboard.state import update_device


def handle_vitals_message(topic: str, payload: str):
    """Process an incoming PPG MQTT message."""

    try:
        data = json.loads(payload)
    except json.JSONDecodeError:
        print("Invalid JSON received.")
        return

    if "device_id" not in data:
        print("Missing device_id")
        return

    if "red" not in data or "ir" not in data:
        print(f"Invalid PPG packet from {data['device_id']}")
        return

    update_device(data)

    print(
        f"PPG received: "
        f"{data['device_id']} "
        f"({data.get('sample_count', 0)} samples)"
    )