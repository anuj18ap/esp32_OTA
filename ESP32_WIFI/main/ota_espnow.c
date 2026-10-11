#include "esp_image_format.h"
#include "esp_now.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "ota_protocol.h"
#include "ota_transports.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#ifdef CONFIG_OTA_ESPNOW_ENABLED
#define HEADER 12
#define PAYLOAD 224
#define START_SIZE 77
enum
{
    FRAME_START = 1,
    FRAME_DATA = 2,
    FRAME_END = 3,
    FRAME_ABORT = 4,
    FRAME_ACK = 5
};
typedef struct
{
    uint16_t len;
    uint8_t data[250];
} frame_t;
static QueueHandle_t rx, acks;
static uint8_t peer[6];
static uint8_t role;
static bool initialized;
static atomic_bool sender_busy;
static uint32_t wire_session, local_session, last_wire;
static bool receiver_finished;
static ota_metadata_t receiver_meta;
static void frame_header(frame_t *frame, unsigned op, uint32_t session, uint32_t sequence)
{
    frame->data[0] = 'O';
    frame->data[1] = 'T';
    frame->data[2] = 1;
    frame->data[3] = op;
    ota_write_be32(frame->data + 4, session);
    ota_write_be32(frame->data + 8, sequence);
}
static void receive_callback(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!info || !data || len < HEADER || len > 250 || memcmp(info->src_addr, peer, 6) ||
        data[0] != 'O' || data[1] != 'T' || data[2] != 1)
    {
        return;
    }
    frame_t frame = {.len = len};
    memcpy(frame.data, data, len);
    // The Wi-Fi callback never writes flash or waits for ACKs.
    QueueHandle_t queue = data[3] == FRAME_ACK ? acks : rx;
    xQueueSend(queue, &frame, 0);
}
static esp_err_t send_ack(uint32_t session, uint32_t sequence, esp_err_t result)
{
    frame_t frame = {.len = HEADER + 4};
    frame_header(&frame, FRAME_ACK, session, sequence);
    ota_write_be32(frame.data + HEADER, (uint32_t)result);
    return esp_now_send(peer, frame.data, frame.len);
}
static bool send_reliably(frame_t *frame)
{
    uint32_t session = ota_read_be32(frame->data + 4), sequence = ota_read_be32(frame->data + 8);
    for (unsigned attempt = 0; attempt < 8; attempt++)
    {
        if (esp_now_send(peer, frame->data, frame->len) != ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        uint64_t deadline = app_millis() + 600;
        frame_t ack;
        while (app_millis() < deadline)
        {
            if (xQueueReceive(acks, &ack, pdMS_TO_TICKS(30)) == pdTRUE && ack.len == HEADER + 4 &&
                ota_read_be32(ack.data + 4) == session && ota_read_be32(ack.data + 8) == sequence)
            {
                return ota_read_be32(ack.data + HEADER) == ESP_OK;
            }
        }
    }
    return false;
}
static void encode_metadata(uint8_t *out, const ota_metadata_t *m)
{
    ota_write_be32(out, m->size);
    ota_write_be32(out + 4, m->chunks);
    ota_write_be32(out + 8, m->crc32);
    memset(out + 12, 0, 32);
    memcpy(out + 12, m->version, strnlen(m->version, 31));
    memcpy(out + 44, m->sha256, 32);
    out[76] = m->has_sha256;
}
static void decode_metadata(const uint8_t *in, ota_metadata_t *m)
{
    memset(m, 0, sizeof(*m));
    m->size = ota_read_be32(in);
    m->chunks = ota_read_be32(in + 4);
    m->crc32 = ota_read_be32(in + 8);
    memcpy(m->version, in + 12, 31);
    memcpy(m->sha256, in + 44, 32);
    m->has_sha256 = in[76] == 1;
}
void ota_espnow_process(void)
{
    if (!initialized || role != 0)
    {
        return;
    }
    frame_t frame;
    for (unsigned processed = 0; processed < 4 && xQueueReceive(rx, &frame, 0) == pdTRUE;
         processed++)
    {
        unsigned op = frame.data[3];
        uint32_t remote = ota_read_be32(frame.data + 4), sequence = ota_read_be32(frame.data + 8);
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (op == FRAME_START && frame.len == HEADER + START_SIZE && remote)
        {
            ota_metadata_t meta;
            decode_metadata(frame.data + HEADER, &meta);
            if (sequence != UINT32_MAX || !meta.size ||
                meta.chunks != (meta.size - 1) / PAYLOAD + 1)
            {
                err = ESP_ERR_INVALID_ARG;
            }
            else if (remote == wire_session && ota_manager_owned(OTA_ESPNOW, local_session) &&
                     !memcmp(&meta, &receiver_meta, sizeof(meta)))
            {
                err = ESP_OK;
            }
            else if (remote == last_wire)
            {
                err = ESP_ERR_INVALID_STATE;
            }
            else
            {
                uint32_t session;
                err = ota_manager_begin(OTA_ESPNOW, &meta, &session);
                if (err == ESP_OK)
                {
                    wire_session = remote;
                    local_session = session;
                    receiver_meta = meta;
                    last_wire = remote;
                    receiver_finished = false;
                }
            }
        }
        else if (remote == wire_session)
        {
            if (op == FRAME_DATA && frame.len > HEADER && frame.len <= HEADER + PAYLOAD)
            {
                err = ota_manager_write(OTA_ESPNOW, local_session, sequence, frame.data + HEADER,
                                        frame.len - HEADER);
            }
            else if (op == FRAME_END && frame.len == HEADER && sequence == UINT32_MAX - 1)
            {
                // A retried END is ACKed after successful boot selection without selecting again.
                if (receiver_finished)
                {
                    err = ESP_OK;
                }
                else if (ota_manager_owned(OTA_ESPNOW, local_session))
                {
                    err = ota_manager_finish(OTA_ESPNOW, local_session);
                    if (err == ESP_OK)
                    {
                        receiver_finished = true;
                    }
                }
            }
            else if (op == FRAME_ABORT && frame.len == HEADER)
            {
                err = ota_manager_abort(OTA_ESPNOW, local_session, "OTA_ABORTED",
                                        "Coordinator cancelled transfer");
            }
        }
        if (send_ack(remote, sequence, err) != ESP_OK)
        {
            app_log("[ESP-NOW] ACK send failed; sender may retry");
        }
    }
}
static void sender_task(void *arg)
{
    (void)arg;
    const esp_partition_t *source = esp_ota_get_running_partition();
    esp_partition_pos_t position = {.offset = source->address, .size = source->size};
    esp_image_metadata_t image = {0};
    esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY, &position, &image);
    uint32_t session;
    do
    {
        session = esp_random();
    } while (!session);
    if (err == ESP_OK)
    {
        ota_metadata_t meta = {.size = image.image_len,
                               .chunks = (image.image_len + PAYLOAD - 1) / PAYLOAD};
        esp_app_desc_t desc = {0};
        err = esp_ota_get_partition_description(source, &desc);
        snprintf(meta.version, sizeof(meta.version), "%s", desc.version);
        uint8_t buffer[PAYLOAD];
        uint32_t crc = UINT32_MAX;
        for (uint32_t offset = 0; err == ESP_OK && offset < meta.size;)
        {
            size_t len = meta.size - offset;
            if (len > sizeof(buffer))
            {
                len = sizeof(buffer);
            }
            err = esp_partition_read(source, offset, buffer, len);
            if (err == ESP_OK)
            {
                crc = ota_crc32(crc, buffer, len);
            }
            offset += len;
        }
        meta.crc32 = crc ^ UINT32_MAX;
        frame_t frame = {.len = HEADER + START_SIZE};
        frame_header(&frame, FRAME_START, session, UINT32_MAX);
        encode_metadata(frame.data + HEADER, &meta);
        bool success = err == ESP_OK && send_reliably(&frame);
        for (uint32_t i = 0; success && i < meta.chunks; i++)
        {
            size_t len = meta.size - i * PAYLOAD;
            if (len > PAYLOAD)
            {
                len = PAYLOAD;
            }
            frame.len = HEADER + len;
            frame_header(&frame, FRAME_DATA, session, i);
            success = esp_partition_read(source, i * PAYLOAD, frame.data + HEADER, len) == ESP_OK &&
                      send_reliably(&frame);
        }
        if (success)
        {
            frame.len = HEADER;
            frame_header(&frame, FRAME_END, session, UINT32_MAX - 1);
            success = send_reliably(&frame);
        }
        if (!success)
        {
            frame.len = HEADER;
            frame_header(&frame, FRAME_ABORT, session, UINT32_MAX - 2);
            esp_now_send(peer, frame.data, frame.len);
            ota_report_external(OTA_ESPNOW, session, "TRANSFER_TIMEOUT",
                                "Coordinator transfer failed; receiver remains on current slot");
        }
        else
        {
            app_log("[ESP-NOW] Peer validated and selected image; confirmation requires peer "
                    "startup report");
        }
    }
    else
    {
        ota_report_external(OTA_ESPNOW, session, "IMAGE_VALIDATION_FAILED",
                            "Cannot read verified running image as coordinator source");
    }
    atomic_store(&sender_busy, false);
    vTaskDelete(NULL);
}
esp_err_t ota_espnow_send_running(void)
{
    if (!initialized || role != 1 || !app_wifi_connected())
    {
        return ESP_ERR_INVALID_STATE;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&sender_busy, &expected, true))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(sender_task, "espnow_tx", 6144, NULL, 3, NULL) != pdPASS)
    {
        atomic_store(&sender_busy, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t ota_espnow_init(void)
{
    nvs_handle_t nvs;
    uint8_t pmk[16], lmk[16];
    size_t len = 6;
    if (nvs_open("otacfg", NVS_READONLY, &nvs) != ESP_OK)
    {
        app_log("[ESP-NOW] Disabled until peer and keys are provisioned");
        return ESP_OK;
    }
    esp_err_t err = nvs_get_blob(nvs, "peer_mac", peer, &len);
    bool ok = err == ESP_OK && len == 6 && !(peer[0] & 1);
    len = 16;
    ok = ok && nvs_get_blob(nvs, "pmk", pmk, &len) == ESP_OK && len == 16;
    len = 16;
    ok = ok && nvs_get_blob(nvs, "lmk", lmk, &len) == ESP_OK && len == 16;
    ok = ok && nvs_get_u8(nvs, "role", &role) == ESP_OK && role <= 1;
    nvs_close(nvs);
    if (!ok)
    {
        app_log("[ESP-NOW] Incomplete provisioning; transport disabled");
        return ESP_OK;
    }
    rx = xQueueCreate(8, sizeof(frame_t));
    acks = xQueueCreate(4, sizeof(frame_t));
    if (!rx || !acks)
    {
        return ESP_ERR_NO_MEM;
    }
    err = esp_now_init();
    if (err != ESP_OK)
    {
        return err;
    }
    err = esp_now_set_pmk(pmk);
    if (err != ESP_OK)
    {
        return err;
    }
    esp_now_peer_info_t info = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = true};
    memcpy(info.peer_addr, peer, 6);
    memcpy(info.lmk, lmk, 16);
    err = esp_now_add_peer(&info);
    memset(pmk, 0, sizeof(pmk));
    memset(lmk, 0, sizeof(lmk));
    if (err != ESP_OK)
    {
        return err;
    }
    err = esp_now_register_recv_cb(receive_callback);
    if (err == ESP_OK)
    {
        initialized = true;
    }
    return err;
}
#else
esp_err_t ota_espnow_init(void)
{
    return ESP_OK;
}
void ota_espnow_process(void)
{
}
esp_err_t ota_espnow_send_running(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
