# ESP32 Firmware Setup

The firmware targets an ESP32-S3 DevKitC-1 (default) or a classic ESP32 Dev
Module connected to a MAX30102 sensor. PlatformIO manages the Arduino framework
and the PubSubClient MQTT library; the MAX30102 driver is part of the firmware.

## Wiring

| MAX30102 | ESP32-S3 | Classic ESP32 |
| --- | --- | --- |
| SDA | GPIO 8 | GPIO 21 |
| SCL | GPIO 9 | GPIO 22 |
| VIN | 3.3 V | 3.3 V |
| GND | GND | GND |

The defaults are in `device/oximeter/include/config.h` and follow the board
being built. To use other pins, define `SDA_PIN` and `SCL_PIN` in `secrets.h`.

Build for the classic ESP32 with `pio run -e esp32dev --target upload`.

## Configure and Upload

From the repository root:

```bash
cd device/oximeter
cp include/secrets.example.h include/secrets.h
```

Edit `include/secrets.h` with the local Wi-Fi network, MQTT broker address,
device identifier, and device-specific topics. The file is ignored by Git and
must not contain credentials that should be committed. If the file is absent,
the firmware falls back to `include/secrets.example.h`, which is useful for
IntelliSense and clean builds but does not provide working network credentials.

Install PlatformIO, then build and upload:

```bash
pio run --target upload
pio device monitor
```

The serial monitor uses `115200` baud.

## MQTT Payload

Each packet is published to the configured PPG topic, normally
`oximeter/<device-number>/ppg`:

```json
{
  "device_id": "OXI001",
  "timestamp_ms": 123456,
  "sample_rate": 100,
  "sample_count": 100,
  "heart_rate": 72,
  "spo2": 98,
  "red": [1, 2, 3],
  "ir": [4, 5, 6]
}
```

The production packet contains `sample_count` values in both arrays.
`heart_rate` and `spo2` are computed on the device and are `null` when the
algorithm cannot verify them (no finger, warm-up, motion it cannot separate).
The packet also carries `state`, `contact`, `quality`, `pi`, `motion`,
`fw_version` and `utc_ms` (0 until NTP time is available); the backend stores
only the fields listed in the MQTT ingestion guide and ignores the rest. `DEVICE_ID` must match a device registered
in the database (see `backend/seed_dev_data.py`). See
[MQTT telemetry ingestion](../mqtt/telemetry-ingestion.md) for how the backend
validates and stores each packet.
