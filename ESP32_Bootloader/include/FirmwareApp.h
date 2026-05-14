#pragma once

#include <Arduino.h>

// Returns a MAC-derived ID string for MQTT topic names.
String getDeviceID();

// Builds all MQTT topics from the current device ID.
void initTopics();

// Connects the ESP32 to the configured Wi-Fi network.
void connectWiFi();

// Publishes retained device firmware information.
void publishDeviceInfo();

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

// Connects to MQTT and subscribes to OTA topics.
void connectMQTT();

// Updates the heartbeat LED when its interval expires.
void handleLedBlink();
