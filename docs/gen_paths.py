#!/usr/bin/env python
"""Render the five Eco Pulse data-flow paths as styled vertical-pipeline PNGs."""
import os, subprocess, tempfile, struct, shutil

CW = 720          # canvas width
CX = 360          # centre x
CARD_W = 540
PAD = 38          # arrow gap

CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"
if not os.path.exists(CHROME):
    CHROME = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"

def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

def chips_layout(items, x0, max_x):
    """Return list of (x, y_off, w, text) and total rows height."""
    rows, cur, cx = [], [], x0
    widths = []
    for it in items:
        w = int(len(it) * 6.7 + 26)
        if cx + w > max_x and cur:
            rows.append(cur); cur = []; cx = x0
        cur.append((cx, w, it)); cx += w + 10
    if cur:
        rows.append(cur)
    return rows

DIAGRAMS = [
    dict(file="path1_sensor_to_ac", title="1 · Sensor Presence → AC Control",
         accent="#2563eb", source=("LD2412 radar", "UART"), sink=("IR  →  AC UNIT", "#059669", "irMutex"),
         steps=[
            dict(text="SensorManager::poll()"),
            dict(text="debounce + flap-delay suppression"),
            dict(text="cachedPresence (atomic)"),
            dict(text="EVENT_PRESENCE_CHANGED  →  automationQueue"),
            dict(text="AutomationTask", sub="ON-NORMAL · arm eco + off timers"),
            dict(text="executeACCommand()", sub="single AC choke point"),
         ]),
    dict(file="path2_command", title="2 · Cloud Command → Device Action",
         accent="#0891b2", source=("Broker", "MQTT"), sink=("persist to NVS  +  publishACK()", "#0e7490", ""),
         steps=[
            dict(text="WiFiManager / GSMManager  RX"),
            dict(text="ArduinoJson deserialize"),
            dict(text="CommandProcessor::processJSON()",
                 chips=["temperature_control", "power_control", "radar_control",
                        "eco / teco / toff", "ota_update", "dynamic IR (protocol/state/code)"]),
            dict(text="Dispatch to managers",
                 chips=["AutomationManager", "ScheduleManager", "IRManager", "OTAManager"]),
         ]),
    dict(file="path3_telemetry", title="3 · Telemetry / Events → Cloud",
         accent="#0d9488", source=("Any task", ""), sink=("Broker", "#0d9488", ""),
         steps=[
            dict(text="NetworkManager publish API",
                 chips=["publishACK", "sendAutomationEvent", "publishHealthAlert"]),
            dict(text="Active transport", sub="Wi-Fi async MQTT   /   GSM  AT QMTPUB"),
         ],
         footnote="WiFiManager / GSMManager loop() also publishes periodic telemetry (~10 s)."),
    dict(file="path4_schedule", title="4 · Clock → Schedule → AC",
         accent="#7c3aed", source=("NTP (Wi-Fi)  /  cell RTC (GSM)", ""), sink=("IR  (only if state changes)", "#059669", ""),
         steps=[
            dict(text="system clock"),
            dict(text="SchedTask", sub="1 s poll · acts once per minute"),
            dict(text="findSegment() in NVS"),
            dict(text="apply  temp / radar / eco / off"),
         ]),
    dict(file="path5_web_maintenance", title="5 · Web / AP → Radar Maintenance",
         accent="#d97706", source=("SoftAP dashboard", ""), sink=("dashboard polls  /devdata", "#0e7490", ""),
         steps=[
            dict(text="WebDashboard route"),
            dict(text="SensorManager::requestX()"),
            dict(text="xTaskNotify(sensorsTaskHandle, bit)"),
            dict(text="SensorsTask applies over UART"),
            dict(text="result mirrored back into sysData"),
         ]),
]

DEFS = '''  <defs>
    <filter id="sh" x="-30%" y="-30%" width="160%" height="160%">
      <feDropShadow dx="0" dy="2" stdDeviation="4" flood-color="#0f172a" flood-opacity="0.16"/>
    </filter>
    <marker id="arr" markerWidth="13" markerHeight="13" refX="9" refY="5" orient="auto" markerUnits="userSpaceOnUse">
      <path d="M0,0 L10,5 L0,10 z" fill="#94a3b8"/>
    </marker>
  </defs>'''

