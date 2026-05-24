#include <ArduinoJson.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <string.h>

#include "Config.h"
#include "FirmwareApp.h"
#include "FirmwareState.h"

static const char* CONFIG_NAMESPACE = "appcfg";
static const char* KEY_DEVICE_NAME = "name";
static const char* KEY_WIFI_SSID = "ssid";
static const char* KEY_WIFI_PASS = "password";
static const byte DNS_PORT = 53;

static DNSServer provisionDns;
static WebServer provisionServer(80);
static String provisionMessage;

/***********************************************************
brief       Decodes form URL encoded text from setup portal posts.
arguments   value - Encoded form value
return-type String - Decoded text
************************************************************/
static String urlDecode(const String& value)
{
    String decoded;
    decoded.reserve(value.length());

    for (size_t i = 0; i < value.length(); i++)
    {
        char c = value[i];
        if (c == '+')
        {
            decoded += ' ';
        }
        else if (c == '%' && i + 2 < value.length())
        {
            char hex[3] = {value[i + 1], value[i + 2], '\0'};
            decoded += (char)strtol(hex, nullptr, 16);
            i += 2;
        }
        else
        {
            decoded += c;
        }
    }

    return decoded;
}

/***********************************************************
brief       Escapes text before inserting it into setup portal HTML.
arguments   value - Raw text
return-type String - HTML-safe text
************************************************************/
static String htmlEscape(const String& value)
{
    String escaped;
    escaped.reserve(value.length());

    for (size_t i = 0; i < value.length(); i++)
    {
        char c = value[i];
        if (c == '&') escaped += F("&amp;");
        else if (c == '<') escaped += F("&lt;");
        else if (c == '>') escaped += F("&gt;");
        else if (c == '"') escaped += F("&quot;");
        else escaped += c;
    }

    return escaped;
}

/***********************************************************
brief       Saves the current identity and Wi-Fi credentials to NVS.
arguments   ssid     - Wi-Fi SSID
            password - Wi-Fi password
return-type void
************************************************************/
static void saveProvisionedConfig(const String& ssid, const String& password)
{
    Preferences preferences;
    preferences.begin(CONFIG_NAMESPACE, false);
    preferences.putString(KEY_DEVICE_NAME, deviceName);
    preferences.putString(KEY_WIFI_SSID, ssid);
    preferences.putString(KEY_WIFI_PASS, password);
    preferences.end();

    currentWiFiSsid = ssid;
    currentWiFiPass = password;
}

/***********************************************************
brief       Builds the device-name setup page.
arguments   None
return-type String - Complete HTML page
************************************************************/
static String buildNamePage()
{
    String html = F("<!DOCTYPE html><html><head><title>ESP32 Device Name</title>"
                    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                    "<style>body{margin:0;font-family:Arial;background:linear-gradient(135deg,#4e73df,#1cc88a);"
                    "display:flex;justify-content:center;align-items:center;height:100vh}.card{background:white;"
                    "padding:25px;border-radius:15px;box-shadow:0 10px 25px rgba(0,0,0,0.2);width:340px;"
                    "text-align:center}input{width:100%;padding:10px;margin:8px 0;border-radius:8px;"
                    "border:1px solid #ccc;box-sizing:border-box}button{width:100%;padding:10px;background:#4e73df;"
                    "border:none;border-radius:8px;color:white}</style></head><body><div class=\"card\">"
                    "<h2>Enter Device Name</h2><form action=\"/setname\" method=\"POST\">"
                    "<input type=\"text\" name=\"dev\" placeholder=\"Device Name\" required value=\"");
    html += htmlEscape(deviceName);
    html += F("\"><button type=\"submit\">Continue</button></form></div></body></html>");
    return html;
}

/***********************************************************
brief       Builds Wi-Fi option tags from nearby scan results.
arguments   None
return-type String - HTML option tags
************************************************************/
static String buildWifiOptions()
{
    int networks = WiFi.scanNetworks();
    String options;

    if (networks <= 0)
    {
        options = F("<option value=\"\">No WiFi networks found</option>");
    }
    else
    {
        for (int i = 0; i < networks; i++)
        {
            String ssid = WiFi.SSID(i);
            if (!ssid.length()) continue;

            options += F("<option value=\"");
            options += htmlEscape(ssid);
            options += F("\">");
            options += htmlEscape(ssid);
            options += F(" (");
            options += WiFi.RSSI(i);
            options += F(" dBm)</option>");
        }
    }

    WiFi.scanDelete();
    return options;
}

