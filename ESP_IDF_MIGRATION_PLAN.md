# ESP-IDF Migration Plan

## Current Firmware Inventory

The existing Arduino firmware is modular but still depends heavily on Arduino runtime APIs.

- `src/main.cpp` owns boot setup, shared globals, Wi-Fi/MQTT supervision, periodic device info, and heartbeat handling.
- `src/network.cpp` owns Wi-Fi, provisioning AP/captive portal, NVS-backed device config, app config MQTT handlers, MQTT reconnect, and subscriptions.
- `src/ota.cpp` owns the current MQTT OTA state machine using Arduino `Update`.
- `src/home_automation.cpp` owns relay control, RGB control, button ISR/debounce, MQTT relay/RGB payload parsing, and ACK publishing.
- `src/mqtt_crypto.cpp` owns XTEA-CTR style payload encryption with base64 transport.
- `src/logger.cpp` owns `<deviceID>/log` publishing plus Serial mirroring.
- `src/device_identity.cpp` owns MAC-derived device ID and MQTT topic construction.
- `src/led.cpp` owns heartbeat LED timing.

## MQTT Topics To Preserve

Device-specific OTA/config topics:

- `<deviceID>/info`
- `<deviceID>/ota_check`
- `<deviceID>/ota_status`
- `<deviceID>/ota/begin`
- `<deviceID>/ota/chunk`
- `<deviceID>/ota/end`
- `<deviceID>/ota/ack`
- `<deviceID>/log`
- Device name, Wi-Fi request/config/set, and reset-config topics generated in `initTopics()`

Automation topics:

- Relay topic stored in `relayTopic`
- RGB topics stored in `rgb1Topic` and `rgb2Topic`
- Compatibility subscriptions `+/rgb/1` and `+/rgb/2`
- Relay ACK topic `relayTopic + "/ack"`
- RGB ACK topic `<deviceID>/rgb/ack`

## Arduino API Replacement Map

| Current Arduino API | ESP-IDF Replacement |
| --- | --- |
| `setup()` / `loop()` | `app_main()` plus FreeRTOS tasks |
| `WiFi`, `WiFiClient` | `esp_wifi`, `esp_netif`, event handlers |
| `PubSubClient` | `esp-mqtt` client |
| `Preferences` | `nvs_flash`, `nvs_open`, `nvs_get_str`, `nvs_set_str` |
| `WebServer` | `esp_http_server` |
| `DNSServer` | ESP-IDF DNS server implementation or captive portal helper task |
| `Update` | `esp_ota_begin`, `esp_ota_write`, `esp_ota_end`, `esp_ota_set_boot_partition` |
| `delay()` | `vTaskDelay(pdMS_TO_TICKS(ms))` |
| `millis()` | `esp_timer_get_time() / 1000` or FreeRTOS ticks |
| `Serial` | `ESP_LOGI`, `ESP_LOGW`, `ESP_LOGE`, plus MQTT log publish |
| `pinMode`, `digitalWrite`, `digitalRead` | `gpio_config`, `gpio_set_level`, `gpio_get_level` |
| `attachInterrupt` | `gpio_isr_handler_add` |
| `analogWrite` | LEDC driver |
| `ESP.restart()` | `esp_restart()` |
| `ESP.getEfuseMac()` | `esp_efuse_mac_get_default()` |
| `ESP.getFreeSketchSpace()` | inactive OTA partition size from `esp_partition_t` |
| Arduino `String` | `std::string` or fixed buffers |
| ArduinoJson | `cJSON` or managed ESP-IDF component if kept intentionally |

## Recommended Port Order

1. Create a new ESP-IDF project skeleton beside the Arduino project.
2. Port common config, device identity, topic construction, and logging.
3. Port crypto helpers and verify encrypted/decrypted MQTT payload compatibility.
4. Port NVS-backed device configuration.
5. Port GPIO, relay, RGB, button ISR, and heartbeat modules.
6. Port Wi-Fi station mode and provisioning AP/captive portal.
7. Port MQTT connection, reconnect, subscriptions, publish helpers, and callback routing.
8. Reconnect existing app MQTT payload handlers without changing topic names or payload formats.
9. Replace OTA with ESP-IDF dual-slot A/B OTA.
10. Enable rollback and mark firmware valid only after application self-check passes.

## OTA A/B Design

The ESP-IDF OTA module should:

- Call `esp_ota_mark_app_valid_cancel_rollback()` after the app reaches a healthy runtime state.
- Use `esp_ota_get_running_partition()` to identify the active slot.
- Use `esp_ota_get_next_update_partition(NULL)` or equivalent partition lookup to select the inactive slot.
- Never write to the running partition.
- Call `esp_ota_begin()` before receiving chunks.
- Call `esp_ota_write()` for each MQTT firmware chunk.
- Validate chunk count and CRC32 before finalizing.
- Call `esp_ota_end()` after the final chunk.
- Call `esp_ota_set_boot_partition()` for the inactive slot.
- Publish final OTA status, flush MQTT if possible, then call `esp_restart()`.
- Call `esp_ota_mark_app_invalid_rollback_and_reboot()` when startup self-check fails.

## Required ESP-IDF Project Settings

- Partition table must include `otadata`, `ota_0`, and `ota_1`.
- Rollback must be enabled in sdkconfig with app rollback support.
- MQTT buffer must support current OTA chunk size or the chunk size must be adjusted.
- NVS must be initialized before loading device name or Wi-Fi credentials.
- Wi-Fi and MQTT events should be event-driven instead of blocking loops.

## First Conversion Target

Start with `device_identity` and `logger` because they are small and allow the rest of the modules to keep a stable topic/logging interface while the lower layers are replaced.

The first ESP-IDF module should expose:

- `device_identity_init()`
- `device_identity_get_id()`
- `device_topics_init()`
- `app_log_info()`, `app_log_warn()`, `app_log_error()`
- `app_log_publish()` hook that publishes to `<deviceID>/log` only after MQTT is connected

## Tooling Note

`idf.py`, `cmake`, and `ninja` were not available on PATH during this analysis. Compile verification should begin after the Espressif IDE terminal or ESP-IDF export script is active.
