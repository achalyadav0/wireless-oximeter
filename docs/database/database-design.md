Wireless Oximeter — Database Design
===================================

**Document:** Database Design Specification  
**Version:** 0.1  
**Status:** Draft / Architecture Baseline  
**Project:** Wireless Oximeter  
**Last Updated:** 2026-08-25

1. Purpose
----------

This document defines the database architecture and data model for the Wireless Oximeter system.

The database must support:

1.  Patient and doctor management

2.  Patient-device assignment

3.  Edge gateway management

4.  Sensor management

5.  Raw physiological signal storage

6.  Derived physiological observations

7.  Machine-learning inference tracking

8.  Alert generation and acknowledgement

9.  Historical patient monitoring

10. Future sensor and ML-model expansion

The design intentionally separates raw high-frequency sensor data from processed physiological measurements, ML inference results, alerts, and device/patient metadata.

2. System Context
-----------------

The intended architecture is:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Patient Nodes (ESP32 + sensors)
            |
           MQTT
            |
            v
      Raspberry Pi Edge
       |      |      |
      MQTT Processing ML
       |      |      |
       +------+------+
              |
           Internet
              |
              v
        Cloud Backend
       |             |
   PostgreSQL     REST/WebSocket
                     |
                     v
              Doctor Dashboard
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

During development, hardware can be replaced by a software simulator. The database design remains the same regardless of whether data originates from an ESP32, Raspberry Pi, simulator, or future device.

3. Database Goals
-----------------

### 3.1 Correctness

Use primary keys, foreign keys, unique constraints, transactions, and validation.

### 3.2 Extensibility

New physiological measurements such as temperature, respiratory rate, blood pressure, ECG, or activity should normally be added as data rather than new columns.

### 3.3 Traceability

Derived values should be traceable through patient, device, sensor, processing, ML model/version, observation, and alert where applicable.

### 3.4 Scalability

High-frequency data such as PPG is separated from lower-frequency observations.

### 3.5 Auditability

Assignments, alerts, and important state changes should retain timestamps and actors where appropriate.

### 3.6 Maintainability

The schema should map cleanly to application-domain concepts and repositories.

### 3.7 Privacy

Collect only necessary patient information, protect credentials, enforce authentication/authorization, and secure data in transit and at rest as appropriate.

4. Database Technology
----------------------

The planned primary relational database is PostgreSQL.

PostgreSQL is selected for:

-   relational integrity

-   transactions

-   indexing

-   UUID support

-   JSONB support

-   Python/SQLAlchemy compatibility

-   scalability

-   mature tooling

During development PostgreSQL will run in Docker.

5. Design Principles
--------------------

### 5.1 Separate raw and derived data

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Raw PPG != Derived HR/SpO2
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Raw PPG is stored separately from observations.

### 5.2 Stable schema with extensible data

Avoid:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
measurements
-------------
heart_rate
spo2
temperature
respiratory_rate
...
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Prefer:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
measurement_types
        |
        v
observation_values
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

### 5.3 Controlled vocabulary

Canonical measurement codes should be used:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
HR
SPO2
TEMPERATURE
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

rather than arbitrary aliases.

### 5.4 Versioning

ML models are explicitly versioned and every inference references the model version that generated it.

### 5.5 Auditability

Assignments and alerts retain timestamps and actors where appropriate.

### 5.6 Avoid premature abstraction

The schema should be extensible without modeling every conceivable future requirement.

6. Entity Overview
------------------

The initial database contains 14 tables:

1.  `doctors`

2.  `patients`

3.  `patient_assignments`

4.  `edge_gateways`

5.  `devices`

6.  `device_assignments`

7.  `sensors`

8.  `measurement_types`

9.  `observations`

10. `observation_values`

11. `ppg_samples`

12. `ml_models`

13. `ml_inferences`

14. `alerts`

7. Tables
---------

### 7.1 doctors

Stores authenticated healthcare users.

+-----------------+-----------+----------+-------------+
| Column          | Type      | Nullable | Constraints |
+-----------------+-----------+----------+-------------+
| `id`            | UUID      | No       | PK          |
+-----------------+-----------+----------+-------------+
| `doctor_code`   | VARCHAR   | No       | UNIQUE      |
+-----------------+-----------+----------+-------------+
| `first_name`    | VARCHAR   | No       |             |
+-----------------+-----------+----------+-------------+
| `last_name`     | VARCHAR   | No       |             |
+-----------------+-----------+----------+-------------+
| `email`         | VARCHAR   | No       | UNIQUE      |
+-----------------+-----------+----------+-------------+
| `password_hash` | VARCHAR   | No       |             |
+-----------------+-----------+----------+-------------+
| `role`          | VARCHAR   | No       |             |
+-----------------+-----------+----------+-------------+
| `status`        | VARCHAR   | No       |             |
+-----------------+-----------+----------+-------------+
| `created_at`    | TIMESTAMP | No       |             |
+-----------------+-----------+----------+-------------+
| `updated_at`    | TIMESTAMP | No       |             |
+-----------------+-----------+----------+-------------+
| `last_login_at` | TIMESTAMP | Yes      |             |
+-----------------+-----------+----------+-------------+

