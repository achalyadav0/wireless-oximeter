# ESP32 Firmware Setup

The firmware targets an ESP32-S3 DevKitC-1 connected to a MAX30102 sensor.
PlatformIO manages the Arduino framework and the two required libraries.

## Wiring

| MAX30102 | ESP32-S3 |
| --- | --- |
| SDA | GPIO 8 |
| SCL | GPIO 9 |
| VIN | 3.3 V |
| GND | GND |

The pin assignments are defined near the top of `device/oximeter/src/main.cpp`.
Change them there if the board wiring differs.

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
  "red": [1, 2, 3],
  "ir": [4, 5, 6]
}
```

The production packet contains `sample_count` values in both arrays. The
backend validates `device_id`, `red`, and `ir`, stores the latest packet in
memory, and exposes it at `/api/devices` for the dashboard.
