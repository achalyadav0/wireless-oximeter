# System Architecture

This document describes the architecture of the wireless oximeter system.

## Components

- Device: collects oxygen saturation and pulse measurements.
- Edge: receives device data and bridges it to the backend through MQTT.
- MQTT broker: receives telemetry on the device PPG topic and makes it
	available to backend consumers.
- Backend: runs the MQTT ingestion worker, stores measurements, and exposes
	application services.
- Frontend: presents measurements and system status.
- Simulator: produces representative device data for development and testing.

## Data Flow

Device measurements are transmitted to the edge layer over the device
transport, published through MQTT, and consumed by the backend MQTT worker.
The worker validates each PPG packet, resolves the device from the
`device_id` in the JSON payload, and persists the raw red and infrared samples
as PPG recordings together with any device-computed heart-rate and SpO2
observations. It also updates the device last-seen timestamp. The web
process serves the dashboard from the database, so the worker and the web
server do not share in-memory state.

The current development telemetry service maps accepted messages to the test
patient `PATIENT-TEST-001`. Device-patient assignment and history will replace
this temporary mapping before multi-patient operation.

See [MQTT Telemetry Ingestion](../mqtt/telemetry-ingestion.md) for the local
broker configuration, payload contract, and startup instructions.
