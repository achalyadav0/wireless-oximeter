#pragma once

// =============================================================================================
//  Copy this file to secrets.h and edit it.  secrets.h is git-ignored: it holds your Wi-Fi
//  password, so never commit or share it.
// =============================================================================================

// 1) Your Wi-Fi network (the ESP32 only supports 2.4 GHz networks, not 5 GHz).
const char* WIFI_SSID = "your-wifi-network";
const char* WIFI_PASSWORD = "your-wifi-password";

// 2) IP address of the Raspberry Pi that runs the MQTT broker (Mosquitto) and the dashboard.
//    On the Pi, run  hostname -I  and use the first address.
const char* MQTT_BROKER = "192.168.1.100";

// 3) THIS device.  Every oximeter on the network needs its OWN id and its OWN number nn:
//        device 1:  "OXI001"   "oximeter/01/ppg"   "oximeter/01/status"
//        device 2:  "OXI002"   "oximeter/02/ppg"   "oximeter/02/status"      ... and so on.
//    Register each id on the Pi with:  python seed_dev_data.py OXI001 OXI002
const char* DEVICE_ID = "OXI001";
const char* MQTT_PPG_TOPIC = "oximeter/01/ppg";
const char* MQTT_STATUS_TOPIC = "oximeter/01/status";

// ---- Optional settings (defaults are in config.h) -------------------------------------------
// Remove the two slashes in front of a line to use it.
// #define SDA_PIN            8                // MAX30102 wiring. Defaults: ESP32-S3 8 / 9,
// #define SCL_PIN            9                //   classic ESP32 21 / 22.
// #define I2C_CLOCK_HZ       100000UL         // only if the sensor is unreliable at 400 kHz
// #define MQTT_PORT_NUMBER   1883
// #define MQTT_USERNAME      "user"           // only if your broker needs a login
// #define MQTT_PASSWORD      "password"
