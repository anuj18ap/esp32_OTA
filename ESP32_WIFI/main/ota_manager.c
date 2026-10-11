#include "ota_manager.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "ota_protocol.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define REPORT_COUNT 8
#define REPORT_CAPACITY 640
typedef struct
{
    uint32_t id;
    char json[REPORT_CAPACITY];
} report_t;
typedef struct
{
    uint32_t count;
    report_t items[REPORT_COUNT];
} journal_t;
typedef struct
{
    uint32_t session;
    char target[17], version[32];
    int transport;
    bool selected;
} boot_record_t;
static SemaphoreHandle_t mutex;
static ota_state_t state = OTA_IDLE;
static ota_transport_t owner;
static uint32_t token;
static esp_ota_handle_t handle;
static bool handle_open;
static const esp_partition_t *target;
static ota_metadata_t metadata;
static ota_stream_t stream;
static mbedtls_sha256_context sha;
static uint64_t started, last_activity, restart_at;
static journal_t journal;
static boot_record_t boot_record;
static boot_record_t receiving_record;
static bool startup_ready;

const char *ota_transport_name(ota_transport_t t)
{
    static const char *names[] = {"mqtt", "http", "ble", "espnow"};
    return t >= OTA_MQTT && t <= OTA_ESPNOW ? names[t] : "unknown";
}
static bool active(void)
{
    return state >= OTA_REQUEST_VALIDATION && state <= OTA_REBOOT_PENDING;
}
bool ota_manager_busy(void)
{
    if (!mutex)
    {
        return false;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool busy = active();
    xSemaphoreGive(mutex);
    return busy;
}
static esp_err_t store(const char *key, const void *data, size_t len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("otaresult", NVS_READWRITE, &nvs);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_blob(nvs, key, data, len);
    if (err == ESP_OK)
    {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}
static void load(const char *key, void *data, size_t len)
{
    nvs_handle_t nvs;
    if (nvs_open("otaresult", NVS_READONLY, &nvs) != ESP_OK)
    {
        return;
    }
    size_t actual = len;
    if (nvs_get_blob(nvs, key, data, &actual) != ESP_OK || actual != len)
    {
        memset(data, 0, len);
    }
    nvs_close(nvs);
}
static esp_err_t report_locked(ota_transport_t transport, uint32_t session, const char *status,
                               const char *code, const char *message, bool durable)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    cJSON *doc = cJSON_CreateObject();
    if (!doc)
    {
        return ESP_ERR_NO_MEM;
    }
    uint32_t id;
    do
    {
        id = esp_random();
    } while (!id);
    cJSON_AddStringToObject(doc, "device_id", device_id);
    cJSON_AddNumberToObject(doc, "report_id", id);
    cJSON_AddNumberToObject(doc, "session_id", session);
    cJSON_AddStringToObject(doc, "transport", ota_transport_name(transport));
    cJSON_AddStringToObject(doc, "firmware_version", FW_VERSION);
    cJSON_AddStringToObject(doc, "target_version",
                            metadata.version[0] ? metadata.version : boot_record.version);
    cJSON_AddStringToObject(doc, "current_slot", running ? running->label : "");
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    cJSON_AddStringToObject(doc, "boot_slot", boot ? boot->label : "");
    cJSON_AddStringToObject(doc, "target_slot", target ? target->label : boot_record.target);
    cJSON_AddStringToObject(doc, "status", status);
    cJSON_AddStringToObject(doc, "error_code", code);
    cJSON_AddStringToObject(doc, "message", message);
    cJSON_AddNumberToObject(doc, "bytes_received", stream.bytes);
    cJSON_AddNumberToObject(doc, "expected_size", metadata.size);
    char json[REPORT_CAPACITY];
    bool ok = cJSON_PrintPreallocated(doc, json, sizeof(json), false);
    cJSON_Delete(doc);
    if (!ok)
    {
        return ESP_ERR_NO_MEM;
    }
    if (!durable)
    {
        app_publish("ota/event", json, 0, false);
        return ESP_OK;
    }
    // Rejecting another request must leave room for the active session's final result.
    if (journal.count >= REPORT_COUNT ||
        (active() && session != token && journal.count >= REPORT_COUNT - 2))
    {
        app_publish("ota/event", json, 1, false);
        app_log("[OTA] Report journal capacity reached");
        return ESP_ERR_NO_MEM;
    }
    report_t *r = &journal.items[journal.count++];
    r->id = id;
    snprintf(r->json, sizeof(r->json), "%s", json);
    esp_err_t err = store("journal", &journal, sizeof(journal));
    if (err != ESP_OK)
    {
        journal.count--;
        app_log("[OTA] Result persistence failed: %s", esp_err_to_name(err));
    }
    return err;
}
static esp_err_t fail_locked(const char *code, const char *message, esp_err_t error)
{
    state = OTA_ABORTING;
    if (handle_open)
    {
        esp_err_t aborted = esp_ota_abort(handle);
        handle_open = false;
        if (aborted != ESP_OK)
        {
            app_log("[OTA] Abort cleanup: %s", esp_err_to_name(aborted));
        }
    }
    mbedtls_sha256_free(&sha);
    state = OTA_FAILED;
    report_locked(owner, token, "failed", code, message, true);
    memset(&receiving_record, 0, sizeof(receiving_record));
    esp_err_t cleared = store("receiving", &receiving_record, sizeof(receiving_record));
    if (cleared != ESP_OK)
    {
        app_log("[OTA] Cannot clear receive record: %s", esp_err_to_name(cleared));
    }
    app_publish("ota_status", "FAILED", 0, false);
    app_log("[OTA] %s: %s", code, message);
    return error;
}
esp_err_t ota_manager_init(void)
{
    mutex = xSemaphoreCreateMutex();
    if (!mutex)
    {
        return ESP_ERR_NO_MEM;
    }
    load("journal", &journal, sizeof(journal));
    if (journal.count > REPORT_COUNT)
    {
        memset(&journal, 0, sizeof(journal));
    }
    load("boot", &boot_record, sizeof(boot_record));
    load("receiving", &receiving_record, sizeof(receiving_record));
    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash_size);
    if (err != ESP_OK)
    {
        return err;
    }
    const esp_partition_t *a =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *b =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    if (!a || !b || a->address + a->size > flash_size || b->address + b->size > flash_size ||
        a->address + a->size > b->address)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    app_log("[OTA] Flash %lu bytes; slots %s=%lu %s=%lu", (unsigned long)flash_size, a->label,
            (unsigned long)a->size, b->label, (unsigned long)b->size);
    if (receiving_record.session && !boot_record.session)
    {
        err = report_locked(receiving_record.transport, receiving_record.session, "failed",
                            "TRANSFER_INTERRUPTED",
                            "Reset interrupted reception; boot selection unchanged", true);
        if (err == ESP_OK)
        {
            memset(&receiving_record, 0, sizeof(receiving_record));
            err = store("receiving", &receiving_record, sizeof(receiving_record));
        }
    }
    if (err != ESP_OK)
    {
        return err;
    }
    return ESP_OK;
}
static bool number_u32(const cJSON *doc, const char *name, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(doc, name);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble < 0 ||
        item->valuedouble > UINT32_MAX || floor(item->valuedouble) != item->valuedouble)
    {
        return false;
    }
    *out = (uint32_t)item->valuedouble;
    return true;
}
esp_err_t ota_metadata_parse(const cJSON *doc, ota_metadata_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!cJSON_IsObject(doc) || !number_u32(doc, "size", &out->size) ||
        !number_u32(doc, "chunks", &out->chunks) || !number_u32(doc, "crc32", &out->crc32) ||
        !out->size || !out->chunks || out->chunks > out->size)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(doc, "version"),
                *chip = cJSON_GetObjectItemCaseSensitive(doc, "chip"),
                *digest = cJSON_GetObjectItemCaseSensitive(doc, "sha256");
    if (chip && (!cJSON_IsString(chip) || strcmp(chip->valuestring, "esp32")))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (version)
    {
        if (!cJSON_IsString(version) || strlen(version->valuestring) >= sizeof(out->version))
        {
            return ESP_ERR_INVALID_ARG;
        }
        strcpy(out->version, version->valuestring);
    }
    if (digest)
    {
        if (!cJSON_IsString(digest) || strlen(digest->valuestring) != 64)
        {
            return ESP_ERR_INVALID_ARG;
        }
        for (int i = 0; i < 32; i++)
        {
            unsigned value;
            char hex[3] = {digest->valuestring[i * 2], digest->valuestring[i * 2 + 1], 0};
            if (strspn(hex, "0123456789abcdefABCDEF") != 2 || sscanf(hex, "%2x", &value) != 1)
            {
                return ESP_ERR_INVALID_ARG;
            }
            out->sha256[i] = value;
        }
        out->has_sha256 = true;
    }
    return ESP_OK;
}
esp_err_t ota_manager_begin(ota_transport_t transport, const ota_metadata_t *meta,
                            uint32_t *session)
{
    if (!mutex || !meta || !session || transport < OTA_MQTT || transport > OTA_ESPNOW ||
        !meta->size || !meta->chunks || meta->chunks > meta->size)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!startup_ready)
    {
        report_locked(transport, 0, "failed", "OTA_BUSY", "Startup verification has not completed",
                      true);
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (active())
    {
        report_locked(transport, 0, "failed", "OTA_BUSY", "Another OTA session owns flash", true);
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    owner = transport;
    do
    {
        token = esp_random();
    } while (!token);
    *session = token;
    metadata = *meta;
    state = OTA_REQUEST_VALIDATION;
    target = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *running = esp_ota_get_running_partition();
    ota_stream_init(&stream, meta->size, meta->chunks, meta->crc32);
    mbedtls_sha256_init(&sha);
    esp_err_t err = ESP_OK;
    if (!target || !running || target->address == running->address || meta->size > target->size)
    {
        err = fail_locked("OTA_BEGIN_FAILED", "Image does not fit an inactive OTA slot",
                          ESP_ERR_INVALID_SIZE);
    }
    else if (journal.count >= REPORT_COUNT - 2)
    {
        err = fail_locked("OTA_REPORT_STORAGE_FULL",
                          "Deliver pending results before another update", ESP_ERR_NO_MEM);
    }
    else
    {
        receiving_record = (boot_record_t){.session = token, .transport = owner};
        snprintf(receiving_record.target, sizeof(receiving_record.target), "%s", target->label);
        err = store("receiving", &receiving_record, sizeof(receiving_record));
        if (err == ESP_OK)
        {
            err = mbedtls_sha256_starts(&sha, 0) == 0 ? esp_ota_begin(target, meta->size, &handle)
                                                      : ESP_FAIL;
        }
        if (err != ESP_OK)
        {
            err = fail_locked("OTA_BEGIN_FAILED", esp_err_to_name(err), err);
        }
        else
        {
            handle_open = true;
            state = OTA_RECEIVING;
            started = last_activity = app_millis();
            report_locked(owner, token, "started", "", "Inactive slot ready", false);
            app_publish("ota_status", "UPDATING", 0, false);
        }
    }
    xSemaphoreGive(mutex);
    return err;
}
bool ota_manager_owned(ota_transport_t transport, uint32_t session)
{
    if (!mutex)
    {
        return false;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool result = active() && owner == transport && token == session;
    xSemaphoreGive(mutex);
    return result;
}
esp_err_t ota_manager_write(ota_transport_t transport, uint32_t session, uint32_t sequence,
                            const uint8_t *data, size_t len)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (state != OTA_RECEIVING || owner != transport || token != session)
    {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    stream_result_t check = ota_stream_check(&stream, sequence, data, len);
    esp_err_t err = ESP_OK;
    if (check == STREAM_DUPLICATE)
    { /* Re-ACK without writing or extending session lifetime. */
    }
    else if (check != STREAM_OK)
    {
        err = fail_locked(check == STREAM_SEQUENCE ? "CHUNK_SEQUENCE_ERROR" : "TRANSFER_INCOMPLETE",
                          "Invalid chunk sequence or length", ESP_ERR_INVALID_ARG);
    }
    else
    {
        err = esp_ota_write(handle, data, len);
        if (err != ESP_OK)
        {
            err = fail_locked("FLASH_WRITE_FAILED", esp_err_to_name(err), err);
        }
        else if (mbedtls_sha256_update(&sha, data, len) != 0)
        {
            err = fail_locked("IMAGE_VALIDATION_FAILED", "SHA256 update failed", ESP_FAIL);
        }
        else
        {
            ota_stream_commit(&stream, data, len);
            last_activity = app_millis();
            if (!(stream.next % 20) || stream.next == stream.chunks)
            {
                report_locked(owner, token, "in_progress", "", "Receiving firmware", false);
            }
        }
    }
    xSemaphoreGive(mutex);
    return err;
}
esp_err_t ota_manager_finish(ota_transport_t transport, uint32_t session)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (state != OTA_RECEIVING || owner != transport || token != session)
    {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    uint8_t digest[32];
    if (!ota_stream_complete(&stream))
    {
        err = fail_locked("TRANSFER_INCOMPLETE", "Size, chunk count or CRC32 mismatch",
                          ESP_ERR_INVALID_CRC);
    }
    else if (mbedtls_sha256_finish(&sha, digest) != 0 ||
             (metadata.has_sha256 && memcmp(digest, metadata.sha256, 32)))
    {
        err = fail_locked("IMAGE_VALIDATION_FAILED", "SHA256 mismatch", ESP_ERR_INVALID_CRC);
    }
    else
    {
        state = OTA_IMAGE_FINALIZATION;
        err = esp_ota_end(handle);
        handle_open = false;
        mbedtls_sha256_free(&sha);
        if (err != ESP_OK)
        {
#if CONFIG_SECURE_BOOT || CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT
            err = fail_locked("FIRMWARE_AUTHENTICATION_FAILED", esp_err_to_name(err), err);
#else
            err = fail_locked("IMAGE_VALIDATION_FAILED", esp_err_to_name(err), err);
#endif
        }
        else
        {
            state = OTA_IMAGE_VALIDATION;
            esp_app_desc_t desc;
            err = esp_ota_get_partition_description(target, &desc);
            if (err != ESP_OK || (metadata.version[0] && strcmp(metadata.version, desc.version)))
            {
                err = fail_locked("IMAGE_VALIDATION_FAILED",
                                  "Invalid application descriptor or version mismatch",
                                  ESP_ERR_INVALID_VERSION);
            }
            else
            {
                report_locked(owner, token, "validated", "", "Image validation succeeded", false);
                boot_record =
                    (boot_record_t){.session = token, .transport = owner, .selected = false};
                snprintf(boot_record.target, sizeof(boot_record.target), "%s", target->label);
                snprintf(boot_record.version, sizeof(boot_record.version), "%s", desc.version);
                // Persist selection intent before boot metadata changes, including across power
                // loss.
                err = store("boot", &boot_record, sizeof(boot_record));
                if (err != ESP_OK)
                {
                    err = fail_locked("OTA_REPORT_STORAGE_FAILED",
                                      "Cannot persist boot selection intent", err);
                }
                else
                {
                    state = OTA_BOOT_SELECTION;
                    err = esp_ota_set_boot_partition(target);
                    if (err != ESP_OK)
                    {
                        memset(&boot_record, 0, sizeof(boot_record));
                        esp_err_t cleared = store("boot", &boot_record, sizeof(boot_record));
                        if (cleared != ESP_OK)
                        {
                            app_log("[OTA] Cannot clear selection intent: %s",
                                    esp_err_to_name(cleared));
                        }
                        err = fail_locked("BOOT_PARTITION_FAILED", esp_err_to_name(err), err);
                    }
                    else
                    {
                        boot_record.selected = true;
                        if (store("boot", &boot_record, sizeof(boot_record)) != ESP_OK)
                        {
                            app_log("[OTA] Boot selected; retained intent will identify startup "
                                    "outcome");
                        }
                        // ESP-NOW sender retries every 600 ms: allow several END ACK retries.
                        state = OTA_REBOOT_PENDING;
                        restart_at = app_millis() + (owner == OTA_ESPNOW ? 6000 : 1000);
                        memset(&receiving_record, 0, sizeof(receiving_record));
                        if (store("receiving", &receiving_record, sizeof(receiving_record)) !=
                            ESP_OK)
                        {
                            app_log("[OTA] Receive record cleanup deferred to next boot");
                        }
                        report_locked(owner, token, "boot_selected", "",
                                      "Validated image selected; awaiting startup verification",
                                      true);
                        app_publish("ota_status", "SUCCESS", 0, false);
                    }
                }
            }
        }
    }
    xSemaphoreGive(mutex);
    return err;
}
esp_err_t ota_manager_abort(ota_transport_t transport, uint32_t session, const char *code,
                            const char *message)
{
    if (!mutex)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    // No cancellation or ordinary transfer failure can reverse a completed boot selection.
    if (state != OTA_RECEIVING || owner != transport || token != session)
    {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    fail_locked(code, message, ESP_FAIL);
    xSemaphoreGive(mutex);
    return ESP_OK;
}
void ota_report_external(ota_transport_t transport, uint32_t session, const char *code,
                         const char *message)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    report_locked(transport, session, "failed", code, message, true);
    xSemaphoreGive(mutex);
}
void ota_manager_snapshot(void)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    report_locked(owner, token, "snapshot", "", "Current application and boot-selection state",
                  false);
    xSemaphoreGive(mutex);
}
void ota_manager_tick(void)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    uint64_t now = app_millis();
    if (state == OTA_RECEIVING && (now - last_activity > 30000 || now - started > 900000))
    {
        fail_locked("TRANSFER_TIMEOUT", "OTA session expired", ESP_ERR_TIMEOUT);
    }
    bool restart = state == OTA_REBOOT_PENDING && now >= restart_at;
    if (restart)
    {
        report_locked(owner, token, "rebooting", "", "Restarting into validated image", false);
        restart_at = UINT64_MAX;
    }
    xSemaphoreGive(mutex);
    if (restart)
    {
        app_request_restart(0);
    }
}
esp_err_t ota_manager_verify_startup(bool healthy)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t image_state;
    if (!running)
    {
        return ESP_FAIL;
    }
    esp_err_t err = esp_ota_get_state_partition(running, &image_state);
    if (err == ESP_ERR_NOT_FOUND)
    {
        image_state = ESP_OTA_IMG_UNDEFINED;
        err = ESP_OK;
    }
    if (err != ESP_OK)
    {
        return err;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (image_state == ESP_OTA_IMG_PENDING_VERIFY)
    {
        state = OTA_VERIFYING;
        if (!healthy)
        {
            report_locked(boot_record.transport, boot_record.session, "failed",
                          "STARTUP_SELF_TEST_FAILED", "Requesting rollback", true);
            xSemaphoreGive(mutex);
            return esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK)
        {
            state = OTA_CONFIRMED;
            err = report_locked(boot_record.transport, boot_record.session, "confirmed", "",
                                "Critical startup checks passed", true);
        }
    }
    else if (boot_record.session && !strcmp(boot_record.target, running->label) &&
             image_state == ESP_OTA_IMG_VALID)
    {
        // A prior confirmation succeeded but its report persistence/reboot cleanup may have failed.
        state = OTA_CONFIRMED;
        err = report_locked(boot_record.transport, boot_record.session, "confirmed", "",
                            "Previously confirmed image resumed", true);
    }
    else if (boot_record.session && strcmp(boot_record.target, running->label))
    {
        const esp_partition_t *attempt = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, boot_record.target);
        esp_ota_img_states_t attempt_state;
        if (attempt && esp_ota_get_state_partition(attempt, &attempt_state) == ESP_OK &&
            (attempt_state == ESP_OTA_IMG_INVALID || attempt_state == ESP_OTA_IMG_ABORTED))
        {
            err = report_locked(boot_record.transport, boot_record.session, "rolled_back",
                                "STARTUP_ROLLBACK",
                                "Previous slot running after unconfirmed update", true);
        }
        else
        {
            err = report_locked(boot_record.transport, boot_record.session, "failed",
                                "BOOT_SELECTION_INTERRUPTED",
                                "Selection intent did not result in target startup", true);
        }
    }
    if (err == ESP_OK && boot_record.session)
    {
        boot_record_t previous = boot_record;
        memset(&boot_record, 0, sizeof(boot_record));
        err = store("boot", &boot_record, sizeof(boot_record));
        if (err != ESP_OK)
        {
            boot_record = previous;
        }
    }
    if (err == ESP_OK)
    {
        startup_ready = true;
        if (receiving_record.session)
        {
            memset(&receiving_record, 0, sizeof(receiving_record));
            err = store("receiving", &receiving_record, sizeof(receiving_record));
        }
    }
    xSemaphoreGive(mutex);
    return err;
}
bool ota_report_peek(char *json, size_t capacity, uint32_t *id)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool ready = journal.count > 0;
    if (ready)
    {
        snprintf(json, capacity, "%s", journal.items[0].json);
        *id = journal.items[0].id;
    }
    xSemaphoreGive(mutex);
    return ready;
}
void ota_report_delivered(uint32_t id)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (journal.count && journal.items[0].id == id)
    {
        journal_t previous = journal;
        memmove(journal.items, journal.items + 1, (journal.count - 1) * sizeof(report_t));
        journal.count--;
        if (store("journal", &journal, sizeof(journal)) != ESP_OK)
        {
            journal = previous;
        }
    }
    xSemaphoreGive(mutex);
}
