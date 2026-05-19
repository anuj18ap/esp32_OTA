#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "FirmwareApp.h"
#include "FirmwareState.h"

// MQTT topics used by the relay and RGB automation feature.
#define TOPIC_RELAY_STATUS  "home/relay/status"
#define TOPIC_RELAY_SET     "home/relay/set"
#define TOPIC_RGB1_STATUS   "home/rgb/1/status"
#define TOPIC_RGB1_SET      "home/rgb/1/set"
#define TOPIC_RGB2_STATUS   "home/rgb/2/status"
#define TOPIC_RGB2_SET      "home/rgb/2/set"

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

// LEDC PWM configuration for 0-100 percent RGB values.
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

// Stores RGB channel levels as 0-100 percent values.
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
brief       Converts a 0-100 percent brightness value to an
            8-bit LEDC PWM duty value.
arguments   pct - Brightness percentage from 0 to 100
return-type uint8_t - PWM duty from 0 to 255
************************************************************/
static uint8_t pct2duty(uint8_t pct)
{
    // Keeps zero brightness fully off.
    if (pct == 0) return 0;

    // Clamps full brightness to the maximum 8-bit PWM value.
    if (pct >= 100) return 255;

    // Scales middle percentage values into the 0-255 duty range.
    return (uint8_t)((pct * 255UL) / 100);
}

/***********************************************************
brief       Writes the stored RGB state for one strip to the
            matching LEDC PWM channels.
arguments   idx - RGB strip index, 0 for RGB1 or 1 for RGB2
return-type void
************************************************************/
static void applyRGB(uint8_t idx)
{
    uint8_t redDuty   = pct2duty(rgb[idx].r);
    uint8_t greenDuty = pct2duty(rgb[idx].g);
    uint8_t blueDuty  = pct2duty(rgb[idx].b);

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
brief       Publishes one RGB strip state as a retained CSV
            payload in R,G,B percent format.
arguments   idx - RGB strip index, 0 for RGB1 or 1 for RGB2
return-type void
************************************************************/
static void publishRGB(uint8_t idx)
{
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%u,%u,%u", rgb[idx].r, rgb[idx].g, rgb[idx].b);

    const char* topic = (idx == 0) ? TOPIC_RGB1_STATUS : TOPIC_RGB2_STATUS;
    mqttClient.publish(topic, buffer, true);
    Serial.printf("[MQTT] pub %s -> %s\n", topic, buffer);
}

/***********************************************************
brief       Parses an MQTT RGB command payload in R,G,B format
            and clamps each value to 0-100 percent.
arguments   payload - MQTT payload bytes
            len     - Number of payload bytes
            r       - Output red percentage
            g       - Output green percentage
            b       - Output blue percentage
return-type bool - true when parsing succeeds
************************************************************/
static bool parseRGB(const char* payload, unsigned int len,
                     uint8_t* r, uint8_t* g, uint8_t* b)
{
    char buffer[32];

    // Rejects payloads that cannot fit in the local parsing buffer.
    if (len >= sizeof(buffer)) return false;

    // Copies the MQTT bytes into a null-terminated string for sscanf.
    memcpy(buffer, payload, len);
    buffer[len] = '\0';

    int red;
    int green;
    int blue;

    // Requires exactly three comma-separated integer values.
    if (sscanf(buffer, "%d,%d,%d", &red, &green, &blue) != 3) return false;

    // Clamps all RGB percentages to the supported 0-100 range.
    *r = (uint8_t)constrain(red, 0, 100);
    *g = (uint8_t)constrain(green, 0, 100);
    *b = (uint8_t)constrain(blue, 0, 100);
    return true;
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
brief       Builds a one-byte relay bitmask from the current
            ON/OFF state of all eight relay channels.
arguments   None
return-type uint8_t - Relay bitmask, bit0 for relay 1 through bit7 for relay 8
************************************************************/
static uint8_t buildRelayByte()
{
    uint8_t mask = 0;
    for (uint8_t i = 0; i < 8; i++)
    {
        // Sets one bit for every relay channel that is currently ON.
        if (channels[i].on) mask |= (1 << i);
    }
    return mask;
}

/***********************************************************
brief       Publishes all relay states as one retained raw byte
            where bit0 maps to relay 1 and bit7 maps to relay 8.
arguments   None
return-type void
************************************************************/
static void publishRelayStatus()
{
    uint8_t mask = buildRelayByte();
    mqttClient.publish(TOPIC_RELAY_STATUS, &mask, 1, true);
    Serial.printf("[MQTT] pub relay/status -> 0x%02X\n", mask);
}

/***********************************************************
brief       Applies an MQTT relay bitmask to all relay outputs
            and publishes the retained relay status.
arguments   mask - Relay bitmask, bit0 for relay 1 through bit7 for relay 8
return-type void
************************************************************/
static void applyRelayByte(uint8_t mask)
{
    for (uint8_t i = 0; i < 8; i++)
    {
        // Extracts this channel state from the matching bit position.
        bool on = (mask >> i) & 0x01;

        // Stores and applies the new relay state.
        channels[i].on = on;
        writeOutput(i, on);
        Serial.printf("[RELAY] ch%u -> %s\n", i + 1, on ? "ON" : "OFF");
    }
    publishRelayStatus();
}

/***********************************************************
brief       Toggles one relay channel from a debounced button
            event and publishes the updated relay bitmask.
arguments   ch - Relay channel index from 0 to 7
return-type void
************************************************************/
static void toggleChannel(uint8_t ch)
{
    channels[ch].on = !channels[ch].on;
    writeOutput(ch, channels[ch].on);
    Serial.printf("[BTN] ch%u toggled -> %s\n", ch + 1,
                  channels[ch].on ? "ON" : "OFF");
    publishRelayStatus();
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

    Serial.println("[HOME] Relay and RGB hardware initialized.");
}

/***********************************************************
brief       Subscribes the shared MQTT client to relay and RGB
            command topics.
arguments   None
return-type void
************************************************************/
void subscribeHomeAutomationTopics()
{
    mqttClient.subscribe(TOPIC_RELAY_SET, 1);
    mqttClient.subscribe(TOPIC_RGB1_SET, 1);
    mqttClient.subscribe(TOPIC_RGB2_SET, 1);
}

/***********************************************************
brief       Publishes retained relay and RGB states after an
            MQTT connection or reconnection.
arguments   None
return-type void
************************************************************/
void publishHomeAutomationState()
{
    publishRelayStatus();
    publishRGB(0);
    publishRGB(1);
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
    // Handles relay bitmask commands from MQTT.
    if (topic == TOPIC_RELAY_SET)
    {
        if (length >= 1)
        {
            uint8_t mask = payload[0];
            Serial.printf("[MQTT] relay set -> 0x%02X\n", mask);
            applyRelayByte(mask);
        }
        return true;
    }

    // Handles RGB CSV commands for either RGB strip.
    if (topic == TOPIC_RGB1_SET || topic == TOPIC_RGB2_SET)
    {
        // Chooses RGB strip 1 or 2 based on the command topic.
        uint8_t idx = (topic == TOPIC_RGB1_SET) ? 0 : 1;
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        // Parses, applies, and republishes valid RGB commands.
        if (parseRGB((const char*)payload, length, &red, &green, &blue))
        {
            rgb[idx] = {red, green, blue};
            applyRGB(idx);
            publishRGB(idx);
            Serial.printf("[RGB%u] set -> R=%u G=%u B=%u\n",
                          idx + 1, red, green, blue);
        }
        else
        {
            Serial.println("[RGB] bad payload - expected R,G,B (0-100)");
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
