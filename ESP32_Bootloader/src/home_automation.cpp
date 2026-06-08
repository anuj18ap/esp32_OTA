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
const uint8_t OUT_PINS[8] = {23, 22, 21, 19, 18, 5, 4, 15};

// Indicator LEDs mirror the relay ON/OFF state.
const uint8_t RELAY_LED_PINS[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Most relay boards are active-LOW, so LOW turns the relay ON.
const bool RELAY_ACTIVE_HIGH = false;

// RGB strip PWM output pins.
const uint8_t RGB1_R = 26;
const uint8_t RGB1_G = 27;
const uint8_t RGB1_B = 14;
const uint8_t RGB2_R = 25;
const uint8_t RGB2_G = 33;
const uint8_t RGB2_B = 32;

// RGB PWM values are Arduino-style 0-255 values for common-ground RGB LEDs.
const uint8_t RGB_OFF_DUTY = 0;
const uint32_t RGB_PWM_FREQ = 1000;
const uint8_t RGB_PWM_BITS = 8;

// Button debounce time in milliseconds.
#define DEBOUNCE_MS 50

// Button pins overlap the RGB PWM pins in this wiring, so keep buttons disabled.
const bool ENABLE_BUTTON_INPUTS = false;

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
static void writeRgbPins(uint8_t rPin, uint8_t gPin, uint8_t bPin,
                         uint8_t redDuty, uint8_t greenDuty, uint8_t blueDuty);
static void writeRgbPin(uint8_t pin, uint8_t duty);

/***********************************************************
brief       Writes one RGB color output using Arduino-style
            PWM, matching analogWrite() prototype examples.
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
        writeRgbPins(RGB1_R, RGB1_G, RGB1_B, redDuty, greenDuty, blueDuty);
    }
    else
    {
        writeRgbPins(RGB2_R, RGB2_G, RGB2_B, redDuty, greenDuty, blueDuty);
    }
}

/***********************************************************
brief       Writes common-ground RGB duty values. For this LED type,
            0 is OFF and 255 is fully ON, just like analogWrite().
return-type void
************************************************************/
static void writeRgbPins(uint8_t rPin, uint8_t gPin, uint8_t bPin,
                         uint8_t redDuty, uint8_t greenDuty, uint8_t blueDuty)
{
    writeRgbPin(rPin, redDuty);
    writeRgbPin(gPin, greenDuty);
    writeRgbPin(bPin, blueDuty);
}

/***********************************************************
brief       Drives one common-ground RGB leg. Exact 0 and 255
            use digital output so full OFF/ON works even if PWM
            setup is the part failing on the prototype board.
return-type void
************************************************************/
static void writeRgbPin(uint8_t pin, uint8_t duty)
{
    if (duty == 0)
    {
        digitalWrite(pin, LOW);
        return;
    }

    if (duty == 255)
    {
        digitalWrite(pin, HIGH);
        return;
    }

    analogWrite(pin, duty);
}

/***********************************************************
brief       Runs a short visible RGB test at boot so wiring/PWM can
            be verified before MQTT or the web app are involved.
return-type void
************************************************************/
static void runRgbBootTest()
{
    const uint8_t pins[2][3] = {
        {RGB1_R, RGB1_G, RGB1_B},
        {RGB2_R, RGB2_G, RGB2_B}
    };

    Serial.println("[RGB] Boot test starting. Common pin must be GND.");
    for (uint8_t led = 0; led < 2; led++)
    {
        Serial.printf("[RGB%u] RED pin=%u ON\n", led + 1, pins[led][0]);
        writeRgbPins(pins[led][0], pins[led][1], pins[led][2], 255, 0, 0);
        delay(700);
        Serial.printf("[RGB%u] GREEN pin=%u ON\n", led + 1, pins[led][1]);
        writeRgbPins(pins[led][0], pins[led][1], pins[led][2], 0, 255, 0);
        delay(700);
        Serial.printf("[RGB%u] BLUE pin=%u ON\n", led + 1, pins[led][2]);
        writeRgbPins(pins[led][0], pins[led][1], pins[led][2], 0, 0, 255);
        delay(700);
        writeRgbPins(pins[led][0], pins[led][1], pins[led][2], 0, 0, 0);
        delay(250);
    }
    Serial.println("[RGB] Boot test done.");
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

    String topic = relayTopic + "/ack";
    mqttClient.publish(topic.c_str(), payload, false);

    mqttClient.loop();
    publishLog("[MQTT] relay ACK -> %s payload=%s", topic.c_str(), payload);
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
    if (RELAY_LED_PINS[ch] != 0xFF)
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
        if (RELAY_LED_PINS[i] != 0xFF)
            pinMode(RELAY_LED_PINS[i], OUTPUT);
        writeOutput(i, false);
    }

    if (ENABLE_BUTTON_INPUTS)
    {
        // Enables internal pull-ups and attaches falling-edge button interrupts.
        for (uint8_t i = 0; i < 8; i++)
        {
            pinMode(BTN_PINS[i], INPUT_PULLUP);
            attachInterrupt(digitalPinToInterrupt(BTN_PINS[i]), ISR_TABLE[i], FALLING);
        }
    }

    // Configures RGB pins as normal PWM outputs, like the prototype analogWrite example.
    pinMode(RGB1_R, OUTPUT);
    pinMode(RGB1_G, OUTPUT);
    pinMode(RGB1_B, OUTPUT);
    pinMode(RGB2_R, OUTPUT);
    pinMode(RGB2_G, OUTPUT);
    pinMode(RGB2_B, OUTPUT);
    analogWriteResolution(RGB_PWM_BITS);
    analogWriteFrequency(RGB_PWM_FREQ);
    runRgbBootTest();

    // Starts both RGB strips fully off.
    for (uint8_t i = 0; i < 2; i++)
    {
        rgb[i] = {RGB_OFF_DUTY, RGB_OFF_DUTY, RGB_OFF_DUTY};
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
    mqttClient.subscribe(rgb1Topic.c_str(), 1);
    mqttClient.subscribe(rgb2Topic.c_str(), 1);
    mqttClient.subscribe("+/rgb/1", 1);
    mqttClient.subscribe("+/rgb/2", 1);
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
    bool isRgb1 = (topic == rgb1Topic);
    bool isRgb2 = (topic == rgb2Topic);
    if (!isRgb1 && !isRgb2)
    {
        String rgb1Suffix = "/rgb/1";
        String rgb2Suffix = "/rgb/2";
        if (topic.endsWith(rgb1Suffix) || topic.endsWith(rgb2Suffix))
        {
            int firstSlash = topic.indexOf('/');
            String topicDevice = firstSlash > 0 ? topic.substring(0, firstSlash) : "";
            topicDevice.toUpperCase();
            if (topicDevice == deviceID)
            {
                isRgb1 = topic.endsWith(rgb1Suffix);
                isRgb2 = topic.endsWith(rgb2Suffix);
            }
        }
    }

    if (topic == relayTopic)
    {
        uint8_t mask;
        bool parsed = false;
        byte payloadCopy[12];
        unsigned int parseLength = length;

        if (length <= sizeof(payloadCopy))
        {
            memcpy(payloadCopy, payload, length);
            parsed = parseRelayMask(payloadCopy, parseLength, &mask);
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

    if (isRgb1 || isRgb2)
    {
        char payloadText[96];
        byte payloadCopy[96];
        if (length >= sizeof(payloadCopy))
        {
            publishLog("[RGB] payload too large len=%u max=%u",
                       length, (unsigned int)(sizeof(payloadCopy) - 1));
            return true;
        }

        memcpy(payloadCopy, payload, length);
        payloadCopy[length] = '\0';
        memcpy(payloadText, payloadCopy, length + 1);

        publishLog("[RGB] RX topic=%s payload=%s len=%u",
                   topic.c_str(), payloadText, length);

        uint8_t idx = isRgb1 ? 0 : 1;
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        if (parseRGBJson(payloadCopy, length, &red, &green, &blue))
        {
            rgb[idx] = {red, green, blue};
            applyRGB(idx);
            publishRgbAck();
            publishLog("[RGB%u] applied common-ground PWM pins R=%u G=%u B=%u values R=%u G=%u B=%u",
                       idx + 1,
                       idx == 0 ? RGB1_R : RGB2_R,
                       idx == 0 ? RGB1_G : RGB2_G,
                       idx == 0 ? RGB1_B : RGB2_B,
                       red, green, blue);
        }
        else
        {
            publishLog("[RGB] bad app payload - expected JSON r/g/b.");
        }
        return true;
    }

    publishLog("[MQTT] Automation topic received: %s (%u bytes)",
               topic.c_str(), length);

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
