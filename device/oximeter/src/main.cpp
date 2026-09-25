#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>

#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

// =====================================================
// PIN CONFIGURATION
// =====================================================

#define SDA_PIN 8
#define SCL_PIN 9

// =====================================================
// SENSOR CONFIGURATION
// =====================================================

#define BUFFER_SIZE 100
#define SAMPLE_RATE 100

// =====================================================
// WI-FI CONFIGURATION
// =====================================================

// =====================================================
// MQTT CONFIGURATION
// =====================================================

const int MQTT_PORT = 1883;

// =====================================================
// OBJECTS
// =====================================================

MAX30105 sensor;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// =====================================================
// PPG BUFFERS
// =====================================================

uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];

// =====================================================
// HR / SPO2 RESULTS
// =====================================================

int32_t spo2;
int8_t validSpO2;

int32_t heartRate;
int8_t validHeartRate;


// =====================================================
// CONNECT TO WIFI
// =====================================================

void connectWiFi()
{
    Serial.println();
    Serial.print("Connecting to Wi-Fi: ");
    Serial.println(WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("Wi-Fi connected.");

    Serial.print("ESP32 IP address: ");
    Serial.println(WiFi.localIP());
}


// =====================================================
// CONNECT TO MQTT
// =====================================================

void connectMQTT()
{
    while (!mqttClient.connected())
    {
        Serial.print("Connecting to MQTT broker... ");

        String clientId = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

        if (mqttClient.connect(clientId.c_str()))
        {
            Serial.println("connected.");

            mqttClient.publish(
                MQTT_STATUS_TOPIC,
                "ESP32 connected",
                true
            );
        }
        else
        {
            Serial.print("failed, state=");
            Serial.println(mqttClient.state());

            delay(2000);
        }
    }
}


// =====================================================
// PUBLISH PPG DATA
// =====================================================

void publishPPG()
{
    // -------------------------------------------------
    // Create JSON payload
    // -------------------------------------------------

    String payload;

    payload.reserve(5000);

    payload += "{";

    payload += "\"device_id\":\"";
    payload += DEVICE_ID;
    payload += "\",";

    payload += "\"timestamp_ms\":";
    payload += String(millis());
    payload += ",";

    payload += "\"sample_rate\":";
    payload += String(SAMPLE_RATE);
    payload += ",";

    payload += "\"sample_count\":";
    payload += String(BUFFER_SIZE);
    payload += ",";


    // -------------------------------------------------
    // RED samples
    // -------------------------------------------------

    payload += "\"red\":[";

    for (int i = 0; i < BUFFER_SIZE; i++)
    {
        payload += String(redBuffer[i]);

        if (i < BUFFER_SIZE - 1)
        {
            payload += ",";
        }
    }

    payload += "],";


    // -------------------------------------------------
    // IR samples
    // -------------------------------------------------

    payload += "\"ir\":[";

    for (int i = 0; i < BUFFER_SIZE; i++)
    {
        payload += String(irBuffer[i]);

        if (i < BUFFER_SIZE - 1)
        {
            payload += ",";
        }
    }

    payload += "]";

    payload += "}";


    // -------------------------------------------------
    // Publish
    // -------------------------------------------------
    Serial.print("PPG payload size: ");
    Serial.print(payload.length());
    Serial.println(" bytes");

    bool success = mqttClient.publish(
        MQTT_PPG_TOPIC,
        payload.c_str()
    );

    if (success)
    {
        Serial.println("PPG packet published to MQTT.");
    }
    else
    {
        Serial.println("ERROR: MQTT publish failed.");
    }
}


// =====================================================
// SETUP
// =====================================================

void setup()
{
    Serial.begin(115200);

    delay(2000);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" ESP32-S3 WIRELESS OXIMETER");
    Serial.println(" MAX30102 + Wi-Fi + MQTT");
    Serial.println("======================================");


    // -------------------------------------------------
    // I2C
    // -------------------------------------------------

    Wire.begin(SDA_PIN, SCL_PIN);


    // -------------------------------------------------
    // MAX30102
    // -------------------------------------------------

    if (!sensor.begin(Wire, I2C_SPEED_FAST))
    {
        Serial.println("MAX30102 NOT FOUND!");

        while (1)
        {
            delay(1000);
        }
    }

    Serial.println("MAX30102 detected.");


    // -------------------------------------------------
    // MAX30102 CONFIGURATION
    // -------------------------------------------------

    byte ledBrightness = 60;
    byte sampleAverage = 4;
    byte ledMode = 2;      // RED + IR
    int sampleRate = 100;  // 100 Hz
    int pulseWidth = 411;
    int adcRange = 4096;

    sensor.setup(
        ledBrightness,
        sampleAverage,
        ledMode,
        sampleRate,
        pulseWidth,
        adcRange
    );

    sensor.setPulseAmplitudeRed(0x3F);
    sensor.setPulseAmplitudeIR(0x3F);

    Serial.println("MAX30102 configured.");


    // -------------------------------------------------
    // WI-FI
    // -------------------------------------------------

    connectWiFi();


    // -------------------------------------------------
    // MQTT
    // -------------------------------------------------

    mqttClient.setServer(
        MQTT_BROKER,
        MQTT_PORT
    );


    mqttClient.setBufferSize(6000);


    connectMQTT();


    Serial.println();
    Serial.println("======================================");
    Serial.println(" SYSTEM READY");
    Serial.println("======================================");
}


// =====================================================
// LOOP
// =====================================================

void loop()
{
    // -------------------------------------------------
    // Maintain Wi-Fi
    // -------------------------------------------------

    if (WiFi.status() != WL_CONNECTED)
    {
        connectWiFi();
    }


    // -------------------------------------------------
    // Maintain MQTT
    // -------------------------------------------------

    if (!mqttClient.connected())
    {
        connectMQTT();
    }

    mqttClient.loop();


    // -------------------------------------------------
    // Collect one second of PPG
    // -------------------------------------------------

    Serial.println();
    Serial.println("Collecting 1 second of PPG data...");

    for (int i = 0; i < BUFFER_SIZE; i++)
    {
        while (!sensor.available())
        {
            sensor.check();
        }

        redBuffer[i] = sensor.getRed();
        irBuffer[i] = sensor.getIR();

        sensor.nextSample();
    }


    // -------------------------------------------------
    // Calculate HR + SpO2 locally
    // -------------------------------------------------

    Serial.println("Calculating HR and SpO2...");

    maxim_heart_rate_and_oxygen_saturation(
        irBuffer,
        BUFFER_SIZE,
        redBuffer,
        &spo2,
        &validSpO2,
        &heartRate,
        &validHeartRate
    );


    Serial.println("--------------------------------------");

    if (validHeartRate)
    {
        Serial.print("Heart Rate: ");
        Serial.print(heartRate);
        Serial.println(" BPM");
    }
    else
    {
        Serial.println("Heart Rate: INVALID");
    }


    if (validSpO2)
    {
        Serial.print("SpO2: ");
        Serial.print(spo2);
        Serial.println(" %");
    }
    else
    {
        Serial.println("SpO2: INVALID");
    }

    Serial.println("--------------------------------------");


    // -------------------------------------------------
    // SEND RAW PPG TO RASPBERRY PI
    // -------------------------------------------------

    publishPPG();


    delay(100);
}
