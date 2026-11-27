# Wireless Oximeter

Wireless pulse oximeter platform for collecting, transporting, storing, and viewing oxygen saturation and pulse measurements.

## Repository Layout

- `backend/`: backend services and APIs
- `device/`: firmware and device-side code
- `deployment/`: deployment manifests and operational configuration
- `docs/`: architecture and development documentation
- `edge/`: local gateway and MQTT bridge code
- `frontend/`: user interface
- `simulator/`: simulated device data sources
- `tests/`: cross-component and integration tests

## Local Development

The local development stack uses PostgreSQL for persistence and Mosquitto as
the MQTT broker. Start both services from the repository root with:

```bash
docker compose -f deployment/docker-compose.yml up -d postgres mosquitto
```

Create a local `.env` from `.env.example`, apply the database migrations, and
start the backend API and MQTT worker in separate terminals:

```bash
cd backend
python -m pip install -r requirements.txt
flask --app run db upgrade
python seed_dev_data.py OXI001
python run.py
python run_mqtt.py
```

The seed script registers the test patient, the measurement types, and a
device whose ID matches the firmware `DEVICE_ID`. Packets from unregistered
devices are rejected. The MQTT worker stores each packet in PostgreSQL, and the
dashboard at `http://127.0.0.1:5000/` reads the latest stored data for every
registered device. The device API is available at
`http://127.0.0.1:5000/api/devices`.

## Running on a Raspberry Pi

On the Pi, set `FLASK_HOST=0.0.0.0` in `.env` so the dashboard is reachable
from other devices, then install the backend as above. To start the dashboard
and MQTT worker on every boot instead of in SSH sessions, run once:

```bash
deployment/systemd/install.sh
```

PostgreSQL and Mosquitto already restart with Docker. Check the services and
their logs with:

```bash
systemctl status oximeter-dashboard oximeter-mqtt
journalctl -u oximeter-mqtt -f
```

## Device Firmware

The ESP32-S3 firmware is in `device/oximeter/` and uses PlatformIO. Copy
`device/oximeter/include/secrets.example.h` to `secrets.h`, set the Wi-Fi and
MQTT values, then build and upload from that directory. When `secrets.h` is
absent, the build uses placeholder values from the tracked example so
IntelliSense and clean checkouts still compile; real credentials are required
for the device to connect.

```bash
cd device/oximeter
cp include/secrets.example.h include/secrets.h
pio run --target upload
pio device monitor
```

The firmware samples the MAX30102 continuously at 100 Hz, estimates heart
rate and SpO2 on the device, and publishes one second (100 samples) of red and
infrared data with the latest estimate to `oximeter/<device-number>/ppg`. The backend subscribes to
`oximeter/+/ppg`. See the firmware setup guide for wiring, configuration, and
the JSON payload contract.

## Documentation

- [Repository guide](docs/development/repository-guide.md)
- [Research and thesis report](docs/research/research-report.md)
- [System architecture](docs/architecture/system-architecture.md)
- [MQTT telemetry ingestion](docs/mqtt/telemetry-ingestion.md)
- [ESP32 firmware setup](docs/hardware/firmware-setup.md)
