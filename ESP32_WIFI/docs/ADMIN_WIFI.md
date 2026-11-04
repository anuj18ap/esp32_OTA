# Admin and User Wi-Fi

The ESP32 target uses ESP-IDF 5.4.1. The Wi-Fi manager is the only caller of
`esp_wifi_scan_start`, `esp_wifi_connect`, `esp_wifi_disconnect`, and
`esp_wifi_set_config`. It uses station mode only; the Admin network is an external
router or hotspot with firmware-defined credentials.

At boot, the RAM-only forced flag is false. The manager scans for Admin for up
to 3 seconds. If absent, or if association/DHCP fails within 15 seconds, it
scans for User for up to 10 seconds. A failed User attempt returns to Admin
search. Scan and connection deadlines are separate. Once an IP is acquired,
the selected profile stays connected. Admin hosts the configuration page and
does not start MQTT. User runs the existing MQTT/OTA/home automation path.
When an established connection drops, automatic mode starts Admin search
again. Force User acknowledges the HTTP command, waits briefly, disconnects,
and retries User only. A device reset clears this runtime flag without erasing
NVS settings.

Open the Admin DHCP IP printed in the serial log. The page asks for a separate
Admin webpage password, set with `CONFIG_ADMIN_WEB_PASSWORD` in menuconfig.
Use at least 12 characters and change its default before deployment. The page and JSON APIs are restricted
to the Admin station IP and require the `X-Admin-Password` header. The header
prevents browser form CSRF; passwords are never returned by APIs. The server
uses plain HTTP on the trusted Admin network, so a network observer can read
traffic. Commercial deployments should add HTTPS, per-device credential
provisioning/rotation, flash encryption, and NVS encryption. The configured
topic prefix applies to device MQTT topics; leave it empty for the legacy topic
layout.

User credentials keep the existing `appcfg` NVS keys. A blank password field
preserves the saved password; selecting Open clears it. MQTT settings are kept
in the versioned `mqtt_v1` blob in the same namespace. The existing application
MQTT client is reconfigured while stopped and started only with User IP.
Updating settings from the Admin page does not disconnect Admin.

## Hardware checks

Run these on an actual ESP32 and inspect the serial log and webpage:

1. Both routers available: Admin wins; HTTP works; MQTT stays stopped.
2. Only User available: Admin scan expires, User gets DHCP, MQTT starts.
3. Neither available: repeated 3 second Admin and 10 second User searches.
4. Wrong Admin password: connection timeout/failure advances to User.
5. Save User and MQTT settings, reset, and confirm NVS persistence.
6. Scan and check the saved SSID: FOUND gives the strongest matching AP;
   absent or failed scans remain distinct. Confirm a hidden SSID caveat.
7. Force User, disconnect the User router, and bring Admin online: retries
   stay on User. Reset restores Admin priority.
8. Check repeated button clicks, malformed JSON, oversized requests, and
   access from the User network. Confirm no configuration API is reachable.
9. Verify MQTT reconnect, OTA reporting, relay/RGB operation, and local
   device functions in User mode.

The build check does not establish radio behavior, DHCP, browser reachability,
or MQTT broker acceptance. No hardware was flashed as part of this change.
