# ClimateIQ Smart AC Controller — User Guide

This guide explains everything you can do with the device and exactly what happens
in each case. It covers the status light, the button, first‑time setup, the web
dashboard, how the automation behaves, cloud control, firmware updates, and
troubleshooting.

---

## 1. What the device does

ClimateIQ turns an ordinary air conditioner into a smart, presence‑aware unit. It:

- Controls your AC over **infrared** (the same signals your AC remote uses).
- Detects whether **someone is in the room** using a 24 GHz presence radar (LD2412).
- Measures **room temperature and humidity** (HDC1080 sensor).
- Runs a **daily schedule** (set from the cloud app) deciding when the AC should be on.
- Saves energy automatically: raises the temperature (**Eco**) and then turns the AC
  **off** when the room stays empty.
- Connects over **WiFi** (or **GSM/cellular** as an alternative) to report status and
  accept commands.
- Can update its own **firmware over the air**.

You interact with it three ways: the **status LED + button** on the device, the
**local web dashboard** (for setup), and the **cloud app** (for day‑to‑day control
and scheduling).

---

## 2. Status light (LED) reference

The device has an **RGB status LED** and a separate **presence LED**.

### RGB status LED — what each colour means

| Colour | Pattern | Meaning |
|--------|---------|---------|
| **White** | Solid | Booting / starting up |
| **Cyan** (green+blue) | Blinking | **Setup mode (Wi‑Fi hotspot active)** — connect to configure it |
| **Blue** | Blinking | Connecting / reconnecting to WiFi |
| **Green** | Brief flash every 5s (heartbeat) | **Healthy** — WiFi connected AND cloud connected, sending data |
| **Amber** (red+green) | Blinking | WiFi is connected but the **cloud broker is not** — not sending data yet |
| **Magenta** (red+blue) | Blinking | GSM/cellular connecting, **or** GSM connected (heartbeat every 5s) |
| **Magenta** | **Solid** | **Offline failsafe active** — no internet, the device is running the room locally on radar only |
| **Red** | Fast blink | Error |

> Tip: a slow green heartbeat = everything is working. Solid magenta = the device lost
> the internet and has taken over locally (this is a safe fallback, not a fault).

### Brief one‑off flashes (events)

| Flash | Meaning |
|-------|---------|
| **White** quick flash | An IR command was just sent to the AC |
| **Green** ×2 | An action succeeded (e.g. saved settings, learned a button) |
| **Red** ×3 | An action failed |
| **Yellow** ×3 | A warning |

### Presence LED

A separate LED lights up **only when radar automation is on AND a person is detected**
in the room. It goes off when the room is empty or radar automation is off.

---

## 3. The physical button

| Action | What happens |
|--------|--------------|
| **Short press** (tap) | Enters **Setup mode** — the device starts its WiFi hotspot so you can open the dashboard and change settings. |
| **Long press** (hold ~5 seconds) | **Exits Setup mode** and reconnects to your normal network. |

---

## 4. First‑time setup (connecting to WiFi)

1. **Power on** the device. If it has no saved WiFi, it automatically starts Setup mode
   (cyan blinking LED). You can also force Setup mode anytime with a **short button press**.
2. On your phone/laptop, connect to the WiFi network:
   - **Network name:** `SmartAC-Setup`
   - **Password:** `password123`
3. Open a browser and go to **`http://192.168.4.1`**.
4. Log in when prompted:
   - **Username:** `admin`
   - **Password:** `admin`
5. In **Network Settings**, choose **WiFi** mode, enter your router’s name (SSID) and
   password, and press **Save & Reboot**.
6. The device restarts and connects to your WiFi. Watch the LED: blue (connecting) →
   amber (waiting for cloud) → green heartbeat (fully connected).

> To switch transport: the same screen lets you choose **GSM/Cellular** instead of WiFi
> (for sites with no WiFi). Note: **OTA firmware updates work on WiFi only.**

---

## 5. The web dashboard (local setup screen)

Reachable at `http://192.168.4.1` in Setup mode (admin / admin). It has these sections:

### System Overview
Shows the saved AC protocol, which buttons you’ve learned, and current credentials/mode.

### IR Learning Center — teach the device your AC remote
Point your AC remote at the device and press the matching button on the page, then
press the button on your remote within 10 seconds:

