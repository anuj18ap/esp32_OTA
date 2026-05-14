#include <HTTPClient.h>

#include "Config.h"
#include "FirmwareApp.h"
#include "FirmwareState.h"

/***********************************************************
brief       Connects the ESP32 to the configured Wi-Fi network
            and waits until the station connection is active.
arguments   None
return type void
************************************************************/
void connectWiFi()
{
    // Skips reconnection if Wi-Fi is already connected.
    if (WiFi.status() == WL_CONNECTED) return;

    // Prints the SSID being used for the connection attempt.
    Serial.print("[WiFi] Connecting to: ");
    Serial.println(WIFI_SSID);

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
return type void
************************************************************/
void publishDeviceInfo()
{
    // Builds a small JSON payload with the firmware version.
    String payload = "{\"firmware\":\"" + String(FW_VERSION) + "\"}";
    // Publishes the payload as a retained message.
    mqttClient.publish(topicInfo.c_str(), payload.c_str(), true);
    Serial.println("[MQTT] Published device info: " + payload);
}

/***********************************************************
brief       Processes incoming MQTT messages for OTA readiness
            checks and OTA update commands.
arguments   topic   - MQTT topic name received from the broker
            payload - Raw MQTT message bytes
            length  - Number of bytes in the payload
return type void
************************************************************/
void mqttCallback(char* topic, byte* payload, unsigned int length)
{
    // Converts the raw MQTT payload into an Arduino String.
    String message;
    message.reserve(length);
    for (unsigned int i = 0; i < length; i++)
        message += (char)payload[i];

    // Converts the received topic into an Arduino String for comparison.
    String currentTopic(topic);

    Serial.println("[MQTT] RX  topic: " + currentTopic);
    Serial.println("[MQTT] RX  payload: " + message);

    // Replies to the readiness check so the sender knows the device is online.
    if (currentTopic == topicOTACheck)
    {
        if (message == "ARE_YOU_READY")
        {
            mqttClient.publish(topicOTAStatus.c_str(), "READY");
            Serial.println("[MQTT] Replied READY");
        }
        return;
    }

    // Starts OTA only when an update URL is received.
    if (currentTopic == topicOTA)
    {
        if (message.length() > 0)
            performOTA(message);
        return;
    }
}

/***********************************************************
brief       Connects or reconnects to the MQTT broker and
            restores all required topic subscriptions.
arguments   None
return type void
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
            mqttClient.subscribe(topicOTA.c_str());
            mqttClient.subscribe(topicOTACheck.c_str());
            Serial.println("[MQTT] Subscribed to OTA topics.");
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
