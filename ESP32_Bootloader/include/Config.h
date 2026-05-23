#pragma once

#define FW_VERSION "0.3" // Firmware version published to MQTT.
#define LED_PIN 2        // Onboard LED pin used for heartbeat blinking.

#define MQTT_BUFFER_SIZE 8192   // Larger MQTT packet buffer for faster OTA chunks.
#define WIFI_RETRY_DELAY 500    // Delay between Wi-Fi retry attempts.
#define MQTT_RETRY_DELAY 2000   // Delay between MQTT retry attempts.
#define LED_BLINK_INTERVAL 100 // LED heartbeat interval in milliseconds.

extern const char* WIFI_SSID;     // Wi-Fi network name declared in main.cpp.
extern const char* WIFI_PASSWORD; // Wi-Fi password declared in main.cpp.
extern const char* MQTT_BROKER;   // MQTT broker host declared in main.cpp.
extern const int MQTT_PORT;       // MQTT broker port declared in main.cpp.
