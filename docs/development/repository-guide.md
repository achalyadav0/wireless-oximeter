# Repository Guide

This guide explains what every file in the repository does, which part of the
system it belongs to, and how the parts connect. Read it before changing code
or when describing the implementation in the thesis.

Last reviewed: 2026-11-03

## 1. The system in one picture

```text
 PATIENT NODE                     RASPBERRY PI 5 (offline, local network)
 ┌───────────────────┐            ┌────────────────────────────────────────────┐
 │ MAX30102 sensor   │            │                                            │
 │   │ I2C           │   Wi-Fi    │  Mosquitto        backend/run_mqtt.py      │
 │ ESP32-S3          │──MQTT────► │  :1883  ─────────► mqtt/client.py          │
 │ device/oximeter/  │ oximeter/  │                     │                      │
 │  src/main.cpp     │  <n>/ppg   │                     ▼                      │
 └───────────────────┘            │                  mqtt/handlers.py          │
                                  │                     │                      │
                                  │                     ▼                      │
                                  │           services/telemetry_service.py    │
                                  │                     │ SQLAlchemy models    │
                                  │                     ▼                      │
                                  │                PostgreSQL                  │
                                  │                     ▲                      │
                                  │                     │                      │
                                  │  backend/run.py ─► services/               │
                                  │  Flask :5000        dashboard_service.py   │
                                  │     │                                      │
                                  │     ▼                                      │
                                  │  templates/index.html (browser dashboard)  │
                                  └────────────────────────────────────────────┘
```

Two backend processes run side by side:

| Process | Entry point | Job |
| --- | --- | --- |
| MQTT worker | `backend/run_mqtt.py` | Receives packets, validates them, writes them to the database |
| Web server | `backend/run.py` | Serves the dashboard and JSON API, reads from the database |

They never talk to each other directly. **PostgreSQL is the only shared state.**

## 2. Life of one packet

1. **Sample** – `main.cpp` reads 100 red and 100 infrared values from the
   MAX30102. The sensor samples at 100 Hz and averages every 4 readings, so
   the effective rate is 25 Hz and one packet holds about 4 seconds of
   signal. The packet reports `sample_rate: 25`.
2. **Compute** – the firmware runs the Maxim algorithm
   (`maxim_heart_rate_and_oxygen_saturation`) to estimate heart rate and SpO2.
3. **Publish** – the firmware builds a JSON packet and publishes it to
   `oximeter/<n>/ppg` on the Mosquitto broker.
4. **Receive** – `mqtt/client.py` (subscribed to `oximeter/+/ppg`) passes the
   raw text to `handle_vitals_message` in `mqtt/handlers.py`.
5. **Validate and store** – the handler parses JSON and calls
   `TelemetryService.process_ppg`, which checks every field, looks up the
   device, sensor and patient, and writes in one transaction:
   - two `ppg_recordings` (RED, IR), each with 100 `ppg_samples`;
   - one `HEART_RATE` and one `SPO2` observation (if not null);
   - the device `last_seen_at` time.
6. **Display** – the browser polls `/api/devices` every 2 s.
   `dashboard_service.py` queries the latest recording and observations per
   device and returns JSON; `index.html` renders it.

Packet format (from `main.cpp`):

```json
{
  "device_id": "OXI001",
  "timestamp_ms": 123456,
  "sample_rate": 25,
  "sample_count": 100,
  "heart_rate": 72,
  "spo2": 98,
  "red": [ ...100 integers... ],
  "ir":  [ ...100 integers... ]
}
```

## 3. Directory map

| Path | Layer | Status |
| --- | --- | --- |
| `device/oximeter/` | Patient-node firmware (ESP32-S3, PlatformIO) | Working |
| `backend/` | Python backend: MQTT worker, web server, data model | Working |
| `migrations/` | Alembic database schema history | Working |
| `deployment/` | Docker Compose for PostgreSQL and Mosquitto | Working (development config) |
| `docs/` | Architecture, database, MQTT, hardware docs and research report | Ongoing |
| `edge/` | Planned edge gateway code | Empty |
| `frontend/` | Planned separate user interface | Empty (dashboard currently lives in `backend/app/templates/`) |
| `simulator/` | Planned software device for testing without hardware | Empty |
| `tests/` | Planned automated tests | Empty |

## 4. File-by-file reference

### 4.1 Repository root

| File | Responsibility |
| --- | --- |
| `README.md` | Project overview and quick-start commands |
| `LICENSE` | Licence terms |
| `.env.example` | Template for the local `.env` file (database URL, MQTT settings). The real `.env` is git-ignored |
| `.gitignore` | Keeps secrets, build output, caches and Mosquitto runtime data out of Git |
| `.vscode/settings.json` | Tells the PlatformIO extension to follow the open editor file |
| `.vscode/c_cpp_properties.json` | IntelliSense include paths for the firmware when the repo root is opened in VS Code |

### 4.2 Firmware – `device/oximeter/`

