#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "ota_transports.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool decode_hex(const cJSON *doc, const char *name, uint8_t *out, size_t size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(doc, name);
    if (!cJSON_IsString(item) || strlen(item->valuestring) != size * 2)
    {
        return false;
    }
    for (size_t i = 0; i < size; i++)
    {
        char hex[3] = {item->valuestring[i * 2], item->valuestring[i * 2 + 1], 0};
        if (!isxdigit((unsigned char)hex[0]) || !isxdigit((unsigned char)hex[1]))
        {
            return false;
        }
        unsigned value;
        if (sscanf(hex, "%2x", &value) != 1)
        {
            return false;
        }
        out[i] = value;
    }
    return true;
}
static void provision(const char *line)
{
    cJSON *doc = cJSON_Parse(line);
    if (!doc)
    {
        return;
    }
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(doc, "provision_ota"),
          *pin = cJSON_GetObjectItemCaseSensitive(doc, "ble_pin"),
          *role = cJSON_GetObjectItemCaseSensitive(doc, "role");
    uint8_t key[32], pmk[16], lmk[16], peer[6];
    bool valid = cJSON_IsTrue(enabled) && cJSON_IsNumber(pin) && pin->valuedouble >= 0 &&
                 pin->valuedouble <= 999999 && pin->valuedouble == pin->valueint &&
                 cJSON_IsNumber(role) && (role->valuedouble == 0 || role->valuedouble == 1) &&
                 decode_hex(doc, "ota_key", key, 32) && decode_hex(doc, "pmk", pmk, 16) &&
                 decode_hex(doc, "lmk", lmk, 16) && decode_hex(doc, "peer_mac", peer, 6) &&
                 !(peer[0] & 1);
    if (valid && !ota_manager_busy())
    {
        nvs_handle_t nvs;
        esp_err_t err = nvs_open("otacfg", NVS_READWRITE, &nvs);
        if (err == ESP_OK)
        {
            err = nvs_set_blob(nvs, "ota_key", key, 32);
            if (err == ESP_OK)
            {
                err = nvs_set_blob(nvs, "pmk", pmk, 16);
            }
            if (err == ESP_OK)
            {
                err = nvs_set_blob(nvs, "lmk", lmk, 16);
            }
            if (err == ESP_OK)
            {
                err = nvs_set_blob(nvs, "peer_mac", peer, 6);
            }
            if (err == ESP_OK)
            {
                err = nvs_set_u32(nvs, "ble_pin", pin->valueint);
            }
            if (err == ESP_OK)
            {
                err = nvs_set_u8(nvs, "role", role->valueint);
            }
            if (err == ESP_OK)
            {
                err = nvs_set_u64(nvs, "nonce", 0);
            }
            if (err == ESP_OK)
            {
                err = nvs_commit(nvs);
            }
            nvs_close(nvs);
        }
        if (err == ESP_OK)
        {
            app_log("[PROVISION] OTA security configuration saved; restarting");
            app_request_restart(1000);
        }
        else
        {
            app_log("[PROVISION] Save failed: %s", esp_err_to_name(err));
        }
    }
    else
    {
        app_log("[PROVISION] Invalid request or OTA busy");
    }
    memset(key, 0, sizeof(key));
    memset(pmk, 0, sizeof(pmk));
    memset(lmk, 0, sizeof(lmk));
    cJSON_Delete(doc);
}
static void provision_task(void *arg)
{
    (void)arg;
    char line[1024];
    size_t used = 0;
    bool overflow = false;
    for (;;)
    {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100)) <= 0)
        {
            continue;
        }
        if (c == '\n')
        {
            if (!overflow)
            {
                line[used] = 0;
                provision(line);
            }
            memset(line, 0, sizeof(line));
            used = 0;
            overflow = false;
        }
        else if (c != '\r')
        {
            if (used + 1 < sizeof(line))
            {
                line[used++] = c;
            }
            else
            {
                overflow = true;
            }
        }
    }
}
esp_err_t ota_provision_init(void)
{
    esp_err_t err = uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0);
    if (err != ESP_OK)
    {
        return err;
    }
    return xTaskCreate(provision_task, "ota_provision", 4096, NULL, 2, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
