/* Compile the actual firmware manager in this translation unit; only IDF services are simulated. */
#include "../main/ota_manager.c"
#include "native_stubs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

char device_id[13] = "0CF73625BF58", device_name[128], wifi_ssid[33], wifi_password[65];
static esp_partition_t slots[2] = {{0x10000, 0x1e0000, "ota_0"}, {0x1f0000, 0x1e0000, "ota_1"}};
static int running_slot, boot_slot, selected, writes, aborted, restarts, validations, rollbacks,
    locked;
static int fault_begin, fault_write, fault_end, fault_select, fault_nvs, fault_sha;
static const char *fault_nvs_key;
static uint64_t now_ms;
static esp_ota_img_states_t image_states[2];
static uint8_t written[64];
static size_t written_size;
static struct
{
    char key[32];
    uint8_t data[8192];
    size_t size;
} saved[4];
uint64_t app_millis(void)
{
    return now_ms;
}
void app_log(const char *fmt, ...)
{
    (void)fmt;
}
bool app_publish(const char *suffix, const char *text, int qos, bool retain)
{
    (void)suffix;
    (void)text;
    (void)qos;
    (void)retain;
    return true;
}
void app_request_restart(unsigned delay)
{
    (void)delay;
    restarts++;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return (void *)1;
}
int xSemaphoreTake(SemaphoreHandle_t m, unsigned t)
{
    (void)m;
    (void)t;
    assert(!locked);
    locked = 1;
    return 1;
}
int xSemaphoreGive(SemaphoreHandle_t m)
{
    (void)m;
    assert(locked);
    locked = 0;
    return 1;
}
const esp_partition_t *esp_ota_get_running_partition(void)
{
    return &slots[running_slot];
}
const esp_partition_t *esp_ota_get_boot_partition(void)
{
    return &slots[boot_slot];
}
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p)
{
    (void)p;
    return &slots[1 - running_slot];
}
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    (void)type;
    for (int i = 0; i < 2; i++)
    {
        if ((subtype == 255 || subtype == 16 + i) && (!label || !strcmp(label, slots[i].label)))
        {
            return &slots[i];
        }
    }
    return NULL;
}
esp_err_t esp_flash_get_size(void *chip, uint32_t *size)
{
    (void)chip;
    *size = 4 * 1024 * 1024;
    return ESP_OK;
}
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t size, esp_ota_handle_t *h)
{
    assert(p != &slots[running_slot]);
    assert(size <= p->size);
    *h = 1;
    written_size = 0;
    return fault_begin ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *data, size_t size)
{
    assert(h == 1);
    if (fault_write)
    {
        return ESP_FAIL;
    }
    assert(written_size + size <= sizeof(written));
    memcpy(written + written_size, data, size);
    written_size += size;
    writes++;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t h)
{
    assert(h == 1);
    validations++;
    return fault_end ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t h)
{
    assert(h == 1);
    aborted++;
    return ESP_OK;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{
    assert(validations);
    if (fault_select)
    {
        return ESP_FAIL;
    }
    boot_slot = (p == &slots[1]);
    selected++;
    return ESP_OK;
}
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *desc)
{
    (void)p;
    strcpy(desc->version, "0.6");
    return ESP_OK;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *s)
{
    *s = image_states[p == &slots[1]];
    return ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    image_states[running_slot] = ESP_OTA_IMG_VALID;
    return ESP_OK;
}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void)
{
    rollbacks++;
    return ESP_FAIL;
}
const char *esp_err_to_name(esp_err_t e)
{
    (void)e;
    return "mock-error";
}
uint32_t esp_random(void)
{
    static uint32_t r = 100;
    return ++r;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h)
{
    (void)name;
    *h = 1;
    return fault_nvs && mode == NVS_READWRITE ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t size)
{
    (void)h;
    if (fault_nvs_key && !strcmp(fault_nvs_key, key))
    {
        return ESP_FAIL;
    }
    for (int i = 0; i < 4; i++)
    {
        if (!saved[i].key[0] || !strcmp(saved[i].key, key))
        {
            assert(size <= sizeof(saved[i].data));
            strcpy(saved[i].key, key);
            memcpy(saved[i].data, data, size);
            saved[i].size = size;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *size)
{
    (void)h;
    for (int i = 0; i < 4; i++)
    {
        if (!strcmp(saved[i].key, key))
        {
            if (*size < saved[i].size)
            {
                return ESP_ERR_INVALID_SIZE;
            }
            *size = saved[i].size;
            memcpy(data, saved[i].data, *size);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h)
{
    (void)h;
}
void mbedtls_sha256_init(mbedtls_sha256_context *c)
{
    c->initialized = 1;
}
void mbedtls_sha256_free(mbedtls_sha256_context *c)
{
    c->initialized = 0;
}
int mbedtls_sha256_starts(mbedtls_sha256_context *c, int n)
{
    (void)c;
    (void)n;
    return 0;
}
int mbedtls_sha256_update(mbedtls_sha256_context *c, const uint8_t *p, size_t n)
{
    (void)c;
    (void)p;
    (void)n;
    return fault_sha ? -1 : 0;
}
int mbedtls_sha256_finish(mbedtls_sha256_context *c, uint8_t *p)
{
    (void)c;
    memset(p, 0, 32);
    return 0;
}

static const uint8_t image[] = {1, 2, 3, 4, 5, 6};
static ota_metadata_t meta(void)
{
    ota_metadata_t m = {.size = sizeof(image), .chunks = 2};
    m.crc32 = ota_crc32(UINT32_MAX, image, sizeof(image)) ^ UINT32_MAX;
    return m;
}
static void unchanged(void)
{
    assert(boot_slot == running_slot);
    assert(!selected);
    assert(!restarts);
    assert(!handle_open);
    assert(!ota_manager_busy());
    assert(!locked);
    assert(journal.count);
    assert(strstr(journal.items[journal.count - 1].json, "failed"));
}
static void receive(uint32_t s)
{
    assert(ota_manager_write(OTA_MQTT, s, 0, image, 3) == ESP_OK);
    assert(ota_manager_write(OTA_MQTT, s, 1, image + 3, 3) == ESP_OK);
}
int main(int argc, char **argv)
{
    assert(argc >= 2);
    const char *test = argv[1];
    if (!strcmp(test, "crypto_wire"))
    {
        assert(argc == 3);
        char *plain = NULL;
        assert(crypto_decrypt((const uint8_t *)argv[2], strlen(argv[2]), &plain));
        assert(!strcmp(plain, "Arduino MQTT compatibility"));
        free(plain);
        char *encrypted = crypto_encrypt("Arduino MQTT compatibility");
        assert(encrypted);
        puts(encrypted);
        free(encrypted);
        assert(!crypto_decrypt((const uint8_t *)"ENC1:AA==", 9, &plain));
        return 0;
    }
    if (!strcmp(test, "relay_parser"))
    {
        uint8_t mask;
        assert(parse_relay_mask((const uint8_t *)" 0x80 ", 6, &mask) && mask == 128);
        const uint8_t raw[] = {0, 0, 0, 255};
        assert(parse_relay_mask(raw, 4, &mask) && mask == 255);
        assert(!parse_relay_mask((const uint8_t *)"0x100", 5, &mask));
        assert(!parse_relay_mask((const uint8_t *)"bad!", 4, &mask));
        return 0;
    }
    if (!strcmp(test, "metadata_parse"))
    {
        const char *bad[] = {"{}", "{\"size\":-1,\"chunks\":1,\"crc32\":0}",
                             "{\"size\":6,\"chunks\":1.5,\"crc32\":0}",
                             "{\"size\":6,\"chunks\":2,\"crc32\":0,\"chip\":\"esp32s3\"}"};
        ota_metadata_t m;
        for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        {
            cJSON *d = cJSON_Parse(bad[i]);
            assert(ota_metadata_parse(d, &m) != ESP_OK);
            cJSON_Delete(d);
        }
        return 0;
    }
    assert(ota_manager_init() == ESP_OK);
    if (!strcmp(test, "startup_busy"))
    {
        uint32_t s;
        ota_metadata_t m = meta();
        assert(ota_manager_begin(OTA_MQTT, &m, &s) != ESP_OK);
        assert(!selected);
        return 0;
    }
    assert(ota_manager_verify_startup(true) == ESP_OK);
    if (!strcmp(test, "selection_intent"))
    {
        boot_record = (boot_record_t){.session = 123, .transport = OTA_MQTT};
        strcpy(boot_record.target, "ota_1");
        assert(ota_manager_verify_startup(true) == ESP_OK);
        assert(strstr(journal.items[0].json, "BOOT_SELECTION_INTERRUPTED"));
        assert(!selected);
        return 0;
    }
    ota_metadata_t m = meta();
    uint32_t s;
    if (!strcmp(test, "oversize"))
    {
        m.size = slots[1].size + 1;
        assert(ota_manager_begin(OTA_MQTT, &m, &s) != ESP_OK);
        unchanged();
        return 0;
    }
    if (!strcmp(test, "begin_failure"))
    {
        fault_begin = 1;
    }
    if (!strcmp(test, "persist_failure"))
    {
        fault_nvs = 1;
    }
    esp_err_t begun = ota_manager_begin(OTA_MQTT, &m, &s);
    if (!strcmp(test, "begin_failure") || !strcmp(test, "persist_failure"))
    {
        assert(begun != ESP_OK);
        assert(!selected && !handle_open);
        return 0;
    }
    assert(begun == ESP_OK);
    if (!strcmp(test, "interrupted_report_persistence"))
    {
        handle_open = false;
        state = OTA_IDLE;
        fault_nvs = 1;
        assert(ota_manager_init() != ESP_OK);
        assert(receiving_record.session == s);
        assert(!journal.count && !selected);
        fault_nvs = 0;
        assert(ota_manager_init() == ESP_OK);
        assert(!receiving_record.session);
        assert(strstr(journal.items[0].json, "TRANSFER_INTERRUPTED"));
        return 0;
    }
    if (!strcmp(test, "rollback_report_persistence") || !strcmp(test, "boot_cleanup_persistence"))
    {
        receive(s);
        assert(ota_manager_finish(OTA_MQTT, s) == ESP_OK);
        if (!strcmp(test, "rollback_report_persistence"))
        {
            image_states[1] = ESP_OTA_IMG_ABORTED;
            boot_slot = running_slot;
            fault_nvs = 1;
            assert(ota_manager_verify_startup(true) != ESP_OK);
            assert(boot_record.session == s);
            fault_nvs = 0;
            assert(ota_manager_verify_startup(true) == ESP_OK);
            assert(strstr(journal.items[journal.count - 1].json, "rolled_back"));
        }
        else
        {
            running_slot = 1;
            image_states[1] = ESP_OTA_IMG_PENDING_VERIFY;
            fault_nvs_key = "boot";
            assert(ota_manager_verify_startup(true) != ESP_OK);
            assert(boot_record.session == s);
            fault_nvs_key = NULL;
            assert(ota_manager_verify_startup(true) == ESP_OK);
            assert(strstr(journal.items[journal.count - 1].json,
                          "Previously confirmed image resumed"));
        }
        assert(!boot_record.session && !locked);
        return 0;
    }
    if (!strcmp(test, "report_replay"))
    {
        assert(ota_manager_abort(OTA_MQTT, s, "OTA_ABORTED", "test") == ESP_OK);
        char json[640];
        uint32_t id;
        assert(ota_report_peek(json, sizeof(json), &id));
        memset(&journal, 0, sizeof(journal));
        assert(ota_manager_init() == ESP_OK);
        uint32_t restored;
        assert(ota_report_peek(json, sizeof(json), &restored) && restored == id);
        ota_report_delivered(id + 1);
        assert(journal.count == 1);
        ota_report_delivered(id);
        assert(journal.count == 0);
        assert(ota_manager_init() == ESP_OK);
        assert(!ota_report_peek(json, sizeof(json), &id));
        return 0;
    }
    if (!strcmp(test, "busy"))
    {
        uint32_t other;
        assert(ota_manager_begin(OTA_BLE, &m, &other) != ESP_OK);
        assert(ota_manager_owned(OTA_MQTT, s));
        assert(ota_manager_write(OTA_HTTP, s, 0, image, 3) != ESP_OK);
        assert(!writes);
        ota_manager_abort(OTA_MQTT, s, "OTA_ABORTED", "test");
        unchanged();
    }
    else if (!strcmp(test, "journal_pressure"))
    {
        for (int i = 0; i < 20; i++)
        {
            ota_report_external(OTA_BLE, 0, "OTA_BUSY", "test");
        }
        assert(journal.count <= REPORT_COUNT - 2);
        ota_manager_abort(OTA_MQTT, s, "OTA_ABORTED", "test");
        unchanged();
    }
    else if (!strcmp(test, "sequence"))
    {
        assert(ota_manager_write(OTA_MQTT, s, 1, image, 3) != ESP_OK);
        unchanged();
    }
    else if (!strcmp(test, "length"))
    {
        assert(ota_manager_write(OTA_MQTT, s, 0, image, 7) != ESP_OK);
        unchanged();
    }
    else if (!strcmp(test, "write_failure"))
    {
        fault_write = 1;
        assert(ota_manager_write(OTA_MQTT, s, 0, image, 3) != ESP_OK);
        assert(aborted == 1);
        unchanged();
    }
    else if (!strcmp(test, "hash_failure"))
    {
        fault_sha = 1;
        assert(ota_manager_write(OTA_MQTT, s, 0, image, 3) != ESP_OK);
        unchanged();
    }
    else if (!strcmp(test, "timeout"))
    {
        now_ms = 30001;
        ota_manager_tick();
        unchanged();
    }
    else if (!strcmp(test, "expiration"))
    {
        now_ms = 900001;
        last_activity = now_ms;
        ota_manager_tick();
        unchanged();
    }
    else if (!strcmp(test, "cancel") || !strcmp(test, "ble_disconnect") ||
             !strcmp(test, "mqtt_disconnect"))
    {
        assert(ota_manager_abort(OTA_MQTT, s, "OTA_ABORTED", test) == ESP_OK);
        unchanged();
    }
    else if (!strcmp(test, "incomplete"))
    {
        assert(ota_manager_write(OTA_MQTT, s, 0, image, 3) == ESP_OK);
        assert(ota_manager_finish(OTA_MQTT, s) != ESP_OK);
        unchanged();
    }
    else if (!strcmp(test, "crc"))
    {
        uint8_t bad[] = {0, 2, 3};
        assert(ota_manager_write(OTA_MQTT, s, 0, bad, 3) == ESP_OK);
        assert(ota_manager_write(OTA_MQTT, s, 1, image + 3, 3) == ESP_OK);
        assert(ota_manager_finish(OTA_MQTT, s) != ESP_OK);
        assert(!validations);
        unchanged();
    }
    else if (!strcmp(test, "duplicate_changed"))
    {
        assert(ota_manager_write(OTA_MQTT, s, 0, image, 3) == ESP_OK);
        uint8_t bad[] = {0, 2, 3};
        assert(ota_manager_write(OTA_MQTT, s, 0, bad, 3) != ESP_OK);
        unchanged();
    }
    else if (!strcmp(test, "reset_receiving"))
    {
        handle_open = false;
        state = OTA_IDLE;
        memset(&receiving_record, 0, sizeof(receiving_record));
        assert(ota_manager_init() == ESP_OK);
        assert(strstr(journal.items[journal.count - 1].json, "TRANSFER_INTERRUPTED"));
        unchanged();
    }
    else
    {
        if (!strcmp(test, "duplicate"))
        {
            assert(ota_manager_write(OTA_MQTT, s, 0, image, 3) == ESP_OK);
            assert(ota_manager_write(OTA_MQTT, s, 0, image, 3) == ESP_OK);
            assert(writes == 1);
            assert(ota_manager_write(OTA_MQTT, s, 1, image + 3, 3) == ESP_OK);
        }
        else
        {
            receive(s);
        }
        if (!strcmp(test, "image_invalid"))
        {
            fault_end = 1;
        }
        if (!strcmp(test, "boot_failure"))
        {
            fault_select = 1;
        }
        if (!strcmp(test, "version"))
        {
            strcpy(metadata.version, "wrong");
        }
        if (!strcmp(test, "sha_mismatch"))
        {
            metadata.has_sha256 = true;
            metadata.sha256[0] = 1;
        }
        esp_err_t result = ota_manager_finish(OTA_MQTT, s);
        if (fault_end || fault_select || !strcmp(test, "version") || !strcmp(test, "sha_mismatch"))
        {
            assert(result != ESP_OK);
            unchanged();
        }
        else
        {
            assert(result == ESP_OK && selected == 1 && boot_slot == 1 && running_slot == 0);
            assert(ota_manager_abort(OTA_MQTT, s, "OTA_ABORTED", "after selection") != ESP_OK);
            assert(boot_slot == 1);
            now_ms = 2000;
            ota_manager_tick();
            assert(restarts == 1);
            running_slot = 1;
            image_states[1] = ESP_OTA_IMG_PENDING_VERIFY;
            if (!strcmp(test, "rollback"))
            {
                assert(ota_manager_verify_startup(false) != ESP_OK);
                assert(rollbacks == 1);
            }
            else
            {
                assert(ota_manager_verify_startup(true) == ESP_OK);
                assert(state == OTA_CONFIRMED);
                assert(strstr(journal.items[journal.count - 1].json, "confirmed"));
                if (!strcmp(test, "repeat_ab"))
                {
                    assert(ota_manager_begin(OTA_MQTT, &m, &s) == ESP_OK);
                    assert(target == &slots[0]);
                    receive(s);
                    assert(ota_manager_finish(OTA_MQTT, s) == ESP_OK);
                    assert(boot_slot == 0 && selected == 2);
                }
            }
        }
    }
    printf("PASS %s\n", test);
    return 0;
}
