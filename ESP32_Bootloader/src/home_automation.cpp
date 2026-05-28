#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FirmwareApp.h"
#include "FirmwareState.h"

// Button inputs are active-LOW and use the ESP32 internal pull-up.
const uint8_t BTN_PINS[8] = {13, 12, 14, 27, 26, 25, 33, 32};

// Relay outputs drive the IN pins on the relay board.
const uint8_t OUT_PINS[8] = {23, 22, 21, 19, 18, 5, 4, 2};

// Indicator LEDs mirror the relay ON/OFF state.
const uint8_t RELAY_LED_PINS[8] = {15, 16, 17, 3, 1, 10, 9, 0};

// Most relay boards are active-LOW, so LOW turns the relay ON.
const bool RELAY_ACTIVE_HIGH = false;

// RGB strip PWM output pins.
const uint8_t RGB1_R = 26;
const uint8_t RGB1_G = 27;
const uint8_t RGB1_B = 14;
const uint8_t RGB2_R = 25;
const uint8_t RGB2_G = 33;
const uint8_t RGB2_B = 32;

// LEDC PWM configuration for 0-255 RGB values.
const uint32_t PWM_FREQ       = 5000;
const uint8_t  PWM_RESOLUTION = 8;

// LEDC channel assignments for both RGB strips.
#define LEDC_RGB1_R  0
#define LEDC_RGB1_G  1
#define LEDC_RGB1_B  2
#define LEDC_RGB2_R  3
#define LEDC_RGB2_G  4
#define LEDC_RGB2_B  5

// Button debounce time in milliseconds.
#define DEBOUNCE_MS 50

// Selects the LEDC API used by the installed ESP32 Arduino core.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
#define USE_LEDC_PIN_API 1
#else
#define USE_LEDC_PIN_API 0
#endif

// Stores relay state and last debounce time for one channel.
struct ChannelState
{
    bool     on;
    uint32_t lastDebounceMs;
};

// Stores RGB channel levels as 0-255 values.
struct RGBState
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

ChannelState channels[8]; // Relay and button state for all eight channels.
RGBState     rgb[2];      // RGB strip states, where index 0 is RGB1 and index 1 is RGB2.

volatile uint8_t btnEventMask = 0;              // ISR-to-loop button event bitmask.
portMUX_TYPE     buttonMux    = portMUX_INITIALIZER_UNLOCKED; // Protects button event updates.

static void writeOutput(uint8_t ch, bool on);

/***********************************************************
brief       Attaches one RGB GPIO to LEDC PWM using the API
            available in the installed ESP32 Arduino core.
arguments   pin        - GPIO pin used for PWM output
            channel    - LEDC channel for Arduino core 2.x
            frequency  - PWM frequency in hertz
            resolution - PWM duty resolution in bits
return-type void
************************************************************/
static void setupPwmPin(uint8_t pin, uint8_t channel,
                        uint32_t frequency, uint8_t resolution)
{
#if USE_LEDC_PIN_API
    // Arduino-ESP32 3.x attaches PWM directly by GPIO pin.
    (void)channel;
    ledcAttach(pin, frequency, resolution);
#else
    // Arduino-ESP32 2.x requires a channel setup before attaching the pin.
    ledcSetup(channel, frequency, resolution);
    ledcAttachPin(pin, channel);
#endif
}

/***********************************************************
brief       Writes an RGB PWM duty value using the LEDC API
            available in the installed ESP32 Arduino core.
arguments   pin     - GPIO pin used for Arduino core 3.x PWM writes
            channel - LEDC channel used for Arduino core 2.x PWM writes
            duty    - PWM duty value
return-type void
************************************************************/
static void writePwmDuty(uint8_t pin, uint8_t channel, uint8_t duty)
{
#if USE_LEDC_PIN_API
    // Arduino-ESP32 3.x writes PWM duty by GPIO pin.
    (void)channel;
    ledcWrite(pin, duty);
#else
    // Arduino-ESP32 2.x writes PWM duty by LEDC channel.
    (void)pin;
    ledcWrite(channel, duty);
#endif
}

