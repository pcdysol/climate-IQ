#!/usr/bin/env python
"""Generate a styled inter-task / event data-flow SVG for the Eco Pulse firmware."""

W, H = 1180, 890
o = []
o.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
         f'font-family="\'Segoe UI\', Roboto, Arial, sans-serif">')
o.append('''  <defs>
    <filter id="sh" x="-30%" y="-30%" width="160%" height="160%">
      <feDropShadow dx="0" dy="2" stdDeviation="4" flood-color="#0f172a" flood-opacity="0.18"/>
    </filter>
    <marker id="arr" markerWidth="13" markerHeight="13" refX="9" refY="5" orient="auto" markerUnits="userSpaceOnUse">
      <path d="M0,0 L10,5 L0,10 z" fill="#64748b"/>
    </marker>
  </defs>''')
o.append(f'  <rect x="0" y="0" width="{W}" height="{H}" fill="#ffffff"/>')
o.append('  <text x="590" y="42" text-anchor="middle" font-size="25" font-weight="700" fill="#0f172a">Eco Pulse — Inter-Task &amp; Event Data Flow</text>')
o.append('  <text x="590" y="66" text-anchor="middle" font-size="13" fill="#64748b">Solid = event posted via automationQueue&#160;&#160;·&#160;&#160;Dashed = direct function call</text>')

def pill(cx, cy, w, h, text, fill):
    x = cx - w / 2
    o.append(f'  <rect x="{x:.0f}" y="{cy-h/2:.0f}" width="{w}" height="{h}" rx="{h/2:.0f}" fill="{fill}" filter="url(#sh)"/>')
    o.append(f'  <text x="{cx:.0f}" y="{cy+5:.0f}" text-anchor="middle" font-size="14.5" font-weight="700" fill="#ffffff">{text}</text>')

def card(cx, cy, w, h, title, accent, sub="", title_size=15):
    x, y = cx - w / 2, cy - h / 2
    cid = f"c{int(cx)}{int(cy)}"
    o.append(f'  <clipPath id="{cid}"><rect x="{x:.0f}" y="{y:.0f}" width="{w}" height="{h}" rx="13"/></clipPath>')
    o.append(f'  <rect x="{x:.0f}" y="{y:.0f}" width="{w}" height="{h}" rx="13" fill="#ffffff" filter="url(#sh)"/>')
    o.append(f'  <g clip-path="url(#{cid})"><rect x="{x:.0f}" y="{y:.0f}" width="{w}" height="6" fill="{accent}"/></g>')
    o.append(f'  <rect x="{x:.0f}" y="{y:.0f}" width="{w}" height="{h}" rx="13" fill="none" stroke="#e2e8f0"/>')
    ty = cy + 5 if not sub else cy - 4
    o.append(f'  <text x="{cx:.0f}" y="{ty:.0f}" text-anchor="middle" font-size="{title_size}" font-weight="700" fill="{accent}">{title}</text>')
    if sub:
        o.append(f'  <text x="{cx:.0f}" y="{cy+15:.0f}" text-anchor="middle" font-size="11.5" fill="#64748b">{sub}</text>')

def queue_node(cx, cy, w, h, title, accent, sub):
    x, y = cx - w / 2, cy - h / 2
    # stacked sheets behind to read as a queue
    o.append(f'  <rect x="{x+10:.0f}" y="{y+10:.0f}" width="{w}" height="{h}" rx="13" fill="#c7d2fe"/>')
    o.append(f'  <rect x="{x+5:.0f}" y="{y+5:.0f}" width="{w}" height="{h}" rx="13" fill="#a5b4fc"/>')
    o.append(f'  <rect x="{x:.0f}" y="{y:.0f}" width="{w}" height="{h}" rx="13" fill="{accent}" filter="url(#sh)"/>')
    o.append(f'  <text x="{cx:.0f}" y="{cy-4:.0f}" text-anchor="middle" font-size="15.5" font-weight="700" fill="#ffffff">{title}</text>')
    o.append(f'  <text x="{cx:.0f}" y="{cy+15:.0f}" text-anchor="middle" font-size="11.5" fill="#e0e7ff">{sub}</text>')

