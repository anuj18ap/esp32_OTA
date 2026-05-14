#include <Arduino.h>

#include "Config.h"
#include "FirmwareApp.h"
#include "FirmwareState.h"

const char* WIFI_SSID = "Anuj prajapati_4G";
const char* WIFI_PASSWORD = "anuj9879212759";
const char* MQTT_BROKER = "broker.hivemq.com";
const int MQTT_PORT = 1883;


WiFiClient espClient;                   // Creates the base TCP client used by the MQTT client.
PubSubClient mqttClient(espClient);     // Wraps the TCP client with MQTT support.
String deviceID;                        // Stores the unique device identifier used in MQTT topics.
String topicInfo;                       // Topic used to publish device firmware information.
String topicOTA;                        // Topic used to receive OTA update URLs.
String topicOTACheck;                   // Topic used to handle OTA readiness checks.
String topicOTAStatus;                  // Topic used to publish OTA status updates.


unsigned long previousLedMillis = 0;    // Tracks the last LED toggle time for non-blocking blinking.
bool ledState = false;                  // Stores the current LED output state.

/***********************************************************
brief       Initializes serial communication, GPIO, device identity,
            Wi-Fi, and MQTT before the normal runtime loop begins.
arguments   None
return type void
************************************************************/
void setup()
{
    // Starts serial logging for debugging and status messages.
    Serial.begin(115200);
    // Gives the serial monitor a short moment to become ready.
    delay(200);

    // Prints a startup banner with the current firmware version.
    Serial.println("\n=============================");
    Serial.println("  ESP32 OTA System  v" FW_VERSION);
    Serial.println("=============================");

    // Configures the onboard LED pin and starts with the LED turned off.
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // Reads the unique device ID from the ESP32 hardware.
    deviceID = getDeviceID();
    Serial.println("[DEVICE] ID: " + deviceID);

    // Builds all MQTT topic names for this device.
    initTopics();
    // Connects the board to the configured Wi-Fi network.
    connectWiFi();

    // Configures the MQTT broker address, buffer size, and callback handler.
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
    mqttClient.setCallback(mqttCallback);

    // Establishes the MQTT connection and subscribes to OTA topics.
    connectMQTT();

    Serial.println("[SETUP] Initialisation complete.");
}

/***********************************************************
brief       Keeps Wi-Fi and MQTT connected while continuously
            processing MQTT traffic and updating the status LED.
arguments   None
return type void
************************************************************/
void loop()
{
    // Reconnects Wi-Fi if the station connection drops.
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[WiFi] Lost connection - reconnecting...");
        connectWiFi();
    }

    // Reconnects MQTT if the broker connection is lost.
    if (!mqttClient.connected())
        connectMQTT();

    // Processes queued MQTT packets and keep-alive traffic.
    mqttClient.loop();
    
    // Blinks the LED without blocking the main loop.
    handleLedBlink();
}
