#include <Arduino.h>
#include <WiFi.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

#include "config.h"
#include "max30102.h"
#include "ppg_algorithm.h"
#include "telemetry.h"

// =====================================================
// STATE
// =====================================================

// Sensor
static bool     sensorOk = false;
static uint32_t nextSensorTry = 0;
static uint32_t sensorRetryMs = SENSOR_RETRY_MIN_MS;
static uint32_t lastRed = 0, lastIr = 0;

// One packet = SAMPLES_PER_PACKET raw samples at 100 Hz (1 second)
static uint32_t redBuffer[SAMPLES_PER_PACKET];
static uint32_t irBuffer[SAMPLES_PER_PACKET];
static int      packetCount = 0;
static uint32_t packetStartMs = 0;

// The algorithm runs at 50 Hz: it receives the mean of each 100 Hz sample pair
static bool     havePair = false;
static uint32_t pairRed = 0, pairIr = 0;

// LED auto-gain and sample-rate measurement
static uint32_t lastAgcMs = 0;
static uint32_t rateStartMs = 0, rateSamples = 0;

static char payload[PACKET_JSON_MAX];


// =====================================================
// SENSOR
// =====================================================

static bool startSensor()
{
    maxBeginBus(SDA_PIN, SCL_PIN, I2C_CLOCK_HZ);

    MaxInitReport report;
    MaxInitResult result = maxInit(&report);

    if (result != MAX_OK)
    {
        Serial.printf(
            "MAX30102 not ready: %s (SDA %d, SCL %d, I2C code %u)\n",
            maxResultName(result), SDA_PIN, SCL_PIN, report.ackError
        );

        uint8_t found[8];
        int n = maxScanBus(found, 8);

        Serial.printf("I2C devices found: %d", n);
        for (int i = 0; i < n && i < 8; i++)
        {
            Serial.printf(" 0x%02X", found[i]);
        }
        Serial.println();

        return false;
    }

    Serial.printf(
        "MAX30102 ready (part 0x%02X, rev %u).\n",
        report.partId, report.revId
    );

    return true;
}

// Drop everything collected so far: the algorithm must not join samples
// from before and after a gap as if they were continuous.
static void resetStream()
{
    algoSensorGap();
    packetCount = 0;
    havePair = false;
    rateStartMs = millis();
    rateSamples = 0;
}

static void handleSensorFailure()
{
    Serial.println("Sensor read failed, recovering I2C bus...");

    resetStream();
    maxBusRecover(SDA_PIN, SCL_PIN, I2C_CLOCK_HZ);

    if (maxConfigIntact() && maxFlushFifo())
    {
        return;
    }

    sensorOk = startSensor();

    if (!sensorOk)
    {
        nextSensorTry = millis() + sensorRetryMs;
        telemetrySetSensorState(STATE_NAMES[ST_SENSOR_ERROR]);
    }
}


// =====================================================
// PUBLISH PPG PACKET
// =====================================================

// Appends to payload; returns false once the buffer is full.
static bool append(int& len, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int n = vsnprintf(payload + len, sizeof(payload) - len, format, args);
    va_end(args);

    if (n < 0 || len + n >= (int)sizeof(payload))
    {
        return false;
    }

    len += n;
    return true;
}