| File | Responsibility |
| --- | --- |
| `platformio.ini` | Build target (`esp32-s3-devkitc-1`, Arduino framework), serial speed, and libraries: SparkFun MAX3010x and PubSubClient (MQTT) |
| `src/main.cpp` | The whole firmware. `setup()` starts I2C on GPIO 8/9, configures the MAX30102 (red + IR, 100 Hz with 4-sample averaging = 25 Hz output, pulse width 411 µs, ADC range 4096), connects Wi-Fi and MQTT. `loop()` keeps connections alive, captures 100 samples, computes HR/SpO2, and calls `publishPPG()` to send the JSON packet |
| `include/secrets.example.h` | Template for Wi-Fi name/password, broker IP, `DEVICE_ID`, and MQTT topics. Copy to `secrets.h` (git-ignored). `main.cpp` falls back to the example when `secrets.h` is missing so clean checkouts still compile |
| `.gitignore` | Ignores `.pio/` build output and `include/secrets.h` |
| `.vscode/extensions.json`, `.vscode/settings.json` | PlatformIO editor recommendations |
| `src/esp32.code-workspace` | Personal VS Code workspace that points to a folder outside this repository; not needed by the build |
| `include/README`, `lib/README`, `test/README` | PlatformIO boilerplate explaining those folders; no project code yet |

**Connects to:** the MQTT broker (`MQTT_BROKER` in `secrets.h`). `DEVICE_ID`
must match a `devices.device_uid` row created by `backend/seed_dev_data.py`.

### 4.3 Backend entry points – `backend/`

| File | Responsibility |
| --- | --- |
| `run.py` | Creates the Flask app and starts the web server (dashboard + API, port 5000) |
| `run_mqtt.py` | Creates the Flask app (for database access), then starts `MQTTClient` with `handle_vitals_message` as the message callback. Blocks forever |
| `seed_dev_data.py` | Idempotent setup script: creates measurement types (`PPG_RAW`, `HEART_RATE`, `SPO2`), test patient `PATIENT-TEST-001`, and a device + MAX30102 sensor per device ID given on the command line |
| `test_sensor_relationships.py` | Manual script that creates a gateway → device → sensor → measurement types chain in the real database, prints the relationships, then deletes them. Not an automated test |
| `requirements.txt` | Python dependencies. Currently a full environment dump (~410 packages); the backend only needs Flask, Flask-SQLAlchemy, Flask-Migrate, Alembic, psycopg, paho-mqtt and python-dotenv |

### 4.4 Backend application package – `backend/app/`

| File | Responsibility |
| --- | --- |
| `__init__.py` | `create_app()` factory. Loads config, connects SQLAlchemy and Flask-Migrate, imports all models, and defines routes: `/health` (app + database check), `/api/devices` (latest data per device), `/` (dashboard page) |
| `config.py` | Reads `.env` and exposes `SQLALCHEMY_DATABASE_URI` (`DATABASE_URL`) and the migrations folder path |
| `extensions.py` | Creates the shared `db` (SQLAlchemy) and `migrate` objects so every module uses the same instances |

#### MQTT – `backend/app/mqtt/`

| File | Responsibility |
| --- | --- |
| `client.py` | `MQTTClient` wraps paho-mqtt: reads broker host/port/topic from environment variables (defaults `localhost`, `1883`, `oximeter/+/ppg`), subscribes on connect, and forwards each message to the handler |
| `handlers.py` | `handle_vitals_message` parses JSON, calls `TelemetryService.process_ppg`, and logs rejected or failed packets without stopping the worker |

#### Services – `backend/app/services/`

| File | Responsibility |
| --- | --- |
| `telemetry_service.py` | **Write path.** Validates a packet (types, lengths, sample rate, max 1000 samples), resolves device → sensor → patient, stores RED/IR recordings with samples and HR/SpO2 observations, updates `last_seen_at`, all in one transaction |
| `dashboard_service.py` | **Read path.** For every device, fetches the newest RED and IR recordings and the newest HR and SpO2 values and returns them as a dictionary for `/api/devices` |

#### Templates – `backend/app/templates/`

| File | Responsibility |
| --- | --- |
| `index.html` | Self-contained dashboard page. JavaScript polls `/api/devices` every 2 s and shows one card per device: heart rate, SpO2, sample count, sample rate, channel sizes, last seen |

#### Data model – `backend/app/models/`

Each file defines one table as a SQLAlchemy class. `models/__init__.py` imports
them all so Flask-Migrate and the app see the full schema.