| Button on page | What to do | What it stores |
|----------------|------------|----------------|
| **Detect AC Protocol** | Press any button on your remote | Identifies your AC brand/protocol |
| **Learn ON** | Press your remote’s power‑on | The exact “turn on” signal |
| **Learn OFF** | Press your remote’s power‑off | The exact “turn off” signal |
| **Learn 24/26/28/30 °C** | Set your remote to that temp and press | The exact signal for that temperature |

- A **green flash / success** message means it was saved. A **timeout** means no signal
  was received (try again, aim the remote closer).
- You don’t have to learn every button. If a specific temperature isn’t learned, the
  device falls back to a **universal signal** built from the detected protocol.

### Radar calibration
- **Auto‑Calibrate:** the radar learns your empty‑room background. **Empty the room
  first**, then press it. Takes up to ~2 minutes; presence detection is paused while it runs.
- **Factory Reset Radar:** restores the radar’s default settings.

### System Admin
- **Reset IR Memory:** erases all learned buttons/protocol (WiFi & settings are kept).
- **Wipe WiFi Credentials:** forgets the WiFi and reboots back into Setup mode.
- **Exit Setup Mode & Reconnect:** leaves the hotspot and rejoins your network (no reboot).
- **OTA Firmware Update:** upload a firmware file directly from your computer.
- **Developer Mode:** opens advanced tools (password protected).

### Developer Mode (password: `dev1234`)
Advanced panel for installers/technicians:

- **Radar Live Feed:** real‑time per‑zone movement/stationary energy bars.
- **Radar Detection Range:** set how far the radar looks (0.3–11 m; quick presets 2/4/6 m).
  Use a shorter range so the radar only covers the room, not the corridor.
- **Automation Parameters** (also settable from the cloud):
  | Parameter | Range | Meaning |
  |-----------|-------|---------|
  | Normal Temp | 16–32 °C | The AC set‑point when running normally |
  | Eco Temp | 16–32 °C | Raised set‑point used when the room is empty |
  | Eco Delay | 1–120 min | How long the room must be empty before switching to Eco |
  | Off Delay | 2–240 min | How long empty before the AC turns fully off |
  | Flap Delay | 0–120 sec | Time to ignore radar right after an OFF (prevents false re‑trigger) |
- **System Diagnostics:** heap, uptime, signal, versions, sensor health.
- **Remote Activity:** the last few remote presses the device detected.
- **Local Schedule Dump:** shows the schedule currently stored on the device.

---

## 6. How the automation works (what happens, and when)

### The schedule is the boss (“schedule is king”)
The daily schedule you set in the cloud app defines **when the AC should be on and at
what temperature**. Whenever the device is inside a scheduled time block, the AC runs;
outside all blocks, the AC is forced **off**. Radar only operates **within** scheduled
hours (unless you have no schedule at all, in which case radar runs freely).

### Radar presence automation (Normal → Eco → Off)
When radar automation is enabled and you’re inside a scheduled block:

1. **Someone present →** AC runs at the **Normal** temperature.
2. **Room empty for “Eco Delay” →** AC raises to the **Eco** temperature (saves energy).
3. **Room still empty for “Off Delay” →** AC turns **off** completely.
4. **Someone returns →** AC immediately turns back on at the Normal temperature.

### Flap delay
Right after the AC turns off, its fan/airflow can look like “motion” to the radar. The
**Flap Delay** makes the device ignore radar for a few seconds after an OFF so it doesn’t
instantly switch the AC back on.

### Manual remote presses (what the device reacts to)
The device is **always listening** to your AC remote. Behaviour:

- If you **change the temperature**, or **turn the AC on/off** with the remote in a way
  that **differs from the schedule** → the device **reverts** it back to the scheduled
  state (the schedule wins). Temperature is compared against the **Normal** set‑point.
- If you press any **other button** — swing, fan speed, mode, etc. — the device **does
  nothing** and your adjustment is respected.
- This only applies to a remote on the **same AC protocol** the device learned. Presses
  from other remotes are ignored (but still shown in *Remote Activity*).

> Note: the device also re‑confirms the AC state on a **periodic timer**. If you change a
> setting the schedule controls (temperature/power), it will be corrected — immediately if
> detected, otherwise at the next periodic check.

### Re‑confirmation (“enforce”)
Every few minutes the device re‑sends the current intended AC state. This corrects any
missed IR signal or manual change and keeps the AC matching the schedule/automation.

### Offline failsafe (no internet)
If the device can’t reach the cloud for about a minute and has no synced clock, it
switches to **local‑only** operation: it forces **radar control** so the room is still
automated by presence, and shows a **solid magenta** LED. When the internet/time returns,
normal scheduled operation resumes automatically.

