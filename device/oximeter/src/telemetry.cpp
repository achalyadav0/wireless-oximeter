#include "telemetry.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdio.h>
#ifndef HOST_SIM
#include <esp_heap_caps.h>
#endif

#ifndef TELEMETRY_HEAP_RESERVE
#define TELEMETRY_HEAP_RESERVE  48000      // bytes of internal heap kept free for Wi-Fi / TCP / MQTT
#endif

// Queue element: one heap block "topic\0payload" (+ length / retain flag).
struct QItem { char* buf; uint16_t topicLen; uint16_t payloadLen; bool retain; };

static TelemetryConfig cfg;
static QueueHandle_t   upQ = nullptr;
static bool            usePsram = false;
static WiFiClient      net;
static PubSubClient    mqtt(net);
static char            clientId[48];
static char            willMsg[128];

// statistics: each field is written by ONE task only (32-bit aligned writes are atomic)
static TelemetryStats  stats;
static uint32_t        lastWifiTry = 0, lastMqttTry = 0, lastStatus = 0;
static bool            ntpStarted = false;
static const char*     sensorState = "init";

static void* allocBuf(size_t n) {
#ifndef HOST_SIM
  if (usePsram) return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
#endif
  return malloc(n);
}

bool telemetryEnqueue(const char* topic, const char* payload, size_t len, bool retain) {
  if (!upQ || !topic || !topic[0]) return false;
  size_t tl = strlen(topic);
  if (tl > 200 || len > cfg.maxPacketBytes - tl - 16) { stats.dropped++; return false; }
#ifndef HOST_SIM
  // Classic ESP32 without PSRAM: the offline buffer lives in the same heap that Wi-Fi/lwIP
  // needs to reconnect. Never let buffered packets eat that reserve (drop + count instead).
  if (!usePsram && ESP.getFreeHeap() < (uint32_t)(tl + len + TELEMETRY_HEAP_RESERVE)) { stats.dropped++; return false; }
#endif
  QItem it;
  it.buf = (char*)allocBuf(tl + 1 + len + 1);
  if (!it.buf) { stats.dropped++; return false; }
  memcpy(it.buf, topic, tl + 1);
  memcpy(it.buf + tl + 1, payload, len);
  it.buf[tl + 1 + len] = 0;
  it.topicLen = (uint16_t)tl; it.payloadLen = (uint16_t)len; it.retain = retain;
  if (xQueueSend(upQ, &it, 0) != pdTRUE) { free(it.buf); stats.dropped++; return false; }   // full
  return true;
}

uint64_t telemetryUtcMs() {
  struct timeval tv; gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1600000000L) return 0;                    // not synchronised yet
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

void telemetrySetSensorState(const char* s) { sensorState = s; }

static void publishStatus(bool online) {
  char b[400];
  int n = snprintf(b, sizeof(b),
                   "{\"device_id\":\"%s\",\"status\":\"%s\",\"fw_version\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
                   "\"uptime_s\":%lu,\"sensor\":\"%s\",\"queued\":%lu,\"published\":%lu,\"dropped\":%lu}",
                   cfg.deviceId, online ? "online" : "offline", cfg.fwVersion, stats.ip, stats.rssi,
                   (unsigned long)(millis() / 1000), sensorState, (unsigned long)stats.queued,
                   (unsigned long)stats.published, (unsigned long)stats.dropped);
  if (n > 0 && n < (int)sizeof(b)) mqtt.publish(cfg.statusTopic, (const uint8_t*)b, n, true);
}

void telemetryStep() {
  // ---- Wi-Fi (non-blocking: begin, then poll status) ----
  if (WiFi.status() != WL_CONNECTED) {
    if (stats.wifiUp) { stats.wifiUp = false; stats.mqttUp = false; }
    if (lastWifiTry == 0 || millis() - lastWifiTry > 15000) {
      WiFi.disconnect();
      WiFi.begin(cfg.wifiSsid, cfg.wifiPassword);
      lastWifiTry = millis();
    }
    stats.queued = upQ ? uxQueueMessagesWaiting(upQ) : 0;
    delay(200);
    return;
  }
  if (!stats.wifiUp) {
    stats.wifiUp = true;
    snprintf(stats.ip, sizeof(stats.ip), "%s", WiFi.localIP().toString().c_str());
  }
  stats.rssi = WiFi.RSSI();
  if (!ntpStarted) { configTime(0, 0, "pool.ntp.org", "time.google.com"); ntpStarted = true; }

  // ---- MQTT (connect with last will = offline status) ----
  if (!mqtt.connected()) {
    if (stats.mqttUp) { stats.mqttUp = false; stats.reconnects++; }
    if (lastMqttTry != 0 && millis() - lastMqttTry < 3000) { delay(50); return; }
    lastMqttTry = millis();
    const char* u = (cfg.username && cfg.username[0]) ? cfg.username : nullptr;
    const char* p = (u && cfg.password && cfg.password[0]) ? cfg.password : nullptr;
    bool ok = mqtt.connect(clientId, u, p, cfg.statusTopic, 0, true, willMsg, true);
    stats.mqttState = mqtt.state();
    if (!ok) return;
    stats.mqttUp = true;
    publishStatus(true);
    lastStatus = millis();
  }

  // ---- publish queued messages in order; a message leaves the queue only once sent ----
  QItem it; int burst = 0;
  while (burst < 10 && xQueuePeek(upQ, &it, 0) == pdTRUE) {
    if (!mqtt.publish(it.buf, (const uint8_t*)(it.buf + it.topicLen + 1), it.payloadLen, it.retain)) {
      mqtt.disconnect();                       // force a clean reconnect; message stays queued
      break;
    }
    xQueueReceive(upQ, &it, 0); free(it.buf); stats.published++; burst++;
  }
  stats.queued = uxQueueMessagesWaiting(upQ);
  mqtt.loop();                                 // keep-alive / incoming packets
  if (mqtt.connected() && millis() - lastStatus > cfg.statusIntervalMs) { publishStatus(true); lastStatus = millis(); }
  delay(burst ? 1 : 10);
}

#ifndef HOST_SIM
static void netTask(void*) { for (;;) telemetryStep(); }
#endif

void telemetryBegin(const TelemetryConfig& c) {
  cfg = c;
  memset(&stats, 0, sizeof(stats));
  snprintf(stats.ip, sizeof(stats.ip), "-");
#ifndef HOST_SIM
  uint64_t mac = ESP.getEfuseMac();
  snprintf(clientId, sizeof(clientId), "%s-%06lx", cfg.deviceId, (unsigned long)((mac >> 24) & 0xFFFFFF));
  usePsram = psramFound();
#else
  snprintf(clientId, sizeof(clientId), "%s-host", cfg.deviceId);
#endif
  snprintf(willMsg, sizeof(willMsg), "{\"device_id\":\"%s\",\"status\":\"offline\"}", cfg.deviceId);
  stats.queueDepth = usePsram ? cfg.queueDepthPsram : cfg.queueDepthInternal;
  upQ = xQueueCreate(stats.queueDepth, sizeof(QItem));
  mqtt.setServer(cfg.brokerHost, cfg.brokerPort);
  mqtt.setBufferSize(cfg.maxPacketBytes);
  mqtt.setKeepAlive(cfg.keepAliveS);
  mqtt.setSocketTimeout(5);
#ifndef HOST_SIM
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                        // lower latency, steadier throughput
  xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);
#endif
}

TelemetryStats telemetryStats() {
  TelemetryStats s = stats;
  if (upQ) s.queued = uxQueueMessagesWaiting(upQ);
  return s;
}
