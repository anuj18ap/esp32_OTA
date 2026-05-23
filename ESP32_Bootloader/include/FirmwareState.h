#pragma once

#include <Arduino.h>
#include <WiFi.h>

// ── MUST be defined before PubSubClient is included ──────────────────────────
#define MQTT_MAX_PACKET_SIZE 8192 // PubSubClient packet limit for faster OTA chunks.
#include <PubSubClient.h>

enum OtaState { OTA_IDLE, OTA_RECEIVING }; // OTA receiver state machine values.

extern WiFiClient   espClient;  // Shared TCP client for MQTT.
extern PubSubClient mqttClient; // Shared MQTT client instance.

extern String deviceID;       // MAC-derived unique device ID.
extern String topicInfo;      // MQTT topic for retained device info.
extern String topicOTACheck;  // MQTT topic for OTA readiness checks.
extern String topicOTABegin;  // MQTT topic for OTA begin metadata.
extern String topicOTAChunk;  // MQTT topic for OTA firmware chunks.
extern String topicOTAEnd;    // MQTT topic for OTA completion command.
extern String topicOTAAck;    // MQTT topic for chunk acknowledgements.
extern String topicOTAStatus; // MQTT topic for OTA status messages.
extern String topicLog;       // MQTT topic for ESP32 debug log messages.
extern String topicSetName;     // MQTT topic for saving the app-visible device name.
extern String topicWifiRequest; // MQTT topic for requesting saved Wi-Fi credentials.
extern String topicWifiConfig;  // MQTT topic for publishing saved Wi-Fi credentials.
extern String topicWifiSet;     // MQTT topic for saving new Wi-Fi credentials.

extern String deviceName;      // App-visible device name loaded from NVS.
extern String currentWiFiSsid; // Wi-Fi SSID loaded from NVS or the firmware default.
extern String currentWiFiPass; // Wi-Fi password loaded from NVS or the firmware default.

extern unsigned long previousLedMillis; // Last LED toggle timestamp.
extern bool          ledState;          // Current heartbeat LED state.
extern unsigned long previousInfoMillis; // Last periodic device info publish timestamp.

extern volatile OtaState otaState; // Shared OTA state flag.
extern uint32_t expectedChunks;    // Expected firmware chunk count.
extern uint32_t expectedCRC32;     // Expected firmware CRC32 value.
extern uint32_t totalSize;         // Expected firmware image size.
extern uint32_t chunksReceived;    // Count of chunks written to flash.
extern uint32_t lastChunkIndex;    // Last accepted chunk index.

// Running CRC32 accumulator — updated chunk by chunk, verified at OTA end.
// Initialise to 0xFFFFFFFF in handleOtaBegin(); XOR with 0xFFFFFFFF once in handleOtaEnd().
extern uint32_t runningCRC32;

/***********************************************************
brief       Pure CRC32 accumulator compatible with Python's
            zlib.crc32.
            Initialise runningCRC32 = 0xFFFFFFFF before the
            first chunk (in handleOtaBegin).
            After the last chunk finalise with:
                finalCRC = runningCRC32 ^ 0xFFFFFFFF;
arguments   crc  - Running CRC value (0xFFFFFFFF for first call)
            data - Pointer to data bytes for this call
            len  - Number of bytes to process
return-type uint32_t - Updated running CRC (NOT yet finalised)
************************************************************/
inline uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len)
{
    // No internal XOR — caller seeds with 0xFFFFFFFF and does the
    // final ^ 0xFFFFFFFF exactly once after all chunks are processed.
    while (len--)
    {
        crc ^= *data++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return crc;
}
