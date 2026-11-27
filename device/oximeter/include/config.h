#pragma once
/*
 * config.h - firmware build-time settings.
 * Network credentials and device identity are in secrets.h (see secrets.example.h).
 * Any value guarded by #ifndef below can be overridden by a #define in secrets.h.
 */

#define FW_VERSION              "3.2.4"

// ---- Wiring (MAX30102 I2C) -------------------------------------------------------------
// ESP32-S3 DevKitC: SDA 8 / SCL 9.  Classic ESP32 Dev Module: SDA 21 / SCL 22.
#ifndef SDA_PIN
#if CONFIG_IDF_TARGET_ESP32S3
#define SDA_PIN                 8
#else
#define SDA_PIN                 21
#endif
#endif
#ifndef SCL_PIN
#if CONFIG_IDF_TARGET_ESP32S3
#define SCL_PIN                 9
#else
#define SCL_PIN                 22
#endif
#endif
#ifndef I2C_CLOCK_HZ
#define I2C_CLOCK_HZ            400000UL    // set 100000UL in secrets.h for weak pull-ups
#endif

// ---- Sampling --------------------------------------------------------------------------
// MAX30102: 400 samples/s, 4x on-chip averaging, 411 us pulses (18-bit) -> 100 Hz to ESP32.
// Every full packet carries exactly SAMPLE_RATE samples = 1 second of raw red + IR data.
#define SAMPLE_RATE             100
#define SAMPLES_PER_PACKET      SAMPLE_RATE

// If a packet has not filled within this many ms of its first sample (I2C glitch / FIFO
// hiccup), publish what we have and restart. Must be > 1000 ms.
#ifndef PACKET_FORCE_PUBLISH_MS
#define PACKET_FORCE_PUBLISH_MS 1500UL
#endif

// LED auto-gain runs this often while a finger is on the sensor.
#define AGC_INTERVAL_MS         1500UL

// The MAX30102 clock drifts by 1-2 %; the true rate is measured over this period and passed
// to the algorithm so heart rates are corrected.
#define FS_MEASURE_MS           10000UL

// ---- MQTT ------------------------------------------------------------------------------
#ifndef MQTT_PORT_NUMBER
#define MQTT_PORT_NUMBER        1883
#endif
#ifndef MQTT_USERNAME
#define MQTT_USERNAME           ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD           ""
#endif
#define MQTT_KEEPALIVE_S        30
#define STATUS_INTERVAL_MS      30000UL
#define PACKET_JSON_MAX         4096
#define QUEUE_PACKETS_INTERNAL  60
#define QUEUE_PACKETS_PSRAM     1200

// ---- Serial monitor --------------------------------------------------------------------
#define SERIAL_BAUD             115200
#define SENSOR_RETRY_MIN_MS     1000UL
#define SENSOR_RETRY_MAX_MS     10000UL
