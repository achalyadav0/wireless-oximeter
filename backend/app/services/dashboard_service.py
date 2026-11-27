from datetime import datetime, timezone

from app.extensions import db
from app.models.device import Device
from app.models.measurement_type import MeasurementType
from app.models.observation import Observation
from app.models.ppg_recording import PPGRecording
from app.models.sensor import Sensor


# The firmware publishes a packet every second (100 samples at 100 Hz), so a
# device silent for longer than this is treated as disconnected.
ONLINE_TIMEOUT_SECONDS = 5

# Same infrared level the firmware's algorithm uses to detect a finger.
FINGER_IR_THRESHOLD = 20000


def get_device_snapshots() -> dict:
    """Return the latest persisted telemetry for each device.

    The MQTT worker and the web server run as separate processes, so the
    dashboard reads from the database rather than from worker memory.
    """

    snapshots = {}

    devices = Device.query.order_by(Device.device_uid).all()

    now = datetime.now(timezone.utc)

    for device in devices:
        red = _latest_recording(device, "RED")
        ir = _latest_recording(device, "IR")

        red_values = [sample.value for sample in red.samples] if red else []
        ir_values = [sample.value for sample in ir.samples] if ir else []

        seconds_since_seen = (
            (now - device.last_seen_at).total_seconds()
            if device.last_seen_at
            else None
        )

        if seconds_since_seen is None:
            connection = "never"
        elif seconds_since_seen <= ONLINE_TIMEOUT_SECONDS:
            connection = "online"
        else:
            connection = "offline"

        # Only report vitals from the latest packet; older valid readings
        # would otherwise linger after the finger is removed.
        since = red.started_at if red else None

        snapshots[device.device_uid] = {
            "device_id": device.device_uid,
            "status": device.status,
            "connection": connection,
            "seconds_since_seen": seconds_since_seen,
            "last_seen_at": _isoformat(device.last_seen_at),
            "finger_detected": (
                bool(ir_values)
                and sum(ir_values) / len(ir_values) >= FINGER_IR_THRESHOLD
            ),
            "sample_rate": red.sample_rate_hz if red else None,
            "sample_count": len(red_values),
            "red": red_values,
            "ir": ir_values,
            "heart_rate": _latest_value(device, "HEART_RATE", since),
            "spo2": _latest_value(device, "SPO2", since),
            "received_at": _isoformat(device.last_seen_at),
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


def _latest_value(
    device: Device,
    code: str,
    since: datetime | None,
) -> int | float | None:
    if since is None:
        return None

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
            Observation.observed_at >= since,
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
