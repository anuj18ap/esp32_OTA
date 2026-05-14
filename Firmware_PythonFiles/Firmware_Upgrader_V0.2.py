"""
================================================================
 ESP32 OTA Firmware Upgrader — MQTT Only (No HTTP Server)
================================================================
 Dependencies:
     pip install paho-mqtt

 Compatible with:
     paho-mqtt  2.x
     Python     3.8+

 Usage:
     python esp32_ota_mqtt.py
================================================================
"""

import os
import json
import struct
import threading
import time
import zlib

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import paho.mqtt.client as mqtt

# ================================================================
#  CONFIGURATION
# ================================================================

MQTT_BROKER  = "broker.hivemq.com"
MQTT_PORT    = 1883
CHUNK_SIZE   = 3072   # bytes per chunk  (safe under 4096 MQTT packet limit)
ACK_TIMEOUT  = 5      # seconds to wait for ESP32 ACK before retry
MAX_RETRIES  = 3      # how many times to retry a single chunk before failing

# ================================================================
#  APPLICATION STATE
# ================================================================

firmware_path  = None   # Absolute path to selected .bin file
target_mac     = None   # MAC ID entered by the user
upload_thread  = None   # Background OTA upload thread

# ACK synchronisation — signalled by _on_message when ACK arrives
ack_event     = threading.Event()
current_chunk = 0       # chunk index we are currently waiting ACK for

# ================================================================
#  MQTT SETUP
# ================================================================

def _on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print("[MQTT] Connected to broker.")
    else:
        print(f"[MQTT] Connection failed — reason code: {reason_code}")


def _on_disconnect(client, userdata, flags, reason_code, properties):
    print(f"[MQTT] Disconnected (reason: {reason_code}). Auto-reconnect active.")


def _on_message(client, userdata, msg):
    """
    Dispatch incoming MQTT messages.
    All Tkinter updates are marshalled via root.after() to stay on main thread.
    """
    topic   = msg.topic
    payload = msg.payload.decode(errors="replace")
    print(f"[MQTT] RX  topic={topic}  payload={payload[:80]}")

    # ── ACK from ESP32 for a chunk ──────────────────────────────
    if topic.endswith("/ota/ack"):
        try:
            acked_index = int(payload)
            if acked_index == current_chunk:
                ack_event.set()   # unblock the upload worker thread
        except ValueError:
            pass
        return

    # ── Status messages from ESP32 ──────────────────────────────
    status_map = {
        "READY"    : ("ESP32 READY FOR OTA",        "#27ae60"),
        "UPDATING" : ("ESP32 UPDATING\u2026",        "#e67e22"),
        "FAILED"   : ("OTA FAILED",                  "#e74c3c"),
        "NO_UPDATE": ("NO UPDATE AVAILABLE",          "#95a5a6"),
        "SUCCESS"  : ("OTA SUCCESS \u2713 REBOOTING","#27ae60"),
    }

    if payload in status_map:
        text, colour = status_map[payload]
        root.after(0, lambda t=text, c=colour: _update_status(t, c))

        if payload == "READY":
            root.after(0, lambda: upload_btn.config(state=tk.NORMAL))

        if payload in ("SUCCESS", "FAILED", "NO_UPDATE"):
            root.after(0, lambda: _set_controls_enabled(True))


def _build_mqtt_client() -> mqtt.Client:
    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2
    )
    client.on_connect    = _on_connect
    client.on_disconnect = _on_disconnect
    client.on_message    = _on_message

    client.reconnect_delay_set(min_delay=1, max_delay=60)
    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    client.loop_start()
    return client


mqtt_client = _build_mqtt_client()


# ================================================================
#  GUI HELPERS
# ================================================================

def _update_status(text: str, colour: str = "#2c3e50") -> None:
    """Update the status label — call only from main thread."""
    status_label.config(text=text, fg=colour)


