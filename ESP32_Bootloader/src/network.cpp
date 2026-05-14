#include "Config.h"
#include "FirmwareApp.h"
#include "FirmwareState.h"

/***********************************************************
brief       Connects the ESP32 to the configured Wi-Fi network
            and waits until the station connection is active.
arguments   None
return-type void
************************************************************/
void connectWiFi()
{
    // Skips reconnection if Wi-Fi is already connected.
    if (WiFi.status() == WL_CONNECTED) return;

    // Prints the SSID being used for the connection attempt.
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);

    // Forces station mode and starts the Wi-Fi connection.
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // Waits here until the ESP32 gets connected to the router.
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(WIFI_RETRY_DELAY);
        Serial.print(".");
    }

    // Prints the assigned local IP address after connection.
    Serial.println();
    Serial.print("[WiFi] Connected. IP: ");
    Serial.println(WiFi.localIP());
}

/***********************************************************
brief       Publishes the current firmware version to the
            retained MQTT device information topic.
arguments   None
return-type void
************************************************************/
void publishDeviceInfo()
{
    // Builds a retained JSON payload with the firmware version and device ID.
    String payload = "{\"firmware\":\"" + String(FW_VERSION)
                   + "\",\"id\":\"" + deviceID + "\"}";
    // Publishes the payload as a retained message.
    mqttClient.publish(topicInfo.c_str(), payload.c_str(), true);
    Serial.println("[MQTT] Listening on MAC: " + deviceID);
}

/***********************************************************
brief       Connects or reconnects to the MQTT broker and
            restores all required topic subscriptions.
arguments   None
return-type void
************************************************************/
void connectMQTT()
{
    // Keeps trying until the MQTT connection succeeds.
    while (!mqttClient.connected())
    {
        Serial.print("[MQTT] Connecting...");
        // Builds a unique MQTT client ID for this device.
        String clientId = "ESP32-" + deviceID;

        // Connects to the broker and restores subscriptions on success.
        if (mqttClient.connect(clientId.c_str()))
        {
            Serial.println(" connected.");
            publishDeviceInfo();
            // Subscribes to the OTA handshake, metadata, chunk, and end topics.
            mqttClient.subscribe(topicOTACheck.c_str(), 1);
            mqttClient.subscribe(topicOTABegin.c_str(), 1);
            mqttClient.subscribe(topicOTAChunk.c_str(), 1);
            mqttClient.subscribe(topicOTAEnd.c_str(), 1);
            Serial.println("[MQTT] Subscribed.");
        }
        else
        {
            // Waits before retrying if the broker connection fails.
            Serial.printf(" failed (rc=%d). Retrying...\n",
                          mqttClient.state());
            delay(MQTT_RETRY_DELAY);
        }
    }
}
