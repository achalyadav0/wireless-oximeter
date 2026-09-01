from app import create_app
from app.extensions import db
from app.models import (
    EdgeGateway,
    Device,
    Sensor,
    MeasurementType,
)


app = create_app()


with app.app_context():

    # ---------------------------------------------------------
    # 1. Create gateway
    # ---------------------------------------------------------
    gateway = EdgeGateway(
        gateway_uid="RPI-SENSOR-TEST-001",
        name="Sensor Test Raspberry Pi",
        location="LAB-01",
    )

    # ---------------------------------------------------------
    # 2. Create device attached to gateway
    # ---------------------------------------------------------
    device = Device(
        device_uid="ESP32-SENSOR-TEST-001",
        device_type="OXIMETER",
        firmware_version="0.1.0",
        edge_gateway=gateway,
    )

    # ---------------------------------------------------------
    # 3. Create MAX30102 sensor
    # ---------------------------------------------------------
    sensor = Sensor(
        sensor_uid="MAX30102-TEST-001",
        sensor_type="PPG",
        manufacturer="Analog Devices",
        model="MAX30102",
        device=device,
    )

    # ---------------------------------------------------------
    # 4. Create measurement types
    # ---------------------------------------------------------
    ppg = MeasurementType(
        code="PPG_RAW",
        name="Raw PPG Signal",
        description="Raw photoplethysmography samples",
        unit="ADC",
        data_type="INTEGER",
    )

    heart_rate = MeasurementType(
        code="HEART_RATE",
        name="Heart Rate",
        description="Heart rate derived from PPG",
        unit="bpm",
        data_type="FLOAT",
    )

    spo2 = MeasurementType(
        code="SPO2",
        name="Blood Oxygen Saturation",
        description="Estimated peripheral oxygen saturation",
        unit="%",
        data_type="FLOAT",
    )

    # ---------------------------------------------------------
    # 5. Associate MAX30102 with measurements
    # ---------------------------------------------------------
    sensor.measurement_types.extend(
        [ppg, heart_rate, spo2]
    )

    # ---------------------------------------------------------
    # 6. Save everything
    # ---------------------------------------------------------
    db.session.add(gateway)
    db.session.commit()

    # ---------------------------------------------------------
    # 7. Test Device → Sensor
    # ---------------------------------------------------------
    print("\nDevice:", device.device_uid)

    print(
        "Device sensors:",
        [s.sensor_uid for s in device.sensors]
    )

    # ---------------------------------------------------------
    # 8. Test Sensor → MeasurementTypes
    # ---------------------------------------------------------
    print("\nSensor:", sensor.sensor_uid)

    print(
        "Sensor measurement types:",
        [m.code for m in sensor.measurement_types]
    )

    # ---------------------------------------------------------
    # 9. Test MeasurementType → Sensors
    # ---------------------------------------------------------
    print("\nMeasurement type:", spo2.code)

    print(
        "Sensors producing this measurement:",
        [s.sensor_uid for s in spo2.sensors]
    )

    # ---------------------------------------------------------
    # 10. Cleanup
    # ---------------------------------------------------------
    db.session.delete(gateway)
    db.session.commit()

    print("\nTemporary test data deleted.")