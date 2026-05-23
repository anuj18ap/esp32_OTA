#include <stdarg.h>
#include <stdio.h>

#include "FirmwareApp.h"
#include "FirmwareState.h"

/***********************************************************
brief       Publishes a formatted debug log string to the
            device MQTT log topic and mirrors it to Serial.
arguments   format - printf-style format string
            ...    - Values used by the format string
return-type void
************************************************************/
void publishLog(const char* format, ...)
{
    char logBuffer[192];

    va_list args;
    va_start(args, format);
    vsnprintf(logBuffer, sizeof(logBuffer), format, args);
    va_end(args);

    Serial.println(logBuffer);

    if (mqttClient.connected() && topicLog.length() > 0)
    {
        mqttClient.publish(topicLog.c_str(), logBuffer, false);
    }
}