/***********************************************************
brief       Writes the stored RGB state for one strip to the
            matching LEDC PWM channels.
arguments   idx - RGB strip index, 0 for RGB1 or 1 for RGB2
return-type void
************************************************************/
static void applyRGB(uint8_t idx)
{
    uint8_t redDuty   = rgb[idx].r;
    uint8_t greenDuty = rgb[idx].g;
    uint8_t blueDuty  = rgb[idx].b;

    if (idx == 0)
    {
        // Applies RGB strip 1 duty values to its three PWM outputs.
        writePwmDuty(RGB1_R, LEDC_RGB1_R, redDuty);
        writePwmDuty(RGB1_G, LEDC_RGB1_G, greenDuty);
        writePwmDuty(RGB1_B, LEDC_RGB1_B, blueDuty);
    }
    else
    {
        // Applies RGB strip 2 duty values to its three PWM outputs.
        writePwmDuty(RGB2_R, LEDC_RGB2_R, redDuty);
        writePwmDuty(RGB2_G, LEDC_RGB2_G, greenDuty);
        writePwmDuty(RGB2_B, LEDC_RGB2_B, blueDuty);
    }
}

/***********************************************************
brief       Parses a JSON RGB payload expected by the Python V3 app:
            {"r":N,"g":N,"b":N}, with each value clamped to 0-255.
return-type bool - true when parsing succeeds
************************************************************/
static bool parseRGBJson(byte* payload, unsigned int len,
                         uint8_t* r, uint8_t* g, uint8_t* b)
{
    JsonDocument document;
    DeserializationError error = deserializeJson(document, payload, len);
    if (error) return false;

    *r = (uint8_t)constrain(document["r"] | 0, 0, 255);
    *g = (uint8_t)constrain(document["g"] | 0, 0, 255);
    *b = (uint8_t)constrain(document["b"] | 0, 0, 255);
    return true;
}

/***********************************************************
brief       Publishes the relay ACK expected by the Python V3 app.
************************************************************/
static void publishRelayAck(uint8_t relayMask)
{
    char payload[5];
    snprintf(payload, sizeof(payload), "0x%02X", relayMask);

    String valueTopic = deviceID + "/relay/ack/" + String(payload);
    mqttClient.publish(valueTopic.c_str(), payload, false);

    String legacyTopic = deviceID + "/relay/ack";
    mqttClient.publish(legacyTopic.c_str(), payload, false);

    mqttClient.loop();
    publishLog("[MQTT] relay ACK -> %s", valueTopic.c_str());
}

/***********************************************************
brief       Publishes the RGB ACK expected by the Python V3 app.
************************************************************/
static void publishRgbAck()
{
    String topic = deviceID + "/rgb/ack";
    mqttClient.publish(topic.c_str(), "ok", false);
    mqttClient.loop();
    publishLog("[MQTT] RGB ACK -> ok");
}

/***********************************************************
brief       Parses Python V3 relay command payloads such as "0x01".
return-type bool - true when a byte mask was parsed
************************************************************/
static bool parseRelayMask(byte* payload, unsigned int len, uint8_t* mask)
{
    if (len == 1)
    {
        char c = (char)payload[0];
        if (c >= '0' && c <= '9')
            *mask = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            *mask = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            *mask = (uint8_t)(c - 'A' + 10);
        else
            *mask = payload[0];
        return true;
    }

    if (len == 4 &&
        payload[0] == 0x00 &&
        payload[1] == 0x00 &&
        payload[2] == 0x00)
    {
        *mask = payload[3];
        return true;
    }

    char buffer[12];
    if (len == 0 || len >= sizeof(buffer)) return false;

    memcpy(buffer, payload, len);
    buffer[len] = '\0';

    char* start = buffer;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;

    char* end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';

    if ((start[0] == '0') && (start[1] == 'x' || start[1] == 'X'))
        start += 2;

    if (*start == '\0') return false;

    uint16_t value = 0;
    while (*start)
    {
        char c = *start++;
        uint8_t nibble;
        if (c >= '0' && c <= '9')
            nibble = c - '0';
        else if (c >= 'a' && c <= 'f')
            nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            nibble = c - 'A' + 10;
        else
            return false;

        value = (uint16_t)((value << 4) | nibble);
        if (value > 0xFF) return false;
    }

    *mask = (uint8_t)value;
    return true;
}

/***********************************************************
brief       Applies one relay state bitmask to all relay outputs.
            Bit 0 maps to relay 1 and bit 7 maps to relay 8.
************************************************************/
static void applyRelayMask(uint8_t mask)
{
    for (uint8_t i = 0; i < 8; i++)
    {
        bool on = (mask & (1 << i)) != 0;
        channels[i].on = on;
        writeOutput(i, on);
        publishLog("[RELAY] ch%u -> %s", i + 1, on ? "ON" : "OFF");
    }
}

