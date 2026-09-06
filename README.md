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

See `docs/architecture/system-architecture.md` for the initial system overview.

See `docs/mqtt/telemetry-ingestion.md` for local MQTT telemetry setup, payloads,
and troubleshooting.