### After a power cut
When power returns, the device reconnects, syncs the time, and then **immediately**
applies whatever the schedule says for the **current time** (it does not wait for
midnight or the next block). If it’s inside a scheduled block, the AC comes on right
away; if not, it stays off.

---

## 7. Cloud control (from the app / backend)

The cloud app can do everything in real time:

- **Set temperature** — change the AC set‑point.
- **Power on / off** — turn the AC on or off.
- **Enable / disable radar automation** — turn presence‑based saving on or off.
- **Set Eco values** — Eco temperature, Eco delay, Off delay.
- **Create / edit the daily schedule** — time blocks per weekday, each with its own
  temperature and optional radar/eco settings. A new or edited schedule for **today**
  takes effect immediately.
- **Trigger a firmware update** (see next section).

When the cloud sends a command, the device performs the action, saves the relevant
setting, and sends back an acknowledgement so the app can confirm it worked.

> If you send a manual on/off or temperature **outside** scheduled hours, the device
> applies it briefly but then reverts to OFF, because the schedule says the AC should be
> off at that time.

---

## 8. Firmware updates (OTA)

Two ways to update:

- **From the cloud:** the backend sends an update command with a download link. The
  device downloads and installs the new firmware, reboots, and reports the result.
- **From the dashboard:** Developer/Admin → **OTA Firmware Update** → upload a `.bin`
  file from your computer.

**Safety net (automatic rollback):** a newly installed firmware boots “on trial.” It is
only made permanent once the device proves it’s healthy (reconnects to the cloud). If the
new firmware keeps crashing/rebooting without connecting, the device **automatically
reverts to the previous working firmware**. You don’t lose the device on a bad update.

Notes:
- OTA requires **WiFi** (not available on GSM).
- The device refuses to install a firmware whose version equals the one it’s already
  running, unless the update is sent with “force”.

---

## 9. What the device reports (telemetry)

About every 10 seconds (when online) the device sends a status report to the cloud,
including: room temperature & humidity, presence/empty seconds, radar mode, current Eco
settings, radar distance, signal strength, uptime, free memory, and firmware version.

It also sends:
- A **boot report** when it starts (including why it last reset).
- **Health alerts** (e.g. low memory, radar gone silent).
- **Automation events** when it changes the AC (turned on / eco / off / manual override).

---

## 10. Quick reference

**Setup hotspot:** `SmartAC-Setup` / `password123` → `http://192.168.4.1`
**Dashboard login:** `admin` / `admin`
**Developer password:** `dev1234`

**Default automation values:** Normal 24 °C · Eco 26 °C · Eco delay 2 min · Off delay 5 min · Flap delay 10 s

**Button:** short press = enter Setup mode · long press (5 s) = exit Setup mode

---

## 11. Troubleshooting

| Symptom | Likely cause / what to do |
|---------|---------------------------|
| **LED stuck on amber blinking** | WiFi is fine but the cloud isn’t reachable. Check internet/broker; it keeps retrying automatically. |
| **LED solid magenta** | Offline failsafe — no internet. The room still works on radar. It recovers on its own when internet returns. |
| **AC doesn’t respond to commands** | The AC protocol/buttons may not be learned. Go to *IR Learning Center* and re‑learn Protocol + ON/OFF + the temperatures. Make sure the device has line‑of‑sight to the AC. |
| **AC keeps turning back on/off by itself** | That’s the schedule/radar automation. Check the schedule and that radar automation is set as you want; adjust Eco/Off delays. |
| **Radar triggers from the corridor / next room** | Reduce the **Detection Range** in Developer Mode, and run **Auto‑Calibrate** with the room empty. |
| **AC briefly turns on right after it turned off** | Increase the **Flap Delay** so radar is ignored a little longer after an OFF. |
| **Changing swing/fan on the remote gets reset** | A periodic re‑confirm re‑sends the full AC state. Swing/fan changes may revert at the next re‑confirm; temperature/power are what the device manages. |
| **My temperature change won’t stick** | Inside scheduled hours the schedule wins — manual temperature changes are reverted to the scheduled set‑point. Change it in the schedule/cloud instead. |
| **Forgot WiFi / moved the device** | Short‑press the button to enter Setup mode, reconnect to `SmartAC-Setup`, and enter the new WiFi. Or use **Wipe WiFi Credentials**. |
| **Can’t open the dashboard** | Make sure you’re connected to `SmartAC-Setup` (the device is in Setup mode — cyan blinking) and using `http://192.168.4.1`. |