Plaintext passwords must never be stored.

### 7.2 patients

Represents monitored people.

+-----------------+-----------+----------+-------------+
| Column          | Type      | Nullable | Constraints |
+-----------------+-----------+----------+-------------+
| `id`            | UUID      | No       | PK          |
+-----------------+-----------+----------+-------------+
| `patient_code`  | VARCHAR   | No       | UNIQUE      |
+-----------------+-----------+----------+-------------+
| `first_name`    | VARCHAR   | No       |             |
+-----------------+-----------+----------+-------------+
| `last_name`     | VARCHAR   | No       |             |
+-----------------+-----------+----------+-------------+
| `date_of_birth` | DATE      | Yes      |             |
+-----------------+-----------+----------+-------------+
| `gender`        | VARCHAR   | Yes      |             |
+-----------------+-----------+----------+-------------+
| `status`        | VARCHAR   | No       |             |
+-----------------+-----------+----------+-------------+
| `created_at`    | TIMESTAMP | No       |             |
+-----------------+-----------+----------+-------------+
| `updated_at`    | TIMESTAMP | No       |             |
+-----------------+-----------+----------+-------------+

### 7.3 patient_assignments

Represents doctor-patient assignments. This is a many-to-many relationship with history.

+-----------------+-----------+----------+------------------+
| Column          | Type      | Nullable | Constraints      |
+-----------------+-----------+----------+------------------+
| `id`            | UUID      | No       | PK               |
+-----------------+-----------+----------+------------------+
| `doctor_id`     | UUID      | No       | FK → doctors.id  |
+-----------------+-----------+----------+------------------+
| `patient_id`    | UUID      | No       | FK → patients.id |
+-----------------+-----------+----------+------------------+
| `assigned_at`   | TIMESTAMP | No       |                  |
+-----------------+-----------+----------+------------------+
| `unassigned_at` | TIMESTAMP | Yes      |                  |
+-----------------+-----------+----------+------------------+
| `status`        | VARCHAR   | No       |                  |
+-----------------+-----------+----------+------------------+
| `assigned_by`   | UUID      | Yes      | FK → doctors.id  |
+-----------------+-----------+----------+------------------+
| `created_at`    | TIMESTAMP | No       |                  |
+-----------------+-----------+----------+------------------+

### 7.4 edge_gateways

Represents Raspberry Pi edge-processing units.

+--------------------+-----------+----------+-------------+
| Column             | Type      | Nullable | Constraints |
+--------------------+-----------+----------+-------------+
| `id`               | UUID      | No       | PK          |
+--------------------+-----------+----------+-------------+
| `gateway_uid`      | VARCHAR   | No       | UNIQUE      |
+--------------------+-----------+----------+-------------+
| `name`             | VARCHAR   | No       |             |
+--------------------+-----------+----------+-------------+
| `hostname`         | VARCHAR   | Yes      |             |
+--------------------+-----------+----------+-------------+
| `location`         | VARCHAR   | Yes      |             |
+--------------------+-----------+----------+-------------+
| `hardware_version` | VARCHAR   | Yes      |             |
+--------------------+-----------+----------+-------------+
| `software_version` | VARCHAR   | Yes      |             |
+--------------------+-----------+----------+-------------+
| `status`           | VARCHAR   | No       |             |
+--------------------+-----------+----------+-------------+
| `last_seen_at`     | TIMESTAMP | Yes      |             |
+--------------------+-----------+----------+-------------+
| `registered_at`    | TIMESTAMP | No       |             |
+--------------------+-----------+----------+-------------+
| `created_at`       | TIMESTAMP | No       |             |
+--------------------+-----------+----------+-------------+
| `updated_at`       | TIMESTAMP | No       |             |
+--------------------+-----------+----------+-------------+

### 7.5 devices

Represents patient-side ESP32 nodes.

+--------------------+-----------+----------+-----------------------+
| Column             | Type      | Nullable | Constraints           |
+--------------------+-----------+----------+-----------------------+
| `id`               | UUID      | No       | PK                    |
+--------------------+-----------+----------+-----------------------+
| `device_uid`       | VARCHAR   | No       | UNIQUE                |
+--------------------+-----------+----------+-----------------------+
| `device_type`      | VARCHAR   | No       |                       |
+--------------------+-----------+----------+-----------------------+
| `gateway_id`       | UUID      | Yes      | FK → edge_gateways.id |
+--------------------+-----------+----------+-----------------------+
| `firmware_version` | VARCHAR   | Yes      |                       |
+--------------------+-----------+----------+-----------------------+
| `hardware_version` | VARCHAR   | Yes      |                       |
+--------------------+-----------+----------+-----------------------+
| `status`           | VARCHAR   | No       |                       |
+--------------------+-----------+----------+-----------------------+
| `last_seen_at`     | TIMESTAMP | Yes      |                       |
+--------------------+-----------+----------+-----------------------+
| `registered_at`    | TIMESTAMP | No       |                       |
+--------------------+-----------+----------+-----------------------+
| `created_at`       | TIMESTAMP | No       |                       |
+--------------------+-----------+----------+-----------------------+
| `updated_at`       | TIMESTAMP | No       |                       |
+--------------------+-----------+----------+-----------------------+