def _update_progress(pct: int, done: int, total: int) -> None:
    """Update progress bar and chunk counter — call only from main thread."""
    progress_bar["value"] = pct
    progress_label.config(text=f"Chunk {done} / {total}  ({pct}%)")


def _set_controls_enabled(enabled: bool) -> None:
    state = tk.NORMAL if enabled else tk.DISABLED
    check_btn.config(state=state)
    firmware_btn.config(state=state)
    mac_entry.config(state=state)


# ================================================================
#  BUTTON CALLBACKS
# ================================================================

def cb_check_device() -> None:
    """Probe the ESP32 via MQTT to confirm it is alive and ready."""
    global target_mac

    mac = mac_entry.get().strip().upper()
    if not mac:
        messagebox.showerror("Missing MAC", "Please enter the ESP32 MAC ID.")
        return

    target_mac = mac
    upload_btn.config(state=tk.DISABLED)

    status_topic = f"{target_mac}/ota_status"
    check_topic  = f"{target_mac}/ota_check"
    ack_topic    = f"{target_mac}/ota/ack"

    # Subscribe to status AND ack topics before sending probe
    mqtt_client.subscribe(status_topic, qos=1)
    mqtt_client.subscribe(ack_topic,    qos=1)
    mqtt_client.publish(check_topic, "ARE_YOU_READY", qos=1)

    print(f"[APP] Checking device: {target_mac}")
    _update_status("Waiting for ESP32\u2026", "#7f8c8d")

    # Reset progress bar
    progress_bar["value"] = 0
    progress_label.config(text="")


def cb_select_firmware() -> None:
    """Open file dialog to choose a .bin firmware file."""
    global firmware_path

    path = filedialog.askopenfilename(
        title="Select ESP32 Firmware (.bin)",
        filetypes=[("Firmware binary", "*.bin"), ("All files", "*.*")],
    )

    if path:
        firmware_path = path
        firmware_label.config(text=os.path.basename(path), fg="#27ae60")
        print(f"[APP] Firmware selected: {path}")


def cb_upload_firmware() -> None:
    """Validate inputs then launch the upload worker in a background thread."""
    if not target_mac:
        messagebox.showerror("No Device", "Check a device first.")
        return
    if not firmware_path:
        messagebox.showerror("No Firmware", "Select a firmware file first.")
        return

    _set_controls_enabled(False)
    upload_btn.config(state=tk.DISABLED)
    progress_bar["value"] = 0
    progress_label.config(text="Starting\u2026")

    t = threading.Thread(target=_upload_worker, daemon=True, name="OTA-Upload")
    t.start()


# ================================================================
#  OTA UPLOAD WORKER  (runs in background thread)
# ================================================================

