# System Architecture

This document describes the architecture of the wireless oximeter system.

## Components

- Device: collects oxygen saturation and pulse measurements.
- Edge: receives device data and bridges it to the backend.
- Backend: stores measurements and exposes application services.
- Frontend: presents measurements and system status.
- Simulator: produces representative device data for development and testing.

## Data Flow

Device measurements are transmitted to the edge layer over the device transport, published through MQTT, and consumed by backend services. The frontend reads processed data from the backend.
