#pragma once

#define FW_VERSION "0.1"
#define LED_PIN 2

#define MQTT_BUFFER_SIZE 1024
#define WIFI_RETRY_DELAY 500
#define MQTT_RETRY_DELAY 2000
#define LED_BLINK_INTERVAL 1000

extern const char* WIFI_SSID;
extern const char* WIFI_PASSWORD;
extern const char* MQTT_BROKER;
extern const int MQTT_PORT;
