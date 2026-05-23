"""
================================================================
 ESP32 OTA Firmware Upgrader — MQTT Only
================================================================
 Dependencies:  pip install paho-mqtt
 Python 3.8+  |  paho-mqtt 2.x

 FIXES in this version:
   - 8-second timeout if ESP32 doesn't respond to Check Device
   - Manual "Force Upload" override button if device is unresponsive
   - File browse BEFORE Check Device now shows a helpful hint
   - Upload button enables correctly when both file + READY received
   - Status resets cleanly on each new Check Device call
================================================================
"""

import os, json, struct, threading, time, zlib
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
import paho.mqtt.client as mqtt

# ─────────────────────────────────────────────
#  CONFIG
# ─────────────────────────────────────────────
APP_VERSION = 0.3
MQTT_BROKER = "broker.emqx.io"
MQTT_PORT   = 1883
CHUNK_SIZE  = 7168   # Larger chunks reduce MQTT round trips and speed up OTA.
ACK_TIMEOUT = 3      # Shorter timeout detects a missed chunk ACK faster.
MAX_RETRIES = 3      # Retries keep QoS 0 chunk transfer reliable.
CHUNK_QOS   = 0      # Chunk data uses custom ACK/retry instead of MQTT QoS 1.
COMMAND_QOS = 1      # Begin, end, and readiness commands stay QoS 1.
BEGIN_DELAY = 0.1    # Small pause lets ESP32 enter Update.begin() before chunks.
CHECK_TIMEOUT_MS = 8000  # ms to wait for ESP32 READY response before showing error

# ─────────────────────────────────────────────
#  DESIGN TOKENS
# ─────────────────────────────────────────────
BG          = "#0b0f1a"
SIDEBAR_BG  = "#0e1420"
PANEL_BG    = "#111827"
CARD_BG     = "#161e2e"
CARD_BORDER = "#1e2d45"
INPUT_BG    = "#0f1829"
INPUT_BD    = "#243352"
INPUT_FOCUS = "#3b82f6"

ACCENT      = "#3b82f6"
ACCENT_HOV  = "#2563eb"
SUCCESS     = "#22c55e"
SUCCESS_HOV = "#16a34a"
WARNING     = "#f59e0b"
DANGER      = "#ef4444"
MUTED       = "#374151"

TEXT_PRI    = "#f1f5f9"
TEXT_SEC    = "#94a3b8"
TEXT_DIM    = "#4b5563"

FONT_MONO   = "Trebuchet MS"
FONT_UI     = "Trebuchet MS" if os.name == "nt" else "TkDefaultFont"

# ─────────────────────────────────────────────
#  STATE
# ─────────────────────────────────────────────
firmware_path   = None
target_mac      = None
mqtt_client     = None
_ack_lock       = threading.Lock()
ack_event       = threading.Event()
current_chunk   = 0
device_ready    = False   # True once ESP32 sends READY
_check_timer_id = None    # after() ID for the Check Device timeout

MQTT_CONN = "connecting"
MQTT_OK   = "connected"
MQTT_DISC = "disconnected"
MQTT_FAIL = "failed"

# ─────────────────────────────────────────────
#  HELPERS — upload button gating
# ─────────────────────────────────────────────
def _refresh_upload_btn():
    """Enable upload button only when file selected AND device is ready."""
    if firmware_path and device_ready:
        upload_btn.config(state=tk.NORMAL, bg=SUCCESS,
                          fg="white", cursor="hand2")
    else:
        upload_btn.config(state=tk.DISABLED, bg=MUTED,
                          fg=TEXT_DIM, cursor="")

# ─────────────────────────────────────────────
#  MQTT CALLBACKS
# ─────────────────────────────────────────────
def _on_connect(client, userdata, flags, rc, props):
    if rc == 0:
        root.after(0, lambda: _mqtt_indicator(MQTT_OK))
    else:
        root.after(0, lambda: _mqtt_indicator(MQTT_FAIL))

