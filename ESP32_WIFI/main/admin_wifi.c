#include "admin_wifi.h"
#include "app.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONNECT_TIMEOUT_MS 15000
#define SCAN_LIMIT 24
typedef enum { SEARCH_ADMIN, SEARCH_USER, CONNECT_ADMIN, CONNECT_USER,
               ONLINE_ADMIN, ONLINE_USER, SWITCH_USER, RETRY_USER } state_t;
typedef enum { NO_SCAN, SEARCH_SCAN, WEB_SCAN } scan_t;
typedef struct { uint32_t version; admin_mqtt_config_t value; } mqtt_store_t;
static const char *TAG = "WIFI_MGR";
static esp_netif_t *sta;
static httpd_handle_t http;
static state_t state;
static scan_t scan_kind;
static bool scanning, timed_out, forced_user_mode, user_online, mqtt_dirty;
static uint64_t deadline, retry_at;
static uint64_t http_retry_at;
static uint32_t admin_ip;
static atomic_bool scan_event, disconnect_event, ip_event, force_command, web_scan;
static atomic_uint ip_value;
static atomic_uint_fast64_t force_at;
static wifi_ap_record_t records[SCAN_LIMIT];
static uint16_t record_count;
static enum { NOT_SCANNED, SCANNING, COMPLETE, FAILED } scan_status;
static admin_mqtt_config_t mqtt_config;
extern const uint8_t admin_html_start[] asm("_binary_admin_html_start");
extern const uint8_t admin_html_end[] asm("_binary_admin_html_end");