static void publishPacket()
{
    const AlgoOut& out = algoOut();
    MeasState state = algoState(sensorOk);

    char heartRate[8] = "null";
    char spo2[8] = "null";

    if (out.hrValid)
    {
        snprintf(heartRate, sizeof(heartRate), "%ld", lroundf(out.hrBpm));
    }

    if (out.spo2Valid)
    {
        snprintf(spo2, sizeof(spo2), "%ld", lroundf(out.spo2));
    }

    int len = 0;
    bool ok = append(
        len,
        "{\"device_id\":\"%s\",\"fw_version\":\"%s\",\"timestamp_ms\":%lu,"
        "\"utc_ms\":%llu,\"sample_rate\":%d,\"sample_count\":%d,"
        "\"heart_rate\":%s,\"spo2\":%s,\"state\":\"%s\",\"contact\":%s,"
        "\"quality\":%.0f,\"pi\":%.2f,\"motion\":\"%s\",\"red\":[",
        DEVICE_ID, FW_VERSION, (unsigned long)packetStartMs,
        (unsigned long long)telemetryUtcMs(), SAMPLE_RATE, packetCount,
        heartRate, spo2, STATE_NAMES[state], out.contact ? "true" : "false",
        out.quality, out.pi, MOTION_NAMES[out.motion]
    );

    for (int i = 0; ok && i < packetCount; i++)
    {
        ok = append(len, i ? ",%lu" : "%lu", (unsigned long)redBuffer[i]);
    }

    ok = ok && append(len, "],\"ir\":[");

    for (int i = 0; ok && i < packetCount; i++)
    {
        ok = append(len, i ? ",%lu" : "%lu", (unsigned long)irBuffer[i]);
    }

    ok = ok && append(len, "]}");

    packetCount = 0;

    if (!ok)
    {
        Serial.println("ERROR: PPG payload too large, packet dropped.");
        return;
    }

    if (!telemetryEnqueue(MQTT_PPG_TOPIC, payload, len))
    {
        Serial.println("ERROR: send queue full, packet dropped.");
    }
}


// =====================================================
// SAMPLE HANDLING
// =====================================================

static void onSample(uint32_t red, uint32_t ir)
{
    lastRed = red;
    lastIr = ir;
    rateSamples++;

    if (packetCount == 0)
    {
        packetStartMs = millis();
    }

    redBuffer[packetCount] = red;
    irBuffer[packetCount] = ir;
    packetCount++;

    if (havePair)
    {
        algoPush((pairRed + red) / 2, (pairIr + ir) / 2);
        havePair = false;
    }
    else
    {
        pairRed = red;
        pairIr = ir;
        havePair = true;
    }

    if (packetCount == SAMPLES_PER_PACKET)
    {
        publishPacket();
    }
}

static void maintainSignal()
{
    uint32_t now = millis();
    const AlgoOut& out = algoOut();

    // Keep the LED current in the ADC's linear range while a finger is on.
    if (out.contact && now - lastAgcMs >= AGC_INTERVAL_MS)
    {
        lastAgcMs = now;
        maxAutoGain(
            algoDcIr(), algoDcRed(),
            algoIrContactMin(), algoRedContactMin(),
            lastIr, lastRed
        );
    }

    // Correct heart rate for the sensor's clock error.
    if (now - rateStartMs >= FS_MEASURE_MS)
    {
        float measured = rateSamples * 1000.0f / (now - rateStartMs);
        algoSetFsScale(measured / SAMPLE_RATE);
        rateStartMs = now;
        rateSamples = 0;
    }

    // Do not let one FIFO hiccup hold back a whole packet.
    if (packetCount > 0 && now - packetStartMs > PACKET_FORCE_PUBLISH_MS)
    {
        publishPacket();
    }
}


// =====================================================
// SERIAL STATUS (once per second)
// =====================================================

// Why Wi-Fi is not connected, in words (WiFi.status() codes).
static const char* wifiReason()
{
    switch (WiFi.status())
    {
        case WL_NO_SSID_AVAIL:  return "down: network not found";
        case WL_CONNECT_FAILED: return "down: wrong password?";
        case WL_CONNECTION_LOST: return "down: connection lost";
        default:                return "down: connecting";
    }
}

