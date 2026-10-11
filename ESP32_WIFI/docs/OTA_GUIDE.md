# Transport protocols and device tests

## Manager and reporting

IDLE → REQUEST_VALIDATION → RECEIVING → IMAGE_FINALIZATION → IMAGE_VALIDATION → BOOT_SELECTION → REBOOT_PENDING; next boot VERIFYING → CONFIRMED. Failure aborts/cleans up and reaches FAILED without selecting another slot or scheduling restart. New sessions are rejected before startup verification and while an owner is active. Complete size/count/CRC, optional SHA256, native image validation, descriptor and requested version precede native boot selection. Idle timeout is 30 s, total session limit 15 min. Duplicates do not extend the lifetime.

`<ID>/ota/event` carries transient JSON; `<ID>/ota/result` carries durable terminal JSON at QoS 1. Fields include device/session/report IDs, transport/version, current/boot/target slot, status/error/message, received/expected bytes. The eight-entry NVS journal retries after reconnect and deletes a matching entry only after PUBACK. Deduplicate by report_id. New sessions stop with fewer than two available result entries; busy/rejected commands cannot consume the active session's reserved terminal capacity. Sustained rejection flooding beyond capacity is best-effort; finite NVS cannot retain unlimited history. NVS failure prevents guaranteed reporting; required receive/selection intent must persist before updating.

CRC/SHA/native image validation establish integrity, not authenticity on their own. Authenticated metadata with SHA256 binds MQTT-requested downloads to an authorized digest. Configured signed-app/Secure Boot validation is delegated to IDF; the supplied configuration does not enable those policies or anti-rollback.

## Provision security

Choose independent per-device 32-byte HMAC keys, six-digit BLE passkeys, reciprocal station peer MACs and agreed 16-byte ESP-NOW PMK/LMK. Send one newline-terminated JSON object through a private UART0 connection at 115200:

```json
{"provision_ota":true,"ota_key":"64_HEX_DIGITS","ble_pin":123456,"role":0,"peer_mac":"12_HEX_DIGITS","pmk":"32_HEX_DIGITS","lmk":"32_HEX_DIGITS"}
```

Replace placeholders and the illustrative passkey. Role 0 receives; role 1 coordinates. Peer MAC is normal esptool station MAC, not reversed application ID. Saving writes `otacfg` and restarts; keys/passkey are not logged. Unprovisioned BLE/ESP-NOW remain disabled. Serial provisioning is physical-access configuration and makes no eFuse changes. Secure Boot/flash encryption provisioning is separate, revision-dependent, potentially irreversible and must follow Espressif's documented process.

## MQTT

Legacy topics: begin `<ID>/ota/begin` JSON `{size,chunks,crc32}`; chunk `<ID>/ota/chunk` is uint32 big-endian sequence starting at zero then bytes; `<ID>/ota/ack` is decimal sequence; end `<ID>/ota/end` accepts existing `END`. Readiness `<ID>/ota_check` returns READY/discovery. `<ID>/ota_status` preserves READY/UPDATING/FAILED/SUCCESS. SUCCESS means boot selected; only detailed confirmed means healthy startup. Existing Python V0.2 GUI remains usable. Legacy lacks wire session/authentication; disable it for protected deployments.

Extended `<ID>/ota/request` is plain or ENC1 JSON with `op`, monotonic integer `nonce`, hex HMAC `auth`. Canonical UTF-8 is `op|nonce|size|chunks|crc32|sha256|version|url|session_id`; absent numbers use 0, absent strings empty. `protocol.signed_request()` creates it. Nonce is committed before executing; use unique keys, accurate laptop clock and integers below 2^53. Operations: http, mqtt, status, cancel, espnow_send_running. HTTP/MQTT requests require SHA256 (the laptop supplies it), and may include chip=esp32 and descriptor version.

V2 chunks on `<ID>/ota/v2/chunk` contain session uint32 + sequence uint32 + image bytes, all big-endian. ACK `<ID>/ota/v2/ack` JSON contains both IDs. Completion `<ID>/ota/v2/end` is exactly the 4-byte session. Get the session from started. Laptop uses 7168-byte chunks and eight ACK retries. Foreign/stale session IDs cannot write.

`run.py mqtt --fault missing|duplicate|timeout|disconnect|incomplete|corrupt|wrong-size|concurrent` supports faults. Duplicate/concurrent expect original transfer success, with second request OTA_BUSY. Other faults expect failure and compare boot/current slots when a key enables snapshots. Laptop disconnect disconnects the test subscriber and waits for device timeout, then reconnects to inspect persistence; it does not disconnect the board's separate MQTT connection. Test board/broker outage by interrupting only the target's network access, restoring it and checking failure replay.