static const char *mode(void)
{
    switch (state) {
    case SEARCH_ADMIN: return "ADMIN_SEARCH";
    case SEARCH_USER: return "USER_SEARCH";
    case CONNECT_ADMIN: return "ADMIN_CONNECTING";
    case CONNECT_USER: return forced_user_mode ? "FORCED_USER_CONNECTING" : "USER_CONNECTING";
    case ONLINE_ADMIN: return "ADMIN_CONNECTED";
    case ONLINE_USER: return forced_user_mode ? "FORCED_USER_CONNECTED" : "USER_CONNECTED";
    case SWITCH_USER: return "SWITCH_TO_USER";
    default: return "FORCED_USER_RETRY";
    }
}
static const char *security(wifi_auth_mode_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN: return "Open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    default: return "Other";
    }
}
/* The ESP event task records facts; app_main owns every Wi-Fi driver operation. */
static void event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) atomic_store(&scan_event, true);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
        atomic_store(&disconnect_event, true);
    if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)
        atomic_store(&disconnect_event, true);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        atomic_store(&ip_value, ((ip_event_got_ip_t *)data)->ip_info.ip.addr);
        atomic_store(&ip_event, true);
    }
}
/* Search deadlines cover discovery only; association and DHCP get their own deadline. */
static void search(bool admin)
{
    state = admin ? SEARCH_ADMIN : SEARCH_USER;
    deadline = app_millis() + (admin ? ADMIN_WIFI_SEARCH_TIMEOUT_MS :
                               USER_WIFI_SEARCH_TIMEOUT_MS);
    retry_at = 0;
    ESP_LOGI(TAG, "Starting %s search (%lu ms)", admin ? "Admin" : "User",
             (unsigned long)(admin ? ADMIN_WIFI_SEARCH_TIMEOUT_MS :
                              USER_WIFI_SEARCH_TIMEOUT_MS));
}
/* Forced mode never falls back to Admin, including after authentication failure. */
static void retry_user(void)
{
    state = RETRY_USER;
    retry_at = app_millis() + 3000;
}
/* Apply one profile and begin a bounded association/DHCP attempt. */
static void connect_profile(bool admin)
{
    if (!admin && !wifi_ssid[0]) {
        if (forced_user_mode) retry_user(); else search(true);
        return;
    }
    wifi_config_t cfg = {0};
    const char *ssid = admin ? ADMIN_WIFI_SSID : wifi_ssid;
    const char *password = admin ? ADMIN_WIFI_PASSWORD : wifi_password;
    memcpy(cfg.sta.ssid, ssid, strlen(ssid));
    memcpy(cfg.sta.password, password, strlen(password));
    state = admin ? CONNECT_ADMIN : CONNECT_USER;
    deadline = app_millis() + CONNECT_TIMEOUT_MS;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err == ESP_OK) err = esp_wifi_connect();
    ESP_LOGI(TAG, "Connecting to %s network: %s", admin ? "Admin" : "User",
             esp_err_to_name(err));
    if (err != ESP_OK) {
        if (forced_user_mode) retry_user(); else search(!admin);
    }
}
/* All discovery and webpage scans pass through this single driver entry point. */
static void begin_scan(scan_t kind)
{
    wifi_scan_config_t cfg = {0};
    if (kind == SEARCH_SCAN) {
        cfg.ssid = (uint8_t *)(state == SEARCH_ADMIN ? ADMIN_WIFI_SSID : wifi_ssid);
        if (!cfg.ssid[0]) { search(true); return; }
    }
    scan_kind = kind;
    scanning = true;
    timed_out = false;
    if (kind == WEB_SCAN) { scan_status = SCANNING; deadline = app_millis() + 10000; }
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err == ESP_OK) return;
    scanning = false;
    scan_kind = NO_SCAN;
    ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(err));
    if (kind == WEB_SCAN) scan_status = FAILED;
    else if (app_millis() >= deadline) search(state == SEARCH_USER);
    else retry_at = app_millis() + 500;
}
/* Consume driver-owned results once, then release any unreturned records. */
static void finished_scan(void)
{
    if (!scanning) { esp_wifi_clear_ap_list(); return; }
    scan_t kind = scan_kind;
    scanning = false;
    scan_kind = NO_SCAN;
    uint16_t count = 0;
    esp_err_t err = esp_wifi_scan_get_ap_num(&count);
    uint16_t kept = count > SCAN_LIMIT ? SCAN_LIMIT : count;
    if (kind == WEB_SCAN) {
        record_count = kept;
        if (err == ESP_OK && kept) err = esp_wifi_scan_get_ap_records(&record_count, records);
        esp_wifi_clear_ap_list();
        scan_status = !timed_out && err == ESP_OK ? COMPLETE : FAILED;
        return;
    }
    wifi_ap_record_t first[SCAN_LIMIT];
    if (err == ESP_OK && kept) err = esp_wifi_scan_get_ap_records(&kept, first);
    esp_wifi_clear_ap_list();
    bool admin = state == SEARCH_ADMIN;
    if (!timed_out && err == ESP_OK && count) connect_profile(admin);
    else if (timed_out || app_millis() >= deadline) search(!admin);
    else retry_at = app_millis() + 500;
}
/* HTTPD listens on all interfaces; require the socket's local IP to be Admin's IP. */
static bool admin_socket(httpd_req_t *req)
{
    struct sockaddr_in addr = {0};
    socklen_t len = sizeof(addr);
    return state == ONLINE_ADMIN && admin_ip &&
        getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) == 0 &&
        addr.sin_family == AF_INET && addr.sin_addr.s_addr == admin_ip;
}
/* A custom header avoids browser form CSRF; compare without an early mismatch exit. */
static bool authorize(httpd_req_t *req)
{
    if (!admin_socket(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Admin interface only");
        return false;
    }
    char supplied[96];
    if (httpd_req_get_hdr_value_str(req, "X-Admin-Password", supplied,
                                    sizeof(supplied)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Admin password required");
        return false;
    }
    size_t n = strlen(supplied), expected = strlen(CONFIG_ADMIN_WEB_PASSWORD);
    unsigned diff = n ^ expected;
    for (size_t i = 0; i < expected; i++)
        diff |= (unsigned char)(i < n ? supplied[i] : 0) ^
                (unsigned char)CONFIG_ADMIN_WEB_PASSWORD[i];
    memset(supplied, 0, sizeof(supplied));
    if (diff) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid Admin password");
        return false;
    }
    return true;
}
static esp_err_t json_reply(httpd_req_t *req, cJSON *doc)
{
    char *body = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}
/* Bound the full request before parsing so malformed or oversized bodies are rejected. */
static cJSON *request_json(httpd_req_t *req)
{
    if (req->content_len < 2 || req->content_len > 1024) return NULL;
    char body[1025];
    size_t used = 0;
    while (used < req->content_len) {
        int n = httpd_req_recv(req, body + used, req->content_len - used);
        if (n <= 0) return NULL;
        used += n;
    }
    body[used] = 0;
    return cJSON_Parse(body);
}
static const char *field(const cJSON *doc, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(doc, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}
static esp_err_t page(httpd_req_t *req)
{
    if (!admin_socket(req))
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Admin interface only");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
        "default-src 'self'; script-src 'self' 'unsafe-inline'; "
        "style-src 'self' 'unsafe-inline'; connect-src 'self'");
    return httpd_resp_send(req, (const char *)admin_html_start,
                           admin_html_end - admin_html_start);
}
static esp_err_t status_api(httpd_req_t *req)
{
    if (!authorize(req)) return ESP_OK;
    cJSON *doc = cJSON_CreateObject();
    wifi_ap_record_t ap = {0};
    esp_wifi_sta_get_ap_info(&ap);
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char mac_text[18], ip[16];
    snprintf(mac_text, sizeof(mac_text), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(sta, &info) == ESP_OK)
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    else snprintf(ip, sizeof(ip), "0.0.0.0");
    cJSON_AddStringToObject(doc, "device_id", device_id);
    cJSON_AddStringToObject(doc, "device_name", device_name);
    cJSON_AddStringToObject(doc, "firmware", FW_VERSION);
    cJSON_AddNumberToObject(doc, "uptime_ms", app_millis());
    cJSON_AddStringToObject(doc, "mode", mode());
    // Keep the Admin network identifier out of HTTP responses.
    cJSON_AddStringToObject(doc, "connected_ssid", "Admin network");
    cJSON_AddStringToObject(doc, "ip_address", ip);
    cJSON_AddStringToObject(doc, "mac_address", mac_text);
    cJSON_AddNumberToObject(doc, "rssi", ap.rssi);
    cJSON_AddBoolToObject(doc, "mqtt_connected", app_mqtt_connected());
    return json_reply(req, doc);
}
static esp_err_t config_api(httpd_req_t *req)
{
    if (!authorize(req)) return ESP_OK;
    cJSON *doc = cJSON_CreateObject();
    cJSON *m = cJSON_AddObjectToObject(doc, "mqtt");
    cJSON_AddStringToObject(doc, "user_ssid",
                            strcmp(wifi_ssid, ADMIN_WIFI_SSID) ? wifi_ssid : "");
    cJSON_AddBoolToObject(doc, "user_password_set", wifi_password[0] != 0);
    cJSON_AddStringToObject(m, "host", mqtt_config.host);
    cJSON_AddNumberToObject(m, "port", mqtt_config.port);
    cJSON_AddStringToObject(m, "client_id", mqtt_config.client_id);
    cJSON_AddStringToObject(m, "username", mqtt_config.username);
    cJSON_AddStringToObject(m, "topic_prefix", mqtt_config.topic_prefix);
    cJSON_AddBoolToObject(m, "tls", mqtt_config.tls);
    cJSON_AddBoolToObject(m, "password_set", mqtt_config.password[0] != 0);
    return json_reply(req, doc);
}
/* Starting a scan only queues work; HTTP polling reads the cached result. */
static esp_err_t scan_api(httpd_req_t *req, bool start)
{
    if (!authorize(req)) return ESP_OK;
    if (start && scan_status != SCANNING && !scanning) {
        scan_status = SCANNING;
        atomic_store(&web_scan, true);
    }
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "status", scan_status == SCANNING ? "running" :
        scan_status == COMPLETE ? "completed" : scan_status == FAILED ? "failed" : "not_scanned");
    cJSON *list = cJSON_AddArrayToObject(doc, "networks");
    if (scan_status == COMPLETE && !start) {
        for (uint16_t i = 0; i < record_count; i++) {
            char ssid[33] = {0};
            memcpy(ssid, records[i].ssid, 32);
            if (!strcmp(ssid, ADMIN_WIFI_SSID)) continue;
            cJSON *ap = cJSON_CreateObject();
            cJSON_AddStringToObject(ap, "ssid", ssid);
            cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);
            cJSON_AddNumberToObject(ap, "channel", records[i].primary);
            cJSON_AddStringToObject(ap, "security", security(records[i].authmode));
            cJSON_AddItemToArray(list, ap);
        }
    }
    return json_reply(req, doc);
}
/* Exact, case-sensitive SSID match; report the strongest matching access point. */
static esp_err_t availability_api(httpd_req_t *req)
{
    if (!authorize(req)) return ESP_OK;
    int best = -1;
    if (scan_status == COMPLETE && wifi_ssid[0]) {
        for (uint16_t i = 0; i < record_count; i++) {
            char ssid[33] = {0};
            memcpy(ssid, records[i].ssid, 32);
            if (!strcmp(ssid, wifi_ssid) &&
                (best < 0 || records[i].rssi > records[best].rssi)) best = i;
        }
    }
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "configured_ssid",
                            strcmp(wifi_ssid, ADMIN_WIFI_SSID) ? wifi_ssid : "");
    cJSON_AddStringToObject(doc, "ssid_status", best >= 0 ? "FOUND" :
        scan_status == COMPLETE ? "NOT FOUND" : scan_status == FAILED ?
        "SCAN FAILED" : "NOT SCANNED");
    cJSON_AddBoolToObject(doc, "ssid_found", best >= 0);
    cJSON_AddBoolToObject(doc, "wifi_connected", false);
    cJSON_AddBoolToObject(doc, "mqtt_connected", false);
    cJSON_AddBoolToObject(doc, "credentials_verified", false);
    if (best >= 0) {
        cJSON_AddNumberToObject(doc, "rssi", records[best].rssi);
        cJSON_AddNumberToObject(doc, "channel", records[best].primary);
        cJSON_AddStringToObject(doc, "security", security(records[best].authmode));
    }
    return json_reply(req, doc);
}
/* A blank password preserves the saved value; an explicit Open choice clears it. */
static esp_err_t save_user(httpd_req_t *req)
{
    if (!authorize(req)) return ESP_OK;
    cJSON *doc = request_json(req);
    const char *ssid = field(doc, "ssid"), *password = field(doc, "password");
    bool open = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(doc, "open_network"));
    if (!ssid || !ssid[0] || strlen(ssid) > 32 ||
        !strcmp(ssid, ADMIN_WIFI_SSID) || (password && strlen(password) > 64) ||
        (!open && password && password[0] && strlen(password) < 8)) {
        cJSON_Delete(doc);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid User Wi-Fi");
    }
    // Restore RAM values if NVS rejects the update.
    char old_ssid[33], old_password[65];
    memcpy(old_ssid, wifi_ssid, sizeof(old_ssid));
    memcpy(old_password, wifi_password, sizeof(old_password));
    snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", ssid);
    if (open) wifi_password[0] = 0;
    else if (password && password[0])
        snprintf(wifi_password, sizeof(wifi_password), "%s", password);
    cJSON_Delete(doc);
    if (config_save() != ESP_OK) {
        memcpy(wifi_ssid, old_ssid, sizeof(old_ssid));
        memcpy(wifi_password, old_password, sizeof(old_password));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
    }
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "saved", true);
    return json_reply(req, reply);
}
static bool valid_host(const char *text)
{
    size_t len = text ? strlen(text) : 0;
    if (!len || len >= sizeof(mqtt_config.host)) return false;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)text[i]) && text[i] != '.' && text[i] != '-') return false;
    return true;
}
/* Persist a versioned MQTT record before making it available to the live client. */
static esp_err_t save_mqtt(httpd_req_t *req)
{
    if (!authorize(req)) return ESP_OK;
    cJSON *doc = request_json(req);
    const char *host = field(doc, "host"), *client = field(doc, "client_id");
    const char *user = field(doc, "username"), *password = field(doc, "password");
    const char *prefix = field(doc, "topic_prefix");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(doc, "port");
    const cJSON *tls = cJSON_GetObjectItemCaseSensitive(doc, "tls");
    if (!valid_host(host) || !cJSON_IsNumber(port) || port->valuedouble < 1 ||
        port->valuedouble > 65535 || port->valuedouble != port->valueint ||
        !client || strlen(client) >= sizeof(mqtt_config.client_id) ||
        !user || strlen(user) >= sizeof(mqtt_config.username) ||
        !prefix || strlen(prefix) >= sizeof(mqtt_config.topic_prefix) ||
        (password && strlen(password) >= sizeof(mqtt_config.password)) ||
        !cJSON_IsBool(tls) || prefix[0] == '/' ||
        (prefix[0] && prefix[strlen(prefix) - 1] == '/') || strstr(prefix, "//")) {
        cJSON_Delete(doc);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid MQTT settings");
    }
    for (const char *p = prefix; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '/' && *p != '_' && *p != '-') {
            cJSON_Delete(doc);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid topic prefix");
        }
    }
    mqtt_store_t saved = {.version = 1, .value = mqtt_config};
    snprintf(saved.value.host, sizeof(saved.value.host), "%s", host);
    saved.value.port = port->valueint;
    snprintf(saved.value.client_id, sizeof(saved.value.client_id), "%s", client);
    snprintf(saved.value.username, sizeof(saved.value.username), "%s", user);
    snprintf(saved.value.topic_prefix, sizeof(saved.value.topic_prefix), "%s", prefix);
    saved.value.tls = cJSON_IsTrue(tls);
    if (password && password[0])
        snprintf(saved.value.password, sizeof(saved.value.password), "%s", password);
    cJSON_Delete(doc);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("appcfg", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "mqtt_v1", &saved, sizeof(saved));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
    mqtt_config = saved.value;
    mqtt_dirty = true;
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "saved", true);
    return json_reply(req, reply);
}
static esp_err_t save_name(httpd_req_t *req)
{
    if (!authorize(req)) return ESP_OK;
    cJSON *doc = request_json(req);
    const char *name = field(doc, "name");
    if (!name || !name[0] || strlen(name) >= sizeof(device_name)) {
        cJSON_Delete(doc);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
    }
    char old[sizeof(device_name)];
    memcpy(old, device_name, sizeof(old));
    snprintf(device_name, sizeof(device_name), "%s", name);
    cJSON_Delete(doc);
    if (config_save() != ESP_OK) {
        memcpy(device_name, old, sizeof(old));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
    }
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "saved", true);
    return json_reply(req, reply);
}
/* Queue the switch after acknowledging the request to the Admin browser. */
static esp_err_t force_user(httpd_req_t *req)
{
    if (!authorize(req)) return ESP_OK;
    cJSON *doc = request_json(req);
    const char *action = field(doc, "action");
    bool valid = action && !strcmp(action, "force_user_wifi");
    cJSON_Delete(doc);
    if (!valid || !wifi_ssid[0])
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Configure User Wi-Fi first");
    // Leave time for the HTTP acknowledgement to reach the browser.
    atomic_store(&force_at, app_millis() + 700);
    if (atomic_exchange(&force_command, true)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Switch already pending");
    }
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "status", "accepted");
    cJSON_AddStringToObject(reply, "message",
        "Admin page will disconnect. Reset device to restore Admin priority.");
    return json_reply(req, reply);
}
static esp_err_t dispatch(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        if (!strcmp(req->uri, "/")) return page(req);
        if (!strcmp(req->uri, "/api/device/status")) return status_api(req);
        if (!strcmp(req->uri, "/api/wifi/config")) return config_api(req);
        if (!strcmp(req->uri, "/api/wifi/scan")) return scan_api(req, true);
        if (!strcmp(req->uri, "/api/wifi/scan-results")) return scan_api(req, false);
        if (!strcmp(req->uri, "/api/wifi/user-availability")) return availability_api(req);
    } else {
        if (!strcmp(req->uri, "/api/device/name")) return save_name(req);
        if (!strcmp(req->uri, "/api/wifi/user")) return save_user(req);
        if (!strcmp(req->uri, "/api/mqtt/config")) return save_mqtt(req);
        if (!strcmp(req->uri, "/api/wifi/force-user")) return force_user(req);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}
