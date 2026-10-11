#include "app.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_protocol.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const int relays[8] = {23, 22, 21, 19, 18, 5, 4, 15};
static const int rgb[6] = {26, 27, 14, 25, 33, 32};
static const char *TAG = "home";
static esp_err_t rgb_set(int channel, int duty)
{
    duty = duty < 0 ? 0 : duty > 255 ? 255 : duty;
    if (duty == 0 || duty == 255)
    {
        return ledc_stop(LEDC_LOW_SPEED_MODE, channel, duty == 255);
    }
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    return err == ESP_OK ? ledc_update_duty(LEDC_LOW_SPEED_MODE, channel) : err;
}
static esp_err_t rgb_apply(int strip, int r, int g, int b)
{
    ESP_RETURN_ON_ERROR(rgb_set(strip * 3, r), TAG, "RGB red failed");
    ESP_RETURN_ON_ERROR(rgb_set(strip * 3 + 1, g), TAG, "RGB green failed");
    return rgb_set(strip * 3 + 2, b);
}
esp_err_t home_automation_init(void)
{
    gpio_config_t pins = {.mode = GPIO_MODE_OUTPUT};
    for (unsigned i = 0; i < 8; i++)
    {
        pins.pin_bit_mask |= 1ULL << relays[i];
    }
    pins.pin_bit_mask |= (1ULL << LED_PIN) | (1ULL << WIFI_LED_PIN) | (1ULL << MQTT_LED_PIN);
    ESP_RETURN_ON_ERROR(gpio_config(&pins), TAG, "GPIO configuration failed");
    for (unsigned i = 0; i < 8; i++)
    {
        ESP_RETURN_ON_ERROR(gpio_set_level(relays[i], 1), TAG, "Relay off failed");
    }
    gpio_set_level(LED_PIN, 0);
    gpio_set_level(WIFI_LED_PIN, 0);
    gpio_set_level(MQTT_LED_PIN, 0);
    ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                 .duty_resolution = LEDC_TIMER_8_BIT,
                                 .timer_num = LEDC_TIMER_0,
                                 .freq_hz = 1000,
                                 .clk_cfg = LEDC_AUTO_CLK};
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "LEDC timer failed");
    for (int i = 0; i < 6; i++)
    {
        ledc_channel_config_t channel = {.gpio_num = rgb[i],
                                         .speed_mode = LEDC_LOW_SPEED_MODE,
                                         .channel = i,
                                         .intr_type = LEDC_INTR_DISABLE,
                                         .timer_sel = LEDC_TIMER_0,
                                         .duty = 0,
                                         .hpoint = 0};
        ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "LEDC channel failed");
    }
    for (int strip = 0; strip < 2; strip++)
    {
        for (int color = 0; color < 3; color++)
        {
            ESP_RETURN_ON_ERROR(
                rgb_apply(strip, color == 0 ? 255 : 0, color == 1 ? 255 : 0, color == 2 ? 255 : 0),
                TAG, "Boot RGB test failed");
            vTaskDelay(pdMS_TO_TICKS(700));
        }
        ESP_RETURN_ON_ERROR(rgb_apply(strip, 0, 0, 0), TAG, "RGB off failed");
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    // Buttons remain disabled: GPIOs overlap RGB, as in the working Arduino firmware.
    app_log("[HOME] Relay and RGB hardware initialized.");
    return ESP_OK;
}
bool home_automation_message(const char *topic, const uint8_t *data, size_t len)
{
    char expected[64];
    snprintf(expected, sizeof(expected), "%s/relay", device_id);
    if (!strcmp(topic, expected))
    {
        uint8_t mask;
        if (!parse_relay_mask(data, len, &mask))
        {
            app_log("[RELAY] bad app payload");
            return true;
        }
        for (unsigned i = 0; i < 8; i++)
        {
            if (gpio_set_level(relays[i], !(mask & (1u << i))) != ESP_OK)
            {
                app_log("[RELAY] GPIO failure");
                return true;
            }
        }
        char ack[5];
        snprintf(ack, sizeof(ack), "0x%02X", mask);
        app_publish("relay/ack", ack, 0, false);
        return true;
    }
    const char *slash = strchr(topic, '/');
    if (!slash || slash - topic != 12)
    {
        return false;
    }
    for (int i = 0; i < 12; i++)
    {
        if (toupper((unsigned char)topic[i]) != device_id[i])
        {
            return false;
        }
    }
    int strip = !strcmp(slash, "/rgb/1") ? 0 : !strcmp(slash, "/rgb/2") ? 1 : -1;
    if (strip < 0)
    {
        return false;
    }
    if (len >= 96)
    {
        app_log("[RGB] payload too large");
        return true;
    }
    char text[96] = {0};
    memcpy(text, data, len);
    cJSON *doc = cJSON_Parse(text);
    if (!doc)
    {
        app_log("[RGB] bad app payload");
        return true;
    }
    cJSON *r = cJSON_GetObjectItemCaseSensitive(doc, "r"),
          *g = cJSON_GetObjectItemCaseSensitive(doc, "g"),
          *b = cJSON_GetObjectItemCaseSensitive(doc, "b");
    esp_err_t err =
        rgb_apply(strip, cJSON_IsNumber(r) ? r->valueint : 0, cJSON_IsNumber(g) ? g->valueint : 0,
                  cJSON_IsNumber(b) ? b->valueint : 0);
    cJSON_Delete(doc);
    if (err == ESP_OK)
    {
        app_publish("rgb/ack", "ok", 0, false);
    }
    else
    {
        app_log("[RGB] PWM failure");
    }
    return true;
}
