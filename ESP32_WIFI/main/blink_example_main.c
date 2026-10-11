#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

#define LED_PIN 2
#define AP_SSID "ESP32_Config"
#define AP_PASS "12345678"

static const char *TAG = "WIFI_PORTAL";

static bool wifi_connected = false;
static httpd_handle_t server = NULL;

/* ================= LED BLINK TIMER ================= */

static void blink_timer_callback(void *arg)
{
    if (wifi_connected)
    {
        gpio_set_level(LED_PIN, !gpio_get_level(LED_PIN));
    }
}

void start_blink_timer()
{
    const esp_timer_create_args_t blink_timer_args = {.callback = &blink_timer_callback,
                                                      .name = "blink_timer"};

    esp_timer_handle_t blink_timer;
    esp_timer_create(&blink_timer_args, &blink_timer);
    esp_timer_start_periodic(blink_timer, 500000); // 500ms
}

/* ================= WIFI EVENT HANDLER ================= */

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "STA Started");
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {

        ESP_LOGI(TAG, "Connected to WiFi!");
        wifi_connected = true;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {

        ESP_LOGI(TAG, "Disconnected!");
        wifi_connected = false;
    }
}

/* ================= HTML PAGE ================= */

static const char *html_page =
    "<!DOCTYPE html><html><head><title>ESP32 WiFi Setup</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>body{font-family:Arial;text-align:center;margin-top:40px;}"
    "input{padding:8px;margin:6px;width:200px;}"
    "button{padding:8px 15px;}</style></head>"
    "<body><h2>Connect to WiFi</h2>"
    "<form action='/connect' method='POST'>"
    "SSID:<br><input name='ssid'><br>"
    "Password:<br><input name='pass' type='password'><br>"
    "<button type='submit'>Connect</button>"
    "</form></body></html>";

/* ================= ROOT HANDLER ================= */

esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ================= CONNECT HANDLER ================= */

esp_err_t connect_post_handler(httpd_req_t *req)
{
    char buf[200];
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret <= 0)
    {
        return ESP_FAIL;
    }

    buf[ret] = '\0';

    char ssid[32] = {0};
    char pass[64] = {0};

    sscanf(buf, "ssid=%31[^&]&pass=%63s", ssid, pass);

    ESP_LOGI(TAG, "SSID: %s", ssid);

    wifi_config_t wifi_config = {0};
    strcpy((char *)wifi_config.sta.ssid, ssid);
    strcpy((char *)wifi_config.sta.password, pass);

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();

    httpd_resp_sendstr(req, "<h2>Connecting... Check LED</h2>");

    return ESP_OK;
}

/* ================= START WEB SERVER ================= */

void start_webserver()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_start(&server, &config);

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};

    httpd_uri_t connect = {.uri = "/connect", .method = HTTP_POST, .handler = connect_post_handler};

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &connect);
}

/* ================= WIFI INIT ================= */

void wifi_init()
{
    esp_netif_init();
    esp_event_loop_create_default();

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    wifi_config_t ap_config = {.ap = {.ssid = AP_SSID,
                                      .ssid_len = strlen(AP_SSID),
                                      .password = AP_PASS,
                                      .max_connection = 4,
                                      .authmode = WIFI_AUTH_WPA_WPA2_PSK}};

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "AP Started. Connect to %s", AP_SSID);
}

/* ================= APP MAIN ================= */

void app_main(void)
{
    nvs_flash_init();

    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0);

    wifi_init();
    start_webserver();
    start_blink_timer();
}