Patient assignment is handled by `device_assignments`, not a permanent `patient_id` field.

### 7.6 device_assignments

Represents which patient uses which device during a period.

+-----------------+-----------+----------+------------------+
| Column          | Type      | Nullable | Constraints      |
+-----------------+-----------+----------+------------------+
| `id`            | UUID      | No       | PK               |
+-----------------+-----------+----------+------------------+
| `device_id`     | UUID      | No       | FK → devices.id  |
+-----------------+-----------+----------+------------------+
| `patient_id`    | UUID      | No       | FK → patients.id |
+-----------------+-----------+----------+------------------+
| `assigned_at`   | TIMESTAMP | No       |                  |
+-----------------+-----------+----------+------------------+
| `unassigned_at` | TIMESTAMP | Yes      |                  |
+-----------------+-----------+----------+------------------+
| `status`        | VARCHAR   | No       |                  |
+-----------------+-----------+----------+------------------+
| `assigned_by`   | UUID      | Yes      | FK → doctors.id  |
+-----------------+-----------+----------+------------------+
| `created_at`    | TIMESTAMP | No       |                  |
+-----------------+-----------+----------+------------------+

### 7.7 sensors

Represents physical sensors attached to patient devices.

+--------------------+-----------+----------+-----------------+
| Column             | Type      | Nullable | Constraints     |
+--------------------+-----------+----------+-----------------+
| `id`               | UUID      | No       | PK              |
+--------------------+-----------+----------+-----------------+
| `device_id`        | UUID      | No       | FK → devices.id |
+--------------------+-----------+----------+-----------------+
| `sensor_uid`       | VARCHAR   | Yes      | UNIQUE          |
+--------------------+-----------+----------+-----------------+
| `sensor_type`      | VARCHAR   | No       |                 |
+--------------------+-----------+----------+-----------------+
| `manufacturer`     | VARCHAR   | Yes      |                 |
+--------------------+-----------+----------+-----------------+
| `model`            | VARCHAR   | No       |                 |
+--------------------+-----------+----------+-----------------+
| `sampling_rate_hz` | NUMERIC   | Yes      |                 |
+--------------------+-----------+----------+-----------------+
| `firmware_version` | VARCHAR   | Yes      |                 |
+--------------------+-----------+----------+-----------------+
| `status`           | VARCHAR   | No       |                 |
+--------------------+-----------+----------+-----------------+
| `configuration`    | JSONB     | Yes      |                 |
+--------------------+-----------+----------+-----------------+
| `created_at`       | TIMESTAMP | No       |                 |
+--------------------+-----------+----------+-----------------+
| `updated_at`       | TIMESTAMP | No       |                 |
+--------------------+-----------+----------+-----------------+

`configuration` is intentionally JSONB because sensor-specific configuration differs between sensor types.

### 7.8 measurement_types

Controlled vocabulary for derived physiological measurements.

+---------------+-----------+----------+-------------+
| Column        | Type      | Nullable | Constraints |
+---------------+-----------+----------+-------------+
| `id`          | UUID      | No       | PK          |
+---------------+-----------+----------+-------------+
| `code`        | VARCHAR   | No       | UNIQUE      |
+---------------+-----------+----------+-------------+
| `name`        | VARCHAR   | No       |             |
+---------------+-----------+----------+-------------+
| `unit`        | VARCHAR   | No       |             |
+---------------+-----------+----------+-------------+
| `category`    | VARCHAR   | No       |             |
+---------------+-----------+----------+-------------+
| `description` | TEXT      | Yes      |             |
+---------------+-----------+----------+-------------+
| `value_type`  | VARCHAR   | No       |             |
+---------------+-----------+----------+-------------+
| `is_active`   | BOOLEAN   | No       |             |
+---------------+-----------+----------+-------------+
| `created_at`  | TIMESTAMP | No       |             |
+---------------+-----------+----------+-------------+

Initial values:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
HR
SPO2
TEMPERATURE
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

### 7.9 observations

Represents a physiological observation event or processing window.

