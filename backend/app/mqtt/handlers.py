import json

from app.services.telemetry_service import TelemetryService


telemetry_service = TelemetryService()


def handle_vitals_message(topic: str, payload: str):
    """Process an incoming vitals MQTT message."""

    try:
        data = json.loads(payload)
    except json.JSONDecodeError:
        print("Invalid JSON received.")
        return

    try:
        device_uid = data["device_uid"]
        heart_rate = data["heart_rate"]
        spo2 = data["spo2"]
    except KeyError as exc:
        print(f"Missing telemetry field: {exc}")
        return

    try:
        telemetry_service.process_vitals(
            device_uid=device_uid,
            heart_rate=heart_rate,
            spo2=spo2,
        )
    except ValueError as exc:
        print(f"Telemetry rejected: {exc}")