/***********************************************************
brief       Builds the Wi-Fi setup page with nearby SSIDs.
arguments   message - Status message to show under the form
            success - true when the status is positive
return-type String - Complete HTML page
************************************************************/
static String buildWifiPage(const String& message, bool success)
{
    String html = F("<!DOCTYPE html><html><head><title>ESP32 WiFi Setup</title>"
                    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                    "<style>body{margin:0;font-family:Arial;background:linear-gradient(135deg,#4e73df,#1cc88a);"
                    "display:flex;justify-content:center;align-items:center;min-height:100vh}.card{background:white;"
                    "padding:25px;border-radius:15px;box-shadow:0 10px 25px rgba(0,0,0,0.2);width:340px;"
                    "text-align:center}select,input{width:100%;padding:10px;margin:8px 0;border-radius:8px;"
                    "border:1px solid #ccc;box-sizing:border-box}.password-wrapper{position:relative}.eye-btn{position:absolute;"
                    "right:12px;top:50%;transform:translateY(-50%);cursor:pointer;background:none;border:none}.eye-btn svg{width:22px;"
                    "height:22px;stroke:#555;fill:none}button[type='submit']{width:100%;padding:10px;background:#4e73df;"
                    "border:none;border-radius:8px;color:white}.message{margin-top:12px;font-weight:bold}</style>"
                    "<script>function togglePassword(){var passField=document.getElementById('pass');var eyeOpen=document.getElementById('eye-open');"
                    "var eyeClosed=document.getElementById('eye-closed');if(passField.type==='password'){passField.type='text';"
                    "eyeOpen.style.display='none';eyeClosed.style.display='inline';}else{passField.type='password';"
                    "eyeOpen.style.display='inline';eyeClosed.style.display='none';}}</script></head><body><div class=\"card\">"
                    "<h2>Select WiFi Network</h2><form action=\"/connect\" method=\"POST\"><select name=\"ssid\" required>");
    html += buildWifiOptions();
    html += F("</select><div class=\"password-wrapper\"><input type=\"password\" id=\"pass\" name=\"pass\" "
              "placeholder=\"Enter Password\" required><button type=\"button\" class=\"eye-btn\" onclick=\"togglePassword()\">"
              "<svg id=\"eye-open\" viewBox=\"0 0 24 24\"><path d=\"M1 12s4-7.5 11-7.5S23 12 23 12s-4 7.5-11 7.5S1 12 1 12z\" "
              "stroke-width=\"2\"/><circle cx=\"12\" cy=\"12\" r=\"3\" stroke-width=\"2\"/></svg>"
              "<svg id=\"eye-closed\" viewBox=\"0 0 24 24\" style=\"display:none;\"><path d=\"M1 12s4-7.5 11-7.5S23 12 23 12s-4 "
              "7.5-11 7.5S1 12 1 12z\" stroke-width=\"2\"/><path d=\"M3 3l18 18\" stroke-width=\"2\" "
              "stroke-linecap=\"round\"/></svg></button></div><button type=\"submit\">Connect</button></form>");

    if (message.length())
    {
        html += F("<div class=\"message\" style=\"color:");
        html += success ? F("green") : F("red");
        html += F(";\">");
        html += htmlEscape(message);
        html += F("</div>");
    }

    html += F("</div></body></html>");
    return html;
}

/***********************************************************
brief       Tries to connect with credentials submitted through
            the local setup portal.
arguments   ssid     - Wi-Fi SSID
            password - Wi-Fi password
return-type bool - true when the ESP32 connected successfully
************************************************************/
static bool testWiFiCredentials(const String& ssid, const String& password)
{
    WiFi.begin(ssid.c_str(), password.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - start) < WIFI_CONNECT_TIMEOUT)
    {
        delay(WIFI_RETRY_DELAY);
    }

    return WiFi.status() == WL_CONNECTED;
}

