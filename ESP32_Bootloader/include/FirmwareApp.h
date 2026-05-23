#pragma once

#include <Arduino.h>

// Returns a MAC-derived ID string for MQTT topic names.
String getDeviceID();

// Builds all MQTT topics from the current device ID.
void initTopics();

// Loads device name and Wi-Fi credentials from NVS.
void loadDeviceConfig();

// Connects the ESP32 to the configured Wi-Fi network.
void connectWiFi();

// Publishes retained device firmware information.
void publishDeviceInfo();

// Republishes device info periodically for app rediscovery.
void publishPeriodicDeviceInfo();

// Routes MQTT messages for app device name and Wi-Fi management.
bool handleDeviceConfigMessage(const String& topic, byte* payload, unsigned int length);

// Publishes a formatted debug log to <deviceID>/log and mirrors it to Serial.
void publishLog(const char* format, ...);

// Publishes OTA status text to the status topic.
void publishStatus(const char* status);

// Publishes an acknowledgement for one OTA chunk.
void publishAck(uint32_t chunkIndex);

// Handles OTA metadata and prepares the update partition.
void handleOtaBegin(const uint8_t* payload, unsigned int len);

// Handles one OTA firmware chunk and writes it to flash.
void handleOtaChunk(const uint8_t* payload, unsigned int len);

// Handles OTA completion, verification, and reboot.
void handleOtaEnd();

// Routes MQTT messages to the matching OTA handlers.
void mqttCallback(char* topic, byte* payload, unsigned int length);

// Initializes relay, button, and RGB GPIO hardware.
void initializeHomeAutomation();

// Subscribes to relay and RGB MQTT command topics.
void subscribeHomeAutomationTopics();

// Publishes retained relay and RGB state after MQTT connects.
void publishHomeAutomationState();

// Routes relay and RGB MQTT messages to automation handlers.
bool handleHomeAutomationMessage(const String& topic, byte* payload, unsigned int length);

// Handles button events and debounce processing.
void processHomeAutomation();

// Connects to MQTT and subscribes to OTA topics.
void connectMQTT();

// Updates the heartbeat LED when its interval expires.
void handleLedBlink();
