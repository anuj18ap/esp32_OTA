#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "ota_protocol.h"
#include "ota_transports.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#ifdef CONFIG_OTA_BLE_ENABLED
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"
// ESP-IDF 5.4.1 example exposes this initializer separately from the public store header.
void ble_store_config_init(void);

// UUIDs: 8e400001/2/3/4-b5a3-f393-e0a9-e50e24dcca9e.
#define UUID(n)                                                                                    \
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, n,    \
                     0x00, 0x40, 0x8e)
static const ble_uuid128_t service_uuid = UUID(1), control_uuid = UUID(2), data_uuid = UUID(3),
                           status_uuid = UUID(4);
typedef struct
{
    uint16_t conn, len;
    uint32_t epoch;
    bool control;
    uint8_t data[512];
} ble_packet_t;
static QueueHandle_t queue;
static uint8_t address_type;
static uint16_t status_handle;
static uint32_t passkey;
static atomic_uint connection, epoch, disconnected_epoch;
static portMUX_TYPE status_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t last_status[16];
static void advertise(void);
static void status_send(uint16_t conn, uint32_t session, uint32_t sequence, esp_err_t result,
                        uint32_t operation)
{
    uint8_t status[16];
    ota_write_be32(status, session);
    ota_write_be32(status + 4, sequence);
    ota_write_be32(status + 8, (uint32_t)result);
    ota_write_be32(status + 12, operation);
    portENTER_CRITICAL(&status_mux);
    memcpy(last_status, status, sizeof(status));
    portEXIT_CRITICAL(&status_mux);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(status, sizeof(status));
    if (om)
    {
        int rc = ble_gatts_notify_custom(conn, status_handle, om);
        if (rc)
        {
            app_log("[BLE] Notification unavailable (%d); read status", rc);
        }
    }
}
static int access_characteristic(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx,
                                 void *arg)
{
    (void)attr;
    if (ctx->op == BLE_GATT_ACCESS_OP_READ_CHR)
    {
        uint8_t status[16];
        portENTER_CRITICAL(&status_mux);
        memcpy(status, last_status, sizeof(status));
        portEXIT_CRITICAL(&status_mux);
        return os_mbuf_append(ctx->om, status, sizeof(status)) == 0 ? 0
                                                                    : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctx->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        return BLE_ATT_ERR_UNLIKELY;
    }
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) != 0 || !desc.sec_state.encrypted ||
        !desc.sec_state.authenticated)
    {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    ble_packet_t packet = {.conn = conn, .epoch = atomic_load(&epoch), .control = arg != NULL};
    if (ble_hs_mbuf_to_flat(ctx->om, packet.data, sizeof(packet.data), &packet.len) != 0 ||
        !packet.len)
    {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    return xQueueSend(queue, &packet, 0) == pdTRUE ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &service_uuid.u,
     .characteristics =
         (struct ble_gatt_chr_def[]){{.uuid = &control_uuid.u,
                                      .access_cb = access_characteristic,
                                      .arg = (void *)1,
                                      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                                               BLE_GATT_CHR_F_WRITE_AUTHEN},
                                     {.uuid = &data_uuid.u,
                                      .access_cb = access_characteristic,
                                      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                                               BLE_GATT_CHR_F_WRITE_AUTHEN},
                                     {.uuid = &status_uuid.u,
                                      .access_cb = access_characteristic,
                                      .val_handle = &status_handle,
                                      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                                               BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_NOTIFY},
                                     {0}}},
    {0}};
