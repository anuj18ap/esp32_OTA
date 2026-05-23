#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>

#include "Config.h"
#include "FirmwareApp.h"
#include "FirmwareState.h"

static const char* CONFIG_NAMESPACE = "appcfg";
static const char* KEY_DEVICE_NAME = "name";
static const char* KEY_WIFI_SSID = "ssid";
static const char* KEY_WIFI_PASS = "password";

/***********************************************************
brief       Loads the app-visible device name and Wi-Fi
            credentials from NVS, falling back to firmware defaults
            when credentials have not been saved yet.
arguments   None
return-type void
************************************************************/
void loadDeviceConfig()
{
    Preferences preferences;
    preferences.begin(CONFIG_NAMESPACE, false);

    deviceName = preferences.getString(KEY_DEVICE_NAME, "");
    if (preferences.isKey(KEY_WIFI_SSID))
    {
        currentWiFiSsid = preferences.getString(KEY_WIFI_SSID, WIFI_SSID);
        currentWiFiPass = preferences.getString(KEY_WIFI_PASS, WIFI_PASSWORD);
    }
    else
    {
        currentWiFiSsid = WIFI_SSID;
        currentWiFiPass = WIFI_PASSWORD;
        preferences.putString(KEY_WIFI_SSID, currentWiFiSsid);
        preferences.putString(KEY_WIFI_PASS, currentWiFiPass);
    }

    preferences.end();

    publishLog("[NVS] Device name: %s", deviceName.length() ? deviceName.c_str() : "(empty)");
    publishLog("[NVS] WiFi SSID: %s", currentWiFiSsid.c_str());
}

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
    publishLog("[WiFi] Connecting to %s", currentWiFiSsid.c_str());

    // Forces station mode and starts the Wi-Fi connection.
    WiFi.mode(WIFI_STA);
    WiFi.begin(currentWiFiSsid.c_str(), currentWiFiPass.c_str());

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
    JsonDocument document;
    document["id"] = deviceID;
    document["firmware"] = FW_VERSION;
    document["name"] = deviceName;

    String payload;
    serializeJson(document, payload);

    // Publishes the payload as a retained message.
    mqttClient.publish(topicInfo.c_str(), payload.c_str(), true);
    publishLog("[MQTT] Listening on MAC: %s", deviceID.c_str());
}

/***********************************************************
brief       Republishes device info every configured interval so
            the app can rediscover devices after it restarts.
arguments   None
return-type void
************************************************************/
void publishPeriodicDeviceInfo()
{
    if (!mqttClient.connected()) return;

    unsigned long now = millis();
    if ((now - previousInfoMillis) < INFO_PUBLISH_INTERVAL) return;

    publishDeviceInfo();
    previousInfoMillis = now;
}

/***********************************************************
brief       Publishes the Wi-Fi credentials currently loaded by
            the firmware from NVS/defaults.
arguments   None
return-type void
************************************************************/
static void publishWiFiConfig()
{
    JsonDocument document;
    document["ssid"] = currentWiFiSsid;
    document["password"] = currentWiFiPass;

    String payload;
    serializeJson(document, payload);

    mqttClient.publish(topicWifiConfig.c_str(), payload.c_str(), false);
    mqttClient.loop();
    publishLog("[WiFi] Config sent for SSID: %s", currentWiFiSsid.c_str());
}

/***********************************************************
brief       Routes app identity and Wi-Fi management MQTT messages.
arguments   topic   - MQTT topic received from the broker
            payload - Raw MQTT payload bytes
            length  - Number of bytes in the payload
return-type bool - true when the topic was handled
************************************************************/
bool handleDeviceConfigMessage(const String& topic, byte* payload, unsigned int length)
{
    if (topic == topicSetName)
    {
        String newName;
        newName.reserve(length);
        for (unsigned int i = 0; i < length; i++)
            newName += (char)payload[i];
        newName.trim();

        Preferences preferences;
        preferences.begin(CONFIG_NAMESPACE, false);
        preferences.putString(KEY_DEVICE_NAME, newName);
        preferences.end();

        deviceName = newName;
        publishLog("[NVS] Device name saved: %s", deviceName.c_str());
        publishDeviceInfo();
        return true;
    }

    if (topic == topicWifiRequest)
    {
        String request;
        request.reserve(length);
        for (unsigned int i = 0; i < length; i++)
            request += (char)payload[i];
        request.trim();

        if (request == "GET_CONFIG")
            publishWiFiConfig();
        else
            publishLog("[WiFi] Ignored request payload: %s", request.c_str());
        return true;
    }

    if (topic == topicWifiSet)
    {
        JsonDocument document;
        DeserializationError error = deserializeJson(document, payload, length);
        if (error)
        {
            publishLog("[WiFi] Set JSON error: %s", error.c_str());
            return true;
        }

        const char* ssid = document["ssid"] | "";
        const char* password = document["password"] | "";
        if (strlen(ssid) == 0)
        {
            publishLog("[WiFi] Set ignored - SSID is empty.");
            return true;
        }

        Preferences preferences;
        preferences.begin(CONFIG_NAMESPACE, false);
        preferences.putString(KEY_WIFI_SSID, ssid);
        preferences.putString(KEY_WIFI_PASS, password);
        preferences.end();

        currentWiFiSsid = ssid;
        currentWiFiPass = password;
        publishLog("INFO: WiFi updated, rebooting...");
        mqttClient.loop();
        delay(500);
        ESP.restart();
        return true;
    }

    return false;
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
            previousInfoMillis = millis();
            mqttClient.subscribe(topicSetName.c_str(), 1);
            publishLog("[MQTT] Subscribed device name topic.");
            mqttClient.subscribe(topicWifiRequest.c_str(), 1);
            publishLog("[MQTT] Subscribed WiFi request topic.");
            mqttClient.subscribe(topicWifiSet.c_str(), 1);
            publishLog("[MQTT] Subscribed WiFi set topic.");
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
