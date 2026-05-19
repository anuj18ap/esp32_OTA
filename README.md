# ESP32 Home Automation MQTT OTA Firmware

![Platform](https://img.shields.io/badge/Platform-ESP32--WROOM--32E-blue.svg)
![Application](https://img.shields.io/badge/Application-v0.2-green.svg)
![Uploader](https://img.shields.io/badge/Uploader-v0.2-brightgreen.svg)
![Framework](https://img.shields.io/badge/Framework-Arduino-00979D.svg)
![Build](https://img.shields.io/badge/Build-PlatformIO-orange.svg)
![OTA](https://img.shields.io/badge/OTA-MQTT-lightgrey.svg)
![Relays](https://img.shields.io/badge/Relays-8--Channel-blue.svg)
![RGB](https://img.shields.io/badge/RGB-2--Strip-ff69b4.svg)

## Overview

This project combines ESP32 home automation control with MQTT OTA firmware updates. The firmware controls 8 relay channels with physical button toggles and MQTT commands, controls 2 RGB LED strips with 0-100 percent PWM values over MQTT, and still supports MQTT-based OTA updates.

The ESP32 connects to Wi-Fi, connects to the configured MQTT broker, subscribes to automation and OTA topics, publishes retained relay/RGB status, receives a PlatformIO-generated `.bin` file in chunks during OTA, verifies CRC32, writes the image using the ESP32 `Update` API, and restarts after a successful update.

The desktop uploader in `Firmware_PythonFiles` provides a small Tkinter GUI that checks the ESP32 status, sends OTA metadata, streams firmware chunks, waits for an ACK after each chunk, and displays upload progress.

## Project Structure

- `ESP32_Bootloader/` - PlatformIO ESP32 Arduino firmware project.
- `ESP32_Bootloader/src/main.cpp` - startup, loop, MQTT client object, topics, and shared OTA state.
- `ESP32_Bootloader/src/network.cpp` - Wi-Fi connection, MQTT connection, subscriptions, and device info publishing.
- `ESP32_Bootloader/src/ota.cpp` - OTA begin, chunk write, ACK, CRC check, end, status, and MQTT callback handling.
- `ESP32_Bootloader/src/home_automation.cpp` - 8 relay channels, button ISRs, RGB PWM control, and automation MQTT handlers.
- `ESP32_Bootloader/src/device_identity.cpp` - ESP32 MAC-based device ID and MQTT topic generation.
- `ESP32_Bootloader/src/led.cpp` - legacy heartbeat helper, not called while GPIO2 is used as relay channel 8.
- `Firmware_PythonFiles/Firmware_Upgrader_V0.2.py` - MQTT OTA uploader GUI.

## Firmware Features

- Controls 8 relay outputs from MQTT or active-LOW physical buttons.
- Publishes relay state as a retained 1-byte bitmask.
- Controls 2 RGB LED strips using 0-100 percent `R,G,B` MQTT payloads.
- Publishes each RGB strip state as retained `R,G,B` CSV text.
- Uses the ESP32 MAC-derived ID as the MQTT topic prefix.
- Publishes retained firmware information on `<deviceID>/info`.
- Responds to OTA readiness checks on `<deviceID>/ota_check`.
- Receives OTA metadata on `<deviceID>/ota/begin`.
- Receives firmware chunks on `<deviceID>/ota/chunk`.
- Publishes per-chunk ACKs on `<deviceID>/ota/ack`.
- Verifies CRC32 before finalising the flash update.
- Publishes OTA status on `<deviceID>/ota_status`.

## MQTT Topics

Automation topics are fixed `home/...` topics.

| Topic | Direction | Payload | Purpose |
| --- | --- | --- | --- |
| `home/relay/status` | ESP32 to broker | 1 raw byte | Retained relay state, bit0 for relay 1 through bit7 for relay 8 |
| `home/relay/set` | Broker to ESP32 | 1 raw byte | Sets all 8 relay states from a bitmask |
| `home/rgb/1/status` | ESP32 to broker | `R,G,B` | Retained RGB strip 1 state, each value from 0 to 100 |
| `home/rgb/1/set` | Broker to ESP32 | `R,G,B` | Sets RGB strip 1 brightness percentages |
| `home/rgb/2/status` | ESP32 to broker | `R,G,B` | Retained RGB strip 2 state, each value from 0 to 100 |
| `home/rgb/2/set` | Broker to ESP32 | `R,G,B` | Sets RGB strip 2 brightness percentages |

OTA topics use the ESP32 MAC-derived device ID as the prefix.

| Topic | Direction | Purpose |
| --- | --- | --- |
| `<deviceID>/info` | ESP32 to broker | Retained firmware version and device ID |
| `<deviceID>/ota_check` | Uploader to ESP32 | Readiness check before upload |
| `<deviceID>/ota_status` | ESP32 to uploader | `READY`, `UPDATING`, `SUCCESS`, or `FAILED` |
| `<deviceID>/ota/begin` | Uploader to ESP32 | Firmware size, chunk count, and CRC32 |
| `<deviceID>/ota/chunk` | Uploader to ESP32 | One firmware chunk with a 4-byte chunk index |
| `<deviceID>/ota/end` | Uploader to ESP32 | Transfer completion command |
| `<deviceID>/ota/ack` | ESP32 to uploader | Acknowledgement for the written chunk index |

## Automation Payloads

Relay command example:

```text
0x01 = relay 1 ON, all others OFF
0xFF = all 8 relays ON
0x00 = all 8 relays OFF
```

RGB command examples:

```text
100,0,50
0,0,0
25,25,100
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
- `MQTT_BUFFER_SIZE` is set to `4096` for OTA chunk packets.
- `WIFI_SSID` and `WIFI_PASSWORD` configure the ESP32 Wi-Fi connection and were kept as already present in code.
- `MQTT_BROKER` and `MQTT_PORT` configure the MQTT broker and were kept as already present in code.

Home automation pin settings are in `ESP32_Bootloader/src/home_automation.cpp`.

| Group | GPIOs |
| --- | --- |
| Buttons | `13, 12, 14, 27, 26, 25, 33, 32` |
| Relay outputs | `23, 22, 21, 19, 18, 5, 4, 2` |
| Relay indicators | `15, 16, 17, 3, 1, 10, 9, 0` |
| RGB 1 | R=`26`, G=`27`, B=`14` |
| RGB 2 | R=`25`, G=`33`, B=`32` |

Important: the supplied pin map reuses some GPIOs between buttons and RGB outputs. It also uses boot, serial, and flash-sensitive GPIOs such as `0`, `1`, `3`, `9`, and `10` for indicators. Adjust the pin arrays before hardware testing if your wiring requires separate safe pins.

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

Python uploader:

- Python 3.8 or newer
- `paho-mqtt`
- Tkinter

## Notes

- The uploader sends one chunk at a time and waits for the ESP32 ACK before continuing.
- The ESP32 skips a duplicate of the last received chunk and re-sends its ACK.
- CRC32 is calculated by the Python uploader and verified by the ESP32 before rebooting.
- GPIO2 is used by relay channel 8, so the old heartbeat LED loop is not called in the merged firmware.
- If OTA ends with `FAILED`, confirm that the selected file is the PlatformIO application `firmware.bin`.