/***********************************************************
brief       Drives one relay output and its indicator LED to
            match the requested logical channel state.
arguments   ch - Relay channel index from 0 to 7
            on - Desired logical relay state
return-type void
************************************************************/
static void writeOutput(uint8_t ch, bool on)
{
    // Converts logical ON/OFF into the electrical level required by the relay board.
    bool relayLevel = RELAY_ACTIVE_HIGH ? on : !on;

    // Drives the relay coil input for the selected channel.
    digitalWrite(OUT_PINS[ch], relayLevel ? HIGH : LOW);

    // Drives the indicator LED active-HIGH regardless of relay polarity.
    digitalWrite(RELAY_LED_PINS[ch], on ? HIGH : LOW);
}

/***********************************************************
brief       Toggles one relay channel from a debounced button
            event or MQTT request.
arguments   ch - Relay channel index from 0 to 7
return-type void
************************************************************/
static void toggleChannel(uint8_t ch)
{
    channels[ch].on = !channels[ch].on;
    writeOutput(ch, channels[ch].on);
    publishLog("[BTN] ch%u toggled -> %s", ch + 1,
               channels[ch].on ? "ON" : "OFF");
}

// Creates a small ISR for each button that only records a pending event bit.
#define MAKE_ISR(N) \
    static void IRAM_ATTR isr_btn##N() \
    { \
        portENTER_CRITICAL_ISR(&buttonMux); \
        btnEventMask |= (1 << (N)); \
        portEXIT_CRITICAL_ISR(&buttonMux); \
    }

MAKE_ISR(0)
MAKE_ISR(1)
MAKE_ISR(2)
MAKE_ISR(3)
MAKE_ISR(4)
MAKE_ISR(5)
MAKE_ISR(6)
MAKE_ISR(7)

typedef void (*isr_fn_t)();

// Maps each button index to its interrupt service routine.
const isr_fn_t ISR_TABLE[8] = {
    isr_btn0, isr_btn1, isr_btn2, isr_btn3,
    isr_btn4, isr_btn5, isr_btn6, isr_btn7
};

/***********************************************************
brief       Initializes relay outputs, indicator LEDs, button
            interrupts, and both RGB PWM output groups.
arguments   None
return-type void
************************************************************/
void initializeHomeAutomation()
{
    // Starts all relay channels OFF and configures output pins.
    for (uint8_t i = 0; i < 8; i++)
    {
        channels[i] = {false, 0};
        pinMode(OUT_PINS[i], OUTPUT);
        pinMode(RELAY_LED_PINS[i], OUTPUT);
        writeOutput(i, false);
    }

    // Enables internal pull-ups and attaches falling-edge button interrupts.
    for (uint8_t i = 0; i < 8; i++)
    {
        pinMode(BTN_PINS[i], INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(BTN_PINS[i]), ISR_TABLE[i], FALLING);
    }

    // Configures PWM outputs for RGB strip 1.
    setupPwmPin(RGB1_R, LEDC_RGB1_R, PWM_FREQ, PWM_RESOLUTION);
    setupPwmPin(RGB1_G, LEDC_RGB1_G, PWM_FREQ, PWM_RESOLUTION);
    setupPwmPin(RGB1_B, LEDC_RGB1_B, PWM_FREQ, PWM_RESOLUTION);

    // Configures PWM outputs for RGB strip 2.
    setupPwmPin(RGB2_R, LEDC_RGB2_R, PWM_FREQ, PWM_RESOLUTION);
    setupPwmPin(RGB2_G, LEDC_RGB2_G, PWM_FREQ, PWM_RESOLUTION);
    setupPwmPin(RGB2_B, LEDC_RGB2_B, PWM_FREQ, PWM_RESOLUTION);

    // Starts both RGB strips fully off.
    for (uint8_t i = 0; i < 2; i++)
    {
        rgb[i] = {0, 0, 0};
        applyRGB(i);
    }

    publishLog("[HOME] Relay and RGB hardware initialized.");
}

