#include "FirmwareApp.h"
#include "FirmwareState.h"

/***********************************************************
brief       Builds a unique device ID from the ESP32 eFuse MAC
            address so every board gets its own MQTT topic set.
arguments   None
return-type String
************************************************************/
String getDeviceID()
{
    // Reads the 48-bit unique MAC-based chip identifier.
    uint64_t chipid = ESP.getEfuseMac();
    // Holds the formatted hexadecimal ID string.
    char id[13];
    // Formats the chip ID into a compact uppercase string.
    snprintf(id, sizeof(id), "%04X%08X",
             (uint16_t)(chipid >> 32),
             (uint32_t)chipid);
    // Returns the formatted ID as an Arduino String.
    return String(id);
}

/***********************************************************
brief       Generates all MQTT topic names using the current
            device ID as the topic prefix.
arguments   None
return-type void
************************************************************/
void initTopics()
{
    // Topic for retained device firmware information.
    topicInfo = deviceID + "/info";

    // Topic for answering OTA readiness checks.
    topicOTACheck = deviceID + "/ota_check";

    // Topic for receiving OTA metadata before chunk transfer starts.
    topicOTABegin = deviceID + "/ota/begin";

    // Topic for receiving binary firmware chunks over MQTT.
    topicOTAChunk = deviceID + "/ota/chunk";

    // Topic for receiving the OTA transfer completion signal.
    topicOTAEnd = deviceID + "/ota/end";

    // Topic for acknowledging each successfully written chunk.
    topicOTAAck = deviceID + "/ota/ack";

    // Topic for publishing OTA status messages.
    topicOTAStatus = deviceID + "/ota_status";
}
