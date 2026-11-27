# Wireless Oximeter – Research and Thesis Report

**Status:** Living document – update after every working session
**Author:** Achal Yadav
**Started:** 2026-08-25
**Last updated:** 2026-11-03

---

## How to use this report

This file is the single running record of the research. Paper and thesis
chapters are written from it, so record facts as they happen rather than
reconstructing them later.

- **After each session:** add a row to the Progress Log (§12) and, if a design
  choice was made, an entry to the Decision Log (§13).
- **After each experiment:** fill in the protocol and results tables in §8 and
  §9 with raw numbers, the date, the hardware used and the commit hash
  (`git rev-parse --short HEAD`).
- **Never leave an estimate unlabelled.** Mark anything not measured as
  *(planned)*, *(estimate)* or *(to verify)*.
- Items marked `TODO` are gaps to fill before submission.

Mapping to a typical thesis structure:

| Thesis chapter | Report sections |
| --- | --- |
| 1 Introduction | §1, §2 |
| 2 Background and related work | §3 |
| 3 System design | §4, §5 |
| 4 Implementation | §6 |
| 5 Evaluation methodology | §8 |
| 6 Results and discussion | §9, §10 |
| 7 Conclusion and future work | §11 |

---

## Abstract (working draft)

> This work presents a low-cost, offline-capable wireless pulse oximetry
> system. A MAX30102 photoplethysmography (PPG) sensor attached to an ESP32-S3
> microcontroller captures red and infrared signals, estimates heart rate and
> peripheral oxygen saturation (SpO2) on the device, and transmits both the
> raw signals and the estimates over Wi-Fi using MQTT. A Raspberry Pi 5 acts
> as a local edge server that hosts the MQTT broker, validates and stores the
> data in a relational database designed to separate raw signals from derived
> measurements, and serves a live monitoring dashboard. The system operates
> without internet connectivity. TODO: add evaluation results (accuracy against
> a reference oximeter, latency, packet delivery, storage cost) once measured.

---

## 1. Introduction

### 1.1 Motivation

TODO: expand with sources.

- Continuous SpO2 and heart-rate monitoring helps detect hypoxaemia early in
  wards, home care and low-resource settings.
- Commercial multi-patient monitoring systems are expensive and usually depend
  on hospital networks or cloud services.
- Low-cost sensors (MAX30102) and microcontrollers (ESP32) make it possible to
  build a networked monitor, but their accuracy and the reliability of the
  data pipeline must be evaluated.
- Many deployment sites have unreliable internet, which motivates a fully
  offline design built around a local edge server.

### 1.2 Problem statement

How can a low-cost, wireless, multi-device pulse oximetry system be designed
so that raw physiological signals and derived vital signs are captured,
transported, stored and displayed reliably **without cloud connectivity**,
while keeping the data suitable for later analysis and machine learning?

### 1.3 Research questions (draft – refine with supervisor)

- **RQ1:** How accurate are heart-rate and SpO2 estimates from a MAX30102 with
  the Maxim reference algorithm, compared with a certified reference
  oximeter?
- **RQ2:** What end-to-end latency and packet delivery ratio does an
  ESP32 → MQTT → Raspberry Pi pipeline achieve on a local Wi-Fi network?
- **RQ3:** What are the storage and compute costs of keeping raw PPG on an
  offline edge device, and how do storage layouts compare?
- **RQ4 (future):** Can models trained on stored raw PPG improve on the
  on-device algorithm?

### 1.4 Objectives

1. Build a patient node that captures raw red/IR PPG and computes HR and SpO2.
2. Transport data over MQTT on a local network.
3. Design and implement a database separating raw signals, derived
   observations, alerts and ML inferences.
4. Provide a live dashboard served from the edge device.
5. Run the full system offline on a Raspberry Pi 5.
6. Evaluate accuracy, reliability, latency and storage cost.

### 1.5 Contributions (expected)

- An open, reproducible hardware/software design for an offline wireless
  oximeter.
- A database schema for raw and derived physiological data with
  ML-traceability (§5).
- An evaluation of the accuracy and pipeline performance of the system.

---

## 2. Scope

**In scope:** single-sensor patient nodes, local MQTT transport, edge storage
and dashboard, offline operation, accuracy and performance evaluation.

**Out of scope (version 0.1):** electronic medical records, prescriptions,
billing, cloud synchronization, regulatory certification. This is a research
prototype and **not a medical device**.

---

## 3. Background and related work

TODO: write each subsection with citations. Verify every reference before
citing it.

### 3.1 Pulse oximetry principles

- Light absorption by oxygenated and deoxygenated haemoglobin at red (~660 nm)
  and infrared (~880–940 nm) wavelengths; Beer–Lambert law.
