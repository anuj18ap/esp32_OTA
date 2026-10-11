#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FW_VERSION "0.6"
#define MQTT_BROKER "mqtt://broker.emqx.io:1883"
#define MQTT_BUFFER_SIZE 8192
#define WIFI_TIMEOUT_MS 20000
#define INFO_INTERVAL_MS 45000
#define LED_PIN 2
#define WIFI_LED_PIN 16
#define MQTT_LED_PIN 17

extern char device_id[13];
extern char device_name[128];
extern char wifi_ssid[33];
extern char wifi_password[65];
uint64_t app_millis(void);
void app_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
bool app_publish(const char *suffix, const char *text, int qos, bool retain);
bool app_publish_encrypted(const char *suffix, const char *text, bool retain);
void app_publish_info(void);
void app_request_restart(unsigned delay_ms);
bool app_wifi_connected(void);
bool app_mqtt_connected(void);
esp_err_t network_init(void);
void network_process(void);
esp_err_t home_automation_init(void);
bool home_automation_message(const char *topic, const uint8_t *data, size_t len);
bool crypto_decrypt(const uint8_t *data, size_t len, char **text);
char *crypto_encrypt(const char *text);
esp_err_t config_save(void);