static void printStatus()
{
    static uint32_t lastPrint = 0;

    if (millis() - lastPrint < 1000)
    {
        return;
    }

    lastPrint = millis();

    const AlgoOut& out = algoOut();
    TelemetryStats net = telemetryStats();

    Serial.printf("[%s] ", STATE_NAMES[algoState(sensorOk)]);

    if (out.hrValid)
    {
        Serial.printf("HR %3.0f bpm%s  ", out.hrBpm, out.hrHeld ? " (held)" : "");
    }
    else
    {
        Serial.print("HR  --      ");
    }

    if (out.spo2Valid)
    {
        Serial.printf("SpO2 %2.0f %%%s  ", out.spo2, out.spo2Held ? " (held)" : "");
    }
    else
    {
        Serial.print("SpO2 --  ");
    }

    if (out.contact && out.warmupPct < 100)
    {
        Serial.printf("warm-up %3.0f%%  ", out.warmupPct);
    }

    Serial.printf(
        "PI %.2f  Q %.0f  | Wi-Fi %s  MQTT %s  sent %lu  queued %lu  dropped %lu\n",
        out.pi, out.quality,
        net.wifiUp ? net.ip : wifiReason(),
        net.mqttUp ? "up" : (net.wifiUp ? "connecting" : "-"),
        (unsigned long)net.published, (unsigned long)net.queued,
        (unsigned long)net.dropped
    );
}


// =====================================================
// SETUP
// =====================================================

void setup()
{
    Serial.begin(SERIAL_BAUD);

    delay(2000);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" ESP32 WIRELESS OXIMETER v" FW_VERSION);
    Serial.println(" MAX30102 + Wi-Fi + MQTT");
    Serial.println("======================================");
    Serial.printf("Device %s -> broker %s:%d, topic %s\n",
                  DEVICE_ID, MQTT_BROKER, MQTT_PORT_NUMBER, MQTT_PPG_TOPIC);

    algoInit();

    TelemetryConfig net = {};
    net.wifiSsid = WIFI_SSID;
    net.wifiPassword = WIFI_PASSWORD;
    net.brokerHost = MQTT_BROKER;
    net.brokerPort = MQTT_PORT_NUMBER;
    net.username = MQTT_USERNAME;
    net.password = MQTT_PASSWORD;
    net.deviceId = DEVICE_ID;
    net.statusTopic = MQTT_STATUS_TOPIC;
    net.fwVersion = FW_VERSION;
    net.queueDepthInternal = QUEUE_PACKETS_INTERNAL;
    net.queueDepthPsram = QUEUE_PACKETS_PSRAM;
    net.keepAliveS = MQTT_KEEPALIVE_S;
    net.statusIntervalMs = STATUS_INTERVAL_MS;
    net.maxPacketBytes = PACKET_JSON_MAX;

    // Wi-Fi and MQTT run in their own task on core 0, so sampling below
    // never waits for the network.
    telemetryBegin(net);

    sensorOk = startSensor();

    if (!sensorOk)
    {
        nextSensorTry = millis() + sensorRetryMs;
    }

    resetStream();
}


// =====================================================
// LOOP
// =====================================================

void loop()
{
    printStatus();

    if (!sensorOk)
    {
        telemetrySetSensorState(STATE_NAMES[ST_SENSOR_ERROR]);

        if ((int32_t)(millis() - nextSensorTry) >= 0)
        {
            sensorOk = startSensor();

            if (sensorOk)
            {
                sensorRetryMs = SENSOR_RETRY_MIN_MS;
                resetStream();
            }
            else
            {
                sensorRetryMs = sensorRetryMs * 2 > SENSOR_RETRY_MAX_MS
                    ? SENSOR_RETRY_MAX_MS
                    : sensorRetryMs * 2;
                nextSensorTry = millis() + sensorRetryMs;
            }
        }

        delay(50);
        return;
    }

    // Drain the sensor FIFO (32 samples = 320 ms at 100 Hz).
    uint32_t red[32], ir[32];
    int n = maxReadFifo(red, ir, 32);

    if (n < 0)
    {
        handleSensorFailure();
        return;
    }

    for (int i = 0; i < n; i++)
    {
        onSample(red[i], ir[i]);
    }

    maintainSignal();
    telemetrySetSensorState(STATE_NAMES[algoState(sensorOk)]);

    delay(10);
}
