#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_CRC 0x109
#define ESP_ERR_INVALID_VERSION 0x10a
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 16
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 17
#define ESP_PARTITION_SUBTYPE_ANY 255
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define portMAX_DELAY 0xffffffff
typedef void *SemaphoreHandle_t;
typedef unsigned nvs_handle_t;
typedef unsigned esp_ota_handle_t;
typedef struct
{
    uint32_t address, size;
    char label[17];
} esp_partition_t;
typedef struct
{
    char version[32];
} esp_app_desc_t;
typedef enum
{
    ESP_OTA_IMG_UNDEFINED,
    ESP_OTA_IMG_PENDING_VERIFY,
    ESP_OTA_IMG_INVALID,
    ESP_OTA_IMG_ABORTED,
    ESP_OTA_IMG_VALID
} esp_ota_img_states_t;
typedef struct
{
    int initialized;
} mbedtls_sha256_context;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_boot_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *);
const esp_partition_t *esp_partition_find_first(int, int, const char *);
esp_err_t esp_flash_get_size(void *, uint32_t *);
esp_err_t esp_ota_begin(const esp_partition_t *, size_t, esp_ota_handle_t *);
esp_err_t esp_ota_write(esp_ota_handle_t, const void *, size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *);
esp_err_t esp_ota_get_partition_description(const esp_partition_t *, esp_app_desc_t *);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);
const char *esp_err_to_name(esp_err_t);
uint32_t esp_random(void);
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
void mbedtls_sha256_init(mbedtls_sha256_context *);
void mbedtls_sha256_free(mbedtls_sha256_context *);
int mbedtls_sha256_starts(mbedtls_sha256_context *, int);
int mbedtls_sha256_update(mbedtls_sha256_context *, const uint8_t *, size_t);
int mbedtls_sha256_finish(mbedtls_sha256_context *, uint8_t *);
