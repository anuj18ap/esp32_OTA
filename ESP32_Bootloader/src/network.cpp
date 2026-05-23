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
    publishLog("[WiFi] Connecting to %s", WIFI_SSID);

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
    publishLog("[WiFi] Connected. IP: %s", WiFi.localIP().toString().c_str());
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
    publishLog("[MQTT] Listening on MAC: %s", deviceID.c_str());
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
        publishLog("[MQTT] Connecting...");
        // Builds a unique MQTT client ID for this device.
        String clientId = "ESP32-" + deviceID;

        // Connects to the broker and restores subscriptions on success.
        if (mqttClient.connect(clientId.c_str()))
        {
            publishLog("[MQTT] Connected.");
            publishLog("[MQTT] Client ID: %s", clientId.c_str());
            publishDeviceInfo();
            // Subscribes to the OTA handshake, metadata, chunk, and end topics.
            mqttClient.subscribe(topicOTACheck.c_str(), 1);
            publishLog("[MQTT] Subscribed OTA check topic.");
            mqttClient.subscribe(topicOTABegin.c_str(), 1);
            publishLog("[MQTT] Subscribed OTA begin topic.");
            // Uses QoS 0 for OTA chunks because the custom ACK/retry handles reliability faster.
            mqttClient.subscribe(topicOTAChunk.c_str(), 0);
            publishLog("[MQTT] Subscribed OTA chunk topic with QoS 0.");
            mqttClient.subscribe(topicOTAEnd.c_str(), 1);
            publishLog("[MQTT] Subscribed OTA end topic.");
            // Subscribes to home automation command topics after MQTT reconnects.
            subscribeHomeAutomationTopics();
            // Publishes retained relay and RGB states for MQTT dashboard sync.
            publishHomeAutomationState();
            publishLog("[MQTT] Subscribed.");
        }
        else
        {
            // Waits before retrying if the broker connection fails.
            publishLog("[MQTT] Failed (rc=%d). Retrying...",
                       mqttClient.state());
            delay(MQTT_RETRY_DELAY);
        }
    }
}
