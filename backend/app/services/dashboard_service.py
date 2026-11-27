from app.extensions import db
from app.models.device import Device
from app.models.measurement_type import MeasurementType
from app.models.observation import Observation
from app.models.ppg_recording import PPGRecording
from app.models.sensor import Sensor


def get_device_snapshots() -> dict:
    """Return the latest persisted telemetry for each device.

    The MQTT worker and the web server run as separate processes, so the
    dashboard reads from the database rather than from worker memory.
    """

    snapshots = {}

    devices = Device.query.order_by(Device.device_uid).all()

    for device in devices:
        red = _latest_recording(device, "RED")
        ir = _latest_recording(device, "IR")

        snapshots[device.device_uid] = {
            "device_id": device.device_uid,
            "status": device.status,
            "last_seen_at": _isoformat(device.last_seen_at),
            "sample_rate": red.sample_rate_hz if red else None,
            "sample_count": len(red.samples) if red else 0,
            "red": [sample.value for sample in red.samples] if red else [],
            "ir": [sample.value for sample in ir.samples] if ir else [],
            "heart_rate": _latest_value(device, "HEART_RATE"),
            "spo2": _latest_value(device, "SPO2"),
            "received_at": _isoformat(red.started_at) if red else None,
        }

    # The worker runs in its own process; end this read so the next poll
    # sees newly committed rows.
    db.session.rollback()

    return snapshots


def _latest_recording(device: Device, channel: str) -> PPGRecording | None:
    return (
        PPGRecording.query
        .join(Sensor, PPGRecording.sensor_id == Sensor.id)
        .filter(
            Sensor.device_id == device.id,
            PPGRecording.channel == channel,
        )
        .order_by(PPGRecording.started_at.desc())
        .first()
    )


def _latest_value(device: Device, code: str) -> int | float | None:
    observation = (
        Observation.query
        .join(Sensor, Observation.sensor_id == Sensor.id)
        .join(
            MeasurementType,
            Observation.measurement_type_id == MeasurementType.id,
        )
        .filter(
            Sensor.device_id == device.id,
            MeasurementType.code == code,
        )
        .order_by(Observation.observed_at.desc())
        .first()
    )

    if observation is None or observation.value is None:
        return None

    value = observation.value

    if value.integer_value is not None:
        return value.integer_value

    return value.numeric_value


def _isoformat(value) -> str | None:
    return value.isoformat() if value else None
