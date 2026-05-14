# ESP32 MQTT OTA Firmware Upgrader

## Overview

This project updates ESP32 application firmware over MQTT. The ESP32 firmware connects to Wi-Fi, subscribes to device-specific OTA topics, receives a PlatformIO-generated `.bin` file in chunks, verifies the CRC32, writes the image using the ESP32 `Update` API, and restarts after a successful update.

The desktop uploader in `Firmware_PythonFiles` provides a small Tkinter GUI that checks the ESP32 status, sends OTA metadata, streams firmware chunks, waits for an ACK after each chunk, and displays upload progress.

## Project Structure

- `ESP32_Bootloader/` - PlatformIO ESP32 Arduino firmware project.
- `ESP32_Bootloader/src/main.cpp` - startup, loop, MQTT client object, topics, and shared OTA state.
- `ESP32_Bootloader/src/network.cpp` - Wi-Fi connection, MQTT connection, subscriptions, and device info publishing.
- `ESP32_Bootloader/src/ota.cpp` - OTA begin, chunk write, ACK, CRC check, end, status, and MQTT callback handling.
- `ESP32_Bootloader/src/device_identity.cpp` - ESP32 MAC-based device ID and MQTT topic generation.
- `ESP32_Bootloader/src/led.cpp` - onboard LED heartbeat.
- `Firmware_PythonFiles/Firmware_Upgrader_V0.2.py` - MQTT OTA uploader GUI.

## Firmware Features

- Uses the ESP32 MAC-derived ID as the MQTT topic prefix.
- Publishes retained firmware information on `<deviceID>/info`.
- Responds to OTA readiness checks on `<deviceID>/ota_check`.
- Receives OTA metadata on `<deviceID>/ota/begin`.
- Receives firmware chunks on `<deviceID>/ota/chunk`.
- Publishes per-chunk ACKs on `<deviceID>/ota/ack`.
- Verifies CRC32 before finalising the flash update.
- Publishes OTA status on `<deviceID>/ota_status`.

## MQTT Topics

| Topic | Direction | Purpose |
| --- | --- | --- |
| `<deviceID>/info` | ESP32 to broker | Retained firmware version and device ID |
| `<deviceID>/ota_check` | Uploader to ESP32 | Readiness check before upload |
| `<deviceID>/ota_status` | ESP32 to uploader | `READY`, `UPDATING`, `SUCCESS`, or `FAILED` |
| `<deviceID>/ota/begin` | Uploader to ESP32 | Firmware size, chunk count, and CRC32 |
| `<deviceID>/ota/chunk` | Uploader to ESP32 | One firmware chunk with a 4-byte chunk index |
| `<deviceID>/ota/end` | Uploader to ESP32 | Transfer completion command |
| `<deviceID>/ota/ack` | ESP32 to uploader | Acknowledgement for the written chunk index |

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
- `WIFI_SSID` and `WIFI_PASSWORD` configure the ESP32 Wi-Fi connection.
- `MQTT_BROKER` and `MQTT_PORT` configure the MQTT broker.

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
- If OTA ends with `FAILED`, confirm that the selected file is the PlatformIO application `firmware.bin`.
