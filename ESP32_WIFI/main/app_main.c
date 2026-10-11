#include "app.h"
#include "driver/gpio.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ota_manager.h"
#include "ota_protocol.h"
#include "ota_transports.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void startup_failure(esp_err_t err)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    app_log("[STARTUP] Critical initialization failed: %s", esp_err_to_name(err));
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY)
    {
        esp_err_t rollback = esp_ota_mark_app_invalid_rollback_and_reboot();
        app_log("[STARTUP] Rollback unavailable: %s", esp_err_to_name(rollback));
    }
    ESP_ERROR_CHECK(err);
}
static bool self_test(void)
{
    static const uint8_t crc_vector[] = "123456789";
    if ((ota_crc32(UINT32_MAX, crc_vector, 9) ^ UINT32_MAX) != 0xcbf43926)
    {
        return false;
    }
    char *encrypted = crypto_encrypt("startup-self-test"), *plain = NULL;
    if (!encrypted)
    {
        return false;
    }
    bool ok = crypto_decrypt((uint8_t *)encrypted, strlen(encrypted), &plain) &&
              !strcmp(plain, "startup-self-test");
    free(encrypted);
    free(plain);
    return ok;
}
void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    // Do not erase all saved WiFi/security credentials when NVS recovery fails.
    if (err != ESP_OK)
    {
        startup_failure(err);
        return;
    }
    uint8_t mac[6];
    err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK)
    {
        startup_failure(err);
        return;
    }
    // ESP.getEfuseMac() packs bytes little-endian; preserve Arduino topic identity.
    snprintf(device_id, sizeof(device_id), "%02X%02X%02X%02X%02X%02X", mac[5], mac[4], mac[3],
             mac[2], mac[1], mac[0]);
    app_log("ESP32 Home Automation + OTA v%s; device %s", FW_VERSION, device_id);
    err = home_automation_init();
    if (err != ESP_OK)
    {
        startup_failure(err);
        return;
    }
    err = ota_manager_init();
    if (err != ESP_OK)
    {
        startup_failure(err);
        return;
    }
    err = network_init();
    if (err != ESP_OK)
    {
        startup_failure(err);
        return;
    }
    err = ota_ble_init();
    if (err != ESP_OK)
    {
        startup_failure(err);
        return;
    }
    err = ota_espnow_init();
    if (err != ESP_OK)
    {
        startup_failure(err);
        return;
    }
    err = ota_provision_init();
    if (err != ESP_OK)
    {
        startup_failure(err);
        return;
    }
    if (!self_test())
    {
        startup_failure(ESP_FAIL);
        return;
    }
    uint64_t initialized = app_millis(), led_at = 0;
    bool led = false, verified = false;
    for (;;)
    {
        network_process();
        ota_espnow_process();
        ota_manager_tick();
        uint64_t now = app_millis();
        // Confirm only after all critical modules passed initialization and 10 seconds of stable
        // task execution. Router/broker availability is not an application correctness test.
        if (!verified && now - initialized >= 10000)
        {
            err = ota_manager_verify_startup(self_test());
            if (err != ESP_OK)
            {
                startup_failure(err);
                return;
            }
            verified = true;
        }
        if (now - led_at >= 1000)
        {
            led_at = now;
            led = !led;
            gpio_set_level(LED_PIN, led);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
