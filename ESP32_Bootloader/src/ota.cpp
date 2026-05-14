#include <ArduinoJson.h>
#include <Update.h>

#include "FirmwareApp.h"
#include "FirmwareState.h"

/***********************************************************
brief       Publishes OTA status immediately and flushes the MQTT
            packet so the sender receives the update without delay.
arguments   status - Status string to publish on the OTA status topic
return-type void
************************************************************/
void publishStatus(const char* status)
{
    mqttClient.publish(topicOTAStatus.c_str(), status, false);
    mqttClient.loop();
    Serial.printf("[MQTT] Status -> %s\n", status);
}

/***********************************************************
brief       Publishes a chunk acknowledgement immediately so
            the sender can continue the OTA transfer.
arguments   chunkIndex - Index of the chunk being acknowledged
return-type void
************************************************************/
void publishAck(uint32_t chunkIndex)
{
    char buffer[12];
    snprintf(buffer, sizeof(buffer), "%u", chunkIndex);
    mqttClient.publish(topicOTAAck.c_str(), buffer, false);
    mqttClient.loop();
}

/***********************************************************
brief       Parses OTA begin metadata and prepares the flash
            update partition for incoming firmware chunks.
arguments   payload - Raw JSON metadata bytes
            len     - Number of metadata bytes received
return-type void
************************************************************/
void handleOtaBegin(const uint8_t* payload, unsigned int len)
{
    if (otaState != OTA_IDLE)
    {
        Serial.println("[OTA] BEGIN ignored - already receiving.");
        return;
    }

    JsonDocument document;
    DeserializationError error = deserializeJson(document, payload, len);
    if (error)
    {
        Serial.printf("[OTA] JSON error: %s\n", error.c_str());
        publishStatus("FAILED");
        return;
    }

    totalSize      = document["size"].as<uint32_t>();
    expectedChunks = document["chunks"].as<uint32_t>();
    expectedCRC32  = document["crc32"].as<uint32_t>();
    chunksReceived = 0;
    lastChunkIndex = 0xFFFFFFFF;

    // Reset the running CRC accumulator for this transfer.
    //runningCRC32 = 0;
    runningCRC32 = 0xFFFFFFFF;

    Serial.printf("[OTA] BEGIN  size=%u  chunks=%u  crc32=0x%08X\n",
                  totalSize, expectedChunks, expectedCRC32);
    Serial.printf("[OTA] Free sketch space: %u\n", ESP.getFreeSketchSpace());

    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
    {
        Serial.printf("[OTA] Update.begin() error: %s\n", Update.errorString());
        publishStatus("FAILED");
        return;
    }

    otaState = OTA_RECEIVING;
    publishStatus("UPDATING");
    Serial.println("[OTA] Ready to receive chunks.");
}

/***********************************************************
brief       Writes one OTA chunk to flash, accumulates the
            running CRC32, skips duplicates, and ACKs each write.
arguments   payload - Chunk packet bytes with 4-byte index header
            len     - Total packet length in bytes
return-type void
************************************************************/
void handleOtaChunk(const uint8_t* payload, unsigned int len)
{
    if (otaState != OTA_RECEIVING)
        return;

    if (len < 5)
    {
        Serial.println("[OTA] Chunk too short.");
        return;
    }

    // Extract big-endian chunk index from the first 4 bytes.
    uint32_t chunkIndex = ((uint32_t)payload[0] << 24) |
                          ((uint32_t)payload[1] << 16) |
                          ((uint32_t)payload[2] <<  8) |
                          ((uint32_t)payload[3]);

    // Re-ACK duplicates without re-writing them.
    if (chunkIndex == lastChunkIndex)
    {
        Serial.printf("[OTA] Duplicate chunk %u - re-ACKing.\n", chunkIndex);
        publishAck(chunkIndex);
        return;
    }

    const uint8_t* chunkData = payload + 4;
    unsigned int   chunkLen  = len - 4;

    // Accumulate CRC32 over raw firmware bytes as they arrive.
    // crc32_le is the ESP32 ROM CRC; use our software fallback instead
    // so it matches Python's zlib.crc32.
    runningCRC32 = crc32_update(runningCRC32, chunkData, chunkLen);

    size_t written = Update.write(const_cast<uint8_t*>(chunkData), chunkLen);
    if (written != chunkLen)
    {
        Serial.printf("[OTA] Write error chunk %u: wrote %u of %u\n",
                      chunkIndex, written, chunkLen);
        Update.abort();
        otaState = OTA_IDLE;
        publishStatus("FAILED");
        return;
    }

    lastChunkIndex = chunkIndex;
    chunksReceived++;
    publishAck(chunkIndex);

    if (chunkIndex % 20 == 0 || chunkIndex == expectedChunks - 1)
    {
        uint8_t pct = (uint8_t)((chunksReceived * 100UL) / expectedChunks);
        Serial.printf("[OTA] Chunk %u/%u (%u%%)\n",
                      chunksReceived, expectedChunks, pct);
    }

    yield();
}

/***********************************************************
brief       Verifies the CRC32 of the received image, finalises
            the flash write, and reboots into new firmware on success.
arguments   None
return-type void
************************************************************/
void handleOtaEnd()
{
    if (otaState != OTA_RECEIVING)
    {
        Serial.println("[OTA] END ignored - not in RECEIVING state.");
        return;
    }

    Serial.printf("[OTA] END - received %u/%u chunks\n",
                  chunksReceived, expectedChunks);

    // ── CRC32 verification ────────────────────────────────────────────────
    // Finalise the CRC (XOR with 0xFFFFFFFF to match zlib convention).
    uint32_t finalCRC = runningCRC32 ^ 0xFFFFFFFF;
    if (finalCRC != expectedCRC32)
    {
        Serial.printf("[OTA] CRC32 MISMATCH  got=0x%08X  expected=0x%08X\n",
                      finalCRC, expectedCRC32);
        Update.abort();
        otaState = OTA_IDLE;
        publishStatus("FAILED");
        return;
    }
    Serial.printf("[OTA] CRC32 OK  (0x%08X)\n", finalCRC);

    // ── Finalise flash ────────────────────────────────────────────────────
    if (!Update.end(true))
    {
        Serial.printf("[OTA] Update.end() error: %s\n", Update.errorString());
        otaState = OTA_IDLE;
        publishStatus("FAILED");
        return;
    }

    Serial.println("[OTA] Flash verified OK - rebooting in 1s...");
    publishStatus("SUCCESS");
    mqttClient.loop();
    delay(1000);
    ESP.restart();
}

/***********************************************************
brief       Routes MQTT OTA topics to the matching OTA handlers.
arguments   topic   - MQTT topic name received from the broker
            payload - Raw MQTT message bytes
            length  - Number of bytes in the payload
return-type void
************************************************************/
void mqttCallback(char* topic, byte* payload, unsigned int length)
{
    String currentTopic(topic);

    if (currentTopic == topicOTACheck)
    {
        Serial.println("[MQTT] CHECK -> READY");
        publishStatus("READY");
        return;
    }

    if (currentTopic == topicOTABegin)
    {
        handleOtaBegin(payload, length);
        return;
    }

    if (currentTopic == topicOTAChunk)
    {
        handleOtaChunk(payload, length);
        return;
    }

    if (currentTopic == topicOTAEnd)
    {
        handleOtaEnd();
        return;
    }
}
