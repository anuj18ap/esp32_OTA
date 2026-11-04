#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#define ADMIN_WIFI_SSID "acetechadmin"
#define ADMIN_WIFI_PASSWORD "adminace@2023"
#define ADMIN_WIFI_SEARCH_TIMEOUT_MS 3000
#define USER_WIFI_SEARCH_TIMEOUT_MS 10000
typedef struct {
    char host[128];
    uint16_t port;
    char client_id[64], username[64], password[128], topic_prefix[64];
    bool tls;
} admin_mqtt_config_t;

/** Initialize the sole station Wi-Fi owner and start Admin-first recovery. */
esp_err_t admin_wifi_init(void);

/** Advance scan, connection, timeout, and force-switch transitions from app_main. */
void admin_wifi_process(void);

/** True only after User Wi-Fi has acquired an IP address. */
bool admin_wifi_user_online(void);

/** Apply saved MQTT settings to the existing client while it is stopped. */
bool admin_wifi_mqtt_dirty(void);
void admin_wifi_mqtt_applied(void);
/** Borrowed configuration; the manager owns its storage. */
const admin_mqtt_config_t *admin_wifi_mqtt_config(void);