- AC/DC components of the PPG signal and the "ratio of ratios"
  R = (AC_red/DC_red) / (AC_ir/DC_ir), mapped to SpO2 with an empirical
  calibration curve.
- Heart rate from the periodicity of the PPG waveform.

### 3.2 Accuracy standards and known limitations

- ISO 80601-2-61 (pulse oximeter equipment): accuracy reported as root-mean-
  square error (A_rms) against reference SaO2. *(to verify: limits and test
  range)*
- Error sources: motion artefacts, low perfusion, ambient light, contact
  pressure, skin pigmentation (e.g. Sjoding et al., NEJM 2020, on racial bias
  in pulse oximetry). *(to verify)*

### 3.3 Low-cost sensing hardware

- MAX30102 integrated PPG sensor (Maxim Integrated / Analog Devices): LEDs,
  photodiode, 18-bit ADC, FIFO with on-chip sample averaging. Cite the
  datasheet.
- Maxim reference algorithm for HR/SpO2 (as packaged in the SparkFun MAX3010x
  library); known limitations of the fixed calibration curve. TODO.

### 3.4 IoT transport and edge computing

- MQTT publish/subscribe protocol (OASIS standard v3.1.1 / v5). Cite the
  standard.
- Edge computing for health monitoring; benefits for latency, privacy and
  offline operation. TODO: find 3–5 papers.

### 3.5 Related systems

TODO: table comparing 5–10 published low-cost or wireless oximetry systems
(sensor, MCU, transport, storage, offline support, evaluation performed).

| Work | Sensor | Transport | Storage | Offline | Evaluation |
| --- | --- | --- | --- | --- | --- |
| TODO | | | | | |

---

## 4. System architecture

### 4.1 Overview

```text
 Patient node                      Edge server (Raspberry Pi 5, offline)
 MAX30102 ─I2C─ ESP32-S3 ─Wi-Fi/MQTT─► Mosquitto ─► MQTT worker ─► PostgreSQL
                                                                     │
                                         Browser ◄─ Flask dashboard ◄┘
```

| Component | Technology | Role |
| --- | --- | --- |
| Patient node | ESP32-S3 DevKitC-1, MAX30102 | Signal capture, on-device HR/SpO2, MQTT publish |
| Broker | Eclipse Mosquitto 2 | Message routing on topic `oximeter/<n>/ppg` |
| Ingestion worker | Python, paho-mqtt, SQLAlchemy | Validation and transactional storage |
| Database | PostgreSQL 16 | Persistent storage of raw and derived data |
| Web server | Flask | JSON API and dashboard |
| Edge host | Raspberry Pi 5 | Runs all server components locally |

The ingestion worker and web server are separate processes. They share state
only through the database (Decision D-006).

### 4.2 Offline operation

All components run on the Raspberry Pi 5 on a local network. Internet is
needed only once, to install software. Consequences:

- **Time:** without NTP, the Pi's clock must be kept by its real-time clock
  with a backup battery. Recordings are timestamped on arrival at the Pi
  (§6.3), so clock accuracy directly affects data quality.
- **Storage:** the database should live on a USB SSD or NVMe drive; SD cards
  and USB flash drives wear under constant small writes and are better suited
  for backups and exports.
- **Networking:** the Pi can act as a Wi-Fi access point if no router is
  present.

TODO: describe the final hardware set-up once built (storage device, RTC
battery, power supply, UPS).

---

## 5. Data model

Full specification: `docs/database/database-design.md`.

Key design principles:

1. **Raw and derived data are separated.** Raw PPG (`ppg_recordings`,
   `ppg_samples`) is kept apart from derived measurements (`observations`,
   `observation_values`), so derived values can be recomputed or compared
   with new algorithms.
2. **Controlled vocabulary.** Measurement kinds are rows in
   `measurement_types` (`HEART_RATE`, `SPO2`, `PPG_RAW`), so new sensors do not
   need schema changes.
3. **ML traceability.** Each `ml_inferences` row references the exact model
   version (`ml_models`) and the recording it was computed from.
4. **Assignment history.** Doctor–patient (and planned device–patient)
   assignments are stored with start/end times rather than overwritten.

Entities: patients, doctors, patient_assignments, edge_gateways, devices,
sensors, measurement_types, sensor_measurement_types, observations,
observation_values, ppg_recordings, ppg_samples, ml_models, ml_inferences,
alerts (15 tables, 8 migrations).

TODO: insert ER diagram figure.

---

## 6. Implementation

### 6.1 Hardware

| Item | Detail |
| --- | --- |
| Microcontroller | ESP32-S3 DevKitC-1 |
| Sensor | MAX30102 (I2C, SDA GPIO 8, SCL GPIO 9, 3.3 V) |
| Edge server | Raspberry Pi 5 (TODO: RAM size, storage device, OS version) |
| Reference oximeter | TODO: model, certification |

