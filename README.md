# ESP32 Home Automation MQTT OTA Firmware

![Platform](https://img.shields.io/badge/Platform-ESP32--WROOM--32E-blue.svg)
![Application](https://img.shields.io/badge/Application-v0.6-green.svg)
![Uploader](https://img.shields.io/badge/Uploader-v0.3-brightgreen.svg)
![Framework](https://img.shields.io/badge/Framework-Arduino-00979D.svg)
![Build](https://img.shields.io/badge/Build-PlatformIO-orange.svg)
![OTA](https://img.shields.io/badge/OTA-MQTT-lightgrey.svg)
![Relays](https://img.shields.io/badge/Relays-8--Channel-blue.svg)
![RGB](https://img.shields.io/badge/RGB-2--Strip-ff69b4.svg)

## Overview

This project combines ESP32 home automation control, MQTT device management, and MQTT OTA firmware updates. The firmware controls 8 relay channels, handles physical inputs, controls 2 RGB LED strips, and can update itself from a PlatformIO-generated firmware binary.

On first boot, the ESP32 opens a setup hotspot and captive portal so the user can enter a device name and WiFi credentials. After provisioning, it connects to the MQTT broker, publishes discovery information, accepts automation commands, supports app-driven WiFi management, and performs chunked OTA updates with CRC32 verification.

The desktop uploader in `Firmware_PythonFiles` is a Tkinter GUI for discovering devices, viewing WiFi/device information, pushing WiFi changes, resetting configuration, and flashing firmware over MQTT.

## Version Control

| Version | Scope | Changes |
| --- | --- | --- |
| `v0.1` | Base automation firmware | Added the core ESP32 automation runtime: MQTT connectivity, 8-channel relay control, physical input handling, retained relay state, and basic automation command processing. |
| `v0.2` | OTA and desktop uploader | Added the MQTT OTA pipeline: firmware metadata transfer, chunked binary upload, per-chunk acknowledgements, CRC32 verification, OTA status reporting, device logs, auto-discovery, and the Python Tkinter uploader workflow. |
| `v0.3` | App identity and WiFi provisioning | Removed hard-coded station WiFi credentials and added NVS-backed setup: first-boot hotspot, captive portal DNS, local device-name and WiFi setup pages, nearby WiFi scanning, wrong-password retry feedback, app-visible device names, app WiFi management, admin-gated reset configuration, and periodic rediscovery publishing. |
| `v0.4` | Encrypted MQTT management payloads | Added shared-key encryption for app/device management payloads so device info, logs, WiFi credentials, and configuration commands are no longer readable as plain text in generic MQTT clients. OTA firmware chunks remain binary and unchanged for transfer reliability. |
| `v0.5` | Relay mask control | Finalized working 8-relay MQTT control from the Python V0.3 app and website. Relay commands now use a full 1-byte mask, preserve already-on relays when adding a new relay, and return ACKs with the complete relay state. |
| `v0.6` | Status LEDs and relay pin update | Moved relay channel 8 from GPIO2 to GPIO15 and added two external status LEDs: GPIO16 turns ON when WiFi is connected, and GPIO17 turns ON when MQTT is connected. |

## Project Structure

- `ESP32_Bootloader/` - PlatformIO ESP32 Arduino firmware project.
- `ESP32_Bootloader/src/main.cpp` - startup, loop, MQTT client object, topics, and shared OTA state.
- `ESP32_Bootloader/src/network.cpp` - Wi-Fi connection, MQTT connection, subscriptions, and device info publishing.
- `ESP32_Bootloader/src/ota.cpp` - OTA begin, chunk write, ACK, CRC check, end, status, and MQTT callback handling.
- `ESP32_Bootloader/src/home_automation.cpp` - 8 relay channels, button ISRs, RGB PWM control, and automation MQTT handlers.
- `ESP32_Bootloader/src/logger.cpp` - publishes ESP32 debug strings to `<deviceID>/log`.
- `ESP32_Bootloader/src/device_identity.cpp` - ESP32 MAC-based device ID and MQTT topic generation.
- `ESP32_Bootloader/src/led.cpp` - heartbeat helper for the onboard GPIO2 LED.
- `Firmware_PythonFiles/Firmware_Upgrader_V0.3.py` - MQTT OTA uploader and home-control GUI.

## Firmware Features

- Controls all 8 relay outputs from MQTT using a single 1-byte bitmask.
- Keeps already-on relay bits set when another relay is switched on.
- Publishes relay ACKs with the complete 8-relay state mask.
- Controls 2 RGB LED strips using JSON `{"r":N,"g":N,"b":N}` payloads, with each value from 0 to 255.
- Turns GPIO16 ON when WiFi is connected.
- Turns GPIO17 ON when MQTT is connected.
- Uses the ESP32 MAC-derived ID as the MQTT topic prefix.
- Publishes retained device discovery information with firmware version and saved name.
- Stores the app-visible device name and WiFi credentials in NVS.
- Starts a setup hotspot and captive portal when no saved WiFi credentials exist.
- Stores WiFi credentials in NVS and reports them to the app when requested.
- Encrypts management payloads and logs before publishing to MQTT.
- Responds to OTA readiness checks.
- Receives OTA metadata and firmware chunks over MQTT.
- Publishes per-chunk acknowledgements.
- Verifies CRC32 before finalising the flash update.
- Publishes OTA status and ESP32 debug logs.

## MQTT Topics

All runtime topics use the ESP32 MAC-derived device ID as the prefix.

Device-management payloads use the `ENC1:` encrypted text envelope. The Python uploader decrypts these automatically. OTA transfer payloads stay in their original binary/control format for transfer reliability.

| Topic | Direction | Payload | Purpose |
| --- | --- | --- | --- |
| `<deviceID>/info` | ESP32 to app | Retained JSON device info: `{"id":"<deviceID>","firmware":"<version>","name":"<name>"}` |
| `<deviceID>/set_name` | App to ESP32 | `ENC1:` text | Device name saved to NVS |
| `<deviceID>/wifi/request` | App to ESP32 | `ENC1:GET_CONFIG` | Request saved WiFi credentials |
| `<deviceID>/wifi/config` | ESP32 to app | `ENC1:` JSON | WiFi reply: `{"ssid":"<ssid>","password":"<password>"}` |
| `<deviceID>/wifi/set` | App to ESP32 | `ENC1:` JSON | WiFi update; ESP32 saves and reboots |
| `<deviceID>/reset_config` | App to ESP32 | `ENC1:RESET_CONFIG` | Clear saved device name and WiFi credentials, then reboot to setup portal |
| `<deviceID>/relay/0xNN` | App/website to ESP32 | `0xNN` | Sets all 8 relay states from one mask; bit0 is relay 1 and bit7 is relay 8 |
| `<deviceID>/relay` | App/website to ESP32 | `0xNN` | Compatibility command topic for the same relay mask |
| `<deviceID>/relay/ack/0xNN` | ESP32 to app/website | `0xNN` | Relay ACK containing the complete current 8-relay mask |
| `<deviceID>/relay/ack` | ESP32 to app/website | `0xNN` | Compatibility ACK topic for the same relay mask |
| `<deviceID>/rgb/1` | App/website to ESP32 | JSON | RGB strip 1: `{"r":N,"g":N,"b":N}`, values 0-255 |
| `<deviceID>/rgb/2` | App/website to ESP32 | JSON | RGB strip 2: `{"r":N,"g":N,"b":N}`, values 0-255 |
| `<deviceID>/rgb/ack` | ESP32 to app/website | `ok` | RGB command acknowledgement |
| `<deviceID>/ota_check` | Uploader to ESP32 | `ARE_YOU_READY` | Readiness check before upload |
| `<deviceID>/ota_status` | ESP32 to uploader | status text | `READY`, `UPDATING`, `SUCCESS`, or `FAILED` |
| `<deviceID>/ota/begin` | Uploader to ESP32 | JSON | Firmware size, chunk count, and CRC32 |
| `<deviceID>/ota/chunk` | Uploader to ESP32 | binary | One firmware chunk with a 4-byte chunk index |
| `<deviceID>/ota/end` | Uploader to ESP32 | `END` | Transfer completion command |
| `<deviceID>/ota/ack` | ESP32 to uploader | chunk index | Acknowledgement for the written chunk |
| `<deviceID>/log` | ESP32 to app | `ENC1:` text | Debug log string from ESP32 firmware |

## Automation Payloads

Relay command example:

```text
<deviceID>/relay/0x01 = relay 1 ON, all others OFF
<deviceID>/relay/0x03 = relay 1 and relay 2 ON
<deviceID>/relay/0xFF = all 8 relays ON
<deviceID>/relay/0x00 = all 8 relays OFF
```

The Python V0.3 app and website always send the full desired mask, not only the clicked relay bit. For example, if relay 1 is already ON and relay 2 is clicked ON, the app sends `0x03`. ESP32 then replies with the complete state on `<deviceID>/relay/ack/0x03`, and the UI updates all relay buttons from that ACK.

RGB command examples:

```json
{"r":255,"g":0,"b":128}
{"r":0,"g":0,"b":0}
{"r":64,"g":64,"b":255}
```

## OTA Flow

1. ESP32 boots, connects to Wi-Fi, connects to MQTT, and subscribes to OTA topics.
2. Uploader sends `ARE_YOU_READY` to `<deviceID>/ota_check`.
3. ESP32 publishes `READY` on `<deviceID>/ota_status`.
4. Uploader sends metadata to `<deviceID>/ota/begin`.
5. ESP32 opens the update partition and publishes `UPDATING`.
6. Uploader sends each chunk to `<deviceID>/ota/chunk`.
7. ESP32 writes each chunk and publishes the chunk index on `<deviceID>/ota/ack`.
8. Uploader sends `END` to `<deviceID>/ota/end`.
9. ESP32 verifies CRC32, publishes `SUCCESS`, and restarts.

## App Discovery And WiFi Flow

1. ESP32 boots, loads the saved device name and WiFi credentials from NVS, then connects to WiFi and MQTT.
2. ESP32 publishes `<deviceID>/info` with `id`, `firmware`, and `name`.
3. The app subscribes to `+/info`, discovers the device, then publishes `GET_CONFIG` to `<deviceID>/wifi/request`.
4. ESP32 replies immediately on `<deviceID>/wifi/config` with the saved `ssid` and `password`.
5. If the app sends a new name to `<deviceID>/set_name`, ESP32 saves it in NVS and republishes `<deviceID>/info`.
6. If the app sends new WiFi credentials to `<deviceID>/wifi/set`, ESP32 saves them in NVS, publishes a log confirmation, and reboots.
7. If the app sends `RESET_CONFIG` to `<deviceID>/reset_config`, ESP32 clears saved device name and WiFi credentials, then reboots into first-boot setup.

## First-Boot WiFi Setup

If no WiFi SSID is saved in ESP32 NVS, the firmware does not use a hard-coded network. Instead it starts a setup hotspot with captive-portal DNS:

```text
SSID: ESP32-Setup-<last 6 MAC chars>
Password: 12345678
Setup page: http://192.168.4.1
```

Setup flow:

1. Connect a phone or laptop to the ESP32 setup hotspot.
2. The phone may automatically show a WiFi sign-in/setup page. If it does not, open `http://192.168.4.1`.
3. Enter the device name.
4. Select a nearby WiFi network from the scanned list.
5. Enter the WiFi password.
6. If the password is correct, ESP32 saves the device name, SSID, and password to NVS, then reboots.
7. If the password is wrong or connection fails, the setup page shows an error and lets the user try again.

After provisioning, normal boot uses the saved NVS credentials. If the saved credentials fail later, the ESP32 starts the setup hotspot again.

The Python uploader Settings tab also includes **Reset Configuration**. Pressing it requires the admin passkey, sends `<deviceID>/reset_config`, clears the app's cached WiFi/name fields for that device, and forces the ESP32 user to enter the device name and WiFi password again through the setup portal.

## Build Firmware

Open a terminal in the PlatformIO project:

```powershell
cd D:\GITHUB\ESP32_OTA\ESP32_Bootloader
platformio run
```

The application firmware file is generated at:

```text
.pio/build/esp32dev/firmware.bin
```

Use this `firmware.bin` for MQTT OTA upload. Do not select `bootloader.bin`, `partitions.bin`, or a merged flash image in the uploader.

## Run Uploader

Install the Python dependency:

```powershell
pip install paho-mqtt
```

Start the GUI uploader:

```powershell
cd D:\GITHUB\ESP32_OTA
python Firmware_PythonFiles\Firmware_Upgrader_V0.2.py
```

Upload steps:

1. Read the ESP32 MAC ID from the serial monitor.
2. Enter the MAC ID in the uploader.
3. Click `Check Device`.
4. Select `.pio/build/esp32dev/firmware.bin`.
5. Click `Upload Firmware via MQTT`.
6. Wait for `OTA SUCCESS` and the ESP32 restart.

## Configuration

Firmware settings are in `ESP32_Bootloader/include/Config.h` and `ESP32_Bootloader/src/main.cpp`.

- `FW_VERSION` controls the firmware version published to MQTT.
- `MQTT_BUFFER_SIZE` is set to `8192` for OTA chunk packets.
- `INFO_PUBLISH_INTERVAL` controls how often `<deviceID>/info` is republished for app rediscovery.
- `PROVISION_AP_PREFIX` and `PROVISION_AP_PASSWORD` configure the setup hotspot used when no saved WiFi credentials are present.
- WiFi station SSID/password are not hard-coded. They are saved through the setup portal or MQTT and loaded from NVS.
- `MQTT_BROKER` and `MQTT_PORT` configure the MQTT broker and were kept as already present in code.

Home automation pin settings are in `ESP32_Bootloader/src/home_automation.cpp`.

| Group | GPIOs |
| --- | --- |
| Buttons | `13, 12, 14, 27, 26, 25, 33, 32` |
| Relay outputs | `23, 22, 21, 19, 18, 5, 4, 15` |
| Relay indicators | `0, 0, 0, 3, 1, 10, 9, 0` |
| RGB 1 | R=`26`, G=`27`, B=`14` |
| RGB 2 | R=`25`, G=`33`, B=`32` |
| WiFi status LED | `16` |
| MQTT status LED | `17` |

Important: the supplied pin map reuses some GPIOs between buttons, RGB outputs, relay indicators, and status LEDs. It also uses boot, serial, and flash-sensitive GPIOs such as `0`, `1`, `3`, `9`, and `10` for indicators. Adjust the pin arrays before hardware testing if your wiring requires separate safe pins.

Uploader settings are in `Firmware_PythonFiles/Firmware_Upgrader_V0.2.py`.

- `CHUNK_SIZE` controls bytes sent per OTA chunk.
- `ACK_TIMEOUT` controls how long the uploader waits for each ACK.
- `MAX_RETRIES` controls retries before upload failure.

## Dependencies

ESP32 firmware:

- PlatformIO
- ESP32 Arduino framework
- `knolleary/PubSubClient`
- `bblanchon/ArduinoJson`
- ESP32 Arduino `WebServer`
- ESP32 Arduino `DNSServer`

Python uploader:

- Python 3.8 or newer
- `paho-mqtt`
- Tkinter

## Notes

- The uploader sends one chunk at a time and waits for the ESP32 ACK before continuing.
- ESP32 runtime debug logs are published to `<deviceID>/log` and mirrored to USB Serial.
- The ESP32 skips a duplicate of the last received chunk and re-sends its ACK.
- CRC32 is calculated by the Python uploader and verified by the ESP32 before rebooting.
- Relay channel 8 now uses GPIO15. GPIO2 is available for the onboard heartbeat LED.
- GPIO16 is the WiFi status LED and GPIO17 is the MQTT status LED.
- If OTA ends with `FAILED`, confirm that the selected file is the PlatformIO application `firmware.bin`.