def arrow(points, dashed=False, color="#64748b"):
    pts = " ".join(f"{px:.0f},{py:.0f}" for px, py in points)
    dash = ' stroke-dasharray="7 5"' if dashed else ""
    o.append(f'  <polyline points="{pts}" fill="none" stroke="{color}" stroke-width="2.6"{dash} '
             f'marker-end="url(#arr)" stroke-linejoin="round"/>')

def label(x, y, text, anchor="middle", size=11.5):
    w = len(text) * size * 0.56
    o.append(f'  <rect x="{x - (w/2 if anchor=="middle" else 0):.0f}" y="{y-12:.0f}" width="{w:.0f}" height="17" rx="4" fill="#ffffff" opacity="0.92"/>')
    o.append(f'  <text x="{x:.0f}" y="{y:.0f}" text-anchor="{anchor}" font-size="{size}" font-weight="600" fill="#475569">{text}</text>')

# ---- arrows first (under nodes) ----
arrow([(110, 133), (110, 191)])                                   # A radar -> SensorsTask
arrow([(1010, 133), (1010, 191)])                                 # B mqtt  -> CommandProcessor
arrow([(580, 133), (580, 292)])                                   # C timers -> queue
arrow([(215, 235), (385, 235), (385, 330), (452, 330)])           # D SensorsTask -> queue
arrow([(580, 366), (580, 420)])                                   # E queue -> AutoTask
arrow([(580, 482), (580, 532)])                                   # F AutoTask -> exec
arrow([(580, 600), (580, 651)])                                   # G exec -> IR
arrow([(580, 711), (580, 769)])                                   # H IR -> AC
arrow([(895, 225), (760, 225), (760, 567), (708, 567)], dashed=True)   # I CommandProcessor -> exec
arrow([(705, 585), (800, 585), (800, 645), (891, 645)])           # J exec -> publish
arrow([(110, 343), (110, 261)])                                   # K WebTask -> SensorsTask

# arrow labels
label(300, 222, "EVENT_*  (presence / override)")
label(596, 252, "EVENT_*")
label(836, 210, "acts directly", anchor="middle")
label(852, 600, "publishACK / event")
label(206, 305, "xTaskNotify bits", anchor="start")
label(206, 322, "(calibrate / range / learn)", anchor="start", size=10.5)
label(635, 745, "IR 38 kHz", anchor="start")

# ---- nodes ----
pill(110, 110, 210, 46, "Radar / HDC poll", "#2563eb")
pill(580, 110, 264, 46, "Timers · eco / off / enforce", "#8b5cf6")
pill(1010, 110, 232, 46, "MQTT RX  (Wi-Fi / GSM)", "#06b6d4")

card(110, 225, 210, 66, "SensorsTask", "#2563eb", "radar + IR-RX owner")
card(1010, 225, 232, 66, "CommandProcessor", "#06b6d4", "acts directly")
card(110, 372, 190, 58, "WebTask", "#f59e0b")
queue_node(580, 330, 250, 72, "automationQueue", "#4338ca", "depth 10 · SystemEvent")
card(580, 452, 250, 60, "AutomationTask", "#6366f1", "drains the queue")
card(580, 567, 250, 66, "executeACCommand()", "#d946ef", "single AC choke point")
card(580, 682, 250, 58, "IR  (irMutex)", "#ec4899", "learned button / universal")
card(1010, 645, 236, 70, "NetworkManager::publish*", "#0d9488", "→ MQTT broker", title_size=14)
pill(580, 797, 210, 52, "AC UNIT", "#059669")

o.append('</svg>')
open("interTask_dataflow.svg", "w", encoding="utf-8").write("\n".join(o))
print("wrote interTask_dataflow.svg")
