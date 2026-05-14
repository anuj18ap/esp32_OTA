#include "Config.h"
#include "FirmwareApp.h"
#include "FirmwareState.h"

/***********************************************************
brief       Toggles the onboard LED at a fixed interval to
            show that the firmware loop is running.
arguments   None
return type void
************************************************************/
void handleLedBlink()
{
    // Reads the current system time in milliseconds.
    unsigned long now = millis();
    // Toggles the LED only when the blink interval has elapsed.
    if (now - previousLedMillis >= LED_BLINK_INTERVAL)
    {
        // Stores the time of this LED state update.
        previousLedMillis = now;

        // Flips the LED state between ON and OFF.
        ledState = !ledState;
        
        // Applies the new state to the physical LED pin.
        digitalWrite(LED_PIN, ledState);
    }
}