## HTTP/HTTPS

Run README server command. Find laptop Wi-Fi address with `ipconfig`, use the same trusted LAN without client isolation, allow inbound TCP 8000 on the private-network firewall and use the laptop's address, not localhost. Plain HTTP requires menuconfig `CONFIG_OTA_ALLOW_DEV_HTTP=y` and rebuild. HTTPS uses the certificate bundle; add a private CA deliberately if needed, never disable verification. Redirects/non-200/credential-bearing URLs are rejected. Metadata chunks=ceil(size/1024). Fixed/chunked responses stream to flash; incomplete/excess bodies fail.

Fixtures: valid.bin, missing.bin (404), status.bin (503), truncated.bin (short body/length), interrupted.bin (full advertised length, connection dropped), corrupt.bin (changed first byte). Use `run.py http ... --url http://LAPTOP_IP:8000/FILE --expect failed` except valid expects confirmed. The harness compares snapshots. Run valid twice for A→B→A; inspect detailed logs and JSON.

## BLE on iOS/Android with nRF Connect

Discover `ESP32-OTA-<last six ID digits>` after provisioning/reboot. Pair with provisioned passkey when prompted; encrypted authenticated access is mandatory. Android may need Bluetooth/Nearby Devices permission. UUID base is `-b5a3-f393-e0a9-e50e24dcca9e`: service 8e400001, control 8e400002 (JSON write with response), data 8e400003 (binary write with response), status 8e400004 (read/notify).

Subscribe to status, negotiate MTU, then write `{"op":"start","size":N,"chunks":N,"crc32":N}` (optional SHA256/version/chip). Status is four big-endian uint32s: session, sequence, ESP error (0 success), operation (1 start, 2 data, 3 finish, 4 abort). Save session; data writes contain session+sequence (8 bytes) then firmware bytes, each normal payload at most MTU−3−8. Attribute values cannot exceed 512 bytes; larger control JSON needs a supported GATT long write. Wait for each ACK; read status if notification unavailable. Finish `{"op":"finish","session_id":SESSION}`, abort `{"op":"abort","session_id":SESSION}`. Finish ACK is boot selection, not confirmation.

Initial phone check: pair, subscribe, start a small partial transfer, inspect ACK, send a data packet, disconnect. Verify BLE_DISCONNECTED and unchanged boot/current slots through MQTT. Check wrong passkey, out-of-order chunks, premature finish and competing session rejection. Bulk upload can use optional `pip install bleak` and `run.py ble FIRMWARE --address ADDRESS`; requires compatible OS adapter and prior pairing. `--disconnect` tests adapter disconnect; inspect failure/slot separately. Complete phone firmware upload is not claimed from an inspection app alone.

## ESP-NOW: same firmware, configured roles

Use identical application on coordinator role 1 and target role 0, reciprocal peers and matching PMK/LMK. Connect both to the same router/channel. Trigger coordinator with `run.py espnow-send --device COORDINATOR_ID --key-file KEY`. It sends its verified **running image**; update coordinator first with HTTP/MQTT/BLE to make a newer image available.

Frames fit ESP-NOW v1 250-byte limits. Header: ASCII OT, version byte 1, operation byte, session uint32, sequence uint32 (12 bytes, big-endian). START=1/DATA=2/END=3/ABORT=4/ACK=5. DATA≤224 bytes. START sequence=0xffffffff; END=0xfffffffe; ABORT=0xfffffffd. START metadata 77 bytes: size/chunks/CRC uint32, 32-byte NUL-terminated version, 32-byte SHA256, has-SHA byte. ACK adds uint32 ESP result. Sender retries eight times at 600 ms. Last unchanged duplicate data and completed END retries are ACKed without extra writes/selections; receiver gives a 6 s reboot window. Missing/order errors abort. Callback only queues; main task writes.

`simulate-espnow` is a stop-and-wait model; native C tests cover shared acceptance/failure logic, not radio behavior. Real two-board test: snapshot target, forward, record selected/confirmed and slots, repeat. Remove/power off only sender during receipt; target must timeout without selecting. Test channel mismatch, retry/duplicate behavior and dropped END ACKs. After final END the coordinator alone cannot prove target outcome; use target's startup result. Signed running-image forwarding needs separate validation: native image length may omit padded signature trailers. Use HTTP/MQTT/BLE for signed artifacts until that sender path is provisioned/tested.
