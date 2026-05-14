#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>

extern WiFiClient espClient;
extern PubSubClient mqttClient;

extern String deviceID;
extern String topicInfo;
extern String topicOTA;
extern String topicOTACheck;
extern String topicOTAStatus;

extern unsigned long previousLedMillis;
extern bool ledState;
