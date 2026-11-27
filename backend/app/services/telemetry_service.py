from datetime import datetime, timedelta, timezone

from app.extensions import db
from app.models.device import Device
from app.models.patient import Patient
from app.models.sensor import Sensor
from app.models.measurement_type import MeasurementType
from app.models.observation import Observation
from app.models.observation_value import ObservationValue
from app.models.ppg_recording import PPGRecording
from app.models.ppg_sample import PPGSample


class TelemetryService:
    """Handles physiological telemetry received from IoT devices."""

    TEST_PATIENT_CODE = "PATIENT-TEST-001"

    MAX_SAMPLES_PER_PACKET = 1000

    def process_ppg(self, data: dict) -> None:
        """Validate and persist one PPG packet.

        Each packet stores one recording per channel (RED and IR) and, when
        the device reports them, heart-rate and SpO2 observations.
        """

        # ---------------------------------
        # 1. Validate payload
        # ---------------------------------

        device_uid = data.get("device_id")

        if not isinstance(device_uid, str) or not device_uid:
            raise ValueError("device_id is required")

        red = data.get("red")
        ir = data.get("ir")

        self._validate_channel("red", red)
        self._validate_channel("ir", ir)

        if len(red) != len(ir):
            raise ValueError(
                f"red and ir lengths differ: {len(red)} != {len(ir)}"
            )

        sample_count = data.get("sample_count", len(red))

        if sample_count != len(red):
            raise ValueError(
                f"sample_count {sample_count} does not match "
                f"{len(red)} samples"
            )

        sample_rate = data.get("sample_rate")

        if (
            not isinstance(sample_rate, (int, float))
            or isinstance(sample_rate, bool)
            or sample_rate <= 0
        ):
            raise ValueError("sample_rate must be a positive number")

        heart_rate = self._optional_number(data, "heart_rate")
        spo2 = self._optional_number(data, "spo2")

        # ---------------------------------
        # 2. Resolve device and sensor
        # ---------------------------------

        device = Device.query.filter_by(
            device_uid=device_uid
        ).first()

        if device is None:
            raise ValueError(
                f"Unknown device: {device_uid}"
            )

        sensor = Sensor.query.filter_by(
            device_id=device.id
        ).first()

        if sensor is None:
            raise ValueError(
                f"No sensor found for device: {device_uid}"
            )

        # ---------------------------------
        # 3. Resolve patient
        # ---------------------------------

        patient = Patient.query.filter_by(
            patient_code=self.TEST_PATIENT_CODE
        ).first()

        if patient is None:
            raise ValueError(
                f"Test patient not found: "
                f"{self.TEST_PATIENT_CODE}"
            )

        # ---------------------------------
        # 4. Create PPG recordings
        # ---------------------------------

        # The packet covers the samples captured just before it was sent.
        # timestamp_ms is the device uptime, not wall-clock time, so the
        # receive time is used as the end of the recording.
        received_at = datetime.now(timezone.utc)
        duration_ms = round(len(red) * 1000 / sample_rate)
        started_at = received_at - timedelta(milliseconds=duration_ms)

        for channel, values in (("RED", red), ("IR", ir)):
            recording = PPGRecording(
                patient_id=patient.id,
                sensor_id=sensor.id,
                started_at=started_at,
                duration_ms=duration_ms,
                sample_rate_hz=float(sample_rate),
                channel=channel,
            )

            recording.samples = [
                PPGSample(
                    sample_index=index,
                    timestamp_offset_ms=round(index * 1000 / sample_rate),
                    value=float(value),
                )
                for index, value in enumerate(values)
            ]

            db.session.add(recording)

        # ---------------------------------
        # 5. Create derived observations
        # ---------------------------------

        for code, value in (("HEART_RATE", heart_rate), ("SPO2", spo2)):
            if value is None:
                continue

            measurement_type = MeasurementType.query.filter_by(
                code=code
            ).first()

            if measurement_type is None:
                raise ValueError(
                    f"{code} measurement type not found"
                )

            observation = Observation(
                patient_id=patient.id,
                sensor_id=sensor.id,
                measurement_type_id=measurement_type.id,
                observed_at=received_at,
                source="DEVICE",
            )

            observation.value = ObservationValue(
                integer_value=int(value),
            )

            db.session.add(observation)

        # ---------------------------------
        # 6. Update device last-seen time
        # ---------------------------------

        device.last_seen_at = received_at

        # ---------------------------------
        # 7. Commit transaction
        # ---------------------------------

        try:
            db.session.commit()
        except Exception:
            db.session.rollback()
            raise

        print("Telemetry persisted:")
        print(f"  Device      : {device.device_uid}")
        print(f"  Sensor      : {sensor.sensor_uid}")
        print(f"  Patient     : {patient.patient_code}")
        print(f"  Samples     : {len(red)} per channel")
        print(f"  Heart Rate  : {heart_rate} bpm")
        print(f"  SpO2        : {spo2}%")

    def _validate_channel(self, name: str, values) -> None:
        if not isinstance(values, list) or not values:
            raise ValueError(f"{name} must be a non-empty list")

        if len(values) > self.MAX_SAMPLES_PER_PACKET:
            raise ValueError(
                f"{name} has {len(values)} samples; "
                f"maximum is {self.MAX_SAMPLES_PER_PACKET}"
            )

        if not all(
            isinstance(value, (int, float)) and not isinstance(value, bool)
            for value in values
        ):
            raise ValueError(f"{name} must contain only numbers")

    def _optional_number(self, data: dict, key: str):
        value = data.get(key)

        if value is None:
            return None

        if not isinstance(value, (int, float)) or isinstance(value, bool):
            raise ValueError(f"{key} must be a number or null")

        return value