### 6.2 Firmware (`device/oximeter/src/main.cpp`)

| Parameter | Configured value | Effect |
| --- | --- | --- |
| LED mode | 2 (red + IR) | Two channels |
| ADC sample rate | 100 Hz | Internal sampling |
| Sample averaging | 4 | FIFO output rate = 100 / 4 = **25 Hz** |
| Pulse width | 411 µs | 18-bit resolution |
| ADC range | 4096 nA | |
| LED brightness / amplitude | 60 / 0x3F | |
| Buffer | 100 samples per channel | ≈ 4 s of signal per packet |
| HR/SpO2 algorithm | `maxim_heart_rate_and_oxygen_saturation` | Assumes 25 Hz, 100-sample buffer |

Each loop collects a buffer, runs the algorithm, and publishes one JSON packet
containing the raw arrays and the estimates (`null` when the algorithm reports
them invalid). Buffers are not overlapped, so each estimate uses a fresh,
independent window.

### 6.3 Ingestion and storage (`backend/app/services/telemetry_service.py`)

- Validates types, equal channel lengths, sample count, positive sample rate,
  and at most 1000 samples per channel.
- Rejects packets from unregistered devices.
- Stores one recording per channel, one row per sample, plus HR and SpO2
  observations and the device's last-seen time, in a single transaction.
- Timestamps: the device clock (`timestamp_ms`) is uptime only, so the
  recording end is taken as the receive time at the Pi and the start as
  receive time minus the packet duration.

### 6.4 Dashboard (`backend/app/services/dashboard_service.py`, `templates/index.html`)

The browser polls `/api/devices` every 2 s; the server returns the newest
recording and HR/SpO2 values per device from the database.

### 6.5 Development and testing tools

- `backend/seed_dev_data.py` registers measurement types, a test patient and
  devices.
- Docker Compose for PostgreSQL and Mosquitto.
- TODO: simulator, automated tests.

---

## 7. Open issues affecting results

Fix or document these before collecting data for the thesis.

| ID | Issue | Impact | Status |
| --- | --- | --- | --- |
| I-01 | Firmware reports `sample_rate: 100`, but the effective rate is 25 Hz (100 Hz ÷ 4-sample averaging) | Stored `sample_rate_hz`, `duration_ms` (1 s instead of ~4 s), `started_at` and per-sample time offsets are wrong by 4× | Fixed – firmware v3.2.4 samples at 400 sps with 4x averaging, so the reported 100 Hz is the real rate |
| I-02 | All data is assigned to `PATIENT-TEST-001` | Cannot run multi-subject studies | Open – needs device–patient assignment |
| I-03 | One database row per sample (≈ 50 rows/s per device at 25 Hz × 2 channels) *(estimate)* | Storage growth and write load on the Pi | Open – evaluate array-per-recording layout (RQ3) |
| I-04 | Dashboard shows the last valid HR/SpO2 even if newer packets were invalid | Possibly stale values displayed | Open |
| I-05 | Timestamps depend on the Pi clock; no RTC battery yet | Wrong times after power loss | Open |
| I-06 | MQTT broker allows anonymous access; no TLS | Security | Accepted for lab use |
| I-07 | Firmware loop has a fixed 100 ms delay plus publish time between buffers | Gaps between consecutive recordings | Open – measure, then decide |

---

## 8. Evaluation methodology (planned)

### 8.1 Experiment E1 – HR and SpO2 accuracy (RQ1)

- **Setup:** MAX30102 node and a certified reference fingertip oximeter used
  at the same time on different fingers of the same hand.
- **Subjects:** TODO (number, consent, ethics approval if required).
- **Conditions:** rest; controlled breathing; light motion (TODO: define).
- **Metrics:** HR – mean absolute error, Bland–Altman bias and limits of
  agreement. SpO2 – A_rms, bias, Bland–Altman. Percentage of windows rejected
  as invalid.

### 8.2 Experiment E2 – Pipeline reliability and latency (RQ2)

- **Packet delivery ratio:** packets stored ÷ packets sent (add a sequence
  number to the payload; TODO).
- **End-to-end latency:** device publish → database commit (needs a
  synchronized clock or a round-trip method; TODO).
- **Conditions:** 1, 2, 4… devices; varying distance from access point.

### 8.3 Experiment E3 – Storage and compute cost (RQ3)

- Database size per device-hour for (a) row-per-sample and (b)
  array-per-recording layouts.
- Raspberry Pi 5 CPU, RAM and disk I/O under load.
- Projected retention for a given drive size.

