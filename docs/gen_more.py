#!/usr/bin/env python
"""Render 5 more Eco Pulse diagrams (power, comms, state machines, error handling) as PNGs."""
import os, subprocess, tempfile, struct, shutil

CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"
if not os.path.exists(CHROME):
    CHROME = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"

DEFS = '''  <defs>
    <filter id="sh" x="-40%" y="-40%" width="180%" height="180%">
      <feDropShadow dx="0" dy="2" stdDeviation="4" flood-color="#0f172a" flood-opacity="0.16"/>
    </filter>
    <marker id="ae" markerWidth="13" markerHeight="13" refX="9" refY="5" orient="auto" markerUnits="userSpaceOnUse">
      <path d="M0,0 L10,5 L0,10 z" fill="#64748b"/></marker>
    <marker id="asr" markerWidth="13" markerHeight="13" refX="9" refY="5" orient="auto-start-reverse" markerUnits="userSpaceOnUse">
      <path d="M0,0 L10,5 L0,10 z" fill="#64748b"/></marker>
  </defs>'''

def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

def solid(cx, cy, w, h, title, fill, sub=None, tsize=15, tcol="#ffffff", rx=14):
    x, y = cx - w / 2, cy - h / 2
    s = [f'<rect x="{x:.0f}" y="{y:.0f}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" filter="url(#sh)"/>']
    if sub:
        s.append(f'<text x="{cx:.0f}" y="{cy-3:.0f}" text-anchor="middle" font-size="{tsize}" font-weight="700" fill="{tcol}">{esc(title)}</text>')
        s.append(f'<text x="{cx:.0f}" y="{cy+15:.0f}" text-anchor="middle" font-size="11.5" fill="{tcol}" opacity="0.92">{esc(sub)}</text>')
    else:
        s.append(f'<text x="{cx:.0f}" y="{cy+5:.0f}" text-anchor="middle" font-size="{tsize}" font-weight="700" fill="{tcol}">{esc(title)}</text>')
    return "\n  ".join(s)

def card(cx, cy, w, h, title, accent, sub=None, tsize=14):
    x, y = cx - w / 2, cy - h / 2
    cid = f"c{int(cx)}_{int(cy)}"
    s = [f'<rect x="{x:.0f}" y="{y:.0f}" width="{w}" height="{h}" rx="13" fill="#ffffff" filter="url(#sh)"/>',
         f'<clipPath id="{cid}"><rect x="{x:.0f}" y="{y:.0f}" width="{w}" height="{h}" rx="13"/></clipPath>',
         f'<g clip-path="url(#{cid})"><rect x="{x:.0f}" y="{y:.0f}" width="{w}" height="6" fill="{accent}"/></g>',
         f'<rect x="{x:.0f}" y="{y:.0f}" width="{w}" height="{h}" rx="13" fill="none" stroke="#e2e8f0"/>']
    if sub:
        s.append(f'<text x="{cx:.0f}" y="{cy+1:.0f}" text-anchor="middle" font-size="{tsize}" font-weight="700" fill="{accent}">{esc(title)}</text>')
        s.append(f'<text x="{cx:.0f}" y="{cy+19:.0f}" text-anchor="middle" font-size="11.5" fill="#64748b">{esc(sub)}</text>')
    else:
        s.append(f'<text x="{cx:.0f}" y="{cy+6:.0f}" text-anchor="middle" font-size="{tsize}" font-weight="700" fill="{accent}">{esc(title)}</text>')
    return "\n  ".join(s)

def lab(x, y, text, anchor="middle", size=11.5, col="#475569"):
    w = len(text) * size * 0.56
    bx = x - (w / 2 if anchor == "middle" else (0 if anchor == "start" else w))
    return (f'<rect x="{bx:.0f}" y="{y-12:.0f}" width="{w:.0f}" height="17" rx="4" fill="#ffffff" opacity="0.92"/>'
            f'<text x="{x:.0f}" y="{y:.0f}" text-anchor="{anchor}" font-size="{size}" font-weight="600" fill="{col}">{esc(text)}</text>')

def line(x1, y1, x2, y2, double=False, dash=False):
    d = ' stroke-dasharray="7 5"' if dash else ""
    ms = ' marker-start="url(#asr)"' if double else ""
    return f'<line x1="{x1:.0f}" y1="{y1:.0f}" x2="{x2:.0f}" y2="{y2:.0f}" stroke="#64748b" stroke-width="2.5"{dash} marker-end="url(#ae)"{ms}/>'

