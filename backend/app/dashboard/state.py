from threading import Lock
from datetime import datetime, timezone


_lock = Lock()

_devices = {}


def update_device(data: dict) -> None:
    device_id = data["device_id"]

    with _lock:
        _devices[device_id] = {
            "device_id": device_id,
            "timestamp_ms": data.get("timestamp_ms"),
            "sample_rate": data.get("sample_rate"),
            "sample_count": data.get("sample_count"),
            "red": data.get("red", []),
            "ir": data.get("ir", []),
            "received_at": datetime.now(timezone.utc).isoformat(),
        }


def get_devices() -> dict:
    with _lock:
        return dict(_devices)