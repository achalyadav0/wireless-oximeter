from datetime import datetime, timezone

from app.extensions import db
from app.models.device import Device
from app.models.patient import Patient
from app.models.sensor import Sensor
from app.models.measurement_type import MeasurementType
from app.models.observation import Observation
from app.models.observation_value import ObservationValue


class TelemetryService:
    """Handles physiological telemetry received from IoT devices."""

    TEST_DEVICE_UID = "ESP32-SENSOR-TEST-001"
    TEST_PATIENT_CODE = "PATIENT-TEST-001"

    def process_vitals(
        self,
        device_uid: str,
        heart_rate: float,
        spo2: float,
    ) -> None:
        """Process and persist a vitals telemetry packet."""

        # ---------------------------------
        # 1. Validate payload
        # ---------------------------------

        if not device_uid:
            raise ValueError("device_uid is required")

        if heart_rate is None:
            raise ValueError("heart_rate is required")

        if spo2 is None:
            raise ValueError("spo2 is required")

        # ---------------------------------
        # 2. Resolve device
        # ---------------------------------

        device = Device.query.filter_by(
            device_uid=device_uid
        ).first()

        if device is None:
            raise ValueError(
                f"Unknown device: {device_uid}"
            )

        # ---------------------------------
        # 3. Resolve sensor
        # ---------------------------------

        sensor = Sensor.query.filter_by(
            device_id=device.id
        ).first()

        if sensor is None:
            raise ValueError(
                f"No sensor found for device: {device_uid}"
            )

        # ---------------------------------
        # 4. Resolve patient
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
        # 5. Resolve measurement types
        # ---------------------------------

        heart_rate_type = MeasurementType.query.filter_by(
            code="HEART_RATE"
        ).first()

        spo2_type = MeasurementType.query.filter_by(
            code="SPO2"
        ).first()

        if heart_rate_type is None:
            raise ValueError(
                "HEART_RATE measurement type not found"
            )

        if spo2_type is None:
            raise ValueError(
                "SPO2 measurement type not found"
            )

        # ---------------------------------
        # 6. Create observations
        # ---------------------------------

        observed_at = datetime.now(timezone.utc)

        heart_rate_observation = Observation(
            patient_id=patient.id,
            sensor_id=sensor.id,
            measurement_type_id=heart_rate_type.id,
            observed_at=observed_at,
            source="DEVICE",
        )

        heart_rate_value = ObservationValue(
            observation=heart_rate_observation,
            integer_value=int(heart_rate),
        )

        spo2_observation = Observation(
            patient_id=patient.id,
            sensor_id=sensor.id,
            measurement_type_id=spo2_type.id,
            observed_at=observed_at,
            source="DEVICE",
        )

        spo2_value = ObservationValue(
            observation=spo2_observation,
            integer_value=int(spo2),
        )

        db.session.add(heart_rate_observation)
        db.session.add(heart_rate_value)

        db.session.add(spo2_observation)
        db.session.add(spo2_value)

        # ---------------------------------
        # 7. Update device last-seen time
        # ---------------------------------

        device.last_seen_at = observed_at

        # ---------------------------------
        # 8. Commit transaction
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
        print(f"  Heart Rate  : {heart_rate} bpm")
        print(f"  SpO2        : {spo2}%")