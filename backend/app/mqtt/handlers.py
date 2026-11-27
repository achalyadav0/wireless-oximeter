import json

from app.extensions import db
from app.services.telemetry_service import TelemetryService


telemetry_service = TelemetryService()


def handle_vitals_message(topic: str, payload: str):
    """Process an incoming PPG MQTT message."""

    try:
        data = json.loads(payload)
    except json.JSONDecodeError:
        print("Invalid JSON received.")
        return

    if not isinstance(data, dict):
        print("PPG payload must be a JSON object.")
        return

    try:
        telemetry_service.process_ppg(data)
    except ValueError as error:
        print(f"Rejected PPG packet on {topic}: {error}")
    except Exception as error:
        # Keep the worker alive; the service has already rolled back.
        db.session.rollback()
        print(f"Failed to persist PPG packet on {topic}: {error!r}")