def _on_disconnect(client, userdata, flags, rc, props):
    root.after(0, lambda: _mqtt_indicator(MQTT_DISC))

def _on_message(client, userdata, msg):
    global device_ready
    topic   = msg.topic
    payload = msg.payload.decode(errors="replace")

    if topic.endswith("/info"):
        try:
            d = json.loads(payload)
            v = d.get("firmware", "?")
            root.after(0, lambda v=v: _show_fw_version(v))
        except Exception:
            pass
        return

    if topic.endswith("/ota/ack"):
        try:
            idx = int(payload)
            with _ack_lock:
                if idx == current_chunk:
                    ack_event.set()
        except ValueError:
            pass
        return

    MAP = {
        "READY"    : ("READY FOR OTA",           SUCCESS),
        "UPDATING" : ("FLASHING…",               WARNING),
        "FAILED"   : ("OTA FAILED",              DANGER),
        "NO_UPDATE": ("NO UPDATE AVAILABLE",     TEXT_SEC),
        "SUCCESS"  : ("SUCCESS  ✓  REBOOTING",   SUCCESS),
    }
    if payload in MAP:
        txt, col = MAP[payload]
        root.after(0, lambda t=txt, c=col: _set_device_status(t, c))

        if payload == "READY":
            _cancel_check_timeout()
            device_ready = True
            root.after(0, _refresh_upload_btn)

        if payload in ("SUCCESS", "FAILED", "NO_UPDATE"):
            device_ready = False
            root.after(0, lambda: _lock_controls(False))
            root.after(0, _refresh_upload_btn)

# ─────────────────────────────────────────────
#  CHECK DEVICE TIMEOUT
# ─────────────────────────────────────────────
def _cancel_check_timeout():
    global _check_timer_id
    if _check_timer_id:
        root.after_cancel(_check_timer_id)
        _check_timer_id = None

def _on_check_timeout():
    """Called if ESP32 never replies READY within CHECK_TIMEOUT_MS."""
    global _check_timer_id, device_ready
    _check_timer_id = None
    if dev_status_lbl.cget("text") == "Querying device…":
        device_ready = False
        _set_device_status(
            "No response — check MAC / device power", DANGER)
        _show_force_upload_hint()

def _show_force_upload_hint():
    """Show the Force Upload button and a hint label after timeout."""
    hint_lbl.config(
        text="⚠  Device didn't respond. If you're sure it's ready, use Force Upload.",
        fg=WARNING)
    force_btn.pack(fill=tk.X, pady=(6, 0))

def _hide_force_upload_hint():
    hint_lbl.config(text="")
    force_btn.pack_forget()

# ─────────────────────────────────────────────
#  MQTT CONNECT
# ─────────────────────────────────────────────
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
            root.after(0, lambda: _mqtt_indicator(MQTT_FAIL))
    threading.Thread(target=_worker, daemon=True).start()

# ─────────────────────────────────────────────
#  MQTT PULSE ANIMATION
# ─────────────────────────────────────────────
_pulse_id  = None
_pulse_a   = 0.0
_pulse_dir = 1