### 8.4 Experiment E4 – Offline robustness

- Power-cycle the Pi and the ESP32; verify reconnection, data integrity and
  timestamp correctness.

---

## 9. Results

Record raw values with date, commit hash and conditions. Do not fill from
estimates.

### 9.1 E1 – Accuracy

| Date | Commit | Subject | Condition | N windows | HR MAE (bpm) | SpO2 A_rms (%) | Invalid (%) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| | | | | | | | |

### 9.2 E2 – Reliability and latency

| Date | Commit | Devices | Distance | Sent | Stored | PDR (%) | Median latency (ms) | p95 (ms) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| | | | | | | | | |

### 9.3 E3 – Storage and compute

| Date | Commit | Layout | Devices | Duration | DB growth (MB/h) | CPU (%) | RAM (MB) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| | | | | | | | |

### 9.4 E4 – Offline robustness

| Date | Commit | Scenario | Outcome | Notes |
| --- | --- | --- | --- | --- |
| | | | | |

---

## 10. Discussion

TODO after results: interpret accuracy against standards and related work;
limitations of the Maxim algorithm; trade-offs of offline edge storage;
threats to validity (small sample, lab conditions, reference device
accuracy).

---

## 11. Future work

- Device–patient assignment and multi-patient monitoring.
- Threshold alerts (e.g. low SpO2) using the `alerts` table.
- Signal-quality index and motion-artefact rejection.
- ML models trained on stored raw PPG, recorded in `ml_models` /
  `ml_inferences`.
- Software simulator and automated tests.
- Optional cloud synchronization when connectivity is available.
- Authentication, broker access control and TLS.

---

## 12. Progress log

Newest last. One row per working session.

| Date | Commit | Work done | Notes / next step |
| --- | --- | --- | --- |
| 2026-08-25 | `0f0153c` | Repository structure; database design v0.1 | |
| 2026-08-25 | `9b17d3b` | Doctor, patient and assignment models | |
| 2026-09-01 | `bca79b6` | Edge gateway, device, sensor, measurement type models | |
| 2026-09-01 | `aa38c37` | Manual relationship test script | |
| 2026-09-06 | `2caad37` | MQTT ingestion of HR/SpO2 into observations | |
| 2026-09-06 | `a746709` | MQTT and architecture documentation | |
| 2026-09-25 | `9995bc1` | ESP32-S3 + MAX30102 firmware publishing raw PPG; first live dashboard | Dashboard state held in worker memory |
| 2026-11-03 | uncommitted | Raw PPG and HR/SpO2 persisted to PostgreSQL; dashboard reads from database; firmware sends HR/SpO2; seed script; documentation and repository guide | Found sample-rate labelling issue I-01 |

---

## 13. Decision log

| ID | Date | Decision | Reason | Alternatives considered |
| --- | --- | --- | --- | --- |
| D-001 | 2026-08-25 | PostgreSQL as the primary database | Relational schema, transactions, concurrent access | SQLite, time-series DB |
| D-002 | 2026-08-25 | Separate raw PPG from derived observations | Re-processing and ML comparison | Single measurements table |
| D-003 | 2026-08-25 | Versioned ML models linked to inferences | Traceability of predictions | Unversioned outputs |
| D-004 | 2026-09-06 | MQTT for device transport | Lightweight publish/subscribe, suits microcontrollers | HTTP, WebSocket |
| D-005 | 2026-09-25 | Transmit raw PPG as well as on-device HR/SpO2 | Enables offline analysis and ML on raw signal | Send only derived values |
| D-006 | 2026-11-03 | Worker and web server share state only through the database | They run as separate processes; in-memory sharing failed | Run MQTT inside Flask, Redis |
| D-007 | 2026-11-03 | Fully offline deployment on Raspberry Pi 5 | Target sites may lack internet; privacy | Cloud backend |
| D-008 | 2026-11-03 | Timestamp recordings on receipt at the edge | ESP32 has no synchronized clock | NTP on device (needs internet) |

---

## 14. References

Verify each entry and convert to the required citation style.

1. Maxim Integrated, *MAX30102 High-Sensitivity Pulse Oximeter and
   Heart-Rate Sensor for Wearable Health*, datasheet.
2. ISO 80601-2-61, *Medical electrical equipment – Particular requirements for
   basic safety and essential performance of pulse oximeter equipment*.
3. OASIS, *MQTT Version 3.1.1* / *MQTT Version 5.0*.
4. M. W. Sjoding et al., "Racial Bias in Pulse Oximetry Measurement," *New
   England Journal of Medicine*, 2020.
5. TODO: pulse oximetry principles review.
6. TODO: low-cost / IoT pulse oximetry systems (related work).
7. TODO: edge computing in healthcare monitoring.
