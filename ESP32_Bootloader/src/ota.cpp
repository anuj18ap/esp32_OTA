#include <HTTPUpdate.h>

#include "FirmwareApp.h"
#include "FirmwareState.h"

/***********************************************************
brief       Starts an HTTP OTA firmware update using the given
            URL and reports the update status through MQTT.
arguments   url - HTTP URL of the firmware binary to install
return type void
************************************************************/
void performOTA(const String& url)
{
    // Prints the OTA source URL for debugging.
    Serial.println("[OTA] Starting update from: " + url);

    // Notifies listeners that the OTA process has started.
    mqttClient.publish(topicOTAStatus.c_str(), "UPDATING");
    // Flushes pending MQTT traffic before disconnecting.
    mqttClient.loop();
    // Disconnects MQTT so the updater can use the network cleanly.
    mqttClient.disconnect();
    delay(200);

    // Creates a Wi-Fi client dedicated to the update download.
    WiFiClient updateClient;
    // Downloads and applies the firmware update from the provided URL.
    t_httpUpdate_return result = httpUpdate.update(updateClient, url);

    // Handles every possible OTA result and reports it back.
    switch (result)
    {
        case HTTP_UPDATE_OK:
            Serial.println("[OTA] Success - rebooting...");
            break;

        case HTTP_UPDATE_FAILED:
        {
            // Prints the OTA error details for troubleshooting.
            Serial.printf("[OTA] Failed. Error (%d): %s\n",
                          httpUpdate.getLastError(),
                          httpUpdate.getLastErrorString().c_str());
            // Restores Wi-Fi and MQTT before sending the failed status.
            connectWiFi();
            String clientId = "ESP32-" + deviceID;
            mqttClient.connect(clientId.c_str());
            mqttClient.publish(topicOTAStatus.c_str(), "FAILED");
            break;
        }

        case HTTP_UPDATE_NO_UPDATES:
        {
            Serial.println("[OTA] No update available.");
            // Restores Wi-Fi and MQTT before sending the no-update status.
            connectWiFi();
            String clientId = "ESP32-" + deviceID;
            mqttClient.connect(clientId.c_str());
            mqttClient.publish(topicOTAStatus.c_str(), "NO_UPDATE");
            break;
        }
    }
}
