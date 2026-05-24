"""
================================================================
 ESP32 Multi-Device OTA Firmware Upgrader — MQTT
================================================================
 Dependencies:  pip install paho-mqtt
 Python 3.8+  |  paho-mqtt 2.x

 ── MQTT Topics (ESP32 → App) ──────────────────────────────────
   <MAC>/info            JSON  {"id":"<MAC>","firmware":"<ver>","name":"<name>"}
   <MAC>/ota_status      str   READY | UPDATING | FAILED | NO_UPDATE | SUCCESS
   <MAC>/ota/ack         str   "<chunk_index>"
   <MAC>/log             str   any log line
   <MAC>/wifi/config     JSON  {"ssid":"<s>","password":"<p>"}   (reply to request)

 ── MQTT Topics (App → ESP32) ──────────────────────────────────
   <MAC>/ota_check       "ARE_YOU_READY"
   <MAC>/ota/begin       JSON  {"size":N,"chunks":N,"crc32":N}
   <MAC>/ota/chunk       binary [4-byte big-endian index][data]
   <MAC>/ota/end         "END"
   <MAC>/set_name        str   "My Device Label"   (ESP32 saves to NVS)
   <MAC>/wifi/request    "GET_CONFIG"               (ESP32 replies on /wifi/config)
   <MAC>/wifi/set        JSON  {"ssid":"<s>","password":"<p>"}
   <MAC>/reset_config    "RESET_CONFIG"             (ESP32 clears NVS and reboots to setup portal)

 ── Admin passkey (WiFi password reveal) ───────────────────────
   Passkey: 7096099673
================================================================
"""

import os, json, struct, threading, time, zlib, datetime
from dataclasses import dataclass, field
from typing import Optional, Any
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
import paho.mqtt.client as mqtt

# ─────────────────────────────────────────────
#  CONFIG
# ─────────────────────────────────────────────
APP_VERSION      = 1.2
MQTT_BROKER      = "broker.emqx.io"
MQTT_PORT        = 1883
CHUNK_SIZE       = 7168
ACK_TIMEOUT      = 5
MAX_RETRIES      = 3
CHUNK_QOS        = 0
COMMAND_QOS      = 1
BEGIN_DELAY      = 0.15
CHECK_TIMEOUT_MS = 8000
MAX_LOG_LINES    = 2000
ADMIN_PASSKEY    = "7096099673"
NAMES_FILE       = "device_names.json"   # local cache

# ─────────────────────────────────────────────
#  DESIGN TOKENS
# ─────────────────────────────────────────────
BG          = "#080c14"
SIDEBAR_BG  = "#0a0f1c"
PANEL_BG    = "#0d1424"
CARD_BG     = "#111d30"
CARD_BORDER = "#1a2d4a"
INPUT_BG    = "#0a1222"
INPUT_BD    = "#1e3355"
INPUT_FOCUS = "#3b82f6"

ACCENT      = "#3b82f6"
ACCENT_HOV  = "#2563eb"
ACCENT_DIM  = "#1e3a5f"
SUCCESS     = "#22c55e"
SUCCESS_HOV = "#16a34a"
WARNING     = "#f59e0b"
DANGER      = "#ef4444"
DANGER_DIM  = "#4a1010"
MUTED       = "#1e2d3d"

TEXT_PRI    = "#e8f0fe"
TEXT_SEC    = "#7a9cc0"
TEXT_DIM    = "#3d5270"

FONT_MONO   = "Courier New" if os.name == "nt" else "Courier"
FONT_UI     = "Trebuchet MS" if os.name == "nt" else "TkDefaultFont"

STATUS_COLORS = {
    "idle"        : TEXT_DIM,
    "querying"    : WARNING,
    "ready"       : SUCCESS,
    "transferring": ACCENT,
    "verifying"   : ACCENT,
    "success"     : SUCCESS,
    "failed"      : DANGER,
    "no_update"   : TEXT_SEC,
    "timeout"     : WARNING,
}

LOG_COLORS = {
    "ERROR"  : DANGER,
    "WARN"   : WARNING,
    "INFO"   : TEXT_PRI,
    "DEBUG"  : TEXT_DIM,
    "OTA"    : ACCENT,
    "SUCCESS": SUCCESS,
}

# ─────────────────────────────────────────────
#  DEVICE NAME STORE  (MAC → friendly name)
#  Persisted to NAMES_FILE; also sent to ESP32
# ─────────────────────────────────────────────
_names: dict[str, str] = {}
_names_lock = threading.Lock()

def _load_names():
    global _names
    try:
        if os.path.exists(NAMES_FILE):
            with open(NAMES_FILE) as f:
                _names = json.load(f)
    except Exception:
        _names = {}

def _save_names():
    try:
        with _names_lock:
            data = dict(_names)
        with open(NAMES_FILE, "w") as f:
            json.dump(data, f, indent=2)
    except Exception as e:
        print(f"[Names] save error: {e}")

def get_name(mac: str) -> str:
    with _names_lock:
        return _names.get(mac.upper(), "")

def set_name(mac: str, name: str):
    mac = mac.upper()
    with _names_lock:
        _names[mac] = name
    _save_names()

def display_name(mac: str) -> str:
    """Returns friendly name if set, else just MAC."""
    n = get_name(mac)
    return n if n else mac

def display_name_with_mac(mac: str) -> str:
    """Returns 'Name (MAC)' if name set, else just MAC. Used in dropdowns/selectors."""
    n = get_name(mac)
    return f"{n}  ({mac})" if n else mac

# ─────────────────────────────────────────────
#  GLOBAL LOG STORE
# ─────────────────────────────────────────────
_log_entries: list[dict] = []
_log_lock    = threading.Lock()
_log_panel: Optional["LogsPanel"] = None

def _add_log(mac: str, text: str, color: str = TEXT_SEC):
    ts    = datetime.datetime.now().strftime("%H:%M:%S")
    entry = {"ts": ts, "mac": mac, "text": text, "color": color}
    with _log_lock:
        _log_entries.append(entry)
        if len(_log_entries) > MAX_LOG_LINES:
            _log_entries.pop(0)
    if _log_panel:
        root.after(0, lambda e=entry: _log_panel.append_entry(e))

# ─────────────────────────────────────────────
#  PER-DEVICE SESSION
# ─────────────────────────────────────────────
@dataclass
class DeviceSession:
    mac:             str
    firmware_path:   str             = ""
    current_chunk:   int             = 0
    ack_event:       threading.Event = field(default_factory=threading.Event)
    ack_lock:        threading.Lock  = field(default_factory=threading.Lock)
    device_ready:    bool            = False
    status:          str             = "idle"
    status_text:     str             = "Not checked"
    fw_version:      str             = "—"
    check_timer_id:  Any             = None
    ota_running:     bool            = False
    auto_discovered: bool            = False
    # WiFi state (populated when ESP32 replies to wifi/request)
    wifi_ssid:       str             = ""
    wifi_password:   str             = ""
    wifi_received:   bool            = False
    ui:              Any             = None   # DeviceRow

# ─────────────────────────────────────────────
#  SESSION REGISTRY
# ─────────────────────────────────────────────
_sessions: dict[str, DeviceSession] = {}
_sessions_lock = threading.Lock()

def get_session(mac: str) -> Optional[DeviceSession]:
    with _sessions_lock:
        return _sessions.get(mac.upper())

def get_or_create_session(mac: str) -> DeviceSession:
    mac = mac.upper()
    with _sessions_lock:
        if mac not in _sessions:
            _sessions[mac] = DeviceSession(mac=mac)
        return _sessions[mac]

def remove_session(mac: str):
    mac = mac.upper()
    with _sessions_lock:
        _sessions.pop(mac, None)

def known_macs() -> list[str]:
    with _sessions_lock:
        return list(_sessions.keys())

# ─────────────────────────────────────────────
#  MQTT
# ─────────────────────────────────────────────
mqtt_client: Optional[mqtt.Client] = None

MQTT_CONN = "connecting"
MQTT_OK   = "connected"
MQTT_DISC = "disconnected"
MQTT_FAIL = "failed"

_pulse_id  = None
_pulse_a   = 0.0
_pulse_dir = 1

# Global references filled by build_gui()
sb_dot:  Optional[tk.Label] = None
sb_text: Optional[tk.Label] = None
device_count_lbl: Optional[tk.Label] = None

# Panels that need refresh on new devices
_settings_panel: Optional["SettingsPanel"] = None
_info_panel:     Optional["DeviceInfoPanel"] = None


def _on_connect(client, userdata, flags, rc, props):
    if rc == 0:
        client.subscribe("+/info",        qos=0)
        client.subscribe("+/ota_status",  qos=1)
        client.subscribe("+/ota/ack",     qos=1)
        client.subscribe("+/log",         qos=0)
        client.subscribe("+/wifi/config", qos=1)
        root.after(0, lambda: _mqtt_indicator(MQTT_OK))
        _add_log("SYSTEM", f"Connected to {MQTT_BROKER}:{MQTT_PORT}", SUCCESS)
    else:
        root.after(0, lambda: _mqtt_indicator(MQTT_FAIL))
        _add_log("SYSTEM", f"MQTT connect failed  rc={rc}", DANGER)

def _on_disconnect(client, userdata, flags, rc, props):
    root.after(0, lambda: _mqtt_indicator(MQTT_DISC))
    _add_log("SYSTEM", "MQTT disconnected — retrying…", WARNING)