/* Retry startup with backoff while Admin remains connected. */
static void start_http(void)
{
    if (http || app_millis() < http_retry_at) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&http, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Admin HTTP start failed");
        http_retry_at = app_millis() + 3000;
        return;
    }
    httpd_uri_t get = {.uri = "/*", .method = HTTP_GET, .handler = dispatch};
    httpd_uri_t post = {.uri = "/*", .method = HTTP_POST, .handler = dispatch};
    if (httpd_register_uri_handler(http, &get) != ESP_OK ||
        httpd_register_uri_handler(http, &post) != ESP_OK) {
        httpd_stop(http);
        http = NULL;
        http_retry_at = app_millis() + 3000;
        ESP_LOGE(TAG, "Admin HTTP routes failed");
    }
}
/* Missing or incompatible NVS data keeps the legacy broker defaults. */
static void load_mqtt(void)
{
    snprintf(mqtt_config.host, sizeof(mqtt_config.host), "broker.emqx.io");
    mqtt_config.port = 1883;
    nvs_handle_t nvs;
    if (nvs_open("appcfg", NVS_READONLY, &nvs) != ESP_OK) return;
    mqtt_store_t saved;
    size_t size = sizeof(saved);
    if (nvs_get_blob(nvs, "mqtt_v1", &saved, &size) == ESP_OK &&
        size == sizeof(saved) && saved.version == 1 &&
        saved.value.host[sizeof(saved.value.host) - 1] == 0 &&
        valid_host(saved.value.host) && saved.value.port) {
        mqtt_config = saved.value;
        mqtt_config.client_id[sizeof(mqtt_config.client_id) - 1] = 0;
        mqtt_config.username[sizeof(mqtt_config.username) - 1] = 0;
        mqtt_config.password[sizeof(mqtt_config.password) - 1] = 0;
        mqtt_config.topic_prefix[sizeof(mqtt_config.topic_prefix) - 1] = 0;
    }
    nvs_close(nvs);
}
esp_err_t admin_wifi_init(void)
{
    forced_user_mode = false;
    if (strlen(CONFIG_ADMIN_WEB_PASSWORD) < 12) {
        ESP_LOGE(TAG, "Configure a webpage password of at least 12 characters");
        return ESP_ERR_INVALID_STATE;
    }
    load_mqtt();
    ESP_LOGI(TAG, "Boot; forced User Mode = FALSE");
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK) return err;
    sta = esp_netif_create_default_wifi_sta();
    if (!sta) return ESP_ERR_NO_MEM;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, event, NULL);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err == ESP_OK) search(true);
    return err;
}
/* State transitions run on app_main's periodic tick, never in the HTTP/event tasks. */
void admin_wifi_process(void)
{
    uint64_t now = app_millis();
    if (state == ONLINE_ADMIN && atomic_load(&force_command) &&
        now >= atomic_load(&force_at) && atomic_exchange(&force_command, false)) {
        forced_user_mode = true;
        user_online = false;
        if (http) { httpd_stop(http); http = NULL; }
        state = SWITCH_USER;
        retry_at = now + 500;
        ESP_LOGI(TAG, "Force User accepted; disconnecting Admin");
        esp_wifi_disconnect();
    }
    if (atomic_exchange(&scan_event, false)) finished_scan();
    if (atomic_exchange(&ip_event, false)) {
        uint32_t ip = atomic_load(&ip_value);
        if (state == CONNECT_ADMIN) {
            state = ONLINE_ADMIN;
            admin_ip = ip;
            ESP_LOGI(TAG, "Admin IP acquired: " IPSTR, IP2STR((esp_ip4_addr_t *)&ip));
            start_http();
        } else if (state == CONNECT_USER) {
            state = ONLINE_USER;
            user_online = true;
            gpio_set_level(WIFI_LED_PIN, 1);
            ESP_LOGI(TAG, "User IP acquired: " IPSTR, IP2STR((esp_ip4_addr_t *)&ip));
        }
    }
    if (atomic_exchange(&disconnect_event, false)) {
        state_t old = state;
        user_online = false;
        gpio_set_level(WIFI_LED_PIN, 0);
        if (http) { httpd_stop(http); http = NULL; }
        admin_ip = 0;
        if (old == SWITCH_USER) connect_profile(false);
        else if (old == ONLINE_ADMIN || old == ONLINE_USER ||
                 old == CONNECT_ADMIN || old == CONNECT_USER) {
            if (forced_user_mode) retry_user();
            else search(old != CONNECT_ADMIN);
        }
    }
    if (state == SWITCH_USER && now >= retry_at) connect_profile(false);
    if (state == RETRY_USER && now >= retry_at) connect_profile(false);
    if ((state == SEARCH_ADMIN || state == SEARCH_USER) && !scanning) {
        if (now >= deadline) search(state == SEARCH_USER);
        else if (now >= retry_at) begin_scan(SEARCH_SCAN);
    }
    if ((state == SEARCH_ADMIN || state == SEARCH_USER) && scanning &&
        now >= deadline && !timed_out) {
        // Let SCAN_DONE release the driver's result list before another scan.
        timed_out = true;
        esp_wifi_scan_stop();
    }
    if ((state == CONNECT_ADMIN || state == CONNECT_USER) && now >= deadline) {
        bool admin = state == CONNECT_ADMIN;
        ESP_LOGW(TAG, "%s connection timed out", admin ? "Admin" : "User");
        esp_wifi_disconnect();
        if (forced_user_mode) retry_user();
        else search(!admin);
    }
    if (state == ONLINE_ADMIN && atomic_exchange(&web_scan, false) && !scanning)
        begin_scan(WEB_SCAN);
    if (state == ONLINE_ADMIN && scanning && scan_kind == WEB_SCAN &&
        now >= deadline && !timed_out) {
        timed_out = true;
        esp_wifi_scan_stop();
    }
    if (state == ONLINE_ADMIN && !http) start_http();
}
bool admin_wifi_user_online(void) { return user_online; }
bool admin_wifi_mqtt_dirty(void) { return mqtt_dirty; }
void admin_wifi_mqtt_applied(void) { mqtt_dirty = false; }
const admin_mqtt_config_t *admin_wifi_mqtt_config(void) { return &mqtt_config; }
