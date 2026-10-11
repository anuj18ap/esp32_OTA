#pragma once
#include "app.h"
#include "cJSON.h"

typedef enum
{
    OTA_MQTT,
    OTA_HTTP,
    OTA_BLE,
    OTA_ESPNOW
} ota_transport_t;
typedef struct
{
    uint32_t size, chunks, crc32;
    char version[32];
    bool has_sha256;
    uint8_t sha256[32];
} ota_metadata_t;
typedef enum
{
    OTA_IDLE,
    OTA_REQUEST_VALIDATION,
    OTA_RECEIVING,
    OTA_IMAGE_FINALIZATION,
    OTA_IMAGE_VALIDATION,
    OTA_BOOT_SELECTION,
    OTA_REBOOT_PENDING,
    OTA_VERIFYING,
    OTA_CONFIRMED,
    OTA_ABORTING,
    OTA_FAILED
} ota_state_t;
esp_err_t ota_manager_init(void);
esp_err_t ota_metadata_parse(const cJSON *doc, ota_metadata_t *out);
esp_err_t ota_manager_begin(ota_transport_t transport, const ota_metadata_t *metadata,
                            uint32_t *session);
esp_err_t ota_manager_write(ota_transport_t transport, uint32_t session, uint32_t sequence,
                            const uint8_t *data, size_t len);
esp_err_t ota_manager_finish(ota_transport_t transport, uint32_t session);
esp_err_t ota_manager_abort(ota_transport_t transport, uint32_t session, const char *code,
                            const char *message);
bool ota_manager_owned(ota_transport_t transport, uint32_t session);
bool ota_manager_busy(void);
void ota_manager_tick(void);
esp_err_t ota_manager_verify_startup(bool healthy);
bool ota_report_peek(char *json, size_t capacity, uint32_t *id);
void ota_report_delivered(uint32_t id);
void ota_report_external(ota_transport_t transport, uint32_t session, const char *code,
                         const char *message);
void ota_manager_snapshot(void);
const char *ota_transport_name(ota_transport_t transport);
