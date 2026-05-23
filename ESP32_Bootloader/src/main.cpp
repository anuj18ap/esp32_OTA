#include <Arduino.h>

#include "Config.h"
#include "FirmwareApp.h"
#include "FirmwareState.h"

const char* WIFI_SSID     = "AceTech Solution";  // Wi-Fi network name used by the ESP32.
const char* WIFI_PASSWORD = "AceTech@2023";      // Wi-Fi password used by the ESP32.
const char* MQTT_BROKER   = "broker.emqx.io";    // Public MQTT broker used for OTA messages.
const int   MQTT_PORT     = 1883;                // MQTT TCP port for the broker.

WiFiClient   espClient;            // TCP client used by the MQTT client.
PubSubClient mqttClient(espClient); // MQTT client used for OTA communication.

String deviceID;       // MAC-derived unique device ID.
String topicInfo;      // MQTT topic for retained firmware information.
String topicOTACheck;  // MQTT topic for OTA readiness checks.
String topicOTABegin;  // MQTT topic for OTA metadata.
String topicOTAChunk;  // MQTT topic for OTA firmware chunks.
String topicOTAEnd;    // MQTT topic for OTA completion command.
String topicOTAAck;    // MQTT topic for per-chunk acknowledgements.
String topicOTAStatus; // MQTT topic for OTA status messages.
String topicLog;       // MQTT topic for ESP32 debug log messages.
String topicSetName;     // MQTT topic for saving the app-visible device name.
String topicWifiRequest; // MQTT topic for requesting saved Wi-Fi credentials.
String topicWifiConfig;  // MQTT topic for publishing saved Wi-Fi credentials.
String topicWifiSet;     // MQTT topic for saving new Wi-Fi credentials.
String deviceName;       // App-visible device name loaded from NVS.
String currentWiFiSsid;  // Wi-Fi SSID loaded from NVS or the firmware default.
String currentWiFiPass;  // Wi-Fi password loaded from NVS or the firmware default.

unsigned long        previousLedMillis = 0;          // Last LED toggle timestamp.
bool                 ledState          = false;      // Current LED output state.
unsigned long        previousInfoMillis = 0;         // Last periodic device info publish timestamp.
volatile OtaState    otaState          = OTA_IDLE;   // Current OTA receive state.
uint32_t             expectedChunks    = 0;          // Number of chunks expected for the update.
uint32_t             expectedCRC32     = 0;          // CRC32 value sent by the uploader.
uint32_t             totalSize         = 0;          // Total firmware image size in bytes.
uint32_t             chunksReceived    = 0;          // Number of unique chunks written to flash.
uint32_t             lastChunkIndex    = 0xFFFFFFFF; // Last chunk index accepted by the device.
uint32_t             runningCRC32      = 0;          // CRC32 accumulator updated per chunk.

/***********************************************************
brief       Initializes serial, device identity, home automation
            I/O, Wi-Fi, and MQTT before the main loop begins.
arguments   None
return-type void
************************************************************/
void setup()
{
    Serial.begin(115200);
    delay(300);

    publishLog("=================================");
    publishLog("ESP32 Home Automation + OTA v%s", FW_VERSION);
    publishLog("=================================");

    // Reads the ESP32 MAC-based ID used for device-specific OTA topics.
    deviceID = getDeviceID();
    Serial.println("[DEVICE] MAC ID: " + deviceID);

    // Loads persisted app-visible identity and Wi-Fi credentials before networking starts.
    loadDeviceConfig();

    // Prepares relay outputs, button interrupts, and RGB PWM before networking starts.
    initializeHomeAutomation();

    // Builds all MQTT OTA topic strings from the current device ID.
    initTopics();

    // Connects to the configured Wi-Fi network before MQTT setup.
    connectWiFi();

    // Points the shared MQTT client at the configured broker.
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);

    // Enlarges the MQTT buffer so OTA chunk packets fit safely.
    mqttClient.setBufferSize(MQTT_BUFFER_SIZE);   // runtime buffer override

    // Registers the common MQTT callback used by OTA and automation topics.
    mqttClient.setCallback(mqttCallback);

    // Keeps the MQTT session alive during normal automation operation.
    mqttClient.setKeepAlive(60);

    // Prevents long socket waits from blocking the firmware too long.
    mqttClient.setSocketTimeout(10);

    // Connects MQTT and subscribes to OTA plus home automation topics.
    connectMQTT();

    publishLog("[SETUP] Ready - automation and OTA online.");
}

/***********************************************************
brief       Keeps Wi-Fi and MQTT alive, processes packets,
            and handles button-driven relay updates.
arguments   None
return-type void
************************************************************/
void loop()
{
    // Reconnects Wi-Fi if the station disconnects from the router.
    if (WiFi.status() != WL_CONNECTED)
    {
        publishLog("[WiFi] Lost - reconnecting...");
        connectWiFi();
    }

    // Restores MQTT and subscriptions when the broker connection drops.
    if (!mqttClient.connected())
        connectMQTT();

    // Processes incoming MQTT packets and keep-alive traffic.
    mqttClient.loop();

    // Handles debounced physical button events for relay toggling.
    //processHomeAutomation();

    // Keeps app auto-discovery fresh after the desktop app restarts.
    publishPeriodicDeviceInfo();

    handleLedBlink();
}