+----------------------+-----------+----------+-----------------------+
| Column               | Type      | Nullable | Constraints           |
+----------------------+-----------+----------+-----------------------+
| `id`                 | UUID      | No       | PK                    |
+----------------------+-----------+----------+-----------------------+
| `patient_id`         | UUID      | No       | FK → patients.id      |
+----------------------+-----------+----------+-----------------------+
| `device_id`          | UUID      | No       | FK → devices.id       |
+----------------------+-----------+----------+-----------------------+
| `gateway_id`         | UUID      | Yes      | FK → edge_gateways.id |
+----------------------+-----------+----------+-----------------------+
| `observed_at`        | TIMESTAMP | No       |                       |
+----------------------+-----------+----------+-----------------------+
| `window_start`       | TIMESTAMP | Yes      |                       |
+----------------------+-----------+----------+-----------------------+
| `window_end`         | TIMESTAMP | Yes      |                       |
+----------------------+-----------+----------+-----------------------+
| `processing_version` | VARCHAR   | Yes      |                       |
+----------------------+-----------+----------+-----------------------+
| `signal_quality`     | NUMERIC   | Yes      |                       |
+----------------------+-----------+----------+-----------------------+
| `source`             | VARCHAR   | No       |                       |
+----------------------+-----------+----------+-----------------------+
| `created_at`         | TIMESTAMP | No       |                       |
+----------------------+-----------+----------+-----------------------+

An observation may represent a point-in-time value or a window such as a 10-second PPG processing interval.

### 7.10 observation_values

Stores individual values within an observation.

+-----------------------+-----------+----------+---------------------------+
| Column                | Type      | Nullable | Constraints               |
+-----------------------+-----------+----------+---------------------------+
| `id`                  | UUID      | No       | PK                        |
+-----------------------+-----------+----------+---------------------------+
| `observation_id`      | UUID      | No       | FK → observations.id      |
+-----------------------+-----------+----------+---------------------------+
| `measurement_type_id` | UUID      | No       | FK → measurement_types.id |
+-----------------------+-----------+----------+---------------------------+
| `numeric_value`       | NUMERIC   | Yes      |                           |
+-----------------------+-----------+----------+---------------------------+
| `text_value`          | TEXT      | Yes      |                           |
+-----------------------+-----------+----------+---------------------------+
| `boolean_value`       | BOOLEAN   | Yes      |                           |
+-----------------------+-----------+----------+---------------------------+
| `unit`                | VARCHAR   | No       |                           |
+-----------------------+-----------+----------+---------------------------+
| `quality_score`       | NUMERIC   | Yes      |                           |
+-----------------------+-----------+----------+---------------------------+
| `created_at`          | TIMESTAMP | No       |                           |
+-----------------------+-----------+----------+---------------------------+

The appropriate value column is determined by `measurement_types.value_type`.

### 7.11 ppg_samples

Stores high-frequency raw PPG samples.

+-------------------+-----------+----------+-----------------+
| Column            | Type      | Nullable | Constraints     |
+-------------------+-----------+----------+-----------------+
| `id`              | BIGINT    | No       | PK              |
+-------------------+-----------+----------+-----------------+
| `device_id`       | UUID      | No       | FK → devices.id |
+-------------------+-----------+----------+-----------------+
| `sensor_id`       | UUID      | No       | FK → sensors.id |
+-------------------+-----------+----------+-----------------+
| `timestamp`       | TIMESTAMP | No       |                 |
+-------------------+-----------+----------+-----------------+
| `sequence_number` | BIGINT    | No       |                 |
+-------------------+-----------+----------+-----------------+
| `red_value`       | INTEGER   | No       |                 |
+-------------------+-----------+----------+-----------------+
| `ir_value`        | INTEGER   | No       |                 |
+-------------------+-----------+----------+-----------------+
| `signal_quality`  | NUMERIC   | Yes      |                 |
+-------------------+-----------+----------+-----------------+

At 100 samples/sec, a device can produce approximately 8.64 million samples/day, so this table will require separate performance and retention considerations.

### 7.12 ml_models

Stores metadata for ML models.

+----------------+-----------+----------+-------------+
| Column         | Type      | Nullable | Constraints |
+----------------+-----------+----------+-------------+
| `id`           | UUID      | No       | PK          |
+----------------+-----------+----------+-------------+
| `name`         | VARCHAR   | No       |             |
+----------------+-----------+----------+-------------+
| `version`      | VARCHAR   | No       |             |
+----------------+-----------+----------+-------------+
| `model_type`   | VARCHAR   | No       |             |
+----------------+-----------+----------+-------------+
| `framework`    | VARCHAR   | Yes      |             |
+----------------+-----------+----------+-------------+
| `artifact_uri` | TEXT      | Yes      |             |
+----------------+-----------+----------+-------------+
| `checksum`     | VARCHAR   | Yes      |             |
+----------------+-----------+----------+-------------+
| `status`       | VARCHAR   | No       |             |
+----------------+-----------+----------+-------------+
| `created_at`   | TIMESTAMP | No       |             |
+----------------+-----------+----------+-------------+
| `activated_at` | TIMESTAMP | Yes      |             |
+----------------+-----------+----------+-------------+
| `retired_at`   | TIMESTAMP | Yes      |             |
+----------------+-----------+----------+-------------+

