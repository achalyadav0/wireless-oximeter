# MQTT Telemetry Ingestion

This document describes the local MQTT telemetry path implemented by the backend.

## Flow

```text
Device or simulator
        |
        | MQTT publish
        v
Mosquitto :1883
        |
        | oximeter/devices/+/vitals
        v
backend/run_mqtt.py
        |
        v
TelemetryService
        |
        +--> observations and observation_values
        +--> devices.last_seen_at
```

The MQTT worker runs inside a Flask application context so it can use the SQLAlchemy session while messages are processed.

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

Apply migrations from the backend environment:

```bash
cd backend
python -m flask --app run.py db upgrade
```

Start the MQTT worker in a second terminal, from `backend/`:

```bash
python run_mqtt.py
```

The worker connects to `localhost:1883` and subscribes to `oximeter/devices/+/vitals` by default.

## Publish a vitals message

The current handler requires valid JSON with these fields:

| Field | Type | Description |
| --- | --- | --- |
| `device_uid` | string | Existing device identifier in the database |
| `heart_rate` | number | Heart rate in beats per minute |
| `spo2` | number | Oxygen saturation percentage |

Example:

```bash
mosquitto_pub \
  -h localhost \
  -p 1883 \
  -t 'oximeter/devices/ESP32-SENSOR-TEST-001/vitals' \
  -m '{"device_uid":"ESP32-SENSOR-TEST-001","heart_rate":72,"spo2":98}'
```

The MQTT subscription uses the device segment as a wildcard for topic matching, while the current service resolves the device using the `device_uid` contained in the JSON payload. The device, its sensor, the `PATIENT-TEST-001` patient, and the `HEART_RATE` and `SPO2` measurement types must already exist. A successful message creates two observations, stores integer values, and updates the device `last_seen_at` timestamp in one transaction.

**Development limitation:** the current telemetry service maps all accepted telemetry to `PATIENT-TEST-001`. This is temporary test logic. A proper device-patient assignment and history model will be implemented before multi-patient operation.

## Configuration

The worker reads these environment variables:

| Variable | Default | Description |
| --- | --- | --- |
| `MQTT_BROKER_HOST` | `localhost` | MQTT broker hostname |
| `MQTT_BROKER_PORT` | `1883` | MQTT broker port |
| `MQTT_VITALS_TOPIC` | `oximeter/devices/+/vitals` | Subscription topic |
| `DATABASE_URL` | none | SQLAlchemy database connection URL |

For example, when the worker runs in a container or on another host:

```bash
export MQTT_BROKER_HOST=localhost
export MQTT_BROKER_PORT=1883
export MQTT_VITALS_TOPIC='oximeter/devices/+/vitals'
```

The development Mosquitto configuration allows anonymous connections. Do not use that configuration for a production deployment; configure authentication, authorization, and encrypted transport before exposing the broker.

## Rejected messages

The handler logs and ignores messages when:

- the payload is not valid JSON;
- `device_uid`, `heart_rate`, or `spo2` is missing;
- the device is unknown;
- the device has no sensor;
- the test patient or measurement types are missing; or
- database persistence cannot be completed.

Malformed messages and validation failures are logged and ignored by the current handler. Database failures roll back the transaction and propagate from the service; the current worker does not implement database-error retry or recovery. Messages are not retried by the current worker. Monitor the worker output and broker logs when troubleshooting.

## Verify the broker

Subscribe from another terminal to see messages published on the vitals topic:

```bash
mosquitto_sub -h localhost -p 1883 -t 'oximeter/devices/+/vitals' -v
```

Useful container checks:

```bash
docker compose -f deployment/docker-compose.yml ps
docker logs wireless-oximeter-mosquitto
```
