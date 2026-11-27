# MQTT Telemetry Ingestion

This document describes the local MQTT telemetry path implemented by the backend.

## Flow

```text
ESP32 firmware or simulator
        |
        | MQTT publish
        v
Mosquitto :1883
        |
        | oximeter/+/ppg
        v
backend/run_mqtt.py
        |
        v
TelemetryService.process_ppg
        |
        +--> ppg_recordings and ppg_samples (RED and IR)
        +--> observations and observation_values (HEART_RATE, SPO2)
        +--> devices.last_seen_at
        |
        v
PostgreSQL <-- backend/run.py (/api/devices, dashboard)
```

The MQTT worker runs inside a Flask application context so it can use the SQLAlchemy session while messages are processed. The worker and the web server are separate processes and share data only through the database.

## Start the local services

From the repository root, start PostgreSQL and Mosquitto:

```bash
cd deployment
docker compose up -d postgres mosquitto
```

Set the database connection for the backend. The local PostgreSQL values from `deployment/docker-compose.yml` map to this URL:

```bash
export DATABASE_URL='postgresql://oximeter:oximeter_dev_password@localhost:5432/wireless_oximeter'
```

Apply migrations and register the development records from the backend environment:

```bash
cd backend
python -m flask --app run.py db upgrade
python seed_dev_data.py OXI001
```

`seed_dev_data.py` creates the `PPG_RAW`, `HEART_RATE`, and `SPO2` measurement types, the `PATIENT-TEST-001` patient, and one device with a MAX30102 sensor for each device ID given. The script is safe to run more than once.

Start the MQTT worker in a second terminal, from `backend/`:

```bash
python run_mqtt.py
```

The worker connects to `localhost:1883` and subscribes to `oximeter/+/ppg` by default.

## Publish a PPG packet

The handler requires a JSON object with these fields:

| Field | Type | Description |
| --- | --- | --- |
| `device_id` | string | Registered `devices.device_uid` |
| `sample_rate` | number | Samples per second, greater than zero |
| `sample_count` | integer | Optional; must equal the length of `red` and `ir` |
| `red` | number[] | Red channel samples, 1 to 1000 values |
| `ir` | number[] | Infrared channel samples, same length as `red` |
| `heart_rate` | number or null | Optional device-computed heart rate in bpm |
| `spo2` | number or null | Optional device-computed SpO2 percentage |
| `timestamp_ms` | number | Optional device uptime; not stored |

Example:

```bash
mosquitto_pub \
  -h localhost \
  -p 1883 \
  -t 'oximeter/01/ppg' \
  -m '{"device_id":"OXI001","sample_rate":100,"sample_count":3,"heart_rate":72,"spo2":98,"red":[51000,51020,51010],"ir":[62000,62040,62030]}'
```

The device is resolved from `device_id` in the payload, not from the topic. A successful packet creates one `ppg_recordings` row per channel with its samples, a `HEART_RATE` and an `SPO2` observation when those values are not null, and updates the device `last_seen_at` timestamp in one transaction.

The device clock is not synchronized, so recordings are timestamped on receipt: `started_at` is the receive time minus the packet duration.

**Development limitation:** the current telemetry service maps all accepted telemetry to `PATIENT-TEST-001`. This is temporary test logic. A proper device-patient assignment and history model will be implemented before multi-patient operation.

## Configuration

The worker reads these environment variables:

| Variable | Default | Description |
| --- | --- | --- |
| `MQTT_BROKER_HOST` | `localhost` | MQTT broker hostname |
| `MQTT_BROKER_PORT` | `1883` | MQTT broker port |
| `MQTT_VITALS_TOPIC` | `oximeter/+/ppg` | Subscription topic |
| `DATABASE_URL` | none | SQLAlchemy database connection URL |

The development Mosquitto configuration allows anonymous connections. Do not use that configuration for a production deployment; configure authentication, authorization, and encrypted transport before exposing the broker.

## Rejected messages

The handler logs and ignores a packet when:

- the payload is not valid JSON or not a JSON object;
- `device_id`, `red`, `ir`, or `sample_rate` is missing or invalid;
- `red` and `ir` differ in length, or disagree with `sample_count`;
- `heart_rate` or `spo2` is present but not a number;
- the device is unknown or has no sensor; or
- the test patient or a required measurement type is missing.

Database failures roll back the transaction, are logged, and the worker continues with the next message. Messages are not retried. Monitor the worker output and broker logs when troubleshooting.

## Verify the broker

Subscribe from another terminal to see messages published on the PPG topic:

```bash
mosquitto_sub -h localhost -p 1883 -t 'oximeter/+/ppg' -v
```

Useful container checks:

```bash
docker compose -f deployment/docker-compose.yml ps
docker logs wireless-oximeter-mosquitto
```