def path(d, dash=False):
    da = ' stroke-dasharray="7 5"' if dash else ""
    return f'<path d="{d}" fill="none" stroke="#64748b" stroke-width="2.5"{da} marker-end="url(#ae)"/>'

def wrap(title, body, W, H):
    return ("\n".join([
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
        f'font-family="\'Segoe UI\', Roboto, Arial, sans-serif">', DEFS,
        f'<rect x="0" y="0" width="{W}" height="{H}" fill="#ffffff"/>',
        f'<text x="{W/2:.0f}" y="46" text-anchor="middle" font-size="23" font-weight="700" fill="#0f172a">{esc(title)}</text>',
    ]) + "\n" + "\n".join("  " + x for x in body) + "\n</svg>")

# ============================================================ 1. POWER
def d_power():
    b = []
    # arrows
    b.append(line(230, 136, 230, 210)); b.append(lab(245, 178, "5V", "start"))
    b.append(line(345, 250, 463, 250)); b.append(lab(404, 240, "3.3V"))
    b.append(line(230, 292, 230, 398)); b.append(lab(245, 348, "5V", "start"))
    b.append(line(355, 440, 488, 440))
    for cy in (120, 195, 270, 345):
        b.append(path(f"M695,250 C 740,250 735,{cy} 773,{cy}"))
    # nodes
    b.append(solid(230, 110, 250, 52, "DC / USB Adapter (5V)", "#ea580c"))
    b.append(solid(230, 250, 230, 84, "On-board regulator", "#f59e0b", "5 V → 3.3 V"))
    b.append(solid(580, 250, 230, 84, "ESP32", "#2563eb", "3V3 rail"))
    b.append(card(900, 120, 250, 54, "Radar — LD2412", "#2563eb", "UART"))
    b.append(card(900, 195, 250, 54, "HDC1080", "#0891b2", "I²C"))
    b.append(card(900, 270, 250, 54, "RGB / status LEDs", "#7c3aed", "GPIO"))
    b.append(card(900, 345, 250, 54, "GSM modem", "#0d9488", "when fitted"))
    b.append(solid(230, 440, 250, 88, "IR MOSFET gate", "#f59e0b", "GPIO 27 · held active"))
    b.append(card(640, 440, 300, 62, "IR TX LED + always-on IR RX", "#ec4899"))
    return wrap("Eco Pulse — Power Architecture", b, 1060, 520)

# ============================================================ 2. COMMS
def d_comms():
    b = []
    left = [(175, "LD2412 / GSM", "UART 16/17", "#2563eb"),
            (247, "HDC1080", "I²C", "#0891b2"),
            (319, "IR RX / TX", "IR 38 kHz", "#ec4899"),
            (391, "LED / Button", "GPIO", "#7c3aed"),
            (463, "IR MOSFET", "GPIO", "#f59e0b")]
    right = [(210, "MQTT Broker", "MQTT · Wi-Fi / GSM", "#0d9488"),
             (320, "Time source", "NTP / cell RTC", "#0891b2"),
             (430, "OTA server", "HTTP(S)", "#6366f1")]
    for cy, _, busl, _ in left:
        b.append(line(325, cy, 455, cy, double=True)); b.append(lab(390, cy - 9, busl))
    for cy, _, busl, _ in right:
        b.append(line(665, cy, 780, cy, double=True)); b.append(lab(722, cy - 9, busl))
    b.append(solid(560, 320, 210, 320, "ESP32 CORE", "#1e293b", "FreeRTOS · NVS", tsize=18))
    for cy, t, _, ac in left:
        b.append(card(200, cy, 250, 54, t, ac))
    for cy, t, _, ac in right:
        b.append(card(905, cy, 250, 56, t, ac))
    return wrap("Eco Pulse — Communication Architecture", b, 1070, 540)

# ============================================================ 3. AC STATE MACHINE
def d_acsm():
    b = []
    b.append(path("M320,282 C 450,180 540,160 658,172")); b.append(lab(505, 150, "room occupied (radar)"))
    b.append(path("M660,205 C 540,250 460,290 349,305")); b.append(lab(505, 258, "schedule end / outside segment / cloud OFF"))
    b.append(path("M660,470 C 530,455 460,380 349,335")); b.append(lab(500, 420, "off countdown elapsed"))
    b.append(line(830, 222, 830, 428)); b.append(lab(845, 325, "eco countdown elapsed", "start"))
    b.append(line(750, 428, 750, 222)); b.append(lab(745, 325, "re-occupied", "end"))
    b.append(solid(240, 320, 210, 80, "AUTO_OFF", "#64748b"))
    b.append(solid(790, 180, 260, 80, "AUTO_ON_NORMAL", "#059669", "normal setpoint"))
    b.append(solid(790, 470, 260, 80, "AUTO_ON_ECO", "#d97706", "raised setpoint"))
    return wrap("Eco Pulse — AC Automation State Machine", b, 1100, 600)