def _on_message(client, userdata, msg):
    topic   = msg.topic
    payload = msg.payload.decode(errors="replace")
    parts   = topic.split("/")
    if not parts:
        return
    mac = parts[0].upper()

    # ── /info — auto-discovery + name sync ──────
    if topic.endswith("/info"):
        try:
            d = json.loads(payload)
            if not isinstance(d, dict):
                return
            discovered_mac = str(d.get("id", mac)).upper().replace(":", "")
            fw_version     = str(d.get("firmware", "?"))
            device_name    = str(d.get("name", "")).strip()
        except Exception:
            return

        # If device sent a name, update local cache
        if device_name:
            set_name(discovered_mac, device_name)

        existing = get_session(discovered_mac)
        if existing:
            existing.fw_version = f"v{fw_version}"
            root.after(0, lambda s=existing: s.ui and s.ui.refresh_fw_version())
            root.after(0, lambda s=existing: s.ui and s.ui.refresh_name())
        else:
            session = get_or_create_session(discovered_mac)
            session.fw_version     = f"v{fw_version}"
            session.status         = "ready"
            session.status_text    = "AUTO-DISCOVERED"
            session.device_ready   = True
            session.auto_discovered = True
            _add_log(discovered_mac,
                     f"Auto-discovered  fw=v{fw_version}"
                     + (f"  name={device_name}" if device_name else ""),
                     ACCENT)
            root.after(0, lambda s=session, fv=fw_version:
                       _auto_add_device_row(s, fv))
            root.after(0, _refresh_all_panels)
            # ── Auto-request WiFi config on discovery ──
            root.after(500, lambda m=discovered_mac: _auto_request_wifi(m))
        return

    # ── /log ────────────────────────────────────
    if topic.endswith("/log"):
        col = TEXT_SEC
        up  = payload.upper()
        for kw, c in LOG_COLORS.items():
            if kw in up:
                col = c
                break
        _add_log(mac, payload, col)
        return

    # ── /wifi/config — ESP32 reply with credentials
    if topic.endswith("/wifi/config"):
        session = get_session(mac)
        if not session:
            return
        try:
            d = json.loads(payload)
            session.wifi_ssid     = d.get("ssid", "")
            session.wifi_password = d.get("password", "")
            session.wifi_received = True
            _add_log(mac, f"WiFi config received  ssid={session.wifi_ssid}", SUCCESS)
        except Exception:
            _add_log(mac, "WiFi config parse error", DANGER)
        root.after(0, lambda: _info_panel and _info_panel.on_wifi_received(mac))
        root.after(0, lambda: _settings_panel and _settings_panel.on_wifi_received(mac))
        return

    session = get_session(mac)
    if not session:
        return

    # ── /ota/ack ────────────────────────────────
    if topic.endswith("/ota/ack"):
        try:
            idx = int(payload)
            with session.ack_lock:
                if idx == session.current_chunk:
                    session.ack_event.set()
        except ValueError:
            pass
        return

    # ── /ota_status ─────────────────────────────
    MAP = {
        "READY"    : ("ready",        "READY FOR OTA"),
        "UPDATING" : ("transferring", "FLASHING…"),
        "FAILED"   : ("failed",       "OTA FAILED"),
        "NO_UPDATE": ("no_update",    "NO UPDATE AVAILABLE"),
        "SUCCESS"  : ("success",      "SUCCESS  ✓  REBOOTING"),
    }
    if payload in MAP:
        status_key, status_txt = MAP[payload]
        col_map = {"ready": SUCCESS, "transferring": ACCENT,
                   "failed": DANGER, "no_update": TEXT_SEC, "success": SUCCESS}
        _add_log(mac, f"OTA status → {payload}", col_map.get(status_key, TEXT_SEC))
        session.status      = status_key
        session.status_text = status_txt

        if payload == "READY":
            _cancel_check_timeout(session)
            session.device_ready = True
            root.after(0, lambda s=session: s.ui and s.ui.on_device_ready())

        if payload in ("SUCCESS", "FAILED", "NO_UPDATE"):
            session.device_ready = False
            session.ota_running  = False
            root.after(0, lambda s=session: s.ui and s.ui.on_ota_finished())

        root.after(0, lambda s=session: s.ui and s.ui.refresh_status())


def _auto_request_wifi(mac: str):
    """Automatically request WiFi config after device discovery."""
    if not mqtt_client:
        return
    session = get_session(mac)
    if not session:
        return
    mqtt_client.subscribe(f"{mac}/wifi/config", qos=1)
    mqtt_client.publish(f"{mac}/wifi/request", "GET_CONFIG", qos=COMMAND_QOS)
    _add_log(mac, "WiFi config auto-requested on discovery", TEXT_DIM)


def _refresh_all_panels():
    if _log_panel:
        _log_panel.refresh_filter_menu()
    if _settings_panel:
        _settings_panel.refresh_device_list()
    if _info_panel:
        _info_panel.refresh_device_list()


def _auto_add_device_row(session: "DeviceSession", fw_version: str):
    if not hasattr(_auto_add_device_row, "_manager") or \
            _auto_add_device_row._manager is None:
        return
    mgr = _auto_add_device_row._manager

    if len(mgr.rows) >= DeviceManager.MAX_DEVICES:
        print(f"[Discovery] Ignored {session.mac} — limit reached")
        return
    for row in mgr.rows:
        if row.session and row.session.mac == session.mac:
            return

    if hasattr(mgr, "_empty_lbl") and mgr._empty_lbl:
        try:
            mgr._empty_lbl.pack_forget()
        except Exception:
            pass

    row = DeviceRow(mgr.scroll_frame, mgr, len(mgr.rows))
    row.session = session
    session.ui  = row

    row.mac_entry.delete(0, tk.END)
    row.mac_entry.insert(0, session.mac)
    row.mac_entry.config(state=tk.DISABLED)
    row.fw_var.set(session.fw_version)
    row.refresh_status()
    row.refresh_name()

    mgr.rows.append(row)
    _update_device_counter(len(mgr.rows))
    print(f"[Discovery] Auto-added {session.mac}  fw={fw_version}")


