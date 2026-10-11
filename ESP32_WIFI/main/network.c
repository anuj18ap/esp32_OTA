#include "app.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "ota_manager.h"
#include "ota_protocol.h"
#include "ota_transports.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char device_id[13], device_name[128], wifi_ssid[33], wifi_password[65];
static esp_mqtt_client_handle_t mqtt;
static EventGroupHandle_t wifi_events;
static QueueHandle_t messages;
static httpd_handle_t portal;
static atomic_bool wifi_online, mqtt_online, connect_event, disconnect_event, retry_station;
static atomic_int published_message;
static atomic_uint connection_epoch;
static atomic_uint_fast64_t reboot_at;
static uint64_t connect_started, info_at, report_sent_at;
static bool provisioning, connecting, mqtt_started;
static uint32_t mqtt_session, report_id;
static int report_message = -1;
static char portal_message[128];
typedef struct
{
    char topic[96];
    size_t len;
    unsigned epoch;
    uint8_t data[];
} message_t;
#define WIFI_UP 1

uint64_t app_millis(void)
{
    return esp_timer_get_time() / 1000;
}
bool app_wifi_connected(void)
{
    return atomic_load(&wifi_online);
}
bool app_mqtt_connected(void)
{
    return atomic_load(&mqtt_online);
}
void app_request_restart(unsigned delay_ms)
{
    atomic_store(&reboot_at, app_millis() + delay_ms + 1);
}
bool app_publish(const char *suffix, const char *text, int qos, bool retain)
{
    if (!mqtt || !app_mqtt_connected())
    {
        return false;
    }
    char topic[96];
    snprintf(topic, sizeof(topic), !strcmp(suffix, "info") ? "AceTech/%s/%s" : "%s/%s", device_id,
             suffix);
    return esp_mqtt_client_publish(mqtt, topic, text, 0, qos, retain) >= 0;
}
bool app_publish_encrypted(const char *suffix, const char *text, bool retain)
{
    char *encrypted = crypto_encrypt(text);
    if (!encrypted)
    {
        return false;
    }
    bool ok = app_publish(suffix, encrypted, 0, retain);
    free(encrypted);
    return ok;
}
void app_log(const char *format, ...)
{
    char text[192];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    printf("%s\n", text);
    app_publish_encrypted("log", text, false);
}
void app_publish_info(void)
{
    cJSON *doc = cJSON_CreateObject();
    if (!doc)
    {
        return;
    }
    cJSON_AddStringToObject(doc, "id", device_id);
    cJSON_AddStringToObject(doc, "firmware", FW_VERSION);
    cJSON_AddStringToObject(doc, "name", device_name);
    char *text = cJSON_PrintUnformatted(doc);
    if (text)
    {
        app_publish_encrypted("info", text, true);
        free(text);
    }
    cJSON_Delete(doc);
}
static void trim(char *text)
{
    char *start = text;
    while (isspace((unsigned char)*start))
    {
        start++;
    }
    size_t len = strlen(start);
    while (len && isspace((unsigned char)start[len - 1]))
    {
        len--;
    }
    memmove(text, start, len);
    text[len] = 0;
}
esp_err_t config_save(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("appcfg", NVS_READWRITE, &nvs);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_str(nvs, "name", device_name);
    if (err == ESP_OK)
    {
        err = nvs_set_str(nvs, "ssid", wifi_ssid);
    }
    if (err == ESP_OK)
    {
        err = nvs_set_str(nvs, "password", wifi_password);
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}
static void config_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open("appcfg", NVS_READONLY, &nvs) != ESP_OK)
    {
        return;
    }
    size_t len = sizeof(device_name);
    if (nvs_get_str(nvs, "name", device_name, &len) != ESP_OK)
    {
        device_name[0] = 0;
    }
    len = sizeof(wifi_ssid);
    if (nvs_get_str(nvs, "ssid", wifi_ssid, &len) != ESP_OK)
    {
        wifi_ssid[0] = 0;
    }
    len = sizeof(wifi_password);
    if (nvs_get_str(nvs, "password", wifi_password, &len) != ESP_OK)
    {
        wifi_password[0] = 0;
    }
    nvs_close(nvs);
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        atomic_store(&wifi_online, true);
        gpio_set_level(WIFI_LED_PIN, 1);
        xEventGroupSetBits(wifi_events, WIFI_UP);
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        atomic_store(&wifi_online, false);
        atomic_store(&mqtt_online, false);
        gpio_set_level(WIFI_LED_PIN, 0);
        gpio_set_level(MQTT_LED_PIN, 0);
        xEventGroupClearBits(wifi_events, WIFI_UP);
        atomic_store(&disconnect_event, true);
        if (atomic_load(&retry_station))
        {
            esp_wifi_connect();
        }
    }
}
static esp_err_t station_credentials(const char *ssid, const char *password)
{
    size_t a = strlen(ssid), b = strlen(password);
    if (!a || a > 32 || b > 64)
    {
        return ESP_ERR_INVALID_ARG;
    }
    atomic_store(&retry_station, false);
    esp_wifi_disconnect();
    xEventGroupClearBits(wifi_events, WIFI_UP);
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, ssid, a);
    memcpy(cfg.sta.password, password, b);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err == ESP_OK)
    {
        atomic_store(&retry_station, true);
        err = esp_wifi_connect();
    }
    return err;
}
static void dns_task(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in local = {
        .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0)
    {
        if (fd >= 0)
        {
            close(fd);
        }
        app_log("[SETUP] DNS start failed");
        vTaskDelete(NULL);
        return;
    }
    for (;;)
    {
        uint8_t packet[512];
        struct sockaddr_in sender;
        socklen_t slen = sizeof(sender);
        int len = recvfrom(fd, packet, sizeof(packet) - 16, 0, (struct sockaddr *)&sender, &slen);
        if (len < 17 || (packet[2] & 0x80) || packet[4] != 0 || packet[5] != 1)
        {
            continue;
        }
        size_t end = 12;
        while (end < (size_t)len && packet[end])
        {
            unsigned label = packet[end];
            if (label > 63 || end + 1 + label >= (size_t)len)
            {
                end = len;
                break;
            }
            end += 1 + label;
        }
        if (end + 5 > (size_t)len)
        {
            continue;
        }
        end++;
        bool a = packet[end] == 0 && packet[end + 1] == 1 && packet[end + 2] == 0 &&
                 packet[end + 3] == 1;
        end += 4;
        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0;
        packet[7] = a ? 1 : 0;
        memset(packet + 8, 0, 4);
        if (a)
        {
            const uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
            memcpy(packet + end, answer, sizeof(answer));
            end += sizeof(answer);
        }
        sendto(fd, packet, end, 0, (struct sockaddr *)&sender, slen);
    }
}
static char *escape_html(const char *input)
{
    size_t capacity = strlen(input) * 6 + 1;
    char *out = calloc(1, capacity);
    if (!out)
    {
        return NULL;
    }
    for (const char *p = input; *p; p++)
    {
        const char *replacement = *p == '&'    ? "&amp;"
                                  : *p == '<'  ? "&lt;"
                                  : *p == '>'  ? "&gt;"
                                  : *p == '\"' ? "&quot;"
                                               : NULL;
        size_t n = strlen(out);
        if (replacement)
        {
            strcat(out, replacement);
        }
        else
        {
            out[n] = *p;
            out[n + 1] = 0;
        }
    }
    return out;
}
static esp_err_t redirect(httpd_req_t *req, const char *path)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", path);
    return httpd_resp_send(req, "", 0);
}
static esp_err_t form_value(const char *form, const char *key, char *out, size_t capacity)
{
    out[0] = 0;
    size_t key_len = strlen(key);
    for (const char *p = form; *p;)
    {
        const char *end = strchr(p, '&');
        if (!end)
        {
            end = p + strlen(p);
        }
        if ((size_t)(end - p) > key_len && !strncmp(p, key, key_len) && p[key_len] == '=')
        {
            p += key_len + 1;
            size_t n = 0;
            while (p < end)
            {
                unsigned char c = *p++;
                if (c == '+')
                {
                    c = ' ';
                }
                else if (c == '%')
                {
                    if (end - p < 2 || !isxdigit((unsigned char)p[0]) ||
                        !isxdigit((unsigned char)p[1]))
                    {
                        return ESP_ERR_INVALID_ARG;
                    }
                    char hex[3] = {p[0], p[1], 0};
                    c = strtoul(hex, NULL, 16);
                    p += 2;
                }
                if (!c || n + 1 >= capacity)
                {
                    return ESP_ERR_INVALID_SIZE;
                }
                out[n++] = c;
            }
            out[n] = 0;
            return ESP_OK;
        }
        p = *end ? end + 1 : end;
    }
    return ESP_ERR_NOT_FOUND;
}
static esp_err_t read_form(httpd_req_t *req, char *out, size_t capacity)
{
    if (req->content_len <= 0 || req->content_len >= capacity)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t used = 0;
    unsigned timeouts = 0;
    while (used < req->content_len)
    {
        int n = httpd_req_recv(req, out + used, req->content_len - used);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3)
        {
            continue;
        }
        if (n <= 0)
        {
            return ESP_FAIL;
        }
        used += n;
    }
    out[used] = 0;
    return ESP_OK;
}
static esp_err_t wifi_page(httpd_req_t *req, const char *message, bool success)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, "<!DOCTYPE html><html><head><meta name='viewport' "
                                  "content='width=device-width,initial-scale=1'><title>ESP32 WiFi "
                                  "Setup</title></head><body><h2>Select WiFi Network</h2><form "
                                  "action='/connect' method='POST'><select name='ssid' required>");
    wifi_scan_config_t scan = {0};
    uint16_t count = 0;
    if (esp_wifi_scan_start(&scan, true) == ESP_OK)
    {
        esp_wifi_scan_get_ap_num(&count);
    }
    if (count > 32)
    {
        count = 32;
    }
    wifi_ap_record_t *records = count ? calloc(count, sizeof(*records)) : NULL;
    if (records && esp_wifi_scan_get_ap_records(&count, records) == ESP_OK)
    {
        for (unsigned i = 0; i < count; i++)
        {
            char ssid[33] = {0};
            memcpy(ssid, records[i].ssid, 32);
            if (!ssid[0])
            {
                continue;
            }
            char *escaped = escape_html(ssid);
            if (!escaped)
            {
                continue;
            }
            char option[512];
            snprintf(option, sizeof(option), "<option value=\"%s\">%s (%d dBm)</option>", escaped,
                     escaped, records[i].rssi);
            httpd_resp_sendstr_chunk(req, option);
            free(escaped);
        }
    }
    else
    {
        httpd_resp_sendstr_chunk(req, "<option value=''>No WiFi networks found</option>");
    }
    free(records);
    esp_wifi_clear_ap_list();
    httpd_resp_sendstr_chunk(
        req, "</select><input id='pass' name='pass' type='password' placeholder='Enter Password' "
             "required><button type='button' onclick=\"var "
             "p=document.getElementById('pass');p.type=p.type==='password'?'text':'password'\">"
             "Show/hide</button><button type='submit'>Connect</button></form>");
    char *escaped = escape_html(message);
    if (escaped)
    {
        httpd_resp_sendstr_chunk(req,
                                 success ? "<p style='color:green'>" : "<p style='color:red'>");
        httpd_resp_sendstr_chunk(req, escaped);
        httpd_resp_sendstr_chunk(req, "</p>");
        free(escaped);
    }
    httpd_resp_sendstr_chunk(req, "</body></html>");
    return httpd_resp_send_chunk(req, NULL, 0);
}
static esp_err_t portal_get(httpd_req_t *req)
{
    if (!strcmp(req->uri, "/"))
    {
        return redirect(req, device_name[0] ? "/wifi" : "/name");
    }
    if (!strcmp(req->uri, "/wifi"))
    {
        return wifi_page(req, portal_message, false);
    }
    if (!strcmp(req->uri, "/name"))
    {
        char *name = escape_html(device_name);
        if (!name)
        {
            return ESP_ERR_NO_MEM;
        }
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr_chunk(
            req,
            "<!DOCTYPE html><html><head><meta name='viewport' "
            "content='width=device-width,initial-scale=1'></head><body><h2>Enter Device "
            "Name</h2><form action='/setname' method='POST'><input name='dev' required value=\"");
        httpd_resp_sendstr_chunk(req, name);
        free(name);
        httpd_resp_sendstr_chunk(req,
                                 "\"><button type='submit'>Continue</button></form></body></html>");
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    return redirect(req, "http://192.168.4.1/");
}
static esp_err_t portal_post(httpd_req_t *req)
{
    char form[1024];
    if (read_form(req, form, sizeof(form)) != ESP_OK)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    }
    if (!strcmp(req->uri, "/setname"))
    {
        char name[sizeof(device_name)];
        if (form_value(form, "dev", name, sizeof(name)) != ESP_OK)
        {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid device name");
        }
        trim(name);
        snprintf(device_name, sizeof(device_name), "%s", name);
        if (config_save() != ESP_OK)
        {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot save name");
        }
        return redirect(req, "/wifi");
    }
    if (strcmp(req->uri, "/connect"))
    {
        return redirect(req, "/");
    }
    char ssid[33], password[65];
    if (form_value(form, "ssid", ssid, sizeof(ssid)) != ESP_OK ||
        form_value(form, "pass", password, sizeof(password)) != ESP_OK)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid WiFi credentials");
    }
    trim(ssid);
    if (station_credentials(ssid, password) == ESP_OK &&
        (xEventGroupWaitBits(wifi_events, WIFI_UP, false, false, pdMS_TO_TICKS(WIFI_TIMEOUT_MS)) &
         WIFI_UP))
    {
        snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", ssid);
        snprintf(wifi_password, sizeof(wifi_password), "%s", password);
        if (config_save() != ESP_OK)
        {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Cannot save credentials");
        }
        esp_err_t err = wifi_page(req, "Connected. Saving and rebooting...", true);
        app_request_restart(1500);
        return err;
    }
    atomic_store(&retry_station, false);
    esp_wifi_disconnect();
    snprintf(portal_message, sizeof(portal_message),
             "Wrong password or connection failed. Try again.");
    return wifi_page(req, portal_message, false);
}
static esp_err_t start_portal(void)
{
    if (provisioning)
    {
        return ESP_OK;
    }
    atomic_store(&retry_station, false);
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK)
    {
        return err;
    }
    wifi_config_t cfg = {0};
    snprintf((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid), "ESP32-Setup-%s", device_id + 6);
    cfg.ap.ssid_len = strlen((char *)cfg.ap.ssid);
    memcpy(cfg.ap.password, "12345678", 8);
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection = 4;
    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK)
    {
        return err;
    }
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 6144;
    http.uri_match_fn = httpd_uri_match_wildcard;
    err = httpd_start(&portal, &http);
    if (err != ESP_OK)
    {
        return err;
    }
    httpd_uri_t get = {.uri = "/*", .method = HTTP_GET, .handler = portal_get},
                post = {.uri = "/*", .method = HTTP_POST, .handler = portal_post};
    err = httpd_register_uri_handler(portal, &get);
    if (err == ESP_OK)
    {
        err = httpd_register_uri_handler(portal, &post);
    }
    if (err != ESP_OK)
    {
        httpd_stop(portal);
        portal = NULL;
        return err;
    }
    if (xTaskCreate(dns_task, "portal_dns", 3072, NULL, 3, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    provisioning = true;
    connecting = false;
    app_log("[SETUP] Hotspot %s; open http://192.168.4.1", cfg.ap.ssid);
    return ESP_OK;
}
static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    static uint8_t assembled[MQTT_BUFFER_SIZE];
    static char topic[96];
    static size_t received, total;
    if (id == MQTT_EVENT_CONNECTED)
    {
        atomic_fetch_add(&connection_epoch, 1);
        atomic_store(&mqtt_online, true);
        atomic_store(&connect_event, true);
        gpio_set_level(MQTT_LED_PIN, 1);
    }
    else if (id == MQTT_EVENT_DISCONNECTED)
    {
        atomic_store(&mqtt_online, false);
        atomic_store(&disconnect_event, true);
        gpio_set_level(MQTT_LED_PIN, 0);
        received = total = 0;
    }
    else if (id == MQTT_EVENT_PUBLISHED)
    {
        atomic_store(&published_message, event->msg_id);
    }
    else if (id == MQTT_EVENT_DATA)
    {
        if (event->current_data_offset == 0)
        {
            received = total = 0;
            if (event->topic_len <= 0 || event->topic_len >= sizeof(topic) ||
                event->total_data_len < 0 || event->total_data_len > sizeof(assembled))
            {
                return;
            }
            memcpy(topic, event->topic, event->topic_len);
            topic[event->topic_len] = 0;
            total = event->total_data_len;
        }
        if (event->current_data_offset != received || event->data_len < 0 ||
            received + (size_t)event->data_len > total)
        {
            received = total = 0;
            return;
        }
        if (event->data_len)
        {
            memcpy(assembled + received, event->data, event->data_len);
        }
        received += event->data_len;
        if (received == total)
        {
            message_t *message = malloc(sizeof(*message) + total + 1);
            if (!message)
            {
                return;
            }
            snprintf(message->topic, sizeof(message->topic), "%s", topic);
            message->len = total;
            message->epoch = atomic_load(&connection_epoch);
            memcpy(message->data, assembled, total);
            message->data[total] = 0;
            if (xQueueSend(messages, &message, 0) != pdTRUE)
            {
                free(message);
            }
            received = total = 0;
        }
    }
}
static void subscribe_all(void)
{
    static const char *suffixes[] = {"set_name",     "wifi/request", "wifi/set",  "reset_config",
                                     "ota_check",    "ota/begin",    "ota/chunk", "ota/end",
                                     "relay",        "rgb/1",        "rgb/2",     "ota/request",
                                     "ota/v2/chunk", "ota/v2/end"};
    for (unsigned i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
    {
        char topic[96];
        snprintf(topic, sizeof(topic), "%s/%s", device_id, suffixes[i]);
        int qos =
            (!strcmp(suffixes[i], "ota/chunk") || !strcmp(suffixes[i], "ota/v2/chunk")) ? 0 : 1;
        if (esp_mqtt_client_subscribe(mqtt, topic, qos) < 0)
        {
            app_log("[MQTT] Subscription failed: %s", suffixes[i]);
        }
    }
    esp_mqtt_client_subscribe(mqtt, "+/rgb/1", 1);
    esp_mqtt_client_subscribe(mqtt, "+/rgb/2", 1);
}
static const char *json_string(const cJSON *doc, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(doc, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}
// New transport-control commands require a provisioned HMAC key and monotonic nonce.
static bool authorize(const cJSON *doc)
{
    uint8_t key[32], digest[32];
    nvs_handle_t nvs;
    if (nvs_open("otacfg", NVS_READWRITE, &nvs) != ESP_OK)
    {
        return false;
    }
    size_t len = sizeof(key);
    uint64_t prior = 0;
    nvs_get_u64(nvs, "nonce", &prior);
    const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(doc, "nonce");
    bool ok = nvs_get_blob(nvs, "ota_key", key, &len) == ESP_OK && len == sizeof(key) &&
              cJSON_IsNumber(nonce) && nonce->valuedouble > prior &&
              nonce->valuedouble <= 9007199254740991.0 &&
              floor(nonce->valuedouble) == nonce->valuedouble;
    const char *auth = json_string(doc, "auth");
    char canonical[1024];
    if (ok)
    {
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(doc, "size"),
                    *chunks = cJSON_GetObjectItemCaseSensitive(doc, "chunks"),
                    *crc = cJSON_GetObjectItemCaseSensitive(doc, "crc32"),
                    *session = cJSON_GetObjectItemCaseSensitive(doc, "session_id");
        int n = snprintf(canonical, sizeof(canonical), "%s|%.0f|%.0f|%.0f|%.0f|%s|%s|%s|%.0f",
                         json_string(doc, "op"), nonce->valuedouble,
                         cJSON_IsNumber(size) ? size->valuedouble : 0,
                         cJSON_IsNumber(chunks) ? chunks->valuedouble : 0,
                         cJSON_IsNumber(crc) ? crc->valuedouble : 0, json_string(doc, "sha256"),
                         json_string(doc, "version"), json_string(doc, "url"),
                         cJSON_IsNumber(session) ? session->valuedouble : 0);
        ok = n > 0 && n < sizeof(canonical) && strlen(auth) == 64 &&
             mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, sizeof(key),
                             (uint8_t *)canonical, n, digest) == 0;
        unsigned difference = 0;
        for (unsigned i = 0; ok && i < 32; i++)
        {
            char hex[3] = {auth[i * 2], auth[i * 2 + 1], 0};
            if (strspn(hex, "0123456789abcdefABCDEF") != 2)
            {
                ok = false;
                break;
            }
            difference |= digest[i] ^ (uint8_t)strtoul(hex, NULL, 16);
        }
        ok = ok && !difference;
        if (ok)
        {
            ok = nvs_set_u64(nvs, "nonce", (uint64_t)nonce->valuedouble) == ESP_OK &&
                 nvs_commit(nvs) == ESP_OK;
        }
    }
    memset(key, 0, sizeof(key));
    nvs_close(nvs);
    return ok;
}
static void extended_request(const uint8_t *data, size_t len)
{
    char *plain = NULL;
    if (!crypto_decrypt(data, len, &plain))
    {
        return;
    }
    cJSON *doc = cJSON_Parse(plain);
    free(plain);
    if (!doc || !authorize(doc))
    {
        cJSON_Delete(doc);
        ota_report_external(OTA_MQTT, 0, "COMMAND_AUTH_FAILED", "Missing or invalid HMAC/nonce");
        return;
    }
    const char *op = json_string(doc, "op");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    ota_metadata_t meta;
    if (!strcmp(op, "status"))
    {
        ota_manager_snapshot();
        err = ESP_OK;
    }
    else if (!strcmp(op, "http") && ota_metadata_parse(doc, &meta) == ESP_OK && meta.has_sha256)
    {
        err = ota_http_start(json_string(doc, "url"), &meta);
    }
    else if (!strcmp(op, "mqtt") && ota_metadata_parse(doc, &meta) == ESP_OK && meta.has_sha256)
    {
        err = ota_manager_begin(OTA_MQTT, &meta, &mqtt_session);
    }
    else if (!strcmp(op, "espnow_send_running"))
    {
        err = ota_espnow_send_running();
    }
    else if (!strcmp(op, "cancel"))
    {
        cJSON *s = cJSON_GetObjectItemCaseSensitive(doc, "session_id");
        if (cJSON_IsNumber(s) && s->valuedouble > 0 && s->valuedouble <= UINT32_MAX)
        {
            uint32_t id = s->valuedouble;
            for (int transport = OTA_MQTT; transport <= OTA_ESPNOW; transport++)
            {
                if (ota_manager_owned(transport, id))
                {
                    err = ota_manager_abort(transport, id, "OTA_ABORTED",
                                            "Cancelled by authorized command");
                    break;
                }
            }
        }
    }
    if (err != ESP_OK)
    {
        ota_report_external(OTA_MQTT, 0, "OTA_REQUEST_REJECTED", esp_err_to_name(err));
    }
    cJSON_Delete(doc);
}
static void handle_message(message_t *message)
{
    const char *topic = message->topic;
    uint8_t *data = message->data;
    size_t len = message->len;
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "%s/", device_id);
    if (strncmp(topic, prefix, strlen(prefix)))
    {
        home_automation_message(topic, data, len);
        return;
    }
    const char *suffix = topic + strlen(prefix);
    if (!strcmp(suffix, "ota_check"))
    {
        app_publish("ota_status", "READY", 0, false);
        app_publish_info();
        return;
    }
    if (!strcmp(suffix, "ota/request"))
    {
        extended_request(data, len);
        return;
    }
    if (!strcmp(suffix, "ota/v2/chunk"))
    {
        if (len > 8 && ota_manager_write(OTA_MQTT, ota_read_be32(data), ota_read_be32(data + 4),
                                         data + 8, len - 8) == ESP_OK)
        {
            char ack[64];
            snprintf(ack, sizeof(ack), "{\"session_id\":%lu,\"sequence\":%lu}",
                     (unsigned long)ota_read_be32(data), (unsigned long)ota_read_be32(data + 4));
            app_publish("ota/v2/ack", ack, 0, false);
        }
        return;
    }
    if (!strcmp(suffix, "ota/v2/end"))
    {
        if (len == 4)
        {
            ota_manager_finish(OTA_MQTT, ota_read_be32(data));
        }
        return;
    }
#ifdef CONFIG_OTA_ENABLE_LEGACY_MQTT
    if (!strcmp(suffix, "ota/begin"))
    {
        cJSON *doc = cJSON_Parse((char *)data);
        ota_metadata_t meta;
        esp_err_t err = ota_metadata_parse(doc, &meta);
        cJSON_Delete(doc);
        if (err == ESP_OK)
        {
            uint32_t session;
            err = ota_manager_begin(OTA_MQTT, &meta, &session);
            if (err == ESP_OK)
            {
                mqtt_session = session;
            }
        }
        if (err != ESP_OK)
        {
            app_publish("ota_status", "FAILED", 0, false);
        }
        return;
    }
    if (!strcmp(suffix, "ota/chunk"))
    {
        if (len > 4 && ota_manager_write(OTA_MQTT, mqtt_session, ota_read_be32(data), data + 4,
                                         len - 4) == ESP_OK)
        {
            char ack[12];
            snprintf(ack, sizeof(ack), "%lu", (unsigned long)ota_read_be32(data));
            app_publish("ota/ack", ack, 0, false);
        }
        return;
    }
    if (!strcmp(suffix, "ota/end"))
    {
        ota_manager_finish(OTA_MQTT, mqtt_session);
        return;
    }
#endif
    if (home_automation_message(topic, data, len))
    {
        return;
    }
    char *plain = NULL;
    if (!crypto_decrypt(data, len, &plain))
    {
        app_log("[MQTT] Payload decode failed");
        return;
    }
    if (!strcmp(suffix, "set_name"))
    {
        trim(plain);
        if (strlen(plain) < sizeof(device_name))
        {
            snprintf(device_name, sizeof(device_name), "%s", plain);
            if (config_save() == ESP_OK)
            {
                app_publish_info();
            }
        }
    }
    else if (!strcmp(suffix, "wifi/request"))
    {
        trim(plain);
        if (!strcmp(plain, "GET_CONFIG"))
        {
            cJSON *doc = cJSON_CreateObject();
            cJSON_AddStringToObject(doc, "ssid", wifi_ssid);
            cJSON_AddStringToObject(doc, "password", wifi_password);
            char *text = cJSON_PrintUnformatted(doc);
            if (text)
            {
                app_publish_encrypted("wifi/config", text, false);
                free(text);
            }
            cJSON_Delete(doc);
        }
    }
    else if (!strcmp(suffix, "wifi/set"))
    {
        cJSON *doc = cJSON_Parse(plain);
        const char *ssid = json_string(doc, "ssid"), *password = json_string(doc, "password");
        if (*ssid && strlen(ssid) <= 32 && strlen(password) <= 64)
        {
            snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", ssid);
            snprintf(wifi_password, sizeof(wifi_password), "%s", password);
            if (config_save() == ESP_OK)
            {
                app_log("INFO: WiFi updated, rebooting...");
                app_request_restart(500);
            }
        }
        cJSON_Delete(doc);
    }
    else if (!strcmp(suffix, "reset_config"))
    {
        trim(plain);
        if (!strcmp(plain, "RESET_CONFIG"))
        {
            nvs_handle_t nvs;
            if (nvs_open("appcfg", NVS_READWRITE, &nvs) == ESP_OK)
            {
                esp_err_t err = nvs_erase_all(nvs);
                if (err == ESP_OK)
                {
                    err = nvs_commit(nvs);
                }
                nvs_close(nvs);
                if (err == ESP_OK)
                {
                    device_name[0] = wifi_ssid[0] = wifi_password[0] = 0;
                    app_log("INFO: Configuration reset, rebooting to setup portal...");
                    app_request_restart(500);
                }
            }
        }
    }
    free(plain);
}
esp_err_t network_init(void)
{
    config_load();
    wifi_events = xEventGroupCreate();
    messages = xQueueCreate(8, sizeof(message_t *));
    if (!wifi_events || !messages)
    {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK)
    {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK)
    {
        return err;
    }
    if (!esp_netif_create_default_wifi_sta() || !esp_netif_create_default_wifi_ap())
    {
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK)
    {
        return err;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK)
    {
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (err != ESP_OK)
    {
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    if (err != ESP_OK)
    {
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK)
    {
        err = esp_wifi_start();
    }
    if (err != ESP_OK)
    {
        return err;
    }
    char client_id[32];
    snprintf(client_id, sizeof(client_id), "ESP32-%s", device_id);
    esp_mqtt_client_config_t mqtt_cfg = {.broker.address.uri = MQTT_BROKER,
                                         .credentials.client_id = client_id,
                                         .session.keepalive = 60,
                                         .network.timeout_ms = 10000,
                                         .network.reconnect_timeout_ms = 2000,
                                         .buffer.size = MQTT_BUFFER_SIZE,
                                         .buffer.out_size = MQTT_BUFFER_SIZE,
                                         .task.stack_size = 6144};
    mqtt = esp_mqtt_client_init(&mqtt_cfg);
    if (!mqtt)
    {
        return ESP_ERR_NO_MEM;
    }
    err = esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    if (err != ESP_OK)
    {
        return err;
    }
    if (!wifi_ssid[0])
    {
        return start_portal();
    }
    err = station_credentials(wifi_ssid, wifi_password);
    if (err != ESP_OK)
    {
        return err;
    }
    connecting = true;
    connect_started = app_millis();
    return ESP_OK;
}
void network_process(void)
{
    uint64_t now = app_millis(), restart = atomic_load(&reboot_at);
    if (restart && now >= restart)
    {
        esp_restart();
    }
    if (atomic_exchange(&disconnect_event, false))
    {
        ota_manager_abort(OTA_MQTT, mqtt_session, "MQTT_DISCONNECTED",
                          "MQTT or WiFi connection lost");
        report_message = -1;
        report_id = 0;
    }
    if (app_wifi_connected() && !provisioning)
    {
        connecting = false;
        if (!mqtt_started)
        {
            if (esp_mqtt_client_start(mqtt) == ESP_OK)
            {
                mqtt_started = true;
            }
        }
    }
    else if (!provisioning)
    {
        if (!connecting)
        {
            connecting = true;
            connect_started = now;
            atomic_store(&retry_station, true);
            esp_wifi_connect();
        }
        if (now - connect_started >= WIFI_TIMEOUT_MS)
        {
            esp_err_t err = start_portal();
            if (err != ESP_OK)
            {
                app_log("[SETUP] Portal start failed: %s", esp_err_to_name(err));
            }
        }
    }
    if (atomic_exchange(&connect_event, false))
    {
        report_message = -1;
        subscribe_all();
        app_publish_info();
        info_at = now;
        app_log("[MQTT] Connected and subscribed.");
    }
    message_t *message;
    for (unsigned processed = 0; processed < 4 && xQueueReceive(messages, &message, 0) == pdTRUE;
         processed++)
    {
        if (app_mqtt_connected() && message->epoch == atomic_load(&connection_epoch))
        {
            handle_message(message);
        }
        free(message);
    }
    if (app_mqtt_connected() && now - info_at >= INFO_INTERVAL_MS)
    {
        app_publish_info();
        info_at = now;
    }
    int published = atomic_exchange(&published_message, 0);
    if (report_message >= 0 && published == report_message)
    {
        ota_report_delivered(report_id);
        report_message = -1;
        report_id = 0;
    }
    if (app_mqtt_connected() && (report_message < 0 || now - report_sent_at > 10000))
    {
        char text[640], topic[96];
        uint32_t id;
        if (ota_report_peek(text, sizeof(text), &id))
        {
            snprintf(topic, sizeof(topic), "%s/ota/result", device_id);
            int mid = esp_mqtt_client_publish(mqtt, topic, text, 0, 1, false);
            if (mid >= 0)
            {
                report_message = mid;
                report_id = id;
                report_sent_at = now;
            }
        }
    }
}