### 7.13 ml_inferences

Represents a specific ML model execution.

+----------------------+-----------+----------+----------------------+
| Column               | Type      | Nullable | Constraints          |
+----------------------+-----------+----------+----------------------+
| `id`                 | UUID      | No       | PK                   |
+----------------------+-----------+----------+----------------------+
| `model_id`           | UUID      | No       | FK → ml_models.id    |
+----------------------+-----------+----------+----------------------+
| `patient_id`         | UUID      | No       | FK → patients.id     |
+----------------------+-----------+----------+----------------------+
| `device_id`          | UUID      | No       | FK → devices.id      |
+----------------------+-----------+----------+----------------------+
| `observation_id`     | UUID      | Yes      | FK → observations.id |
+----------------------+-----------+----------+----------------------+
| `inference_type`     | VARCHAR   | No       |                      |
+----------------------+-----------+----------+----------------------+
| `prediction`         | JSONB     | No       |                      |
+----------------------+-----------+----------+----------------------+
| `confidence`         | NUMERIC   | Yes      |                      |
+----------------------+-----------+----------+----------------------+
| `input_start`        | TIMESTAMP | Yes      |                      |
+----------------------+-----------+----------+----------------------+
| `input_end`          | TIMESTAMP | Yes      |                      |
+----------------------+-----------+----------+----------------------+
| `executed_at`        | TIMESTAMP | No       |                      |
+----------------------+-----------+----------+----------------------+
| `processing_time_ms` | NUMERIC   | Yes      |                      |
+----------------------+-----------+----------+----------------------+

`prediction` uses JSONB because different models can produce different structured outputs.

### 7.14 alerts

Represents events requiring attention.

+-----------------------+-----------+----------+---------------------------+
| Column                | Type      | Nullable | Constraints               |
+-----------------------+-----------+----------+---------------------------+
| `id`                  | UUID      | No       | PK                        |
+-----------------------+-----------+----------+---------------------------+
| `patient_id`          | UUID      | No       | FK → patients.id          |
+-----------------------+-----------+----------+---------------------------+
| `device_id`           | UUID      | Yes      | FK → devices.id           |
+-----------------------+-----------+----------+---------------------------+
| `observation_id`      | UUID      | Yes      | FK → observations.id      |
+-----------------------+-----------+----------+---------------------------+
| `measurement_type_id` | UUID      | Yes      | FK → measurement_types.id |
+-----------------------+-----------+----------+---------------------------+
| `alert_type`          | VARCHAR   | No       |                           |
+-----------------------+-----------+----------+---------------------------+
| `severity`            | VARCHAR   | No       |                           |
+-----------------------+-----------+----------+---------------------------+
| `message`             | TEXT      | No       |                           |
+-----------------------+-----------+----------+---------------------------+
| `trigger_value`       | NUMERIC   | Yes      |                           |
+-----------------------+-----------+----------+---------------------------+
| `threshold_value`     | NUMERIC   | Yes      |                           |
+-----------------------+-----------+----------+---------------------------+
| `status`              | VARCHAR   | No       |                           |
+-----------------------+-----------+----------+---------------------------+
| `created_at`          | TIMESTAMP | No       |                           |
+-----------------------+-----------+----------+---------------------------+
| `acknowledged_at`     | TIMESTAMP | Yes      |                           |
+-----------------------+-----------+----------+---------------------------+
| `acknowledged_by`     | UUID      | Yes      | FK → doctors.id           |
+-----------------------+-----------+----------+---------------------------+
| `resolved_at`         | TIMESTAMP | Yes      |                           |
+-----------------------+-----------+----------+---------------------------+

Alert lifecycle:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
CREATED
   |
   v
ACKNOWLEDGED
   |
   v
RESOLVED
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Optional future state:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
DISMISSED
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

8. Cardinality and Relationships
================================

The following cardinalities are the baseline to verify in the ER diagram.

8.1 Doctor ↔ Patient
--------------------

Relationship:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
DOCTOR
  N
  |
  |
  N
PATIENT
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Implemented through:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
patient_assignments
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Therefore:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Doctor 1:N PatientAssignment
Patient 1:N PatientAssignment
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

which produces:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Doctor N:M Patient
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A doctor can manage many patients, and a patient can be assigned to multiple doctors.

8.2 Doctor ↔ PatientAssignment
------------------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Doctor 1:N PatientAssignment
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One doctor can create/own many assignment records.

Each assignment belongs to one doctor.

8.3 Patient ↔ PatientAssignment
-------------------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Patient 1:N PatientAssignment
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A patient can have many historical/current assignment records.

Each assignment belongs to one patient.

8.4 Doctor ↔ Assignment.assigned_by
-----------------------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Doctor 1:N PatientAssignment
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A doctor can perform many assignments.

This is a second relationship between doctors and assignments and must be represented with a separate FK.

