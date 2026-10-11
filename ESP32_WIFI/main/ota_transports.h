#pragma once
#include "ota_manager.h"
esp_err_t ota_http_start(const char *url, const ota_metadata_t *meta);
esp_err_t ota_ble_init(void);
esp_err_t ota_provision_init(void);
esp_err_t ota_espnow_init(void);
esp_err_t ota_espnow_send_running(void);
void ota_espnow_process(void);
