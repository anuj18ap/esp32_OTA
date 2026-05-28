#pragma once

#define FW_VERSION "0.5" // Firmware version published to MQTT.
#define LED_PIN 2        // Onboard LED pin used for heartbeat blinking.

#define MQTT_BUFFER_SIZE 8192   // Larger MQTT packet buffer for faster OTA chunks.
#define WIFI_RETRY_DELAY 500    // Delay between Wi-Fi retry attempts.
#define WIFI_CONNECT_TIMEOUT 20000 // Maximum Wi-Fi connection attempt time in milliseconds.
#define MQTT_RETRY_DELAY 2000   // Delay between MQTT retry attempts.
#define LED_BLINK_INTERVAL 1000 // LED heartbeat interval in milliseconds.
#define INFO_PUBLISH_INTERVAL 45000 // Device info rediscovery interval in milliseconds.

extern const char* PROVISION_AP_PREFIX;   // Setup hotspot SSID prefix declared in main.cpp.
extern const char* PROVISION_AP_PASSWORD; // Setup hotspot password declared in main.cpp.
extern const char* MQTT_BROKER;   // MQTT broker host declared in main.cpp.
extern const int MQTT_PORT;       // MQTT broker port declared in main.cpp.
