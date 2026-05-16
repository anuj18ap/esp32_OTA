#include <Arduino.h>

#include "Config.h"
#include "FirmwareApp.h"
#include "FirmwareState.h"

const char* WIFI_SSID     = "AceTech Solution";  // Wi-Fi network name used by the ESP32.
const char* WIFI_PASSWORD = "AceTech@2023";     // Wi-Fi password used by the ESP32.
const char* MQTT_BROKER   = "broker.emqx.io";       // Public MQTT broker used for OTA messages.
const int   MQTT_PORT     = 1883;                 // MQTT TCP port for the broker.

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

unsigned long        previousLedMillis = 0;          // Last LED toggle timestamp.
bool                 ledState          = false;      // Current LED output state.
volatile OtaState    otaState          = OTA_IDLE;   // Current OTA receive state.
uint32_t             expectedChunks    = 0;          // Number of chunks expected for the update.
uint32_t             expectedCRC32     = 0;          // CRC32 value sent by the uploader.
uint32_t             totalSize         = 0;          // Total firmware image size in bytes.
uint32_t             chunksReceived    = 0;          // Number of unique chunks written to flash.
uint32_t             lastChunkIndex    = 0xFFFFFFFF; // Last chunk index accepted by the device.
uint32_t             runningCRC32      = 0;          // CRC32 accumulator updated per chunk.

/***********************************************************
brief       Initializes serial, GPIO, device identity, Wi-Fi,
            and MQTT before the main loop begins.
arguments   None
return-type void
************************************************************/
void setup()
{
    Serial.begin(115200);
    delay(300);

    Serial.println("\n=============================");
    Serial.printf("  ESP32 OTA  (MQTT)  v%s\n", FW_VERSION);
    Serial.println("=============================");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    deviceID = getDeviceID();
    Serial.println("[DEVICE] MAC ID: " + deviceID);

    initTopics();
    connectWiFi();

    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setBufferSize(MQTT_BUFFER_SIZE);   // runtime buffer override
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(60);
    mqttClient.setSocketTimeout(10);

    connectMQTT();

    Serial.println("[SETUP] Ready - waiting for OTA commands.");
}

/***********************************************************
brief       Keeps Wi-Fi and MQTT alive, processes packets,
            and blinks the status LED.
arguments   None
return-type void
************************************************************/
void loop()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[WiFi] Lost - reconnecting...");
        connectWiFi();
    }

    if (!mqttClient.connected())
        connectMQTT();

    mqttClient.loop();
    handleLedBlink();
}