def _upload_worker() -> None:
    """
    Reads the .bin file, splits into CHUNK_SIZE chunks, publishes
    each chunk over MQTT and waits for an ACK before continuing.
    Retries up to MAX_RETRIES times per chunk on timeout.
    """
    global current_chunk

    try:
        # ── 1. Read firmware ────────────────────────────────────
        with open(firmware_path, "rb") as f:
            firmware_data = f.read()

        total_size   = len(firmware_data)
        total_chunks = (total_size + CHUNK_SIZE - 1) // CHUNK_SIZE
        crc32_val    = zlib.crc32(firmware_data) & 0xFFFFFFFF

        print(f"[OTA] Size={total_size}B  Chunks={total_chunks}  CRC=0x{crc32_val:08X}")

        # ── 2. Subscribe to OTA topics ──────────────────────────
        mqtt_client.subscribe(f"{target_mac}/ota/begin",  qos=1)
        mqtt_client.subscribe(f"{target_mac}/ota/chunk",  qos=1)
        mqtt_client.subscribe(f"{target_mac}/ota/end",    qos=1)

        # ── 3. Publish BEGIN metadata ───────────────────────────
        begin_payload = json.dumps({
            "size"  : total_size,
            "chunks": total_chunks,
            "crc32" : crc32_val,
        })
        mqtt_client.publish(f"{target_mac}/ota/begin", begin_payload, qos=1)
        root.after(0, lambda: _update_status("OTA Started \u2014 sending chunks\u2026", "#2980b9"))

        # Give ESP32 time to call Update.begin() before first chunk arrives
        time.sleep(1.5)

        # ── 4. Send chunks with ACK handshake ───────────────────
        for i in range(total_chunks):
            chunk_data = firmware_data[i * CHUNK_SIZE : (i + 1) * CHUNK_SIZE]

            # Packet format: [4-byte big-endian index] + [chunk bytes]
            packet = struct.pack(">I", i) + chunk_data

            retries = 0
            success = False

            while retries < MAX_RETRIES:
                current_chunk = i
                ack_event.clear()

                mqtt_client.publish(f"{target_mac}/ota/chunk", packet, qos=1)

                if ack_event.wait(timeout=ACK_TIMEOUT):
                    success = True
                    break
                else:
                    retries += 1
                    print(f"[OTA] Chunk {i} timeout — retry {retries}/{MAX_RETRIES}")

            if not success:
                root.after(0, lambda ci=i: _update_status(
                    f"FAILED: no ACK for chunk {ci}", "#e74c3c"
                ))
                root.after(0, lambda: _set_controls_enabled(True))
                return

            # Update progress on main thread
            pct = int(((i + 1) / total_chunks) * 100)
            root.after(0, lambda p=pct, ci=i + 1, ct=total_chunks:
                _update_progress(p, ci, ct))

        # ── 5. Publish END ──────────────────────────────────────
        mqtt_client.publish(f"{target_mac}/ota/end", "END", qos=1)
        root.after(0, lambda: _update_status(
            "All chunks sent \u2014 waiting for ESP32 to verify\u2026", "#27ae60"
        ))

    except FileNotFoundError:
        root.after(0, lambda: _update_status("ERROR: Firmware file not found", "#e74c3c"))
        root.after(0, lambda: _set_controls_enabled(True))

    except Exception as exc:
        print(f"[OTA] Exception: {exc}")
        root.after(0, lambda e=str(exc): _update_status(f"ERROR: {e}", "#e74c3c"))
        root.after(0, lambda: _set_controls_enabled(True))


# ================================================================
#  GUI LAYOUT
# ================================================================

