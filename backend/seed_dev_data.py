"""Create the development records the MQTT worker needs.

Usage: python seed_dev_data.py [DEVICE_ID ...]

DEVICE_ID must match DEVICE_ID in the firmware secrets.h (default OXI001).
Safe to run more than once.
"""

import sys

from app import create_app
from app.extensions import db
from app.models import Device, MeasurementType, Patient, Sensor
from app.services.telemetry_service import TelemetryService


MEASUREMENT_TYPES = [
    {
        "code": "PPG_RAW",
        "name": "Raw PPG Signal",
        "description": "Raw photoplethysmography samples",
        "unit": "ADC",
        "data_type": "INTEGER",
    },
    {
        "code": "HEART_RATE",
        "name": "Heart Rate",
        "description": "Heart rate derived from PPG",
        "unit": "bpm",
        "data_type": "INTEGER",
    },
    {
        "code": "SPO2",
        "name": "Blood Oxygen Saturation",
        "description": "Estimated peripheral oxygen saturation",
        "unit": "%",
        "data_type": "INTEGER",
    },
]


def seed(device_uids: list[str]) -> None:
    measurement_types = []

    for fields in MEASUREMENT_TYPES:
        measurement_type = MeasurementType.query.filter_by(
            code=fields["code"]
        ).first()

        if measurement_type is None:
            measurement_type = MeasurementType(**fields)
            db.session.add(measurement_type)
            print(f"Created measurement type {fields['code']}")

        measurement_types.append(measurement_type)

    patient_code = TelemetryService.TEST_PATIENT_CODE

    if Patient.query.filter_by(patient_code=patient_code).first() is None:
        db.session.add(
            Patient(
                patient_code=patient_code,
                first_name="Test",
                last_name="Patient",
            )
        )
        print(f"Created patient {patient_code}")

    for device_uid in device_uids:
        device = Device.query.filter_by(device_uid=device_uid).first()

        if device is None:
            device = Device(
                device_uid=device_uid,
                device_type="ESP32_OXIMETER",
            )
            db.session.add(device)
            print(f"Created device {device_uid}")

        if not device.sensors:
            device.sensors.append(
                Sensor(
                    sensor_uid=f"MAX30102-{device_uid}",
                    sensor_type="PPG",
                    manufacturer="Analog Devices",
                    model="MAX30102",
                    measurement_types=measurement_types,
                )
            )
            print(f"Created sensor MAX30102-{device_uid}")

    db.session.commit()


if __name__ == "__main__":
    app = create_app()

    with app.app_context():
        seed(sys.argv[1:] or ["OXI001"])
