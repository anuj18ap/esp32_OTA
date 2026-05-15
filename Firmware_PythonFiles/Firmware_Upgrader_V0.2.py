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

MQTT_BROKER  = "broker.emqx.io"
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
mqtt_client    = None   # Created after GUI is ready

# ACK synchronisation — thread-safe
_ack_lock     = threading.Lock()
ack_event     = threading.Event()
current_chunk = 0       # chunk index we are currently waiting ACK for

# ================================================================
#  MQTT STATUS STATES
# ================================================================

MQTT_STATE_CONNECTING   = "connecting"
MQTT_STATE_CONNECTED    = "connected"
MQTT_STATE_DISCONNECTED = "disconnected"
MQTT_STATE_FAILED       = "failed"

# ================================================================
#  MQTT CALLBACKS
# ================================================================

def _on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print("[MQTT] Connected to broker.")
        root.after(0, lambda: _set_mqtt_status(MQTT_STATE_CONNECTED))
    else:
        print(f"[MQTT] Connection failed — reason code: {reason_code}")
        root.after(0, lambda: _set_mqtt_status(MQTT_STATE_FAILED))


def _on_disconnect(client, userdata, flags, reason_code, properties):
    print(f"[MQTT] Disconnected (reason: {reason_code}). Auto-reconnect active.")
    root.after(0, lambda: _set_mqtt_status(MQTT_STATE_DISCONNECTED))


def _on_message(client, userdata, msg):
    """
    Dispatch incoming MQTT messages.
    All Tkinter updates are marshalled via root.after() to stay on main thread.
    """
    topic   = msg.topic
    payload = msg.payload.decode(errors="replace")

    # ── ACK from ESP32 for a chunk ──────────────────────────────
    if topic.endswith("/ota/ack"):
        try:
            acked_index = int(payload)
            with _ack_lock:
                if acked_index == current_chunk:
                    ack_event.set()
        except ValueError:
            pass
        return

    # ── Status messages from ESP32 ──────────────────────────────
    status_map = {
        "READY"    : ("ESP32 READY FOR OTA",          "#27ae60"),
        "UPDATING" : ("ESP32 UPDATING\u2026",          "#e67e22"),
        "FAILED"   : ("OTA FAILED",                    "#e74c3c"),
        "NO_UPDATE": ("NO UPDATE AVAILABLE",            "#95a5a6"),
        "SUCCESS"  : ("OTA SUCCESS \u2713 REBOOTING",  "#27ae60"),
    }

    if payload in status_map:
        text, colour = status_map[payload]
        root.after(0, lambda t=text, c=colour: _update_status(t, c))

        if payload == "READY":
            root.after(0, lambda: upload_btn.config(state=tk.NORMAL))

        if payload in ("SUCCESS", "FAILED", "NO_UPDATE"):
            root.after(0, lambda: _set_controls_enabled(True))


# ================================================================
#  MQTT CONNECT  (called after GUI exists)
# ================================================================

def _connect_mqtt() -> None:
    """Build and connect the MQTT client in a background thread so the
    GUI window opens instantly without blocking on network I/O."""
    global mqtt_client

    def _do_connect():
        global mqtt_client
        root.after(0, lambda: _set_mqtt_status(MQTT_STATE_CONNECTING))
        try:
            client = mqtt.Client(
                callback_api_version=mqtt.CallbackAPIVersion.VERSION2
            )
            client.on_connect    = _on_connect
            client.on_disconnect = _on_disconnect
            client.on_message    = _on_message
            client.reconnect_delay_set(min_delay=1, max_delay=60)
            client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
            client.loop_start()
            mqtt_client = client
        except Exception as exc:
            print(f"[MQTT] Initial connect error: {exc}")
            root.after(0, lambda: _set_mqtt_status(MQTT_STATE_FAILED))

    threading.Thread(target=_do_connect, daemon=True, name="MQTT-Connect").start()


# ================================================================
#  MQTT STATUS INDICATOR HELPERS
# ================================================================

# Pulsing animation state
_pulse_after_id  = None
_pulse_alpha_dir = 1       # +1 growing, -1 shrinking
_pulse_alpha     = 0.0     # 0.0 – 1.0

