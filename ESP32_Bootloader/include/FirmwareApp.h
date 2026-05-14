#pragma once

#include <Arduino.h>

String getDeviceID();
void initTopics();
void connectWiFi();
void publishDeviceInfo();
void performOTA(const String& url);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void connectMQTT();
void handleLedBlink();