/***********************************************************
brief       Subscribes the shared MQTT client to relay and RGB
            command topics.
arguments   None
return-type void
************************************************************/
void subscribeHomeAutomationTopics()
{
    mqttClient.subscribe(relayTopic.c_str(), 1);
    String relayValueTopic = relayTopic + "/+";
    mqttClient.subscribe(relayValueTopic.c_str(), 1);
    mqttClient.subscribe(rgb1Topic.c_str(), 1);
    mqttClient.subscribe(rgb2Topic.c_str(), 1);
    publishLog("[MQTT] Subscribed relay/RGB request topics.");
}

/***********************************************************
brief       Publishes retained relay and RGB states after an
            MQTT connection or reconnection.
arguments   None
return-type void
************************************************************/
void publishHomeAutomationState()
{
    publishLog("[HOME] Relay/RGB state ready; request topics use ACK responses.");
}

/***********************************************************
brief       Handles relay and RGB MQTT command topics that are
            not part of the OTA update protocol.
arguments   topic   - MQTT topic received from the broker
            payload - Raw MQTT payload bytes
            length  - Number of payload bytes
return-type bool - true when the topic was handled
************************************************************/
bool handleHomeAutomationMessage(const String& topic, byte* payload, unsigned int length)
{
    String appRelayTopic = deviceID + "/relay";
    String appRelayTopicPrefix = appRelayTopic + "/";
    if (topic == appRelayTopic || topic.startsWith(appRelayTopicPrefix))
    {
        uint8_t mask;
        bool parsed = false;
        byte payloadCopy[12];
        unsigned int parseLength = length;

        if (topic.startsWith(appRelayTopicPrefix))
        {
            String value = topic.substring(appRelayTopicPrefix.length());
            if (value == "ack" || value.startsWith("ack/"))
            {
                return true;
            }
            else
            {
                parsed = parseRelayMask((byte*)value.c_str(), value.length(), &mask);
            }
        }

        if (!parsed)
        {
            if (length <= sizeof(payloadCopy))
            {
                memcpy(payloadCopy, payload, length);
                parsed = parseRelayMask(payloadCopy, parseLength, &mask);
            }
        }

        if (!parsed)
        {
            publishLog("[RELAY] bad app payload bytes: %02X %02X %02X %02X len=%u",
                       length > 0 ? payload[0] : 0,
                       length > 1 ? payload[1] : 0,
                       length > 2 ? payload[2] : 0,
                       length > 3 ? payload[3] : 0,
                       length);
            return true;
        }

        publishLog("[MQTT] relay set topic=%s mask=0x%02X",
                   topic.c_str(), mask);
        applyRelayMask(mask);
        publishRelayAck(mask);
        return true;
    }

    publishLog("[MQTT] Automation topic received: %s (%u bytes)",
               topic.c_str(), length);

    String appRgb1Topic = deviceID + "/rgb/1";
    String appRgb2Topic = deviceID + "/rgb/2";
    if (topic == appRgb1Topic || topic == appRgb2Topic)
    {
        uint8_t idx = (topic == appRgb1Topic) ? 0 : 1;
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        if (parseRGBJson(payload, length, &red, &green, &blue))
        {
            rgb[idx] = {red, green, blue};
            applyRGB(idx);
            publishRgbAck();
            publishLog("[RGB%u] app set -> R=%u G=%u B=%u",
                       idx + 1, red, green, blue);
        }
        else
        {
            publishLog("[RGB] bad app payload - expected JSON r/g/b.");
        }
        return true;
    }

    return false;
}

/***********************************************************
brief       Processes pending button ISR events, applies debounce,
            and toggles relay channels from confirmed button presses.
arguments   None
return-type void
************************************************************/
void processHomeAutomation()
{
    uint8_t events;

    // Copies and clears pending button events from the ISR safely.
    portENTER_CRITICAL(&buttonMux);
    events = btnEventMask;
    btnEventMask = 0;
    portEXIT_CRITICAL(&buttonMux);

    uint32_t now = millis();
    for (uint8_t i = 0; i < 8; i++)
    {
        // Skips channels that did not report a button interrupt.
        if (!(events & (1 << i))) continue;

        // Ignores button noise inside the debounce window.
        if ((now - channels[i].lastDebounceMs) < DEBOUNCE_MS) continue;

        channels[i].lastDebounceMs = now;

        // Confirms the button is still pressed before toggling the relay.
        if (digitalRead(BTN_PINS[i]) == LOW)
        {
            toggleChannel(i);
        }
    }
}