def _set_mqtt_status(state: str) -> None:
    """Update the MQTT status dot + label in the status bar. Main thread only."""
    global _pulse_after_id

    # Stop any existing pulse animation
    if _pulse_after_id is not None:
        root.after_cancel(_pulse_after_id)
        _pulse_after_id = None

    if state == MQTT_STATE_CONNECTING:
        mqtt_dot.config(fg="#e67e22")          # orange
        mqtt_status_lbl.config(text=f"Connecting to MQTT_BROKER…", fg="#e67e22")
        _start_pulse()

    elif state == MQTT_STATE_CONNECTED:
        mqtt_dot.config(fg="#2ecc71")          # green
        mqtt_status_lbl.config(text=f"Connected  ·  {MQTT_BROKER}:{MQTT_PORT}", fg="#2ecc71")

    elif state == MQTT_STATE_DISCONNECTED:
        mqtt_dot.config(fg="#e74c3c")          # red
        mqtt_status_lbl.config(text="Disconnected — reconnecting…", fg="#e74c3c")
        _start_pulse()

    elif state == MQTT_STATE_FAILED:
        mqtt_dot.config(fg="#e74c3c")
        mqtt_status_lbl.config(text=f"Failed to connect to {MQTT_BROKER}", fg="#e74c3c")


def _start_pulse() -> None:
    """Animate the dot by cycling between two brightness levels."""
    global _pulse_after_id, _pulse_alpha, _pulse_alpha_dir

    _pulse_alpha     = 0.0
    _pulse_alpha_dir = 1
    _pulse_after_id  = root.after(40, _pulse_tick)


_PULSE_PAIRS = [
    ("#e67e22", "#f39c12"),   # orange dim / bright
    ("#e74c3c", "#ff6b6b"),   # red dim / bright
]
_pulse_pair_idx = 0


def _pulse_tick() -> None:
    global _pulse_after_id, _pulse_alpha, _pulse_alpha_dir, _pulse_pair_idx

    if _pulse_after_id is None:
        return

    _pulse_alpha += 0.06 * _pulse_alpha_dir
    if _pulse_alpha >= 1.0:
        _pulse_alpha     = 1.0
        _pulse_alpha_dir = -1
    elif _pulse_alpha <= 0.0:
        _pulse_alpha     = 0.0
        _pulse_alpha_dir = 1

    # Choose colour pair based on current dot colour
    current_fg = mqtt_dot.cget("fg")
    if current_fg in ("#e67e22", "#f39c12"):
        dim, bright = "#e67e22", "#f39c12"
    else:
        dim, bright = "#c0392b", "#e74c3c"

    # Lerp between dim and bright
    colour = _lerp_hex(dim, bright, _pulse_alpha)
    mqtt_dot.config(fg=colour)

    _pulse_after_id = root.after(40, _pulse_tick)


def _lerp_hex(c1: str, c2: str, t: float) -> str:
    """Linear-interpolate between two #rrggbb colours."""
    r1, g1, b1 = int(c1[1:3], 16), int(c1[3:5], 16), int(c1[5:7], 16)
    r2, g2, b2 = int(c2[1:3], 16), int(c2[3:5], 16), int(c2[5:7], 16)
    r = int(r1 + (r2 - r1) * t)
    g = int(g1 + (g2 - g1) * t)
    b = int(b1 + (b2 - b1) * t)
    return f"#{r:02x}{g:02x}{b:02x}"


# ================================================================
#  GUI HELPERS
# ================================================================

def _update_status(text: str, colour: str = "#2c3e50") -> None:
    status_label.config(text=text, fg=colour)


def _update_progress(pct: int, done: int, total: int) -> None:
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
    global target_mac

    if mqtt_client is None:
        messagebox.showerror("Not Connected", "MQTT broker not connected yet. Please wait.")
        return

    mac = mac_entry.get().strip().upper()
    if not mac:
        messagebox.showerror("Missing MAC", "Please enter the ESP32 MAC ID.")
        return

    target_mac = mac
    upload_btn.config(state=tk.DISABLED)

    status_topic = f"{target_mac}/ota_status"
    ack_topic    = f"{target_mac}/ota/ack"
    check_topic  = f"{target_mac}/ota_check"

    mqtt_client.subscribe(status_topic, qos=1)
    mqtt_client.subscribe(ack_topic,    qos=1)
    mqtt_client.publish(check_topic, "ARE_YOU_READY", qos=1)

    print(f"[APP] Checking device: {target_mac}")
    _update_status("Waiting for ESP32\u2026", "#7f8c8d")

    progress_bar["value"] = 0
    progress_label.config(text="")