static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type)
    {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0)
            {
                atomic_store(&connection, event->connect.conn_handle);
                atomic_fetch_add(&epoch, 1);
            }
            else
            {
                advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            atomic_store(&disconnected_epoch, atomic_load(&epoch));
            atomic_store(&connection, BLE_HS_CONN_HANDLE_NONE);
            advertise();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            advertise();
            break;
        case BLE_GAP_EVENT_PASSKEY_ACTION:
        {
            struct ble_sm_io io = {.action = event->passkey.params.action, .passkey = passkey};
            if (io.action == BLE_SM_IOACT_DISP)
            {
                return ble_sm_inject_io(event->passkey.conn_handle, &io);
            }
            return BLE_HS_ENOTSUP;
        }
        case BLE_GAP_EVENT_REPEAT_PAIRING:
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        default:
            break;
    }
    return 0;
}
static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0)
    {
        return;
    }
    struct ble_hs_adv_fields scan = {0};
    const char *name = ble_svc_gap_device_name();
    scan.name = (uint8_t *)name;
    scan.name_len = strlen(name);
    scan.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&scan);
    struct ble_gap_adv_params params = {.conn_mode = BLE_GAP_CONN_MODE_UND,
                                        .disc_mode = BLE_GAP_DISC_MODE_GEN};
    int rc = ble_gap_adv_start(address_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc)
    {
        app_log("[BLE] Advertising failed (%d)", rc);
    }
}
static void synchronized(void)
{
    if (ble_hs_id_infer_auto(0, &address_type) == 0)
    {
        advertise();
    }
}
static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}
static void worker_task(void *arg)
{
    (void)arg;
    uint32_t session = 0, session_epoch = 0;
    ble_packet_t packet;
    for (;;)
    {
        uint32_t dropped = atomic_exchange(&disconnected_epoch, 0);
        if (dropped && dropped == session_epoch)
        {
            ota_manager_abort(OTA_BLE, session, "BLE_DISCONNECTED", "BLE connection closed");
            session = 0;
        }
        if (xQueueReceive(queue, &packet, pdMS_TO_TICKS(50)) != pdTRUE)
        {
            continue;
        }
        if (packet.epoch != atomic_load(&epoch) || packet.conn != atomic_load(&connection))
        {
            continue;
        }
        esp_err_t result = ESP_ERR_INVALID_ARG;
        uint32_t sequence = 0, operation = 0;
        if (packet.control)
        {
            char text[513];
            memcpy(text, packet.data, packet.len);
            text[packet.len] = 0;
            cJSON *doc = cJSON_Parse(text);
            cJSON *op = cJSON_GetObjectItemCaseSensitive(doc, "op"),
                  *id = cJSON_GetObjectItemCaseSensitive(doc, "session_id");
            const char *command = cJSON_IsString(op) ? op->valuestring : "";
            if (!strcmp(command, "start"))
            {
                operation = 1;
                ota_metadata_t meta;
                if (ota_metadata_parse(doc, &meta) == ESP_OK)
                {
                    uint32_t new_session;
                    result = ota_manager_begin(OTA_BLE, &meta, &new_session);
                    if (result == ESP_OK)
                    {
                        session = new_session;
                        session_epoch = packet.epoch;
                    }
                }
            }
            else if (cJSON_IsNumber(id) && id->valuedouble == session &&
                     session_epoch == packet.epoch)
            {
                if (!strcmp(command, "finish"))
                {
                    operation = 3;
                    result = ota_manager_finish(OTA_BLE, session);
                }
                else if (!strcmp(command, "abort"))
                {
                    operation = 4;
                    result =
                        ota_manager_abort(OTA_BLE, session, "OTA_ABORTED", "BLE user cancellation");
                }
            }
            cJSON_Delete(doc);
        }
        else if (packet.len > 8 && session_epoch == packet.epoch &&
                 ota_read_be32(packet.data) == session)
        {
            operation = 2;
            sequence = ota_read_be32(packet.data + 4);
            result = ota_manager_write(OTA_BLE, session, sequence, packet.data + 8, packet.len - 8);
        }
        status_send(packet.conn, session, sequence, result, operation);
    }
}
esp_err_t ota_ble_init(void)
{
    nvs_handle_t nvs;
    if (nvs_open("otacfg", NVS_READONLY, &nvs) != ESP_OK)
    {
        app_log("[BLE] OTA disabled until BLE passkey is provisioned");
        return ESP_OK;
    }
    esp_err_t err = nvs_get_u32(nvs, "ble_pin", &passkey);
    nvs_close(nvs);
    if (err != ESP_OK || passkey > 999999)
    {
        app_log("[BLE] OTA disabled until BLE passkey is provisioned");
        return ESP_OK;
    }
    queue = xQueueCreate(6, sizeof(ble_packet_t));
    if (!queue)
    {
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&connection, BLE_HS_CONN_HANDLE_NONE);
    err = nimble_port_init();
    if (err != ESP_OK)
    {
        return err;
    }
    ble_hs_cfg.sync_cb = synchronized;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_store_config_init();
    char name[32];
    snprintf(name, sizeof(name), "ESP32-OTA-%s", device_id + 6);
    ble_svc_gap_device_name_set(name);
    if (ble_gatts_count_cfg(services) != 0 || ble_gatts_add_svcs(services) != 0)
    {
        return ESP_FAIL;
    }
    if (xTaskCreate(worker_task, "ble_ota", 6144, NULL, 4, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
#else
esp_err_t ota_ble_init(void)
{
    return ESP_OK;
}
#endif