def build(d):
    o = []
    accent = d["accent"]
    y = 96  # below title
    body = []

    def arrow(y1, y2, label=""):
        body.append(f'  <line x1="{CX}" y1="{y1}" x2="{CX}" y2="{y2-4}" stroke="#cbd5e1" '
                    f'stroke-width="2.6" marker-end="url(#arr)"/>')
        if label:
            w = int(len(label) * 6.8 + 16)
            body.append(f'  <rect x="{CX+10}" y="{(y1+y2)/2-9:.0f}" width="{w}" height="17" rx="4" fill="#ffffff"/>')
            body.append(f'  <text x="{CX+10+w/2:.0f}" y="{(y1+y2)/2+3:.0f}" text-anchor="middle" '
                        f'font-size="11" font-weight="700" fill="{accent}">{esc(label)}</text>')

    # source pill
    txt = d["source"][0]
    pw = max(220, int(len(txt) * 9.2 + 50))
    h = 52
    body.append(f'  <rect x="{CX-pw/2:.0f}" y="{y}" width="{pw}" height="{h}" rx="26" fill="{accent}" filter="url(#sh)"/>')
    body.append(f'  <text x="{CX}" y="{y+33}" text-anchor="middle" font-size="16" font-weight="700" fill="#fff">{esc(txt)}</text>')
    prev_bottom = y + h
    n = 0
    for st in d["steps"]:
        n += 1
        edge = d["source"][1] if n == 1 else ""
        top = prev_bottom + PAD
        arrow(prev_bottom, top, edge)
        x = CX - CARD_W / 2
        chips = st.get("chips")
        sub = st.get("sub")
        if chips:
            rows = chips_layout(chips, x + 22, x + CARD_W - 22)
            ch = 38 + len(rows) * 40 + 12
            body.append(f'  <rect x="{x:.0f}" y="{top}" width="{CARD_W}" height="{ch}" rx="13" fill="#fff" filter="url(#sh)"/>')
            body.append(f'  <clipPath id="cl{n}{d["file"][5]}"><rect x="{x:.0f}" y="{top}" width="{CARD_W}" height="{ch}" rx="13"/></clipPath>')
            body.append(f'  <g clip-path="url(#cl{n}{d["file"][5]})"><rect x="{x:.0f}" y="{top}" width="{CARD_W}" height="6" fill="{accent}"/></g>')
            body.append(f'  <rect x="{x:.0f}" y="{top}" width="{CARD_W}" height="{ch}" rx="13" fill="none" stroke="#e2e8f0"/>')
            body.append(f'  <circle cx="{x:.0f}" cy="{top+24}" r="15" fill="{accent}"/>')
            body.append(f'  <text x="{x:.0f}" y="{top+29}" text-anchor="middle" font-size="13" font-weight="700" fill="#fff">{n}</text>')
            body.append(f'  <text x="{x+30:.0f}" y="{top+29}" font-size="14.5" font-weight="700" fill="{accent}">{esc(st["text"])}</text>')
            cy = top + 56
            for row in rows:
                for cx_, w_, it in row:
                    body.append(f'  <rect x="{cx_:.0f}" y="{cy}" width="{w_}" height="30" rx="8" fill="#f8fafc" stroke="{accent}" stroke-opacity="0.5"/>')
                    body.append(f'  <text x="{cx_+w_/2:.0f}" y="{cy+20}" text-anchor="middle" font-size="12" font-weight="600" fill="#334155">{esc(it)}</text>')
                cy += 40
            prev_bottom = top + ch
        else:
            ch = 66 if sub else 58
            body.append(f'  <rect x="{x:.0f}" y="{top}" width="{CARD_W}" height="{ch}" rx="13" fill="#fff" filter="url(#sh)"/>')
            body.append(f'  <clipPath id="cl{n}{d["file"][5]}"><rect x="{x:.0f}" y="{top}" width="{CARD_W}" height="{ch}" rx="13"/></clipPath>')
            body.append(f'  <g clip-path="url(#cl{n}{d["file"][5]})"><rect x="{x:.0f}" y="{top}" width="{CARD_W}" height="6" fill="{accent}"/></g>')
            body.append(f'  <rect x="{x:.0f}" y="{top}" width="{CARD_W}" height="{ch}" rx="13" fill="none" stroke="#e2e8f0"/>')
            cyc = top + ch / 2 + 4
            body.append(f'  <circle cx="{x:.0f}" cy="{top+ch/2:.0f}" r="15" fill="{accent}"/>')
            body.append(f'  <text x="{x:.0f}" y="{top+ch/2+5:.0f}" text-anchor="middle" font-size="13" font-weight="700" fill="#fff">{n}</text>')
            if sub:
                body.append(f'  <text x="{CX}" y="{top+28:.0f}" text-anchor="middle" font-size="14.5" font-weight="700" fill="#0f172a">{esc(st["text"])}</text>')
                body.append(f'  <text x="{CX}" y="{top+48:.0f}" text-anchor="middle" font-size="12" fill="#64748b">{esc(sub)}</text>')
            else:
                body.append(f'  <text x="{CX}" y="{cyc:.0f}" text-anchor="middle" font-size="14.5" font-weight="700" fill="#0f172a">{esc(st["text"])}</text>')
            prev_bottom = top + ch

    # sink pill
    stext, sfill, slabel = d["sink"]
    top = prev_bottom + PAD
    arrow(prev_bottom, top, slabel)
    pw = max(240, int(len(stext) * 9.2 + 50))
    h = 54
    body.append(f'  <rect x="{CX-pw/2:.0f}" y="{top}" width="{pw}" height="{h}" rx="27" fill="{sfill}" filter="url(#sh)"/>')
    body.append(f'  <text x="{CX}" y="{top+34}" text-anchor="middle" font-size="16" font-weight="700" fill="#fff">{esc(stext)}</text>')
    bottom = top + h

    fn = d.get("footnote")
    if fn:
        bottom += 24
        body.append(f'  <rect x="60" y="{bottom}" width="{CW-120}" height="40" rx="10" fill="#f1f5f9" stroke="#cbd5e1"/>')
        body.append(f'  <text x="{CX}" y="{bottom+25}" text-anchor="middle" font-size="12" font-style="italic" fill="#475569">{esc(fn)}</text>')
        bottom += 40

    H = bottom + 30
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{CW}" height="{H}" viewBox="0 0 {CW} {H}" '
           f'font-family="\'Segoe UI\', Roboto, Arial, sans-serif">', DEFS,
           f'  <rect x="0" y="0" width="{CW}" height="{H}" fill="#ffffff"/>',
           f'  <text x="{CX}" y="52" text-anchor="middle" font-size="22" font-weight="700" fill="#0f172a">{esc(d["title"])}</text>']
    svg.extend(body)
    svg.append('</svg>')
    return "\n".join(svg), CW, H

for d in DIAGRAMS:
    svg, w, h = build(d)
    svg_path = d["file"] + ".svg"
    png_path = os.path.abspath(d["file"] + ".png")
    open(svg_path, "w", encoding="utf-8").write(svg)
    tmp = tempfile.mkdtemp()
    html = os.path.join(tmp, "w.html")
    open(html, "w", encoding="utf-8").write('<!doctype html><meta charset=utf-8><style>html,body{margin:0;padding:0}</style>' + svg)
    profile = tempfile.mkdtemp()
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
                    f"--user-data-dir={profile}", "--force-device-scale-factor=2",
                    f"--window-size={w},{h}", f"--screenshot={png_path}", html],
                   capture_output=True)
    shutil.rmtree(tmp, ignore_errors=True); shutil.rmtree(profile, ignore_errors=True)
    data = open(png_path, "rb").read()
    pw, ph = struct.unpack(">II", data[16:24])
    print(f'{d["file"]}.png  ->  {pw} x {ph} px  ({len(data)//1024} KB)')