8.5 EdgeGateway ↔ Device
------------------------

Current architecture:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
EdgeGateway 1:N Device
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One Raspberry Pi can manage multiple patient devices.

A device has zero or one currently configured gateway in the current model.

Therefore:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
edge_gateways 1 ───── N devices
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

`devices.gateway_id` is nullable because a device can exist before being connected to a gateway.

8.6 Patient ↔ Device
--------------------

This is:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Patient N:M Device
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

over time, implemented through:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
device_assignments
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Reason:

A patient can use multiple devices over time.

A device can be reassigned to multiple patients over time.

Therefore:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Patient 1:N DeviceAssignment
Device 1:N DeviceAssignment
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

and:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Patient N:M Device
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

over the lifetime of the system.

8.7 Doctor ↔ DeviceAssignment.assigned_by
-----------------------------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Doctor 1:N DeviceAssignment
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A doctor can perform many device assignments.

8.8 Device ↔ Sensor
-------------------

Current design:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Device 1:N Sensor
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One device can contain multiple sensors.

Each sensor currently belongs to one device.

Future reusable sensor modules may require a `sensor_assignments` table.

8.9 Patient ↔ Observation
-------------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Patient 1:N Observation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One patient can have many observations.

Each observation belongs to one patient.

8.10 Device ↔ Observation
-------------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Device 1:N Observation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One device can generate many observations.

Each observation is associated with one device.

8.11 EdgeGateway ↔ Observation
------------------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
EdgeGateway 1:N Observation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A gateway can be associated with many observations.

`gateway_id` is nullable because some observations may be created without a known gateway, such as certain development/test scenarios.

8.12 Observation ↔ ObservationValue
-----------------------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Observation 1:N ObservationValue
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One observation can contain:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
HR
SpO2
Temperature
...
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Each observation value belongs to one observation.

8.13 MeasurementType ↔ ObservationValue
---------------------------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
MeasurementType 1:N ObservationValue
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One measurement type can occur in many observations.

Example:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
SPO2
  |
  +-- Observation 1
  +-- Observation 2
  +-- Observation 3
  +-- ...
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

8.14 Device ↔ PPGSample
-----------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Device 1:N PPGSample
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One device generates many PPG samples.

Each sample belongs to one device.

8.15 Sensor ↔ PPGSample
-----------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Sensor 1:N PPGSample
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One PPG sensor generates many samples.

Each sample references one sensor.

8.16 MLModel ↔ MLInference
--------------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
MLModel 1:N MLInference
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One model version can be used for many inferences.

Each inference references one model.

8.17 Patient ↔ MLInference
--------------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Patient 1:N MLInference
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One patient can have many ML inference records.

8.18 Device ↔ MLInference
-------------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Device 1:N MLInference
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One device can generate input for many ML inferences.

8.19 Observation ↔ MLInference
------------------------------

Current design:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Observation 1:N MLInference
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

but the FK is nullable.

This supports both:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
PPG
 ↓
ML inference
 ↓
Observation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

and development/research cases where an inference exists without being attached to a persisted observation.

If we later determine that every inference must always produce an observation, this can be changed to mandatory `1:N`.

8.20 Patient ↔ Alert
--------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Patient 1:N Alert
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A patient can have many alerts.

Each alert belongs to one patient.

8.21 Observation ↔ Alert
------------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Observation 1:N Alert
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One observation may trigger multiple alerts.

Example:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Observation
 ├── Low SpO2 alert
 └── Poor signal alert
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The FK is nullable because some alerts may be device/system alerts rather than observation-derived alerts.

8.22 MeasurementType ↔ Alert
----------------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
MeasurementType 1:N Alert
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One measurement type can be associated with many alerts.

Example:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
SPO2
 ├── Alert 1
 ├── Alert 2
 └── ...
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The relationship is optional because not every alert is about a physiological measurement.

8.23 Doctor ↔ Alert acknowledgement
-----------------------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Doctor 1:N Alert
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

through:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ 
alerts.acknowledged_by
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A doctor can acknowledge many alerts.

An alert can be acknowledged by zero or one doctor.

Therefore from the alert perspective:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Alert 0..1 ─── Doctor
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

9. Cardinality Summary
======================

