# Native ESP-IDF port of the Arduino application

This folder ports `../ESP32_Bootloader` into native ESP-IDF, preserving device IDs, EMQX broker/topics, ENC1 encryption, provisioning, saved settings, relay/RGB pins and commands, heartbeat and the original MQTT uploader protocol. The Arduino source and Python GUI are unchanged. The previous portal example is retained in `examples/original_portal` and excluded from the build.

Read [inspection](docs/INSPECTION.md), [protocols and device testing](docs/OTA_GUIDE.md), and [recovery analysis](docs/RECOVERY.md). Executed tests: `tests/results/report.json`. Full build log: `docs/build-validation.log`.

## Build and installation

Use an **ESP-IDF 5.4.1** IDE terminal in this folder:

```powershell
idf.py -B build-idf54 build
idf.py -B build-idf54 size
# Read-only capacity probe resets the selected board:
python -m esptool --chip esp32 --port COM3 flash_id
# Back up this verified 4 MB board before migration:
python -m esptool --chip esp32 --port COM3 read_flash 0 0x400000 board-backup.bin
idf.py -B build-idf54 -p COM3 flash monitor
```

On this computer, `./build-local.ps1` uses the existing IDF 5.4.1 toolchain on G:. Pass `-IdfPath`/`-ToolsPath` to override; other installations should use their exported IDE terminal. Build uses a fresh directory, not the old copied blink cache.

**First installation requires serial flashing of the standard bootloader and partition table.** OTA cannot migrate the old blink/Arduino layout. NVS stays at `0x9000`, size `0x5000`; compare the original board's partition table and back it up before relying on preservation. OTA input is only `build-idf54/esp32_home_automation_ota.bin`, never a merged flash image.

COM3 on 2026-10-11 reported 4 MB flash and ESP32-D0WDQ6 revision 1.0. This is not WROOM-32E revision evidence; verify other boards separately. Slots are `ota_0=0x10000/0x1e0000`, `ota_1=0x1f0000/0x1e0000`, each 1,966,080 bytes. `otadata=0xe000/0x2000`. Runtime also checks physical flash bounds.

## Laptop tests

```powershell
python -m pip install -r tests/requirements.txt
python tests/run.py host --idf "$env:IDF_PATH" --compiler zig
python tests/run.py simulate-espnow --loss 0.2 --ack-loss 0.4
python tests/run.py serve build-idf54/esp32_home_automation_ota.bin --bind 0.0.0.0 --port 8000
python tests/run.py status --device DEVICE_ID --key-file ota-key.txt
python tests/run.py http build-idf54/esp32_home_automation_ota.bin --device DEVICE_ID --key-file ota-key.txt --url http://LAPTOP_IP:8000/valid.bin
python tests/run.py mqtt build-idf54/esp32_home_automation_ota.bin --device DEVICE_ID --legacy
python tests/run.py mqtt build-idf54/esp32_home_automation_ota.bin --device DEVICE_ID --key-file ota-key.txt --fault incomplete
```

Python 3.10+; Paho for MQTT, pyserial for serial provisioning, optional Bleak for real BLE adapters. Host HTTP tests and simulations use the standard library. Native tests need a C compiler (`zig`, `cc`, `gcc`) and IDF's cJSON source. The runner produces console counts, a log and JSON with passed/failed/skipped/hardware-required classifications. Actual C manager tests mock IDF flash/NVS/SHA/RTOS services and do not establish radio, physical flash, TLS or rollback acceptance.

Enable `CONFIG_OTA_ALLOW_DEV_HTTP` in menuconfig and rebuild for local HTTP only. Production HTTPS keeps certificate verification. The broker remains `broker.emqx.io:1883`. Legacy OTA remains enabled to preserve the GUI; disable `CONFIG_OTA_ENABLE_LEGACY_MQTT` for authenticated OTA deployments. BLE, ESP-NOW and extended MQTT need provisioned keys/passkey/peer settings. Secure Boot, flash encryption and signed-app enforcement are not enabled automatically.

Legacy statuses/ACKs remain unchanged; `SUCCESS` means validated image selected for boot. Final startup `confirmed` is on the additional `<ID>/ota/result` topic. No firmware was flashed during this implementation.

## Coding style

Maintained C/header and Python files use four spaces, no tabs, and a 100-column formatting target. C control blocks always use braces on their own lines, matching the Arduino source style. `.editorconfig`, `.clang-format`, and `pyproject.toml` record these rules for future changes. Generated builds, downloaded tools, and the archived original example are excluded.

Run in this folder:

```powershell
python -m pip install --target .format-tools -r tools/format-requirements.txt
python -B tools/format_code.py
python -B tools/format_code.py --check
```

The check exits with an error if any maintained firmware or test/helper source needs formatting.
