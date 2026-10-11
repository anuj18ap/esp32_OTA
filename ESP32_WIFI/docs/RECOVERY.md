# Recovery and acceptance checklist

ESP-IDF's standard bootloader supplies redundant otadata sectors and pending-image rollback. Application logic supplies transfer checks, ownership, aborts, durable results and startup diagnostics. References: [IDF 5.4.1 OTA](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32/api-reference/system/ota.html), [ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32/api-reference/network/esp_now.html).

| Interruption/failure | Boot and recovery behavior |
|---|---|
| Before begin | Existing selection; no new result |
| Inactive erase | Existing selection; persisted receive intent reports interrupted transfer after restart |
| Reception | Existing selection; partial inactive image can be overwritten |
| Flash writing | Same; running slot is never the write target |
| Validation before selection | Existing selection; native set-boot has not succeeded |
| During set-boot metadata write | Bootloader resolves redundant sectors; physical interruption can leave old or validated-new selection |
| After selection before restart | New validated image starts; pending verification/rollback apply |
| New app starting, reset before confirmation | Bootloader marks pending image aborted and uses previous eligible image |
| Repeated crashes before confirmation | One pending-verification attempt; previous eligible image resumes |
| MQTT connection lost | MQTT transfer aborts, app remains running, failure replays after reconnect; HTTP need not stop solely for broker outage |
| BLE disconnect | Safe abort; boot unchanged; later MQTT result |
| ESP-NOW sender failure/loss | Retries/timeout; receiver aborts; sender cannot prove receiver outcome if final ACK lost |
| HTTP interruption | Client cleanup and manager abort; unchanged boot selection |
| Boot-selection API failure | Report BOOT_PARTITION_FAILED; no intentional update restart |
| Final result unavailable | Keep successful boot decision; persist/replay QoS 1 result, deduplicate report_id |
| NVS write fails during startup reporting | Preserve interrupted-transfer or boot intent until its result is persisted; retry on subsequent startup after storage recovery |

Rollback requires a previous eligible application and rollback-enabled bootloader. First serial installation has no previously confirmed second image. A corrupt flash or absent fallback cannot be repaired by rollback. Failed rollback API calls are logged. Rollback does not undo NVS/schema changes. Broker reachability is not used as a health test, so router outages do not reject otherwise healthy firmware. Startup tests verify initialization/CRC/crypto and 10 seconds of stable execution, not physical relay contacts or loads; add critical device-specific diagnostics if required.

Hardware acceptance:

- Back up board; verify capacity/layout; serial-install standard bootloader/table; establish an eligible previous image.
- Verify original GUI discovery, ID/name, Wi-Fi request/update/reset, encrypted logs, relay masks/RGB JSON and ACKs, GPIO boot test and status LEDs.
- Run two valid updates and record A→B→A with confirmed results.
- Test invalid images, bad CRC/SHA, wrong size/order, missing chunks, timeout, cancellation and concurrent requests; compare boot/current slots and continued app operation.
- Fail a transfer with board network unavailable, reconnect and verify durable failure replay/PUBACK behavior.
- Test BLE authenticated pairing and mid-transfer disconnect on iOS/Android.
- Test two-board ESP-NOW radio transfer/loss/retries/duplicates/channel mismatch and sender outage.
- Upload a controlled deliberately failing startup test build once; record bootloader rollback and old slot/result. No firmware changes per ordinary laptop fault test are needed.
- Interrupt only target ESP32 power through a controlled isolated switch at erase/write/validation/selection/startup stages, collecting logs. Never cut laptop or unrelated-device power.
- Validate signed-image rejection separately after deliberate security provisioning. No eFuses are changed by this port.

Unchecked items remain hardware-required. No firmware was flashed during this session; mocked tests do not prove physical rollback or power-loss recovery.