+------------------------------------+---------------+---------------------+
| Relationship                       | Cardinality   | Implementation      |
+------------------------------------+---------------+---------------------+
| Doctor → PatientAssignment         | 1:N           | FK                  |
+------------------------------------+---------------+---------------------+
| Patient → PatientAssignment        | 1:N           | FK                  |
+------------------------------------+---------------+---------------------+
| Doctor ↔ Patient                   | N:M           | patient_assignments |
+------------------------------------+---------------+---------------------+
| Doctor → DeviceAssignment          | 1:N           | FK                  |
+------------------------------------+---------------+---------------------+
| Patient → DeviceAssignment         | 1:N           | FK                  |
+------------------------------------+---------------+---------------------+
| Device → DeviceAssignment          | 1:N           | FK                  |
+------------------------------------+---------------+---------------------+
| Patient ↔ Device                   | N:M over time | device_assignments  |
+------------------------------------+---------------+---------------------+
| EdgeGateway → Device               | 1:N           | devices.gateway_id  |
+------------------------------------+---------------+---------------------+
| Device → Sensor                    | 1:N           | sensors.device_id   |
+------------------------------------+---------------+---------------------+
| Patient → Observation              | 1:N           | FK                  |
+------------------------------------+---------------+---------------------+
| Device → Observation               | 1:N           | FK                  |
+------------------------------------+---------------+---------------------+
| EdgeGateway → Observation          | 1:N           | nullable FK         |
+------------------------------------+---------------+---------------------+
| Observation → ObservationValue     | 1:N           | FK                  |
+------------------------------------+---------------+---------------------+
| MeasurementType → ObservationValue | 1:N           | FK                  |
+------------------------------------+---------------+---------------------+
| Device → PPGSample                 | 1:N           | FK                  |
+------------------------------------+---------------+---------------------+
| Sensor → PPGSample                 | 1:N           | FK                  |
+------------------------------------+---------------+---------------------+
| MLModel → MLInference              | 1:N           | FK                  |
+------------------------------------+---------------+---------------------+
| Patient → MLInference              | 1:N           | FK                  |
+------------------------------------+---------------+---------------------+
| Device → MLInference               | 1:N           | FK                  |
+------------------------------------+---------------+---------------------+
| Observation → MLInference          | 1:N currently | nullable FK         |
+------------------------------------+---------------+---------------------+
| Patient → Alert                    | 1:N           | FK                  |
+------------------------------------+---------------+---------------------+
| Observation → Alert                | 1:N           | nullable FK         |
+------------------------------------+---------------+---------------------+
| MeasurementType → Alert            | 1:N           | nullable FK         |
+------------------------------------+---------------+---------------------+
| Doctor → acknowledged Alert        | 1:N           | nullable FK         |
+------------------------------------+---------------+---------------------+

10. Index Strategy
==================

Initial indexes should support expected access patterns.

Doctors
-------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
doctors.email
doctors.doctor_code
doctors.status
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Patients
--------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ 
patients.patient_code
patients.status
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Devices
-------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ 
devices.device_uid
devices.gateway_id
devices.status
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Sensors
-------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
sensors.device_id
sensors.sensor_type
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Assignments
-----------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ 
patient_assignments.doctor_id
patient_assignments.patient_id

device_assignments.device_id
device_assignments.patient_id
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Observations
------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
observations.patient_id
observations.device_id
observations.observed_at
(patient_id, observed_at)
(device_id, observed_at)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Observation values
------------------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
observation_values.observation_id
observation_values.measurement_type_id
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

PPG
---

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
ppg_samples.device_id
ppg_samples.sensor_id
ppg_samples.timestamp
(device_id, timestamp)
(sensor_id, timestamp)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

ML
--

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
ml_inferences.model_id
ml_inferences.patient_id
ml_inferences.device_id
ml_inferences.executed_at
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Alerts
------

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
alerts.patient_id
alerts.status
alerts.created_at
alerts.measurement_type_id
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Indexes should be validated using actual query plans after the first working implementation.

11. Raw PPG Storage Strategy
============================

PPG is high-frequency data.

At 100 samples/second:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
100 samples/sec
6,000 samples/min
360,000 samples/hour
8,640,000 samples/day
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

per device.

Therefore:

-   PPG must remain separate from observations.

-   Batch inserts should be considered.

-   Queries should generally use device + time range.

-   Retention policies will be required at scale.

-   Partitioning/compression may be introduced later.

-   A dedicated time-series or object-storage solution may eventually be evaluated.

The first implementation will use PostgreSQL.

12. Edge vs Cloud
=================

The Raspberry Pi should be able to continue operating during temporary cloud outages.

Conceptually:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
ESP32
  |
  v
Raspberry Pi
  |
  +--> Local processing
  |
  +--> Local storage
  |
  +--> Pending synchronization
  |
  v
Cloud
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Cloud storage will contain synchronized long-term data according to the eventual retention policy.

Edge-only synchronization tables are intentionally deferred until the edge architecture is designed.

13. Data Retention
==================

Final retention periods are not yet defined.

Initial direction:

-   Raw PPG: shorter/controlled retention depending on research and storage requirements.

-   Derived observations: longer retention.

-   Alerts: long-term retention for history/audit.

-   ML inferences: long-term retention for model traceability.

-   Device status/history: operational retention.

Production retention policies must be finalized before clinical deployment.

14. Extensibility
=================

14.1 Adding temperature
-----------------------

Add:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
sensors:
    sensor_type = TEMPERATURE
    model = TMP117
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

and:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
measurement_types:
    code = TEMPERATURE
    name = Body Temperature
    unit = °C
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