def _mqtt_indicator(state):
    global _pulse_id
    if _pulse_id:
        root.after_cancel(_pulse_id)
        _pulse_id = None

    cfg = {
        MQTT_CONN : (WARNING,  "Connecting…",                         True),
        MQTT_OK   : (SUCCESS,  f"MQTT  ·  {MQTT_BROKER}:{MQTT_PORT}", False),
        MQTT_DISC : (DANGER,   "Disconnected — retrying",             True),
        MQTT_FAIL : (DANGER,   f"Cannot reach {MQTT_BROKER}",         False),
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
    if _pulse_a >= 1:   _pulse_a = 1;   _pulse_dir = -1
    elif _pulse_a <= 0: _pulse_a = 0;   _pulse_dir =  1
    dim = _darken(col, 0.35)
    sb_dot.config(fg=_lerp_hex(dim, col, _pulse_a))
    _pulse_id = root.after(35, lambda: _tick_pulse(col))

def _darken(hex_col, amount):
    r, g, b = (int(hex_col[i:i+2], 16) for i in (1, 3, 5))
    return f"#{int(r*amount):02x}{int(g*amount):02x}{int(b*amount):02x}"

def _lerp_hex(c1, c2, t):
    r1, g1, b1 = (int(c1[i:i+2], 16) for i in (1, 3, 5))
    r2, g2, b2 = (int(c2[i:i+2], 16) for i in (1, 3, 5))
    return (f"#{int(r1+(r2-r1)*t):02x}"
            f"{int(g1+(g2-g1)*t):02x}"
            f"{int(b1+(b2-b1)*t):02x}")

# ─────────────────────────────────────────────
#  UI STATE HELPERS
# ─────────────────────────────────────────────
def _set_device_status(text, colour):
    dev_status_dot.config(fg=colour)
    dev_status_lbl.config(text=text, fg=colour)

def _show_fw_version(version):
    fw_var.set(f"v{version}")
    fw_entry.config(fg=SUCCESS)

def _lock_controls(locked):
    s = tk.DISABLED if locked else tk.NORMAL
    mac_entry.config(state=s)
    check_btn.config(state=s)
    file_btn.config(state=s)

def _update_progress(pct, done, total):
    progress_bar["value"] = pct
    prog_left.config(text=f"Chunk  {done} / {total}")
    prog_right.config(text=f"{pct}%")
    prog_bar_lbl.config(
        text="▓" * (pct // 5) + "░" * (20 - pct // 5))

def _reset_progress():
    progress_bar["value"] = 0
    prog_left.config(text="")
    prog_right.config(text="")
    prog_bar_lbl.config(text="")

# ─────────────────────────────────────────────
#  BUTTON CALLBACKS
# ─────────────────────────────────────────────
def cb_check():
    global target_mac, device_ready, _check_timer_id
    if not mqtt_client:
        messagebox.showerror("Not Connected", "MQTT not ready — please wait.")
        return
    mac = mac_entry.get().strip().upper()
    if not mac:
        messagebox.showerror("Missing MAC", "Enter the ESP32 MAC address.")
        return

    # Reset state
    target_mac   = mac
    device_ready = False
    _cancel_check_timeout()
    _hide_force_upload_hint()

    fw_var.set("—")
    fw_entry.config(fg=TEXT_DIM)
    upload_btn.config(state=tk.DISABLED, bg=MUTED,
                      fg=TEXT_DIM, cursor="")
    _set_device_status("Querying device…", TEXT_SEC)
    _reset_progress()

    mqtt_client.subscribe(f"{target_mac}/ota_status", qos=1)
    mqtt_client.subscribe(f"{target_mac}/ota/ack",    qos=1)
    mqtt_client.subscribe(f"{target_mac}/info",       qos=0)
    mqtt_client.publish(f"{target_mac}/ota_check",
                        "ARE_YOU_READY", qos=COMMAND_QOS)

    # Start timeout — if no READY in CHECK_TIMEOUT_MS, show error
    _check_timer_id = root.after(CHECK_TIMEOUT_MS, _on_check_timeout)

def cb_browse():
    global firmware_path
    p = filedialog.askopenfilename(
        title="Select firmware binary",
        filetypes=[("Firmware binary", "*.bin"), ("All files", "*.*")],
    )
    if p:
        firmware_path = p
        file_name_lbl.config(text=os.path.basename(p), fg=TEXT_PRI)
        size_kb = os.path.getsize(p) / 1024
        file_meta_lbl.config(text=f"{size_kb:.1f} KB", fg=TEXT_SEC)
        _refresh_upload_btn()

        # Hint if user picked a file but hasn't checked device yet
        if not device_ready and not target_mac:
            _set_device_status(
                "File selected — run Check Device before uploading",
                WARNING)

def cb_upload():
    if not target_mac:
        messagebox.showerror("No Device", "Run 'Check Device' first.")
        return
    if not firmware_path:
        messagebox.showerror("No File", "Select a firmware file first.")
        return
    _lock_controls(True)
    upload_btn.config(state=tk.DISABLED, bg=MUTED,
                      fg=TEXT_DIM, cursor="")
    _reset_progress()
    threading.Thread(target=_ota_worker, daemon=True).start()

def cb_force_upload():
    """Bypass READY handshake — use when ESP32 is known ready but silent."""
    global device_ready
    if not target_mac:
        messagebox.showerror("No Device",
            "Enter a MAC and click Check Device first\n"
            "(even if it timed out).")
        return
    if not firmware_path:
        messagebox.showerror("No File", "Select a firmware file first.")
        return
    if not messagebox.askyesno(
            "Force Upload",
            "Skip the READY handshake and upload immediately?\n\n"
            "Only do this if you are certain the device is\n"
            "waiting for OTA data."):
        return
    device_ready = True
    _hide_force_upload_hint()
    _set_device_status("Force upload — transferring…", WARNING)
    _lock_controls(True)
    upload_btn.config(state=tk.DISABLED, bg=MUTED,
                      fg=TEXT_DIM, cursor="")
    _reset_progress()
    threading.Thread(target=_ota_worker, daemon=True).start()

# ─────────────────────────────────────────────
#  OTA WORKER
# ─────────────────────────────────────────────
def _ota_worker():
    global current_chunk
    try:
        with open(firmware_path, "rb") as f:
            data = f.read()
        total  = len(data)
        chunks = (total + CHUNK_SIZE - 1) // CHUNK_SIZE
        crc    = zlib.crc32(data) & 0xFFFFFFFF

        mqtt_client.publish(
            f"{target_mac}/ota/begin",
            json.dumps({"size": total, "chunks": chunks, "crc32": crc}),
            qos=COMMAND_QOS)
        root.after(0, lambda: _set_device_status(
            "Transferring firmware…", ACCENT))
        time.sleep(BEGIN_DELAY)

        for i in range(chunks):
            pkt = struct.pack(">I", i) + data[i*CHUNK_SIZE:(i+1)*CHUNK_SIZE]
            ok  = False
            for attempt in range(MAX_RETRIES):
                with _ack_lock:
                    current_chunk = i
                    ack_event.clear()
                mqtt_client.publish(
                    f"{target_mac}/ota/chunk", pkt, qos=CHUNK_QOS)
                if ack_event.wait(ACK_TIMEOUT):
                    ok = True
                    break
                print(f"[OTA] chunk {i} retry {attempt+1}")
            if not ok:
                root.after(0, lambda ci=i: _set_device_status(
                    f"FAILED — no ACK on chunk {ci}", DANGER))
                root.after(0, lambda: _lock_controls(False))
                return
            pct = int((i + 1) / chunks * 100)
            root.after(0, lambda p=pct, ci=i+1, ct=chunks:
                _update_progress(p, ci, ct))

        mqtt_client.publish(
            f"{target_mac}/ota/end", "END", qos=COMMAND_QOS)
        root.after(0, lambda: _set_device_status(
            "Verifying — waiting for ESP32…", SUCCESS))

    except FileNotFoundError:
        root.after(0, lambda: _set_device_status("File not found", DANGER))
        root.after(0, lambda: _lock_controls(False))
    except Exception as e:
        root.after(0, lambda e=str(e): _set_device_status(
            f"Error: {e}", DANGER))
        root.after(0, lambda: _lock_controls(False))

# ─────────────────────────────────────────────
#  WIDGET FACTORIES
# ─────────────────────────────────────────────
def section_header(parent, text):
    row = tk.Frame(parent, bg=PANEL_BG)
    row.pack(fill=tk.X, pady=(0, 10))
    tk.Frame(row, bg=ACCENT, width=3).pack(side=tk.LEFT, fill=tk.Y)
    tk.Label(row, text=f"  {text}",
             font=(FONT_MONO, 8, "bold"),
             bg=PANEL_BG, fg=TEXT_SEC,
             anchor="w").pack(side=tk.LEFT, fill=tk.Y, pady=6)

def field_label(parent, text, bg=None):
    bg = bg or PANEL_BG
    tk.Label(parent, text=text,
             font=(FONT_UI, 9),
             bg=bg, fg=TEXT_SEC, anchor="w").pack(fill=tk.X, pady=(0, 4))

def styled_entry(parent, width=28, **kw):
    return tk.Entry(parent,
                    font=(FONT_MONO, 11),
                    bg=INPUT_BG, fg=TEXT_PRI,
                    insertbackground=TEXT_PRI,
                    relief=tk.FLAT, bd=0,
                    highlightthickness=1,
                    highlightbackground=INPUT_BD,
                    highlightcolor=INPUT_FOCUS,
                    width=width, **kw)

def readonly_field(parent, textvariable, width=18):
    return tk.Entry(parent,
                    textvariable=textvariable,
                    font=(FONT_MONO, 11, "bold"),
                    bg=INPUT_BG, fg=TEXT_DIM,
                    relief=tk.FLAT, bd=0,
                    highlightthickness=1,
                    highlightbackground=INPUT_BD,
                    width=width,
                    state="readonly",
                    readonlybackground=INPUT_BG,
                    cursor="arrow")

def primary_btn(parent, text, command, bg=ACCENT, fg="white", **kw):
    b = tk.Button(parent, text=text,
                  font=(FONT_UI, 10, "bold"),
                  bg=bg, fg=fg,
                  activebackground=ACCENT_HOV, activeforeground="white",
                  relief=tk.FLAT, bd=0,
                  highlightthickness=0,
                  padx=20, pady=8,
                  cursor="hand2",
                  command=command, **kw)
    b.bind("<Enter>", lambda e, b=b, hov=ACCENT_HOV if bg == ACCENT else SUCCESS_HOV:
           b.config(bg=hov) if b["state"] != tk.DISABLED else None)
    b.bind("<Leave>", lambda e, b=b, orig=bg:
           b.config(bg=orig) if b["state"] != tk.DISABLED else None)
    return b

def ghost_btn(parent, text, command):
    b = tk.Button(parent, text=text,
                  font=(FONT_UI, 10),
                  bg=CARD_BG, fg=TEXT_SEC,
                  activebackground=INPUT_BD, activeforeground=TEXT_PRI,
                  relief=tk.FLAT, bd=0,
                  highlightthickness=1,
                  highlightbackground=CARD_BORDER,
                  padx=16, pady=8,
                  cursor="hand2",
                  command=command)
    b.bind("<Enter>", lambda e: b.config(fg=TEXT_PRI))
    b.bind("<Leave>", lambda e: b.config(fg=TEXT_SEC))
    return b

def divider(parent, bg=PANEL_BG):
    tk.Frame(parent, bg=CARD_BORDER, height=1).pack(fill=tk.X, pady=14)

def info_row(parent, label, value_var):
    row = tk.Frame(parent, bg=PANEL_BG)
    row.pack(fill=tk.X, pady=4)
    tk.Label(row, text=label,
             font=(FONT_UI, 9),
             bg=PANEL_BG, fg=TEXT_SEC,
             width=20, anchor="w").pack(side=tk.LEFT)
    e = readonly_field(row, value_var, width=20)
    e.pack(side=tk.LEFT, ipady=5, ipadx=8)
    return e

# ─────────────────────────────────────────────
#  GUI BUILD
# ─────────────────────────────────────────────
def build_gui():
    win = tk.Tk()
    win.title("AceTech ESP32 OTA-FU")
    win.geometry("700x680")
    win.resizable(False, False)
    win.configure(bg=BG)

    # ── TITLEBAR ──────────────────────────────
    tb = tk.Frame(win, bg=BG, height=56)
    tb.pack(fill=tk.X)
    tb.pack_propagate(False)

    logo = tk.Frame(tb, bg=ACCENT, width=56, height=56)
    logo.pack(side=tk.LEFT)
    logo.pack_propagate(False)
    tk.Label(logo, text="⬡", font=(FONT_MONO, 20, "bold"),
             bg=ACCENT, fg="white").place(relx=0.5, rely=0.5, anchor="center")

    tk.Label(tb, text="AceTech ESP32 OTA-FU",
             font=(FONT_UI, 13, "bold"),
             bg=BG, fg=TEXT_PRI, anchor="w").pack(side=tk.LEFT, padx=14)

    tk.Label(tb, text=f"MQTT ONLY v{APP_VERSION}",
             font=(FONT_MONO, 8, "bold"),
             bg=BG, fg=TEXT_DIM, anchor="e").pack(side=tk.RIGHT, padx=18)

    # ── STATUSBAR ─────────────────────────────
    statusbar = tk.Frame(win, bg=SIDEBAR_BG, height=26)
    statusbar.pack(side=tk.BOTTOM, fill=tk.X)
    statusbar.pack_propagate(False)

    global sb_dot, sb_text
    sb_dot = tk.Label(statusbar, text="●",
                      font=(FONT_MONO, 11),
                      bg=SIDEBAR_BG, fg=TEXT_DIM, padx=10)
    sb_dot.pack(side=tk.LEFT)

    sb_text = tk.Label(statusbar, text="Initialising…",
                       font=(FONT_MONO, 9),
                       bg=SIDEBAR_BG, fg=TEXT_DIM, anchor="w")
    sb_text.pack(side=tk.LEFT)

    tk.Label(statusbar,
             text=(f"chunk {CHUNK_SIZE} B  ·  "
                   f"ack {ACK_TIMEOUT} s  ·  "
                   f"retries {MAX_RETRIES}  ·  "
                   f"check timeout {CHECK_TIMEOUT_MS//1000} s"),
             font=(FONT_MONO, 8),
             bg=SIDEBAR_BG, fg=TEXT_DIM, anchor="e").pack(
                 side=tk.RIGHT, padx=12)

    # ── BODY ──────────────────────────────────
    body = tk.Frame(win, bg=BG)
    body.pack(fill=tk.BOTH, expand=True)

    # Sidebar
    sidebar = tk.Frame(body, bg=SIDEBAR_BG, width=180)
    sidebar.pack(side=tk.LEFT, fill=tk.Y)
    sidebar.pack_propagate(False)
    tk.Frame(sidebar, bg=CARD_BORDER, height=1).pack(fill=tk.X)

    def nav_item(icon, label, active=False):
        bg  = ACCENT if active else SIDEBAR_BG
        fg  = "white" if active else TEXT_SEC
        row = tk.Frame(sidebar, bg=bg, cursor="hand2")
        row.pack(fill=tk.X)
        tk.Frame(row, bg=ACCENT if active else SIDEBAR_BG,
                 width=3).pack(side=tk.LEFT, fill=tk.Y)
        tk.Label(row, text=f"  {icon}  {label}",
                 font=(FONT_UI, 10, "bold" if active else "normal"),
                 bg=bg, fg=fg, anchor="w",
                 pady=12).pack(side=tk.LEFT, fill=tk.X, expand=True)

    nav_item("⬡", "OTA Flash",   active=True)
    nav_item("◈", "Device Info")
    nav_item("≡", "Logs")
    nav_item("⚙", "Settings")

    tk.Frame(sidebar, bg=CARD_BORDER, height=1).pack(fill=tk.X, pady=(12, 0))

    tk.Label(sidebar, text="BROKER",
             font=(FONT_MONO, 7, "bold"),
             bg=SIDEBAR_BG, fg=TEXT_DIM, anchor="w").pack(
                 fill=tk.X, padx=14, pady=(12, 2))
    tk.Label(sidebar, text=MQTT_BROKER,
             font=(FONT_MONO, 8),
             bg=SIDEBAR_BG, fg=TEXT_SEC, anchor="w",
             wraplength=150).pack(fill=tk.X, padx=14)

    tk.Label(sidebar, text="PORT",
             font=(FONT_MONO, 7, "bold"),
             bg=SIDEBAR_BG, fg=TEXT_DIM, anchor="w").pack(
                 fill=tk.X, padx=14, pady=(10, 2))
    tk.Label(sidebar, text=str(MQTT_PORT),
             font=(FONT_MONO, 8),
             bg=SIDEBAR_BG, fg=TEXT_SEC, anchor="w").pack(fill=tk.X, padx=14)

    # Main panel
    main = tk.Frame(body, bg=PANEL_BG)
    main.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
    tk.Frame(main, bg=CARD_BORDER, height=1).pack(fill=tk.X)

    scroll_canvas = tk.Canvas(main, bg=PANEL_BG, bd=0, highlightthickness=0)
    scroll_canvas.pack(fill=tk.BOTH, expand=True)

    content = tk.Frame(scroll_canvas, bg=PANEL_BG)
    scroll_canvas.create_window((0, 0), window=content, anchor="nw")

    pad = tk.Frame(content, bg=PANEL_BG)
    pad.pack(fill=tk.BOTH, expand=True, padx=28, pady=22)

    # ══ SECTION 1 — DEVICE ════════════════════
    section_header(pad, "DEVICE")

    field_label(pad, "MAC Address")
    mac_row = tk.Frame(pad, bg=PANEL_BG)
    mac_row.pack(fill=tk.X, pady=(0, 16))

    global mac_entry, check_btn
    mac_entry = styled_entry(mac_row, width=22)
    mac_entry.pack(side=tk.LEFT, ipady=7, ipadx=6, padx=(0, 10))
    mac_entry.insert(0, "0CF73625BF58")

    check_btn = primary_btn(mac_row, "  Check Device  ", cb_check)
    check_btn.pack(side=tk.LEFT)

    # Firmware version read-only field
    info_grid = tk.Frame(pad, bg=PANEL_BG)
    info_grid.pack(fill=tk.X, pady=(0, 4))

    global fw_var, fw_entry
    fw_var   = tk.StringVar(value="—")
    fw_entry = info_row(info_grid, "Firmware Version", fw_var)

    # Device status badge
    status_row = tk.Frame(pad, bg=PANEL_BG)
    status_row.pack(fill=tk.X, pady=(8, 0))

    global dev_status_dot, dev_status_lbl
    dev_status_dot = tk.Label(status_row, text="●",
                              font=(FONT_MONO, 12),
                              bg=PANEL_BG, fg=TEXT_DIM)
    dev_status_dot.pack(side=tk.LEFT)

    dev_status_lbl = tk.Label(status_row, text="Not checked",
                              font=(FONT_UI, 10, "bold"),
                              bg=PANEL_BG, fg=TEXT_DIM, anchor="w")
    dev_status_lbl.pack(side=tk.LEFT, padx=8)

    divider(pad)

    # ══ SECTION 2 — FIRMWARE FILE ═════════════
    section_header(pad, "FIRMWARE FILE")

    file_card = tk.Frame(pad, bg=CARD_BG,
                         highlightthickness=1,
                         highlightbackground=CARD_BORDER)
    file_card.pack(fill=tk.X, pady=(0, 16))

    file_inner = tk.Frame(file_card, bg=CARD_BG, padx=16, pady=12)
    file_inner.pack(fill=tk.X)

    file_top = tk.Frame(file_inner, bg=CARD_BG)
    file_top.pack(fill=tk.X)

    tk.Label(file_top, text="📦",
             font=(FONT_UI, 20),
             bg=CARD_BG, fg=TEXT_SEC).pack(side=tk.LEFT, padx=(0, 12))

    file_text = tk.Frame(file_top, bg=CARD_BG)
    file_text.pack(side=tk.LEFT, fill=tk.X, expand=True)

    global file_name_lbl, file_meta_lbl
    file_name_lbl = tk.Label(file_text, text="No file selected",
                             font=(FONT_UI, 10, "bold"),
                             bg=CARD_BG, fg=TEXT_DIM, anchor="w")
    file_name_lbl.pack(fill=tk.X)

    file_meta_lbl = tk.Label(file_text,
                             text="Choose a .bin firmware binary",
                             font=(FONT_UI, 9),
                             bg=CARD_BG, fg=TEXT_DIM, anchor="w")
    file_meta_lbl.pack(fill=tk.X)

    global file_btn
    file_btn = ghost_btn(file_top, "  Browse…  ", cb_browse)
    file_btn.pack(side=tk.RIGHT)

    divider(pad)

    # ══ SECTION 3 — TRANSFER ══════════════════
    section_header(pad, "TRANSFER")

    # Progress header
    prog_header = tk.Frame(pad, bg=PANEL_BG)
    prog_header.pack(fill=tk.X, pady=(0, 6))

    global prog_left, prog_right
    prog_left  = tk.Label(prog_header, text="",
                          font=(FONT_MONO, 9),
                          bg=PANEL_BG, fg=TEXT_SEC, anchor="w")
    prog_left.pack(side=tk.LEFT)

    prog_right = tk.Label(prog_header, text="",
                          font=(FONT_MONO, 9, "bold"),
                          bg=PANEL_BG, fg=TEXT_PRI, anchor="e")
    prog_right.pack(side=tk.RIGHT)

    style = ttk.Style()
    style.theme_use("default")
    style.configure("OTA.Horizontal.TProgressbar",
                    troughcolor=INPUT_BG,
                    background=ACCENT,
                    thickness=6, borderwidth=0)
    global progress_bar
    progress_bar = ttk.Progressbar(pad, orient="horizontal",
                                   mode="determinate",
                                   style="OTA.Horizontal.TProgressbar")
    progress_bar.pack(fill=tk.X, pady=(0, 6))

    global prog_bar_lbl
    prog_bar_lbl = tk.Label(pad, text="",
                            font=(FONT_MONO, 8),
                            bg=PANEL_BG, fg=TEXT_DIM, anchor="w")
    prog_bar_lbl.pack(fill=tk.X, pady=(0, 16))

    # Upload button — full width
    global upload_btn
    upload_btn = tk.Button(pad,
                           text="⬆    Upload Firmware via MQTT",
                           font=(FONT_UI, 11, "bold"),
                           bg=MUTED, fg=TEXT_DIM,
                           activebackground=SUCCESS_HOV,
                           activeforeground="white",
                           relief=tk.FLAT, bd=0,
                           highlightthickness=0,
                           pady=13, cursor="",
                           state=tk.DISABLED,
                           command=cb_upload)
    upload_btn.pack(fill=tk.X)

    # ── Timeout hint + Force Upload (hidden until needed) ──
    global hint_lbl, force_btn
    hint_lbl = tk.Label(pad, text="",
                        font=(FONT_UI, 9),
                        bg=PANEL_BG, fg=WARNING,
                        anchor="w", wraplength=460,
                        justify=tk.LEFT)
    hint_lbl.pack(fill=tk.X, pady=(8, 0))

    force_btn = tk.Button(pad,
                          text="⚡  Force Upload (skip READY handshake)",
                          font=(FONT_UI, 10, "bold"),
                          bg="#7c3a00", fg="#fbbf24",
                          activebackground="#92400e",
                          activeforeground="white",
                          relief=tk.FLAT, bd=0,
                          highlightthickness=1,
                          highlightbackground="#92400e",
                          pady=10, cursor="hand2",
                          command=cb_force_upload)
    # Not packed initially — shown only after timeout

    return win

# ─────────────────────────────────────────────
#  ENTRY POINT
# ─────────────────────────────────────────────
if __name__ == "__main__":
    root = build_gui()
    root.after(200, _connect_mqtt)
    root.mainloop()
    if mqtt_client:
        mqtt_client.loop_stop()
        mqtt_client.disconnect()