/***********************************************************
brief       Starts the local AP and blocking setup portal.
arguments   reason - Message printed to Serial before portal starts
return-type void
************************************************************/
static void startProvisioningPortal(const char* reason)
{
    publishLog("[SETUP] %s", reason);

    String apName = String(PROVISION_AP_PREFIX) + deviceID.substring(deviceID.length() - 6);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apName.c_str(), PROVISION_AP_PASSWORD);

    publishLog("[SETUP] Hotspot SSID: %s", apName.c_str());
    publishLog("[SETUP] Open http://%s", WiFi.softAPIP().toString().c_str());

    provisionDns.start(DNS_PORT, "*", WiFi.softAPIP());

    auto redirectToPortal = []() {
        provisionServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
        provisionServer.send(302, "text/plain", "");
    };

    provisionServer.on("/", HTTP_GET, []() {
        if (deviceName.length())
            provisionServer.sendHeader("Location", "/wifi", true);
        else
            provisionServer.sendHeader("Location", "/name", true);
        provisionServer.send(302, "text/plain", "");
    });

    provisionServer.on("/generate_204", HTTP_GET, redirectToPortal);
    provisionServer.on("/gen_204", HTTP_GET, redirectToPortal);
    provisionServer.on("/hotspot-detect.html", HTTP_GET, redirectToPortal);
    provisionServer.on("/library/test/success.html", HTTP_GET, redirectToPortal);
    provisionServer.on("/connecttest.txt", HTTP_GET, redirectToPortal);
    provisionServer.on("/ncsi.txt", HTTP_GET, redirectToPortal);
    provisionServer.on("/fwlink", HTTP_GET, redirectToPortal);

    provisionServer.on("/name", HTTP_GET, []() {
        provisionServer.send(200, "text/html", buildNamePage());
    });

    provisionServer.on("/setname", HTTP_POST, []() {
        deviceName = urlDecode(provisionServer.arg("dev"));
        deviceName.trim();

        Preferences preferences;
        preferences.begin(CONFIG_NAMESPACE, false);
        preferences.putString(KEY_DEVICE_NAME, deviceName);
        preferences.end();

        provisionServer.sendHeader("Location", "/wifi", true);
        provisionServer.send(302, "text/plain", "");
    });

    provisionServer.on("/wifi", HTTP_GET, []() {
        provisionServer.send(200, "text/html", buildWifiPage(provisionMessage, false));
    });

    provisionServer.on("/connect", HTTP_POST, []() {
        String ssid = urlDecode(provisionServer.arg("ssid"));
        String password = urlDecode(provisionServer.arg("pass"));
        ssid.trim();

        if (!ssid.length())
        {
            provisionMessage = "Please select a WiFi network.";
            provisionServer.send(200, "text/html", buildWifiPage(provisionMessage, false));
            return;
        }

        publishLog("[SETUP] Testing WiFi SSID: %s", ssid.c_str());
        if (testWiFiCredentials(ssid, password))
        {
            saveProvisionedConfig(ssid, password);
            provisionServer.send(200, "text/html",
                                 buildWifiPage("Connected. Saving and rebooting...", true));
            publishLog("[SETUP] WiFi connected. Saving and rebooting.");
            delay(1500);
            ESP.restart();
        }

        WiFi.disconnect(false);
        provisionMessage = "Wrong password or connection failed. Try again.";
        provisionServer.send(200, "text/html", buildWifiPage(provisionMessage, false));
    });

    provisionServer.onNotFound([]() {
        provisionServer.sendHeader("Location", "/", true);
        provisionServer.send(302, "text/plain", "");
    });

    provisionServer.begin();
    while (true)
    {
        provisionDns.processNextRequest();
        provisionServer.handleClient();
        delay(2);
    }
}

/***********************************************************
brief       Loads the app-visible device name and Wi-Fi
            credentials from NVS.
arguments   None
return-type void
************************************************************/
void loadDeviceConfig()
{
    Preferences preferences;
    preferences.begin(CONFIG_NAMESPACE, false);

    deviceName = preferences.getString(KEY_DEVICE_NAME, "");
    currentWiFiSsid = preferences.getString(KEY_WIFI_SSID, "");
    currentWiFiPass = preferences.getString(KEY_WIFI_PASS, "");

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

    if (!currentWiFiSsid.length())
    {
        startProvisioningPortal("No saved WiFi credentials found.");
    }

    // Prints the SSID being used for the connection attempt.
    publishLog("[WiFi] Connecting to %s", currentWiFiSsid.c_str());

    // Forces station mode and starts the Wi-Fi connection.
    WiFi.mode(WIFI_STA);
    WiFi.begin(currentWiFiSsid.c_str(), currentWiFiPass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - start) < WIFI_CONNECT_TIMEOUT)
    {
        delay(WIFI_RETRY_DELAY);
        Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        WiFi.disconnect(false);
        startProvisioningPortal("Saved WiFi credentials failed.");
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

    if (topic == topicResetConfig)
    {
        String request;
        request.reserve(length);
        for (unsigned int i = 0; i < length; i++)
            request += (char)payload[i];
        request.trim();

        if (request != "RESET_CONFIG")
        {
            publishLog("[NVS] Reset config ignored - bad payload: %s", request.c_str());
            return true;
        }

        Preferences preferences;
        preferences.begin(CONFIG_NAMESPACE, false);
        preferences.clear();
        preferences.end();

        deviceName = "";
        currentWiFiSsid = "";
        currentWiFiPass = "";

        publishLog("INFO: Configuration reset, rebooting to setup portal...");
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
            //publishLog("[MQTT] Subscribed device name topic.");
            mqttClient.subscribe(topicWifiRequest.c_str(), 1);
            //publishLog("[MQTT] Subscribed WiFi request topic.");
            mqttClient.subscribe(topicWifiSet.c_str(), 1);
            //publishLog("[MQTT] Subscribed WiFi set topic.");
            mqttClient.subscribe(topicResetConfig.c_str(), 1);
            //publishLog("[MQTT] Subscribed reset config topic.");
            // Subscribes to the OTA handshake, metadata, chunk, and end topics.
            mqttClient.subscribe(topicOTACheck.c_str(), 1);
            //publishLog("[MQTT] Subscribed OTA check topic.");
            mqttClient.subscribe(topicOTABegin.c_str(), 1);
            //publishLog("[MQTT] Subscribed OTA begin topic.");
            // Uses QoS 0 for OTA chunks because the custom ACK/retry handles reliability faster.
            mqttClient.subscribe(topicOTAChunk.c_str(), 0);
            //publishLog("[MQTT] Subscribed OTA chunk topic with QoS 0.");
            mqttClient.subscribe(topicOTAEnd.c_str(), 1);
            //publishLog("[MQTT] Subscribed OTA end topic.");
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