def _connect_mqtt():
    global mqtt_client
    def _worker():
        global mqtt_client
        root.after(0, lambda: _mqtt_indicator(MQTT_CONN))
        try:
            c = mqtt.Client(
                callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
            c.on_connect    = _on_connect
            c.on_disconnect = _on_disconnect
            c.on_message    = _on_message
            c.reconnect_delay_set(1, 60)
            c.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
            c.loop_start()
            mqtt_client = c
        except Exception as e:
            print(f"[MQTT] {e}")
            _add_log("SYSTEM", f"Cannot reach broker: {e}", DANGER)
            root.after(0, lambda: _mqtt_indicator(MQTT_FAIL))
    threading.Thread(target=_worker, daemon=True).start()

# ─────────────────────────────────────────────
#  MQTT PULSE ANIMATION
# ─────────────────────────────────────────────
def _mqtt_indicator(state):
    global _pulse_id
    if _pulse_id:
        root.after_cancel(_pulse_id)
        _pulse_id = None
    cfg = {
        MQTT_CONN: (WARNING, "Connecting…",                         True),
        MQTT_OK  : (SUCCESS, f"MQTT  ·  {MQTT_BROKER}:{MQTT_PORT}", False),
        MQTT_DISC: (DANGER,  "Disconnected — retrying",             True),
        MQTT_FAIL: (DANGER,  f"Cannot reach {MQTT_BROKER}",         False),
    }
    col, txt, pulse = cfg[state]
    sb_dot.config(fg=col)
    sb_text.config(text=txt, fg=col)
    if pulse:
        _start_pulse(col)

def _start_pulse(col):
    global _pulse_id, _pulse_a, _pulse_dir
    _pulse_a, _pulse_dir = 0.0, 1
    _pulse_id = root.after(35, lambda: _tick_pulse(col))

def _tick_pulse(col):
    global _pulse_id, _pulse_a, _pulse_dir
    if not _pulse_id:
        return
    _pulse_a += 0.07 * _pulse_dir
    if _pulse_a >= 1:   _pulse_a = 1;  _pulse_dir = -1
    elif _pulse_a <= 0: _pulse_a = 0;  _pulse_dir =  1
    dim = _darken(col, 0.3)
    sb_dot.config(fg=_lerp_hex(dim, col, _pulse_a))
    _pulse_id = root.after(35, lambda: _tick_pulse(col))

def _darken(h, a):
    r,g,b = int(h[1:3],16), int(h[3:5],16), int(h[5:7],16)
    return f"#{int(r*a):02x}{int(g*a):02x}{int(b*a):02x}"

def _lerp_hex(c1, c2, t):
    r1,g1,b1 = int(c1[1:3],16),int(c1[3:5],16),int(c1[5:7],16)
    r2,g2,b2 = int(c2[1:3],16),int(c2[3:5],16),int(c2[5:7],16)
    return (f"#{int(r1+(r2-r1)*t):02x}"
            f"{int(g1+(g2-g1)*t):02x}"
            f"{int(b1+(b2-b1)*t):02x}")

# ─────────────────────────────────────────────
#  CHECK TIMEOUT
# ─────────────────────────────────────────────
def _cancel_check_timeout(session: DeviceSession):
    if session.check_timer_id:
        root.after_cancel(session.check_timer_id)
        session.check_timer_id = None

def _on_check_timeout(session: DeviceSession):
    session.check_timer_id = None
    if session.status == "querying":
        session.status       = "timeout"
        session.status_text  = "No response — check MAC / power"
        session.device_ready = False
        _add_log(session.mac, "Check timed out — no response", WARNING)
        root.after(0, lambda s=session: s.ui and s.ui.on_check_timeout())

# ─────────────────────────────────────────────
#  HELPERS
# ─────────────────────────────────────────────
def _make_input(parent, width=22, show=None):
    e = tk.Entry(parent, font=(FONT_MONO, 10),
                 bg=INPUT_BG, fg=TEXT_PRI,
                 insertbackground=TEXT_PRI,
                 relief=tk.FLAT, bd=0,
                 highlightthickness=1,
                 highlightbackground=INPUT_BD,
                 highlightcolor=INPUT_FOCUS,
                 width=width)
    if show:
        e.config(show=show)
    return e

def _make_btn(parent, text, cmd, bg=ACCENT, fg="white",
              padx=14, pady=6):
    return tk.Button(parent, text=text,
                     font=(FONT_UI, 9, "bold"),
                     bg=bg, fg=fg,
                     activebackground=ACCENT_HOV,
                     activeforeground="white",
                     relief=tk.FLAT, bd=0,
                     highlightthickness=0,
                     padx=padx, pady=pady,
                     cursor="hand2",
                     command=cmd)

def _section_label(parent, text):
    tk.Label(parent, text=text,
             font=(FONT_MONO, 7, "bold"),
             bg=CARD_BG, fg=TEXT_DIM,
             anchor="w").pack(fill=tk.X, pady=(10, 2))

# ─────────────────────────────────────────────
#  ADMIN PASSKEY DIALOG
# ─────────────────────────────────────────────
def _ask_admin_passkey(on_success, message="Enter the admin passkey to reveal WiFi passwords."):
    """Show a modal dialog asking for the admin passkey.
    Calls on_success() if correct."""
    dlg = tk.Toplevel(root)
    dlg.title("Admin Access Required")
    dlg.configure(bg=CARD_BG)
    dlg.resizable(False, False)
    dlg.grab_set()
    dlg.transient(root)

    # Centre over root
    root.update_idletasks()
    x = root.winfo_x() + root.winfo_width()  // 2 - 200
    y = root.winfo_y() + root.winfo_height() // 2 - 100
    dlg.geometry(f"400x210+{x}+{y}")

    tk.Frame(dlg, bg=WARNING, height=3).pack(fill=tk.X)

    body = tk.Frame(dlg, bg=CARD_BG, padx=28, pady=20)
    body.pack(fill=tk.BOTH, expand=True)

    tk.Label(body, text="🔒  Admin Passkey Required",
             font=(FONT_UI, 11, "bold"),
             bg=CARD_BG, fg=TEXT_PRI).pack(anchor="w")
    tk.Label(body,
             text=message,
             font=(FONT_UI, 9),
             bg=CARD_BG, fg=TEXT_SEC).pack(anchor="w", pady=(4, 14))

    entry = _make_input(body, width=28, show="●")
    entry.pack(anchor="w", ipady=6, ipadx=6)
    entry.focus_set()

    err_lbl = tk.Label(body, text="",
                       font=(FONT_UI, 9, "bold"),
                       bg=CARD_BG, fg=DANGER)
    err_lbl.pack(anchor="w", pady=(6, 0))

    btn_row = tk.Frame(body, bg=CARD_BG)
    btn_row.pack(fill=tk.X, pady=(10, 0))

    def _submit():
        if entry.get() == ADMIN_PASSKEY:
            dlg.destroy()
            on_success()
        else:
            err_lbl.config(text="Incorrect passkey. Try again.")
            entry.delete(0, tk.END)
            entry.focus_set()

    def _cancel():
        dlg.destroy()

    _make_btn(btn_row, "Unlock", _submit,
              bg=WARNING, fg="#1a0a00").pack(side=tk.LEFT)
    _make_btn(btn_row, "Cancel", _cancel,
              bg=MUTED, fg=TEXT_SEC).pack(side=tk.LEFT, padx=(8, 0))

    entry.bind("<Return>", lambda e: _submit())

# ─────────────────────────────────────────────
#  WIFI PASSWORD CONFIRM DIALOG
#  Used before pushing WiFi credentials to a device
# ─────────────────────────────────────────────
def _ask_wifi_password(on_confirm):
    """Show a modal dialog asking for the WiFi password before pushing config."""
    dlg = tk.Toplevel(root)
    dlg.title("Confirm WiFi Password")
    dlg.configure(bg=CARD_BG)
    dlg.resizable(False, False)
    dlg.grab_set()
    dlg.transient(root)

    root.update_idletasks()
    x = root.winfo_x() + root.winfo_width()  // 2 - 200
    y = root.winfo_y() + root.winfo_height() // 2 - 100
    dlg.geometry(f"420x230+{x}+{y}")

    tk.Frame(dlg, bg=ACCENT, height=3).pack(fill=tk.X)

    body = tk.Frame(dlg, bg=CARD_BG, padx=28, pady=20)
    body.pack(fill=tk.BOTH, expand=True)

    tk.Label(body, text="⚙  Confirm WiFi Password",
             font=(FONT_UI, 11, "bold"),
             bg=CARD_BG, fg=TEXT_PRI).pack(anchor="w")
    tk.Label(body,
             text="Enter the WiFi password to push to the device.",
             font=(FONT_UI, 9),
             bg=CARD_BG, fg=TEXT_SEC).pack(anchor="w", pady=(4, 14))

    entry = _make_input(body, width=30, show="●")
    entry.pack(anchor="w", ipady=6, ipadx=6)
    entry.focus_set()

    err_lbl = tk.Label(body, text="",
                       font=(FONT_UI, 9, "bold"),
                       bg=CARD_BG, fg=DANGER)
    err_lbl.pack(anchor="w", pady=(4, 0))

    btn_row = tk.Frame(body, bg=CARD_BG)
    btn_row.pack(fill=tk.X, pady=(10, 0))

    def _submit():
        pw = entry.get()
        dlg.destroy()
        on_confirm(pw)

    def _cancel():
        dlg.destroy()

    _make_btn(btn_row, "Apply", _submit,
              bg=SUCCESS, fg="#001a00").pack(side=tk.LEFT)
    _make_btn(btn_row, "Cancel", _cancel,
              bg=MUTED, fg=TEXT_SEC).pack(side=tk.LEFT, padx=(8, 0))

    entry.bind("<Return>", lambda e: _submit())

# ─────────────────────────────────────────────
#  OTA WORKER
# ─────────────────────────────────────────────
def _ota_worker(session: DeviceSession):
    try:
        with open(session.firmware_path, "rb") as f:
            data = f.read()
        total  = len(data)
        chunks = (total + CHUNK_SIZE - 1) // CHUNK_SIZE
        crc    = zlib.crc32(data) & 0xFFFFFFFF

        _add_log(session.mac,
                 f"OTA begin  size={total}B  chunks={chunks}  crc32={crc:#010x}",
                 ACCENT)
        mqtt_client.publish(
            f"{session.mac}/ota/begin",
            json.dumps({"size": total, "chunks": chunks, "crc32": crc}),
            qos=COMMAND_QOS)

        session.status      = "transferring"
        session.status_text = "Transferring firmware…"
        root.after(0, lambda s=session: s.ui and s.ui.refresh_status())
        time.sleep(BEGIN_DELAY)

        for i in range(chunks):
            pkt = struct.pack(">I", i) + data[i*CHUNK_SIZE:(i+1)*CHUNK_SIZE]
            ok  = False
            for attempt in range(MAX_RETRIES):
                with session.ack_lock:
                    session.current_chunk = i
                    session.ack_event.clear()
                mqtt_client.publish(
                    f"{session.mac}/ota/chunk", pkt, qos=CHUNK_QOS)
                if session.ack_event.wait(ACK_TIMEOUT):
                    ok = True
                    break
                _add_log(session.mac,
                         f"Chunk {i} retry {attempt+1}/{MAX_RETRIES}", WARNING)

            if not ok:
                session.status      = "failed"
                session.status_text = f"FAILED — no ACK on chunk {i}"
                session.ota_running = False
                _add_log(session.mac, f"OTA FAILED at chunk {i}", DANGER)
                root.after(0, lambda s=session: s.ui and (
                    s.ui.refresh_status(), s.ui.on_ota_finished()))
                return

            pct = int((i + 1) / chunks * 100)
            root.after(0, lambda p=pct, ci=i+1, ct=chunks, s=session:
                s.ui and s.ui.update_progress(p, ci, ct))

        mqtt_client.publish(f"{session.mac}/ota/end", "END", qos=COMMAND_QOS)
        session.status      = "verifying"
        session.status_text = "Verifying — waiting for ESP32…"
        _add_log(session.mac, "All chunks sent — verifying CRC…", ACCENT)
        root.after(0, lambda s=session: s.ui and s.ui.refresh_status())

    except FileNotFoundError:
        session.status      = "failed"
        session.status_text = "Firmware file not found"
        session.ota_running = False
        _add_log(session.mac, "Firmware file not found", DANGER)
        root.after(0, lambda s=session: s.ui and (
            s.ui.refresh_status(), s.ui.on_ota_finished()))
    except Exception as e:
        session.status      = "failed"
        session.status_text = f"Error: {e}"
        session.ota_running = False
        _add_log(session.mac, f"OTA error: {e}", DANGER)
        root.after(0, lambda s=session: s.ui and (
            s.ui.refresh_status(), s.ui.on_ota_finished()))

# ─────────────────────────────────────────────
#  DEVICE ROW  (OTA Flash page)
#  ── Name shown as read-only label, no edit ──
# ─────────────────────────────────────────────
class DeviceRow:
    def __init__(self, parent_frame, manager, index: int):
        self.manager = manager
        self.index   = index
        self.session: Optional[DeviceSession] = None

        self.card = tk.Frame(parent_frame, bg=CARD_BG,
                             highlightthickness=1,
                             highlightbackground=CARD_BORDER)
        self.card.pack(fill=tk.X, pady=(0, 10), padx=2)

        tk.Frame(self.card, bg=ACCENT_DIM, height=2).pack(fill=tk.X)

        top = tk.Frame(self.card, bg=CARD_BG, padx=14, pady=10)
        top.pack(fill=tk.X)

        # Badge
        badge = tk.Frame(top, bg=ACCENT, width=28, height=28)
        badge.pack(side=tk.LEFT, padx=(0, 12))
        badge.pack_propagate(False)
        tk.Label(badge, text=str(index + 1),
                 font=(FONT_MONO, 9, "bold"),
                 bg=ACCENT, fg="white").place(relx=0.5, rely=0.5, anchor="center")

        # ── Left column: Name (read-only label) + MAC ──
        left_col = tk.Frame(top, bg=CARD_BG)
        left_col.pack(side=tk.LEFT)

        # Device name — read-only display label
        tk.Label(left_col, text="DEVICE NAME",
                 font=(FONT_MONO, 7, "bold"),
                 bg=CARD_BG, fg=TEXT_DIM, anchor="w").pack(fill=tk.X)

        self._name_lbl = tk.Label(left_col, text="—",
                                   font=(FONT_UI, 10, "bold"),
                                   bg=CARD_BG, fg=SUCCESS, anchor="w")
        self._name_lbl.pack(fill=tk.X, pady=(2, 8))

        # MAC address row
        mac_lbl_row = tk.Frame(left_col, bg=CARD_BG)
        mac_lbl_row.pack(fill=tk.X)
        tk.Label(mac_lbl_row, text="MAC ADDRESS",
                 font=(FONT_MONO, 7, "bold"),
                 bg=CARD_BG, fg=TEXT_DIM, anchor="w").pack(side=tk.LEFT)
        self.auto_badge = tk.Label(mac_lbl_row, text=" AUTO-DISCOVERED ",
                                   font=(FONT_MONO, 7, "bold"),
                                   bg="#0d2a4a", fg=ACCENT, padx=4, pady=0)

        mac_input_row = tk.Frame(left_col, bg=CARD_BG)
        mac_input_row.pack(fill=tk.X)
        self.mac_entry = tk.Entry(mac_input_row,
                                  font=(FONT_MONO, 11),
                                  bg=INPUT_BG, fg=TEXT_PRI,
                                  insertbackground=TEXT_PRI,
                                  relief=tk.FLAT, bd=0,
                                  highlightthickness=1,
                                  highlightbackground=INPUT_BD,
                                  highlightcolor=INPUT_FOCUS,
                                  width=16)
        self.mac_entry.pack(side=tk.LEFT, ipady=5, ipadx=6)
        self.check_btn = tk.Button(mac_input_row, text="Check",
                                   font=(FONT_UI, 9, "bold"),
                                   bg=ACCENT, fg="white",
                                   activebackground=ACCENT_HOV,
                                   activeforeground="white",
                                   relief=tk.FLAT, bd=0,
                                   highlightthickness=0,
                                   padx=12, pady=5, cursor="hand2",
                                   command=self.cb_check)
        self.check_btn.pack(side=tk.LEFT, padx=(8, 0))

        # FW version
        fw_block = tk.Frame(top, bg=CARD_BG)
        fw_block.pack(side=tk.LEFT, padx=(20, 0))
        tk.Label(fw_block, text="FW VERSION",
                 font=(FONT_MONO, 7, "bold"),
                 bg=CARD_BG, fg=TEXT_DIM, anchor="w").pack(fill=tk.X)
        self.fw_var = tk.StringVar(value="—")
        tk.Entry(fw_block,
                 textvariable=self.fw_var,
                 font=(FONT_MONO, 10, "bold"),
                 bg=INPUT_BG, fg=TEXT_DIM,
                 relief=tk.FLAT, bd=0,
                 highlightthickness=1, highlightbackground=INPUT_BD,
                 width=9, state="readonly",
                 readonlybackground=INPUT_BG, cursor="arrow"
                 ).pack(ipady=5, ipadx=6)

        # Remove
        self.remove_btn = tk.Button(top, text="✕",
                  font=(FONT_MONO, 9), bg=CARD_BG, fg=TEXT_DIM,
                  activebackground=DANGER_DIM, activeforeground=DANGER,
                  relief=tk.FLAT, bd=0, highlightthickness=0,
                  padx=6, pady=4, cursor="hand2",
                  command=self.cb_remove)
        self.remove_btn.pack(side=tk.RIGHT, padx=(6, 0))

        # Status
        status_block = tk.Frame(top, bg=CARD_BG)
        status_block.pack(side=tk.RIGHT, padx=(0, 8))
        self.status_dot = tk.Label(status_block, text="●",
                                   font=(FONT_MONO, 10),
                                   bg=CARD_BG, fg=TEXT_DIM)
        self.status_dot.pack(side=tk.LEFT)
        self.status_lbl = tk.Label(status_block, text="Not checked",
                                   font=(FONT_UI, 9, "bold"),
                                   bg=CARD_BG, fg=TEXT_DIM, anchor="e",
                                   wraplength=160, justify=tk.RIGHT)
        self.status_lbl.pack(side=tk.LEFT, padx=(4, 0))

        # Middle: file + upload
        mid = tk.Frame(self.card, bg=CARD_BG, padx=14, pady=4)
        mid.pack(fill=tk.X)

        file_area = tk.Frame(mid, bg=CARD_BG)
        file_area.pack(side=tk.LEFT, fill=tk.X, expand=True)
        tk.Label(file_area, text="FIRMWARE FILE",
                 font=(FONT_MONO, 7, "bold"),
                 bg=CARD_BG, fg=TEXT_DIM, anchor="w").pack(fill=tk.X)

        file_row = tk.Frame(file_area, bg=CARD_BG)
        file_row.pack(fill=tk.X)
        self.file_lbl = tk.Label(file_row, text="No file selected",
                                 font=(FONT_UI, 9), bg=CARD_BG,
                                 fg=TEXT_DIM, anchor="w", width=30)
        self.file_lbl.pack(side=tk.LEFT)
        tk.Button(file_row, text="Browse…",
                  font=(FONT_UI, 8),
                  bg=MUTED, fg=TEXT_SEC,
                  activebackground=INPUT_BD, activeforeground=TEXT_PRI,
                  relief=tk.FLAT, bd=0,
                  highlightthickness=1, highlightbackground=CARD_BORDER,
                  padx=10, pady=4, cursor="hand2",
                  command=self.cb_browse
                  ).pack(side=tk.LEFT, padx=(8, 0))

        self.upload_btn = tk.Button(mid, text="⬆  Upload",
                                    font=(FONT_UI, 10, "bold"),
                                    bg=MUTED, fg=TEXT_DIM,
                                    activebackground=SUCCESS_HOV,
                                    activeforeground="white",
                                    relief=tk.FLAT, bd=0,
                                    highlightthickness=0,
                                    padx=18, pady=8, cursor="",
                                    state=tk.DISABLED,
                                    command=self.cb_upload)
        self.upload_btn.pack(side=tk.RIGHT, padx=(12, 0))

        # Progress
        prog_frame = tk.Frame(self.card, bg=CARD_BG)
        prog_frame.pack(fill=tk.X, padx=14, pady=(2, 10))
        prog_top = tk.Frame(prog_frame, bg=CARD_BG)
        prog_top.pack(fill=tk.X, pady=(0, 3))
        self.prog_left = tk.Label(prog_top, text="",
                                  font=(FONT_MONO, 8),
                                  bg=CARD_BG, fg=TEXT_SEC, anchor="w")
        self.prog_left.pack(side=tk.LEFT)
        self.prog_right = tk.Label(prog_top, text="",
                                   font=(FONT_MONO, 8, "bold"),
                                   bg=CARD_BG, fg=TEXT_PRI, anchor="e")
        self.prog_right.pack(side=tk.RIGHT)

        sn = f"OTA{index}.Horizontal.TProgressbar"
        s  = ttk.Style()
        s.theme_use("default")
        s.configure(sn, troughcolor=INPUT_BG, background=ACCENT,
                    thickness=4, borderwidth=0)
        self.pbar = ttk.Progressbar(prog_frame, orient="horizontal",
                                    mode="determinate", style=sn)
        self.pbar.pack(fill=tk.X)

        # Force-upload warning
        self.force_frame = tk.Frame(self.card, bg=CARD_BG)
        tk.Label(self.force_frame,
                 text="⚠  No response. Use Force Upload to skip handshake.",
                 font=(FONT_UI, 8), bg=CARD_BG, fg=WARNING, anchor="w"
                 ).pack(side=tk.LEFT)
        tk.Button(self.force_frame, text="Force Upload",
                  font=(FONT_UI, 8, "bold"),
                  bg="#7c3a00", fg="#fbbf24",
                  activebackground="#92400e", activeforeground="white",
                  relief=tk.FLAT, bd=0, padx=10, pady=4, cursor="hand2",
                  command=self.cb_force_upload
                  ).pack(side=tk.LEFT, padx=(10, 0))

    # ── refresh helpers ──────────────────────────
    def refresh_status(self):
        if not self.session:
            return
        col = STATUS_COLORS.get(self.session.status, TEXT_DIM)
        self.status_dot.config(fg=col)
        self.status_lbl.config(text=self.session.status_text, fg=col)
        if getattr(self.session, "auto_discovered", False):
            self.auto_badge.pack(side=tk.LEFT, padx=(6, 0))
        else:
            self.auto_badge.pack_forget()

    def refresh_fw_version(self):
        if self.session:
            self.fw_var.set(self.session.fw_version)

    def refresh_name(self):
        """Update the read-only name label from local name store."""
        if not self.session:
            return
        n = get_name(self.session.mac)
        self._name_lbl.config(text=n if n else "—")

    def update_progress(self, pct, done, total):
        self.pbar["value"] = pct
        self.prog_left.config(text=f"Chunk {done}/{total}")
        self.prog_right.config(text=f"{pct}%")

    def reset_progress(self):
        self.pbar["value"] = 0
        self.prog_left.config(text="")
        self.prog_right.config(text="")

    def on_device_ready(self):
        self.refresh_status()
        self._refresh_upload_btn()

    def on_ota_finished(self):
        self.refresh_status()
        self._unlock_controls()
        self._refresh_upload_btn()

    def on_check_timeout(self):
        self.refresh_status()
        self.force_frame.pack(fill=tk.X, padx=14, pady=(0, 8))

    def _hide_force(self):
        self.force_frame.pack_forget()

    def _refresh_upload_btn(self):
        if self.session and self.session.device_ready and self.session.firmware_path:
            self.upload_btn.config(state=tk.NORMAL, bg=SUCCESS,
                                   fg="white", cursor="hand2")
        else:
            self.upload_btn.config(state=tk.DISABLED, bg=MUTED,
                                   fg=TEXT_DIM, cursor="")

    def _lock_controls(self):
        self.mac_entry.config(state=tk.DISABLED)
        self.check_btn.config(state=tk.DISABLED)
        self.upload_btn.config(state=tk.DISABLED, bg=MUTED,
                               fg=TEXT_DIM, cursor="")

    def _unlock_controls(self):
        is_auto = self.session and getattr(self.session, "auto_discovered", False)
        self.mac_entry.config(state=tk.DISABLED if is_auto else tk.NORMAL)
        self.check_btn.config(state=tk.NORMAL)

    # ── callbacks ───────────────────────────────
    def cb_check(self):
        if not mqtt_client:
            messagebox.showerror("Not Connected", "MQTT not ready.")
            return
        mac = self.mac_entry.get().strip().upper()
        if not mac:
            messagebox.showerror("Missing MAC", "Enter the ESP32 MAC address.")
            return
        old = self.session
        if old and old.mac != mac:
            _cancel_check_timeout(old)
            remove_session(old.mac)

        session = get_or_create_session(mac)
        session.ui           = self
        session.device_ready = False
        session.status       = "querying"
        session.status_text  = "Querying device…"
        session.fw_version   = "—"
        self.session = session

        _cancel_check_timeout(session)
        self._hide_force()
        self.fw_var.set("—")
        self.reset_progress()
        self.refresh_status()
        self.refresh_name()
        self._refresh_upload_btn()

        _add_log(mac, "Sending ARE_YOU_READY…", TEXT_SEC)
        mqtt_client.subscribe(f"{mac}/ota_status",  qos=1)
        mqtt_client.subscribe(f"{mac}/ota/ack",     qos=1)
        mqtt_client.subscribe(f"{mac}/info",        qos=0)
        mqtt_client.subscribe(f"{mac}/log",         qos=0)
        mqtt_client.subscribe(f"{mac}/wifi/config", qos=1)
        mqtt_client.publish(f"{mac}/ota_check", "ARE_YOU_READY", qos=COMMAND_QOS)

        session.check_timer_id = root.after(
            CHECK_TIMEOUT_MS, lambda s=session: _on_check_timeout(s))

        root.after(100, _refresh_all_panels)

    def cb_browse(self):
        p = filedialog.askopenfilename(
            title="Select firmware binary",
            filetypes=[("Firmware binary", "*.bin"), ("All files", "*.*")])
        if not p:
            return
        self.file_lbl.config(
            text=f"{os.path.basename(p)}  ({os.path.getsize(p)//1024} KB)",
            fg=TEXT_PRI)
        if self.session:
            self.session.firmware_path = p
        else:
            self._pending_firmware = p
        self._refresh_upload_btn()

    def cb_upload(self):
        if not self.session:
            messagebox.showerror("No Device", "Run Check first.")
            return
        if not self.session.firmware_path:
            fp = getattr(self, "_pending_firmware", "")
            if fp:
                self.session.firmware_path = fp
            else:
                messagebox.showerror("No File", "Select a firmware file.")
                return
        self._start_ota()

    def cb_force_upload(self):
        if not self.session:
            messagebox.showerror("No Device", "Enter a MAC and click Check first.")
            return
        fp = self.session.firmware_path or getattr(self, "_pending_firmware", "")
        if not fp:
            messagebox.showerror("No File", "Select a firmware file first.")
            return
        if not messagebox.askyesno("Force Upload",
                f"Skip READY handshake for {self.session.mac}?\n\n"
                "Only proceed if the device is waiting for OTA."):
            return
        self.session.firmware_path = fp
        self.session.device_ready  = True
        self._hide_force()
        self._start_ota()

    def _start_ota(self):
        self.session.ota_running = True
        self._lock_controls()
        self.reset_progress()
        self.session.status      = "transferring"
        self.session.status_text = "Starting transfer…"
        self.refresh_status()
        threading.Thread(target=_ota_worker, args=(self.session,),
                         daemon=True).start()

    def cb_remove(self):
        if self.session and self.session.ota_running:
            messagebox.showwarning("OTA Running",
                "Cannot remove while OTA is in progress.")
            return
        if self.session:
            _cancel_check_timeout(self.session)
            remove_session(self.session.mac)
        self.manager.remove_row(self)

# ─────────────────────────────────────────────
#  DEVICE MANAGER
# ─────────────────────────────────────────────
class DeviceManager:
    MAX_DEVICES = 16

    def __init__(self, scroll_frame):
        self.scroll_frame = scroll_frame
        self.rows: list[DeviceRow] = []

    def add_row(self):
        if len(self.rows) >= self.MAX_DEVICES:
            messagebox.showinfo("Limit Reached",
                f"Maximum {self.MAX_DEVICES} devices supported.")
            return
        row = DeviceRow(self.scroll_frame, self, len(self.rows))
        self.rows.append(row)
        _update_device_counter(len(self.rows))

    def remove_row(self, row: DeviceRow):
        row.card.destroy()
        self.rows.remove(row)
        _update_device_counter(len(self.rows))

    def flash_all(self):
        eligible = [r for r in self.rows
                    if r.session and r.session.device_ready
                    and r.session.firmware_path and not r.session.ota_running]
        if not eligible:
            messagebox.showinfo("Flash All",
                "No devices are READY with a firmware file selected.")
            return
        if not messagebox.askyesno("Flash All",
                f"Start OTA on {len(eligible)} device(s) simultaneously?"):
            return
        for r in eligible:
            r._start_ota()

    def check_all(self):
        if not mqtt_client:
            messagebox.showerror("Not Connected", "MQTT not ready.")
            return
        for r in self.rows:
            mac = r.mac_entry.get().strip().upper()
            if mac:
                r.cb_check()

# ─────────────────────────────────────────────
#  LOGS PANEL
#  ── If name known: show name only (no MAC) ──
#  ── If no name: show MAC only              ──
# ─────────────────────────────────────────────
class LogsPanel:
    ALL = "All Devices"

    def __init__(self, parent: tk.Frame):
        self.parent      = parent
        self._filter_mac = self.ALL

        bar = tk.Frame(parent, bg=PANEL_BG, padx=20, pady=12)
        bar.pack(fill=tk.X)

        tk.Label(bar, text="MQTT LOGS",
                 font=(FONT_MONO, 10, "bold"),
                 bg=PANEL_BG, fg=TEXT_PRI).pack(side=tk.LEFT)

        _make_btn(bar, "✕  Clear", self._clear,
                  bg=DANGER_DIM, fg=DANGER,
                  padx=12, pady=5).pack(side=tk.RIGHT)

        tk.Label(bar, text="Filter by MAC / Name:",
                 font=(FONT_UI, 9),
                 bg=PANEL_BG, fg=TEXT_SEC).pack(side=tk.RIGHT, padx=(12, 4))

        self._filter_var = tk.StringVar(value=self.ALL)
        self._filter_var.trace_add("write", self._on_filter_change)
        self._menu_btn = tk.OptionMenu(bar, self._filter_var, self.ALL)
        self._style_menu(self._menu_btn)
        self._menu_btn.pack(side=tk.RIGHT)

        tk.Frame(parent, bg=CARD_BORDER, height=1).pack(fill=tk.X)

        log_frame = tk.Frame(parent, bg=PANEL_BG)
        log_frame.pack(fill=tk.BOTH, expand=True, padx=16, pady=10)

        vscroll = ttk.Scrollbar(log_frame, orient="vertical")
        hscroll = ttk.Scrollbar(log_frame, orient="horizontal")

        self.text = tk.Text(log_frame,
                            bg=INPUT_BG, fg=TEXT_SEC,
                            font=(FONT_MONO, 9),
                            relief=tk.FLAT, bd=0,
                            wrap=tk.NONE,
                            state=tk.DISABLED,
                            yscrollcommand=vscroll.set,
                            xscrollcommand=hscroll.set,
                            highlightthickness=1,
                            highlightbackground=CARD_BORDER,
                            selectbackground=ACCENT_DIM,
                            insertbackground=TEXT_PRI)

        vscroll.config(command=self.text.yview)
        hscroll.config(command=self.text.xview)
        vscroll.pack(side=tk.RIGHT,  fill=tk.Y)
        hscroll.pack(side=tk.BOTTOM, fill=tk.X)
        self.text.pack(fill=tk.BOTH, expand=True)

        for kw, col in LOG_COLORS.items():
            self.text.tag_config(kw, foreground=col)
        self.text.tag_config("TS",      foreground=TEXT_DIM)
        self.text.tag_config("MAC",     foreground=ACCENT)
        self.text.tag_config("SYSTEM",  foreground=WARNING)
        self.text.tag_config("DEFAULT", foreground=TEXT_SEC)

        with _log_lock:
            entries = list(_log_entries)
        for e in entries:
            self._insert_entry(e, scroll=False)
        self._scroll_bottom()

    def _style_menu(self, btn):
        btn.config(bg=MUTED, fg=TEXT_PRI,
                   activebackground=ACCENT_DIM, activeforeground=TEXT_PRI,
                   relief=tk.FLAT, bd=0, highlightthickness=0,
                   font=(FONT_MONO, 9), padx=10, pady=4)
        btn["menu"].config(bg=CARD_BG, fg=TEXT_PRI,
                           activebackground=ACCENT_DIM,
                           activeforeground=TEXT_PRI,
                           relief=tk.FLAT, bd=0)

    def refresh_filter_menu(self):
        menu = self._menu_btn["menu"]
        menu.delete(0, tk.END)
        menu.add_command(label=self.ALL,
                         command=lambda: self._filter_var.set(self.ALL))
        menu.add_command(label="SYSTEM",
                         command=lambda: self._filter_var.set("SYSTEM"))
        for mac in known_macs():
            # Dropdown shows "Name (MAC)" for context, but filter key is MAC
            label = display_name_with_mac(mac)
            m = mac
            menu.add_command(label=label,
                             command=lambda v=m: self._filter_var.set(v))

    def _on_filter_change(self, *_):
        self._filter_mac = self._filter_var.get()
        self._redraw()

    def _redraw(self):
        self.text.config(state=tk.NORMAL)
        self.text.delete("1.0", tk.END)
        with _log_lock:
            entries = list(_log_entries)
        for e in entries:
            if self._matches(e):
                self._insert_entry(e, scroll=False)
        self._scroll_bottom()
        self.text.config(state=tk.DISABLED)

    def _matches(self, entry: dict) -> bool:
        if self._filter_mac == self.ALL:
            return True
        return entry["mac"] == self._filter_mac

    def append_entry(self, entry: dict):
        if self._matches(entry):
            self.text.config(state=tk.NORMAL)
            self._insert_entry(entry, scroll=True)
            self.text.config(state=tk.DISABLED)

    def _insert_entry(self, entry: dict, scroll: bool = True):
        ts   = entry["ts"]
        mac  = entry["mac"]
        text = entry["text"]
        col  = entry["color"]

        tag = "DEFAULT"
        for kw, c in LOG_COLORS.items():
            if col == c:
                tag = kw
                break
        if mac == "SYSTEM":
            tag = "SYSTEM"

        # ── Name logic: name only if set, else MAC only ──
        name = get_name(mac)
        if mac == "SYSTEM":
            mac_disp = "SYSTEM"
        elif name:
            mac_disp = name          # name only, no MAC suffix
        else:
            mac_disp = mac           # MAC only, no name prefix

        self.text.insert(tk.END, f"[{ts}] ", "TS")
        self.text.insert(tk.END, f"{mac_disp:<28}", "MAC" if mac != "SYSTEM" else "SYSTEM")
        self.text.insert(tk.END, f"  {text}\n", tag)

        if scroll:
            self._scroll_bottom()

    def _scroll_bottom(self):
        self.text.see(tk.END)

    def _clear(self):
        with _log_lock:
            _log_entries.clear()
        self.text.config(state=tk.NORMAL)
        self.text.delete("1.0", tk.END)
        self.text.config(state=tk.DISABLED)

# ─────────────────────────────────────────────
#  DEVICE INFO PANEL  (Tab: WiFi & Info)
# ─────────────────────────────────────────────
class DeviceInfoPanel:
    """Shows WiFi credentials and device info per-device (read-only name)."""

    def __init__(self, parent: tk.Frame):
        self.parent          = parent
        self._selected_mac   = tk.StringVar(value="— select device —")
        self._admin_unlocked = False

        # ── toolbar ──────────────────────────────
        bar = tk.Frame(parent, bg=PANEL_BG, padx=20, pady=12)
        bar.pack(fill=tk.X)

        tk.Label(bar, text="DEVICE INFO  ·  WiFi & Identity",
                 font=(FONT_MONO, 10, "bold"),
                 bg=PANEL_BG, fg=TEXT_PRI).pack(side=tk.LEFT)

        tk.Frame(parent, bg=CARD_BORDER, height=1).pack(fill=tk.X)

        # ── two-column layout ─────────────────────
        body = tk.Frame(parent, bg=PANEL_BG)
        body.pack(fill=tk.BOTH, expand=True, padx=20, pady=16)

        # Left: device selector
        left = tk.Frame(body, bg=PANEL_BG, width=240)
        left.pack(side=tk.LEFT, fill=tk.Y)
        left.pack_propagate(False)

        tk.Label(left, text="SELECT DEVICE",
                 font=(FONT_MONO, 7, "bold"),
                 bg=PANEL_BG, fg=TEXT_DIM, anchor="w").pack(fill=tk.X)

        lb_frame = tk.Frame(left, bg=INPUT_BG,
                            highlightthickness=1,
                            highlightbackground=CARD_BORDER)
        lb_frame.pack(fill=tk.BOTH, expand=True, pady=(6, 0))

        self._listbox = tk.Listbox(lb_frame,
                                   bg=INPUT_BG, fg=TEXT_SEC,
                                   font=(FONT_UI, 10),
                                   selectbackground=ACCENT_DIM,
                                   selectforeground=TEXT_PRI,
                                   relief=tk.FLAT, bd=0,
                                   highlightthickness=0,
                                   activestyle="none")
        self._listbox.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)
        self._listbox.bind("<<ListboxSelect>>", self._on_device_select)
        self._mac_list: list[str] = []

        # Right: detail panel
        self._right = tk.Frame(body, bg=PANEL_BG)
        self._right.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(24, 0))

        self._no_sel_lbl = tk.Label(self._right,
                                    text="Select a device on the left\nto view its info.",
                                    font=(FONT_UI, 11),
                                    bg=PANEL_BG, fg=TEXT_DIM)
        self._no_sel_lbl.pack(pady=60)

        self._detail_frame = tk.Frame(self._right, bg=PANEL_BG)
        self._build_detail()

        self.refresh_device_list()

    def _build_detail(self):
        f = self._detail_frame

        # Device name — read-only display
        tk.Label(f, text="DEVICE NAME",
                 font=(FONT_MONO, 7, "bold"),
                 bg=PANEL_BG, fg=TEXT_DIM, anchor="w").pack(fill=tk.X)
        self._name_display = tk.Label(f, text="—",
                                      font=(FONT_UI, 12, "bold"),
                                      bg=PANEL_BG, fg=SUCCESS, anchor="w")
        self._name_display.pack(anchor="w", pady=(4, 14))

        # Divider
        tk.Frame(f, bg=CARD_BORDER, height=1).pack(fill=tk.X, pady=(0, 14))

        # WiFi section header
        wifi_hdr = tk.Frame(f, bg=PANEL_BG)
        wifi_hdr.pack(fill=tk.X)
        tk.Label(wifi_hdr, text="WiFi CREDENTIALS",
                 font=(FONT_MONO, 8, "bold"),
                 bg=PANEL_BG, fg=TEXT_SEC, anchor="w").pack(side=tk.LEFT)
        self._admin_badge = tk.Label(wifi_hdr,
                                     text=" 🔒 ADMIN LOCKED ",
                                     font=(FONT_MONO, 7, "bold"),
                                     bg=DANGER_DIM, fg=DANGER,
                                     padx=6)
        self._admin_badge.pack(side=tk.LEFT, padx=(10, 0))

        # SSID
        tk.Label(f, text="SSID",
                 font=(FONT_MONO, 7, "bold"),
                 bg=PANEL_BG, fg=TEXT_DIM, anchor="w").pack(fill=tk.X, pady=(10, 2))
        self._ssid_var = tk.StringVar()
        self._ssid_entry = tk.Entry(f,
                                    textvariable=self._ssid_var,
                                    font=(FONT_MONO, 10),
                                    bg=INPUT_BG, fg=TEXT_PRI,
                                    insertbackground=TEXT_PRI,
                                    relief=tk.FLAT, bd=0,
                                    highlightthickness=1,
                                    highlightbackground=INPUT_BD,
                                    width=28)
        self._ssid_entry.pack(anchor="w", ipady=5, ipadx=6)

        # Password (admin unlock to reveal plain text)
        tk.Label(f, text="PASSWORD",
                 font=(FONT_MONO, 7, "bold"),
                 bg=PANEL_BG, fg=TEXT_DIM, anchor="w").pack(fill=tk.X, pady=(10, 2))
        pw_row = tk.Frame(f, bg=PANEL_BG)
        pw_row.pack(fill=tk.X)
        self._pw_var = tk.StringVar()
        self._pw_entry = tk.Entry(pw_row,
                                  textvariable=self._pw_var,
                                  font=(FONT_MONO, 10),
                                  bg=INPUT_BG, fg=TEXT_PRI,
                                  insertbackground=TEXT_PRI,
                                  show="●",
                                  relief=tk.FLAT, bd=0,
                                  highlightthickness=1,
                                  highlightbackground=INPUT_BD,
                                  state=tk.DISABLED,
                                  disabledbackground=INPUT_BG,
                                  disabledforeground=TEXT_DIM,
                                  width=22)
        self._pw_entry.pack(side=tk.LEFT, ipady=5, ipadx=6)
        self._unlock_btn = _make_btn(pw_row, "🔒 Unlock",
                                     self._request_admin,
                                     bg=DANGER_DIM, fg=WARNING,
                                     padx=10, pady=5)
        self._unlock_btn.pack(side=tk.LEFT, padx=(8, 0))

        # Action row
        action_row = tk.Frame(f, bg=PANEL_BG)
        action_row.pack(fill=tk.X, pady=(16, 0))
        _make_btn(action_row, "⬆  Push to Device",
                  self._push_wifi,
                  bg="#1a3a1a", fg=SUCCESS, padx=12, pady=6
                  ).pack(side=tk.LEFT)

        self._wifi_status = tk.Label(f, text="",
                                     font=(FONT_UI, 9),
                                     bg=PANEL_BG, fg=TEXT_SEC, anchor="w")
        self._wifi_status.pack(anchor="w", pady=(8, 0))

    def refresh_device_list(self):
        macs = known_macs()
        self._mac_list = macs
        self._listbox.delete(0, tk.END)
        for mac in macs:
            n = get_name(mac)
            self._listbox.insert(tk.END, f"  {n if n else mac}")

    def _on_device_select(self, *_):
        sel = self._listbox.curselection()
        if not sel:
            return
        idx = sel[0]
        if idx >= len(self._mac_list):
            return
        mac = self._mac_list[idx]
        self._selected_mac.set(mac)
        self._no_sel_lbl.pack_forget()
        self._detail_frame.pack(fill=tk.BOTH, expand=True)
        self._load_device(mac)

    def _load_device(self, mac: str):
        # Name — read-only
        n = get_name(mac)
        self._name_display.config(text=n if n else mac)

        session = get_session(mac)
        if session and session.wifi_received:
            self._ssid_var.set(session.wifi_ssid)
            if self._admin_unlocked:
                # ── FIX: show plain text when admin unlocked ──
                self._pw_var.set(session.wifi_password)
                self._pw_entry.config(
                    state=tk.NORMAL,
                    show=""                   # remove masking
                )
            else:
                self._pw_var.set("")
                self._pw_entry.config(
                    state=tk.DISABLED,
                    show="●"
                )
            self._wifi_status.config(
                text="✓ WiFi config received from device", fg=SUCCESS)
        else:
            self._ssid_var.set("")
            self._pw_var.set("")
            self._pw_entry.config(state=tk.DISABLED, show="●")
            self._wifi_status.config(
                text="Waiting for WiFi config from device…", fg=TEXT_DIM)

        if self._admin_unlocked:
            self._admin_badge.config(text=" 🔓 ADMIN UNLOCKED ", bg="#0d2a0d", fg=SUCCESS)
            self._unlock_btn.config(text="🔓 Unlocked", bg="#0d2a0d", fg=SUCCESS)
        else:
            self._admin_badge.config(text=" 🔒 ADMIN LOCKED ", bg=DANGER_DIM, fg=DANGER)
            self._unlock_btn.config(text="🔒 Unlock", bg=DANGER_DIM, fg=WARNING)

    def _request_admin(self):
        def _on_unlock():
            self._admin_unlocked = True
            mac = self._selected_mac.get()
            if mac != "— select device —":
                # ── FIX: immediately reveal password on unlock ──
                self._load_device(mac)
        _ask_admin_passkey(_on_unlock)

    def _push_wifi(self):
        mac = self._selected_mac.get()
        if mac == "— select device —":
            messagebox.showerror("No Device", "Select a device first.")
            return
        if not mqtt_client:
            messagebox.showerror("Not Connected", "MQTT not ready.")
            return
        ssid = self._ssid_var.get().strip()
        if not ssid:
            messagebox.showerror("Missing SSID",
                "No SSID available. WiFi config may not have been received yet.")
            return

        def _do_push(pw: str):
            if not messagebox.askyesno("Push WiFi Config",
                    f"Send WiFi credentials to {display_name(mac)}?\n\n"
                    f"SSID: {ssid}\n"
                    "The device will save these to NVS and reconnect."):
                return
            payload = json.dumps({"ssid": ssid, "password": pw})
            mqtt_client.publish(f"{mac}/wifi/set", payload, qos=COMMAND_QOS)
            self._wifi_status.config(text="✓ Pushed to device", fg=SUCCESS)
            _add_log(mac, f"WiFi credentials pushed  ssid={ssid}", SUCCESS)

        _ask_wifi_password(_do_push)

    def on_wifi_received(self, mac: str):
        if self._selected_mac.get() == mac:
            self._load_device(mac)
        self.refresh_device_list()