| File | Table | What it stores | Used by code today? |
| --- | --- | --- | --- |
| `patient.py` | `patients` | Patient identity and status | Yes (test patient) |
| `doctor.py` | `doctors` | Clinician accounts (email, password hash, role) | No |
| `patient_assignment.py` | `patient_assignments` | Which doctor looks after which patient, with history | No |
| `edge_gateway.py` | `edge_gateways` | Raspberry Pi gateways and their location | Only in the manual test |
| `device.py` | `devices` | ESP32 nodes: `device_uid`, firmware version, status, gateway, `last_seen_at` | Yes |
| `sensor.py` | `sensors` | Physical sensors on a device (e.g. MAX30102) | Yes |
| `measurement_type.py` | `measurement_types` | Vocabulary of measurements (`HEART_RATE`, `SPO2`, `PPG_RAW`) with units | Yes |
| `sensor_measurement_type.py` | `sensor_measurement_types` | Many-to-many link: which sensor produces which measurement | Yes (seed) |
| `observation.py` | `observations` | One derived measurement event (patient, sensor, type, time, quality, source) | Yes |
| `observation_value.py` | `observation_values` | The value of an observation (integer, numeric or text) | Yes |
| `ppg_recording.py` | `ppg_recordings` | One segment of raw signal for one channel (RED or IR): start time, duration, sample rate | Yes |
| `ppg_sample.py` | `ppg_samples` | One raw sample inside a recording: index, time offset, value | Yes |
| `ml_model.py` | `ml_models` | Versioned machine-learning models | No |
| `ml_inference.py` | `ml_inferences` | One model prediction on one recording, with confidence | No |
| `alert.py` | `alerts` | Clinical alerts (type, severity, status, acknowledged/resolved times) | No |

Relationships:

```text
doctors ──< patient_assignments >── patients
                                       │
edge_gateways ──< devices ──< sensors  │
                               │  │    │
     measurement_types >──<────┘  │    │
            │                     │    │
            └──< observations >───┴────┤──< alerts
                     │                 │
             observation_values        │
                                       │
                 ppg_recordings >──────┘
                     │        │
               ppg_samples   ml_inferences >── ml_models
```

`──<` means "one to many".

### 4.5 Database migrations – `migrations/`

| File | Responsibility |
| --- | --- |
| `alembic.ini` | Alembic settings (logging, script location) |
| `env.py` | Connects Alembic to the Flask app's database engine and model metadata |
| `script.py.mako` | Template for new migration files |
| `README` | Alembic boilerplate |
| `versions/*.py` | Ordered schema changes, applied by `upgrade head` |

Migration order:

1. `4f5a84c548fd` – patients
2. `ca187cc309e4` – doctors, patient assignments
3. `583afb5200e8` – edge gateways, devices
4. `b521956e1357` – sensors, measurement types, link table
5. `cdf73bb6afec` – observations, observation values
6. `44ec2f09d933` – PPG recordings, PPG samples
7. `46015efa6a55` – ML models, ML inferences
8. `bff24b727aec` – alerts

### 4.6 Deployment – `deployment/`

| File | Responsibility |
| --- | --- |
| `docker-compose.yml` | Starts PostgreSQL 16 (database `wireless_oximeter`, port 5432, persistent volume) and Mosquitto 2 (MQTT 1883, WebSocket 9001) |
| `mosquitto/config/mosquitto.conf` | Broker settings: listeners, anonymous access (development only), persistence, logging to stdout |

### 4.7 Documentation – `docs/`

| File | Content |
| --- | --- |
| `architecture/system-architecture.md` | Components and data flow |
| `database/database-design.md` | Full database specification: goals, principles, every table, decisions, future work |
| `mqtt/telemetry-ingestion.md` | Broker setup, packet contract, validation rules, troubleshooting |
| `hardware/firmware-setup.md` | Wiring table, firmware configuration and upload steps, payload format |
| `development/repository-guide.md` | This guide |
| `research/research-report.md` | Living report for the research paper and thesis |

## 5. How to run the whole system

```bash
# 1. Infrastructure
docker compose -f deployment/docker-compose.yml up -d postgres mosquitto

# 2. Backend (from backend/, with .env containing DATABASE_URL)
python -m pip install -r requirements.txt
flask --app run db upgrade
python seed_dev_data.py OXI001
python run_mqtt.py        # terminal 1
python run.py             # terminal 2 -> http://127.0.0.1:5000/

# 3. Firmware (from device/oximeter/, after creating include/secrets.h)
pio run --target upload
pio device monitor
```

## 6. Where to change things

| You want to… | Edit |
| --- | --- |
| Change sensor wiring or sampling settings | `device/oximeter/src/main.cpp` |
| Add a field to the packet | `main.cpp` → `telemetry_service.py` → `dashboard_service.py` → `index.html`, then `docs/mqtt/telemetry-ingestion.md` |
| Add a table or column | `backend/app/models/`, then generate a migration in `migrations/versions/` |
| Register a new device | `python seed_dev_data.py <DEVICE_ID>` |
| Add an API endpoint | `backend/app/__init__.py` (route) + a function in `backend/app/services/` |
| Change broker or topic | Environment variables `MQTT_BROKER_HOST`, `MQTT_BROKER_PORT`, `MQTT_VITALS_TOPIC`, and `secrets.h` on the device |