# ============================================================ 4. SYSTEM/CONNECTIVITY STATE
def d_sysstate():
    b = []
    # BOOTING -> mid
    for cy in (110, 250, 420, 540):
        b.append(path(f"M245,320 C 300,320 300,{cy} 353,{cy}"))
    # WIFI_CONN -> right
    b.append(path("M585,250 C 660,250 650,200 713,200")); b.append(lab(648, 188, "Wi-Fi + MQTT up"))
    b.append(path("M585,250 C 660,250 650,300 713,300")); b.append(lab(648, 360, "Wi-Fi up, broker down"))
    b.append(line(585, 420, 713, 420))
    b.append(lab(470, 96, "no SSID / config"))
    b.append(line(830, 330, 830, 508, dash=True)); b.append(lab(845, 420, "60 s offline", "start"))
    # nodes
    b.append(solid(150, 320, 190, 70, "SYS_BOOTING", "#64748b"))
    b.append(solid(470, 110, 230, 60, "SYS_AP_MODE", "#2563eb"))
    b.append(solid(470, 250, 230, 60, "SYS_WIFI_CONN", "#3b82f6"))
    b.append(solid(470, 420, 230, 60, "SYS_GSM_CONN", "#0891b2"))
    b.append(solid(470, 540, 230, 60, "SYS_ERROR", "#dc2626"))
    b.append(solid(830, 200, 230, 60, "SYS_WIFI_OK", "#059669", "delivering data"))
    b.append(solid(830, 300, 230, 60, "SYS_MQTT_DOWN", "#ea580c", "not sending"))
    b.append(solid(830, 420, 230, 60, "SYS_GSM_OK", "#059669"))
    b.append(solid(830, 540, 300, 64, "Offline failsafe", "#c026d3", "magenta override · radar takes over"))
    return wrap("Eco Pulse — System / Connectivity State", b, 1120, 620)

# ============================================================ 5. ERROR HANDLING
def d_error():
    b = []
    cols = [(240, "DETECT", "#dc2626"), (600, "RECOVER", "#2563eb"), (960, "REPORT", "#0d9488")]
    rows = [(190, ["HealthTimer staleness", "re-init bus · restart link", "MQTT health alert", "publishHealthAlert"]),
            (330, ["WDT / brownout", "safe reboot", "reset-reason on boot", None]),
            (470, ["OTA trial boot", "auto-rollback", "OTA phase events", None])]
    for cy, _ in rows:
        b.append(line(392, cy, 448, cy))
        b.append(line(752, cy, 808, cy))
    for cx, t, c in cols:
        b.append(solid(cx, 100, 300, 46, t, c, tsize=16))
    for cy, items in rows:
        det, rec, rep, repsub = items
        b.append(card(240, cy, 300, 92, det, "#dc2626"))
        b.append(card(600, cy, 300, 92, rec, "#2563eb"))
        b.append(card(960, cy, 300, 92, rep, "#0d9488", repsub))
    return wrap("Eco Pulse — Error Handling: Detect → Recover → Report", b, 1200, 560)

JOBS = [("power_architecture", d_power), ("communication_architecture", d_comms),
        ("ac_state_machine", d_acsm), ("system_state", d_sysstate), ("error_handling", d_error)]

for name, fn in JOBS:
    svg = fn()
    open(name + ".svg", "w", encoding="utf-8").write(svg)
    import re
    m = re.search(r'width="(\d+)" height="(\d+)"', svg)
    w, h = int(m.group(1)), int(m.group(2))
    png = os.path.abspath(name + ".png")
    tmp = tempfile.mkdtemp(); prof = tempfile.mkdtemp()
    html = os.path.join(tmp, "w.html")
    open(html, "w", encoding="utf-8").write('<!doctype html><meta charset=utf-8><style>html,body{margin:0;padding:0}</style>' + svg)
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
                    f"--user-data-dir={prof}", "--force-device-scale-factor=2",
                    f"--window-size={w},{h}", f"--screenshot={png}", html], capture_output=True)
    shutil.rmtree(tmp, ignore_errors=True); shutil.rmtree(prof, ignore_errors=True)
    data = open(png, "rb").read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pw, ph = struct.unpack(">II", data[16:24])
    print(f"{name}.png  ->  {pw} x {ph} px  ({len(data)//1024} KB)")
