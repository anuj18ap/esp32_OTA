"""
================================================================
 ESP32 OTA Firmware Upgrader  —  Python Desktop GUI
================================================================
 Dependencies:
     pip install paho-mqtt

 Compatible with:
     paho-mqtt  2.x
     Python     3.8+

 Usage:
     python esp32_ota_uploader.py
================================================================
"""

import os
import socket
import threading
from http.server import HTTPServer, SimpleHTTPRequestHandler

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import paho.mqtt.client as mqtt

# ================================================================
#  CONFIGURATION
# ================================================================

MQTT_BROKER   = "broker.hivemq.com"
MQTT_PORT     = 1883
HTTP_PORT     = 8000

# ================================================================
#  APPLICATION STATE  (module-level, accessed across callbacks)
# ================================================================

firmware_path      = None   # Absolute path to selected .bin file
target_mac         = None   # MAC ID entered by the user
esp_ready          = False  # True after ESP32 replies READY
http_server_thread = None   # Background HTTP server thread
http_server_inst   = None   # HTTPServer instance (for future stop)
http_server_folder = None   # Folder being served (must not change mid-serve)

# ================================================================
#  NETWORK HELPERS
# ================================================================

def get_local_ip() -> str:
    """
    Determine the local machine's LAN IP by opening a UDP socket
    toward a public address.  No data is actually sent.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()


# ================================================================
#  HTTP SERVER — serves the firmware .bin file to the ESP32
# ================================================================

class _SilentHTTPHandler(SimpleHTTPRequestHandler):
    """Suppress HTTP access logs in the console."""
    def log_message(self, format, *args):   # noqa: A002
        pass


def _run_http_server(folder: str) -> None:
    """
    Change working directory to `folder` and start a blocking
    HTTPServer on HTTP_PORT.  Intended to run in a daemon thread.
    """
    global http_server_inst

    os.chdir(folder)
    http_server_inst = HTTPServer(("0.0.0.0", HTTP_PORT), _SilentHTTPHandler)
    print(f"[HTTP] Server started on port {HTTP_PORT} serving: {folder}")
    http_server_inst.serve_forever()


def ensure_http_server(folder: str) -> None:
    """
    Start the HTTP server in a daemon thread if it is not already
    running, or if the firmware folder has changed.
    """
    global http_server_thread, http_server_folder

    if http_server_thread is not None and http_server_thread.is_alive():
        # Server already running — nothing to do
        # (folder changes after first start are intentionally ignored;
        #  users should restart the tool if they move firmware files)
        return

    http_server_folder = folder
    http_server_thread = threading.Thread(
        target=_run_http_server,
        args=(folder,),
        daemon=True,   # exits when the main window closes
        name="HTTP-OTA-Server",
    )
    http_server_thread.start()


# ================================================================
#  MQTT SETUP
# ================================================================

def _on_connect(client, userdata, flags, reason_code, properties):
    """Called when the MQTT client establishes a connection."""
    if reason_code == 0:
        print("[MQTT] Connected to broker.")
    else:
        print(f"[MQTT] Connection failed — reason code: {reason_code}")


def _on_disconnect(client, userdata, flags, reason_code, properties):
    """Called when the MQTT connection drops."""
    print(f"[MQTT] Disconnected (reason: {reason_code}). Auto-reconnect active.")


def _on_message(client, userdata, msg):
    """
    Dispatch incoming MQTT messages to the GUI status label.
    All Tkinter updates are marshalled via `root.after()` to stay
    on the main thread.
    """
    payload = msg.payload.decode(errors="replace")
    print(f"[MQTT] RX  topic={msg.topic}  payload={payload}")

    status_map = {
        "READY"     : ("ESP32 READY FOR OTA",   "#27ae60"),
        "UPDATING"  : ("ESP32 UPDATING…",        "#e67e22"),
        "FAILED"    : ("OTA FAILED",             "#e74c3c"),
        "NO_UPDATE" : ("NO UPDATE AVAILABLE",    "#95a5a6"),
    }

    if payload in status_map:
        text, colour = status_map[payload]
        # Schedule GUI update on the main thread
        root.after(0, lambda t=text, c=colour: _update_status(t, c))

        if payload == "READY":
            root.after(0, lambda: upload_btn.config(state=tk.NORMAL))


def _build_mqtt_client() -> mqtt.Client:
    """Initialise and connect the paho-mqtt 2.x client."""
    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2
    )
    client.on_connect    = _on_connect
    client.on_disconnect = _on_disconnect
    client.on_message    = _on_message

    # Enable automatic reconnect (1 s → 60 s back-off)
    client.reconnect_delay_set(min_delay=1, max_delay=60)

    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    client.loop_start()   # background network thread

    return client


mqtt_client = _build_mqtt_client()


# ================================================================
#  GUI HELPERS
# ================================================================

def _update_status(text: str, colour: str = "#2c3e50") -> None:
    """Update the status label text and colour (call from main thread)."""
    status_label.config(text=text, fg=colour)


def _set_controls_enabled(enabled: bool) -> None:
    """Enable or disable interactive controls during an operation."""
    state = tk.NORMAL if enabled else tk.DISABLED
    check_btn.config(state=state)
    firmware_btn.config(state=state)
    mac_entry.config(state=state)


# ================================================================
#  BUTTON CALLBACKS
# ================================================================

def cb_check_device() -> None:
    """
    Read the MAC ID from the entry, subscribe to the status topic,
    and publish an ARE_YOU_READY probe to the device.
    """
    global target_mac, esp_ready

    mac = mac_entry.get().strip().upper()

    if not mac:
        messagebox.showerror("Missing MAC", "Please enter the ESP32 MAC ID.")
        return

    target_mac = mac
    esp_ready  = False

    # Disable Upload until we get a fresh READY reply
    upload_btn.config(state=tk.DISABLED)

    status_topic = f"{target_mac}/ota_status"
    check_topic  = f"{target_mac}/ota_check"

    # Subscribe first, then probe — avoids missing a fast reply
    mqtt_client.subscribe(status_topic, qos=1)
    mqtt_client.publish(check_topic, "ARE_YOU_READY", qos=1)

    print(f"[APP] Checking device: {target_mac}")
    _update_status("Waiting for ESP32…", "#7f8c8d")


def cb_select_firmware() -> None:
    """Open a file dialog restricted to .bin files."""
    global firmware_path

    path = filedialog.askopenfilename(
        title="Select ESP32 Firmware (.bin)",
        filetypes=[("Firmware binary", "*.bin"), ("All files", "*.*")],
    )

    if path:
        firmware_path = path
        firmware_label.config(
            text=os.path.basename(path),
            fg="#27ae60",
        )
        print(f"[APP] Firmware selected: {path}")


def cb_upload_firmware() -> None:
    """
    Start the HTTP server (if needed), build the firmware URL,
    and publish it to the ESP32's OTA topic.
    """
    if not target_mac:
        messagebox.showerror("No Device", "Check a device first.")
        return

    if not firmware_path:
        messagebox.showerror("No Firmware", "Select a firmware file first.")
        return

    folder   = os.path.dirname(firmware_path)
    filename = os.path.basename(firmware_path)

    # Ensure the HTTP server is serving the firmware directory
    ensure_http_server(folder)

    local_ip     = get_local_ip()
    firmware_url = f"http://{local_ip}:{HTTP_PORT}/{filename}"

    ota_topic = f"{target_mac}/ota"

    mqtt_client.publish(ota_topic, firmware_url, qos=1)

    print(f"[APP] OTA URL published: {firmware_url}")
    _update_status("OTA COMMAND SENT", "#2980b9")


# ================================================================
#  GUI LAYOUT
# ================================================================

def _build_gui() -> tk.Tk:
    """Construct and return the main application window."""

    # ---- Window ----
    win = tk.Tk()
    win.title("ESP32 Firmware Upgrader")
    win.geometry("480x400")
    win.resizable(False, False)
    win.configure(bg="#1c2333")

    # ---- Fonts ----
    font_title  = ("Consolas", 15, "bold")
    font_label  = ("Consolas", 10)
    font_entry  = ("Consolas", 11)
    font_status = ("Consolas", 11, "bold")
    font_btn    = ("Consolas", 10, "bold")

    # ---- Colour palette ----
    BG       = "#1c2333"
    FG       = "#ecf0f1"
    ACCENT   = "#3498db"
    BTN_BG   = "#2c3e50"
    BTN_FG   = "#ecf0f1"
    ENTRY_BG = "#2c3e50"

    # ---- Title bar ----
    title_frame = tk.Frame(win, bg=ACCENT, height=46)
    title_frame.pack(fill=tk.X)
    tk.Label(
        title_frame,
        text="  ⬡  ESP32 Firmware Upgrader",
        font=font_title,
        bg=ACCENT,
        fg="white",
        anchor="w",
    ).pack(side=tk.LEFT, padx=12, pady=10)

    # ---- Body frame ----
    body = tk.Frame(win, bg=BG, padx=30, pady=20)
    body.pack(fill=tk.BOTH, expand=True)

    # ---- MAC ID row ----
    tk.Label(body, text="ESP32 MAC ID", font=font_label, bg=BG, fg=FG, anchor="w")\
        .pack(fill=tk.X, pady=(0, 4))

    mac_row = tk.Frame(body, bg=BG)
    mac_row.pack(fill=tk.X, pady=(0, 10))

    global mac_entry, check_btn
    mac_entry = tk.Entry(
        mac_row,
        width=26,
        font=font_entry,
        bg=ENTRY_BG,
        fg=FG,
        insertbackground=FG,
        relief=tk.FLAT,
        bd=6,
    )
    mac_entry.pack(side=tk.LEFT, padx=(0, 10))
    mac_entry.insert(0, "0CF73625BF58")
    mac_entry.bind("<FocusIn>", lambda e: (
        mac_entry.delete(0, tk.END)
        if mac_entry.get().startswith("e.g.")
        else None
    ))

    check_btn = tk.Button(
        mac_row,
        text="Check Device",
        font=font_btn,
        bg=ACCENT,
        fg="white",
        relief=tk.FLAT,
        padx=12,
        pady=4,
        cursor="hand2",
        command=cb_check_device,
    )
    check_btn.pack(side=tk.LEFT)

    # ---- Separator ----
    ttk.Separator(body, orient="horizontal").pack(fill=tk.X, pady=10)

    # ---- Status label ----
    global status_label
    status_label = tk.Label(
        body,
        text="Device Not Checked",
        font=font_status,
        bg=BG,
        fg="#7f8c8d",
        anchor="w",
    )
    status_label.pack(fill=tk.X, pady=(0, 10))

    # ---- Firmware row ----
    ttk.Separator(body, orient="horizontal").pack(fill=tk.X, pady=6)

    fw_row = tk.Frame(body, bg=BG)
    fw_row.pack(fill=tk.X, pady=(8, 4))

    global firmware_btn, firmware_label
    firmware_btn = tk.Button(
        fw_row,
        text="Select Firmware",
        font=font_btn,
        bg=BTN_BG,
        fg=BTN_FG,
        relief=tk.FLAT,
        padx=12,
        pady=4,
        cursor="hand2",
        command=cb_select_firmware,
    )
    firmware_btn.pack(side=tk.LEFT, padx=(0, 14))

    firmware_label = tk.Label(
        fw_row,
        text="No firmware selected",
        font=font_label,
        bg=BG,
        fg="#7f8c8d",
        anchor="w",
    )
    firmware_label.pack(side=tk.LEFT)

    # ---- Upload button ----
    global upload_btn
    upload_btn = tk.Button(
        body,
        text="⬆  Upload Firmware",
        font=font_btn,
        bg="#27ae60",
        fg="white",
        relief=tk.FLAT,
        padx=16,
        pady=8,
        cursor="hand2",
        state=tk.DISABLED,
        command=cb_upload_firmware,
    )
    upload_btn.pack(pady=22)

    # ---- Footer ----
    tk.Label(
        win,
        text=f"MQTT: {MQTT_BROKER}:{MQTT_PORT}   |   HTTP: :{HTTP_PORT}",
        font=("Consolas", 8),
        bg="#111827",
        fg="#4b5563",
    ).pack(fill=tk.X, side=tk.BOTTOM, pady=4)

    return win


# ================================================================
#  ENTRY POINT
# ================================================================

if __name__ == "__main__":
    root = _build_gui()
    root.mainloop()

    # Clean shutdown — stop the MQTT loop when window closes
    mqtt_client.loop_stop()
    mqtt_client.disconnect()