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

The backend MQTT worker consumes device vitals and persists heart-rate and
SpO2 observations. Follow the MQTT guide for database migrations, environment
variables, payload examples, and worker startup.

## Documentation

- [System architecture](docs/architecture/system-architecture.md)
- [MQTT telemetry ingestion](docs/mqtt/telemetry-ingestion.md)
