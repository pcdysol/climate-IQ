#!/usr/bin/env python
"""Generate a styled layered-architecture SVG for the Eco Pulse firmware."""

CARD_W = 420
LEFT_X, RIGHT_X = 60, 520
HEADER_H, ITEM_H = 44, 28
TOP_PAD, BOT_PAD = 14, 14

def card_height(n):
    return HEADER_H + TOP_PAD + n * ITEM_H + BOT_PAD

rows = [
    [  # row 1
        ("APPLICATION LAYER", "#2563eb", "#1d4ed8",
         ["AutomationManager", "ScheduleManager", "CommandProcessor", "OTAManager", "HealthManager"]),
        ("SERVICE / ABSTRACTION LAYER", "#6366f1", "#4338ca",
         ["SensorManager", "IRManager", "NetworkManager", "WebDashboard", "Indicator"]),
    ],
    [  # row 2
        ("TRANSPORT LAYER", "#8b5cf6", "#6d28d9",
         ["WiFiManager  (AsyncMqtt)", "GSMManager  (AT cmds)"]),
        ("DRIVER / LIBRARY LAYER", "#d946ef", "#a21caf",
         ["MyLD2410  (LD2412 fork)", "Adafruit HDC1000",
          "IRremoteESP8266 / TinyGSM", "AsyncMqttClient / ArduinoJson"]),
    ],
    [  # row 3
        ("PLATFORM / RTOS LAYER", "#0891b2", "#0e7490",
         ["Arduino-ESP32 core", "FreeRTOS scheduler", "ESP-IDF  (wdt / log / time)", "NVS  (Preferences)"]),
        ("HARDWARE", "#059669", "#047857",
         ["ESP32-WROOM-32", "LD2412 radar / HDC1080", "IR TX + RX / MOSFET", "GSM modem / RGB LED / button"]),
    ],
]

START_Y, ROW_GAP = 120, 26
row_h = [max(card_height(len(l[3])), card_height(len(r[3]))) for l, r in rows]
row_y = []
y = START_Y
for h in row_h:
    row_y.append(y)
    y += h + ROW_GAP
container_h = (row_y[-1] + row_h[-1]) - 20 + 34
canvas_h = 20 + container_h + 20

out = []
out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="{canvas_h}" '
           f'viewBox="0 0 1000 {canvas_h}" font-family="\'Segoe UI\', Roboto, Arial, sans-serif">')
out.append('''  <defs>
    <filter id="sh" x="-20%" y="-20%" width="140%" height="140%">
      <feDropShadow dx="0" dy="2" stdDeviation="4" flood-color="#0f172a" flood-opacity="0.16"/>
    </filter>
    <marker id="arr" markerWidth="12" markerHeight="12" refX="5" refY="9" orient="auto" markerUnits="userSpaceOnUse">
      <path d="M0,0 L10,0 L5,9 z" fill="#94a3b8"/>
    </marker>
  </defs>''')
out.append(f'  <rect x="0" y="0" width="1000" height="{canvas_h}" fill="#ffffff"/>')
# outer container
out.append(f'  <rect x="20" y="20" width="960" height="{container_h}" rx="20" fill="#f1f5f9" stroke="#cbd5e1"/>')
out.append('  <text x="500" y="62" text-anchor="middle" font-size="26" font-weight="700" fill="#0f172a">ECO PULSE FIRMWARE</text>')
out.append('  <text x="500" y="88" text-anchor="middle" font-size="13" fill="#64748b">Layered firmware architecture — upper layers depend only on the layer below</text>')

def emit_card(x, y, title, accent, accent_dark, items):
    h = row_height
    cid = f"clip{x}{y}"
    out.append(f'  <clipPath id="{cid}"><rect x="{x}" y="{y}" width="{CARD_W}" height="{h}" rx="14"/></clipPath>')
    out.append(f'  <rect x="{x}" y="{y}" width="{CARD_W}" height="{h}" rx="14" fill="#ffffff" filter="url(#sh)"/>')
    out.append(f'  <g clip-path="url(#{cid})"><rect x="{x}" y="{y}" width="{CARD_W}" height="{HEADER_H}" fill="{accent}"/></g>')
    out.append(f'  <rect x="{x}" y="{y}" width="{CARD_W}" height="{h}" rx="14" fill="none" stroke="#e2e8f0"/>')
    out.append(f'  <text x="{x + CARD_W/2:.0f}" y="{y + 28}" text-anchor="middle" font-size="14.5" '
               f'font-weight="700" fill="#ffffff" letter-spacing="0.5">{title}</text>')
    iy = y + HEADER_H + TOP_PAD + 12
    for it in items:
        out.append(f'  <circle cx="{x + 20}" cy="{iy - 4}" r="3.2" fill="{accent}"/>')
        out.append(f'  <text x="{x + 34}" y="{iy}" font-size="13" fill="#334155">{it}</text>')
        iy += ITEM_H

for ri, (left, right) in enumerate(rows):
    row_height = row_h[ri]
    ry = row_y[ri]
    emit_card(LEFT_X, ry, *left)
    emit_card(RIGHT_X, ry, *right)

# downward dependency arrows between rows (both columns)
out.append('  <g stroke="#94a3b8" stroke-width="2.5" marker-end="url(#arr)">')
for ri in range(len(rows) - 1):
    y1 = row_y[ri] + row_h[ri]
    y2 = row_y[ri + 1]
    for cx in (LEFT_X + CARD_W / 2, RIGHT_X + CARD_W / 2):
        out.append(f'    <line x1="{cx:.0f}" y1="{y1 + 2}" x2="{cx:.0f}" y2="{y2 - 4}"/>')
out.append('  </g>')

out.append('</svg>')

open("layer_architecture.svg", "w", encoding="utf-8").write("\n".join(out))
print("wrote layer_architecture.svg", f"({canvas_h}px tall)")