No schema modification is required.

14.2 Adding accelerometer
-------------------------

Add an accelerometer sensor.

If its raw data is high frequency, introduce a dedicated table such as:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
accelerometer_samples
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Derived data can continue to use:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
measurement_types
observations
observation_values
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

14.3 Replacing an ML model
--------------------------

Register:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
PPG Quality Model v1
PPG Quality Model v2
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

New inference records reference v2.

Old inference records continue to reference v1.

15. OOP/Application Architecture Mapping
========================================

The database will eventually be accessed through a layered application architecture:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
API
 |
 v
Application / Service Layer
 |
 v
Domain Layer
 |
 v
Repository Layer
 |
 v
SQLAlchemy
 |
 v
PostgreSQL
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Likely domain concepts include:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Doctor
Patient
Device
Sensor
EdgeGateway
Observation
Measurement
Alert
MLModel
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Database models should not automatically become the entire domain model.

16. Transactions
================

Operations that modify multiple related records should use transactions.

Example:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Create patient
+
Create assignment
+
Assign device
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

If a required step fails, the transaction should roll back to avoid inconsistent state.

17. Soft Delete and Status
==========================

Important records should generally not be physically deleted.

Examples:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Patient:
    ACTIVE
    INACTIVE

Device:
    ACTIVE
    OFFLINE
    MAINTENANCE
    RETIRED

Gateway:
    ONLINE
    OFFLINE
    MAINTENANCE

ML Model:
    DEVELOPMENT
    ACTIVE
    RETIRED
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Exact status values will be finalized during implementation.

18. Timestamp Strategy
======================

All timestamps should be stored in UTC.

The frontend can convert UTC timestamps to the user's local timezone for display.

This avoids ambiguity when systems operate across locations.

19. Schema Versioning
=====================

Database schema changes will be managed through migrations.

Planned migration tooling:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
SQLAlchemy
+
Alembic
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Example:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Migration 001 → Initial schema
Migration 002 → Alerts
Migration 003 → ML inference
Migration 004 → Future feature
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Manual production schema modification should be avoided.

20. Security
============

The database design assumes:

-   authenticated users

-   authorization based on patient assignments

-   hashed passwords

-   protected database credentials

-   encrypted network communication

-   least-privilege database access

-   validation of device messages

-   auditability of sensitive actions

The final clinical/security requirements must be reviewed separately before real-world deployment.

21. Known Future Considerations
===============================

The following are deliberately deferred:

### Sensor assignments

If sensors become reusable modules, introduce:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
sensor_assignments
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

### Time-series optimization

Possible future technologies:

-   PostgreSQL partitioning

-   compression

-   time-series extensions

-   object storage

-   dedicated time-series databases

### Multi-organization support

Potential future entities:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
organizations
facilities
departments
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

### Advanced authorization

Potential future entities:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
roles
permissions
role_permissions
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

22. Non-Goals
=============

Version 0.1 does not attempt to model:

-   complete electronic medical records

-   prescriptions

-   laboratory systems

-   billing

-   insurance

-   hospital scheduling

-   complete clinical workflows

The database is focused on the wireless physiological monitoring system.

23. Approved Architecture Decisions
===================================

1.  PostgreSQL is the primary relational database.

2.  Raw high-frequency signals are separated from derived observations.

3.  Derived measurements use `observations`, `observation_values`, and `measurement_types`.

4.  Device assignment history is stored separately from devices.

5.  ML models are explicitly versioned.

6.  ML inferences reference the model version that generated them.

7.  UUIDs are used for most entity identifiers.

8.  PPG samples use BIGINT identifiers due to expected volume.

9.  JSONB is used only for genuinely variable structures such as sensor configuration and ML output.

10. Edge operation should continue during temporary cloud outages.

11. Database schema changes use migrations.

12. The architecture must support future sensor additions without redesigning the core observation schema.

24. Version History
===================

+---------+------------+-------------------------------+
| Version | Date       | Description                   |
+---------+------------+-------------------------------+
| 0.1     | 2026-08-25 | Initial database architecture |
+---------+------------+-------------------------------+

25. Next Steps
==============

Before implementing SQLAlchemy models:

1.  Create the formal ER diagram.

2.  Verify every relationship cardinality.

3.  Verify optional vs mandatory relationships.

4.  Review primary and foreign keys.

5.  Review unique constraints.

6.  Finalize indexes.

7.  Define initial `measurement_types` seed data.

8.  Define naming conventions.

9.  Create SQLAlchemy models.

10. Create the first Alembic migration.

11. Start PostgreSQL with Docker Compose.

12. Run database integration tests.

The implementation path is:

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ text
Database Design
      |
      v
ER Diagram
      |
      v
Cardinality Verification
      |
      v
SQLAlchemy Models
      |
      v
Alembic Migration
      |
      v
PostgreSQL
      |
      v
Database Tests
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