def _build_gui() -> tk.Tk:
    win = tk.Tk()
    win.title("ESP32 OTA Upgrader — MQTT Only")
    win.geometry("500x460")
    win.resizable(False, False)
    win.configure(bg="#1c2333")

    # ── Fonts ───────────────────────────────────────────────────
    font_title  = ("Consolas", 15, "bold")
    font_label  = ("Consolas", 10)
    font_entry  = ("Consolas", 11)
    font_status = ("Consolas", 11, "bold")
    font_btn    = ("Consolas", 10, "bold")

    # ── Colours ─────────────────────────────────────────────────
    BG       = "#1c2333"
    FG       = "#ecf0f1"
    ACCENT   = "#3498db"
    BTN_BG   = "#2c3e50"
    BTN_FG   = "#ecf0f1"
    ENTRY_BG = "#2c3e50"

    # ── Title bar ───────────────────────────────────────────────
    title_frame = tk.Frame(win, bg=ACCENT, height=46)
    title_frame.pack(fill=tk.X)
    tk.Label(
        title_frame,
        text="  \u2b21  ESP32 OTA Upgrader \u2014 MQTT Only",
        font=font_title, bg=ACCENT, fg="white", anchor="w",
    ).pack(side=tk.LEFT, padx=12, pady=10)

    # ── Body ────────────────────────────────────────────────────
    body = tk.Frame(win, bg=BG, padx=30, pady=20)
    body.pack(fill=tk.BOTH, expand=True)

    # ── MAC ID row ──────────────────────────────────────────────
    tk.Label(body, text="ESP32 MAC ID", font=font_label, bg=BG, fg=FG, anchor="w")\
        .pack(fill=tk.X, pady=(0, 4))

    mac_row = tk.Frame(body, bg=BG)
    mac_row.pack(fill=tk.X, pady=(0, 10))

    global mac_entry, check_btn
    mac_entry = tk.Entry(
        mac_row, width=26,
        font=font_entry, bg=ENTRY_BG, fg=FG,
        insertbackground=FG, relief=tk.FLAT, bd=6,
    )
    mac_entry.pack(side=tk.LEFT, padx=(0, 10))
    mac_entry.insert(0, "0CF73625BF58")

    check_btn = tk.Button(
        mac_row, text="Check Device",
        font=font_btn, bg=ACCENT, fg="white",
        relief=tk.FLAT, padx=12, pady=4,
        cursor="hand2", command=cb_check_device,
    )
    check_btn.pack(side=tk.LEFT)

    # ── Separator ───────────────────────────────────────────────
    ttk.Separator(body, orient="horizontal").pack(fill=tk.X, pady=10)

    # ── Status label ────────────────────────────────────────────
    global status_label
    status_label = tk.Label(
        body, text="Device Not Checked",
        font=font_status, bg=BG, fg="#7f8c8d", anchor="w",
    )
    status_label.pack(fill=tk.X, pady=(0, 8))

    # ── Progress bar ────────────────────────────────────────────
    global progress_bar, progress_label
    progress_label = tk.Label(
        body, text="",
        font=font_label, bg=BG, fg=FG, anchor="w",
    )
    progress_label.pack(fill=tk.X, pady=(0, 4))

    style = ttk.Style()
    style.theme_use("default")
    style.configure(
        "OTA.Horizontal.TProgressbar",
        troughcolor="#2c3e50",
        background="#27ae60",
        thickness=14,
    )
    progress_bar = ttk.Progressbar(
        body, orient="horizontal",
        length=440, mode="determinate",
        style="OTA.Horizontal.TProgressbar",
    )
    progress_bar.pack(fill=tk.X, pady=(0, 10))

    # ── Separator ───────────────────────────────────────────────
    ttk.Separator(body, orient="horizontal").pack(fill=tk.X, pady=6)

    # ── Firmware row ────────────────────────────────────────────
    fw_row = tk.Frame(body, bg=BG)
    fw_row.pack(fill=tk.X, pady=(8, 4))

    global firmware_btn, firmware_label
    firmware_btn = tk.Button(
        fw_row, text="Select Firmware",
        font=font_btn, bg=BTN_BG, fg=BTN_FG,
        relief=tk.FLAT, padx=12, pady=4,
        cursor="hand2", command=cb_select_firmware,
    )
    firmware_btn.pack(side=tk.LEFT, padx=(0, 14))

    firmware_label = tk.Label(
        fw_row, text="No firmware selected",
        font=font_label, bg=BG, fg="#7f8c8d", anchor="w",
    )
    firmware_label.pack(side=tk.LEFT)

    # ── Upload button ────────────────────────────────────────────
    global upload_btn
    upload_btn = tk.Button(
        body,
        text="\u2b06  Upload Firmware via MQTT",
        font=font_btn, bg="#27ae60", fg="white",
        relief=tk.FLAT, padx=16, pady=8,
        cursor="hand2", state=tk.DISABLED,
        command=cb_upload_firmware,
    )
    upload_btn.pack(pady=18)

    # ── Footer ───────────────────────────────────────────────────
    tk.Label(
        win,
        text=f"MQTT: {MQTT_BROKER}:{MQTT_PORT}   |   Chunk: {CHUNK_SIZE}B   |   ACK timeout: {ACK_TIMEOUT}s",
        font=("Consolas", 8),
        bg="#111827", fg="#4b5563",
    ).pack(fill=tk.X, side=tk.BOTTOM, pady=4)

    return win


# ================================================================
#  ENTRY POINT
# ================================================================

if __name__ == "__main__":
    root = _build_gui()
    root.mainloop()

    mqtt_client.loop_stop()
    mqtt_client.disconnect()