def cb_select_firmware() -> None:
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
#  OTA UPLOAD WORKER
# ================================================================

def _upload_worker() -> None:
    global current_chunk

    try:
        with open(firmware_path, "rb") as f:
            firmware_data = f.read()

        total_size   = len(firmware_data)
        total_chunks = (total_size + CHUNK_SIZE - 1) // CHUNK_SIZE
        crc32_val    = zlib.crc32(firmware_data) & 0xFFFFFFFF

        print(f"[OTA] Size={total_size}B  Chunks={total_chunks}  CRC=0x{crc32_val:08X}")

        begin_payload = json.dumps({
            "size"  : total_size,
            "chunks": total_chunks,
            "crc32" : crc32_val,
        })
        mqtt_client.publish(f"{target_mac}/ota/begin", begin_payload, qos=1)
        root.after(0, lambda: _update_status("OTA Started \u2014 sending chunks\u2026", "#2980b9"))

        time.sleep(0.3)

        for i in range(total_chunks):
            chunk_data = firmware_data[i * CHUNK_SIZE : (i + 1) * CHUNK_SIZE]
            packet = struct.pack(">I", i) + chunk_data

            retries = 0
            success = False

            while retries < MAX_RETRIES:
                with _ack_lock:
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

            pct = int(((i + 1) / total_chunks) * 100)
            root.after(0, lambda p=pct, ci=i + 1, ct=total_chunks:
                _update_progress(p, ci, ct))

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
    win.geometry("520x510")
    win.resizable(False, False)
    win.configure(bg="#1c2333")

    font_title  = ("Consolas", 15, "bold")
    font_label  = ("Consolas", 10)
    font_entry  = ("Consolas", 11)
    font_status = ("Consolas", 11, "bold")
    font_btn    = ("Consolas", 10, "bold")
    font_dot    = ("Consolas", 16, "bold")
    font_mqtt   = ("Consolas", 9)

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

    # ── MQTT Status bar (just below title) ──────────────────────
    mqtt_bar = tk.Frame(win, bg="#111827", height=28)
    mqtt_bar.pack(fill=tk.X)
    mqtt_bar.pack_propagate(False)

    global mqtt_dot, mqtt_status_lbl
    mqtt_dot = tk.Label(
        mqtt_bar,
        text="\u25cf",          # filled circle
        font=font_dot,
        bg="#111827", fg="#4b5563",
        padx=8, pady=0,
    )
    mqtt_dot.pack(side=tk.LEFT)

    mqtt_status_lbl = tk.Label(
        mqtt_bar,
        text="MQTT: Initialising…",
        font=font_mqtt,
        bg="#111827", fg="#4b5563",
        anchor="w",
    )
    mqtt_status_lbl.pack(side=tk.LEFT, pady=4)

    # ── Body ────────────────────────────────────────────────────
    body = tk.Frame(win, bg=BG, padx=30, pady=20)
    body.pack(fill=tk.BOTH, expand=True)

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

    ttk.Separator(body, orient="horizontal").pack(fill=tk.X, pady=10)

    global status_label
    status_label = tk.Label(
        body, text="Device Not Checked",
        font=font_status, bg=BG, fg="#7f8c8d", anchor="w",
    )
    status_label.pack(fill=tk.X, pady=(0, 8))

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

    ttk.Separator(body, orient="horizontal").pack(fill=tk.X, pady=6)

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

    tk.Label(
        win,
        text=f"Chunk: {CHUNK_SIZE}B   |   ACK timeout: {ACK_TIMEOUT}s   |   Max retries: {MAX_RETRIES}",
        font=("Consolas", 8),
        bg="#111827", fg="#4b5563",
    ).pack(fill=tk.X, side=tk.BOTTOM, pady=4)

    return win


# ================================================================
#  ENTRY POINT
# ================================================================

if __name__ == "__main__":
    # 1. Build GUI first — window appears immediately
    root = _build_gui()

    # 2. Connect MQTT in background — status bar updates live
    root.after(100, _connect_mqtt)

    root.mainloop()

    if mqtt_client:
        mqtt_client.loop_stop()
        mqtt_client.disconnect()