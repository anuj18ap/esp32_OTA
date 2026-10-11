#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_transports.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    char url[512];
    ota_metadata_t meta;
    uint32_t session;
} download_t;
static void download_task(void *arg)
{
    download_t *job = arg;
    esp_http_client_config_t config = {.url = job->url,
                                       .timeout_ms = 10000,
                                       .crt_bundle_attach = esp_crt_bundle_attach,
                                       .disable_auto_redirect = true,
                                       .buffer_size = 1024};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    bool success = false;
    const char *error = "HTTP_DOWNLOAD_FAILED", *message = "HTTP client initialization failed";
    if (client)
    {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err == ESP_OK)
        {
            int64_t length = esp_http_client_fetch_headers(client);
            if (length < 0)
            {
                message = "Cannot read response headers";
            }
            else if (esp_http_client_get_status_code(client) != 200)
            {
                error = "HTTP_STATUS_ERROR";
                message = "Expected HTTP 200 (redirects rejected)";
            }
            else if (length > 0 && length != job->meta.size)
            {
                error = "TRANSFER_INCOMPLETE";
                message = "Content-Length differs from metadata";
            }
            else
            {
                uint8_t buffer[1024];
                uint32_t index = 0, total = 0;
                while (total < job->meta.size && ota_manager_owned(OTA_HTTP, job->session))
                {
                    size_t expected = job->meta.size - total;
                    if (expected > sizeof(buffer))
                    {
                        expected = sizeof(buffer);
                    }
                    size_t received = 0;
                    while (received < expected)
                    {
                        int got = esp_http_client_read(client, (char *)buffer + received,
                                                       expected - received);
                        if (got <= 0)
                        {
                            break;
                        }
                        received += got;
                    }
                    if (received != expected)
                    {
                        error = "TRANSFER_INCOMPLETE";
                        message = "Download ended before expected size";
                        break;
                    }
                    if (ota_manager_write(OTA_HTTP, job->session, index++, buffer, received) !=
                        ESP_OK)
                    {
                        break;
                    }
                    total += received;
                }
                if (total == job->meta.size)
                {
                    char extra;
                    int n = esp_http_client_read(client, &extra, 1);
                    if (n != 0 || !esp_http_client_is_complete_data_received(client))
                    {
                        error = "TRANSFER_INCOMPLETE";
                        message = "HTTP body length or termination mismatch";
                    }
                    else
                    {
                        success = ota_manager_finish(OTA_HTTP, job->session) == ESP_OK;
                    }
                }
            }
        }
        else
        {
            message = "HTTP connection failed";
        }
        esp_err_t closed = esp_http_client_close(client), cleaned = esp_http_client_cleanup(client);
        if (closed != ESP_OK || cleaned != ESP_OK)
        {
            app_log("[HTTP] Connection cleanup failed");
        }
    }
    if (!success)
    {
        ota_manager_abort(OTA_HTTP, job->session, error, message);
    }
    free(job);
    vTaskDelete(NULL);
}
esp_err_t ota_http_start(const char *url, const ota_metadata_t *meta)
{
    if (!url || !meta || !meta->size || strlen(url) >= 512 || strchr(url, '\r') ||
        strchr(url, '\n'))
    {
        return ESP_ERR_INVALID_ARG;
    }
    bool https = !strncmp(url, "https://", 8), http = !strncmp(url, "http://", 7);
#ifndef CONFIG_OTA_ALLOW_DEV_HTTP
    if (!https)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
#else
    if (!https && !http)
    {
        return ESP_ERR_INVALID_ARG;
    }
#endif
    (void)http;
    const char *host = url + (https ? 8 : 7);
    const char *path = strchr(host, '/');
    size_t host_len = path ? (size_t)(path - host) : strlen(host);
    if (!host_len || memchr(host, '@', host_len))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!app_wifi_connected())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (meta->chunks != (meta->size - 1) / 1024 + 1)
    {
        return ESP_ERR_INVALID_ARG;
    }
    download_t *job = calloc(1, sizeof(*job));
    if (!job)
    {
        return ESP_ERR_NO_MEM;
    }
    snprintf(job->url, sizeof(job->url), "%s", url);
    job->meta = *meta;
    esp_err_t err = ota_manager_begin(OTA_HTTP, meta, &job->session);
    if (err == ESP_OK && xTaskCreate(download_task, "ota_http", 6144, job, 4, NULL) != pdPASS)
    {
        ota_manager_abort(OTA_HTTP, job->session, "OTA_BEGIN_FAILED",
                          "Cannot create download task");
        err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK)
    {
        free(job);
    }
    return err;
}
