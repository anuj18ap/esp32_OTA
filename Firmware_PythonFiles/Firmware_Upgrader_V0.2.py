"""
================================================================
 ESP32 Multi-Device OTA Firmware Upgrader — MQTT
================================================================
 Dependencies:  pip install paho-mqtt
 Python 3.8+  |  paho-mqtt 2.x

 MQTT Topics (publish from ESP32):
   <MAC>/info          → JSON  {"id":"<MAC>","firmware":"<ver>"}
   <MAC>/ota_status    → str   READY | UPDATING | FAILED | NO_UPDATE | SUCCESS
   <MAC>/ota/ack       → str   "<chunk_index>"
   <MAC>/log           → str   any log line you want to display

 Topics published TO the ESP32 from this app:
   <MAC>/ota_check     → "ARE_YOU_READY"
   <MAC>/ota/begin     → JSON  {"size":N,"chunks":N,"crc32":N}
   <MAC>/ota/chunk     → binary  [4-byte big-endian index][data]
   <MAC>/ota/end       → "END"
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
APP_VERSION      = 1.0
MQTT_BROKER      = "broker.emqx.io"
MQTT_PORT        = 1883
CHUNK_SIZE       = 7168
ACK_TIMEOUT      = 5
MAX_RETRIES      = 3
CHUNK_QOS        = 0
COMMAND_QOS      = 1
BEGIN_DELAY      = 0.15
CHECK_TIMEOUT_MS = 8000
MAX_LOG_LINES    = 2000        # cap log buffer

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

# Log-line colour by level keyword
LOG_COLORS = {
    "ERROR"  : DANGER,
    "WARN"   : WARNING,
    "INFO"   : TEXT_PRI,
    "DEBUG"  : TEXT_DIM,
    "OTA"    : ACCENT,
    "SUCCESS": SUCCESS,
}

# ─────────────────────────────────────────────
#  GLOBAL LOG STORE
#  Each entry: {"ts": str, "mac": str, "text": str, "color": str}
# ─────────────────────────────────────────────
_log_entries: list[dict] = []
_log_lock    = threading.Lock()
_log_panel: Optional["LogsPanel"] = None   # set after build_gui()

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

def _on_connect(client, userdata, flags, rc, props):
    if rc == 0:
        client.subscribe("+/info",       qos=0)
        client.subscribe("+/ota_status", qos=1)
        client.subscribe("+/ota/ack",    qos=1)
        client.subscribe("+/log",        qos=0)   # device log lines
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

    # ── /info — auto-discovery ──────────────────
    if topic.endswith("/info"):
        try:
            d = json.loads(payload)
            if not isinstance(d, dict):
                return
            discovered_mac = str(d.get("id", mac)).upper().replace(":", "")
            fw_version     = str(d.get("firmware", "?"))
        except Exception:
            return

        existing = get_session(discovered_mac)
        if existing:
            existing.fw_version = f"v{fw_version}"
            root.after(0, lambda s=existing: s.ui and s.ui.refresh_fw_version())
        else:
            session = get_or_create_session(discovered_mac)
            session.fw_version    = f"v{fw_version}"
            session.status        = "ready"
            session.status_text   = "AUTO-DISCOVERED"
            session.device_ready  = True
            session.auto_discovered = True
            _add_log(discovered_mac, f"Auto-discovered  fw=v{fw_version}", ACCENT)
            root.after(0, lambda s=session, fv=fw_version:
                       _auto_add_device_row(s, fv))
            root.after(0, lambda: _log_panel and _log_panel.refresh_filter_menu())
        return

    # ── /log — device log line ──────────────────
    if topic.endswith("/log"):
        col = TEXT_SEC
        up  = payload.upper()
        for kw, c in LOG_COLORS.items():
            if kw in up:
                col = c
                break
        _add_log(mac, payload, col)
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


def _auto_add_device_row(session: "DeviceSession", fw_version: str):
    if not hasattr(_auto_add_device_row, "_manager") or _auto_add_device_row._manager is None:
        return
    mgr = _auto_add_device_row._manager

    if len(mgr.rows) >= DeviceManager.MAX_DEVICES:
        print(f"[Discovery] Ignored {session.mac} — device limit reached")
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
        _add_log(session.mac, "All chunks sent — verifying CRC on device…", ACCENT)
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
#  DEVICE ROW
# ─────────────────────────────────────────────
class DeviceRow:
    ROW_HEIGHT = 170

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

        # MAC block
        mac_block = tk.Frame(top, bg=CARD_BG)
        mac_block.pack(side=tk.LEFT)

        mac_lbl_row = tk.Frame(mac_block, bg=CARD_BG)
        mac_lbl_row.pack(fill=tk.X)
        tk.Label(mac_lbl_row, text="MAC ADDRESS",
                 font=(FONT_MONO, 7, "bold"),
                 bg=CARD_BG, fg=TEXT_DIM, anchor="w").pack(side=tk.LEFT)
        self.auto_badge = tk.Label(mac_lbl_row, text=" AUTO-DISCOVERED ",
                                   font=(FONT_MONO, 7, "bold"),
                                   bg="#0d2a4a", fg=ACCENT, padx=4, pady=0)

        mac_input_row = tk.Frame(mac_block, bg=CARD_BG)
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
        self.fw_entry = tk.Entry(fw_block,
                 textvariable=self.fw_var,
                 font=(FONT_MONO, 10, "bold"),
                 bg=INPUT_BG, fg=TEXT_DIM,
                 relief=tk.FLAT, bd=0,
                 highlightthickness=1, highlightbackground=INPUT_BD,
                 width=9, state="readonly",
                 readonlybackground=INPUT_BG, cursor="arrow")
        self.fw_entry.pack(ipady=5, ipadx=6)

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
        self.browse_btn = tk.Button(file_row, text="Browse…",
                                    font=(FONT_UI, 8),
                                    bg=MUTED, fg=TEXT_SEC,
                                    activebackground=INPUT_BD,
                                    activeforeground=TEXT_PRI,
                                    relief=tk.FLAT, bd=0,
                                    highlightthickness=1,
                                    highlightbackground=CARD_BORDER,
                                    padx=10, pady=4, cursor="hand2",
                                    command=self.cb_browse)
        self.browse_btn.pack(side=tk.LEFT, padx=(8, 0))

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

        # Force upload warning
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
        if not self.session:
            return
        self.fw_var.set(self.session.fw_version)

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
        self.browse_btn.config(state=tk.DISABLED)
        self.upload_btn.config(state=tk.DISABLED, bg=MUTED,
                               fg=TEXT_DIM, cursor="")

    def _unlock_controls(self):
        is_auto = self.session and getattr(self.session, "auto_discovered", False)
        self.mac_entry.config(state=tk.DISABLED if is_auto else tk.NORMAL)
        self.check_btn.config(state=tk.NORMAL)
        self.browse_btn.config(state=tk.NORMAL)

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
        self._refresh_upload_btn()

        _add_log(mac, "Sending ARE_YOU_READY…", TEXT_SEC)
        mqtt_client.subscribe(f"{mac}/ota_status", qos=1)
        mqtt_client.subscribe(f"{mac}/ota/ack",    qos=1)
        mqtt_client.subscribe(f"{mac}/info",       qos=0)
        mqtt_client.subscribe(f"{mac}/log",        qos=0)
        mqtt_client.publish(f"{mac}/ota_check", "ARE_YOU_READY",
                            qos=COMMAND_QOS)

        session.check_timer_id = root.after(
            CHECK_TIMEOUT_MS, lambda s=session: _on_check_timeout(s))

        # Update log filter menu with new MAC
        if _log_panel:
            root.after(100, _log_panel.refresh_filter_menu)

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
        if not messagebox.askyesno(
                "Force Upload",
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
                "No devices are READY with a firmware file selected.\n\n"
                "Check each device first.")
            return
        if not messagebox.askyesno(
                "Flash All",
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
# ─────────────────────────────────────────────
class LogsPanel:
    """Full-page logs view with MAC filter dropdown and clear button."""

    ALL = "All Devices"

    def __init__(self, parent: tk.Frame):
        self.parent       = parent
        self._filter_mac  = self.ALL   # currently selected filter

        # ── toolbar ──────────────────────────────
        bar = tk.Frame(parent, bg=PANEL_BG, padx=20, pady=12)
        bar.pack(fill=tk.X)

        tk.Label(bar, text="MQTT LOGS",
                 font=(FONT_MONO, 10, "bold"),
                 bg=PANEL_BG, fg=TEXT_PRI).pack(side=tk.LEFT)

        # Clear button
        tk.Button(bar, text="✕  Clear",
                  font=(FONT_UI, 9, "bold"),
                  bg=DANGER_DIM, fg=DANGER,
                  activebackground="#6b1010", activeforeground="white",
                  relief=tk.FLAT, bd=0, highlightthickness=0,
                  padx=12, pady=5, cursor="hand2",
                  command=self._clear).pack(side=tk.RIGHT, padx=(6, 0))

        # Filter dropdown
        tk.Label(bar, text="Filter by MAC:",
                 font=(FONT_UI, 9),
                 bg=PANEL_BG, fg=TEXT_SEC).pack(side=tk.RIGHT, padx=(12, 4))

        self._filter_var = tk.StringVar(value=self.ALL)
        self._filter_var.trace_add("write", self._on_filter_change)
        self._menu_btn = tk.OptionMenu(bar, self._filter_var, self.ALL)
        self._style_menu(self._menu_btn)
        self._menu_btn.pack(side=tk.RIGHT)

        tk.Frame(parent, bg=CARD_BORDER, height=1).pack(fill=tk.X)

        # ── log text area ─────────────────────────
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

        # Tags for colours
        for kw, col in LOG_COLORS.items():
            self.text.tag_config(kw, foreground=col)
        self.text.tag_config("TS",     foreground=TEXT_DIM)
        self.text.tag_config("MAC",    foreground=ACCENT)
        self.text.tag_config("SYSTEM", foreground=WARNING)
        self.text.tag_config("DEFAULT",foreground=TEXT_SEC)

        self._auto_scroll = True

        # Replay existing entries on first show
        with _log_lock:
            entries = list(_log_entries)
        for e in entries:
            self._insert_entry(e, scroll=False)
        self._scroll_bottom()

    def _style_menu(self, btn):
        btn.config(bg=MUTED, fg=TEXT_PRI,
                   activebackground=ACCENT_DIM, activeforeground=TEXT_PRI,
                   relief=tk.FLAT, bd=0, highlightthickness=0,
                   font=(FONT_MONO, 9), padx=10, pady=4,
                   indicatoron=True)
        btn["menu"].config(bg=CARD_BG, fg=TEXT_PRI,
                           activebackground=ACCENT_DIM,
                           activeforeground=TEXT_PRI,
                           relief=tk.FLAT, bd=0)

    def refresh_filter_menu(self):
        """Rebuild the MAC dropdown from current sessions + SYSTEM."""
        menu = self._menu_btn["menu"]
        menu.delete(0, tk.END)
        menu.add_command(label=self.ALL,
                         command=lambda: self._filter_var.set(self.ALL))
        menu.add_command(label="SYSTEM",
                         command=lambda: self._filter_var.set("SYSTEM"))
        for mac in known_macs():
            m = mac  # capture
            menu.add_command(label=m,
                             command=lambda v=m: self._filter_var.set(v))

    def _on_filter_change(self, *_):
        self._filter_mac = self._filter_var.get()
        self._redraw()

    def _redraw(self):
        """Re-render text widget from the full log buffer with current filter."""
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
        """Called from main thread when a new log line arrives."""
        if self._matches(entry):
            self.text.config(state=tk.NORMAL)
            self._insert_entry(entry, scroll=True)
            self.text.config(state=tk.DISABLED)

    def _insert_entry(self, entry: dict, scroll: bool = True):
        ts   = entry["ts"]
        mac  = entry["mac"]
        text = entry["text"]
        col  = entry["color"]

        # Pick tag closest to colour
        tag = "DEFAULT"
        for kw, c in LOG_COLORS.items():
            if col == c:
                tag = kw
                break
        if mac == "SYSTEM":
            tag = "SYSTEM"

        self.text.insert(tk.END, f"[{ts}] ", "TS")
        self.text.insert(tk.END, f"{mac:<18}", "MAC" if mac != "SYSTEM" else "SYSTEM")
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
#  BLANK PANELS
# ─────────────────────────────────────────────
def _blank_panel(parent: tk.Frame, icon: str, title: str, subtitle: str):
    f = tk.Frame(parent, bg=PANEL_BG)
    tk.Label(f, text=icon,
             font=(FONT_MONO, 48),
             bg=PANEL_BG, fg=MUTED).pack(pady=(80, 8))
    tk.Label(f, text=title,
             font=(FONT_UI, 14, "bold"),
             bg=PANEL_BG, fg=TEXT_SEC).pack()
    tk.Label(f, text=subtitle,
             font=(FONT_UI, 10),
             bg=PANEL_BG, fg=TEXT_DIM).pack(pady=(4, 0))
    return f

# ─────────────────────────────────────────────
#  GUI BUILD
# ─────────────────────────────────────────────
device_count_lbl: Optional[tk.Label] = None
sb_dot:  Optional[tk.Label] = None
sb_text: Optional[tk.Label] = None

def _update_device_counter(n):
    if device_count_lbl:
        device_count_lbl.config(text=f"{n} device{'s' if n != 1 else ''}")

def build_gui():
    global device_count_lbl, sb_dot, sb_text, _log_panel

    win = tk.Tk()
    win.title("AceTech ESP32 OTA-FU  ·  Multi-Device")
    win.geometry("900x740")
    win.minsize(720, 520)
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

    # ── SIDEBAR ───────────────────────────────
    sidebar = tk.Frame(body, bg=SIDEBAR_BG, width=170)
    sidebar.pack(side=tk.LEFT, fill=tk.Y)
    sidebar.pack_propagate(False)
    tk.Frame(sidebar, bg=CARD_BORDER, height=1).pack(fill=tk.X)

    # ── MAIN CONTENT AREA (pages stacked) ─────
    content = tk.Frame(body, bg=PANEL_BG)
    content.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

    # ══ PAGE: OTA Flash ══════════════════════
    page_ota = tk.Frame(content, bg=PANEL_BG)

    tk.Frame(page_ota, bg=CARD_BORDER, height=1).pack(fill=tk.X)

    # OTA toolbar
    ota_toolbar = tk.Frame(page_ota, bg=PANEL_BG, padx=20, pady=12)
    ota_toolbar.pack(fill=tk.X)

    device_count_lbl = tk.Label(ota_toolbar, text="0 devices",
                                 font=(FONT_MONO, 9, "bold"),
                                 bg=PANEL_BG, fg=TEXT_SEC)
    device_count_lbl.pack(side=tk.LEFT, padx=(0, 20))

    def tb_btn(parent, text, cmd, bg=MUTED, fg=TEXT_SEC):
        b = tk.Button(parent, text=text,
                      font=(FONT_UI, 9, "bold"),
                      bg=bg, fg=fg,
                      activebackground=ACCENT_HOV, activeforeground="white",
                      relief=tk.FLAT, bd=0, highlightthickness=0,
                      padx=14, pady=6, cursor="hand2", command=cmd)
        b.pack(side=tk.RIGHT, padx=(6, 0))
        return b

    # manager forward-ref
    _manager_ref = [None]

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

    # Scrollable device list
    canvas     = tk.Canvas(page_ota, bg=PANEL_BG, bd=0, highlightthickness=0)
    scrollbar  = ttk.Scrollbar(page_ota, orient="vertical",
                                command=canvas.yview)
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

    # ══ PAGE: Device Info (blank) ═════════════
    page_info = _blank_panel(content, "◈",
                              "Device Info",
                              "Coming soon — live device telemetry & stats")

    # ══ PAGE: Logs ════════════════════════════
    page_logs = tk.Frame(content, bg=PANEL_BG)
    logs_panel = LogsPanel(page_logs)
    _log_panel = logs_panel

    # ══ PAGE: Settings (blank) ════════════════
    page_settings = _blank_panel(content, "⚙",
                                  "Settings",
                                  "Coming soon — broker config, chunk size, retries…")

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
        _active_page[0] = key
        for k, p in pages.items():
            if k == key:
                p.pack(fill=tk.BOTH, expand=True)
            else:
                p.pack_forget()
        # Rebuild nav highlight
        _build_nav(key)
        # Refresh log filter whenever logs page is opened
        if key == "logs":
            logs_panel.refresh_filter_menu()

    # ── SIDEBAR NAV ───────────────────────────
    _nav_frames: list[tk.Frame] = []

    NAV_ITEMS = [
        ("⬡", "OTA Flash",   "ota"),
        ("◈", "Device Info", "info"),
        ("≡", "Logs",        "logs"),
        ("⚙", "Settings",    "settings"),
    ]

    def _build_nav(active_key: str):
        for w in sidebar.winfo_children():
            # Keep the top divider (first child); rebuild the rest
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
            tk.Frame(row_frame,
                     bg=ACCENT if is_active else SIDEBAR_BG,
                     width=3).pack(side=tk.LEFT, fill=tk.Y)
            lbl = tk.Label(row_frame,
                           text=f"  {icon}  {label}",
                           font=(FONT_UI, 9, "bold" if is_active else "normal"),
                           bg=bg, fg=fg, anchor="w", pady=11,
                           cursor="hand2")
            lbl.pack(side=tk.LEFT, fill=tk.X, expand=True)

            k = key  # capture
            row_frame.bind("<Button-1>", lambda e, k=k: show_page(k))
            lbl.bind      ("<Button-1>", lambda e, k=k: show_page(k))

        # Divider + broker info
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
                     wraplength=140).pack(fill=tk.X, padx=14)

    # Initial page
    show_page("ota")

    return win, manager, logs_panel


# ─────────────────────────────────────────────
#  ENTRY POINT
# ─────────────────────────────────────────────
if __name__ == "__main__":
    root, manager, logs_panel = build_gui()
    root.after(200, _connect_mqtt)
    root.mainloop()
    if mqtt_client:
        mqtt_client.loop_stop()
        mqtt_client.disconnect()