# ─────────────────────────────────────────────
#  SETTINGS PANEL  (Tab: Device Names + WiFi)
# ─────────────────────────────────────────────
class SettingsPanel:
    def __init__(self, parent: tk.Frame):
        self.parent          = parent
        self._admin_unlocked = False

        # Header
        hdr = tk.Frame(parent, bg=PANEL_BG, padx=20, pady=12)
        hdr.pack(fill=tk.X)
        tk.Label(hdr, text="SETTINGS",
                 font=(FONT_MONO, 10, "bold"),
                 bg=PANEL_BG, fg=TEXT_PRI).pack(side=tk.LEFT)
        tk.Frame(parent, bg=CARD_BORDER, height=1).pack(fill=tk.X)

        # Tab bar
        tab_bar = tk.Frame(parent, bg=SIDEBAR_BG)
        tab_bar.pack(fill=tk.X)

        self._tab_frames: dict[str, tk.Frame] = {}
        self._tab_btns:   dict[str, tk.Button] = {}
        self._active_tab  = [None]

        content_area = tk.Frame(parent, bg=PANEL_BG)
        content_area.pack(fill=tk.BOTH, expand=True)

        def make_tab(key, label):
            f = tk.Frame(content_area, bg=PANEL_BG)
            self._tab_frames[key] = f

            def _switch(k=key):
                for k2, f2 in self._tab_frames.items():
                    if k2 == k:
                        f2.pack(fill=tk.BOTH, expand=True)
                        self._tab_btns[k2].config(bg=PANEL_BG, fg=TEXT_PRI)
                    else:
                        f2.pack_forget()
                        self._tab_btns[k2].config(bg=SIDEBAR_BG, fg=TEXT_DIM)
                self._active_tab[0] = k

            b = tk.Button(tab_bar, text=label,
                          font=(FONT_UI, 9, "bold"),
                          bg=SIDEBAR_BG, fg=TEXT_DIM,
                          activebackground=PANEL_BG, activeforeground=TEXT_PRI,
                          relief=tk.FLAT, bd=0, highlightthickness=0,
                          padx=18, pady=9, cursor="hand2",
                          command=_switch)
            b.pack(side=tk.LEFT)
            self._tab_btns[key] = b
            return f, _switch

        tab_names, sw_names = make_tab("names",  "  ✎  Device Names  ")
        tab_wifi,  sw_wifi  = make_tab("wifi",   "  ⚙  WiFi Settings  ")

        self._build_names_tab(tab_names)
        self._build_wifi_tab(tab_wifi)

        # Default tab
        sw_names()

    # ── Tab 1: Device Names ─────────────────────
    def _build_names_tab(self, f: tk.Frame):
        info = tk.Frame(f, bg=PANEL_BG, padx=24, pady=16)
        info.pack(fill=tk.X)

        tk.Label(info,
                 text="Assign friendly names to devices.\n"
                      "Names are saved locally and displayed throughout the app.\n"
                      "Only devices that have been discovered or checked appear here.",
                 font=(FONT_UI, 9), bg=PANEL_BG, fg=TEXT_SEC,
                 justify=tk.LEFT, anchor="w").pack(anchor="w")

        tk.Frame(f, bg=CARD_BORDER, height=1).pack(fill=tk.X, padx=20)

        # Scrollable card list
        canvas    = tk.Canvas(f, bg=PANEL_BG, bd=0, highlightthickness=0)
        scrollbar = ttk.Scrollbar(f, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        canvas.pack(fill=tk.BOTH, expand=True)

        self._names_inner = tk.Frame(canvas, bg=PANEL_BG)
        cw = canvas.create_window((0, 0), window=self._names_inner, anchor="nw")

        self._names_inner.bind("<Configure>",
            lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>",
            lambda e: canvas.itemconfig(cw, width=e.width))

        self._names_canvas = canvas
        self._names_cw     = cw
        self._name_rows:   list[tk.Frame] = []

        self.refresh_device_list()

    def refresh_device_list(self):
        """Rebuild name rows from known sessions."""
        for w in self._names_inner.winfo_children():
            w.destroy()
        self._name_rows.clear()

        macs = known_macs()
        if not macs:
            tk.Label(self._names_inner,
                     text="No devices discovered yet.\nConnect devices or run Check on the OTA Flash page.",
                     font=(FONT_UI, 10), bg=PANEL_BG, fg=TEXT_DIM
                     ).pack(pady=40)
            return

        for mac in macs:
            self._add_name_row(mac)

    def _add_name_row(self, mac: str):
        card = tk.Frame(self._names_inner, bg=CARD_BG,
                        highlightthickness=1,
                        highlightbackground=CARD_BORDER)
        card.pack(fill=tk.X, padx=20, pady=(10, 0))

        tk.Frame(card, bg=ACCENT_DIM, height=2).pack(fill=tk.X)

        row = tk.Frame(card, bg=CARD_BG, padx=16, pady=12)
        row.pack(fill=tk.X)

        # MAC
        left = tk.Frame(row, bg=CARD_BG)
        left.pack(side=tk.LEFT, fill=tk.Y, padx=(0, 20))
        tk.Label(left, text="MAC ADDRESS",
                 font=(FONT_MONO, 7, "bold"),
                 bg=CARD_BG, fg=TEXT_DIM, anchor="w").pack(anchor="w")
        tk.Label(left, text=mac,
                 font=(FONT_MONO, 10),
                 bg=CARD_BG, fg=TEXT_SEC, anchor="w").pack(anchor="w")

        # Name input
        mid = tk.Frame(row, bg=CARD_BG)
        mid.pack(side=tk.LEFT, fill=tk.X, expand=True)
        tk.Label(mid, text="FRIENDLY NAME",
                 font=(FONT_MONO, 7, "bold"),
                 bg=CARD_BG, fg=TEXT_DIM, anchor="w").pack(anchor="w")

        inp_row = tk.Frame(mid, bg=CARD_BG)
        inp_row.pack(fill=tk.X)
        name_var = tk.StringVar(value=get_name(mac))
        entry = tk.Entry(inp_row,
                         textvariable=name_var,
                         font=(FONT_UI, 11, "bold"),
                         bg=INPUT_BG, fg=SUCCESS,
                         insertbackground=TEXT_PRI,
                         relief=tk.FLAT, bd=0,
                         highlightthickness=1,
                         highlightbackground=INPUT_BD,
                         highlightcolor=INPUT_FOCUS,
                         width=26)
        entry.pack(side=tk.LEFT, ipady=5, ipadx=8)

        def _save(m=mac, v=name_var):
            n = v.get().strip()
            if not n:
                return
            set_name(m, n)
            _add_log(m, f"Name saved → \"{n}\"", ACCENT)
            # Refresh other panels
            if _info_panel:
                root.after(50, _info_panel.refresh_device_list)
            if _log_panel:
                root.after(50, _log_panel.refresh_filter_menu)

        _make_btn(inp_row, "Save", _save,
                  bg=ACCENT_DIM, fg=ACCENT, padx=10, pady=5
                  ).pack(side=tk.LEFT, padx=(8, 0))

        self._name_rows.append(card)

    # ── Tab 2: WiFi Settings ────────────────────
    def _build_wifi_tab(self, f: tk.Frame):
        info = tk.Frame(f, bg=PANEL_BG, padx=24, pady=14)
        info.pack(fill=tk.X)
        tk.Label(info,
                 text="View and push WiFi credentials to ESP32 devices.\n"
                      "Passwords are hidden — click Unlock and enter the admin passkey to reveal.\n"
                      "Topic: <MAC>/wifi/set  →  {\"ssid\":\"…\",\"password\":\"…\"}",
                 font=(FONT_UI, 9), bg=PANEL_BG, fg=TEXT_SEC,
                 justify=tk.LEFT, anchor="w").pack(anchor="w")
        tk.Frame(f, bg=CARD_BORDER, height=1).pack(fill=tk.X, padx=20)

        # Device selector + form
        body = tk.Frame(f, bg=PANEL_BG, padx=24, pady=16)
        body.pack(fill=tk.BOTH, expand=True)

        # Device picker
        tk.Label(body, text="SELECT DEVICE",
                 font=(FONT_MONO, 7, "bold"),
                 bg=PANEL_BG, fg=TEXT_DIM, anchor="w").pack(anchor="w")

        pick_row = tk.Frame(body, bg=PANEL_BG)
        pick_row.pack(fill=tk.X, pady=(4, 16))

        self._wifi_mac_var = tk.StringVar(value="— select —")
        self._wifi_mac_menu = tk.OptionMenu(pick_row, self._wifi_mac_var, "— select —")
        self._wifi_mac_menu.config(
            bg=MUTED, fg=TEXT_PRI,
            activebackground=ACCENT_DIM, activeforeground=TEXT_PRI,
            relief=tk.FLAT, bd=0, highlightthickness=0,
            font=(FONT_MONO, 10), padx=12, pady=6)
        self._wifi_mac_menu["menu"].config(
            bg=CARD_BG, fg=TEXT_PRI,
            activebackground=ACCENT_DIM,
            activeforeground=TEXT_PRI,
            relief=tk.FLAT, bd=0)
        self._wifi_mac_menu.pack(side=tk.LEFT)

        _make_btn(pick_row, "⬇  Request from Device",
                  self._settings_request_wifi,
                  bg=ACCENT_DIM, fg=ACCENT, padx=12, pady=6
                  ).pack(side=tk.LEFT, padx=(12, 0))

        # Admin unlock row
        admin_row = tk.Frame(body, bg=PANEL_BG)
        admin_row.pack(fill=tk.X, pady=(0, 14))
        self._settings_admin_badge = tk.Label(admin_row,
                                              text=" 🔒 PASSWORDS HIDDEN ",
                                              font=(FONT_MONO, 7, "bold"),
                                              bg=DANGER_DIM, fg=DANGER, padx=6)
        self._settings_admin_badge.pack(side=tk.LEFT)
        self._settings_unlock_btn = _make_btn(admin_row, "Unlock Passwords",
                                              self._settings_request_admin,
                                              bg=DANGER_DIM, fg=WARNING,
                                              padx=10, pady=4)
        self._settings_unlock_btn.pack(side=tk.LEFT, padx=(10, 0))

        # SSID
        tk.Label(body, text="SSID",
                 font=(FONT_MONO, 7, "bold"),
                 bg=PANEL_BG, fg=TEXT_DIM, anchor="w").pack(anchor="w")
        self._settings_ssid_var = tk.StringVar()
        tk.Entry(body,
                 textvariable=self._settings_ssid_var,
                 font=(FONT_MONO, 10),
                 bg=INPUT_BG, fg=TEXT_PRI,
                 insertbackground=TEXT_PRI,
                 relief=tk.FLAT, bd=0,
                 highlightthickness=1, highlightbackground=INPUT_BD,
                 width=30
                 ).pack(anchor="w", ipady=5, ipadx=6, pady=(4, 12))

        # Password display (admin-unlock reveals plain text)
        tk.Label(body, text="PASSWORD",
                 font=(FONT_MONO, 7, "bold"),
                 bg=PANEL_BG, fg=TEXT_DIM, anchor="w").pack(anchor="w")
        pw_row2 = tk.Frame(body, bg=PANEL_BG)
        pw_row2.pack(anchor="w", pady=(4, 16))
        self._settings_pw_var = tk.StringVar()
        self._settings_pw_entry = tk.Entry(pw_row2,
                                           textvariable=self._settings_pw_var,
                                           font=(FONT_MONO, 10),
                                           bg=INPUT_BG, fg=TEXT_PRI,
                                           insertbackground=TEXT_PRI,
                                           show="●",
                                           relief=tk.FLAT, bd=0,
                                           highlightthickness=1,
                                           highlightbackground=INPUT_BD,
                                           state=tk.DISABLED,
                                           disabledbackground=INPUT_BG,
                                           disabledforeground=TEXT_DIM,
                                           width=26)
        self._settings_pw_entry.pack(side=tk.LEFT, ipady=5, ipadx=6)

        # Apply WiFi Credentials button
        action_row = tk.Frame(body, bg=PANEL_BG)
        action_row.pack(anchor="w")

        _make_btn(action_row, "⬆  Apply WiFi Credentials",
                  self._settings_push_wifi,
                  bg="#1a3a1a", fg=SUCCESS, padx=14, pady=8
                  ).pack(side=tk.LEFT)

        _make_btn(action_row, "Reset Configuration",
                  self._settings_reset_config,
                  bg=DANGER_DIM, fg=DANGER, padx=14, pady=8
                  ).pack(side=tk.LEFT, padx=(10, 0))

        self._settings_wifi_status = tk.Label(body, text="",
                                              font=(FONT_UI, 9),
                                              bg=PANEL_BG, fg=TEXT_SEC, anchor="w")
        self._settings_wifi_status.pack(anchor="w", pady=(8, 0))

        self._update_wifi_mac_menu()

    def _update_wifi_mac_menu(self):
        menu = self._wifi_mac_menu["menu"]
        menu.delete(0, tk.END)
        menu.add_command(label="— select —",
                         command=lambda: self._wifi_mac_var.set("— select —"))
        for mac in known_macs():
            label = display_name_with_mac(mac)
            m     = mac
            menu.add_command(label=label,
                             command=lambda v=m: self._wifi_mac_var.set(v))

    def _settings_request_admin(self):
        def _on_unlock():
            self._admin_unlocked = True
            # ── FIX: remove masking and enable entry on unlock ──
            self._settings_pw_entry.config(state=tk.NORMAL, show="")
            self._settings_admin_badge.config(
                text=" 🔓 PASSWORDS VISIBLE ", bg="#0d2a0d", fg=SUCCESS)
            self._settings_unlock_btn.config(
                text="🔓 Unlocked", bg="#0d2a0d", fg=SUCCESS)
        _ask_admin_passkey(_on_unlock)

    def _settings_request_wifi(self):
        mac = self._wifi_mac_var.get()
        if mac in ("— select —", ""):
            messagebox.showerror("No Device", "Select a device first.")
            return
        if not mqtt_client:
            messagebox.showerror("Not Connected", "MQTT not ready.")
            return
        mqtt_client.subscribe(f"{mac}/wifi/config", qos=1)
        mqtt_client.publish(f"{mac}/wifi/request", "GET_CONFIG", qos=COMMAND_QOS)
        self._settings_wifi_status.config(
            text="Requesting config from device…", fg=WARNING)
        _add_log(mac, "WiFi config requested (Settings)", TEXT_SEC)

    def _settings_push_wifi(self):
        mac = self._wifi_mac_var.get()
        if mac in ("— select —", ""):
            messagebox.showerror("No Device", "Select a device first.")
            return
        if not mqtt_client:
            messagebox.showerror("Not Connected", "MQTT not ready.")
            return
        ssid = self._settings_ssid_var.get().strip()
        if not ssid:
            messagebox.showerror("Missing SSID", "Enter the WiFi SSID.")
            return

        def _do_push(pw: str):
            if not messagebox.askyesno("Apply WiFi Credentials",
                    f"Send credentials to {display_name(mac)}?\n\nSSID: {ssid}"):
                return
            mqtt_client.publish(f"{mac}/wifi/set",
                                json.dumps({"ssid": ssid, "password": pw}),
                                qos=COMMAND_QOS)
            self._settings_wifi_status.config(text="✓ Pushed to device", fg=SUCCESS)
            _add_log(mac, f"WiFi credentials pushed  ssid={ssid}", SUCCESS)

        _ask_wifi_password(_do_push)

    def _settings_reset_config(self):
        mac = self._wifi_mac_var.get()
        if mac in ("— select —", ""):
            messagebox.showerror("No Device", "Select a device first.")
            return
        if not mqtt_client:
            messagebox.showerror("Not Connected", "MQTT not ready.")
            return

        def _do_reset():
            if not messagebox.askyesno(
                    "Reset Configuration",
                    f"Reset saved device name and WiFi credentials on {display_name(mac)}?\n\n"
                    "The ESP32 will reboot and start the setup hotspot. The user must enter the device name and WiFi password again."):
                return

            mqtt_client.publish(f"{mac}/reset_config", "RESET_CONFIG", qos=COMMAND_QOS)
            set_name(mac, "")

            session = get_session(mac)
            if session:
                session.wifi_ssid = ""
                session.wifi_password = ""
                session.wifi_received = False

            self._settings_ssid_var.set("")
            self._settings_pw_var.set("")
            self._settings_wifi_status.config(
                text="Reset command sent. Device will reboot to setup portal.", fg=WARNING)
            _add_log(mac, "Reset configuration command sent", WARNING)

        _ask_admin_passkey(
            _do_reset,
            "Enter the admin passkey to reset this device configuration.")

    def on_wifi_received(self, mac: str):
        if self._wifi_mac_var.get() == mac:
            session = get_session(mac)
            if session:
                self._settings_ssid_var.set(session.wifi_ssid)
                if self._admin_unlocked:
                    # ── FIX: show plain text if already unlocked ──
                    self._settings_pw_var.set(session.wifi_password)
                    self._settings_pw_entry.config(state=tk.NORMAL, show="")
                else:
                    self._settings_pw_var.set("")
            self._settings_wifi_status.config(
                text="✓ Received from device", fg=SUCCESS)

# ─────────────────────────────────────────────
#  GUI BUILD
# ─────────────────────────────────────────────
def _update_device_counter(n):
    if device_count_lbl:
        device_count_lbl.config(text=f"{n} device{'s' if n != 1 else ''}")

def build_gui():
    global device_count_lbl, sb_dot, sb_text
    global _log_panel, _settings_panel, _info_panel

    _load_names()

    win = tk.Tk()
    win.title("AceTech ESP32 OTA-FU  ·  Multi-Device")
    win.geometry("980x780")
    win.minsize(760, 540)
    win.configure(bg=BG)

    # ── TITLEBAR ──────────────────────────────
    tb = tk.Frame(win, bg=BG, height=58)
    tb.pack(fill=tk.X)
    tb.pack_propagate(False)

    logo = tk.Frame(tb, bg=ACCENT, width=58, height=58)
    logo.pack(side=tk.LEFT)
    logo.pack_propagate(False)
    tk.Label(logo, text="⬡", font=(FONT_MONO, 22, "bold"),
             bg=ACCENT, fg="white").place(relx=0.5, rely=0.5, anchor="center")

    title_block = tk.Frame(tb, bg=BG)
    title_block.pack(side=tk.LEFT, padx=16, fill=tk.Y, pady=8)
    tk.Label(title_block, text="AceTech ESP32 OTA-FU",
             font=(FONT_UI, 13, "bold"),
             bg=BG, fg=TEXT_PRI, anchor="w").pack(anchor="w")
    tk.Label(title_block, text="MULTI-DEVICE  ·  MQTT ONLY",
             font=(FONT_MONO, 8),
             bg=BG, fg=TEXT_DIM, anchor="w").pack(anchor="w")

    tk.Label(tb, text=f"v{APP_VERSION}",
             font=(FONT_MONO, 9, "bold"),
             bg=BG, fg=TEXT_DIM).pack(side=tk.RIGHT, padx=18)

    # ── STATUSBAR ─────────────────────────────
    statusbar = tk.Frame(win, bg=SIDEBAR_BG, height=26)
    statusbar.pack(side=tk.BOTTOM, fill=tk.X)
    statusbar.pack_propagate(False)

    sb_dot = tk.Label(statusbar, text="●",
                      font=(FONT_MONO, 11),
                      bg=SIDEBAR_BG, fg=TEXT_DIM, padx=10)
    sb_dot.pack(side=tk.LEFT)
    sb_text = tk.Label(statusbar, text="Initialising…",
                       font=(FONT_MONO, 9),
                       bg=SIDEBAR_BG, fg=TEXT_DIM, anchor="w")
    sb_text.pack(side=tk.LEFT)
    tk.Label(statusbar,
             text=(f"chunk {CHUNK_SIZE}B  ·  ack {ACK_TIMEOUT}s  ·  "
                   f"retries {MAX_RETRIES}  ·  timeout {CHECK_TIMEOUT_MS//1000}s"),
             font=(FONT_MONO, 8),
             bg=SIDEBAR_BG, fg=TEXT_DIM).pack(side=tk.RIGHT, padx=12)

    # ── BODY ──────────────────────────────────
    body = tk.Frame(win, bg=BG)
    body.pack(fill=tk.BOTH, expand=True)

    sidebar = tk.Frame(body, bg=SIDEBAR_BG, width=180)
    sidebar.pack(side=tk.LEFT, fill=tk.Y)
    sidebar.pack_propagate(False)
    tk.Frame(sidebar, bg=CARD_BORDER, height=1).pack(fill=tk.X)

    content = tk.Frame(body, bg=PANEL_BG)
    content.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

    # ══ PAGE: OTA Flash ═══════════════════════
    page_ota = tk.Frame(content, bg=PANEL_BG)
    tk.Frame(page_ota, bg=CARD_BORDER, height=1).pack(fill=tk.X)

    ota_toolbar = tk.Frame(page_ota, bg=PANEL_BG, padx=20, pady=12)
    ota_toolbar.pack(fill=tk.X)

    device_count_lbl = tk.Label(ota_toolbar, text="0 devices",
                                 font=(FONT_MONO, 9, "bold"),
                                 bg=PANEL_BG, fg=TEXT_SEC)
    device_count_lbl.pack(side=tk.LEFT, padx=(0, 20))

    _manager_ref = [None]

    def tb_btn(parent, text, cmd, bg=MUTED, fg=TEXT_SEC):
        b = tk.Button(parent, text=text,
                      font=(FONT_UI, 9, "bold"),
                      bg=bg, fg=fg,
                      activebackground=ACCENT_HOV, activeforeground="white",
                      relief=tk.FLAT, bd=0, highlightthickness=0,
                      padx=14, pady=6, cursor="hand2", command=cmd)
        b.pack(side=tk.RIGHT, padx=(6, 0))
        return b

    tb_btn(ota_toolbar, "⚡  Flash All",
           lambda: _manager_ref[0] and _manager_ref[0].flash_all(),
           bg="#1a3a1a", fg=SUCCESS)
    tb_btn(ota_toolbar, "◎  Check All",
           lambda: _manager_ref[0] and _manager_ref[0].check_all(),
           bg=ACCENT_DIM, fg=ACCENT)
    tb_btn(ota_toolbar, "＋  Add Device",
           lambda: _manager_ref[0] and _manager_ref[0].add_row(),
           bg=MUTED, fg=TEXT_SEC)

    tk.Frame(page_ota, bg=CARD_BORDER, height=1).pack(fill=tk.X)

    canvas    = tk.Canvas(page_ota, bg=PANEL_BG, bd=0, highlightthickness=0)
    scrollbar = ttk.Scrollbar(page_ota, orient="vertical", command=canvas.yview)
    canvas.configure(yscrollcommand=scrollbar.set)
    scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
    canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

    scroll_frame = tk.Frame(canvas, bg=PANEL_BG)
    canvas_win   = canvas.create_window((0, 0), window=scroll_frame, anchor="nw")

    scroll_frame.bind("<Configure>",
        lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
    canvas.bind("<Configure>",
        lambda e: canvas.itemconfig(canvas_win, width=e.width))
    canvas.bind_all("<MouseWheel>",
        lambda e: canvas.yview_scroll(int(-1*(e.delta/120)), "units"))

    inner = tk.Frame(scroll_frame, bg=PANEL_BG, padx=16, pady=14)
    inner.pack(fill=tk.BOTH, expand=True)

    empty_lbl = tk.Label(inner,
                         text="Click  + Add Device  to add manually\n"
                              "— or power on an ESP32 to auto-discover it —",
                         font=(FONT_UI, 12), bg=PANEL_BG, fg=TEXT_DIM)
    empty_lbl.pack(pady=60)

    manager = DeviceManager(inner)
    manager._empty_lbl = empty_lbl
    _manager_ref[0]    = manager
    _auto_add_device_row._manager = manager

    original_add = manager.add_row
    def patched_add():
        empty_lbl.pack_forget()
        original_add()
    manager.add_row = patched_add

    # ══ PAGE: Device Info ═════════════════════
    page_info   = tk.Frame(content, bg=PANEL_BG)
    info_panel  = DeviceInfoPanel(page_info)
    _info_panel = info_panel

    # ══ PAGE: Logs ════════════════════════════
    page_logs  = tk.Frame(content, bg=PANEL_BG)
    logs_panel = LogsPanel(page_logs)
    _log_panel = logs_panel

    # ══ PAGE: Settings ════════════════════════
    page_settings  = tk.Frame(content, bg=PANEL_BG)
    settings_panel = SettingsPanel(page_settings)
    _settings_panel = settings_panel

    # ── PAGE SWITCHER ─────────────────────────
    pages = {
        "ota"     : page_ota,
        "info"    : page_info,
        "logs"    : page_logs,
        "settings": page_settings,
    }
    _active_page = [None]

    def show_page(key: str):
        if _active_page[0] == key:
            return

        # ── Reset admin unlock when navigating away ──
        prev = _active_page[0]
        if prev == "info":
            info_panel._admin_unlocked = False
            info_panel._pw_entry.config(state=tk.DISABLED, show="●")
            info_panel._pw_var.set("")
            info_panel._admin_badge.config(
                text=" 🔒 ADMIN LOCKED ", bg=DANGER_DIM, fg=DANGER)
            info_panel._unlock_btn.config(
                text="🔒 Unlock", bg=DANGER_DIM, fg=WARNING)
        if prev == "settings":
            settings_panel._admin_unlocked = False
            settings_panel._settings_pw_entry.config(state=tk.DISABLED, show="●")
            settings_panel._settings_pw_var.set("")
            settings_panel._settings_admin_badge.config(
                text=" 🔒 PASSWORDS HIDDEN ", bg=DANGER_DIM, fg=DANGER)
            settings_panel._settings_unlock_btn.config(
                text="Unlock Passwords", bg=DANGER_DIM, fg=WARNING)

        _active_page[0] = key
        for k, p in pages.items():
            if k == key:
                p.pack(fill=tk.BOTH, expand=True)
            else:
                p.pack_forget()
        _build_nav(key)
        if key == "logs":
            logs_panel.refresh_filter_menu()
        if key == "info":
            info_panel.refresh_device_list()
        if key == "settings":
            settings_panel.refresh_device_list()
            settings_panel._update_wifi_mac_menu()

    # ── SIDEBAR NAV ───────────────────────────
    NAV_ITEMS = [
        ("⬡", "OTA Flash",   "ota"),
        ("◈", "Device Info", "info"),
        ("≡", "Logs",        "logs"),
        ("⚙", "Settings",    "settings"),
    ]

    def _build_nav(active_key: str):
        for w in sidebar.winfo_children():
            if isinstance(w, tk.Frame) and w != sidebar.winfo_children()[0]:
                w.destroy()
            elif isinstance(w, tk.Label):
                w.destroy()

        for icon, label, key in NAV_ITEMS:
            is_active = (key == active_key)
            bg = ACCENT if is_active else SIDEBAR_BG
            fg = "white" if is_active else TEXT_SEC

            row_frame = tk.Frame(sidebar, bg=bg, cursor="hand2")
            row_frame.pack(fill=tk.X)
            tk.Frame(row_frame, bg=ACCENT if is_active else SIDEBAR_BG,
                     width=3).pack(side=tk.LEFT, fill=tk.Y)
            lbl = tk.Label(row_frame,
                           text=f"  {icon}  {label}",
                           font=(FONT_UI, 9, "bold" if is_active else "normal"),
                           bg=bg, fg=fg, anchor="w", pady=11, cursor="hand2")
            lbl.pack(side=tk.LEFT, fill=tk.X, expand=True)

            k = key
            row_frame.bind("<Button-1>", lambda e, k=k: show_page(k))
            lbl.bind      ("<Button-1>", lambda e, k=k: show_page(k))

        tk.Frame(sidebar, bg=CARD_BORDER, height=1).pack(fill=tk.X, pady=(10, 0))
        for lbl_text, val in [
            ("BROKER",      MQTT_BROKER),
            ("PORT",        str(MQTT_PORT)),
            ("MAX DEVICES", str(DeviceManager.MAX_DEVICES)),
        ]:
            tk.Label(sidebar, text=lbl_text,
                     font=(FONT_MONO, 7, "bold"),
                     bg=SIDEBAR_BG, fg=TEXT_DIM, anchor="w"
                     ).pack(fill=tk.X, padx=14, pady=(10, 1))
            tk.Label(sidebar, text=val,
                     font=(FONT_MONO, 8),
                     bg=SIDEBAR_BG, fg=TEXT_SEC, anchor="w",
                     wraplength=160).pack(fill=tk.X, padx=14)

    show_page("ota")

    return win, manager


# ─────────────────────────────────────────────
#  ENTRY POINT
# ─────────────────────────────────────────────
if __name__ == "__main__":
    root, manager = build_gui()
    root.after(200, _connect_mqtt)
    root.mainloop()
    if mqtt_client:
        mqtt_client.loop_stop()
        mqtt_client.disconnect()
