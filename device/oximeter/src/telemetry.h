#pragma once
/*
 * telemetry.h - Wi-Fi + MQTT uplink (PubSubClient) running in its own FreeRTOS task on
 * CPU core 0, so network delays never block sensor sampling on core 1.
 *
 * Messages are copied into a queue by telemetryEnqueue() and published in order. While the
 * network is down they are buffered (up to 60 messages without PSRAM, 1200 with PSRAM) and sent when
 * the connection returns; when the buffer is full the newest message is dropped and counted.
 * Without PSRAM (classic ESP32 Dev Module) a message is also dropped (and counted) when free
 * heap would fall below TELEMETRY_HEAP_RESERVE, so Wi-Fi/MQTT can always reconnect.
 */
#include <stdint.h>
#include <stddef.h>

struct TelemetryConfig {
  const char* wifiSsid;
  const char* wifiPassword;
  const char* brokerHost;
  uint16_t    brokerPort;
  const char* username;          // "" = anonymous
  const char* password;
  const char* deviceId;          // also the MQTT client-id prefix
  const char* statusTopic;       // retained online/offline status (last will = offline)
  const char* fwVersion;
  int         queueDepthInternal;
  int         queueDepthPsram;
  uint16_t    keepAliveS;
  uint32_t    statusIntervalMs;
  uint16_t    maxPacketBytes;    // PubSubClient buffer size
};

struct TelemetryStats {
  bool     wifiUp;
  bool     mqttUp;
  int      rssi;
  int      mqttState;            // PubSubClient::state() of the last attempt
  uint32_t published;            // messages delivered to the broker
  uint32_t dropped;              // messages lost because the buffer was full
  uint32_t queued;               // messages waiting right now
  uint32_t queueDepth;
  uint32_t reconnects;
  char     ip[16];
};

void           telemetryBegin(const TelemetryConfig& cfg);
bool           telemetryEnqueue(const char* topic, const char* payload, size_t len, bool retain = false);
TelemetryStats telemetryStats();
uint64_t       telemetryUtcMs();          // wall-clock UTC in ms, 0 until NTP time is available
void           telemetrySetSensorState(const char* s);   // reported in the status message

// One iteration of the network loop (the task calls this forever; exposed for host tests).
void           telemetryStep();
