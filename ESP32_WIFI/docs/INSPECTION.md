# Inspection and compatibility — 2026-10-11

Before edits, `ESP32_WIFI` contained a native Wi-Fi portal in a blink example structure, with `app_main()` in `main/blink_example_main.c`, HTTP setup and a 500 ms LED timer. It did not implement MQTT, saved name/Wi-Fi configuration, automation or OTA. Its sdkconfig selected ESP32, 2 MB flash, a single factory app and no rollback, Secure Boot or flash encryption. The copied 140,944-byte blink image was built elsewhere using IDF 5.5.3.

The working application is `ESP32_Bootloader`, using PlatformIO Arduino. `ESP_IDF_MIGRATION_PLAN.md` was inspected. A native draft under `G:/Application_deployment/esp32_idf_port` was found, compared against Arduino, and reused with corrections. Its previous toolchain/configuration logs were not treated as successful builds.

| Existing Arduino behavior | Native port |
|---|---|
| Firmware 0.6; LED GPIO 2 at 1 s; Wi-Fi LED 16; MQTT LED 17; discovery every 45 s | `app_main.c`, `app.h`, `network.c` |
| Little-endian `ESP.getEfuseMac()` formatted uppercase | Reverse eFuse MAC bytes to preserve IDs/topics |
| NVS namespace `appcfg`, keys `name`, `ssid`, `password` | Native NVS with same names |
| Setup AP `ESP32-Setup-` + last six ID digits, password `12345678`, name then scanned Wi-Fi, captive DNS, save/reboot | Native Wi-Fi/HTTP/DNS, bounded form parsing |
| Broker `broker.emqx.io:1883`, client `ESP32-<ID>`, keepalive 60 s, 2 s reconnect, 8192 buffer | esp-mqtt replaces PubSubClient runtime while preserving the wire interface |
| XTEA CTR four-word key, random 8-byte nonce, base64 `ENC1:`, plain transition inputs | `mqtt_crypto.c` |
| 192-byte serial and encrypted MQTT log | `app_log()` |
| Relays `{23,22,21,19,18,5,4,15}`, active LOW, hex/raw mask, `0xNN` ACK | Native GPIO; parser in `ota_protocol.c` |
| RGB `{26,27,14}` and `{25,33,32}`, common GND, 1 kHz 8-bit PWM, clamped JSON r/g/b, `ok` ACK, 700 ms/color and 250 ms/strip boot test | Native LEDC |
| Buttons disabled because they overlap RGB; button processing commented out | Remain disabled |
| MQTT begin JSON, 4-byte sequence + image, decimal ACK, CRC32 | Legacy adapter feeds common OTA manager |

Subscriptions preserve `<ID>/{set_name,wifi/request,wifi/set,reset_config,ota_check,ota/begin,ota/chunk,ota/end,relay,rgb/1,rgb/2}` plus `+/rgb/1`, `+/rgb/2`. Foreign RGB IDs are ignored. Discovery is retained encrypted JSON `{id,firmware,name}` on **`AceTech/<ID>/info`** (the actual source differs from some old comments). Logs/Wi-Fi responses stay encrypted, automation commands/ACKs stay plain. Arduino's named state-publication function only logged readiness; it did not publish retained automation state.

Arduino OTA used `Update`, lacked complete size/order enforcement, timeouts, rollback confirmation and other transports. The new common manager adds those checks while keeping uploader formats. Only it writes/selects OTA partitions. A mutex protects ownership/state/hash/report data. MQTT fragmented events are assembled then queued with connection epochs. BLE/ESP-NOW callbacks enqueue; flash operations occur in workers/main task. HTTP uses a temporary task and 1 KB streaming buffer; ESP-NOW uses a temporary sender task. SDK event/MQTT/NimBLE tasks, captive DNS and serial provisioning are used.

Before selecting partitions, esptool on COM3 reported flash manufacturer `5e`, device `4016`, **4 MB**, ESP32-D0WDQ6 revision 1.0. Only a RAM stub was loaded and the board reset; flash was not written. This does not identify a WROOM-32E module. IDF 5.4.1 is available on G:. NVS offset/size are preserved; the original blink table had no other required application data partition. The on-board Arduino partition table has not been backed up or physically migrated.

Startup checks initialization of NVS, GPIO/LEDC, manager, network and provisioned transports, CRC and crypto round-trip. Confirmation follows 10 s of stable main-loop execution. Router/broker reachability is not a health test. These checks do not verify relay contacts or external loads; extend `self_test()` for any further critical hardware requirement.

Legacy encryption/control remain compatibility interfaces, not authenticated security. New requests use HMAC/nonce; BLE requires authenticated encryption/passkey; ESP-NOW requires an encrypted unicast peer. Security provisioning, board flashing, phone/radio tests, physical rollback and power interruption were not performed. All remain hardware-required until executed.
