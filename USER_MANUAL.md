# ClimateIQ Smart AC Controller — Full User Manual

**Read me first.** This manual is written in plain language and assumes you have
never used a device like this before. Take it one step at a time. If something
doesn't work, jump to **Section 14 — Troubleshooting** at the end. Nothing you do
with the buttons or the dashboard can permanently break the device.

---

## Table of contents

1. What this device is (in one paragraph)
2. Simple words you need to know (glossary)
3. The parts of the device
4. The 5‑minute quick start
5. First‑time setup (the long, careful version)
6. The status light — what every colour means and what to do
7. The button — the only two things it does
8. Teaching the device your AC remote (very important)
9. Setting up the radar (presence sensor)
10. How the device decides to turn the AC on and off
11. Everyday use (the phone app / cloud)
12. Saving energy automatically
13. Updating the device software (firmware)
14. Troubleshooting (when something looks wrong)
15. Frequently asked questions
16. Do's and don'ts
17. One‑page quick reference (print this)

---

## 1. What this device is (in one paragraph)

ClimateIQ is a small box that sits near your air conditioner (AC) and makes it
"smart." It sends the **same invisible infrared signals your AC remote sends**, so
it can turn the AC on and off and change the temperature by itself. It can **tell
when someone is in the room** using a radar sensor, it knows the **room temperature
and humidity**, and it follows a **daily schedule** you set from a phone app. When
the room is empty it **saves electricity** by raising the temperature and then
switching the AC off. It connects to the internet over **WiFi** (or a **SIM/cellular**
connection) so you can control it from anywhere.

---

## 2. Simple words you need to know (glossary)

- **AC** — your air conditioner.
- **IR / Infrared** — the invisible light your AC remote uses. The device uses the
  same thing. This is why the device must be able to "see" the AC.
- **Radar / presence sensor** — the part that senses if a person is in the room.
  It does **not** use a camera and does **not** record anything. It just senses
  movement and a person's presence.
- **WiFi** — your home/office wireless internet.
- **GSM / Cellular** — internet using a SIM card, for places with no WiFi.
- **Setup mode (hotspot / AP mode)** — a special mode where the device makes its
  **own** little WiFi network so you can connect your phone to it and set it up.
- **Dashboard** — the setup web page you open in a browser.
- **Schedule** — a daily plan (set in the app) that says when the AC should be on
  and at what temperature.
- **Eco** — an energy‑saving mode: the AC keeps running but at a warmer temperature.
- **Firmware** — the software inside the device. It can be updated.
- **Cloud / app / backend** — the phone app or online service you use day to day.

---

## 3. The parts of the device

- **Status light (RGB LED)** — one small light that changes colour to tell you what
  the device is doing. This is your main way of knowing it's healthy. See Section 6.
- **Presence light** — a separate small light that turns **on only when the radar is
  active and a person is in the room**. If it's off, either the room is empty or
  radar automation is turned off. This is normal.
- **The button** — one physical button. It only does two things (Section 7).
- **Infrared blaster** — the part that "speaks" to your AC. It must point toward
  the AC with nothing blocking it.
- **Radar sensor** — senses people. Should face into the room, not the doorway/corridor.

> You don't need to open the device or touch any wiring. Everything in this manual
> is done with the button, your phone, or the app.

---

## 4. The 5‑minute quick start

If you just want it working fast, do these steps. Detailed versions follow later.

1. **Plug in / power on** the device. Wait about 30 seconds.
2. The light blinks **cyan (greenish‑blue)** = it wants to be set up. (If it doesn't,
   **press the button once** to force setup mode.)
3. On your phone, open WiFi settings and connect to:
   - **Network name:** `SmartAC-Setup`
   - **Password:** `password123`
4. Open a web browser and type: **`192.168.4.1`** then press Go.
5. When asked to log in: **Username** `admin`, **Password** `admin`.
6. Go to **Network Settings**, pick **WiFi**, type your home WiFi name and password,
   press **Save & Reboot**.
7. Wait. The light goes **blue** (connecting) → **amber/yellow** (almost there) →
   **green heartbeat** (a brief green flash every 5 seconds = fully working). ✅
8. **Teach it your AC remote** (Section 8) and you're done.

---

## 5. First‑time setup (the long, careful version)

### Step 5.1 — Power on
Connect the device to power. Give it about half a minute to start. While starting,
the light may be **solid white**.

### Step 5.2 — Get into Setup mode
- If the device has **never** been connected to WiFi, it goes into Setup mode by
  itself. You'll see the light **blinking cyan** (a greenish‑blue).
- If it doesn't, **press the button once** (a quick tap). That forces Setup mode.

### Step 5.3 — Connect your phone to the device
On your phone or laptop, open the WiFi list. You will see a network called
**`SmartAC-Setup`**. Connect to it using the password **`password123`**.

> Your phone may warn "No Internet" on this network. **That's expected.** The device's
> own network has no internet — you're only using it to set things up. Stay connected.

### Step 5.4 — Open the dashboard
Open any web browser (Chrome, Safari, etc.). In the address bar type exactly:

```
192.168.4.1
```

Press Go / Enter. A login box appears.
- **Username:** `admin`
- **Password:** `admin`

### Step 5.5 — Enter your WiFi
Find **Network Settings** on the page.
- Choose **WiFi** mode.
- Type your real WiFi **name (SSID)** exactly, including capital letters.
- Type your real WiFi **password** exactly.
- Press **Save & Reboot**.

> No WiFi at this location? Choose **GSM / Cellular** instead (needs a working SIM in
> the device). Note: software updates over the air work on **WiFi only**, not GSM.

### Step 5.6 — Watch it connect
The device restarts and joins your WiFi. Follow the light:
- **Blue blinking** = connecting to WiFi.
- **Amber/yellow blinking** = on WiFi, finishing the connection to the cloud.
- **Green flash every 5 seconds** = fully connected and healthy. ✅

If it gets stuck on amber, see Section 14.

### Step 5.7 — Teach the AC remote and set the schedule
Do Section 8 (teach the remote) now, then set your schedule in the app (Section 11).

---

## 6. The status light — what every colour means and what to do

This one light tells you almost everything. Here is the **complete** list.

### Steady states (the device is in this state until something changes)

| Light | How it looks | What it means | What you should do |
|------|--------------|---------------|--------------------|
| **White** | Solid (not blinking) | Starting up / booting | Wait ~30 seconds |
| **Cyan** (greenish‑blue) | Blinking | **Setup mode** — making its own hotspot | Connect to `SmartAC-Setup` to set it up |
| **Blue** | Blinking | Connecting / reconnecting to WiFi | Wait. If stuck, check WiFi name/password |
| **Green** | One brief flash every 5 seconds (heartbeat) | **Everything is healthy** — online and sending data | Nothing. This is the good one ✅ |
| **Amber / Yellow** | Blinking | On WiFi but **not yet talking to the cloud** | Wait; check your internet. It keeps retrying |
| **Magenta** (pink/purple) | Blinking | Cellular (GSM) is connecting | Wait |
| **Magenta** (pink/purple) | Brief flash every 5 seconds (heartbeat) | Cellular connected and healthy | Nothing. Good on GSM ✅ |
| **Magenta** (pink/purple) | **Solid, not blinking** | **Offline backup mode** — no internet, running the room locally on radar only | Nothing required. It's a safe fallback and fixes itself when internet returns |
| **Red** | Fast blinking | Error | See Troubleshooting. Try powering off and on |

### Quick one‑time flashes (an event just happened, then it returns to normal)

| Flash | Meaning |
|-------|---------|
| **White**, quick single flash | The device just sent a signal to your AC |
| **Green**, twice | Something worked (settings saved, button learned) |
| **Red**, three times | Something failed (try again) |
| **Yellow**, three times | A warning |
| **Red/Blue** fast alternating strobe (like police lights) | **Learning failed** — it didn't catch your remote's signal. Try again, closer |

> **The single most useful thing to remember:** a calm **green flash every 5 seconds**
> means the device is online and working. **Solid magenta** means no internet but the
> room is still being handled locally — also OK.

---

## 7. The button — the only two things it does

There is one button. It does exactly two things:

| You do this | This happens |
|-------------|--------------|
| **Tap it once** (short press) | Enters **Setup mode** (makes the `SmartAC-Setup` hotspot so you can change settings) |
| **Hold it ~5 seconds** then let go (long press) | **Exits Setup mode** and reconnects to your normal network |

That's it. You can't break anything with the button.

---

## 8. Teaching the device your AC remote (very important)

The device must learn your specific AC's signals, because every AC brand is
different. Do this in the **dashboard** (Setup mode → `192.168.4.1` → admin/admin),
in the section called **IR Learning Center**.

### How learning works (read this once)
For each item below, you **press the button on the web page first**, then **point
your real AC remote at the device and press the matching remote button within about
10 seconds.** A green flash / success means it saved. A failure (or the red/blue
police strobe) means it didn't hear the remote — try again, closer, aimed straight
at the device.

### What to teach (in order)

| On the web page press… | Then on your AC remote press… | What it saves |
|------------------------|-------------------------------|----------------|
| **Detect AC Protocol** | Any button on the remote | Your AC's brand/"language" |
| **Learn ON** | The power‑ON button | The exact "turn on" signal |
| **Learn OFF** | The power‑OFF button | The exact "turn off" signal |
| **Learn 24 °C** | Set remote to 24° and press | The 24° signal |
| **Learn 26 °C** | Set remote to 26° and press | The 26° signal |
| **Learn 28 °C** | Set remote to 28° and press | The 28° signal |
| **Learn 30 °C** | Set remote to 30° and press | The 30° signal |

### Good to know
- **You don't have to learn every temperature.** If a temperature isn't learned, the
  device builds a universal signal from the detected protocol. Learning them just
  makes it more reliable.
- Always **detect the protocol first**, then learn ON/OFF, then the temperatures.
- If the AC ignores the device later, come back here and re‑learn (Section 14).

---

## 9. Setting up the radar (presence sensor)

The radar is what makes the AC react to people. It's in the dashboard, partly under
**Developer Mode** (advanced tools, password `dev1234`).

### Auto‑Calibrate (teach it the empty room)
1. **Make the room empty** — everyone out, including you if you can, or stand very
   still out of the radar's view.
2. Press **Auto‑Calibrate Radar**.
3. Wait up to about **2 minutes**. Presence detection is paused while it learns the
   empty room's background.
4. A success message means it's done.

> Do this once after installing, and again if the radar gives false alarms.

### Detection Range (how far it looks)
In Developer Mode → **Radar Detection Range**, set how far the radar should sense
(roughly 0.3 m to 11 m; there are quick **2 m / 4 m / 6 m** buttons). **Set it just
big enough to cover your room** so it doesn't sense people in the corridor or the
next room.

### Factory Reset Radar
Restores the radar's own default settings if it's misbehaving. Re‑calibrate after.

---

## 10. How the device decides to turn the AC on and off

This section explains the "brain." Read it so the AC's behaviour never surprises you.

### Rule 1 — The schedule is the boss ("schedule is king")
The **daily schedule** (set in the app) says when the AC may run and at what
temperature.
- **Inside** a scheduled time block → the AC is allowed to run.
- **Outside** all blocks → the AC is forced **off**.
- The radar only works **inside** scheduled hours. (If you have **no** schedule at
  all, the radar runs freely all the time.)

### Rule 2 — Radar saves energy in three steps
When radar automation is on and you're inside a scheduled block:
1. **Person in the room** → AC runs at the **Normal** temperature.
2. **Room empty for the "Eco Delay"** (e.g. 2 minutes) → AC raises to the **Eco**
   temperature to save power.
3. **Room still empty for the "Off Delay"** (e.g. 5 minutes) → AC turns **off**.
4. **Someone comes back** → AC immediately returns to the Normal temperature.

### Rule 3 — Manual remote presses
The device is **always listening** to your AC remote:
- If you use the remote to **change temperature or turn the AC on/off** in a way that
  **disagrees with the schedule**, the device will **put it back** to what the
  schedule wants (the schedule wins).
- If you press **other** remote buttons (fan speed, swing, mode), the device leaves
  them alone — your adjustment stays.
- This only applies to a remote using the **same AC** the device learned. Other
  remotes are ignored (but shown in "Remote Activity" on the dashboard).

### Rule 4 — Power on/off from the app
- **App "Turn OFF"** → AC turns off immediately and stays off until the next scheduled
  block begins.
- **App "Turn ON"** → only works **during** scheduled hours. If you press ON when the
  schedule says the AC should be off, it turns on briefly then goes back off.

### Rule 5 — Flap delay (stops on/off flickering)
Right after the AC turns off, its moving air can look like "motion" to the radar. The
**Flap Delay** (a few seconds) makes the radar ignore this so the AC doesn't bounce
straight back on.

### Rule 6 — Re‑confirm every few minutes ("enforce")
Every few minutes the device re‑sends the AC's intended state. This fixes any signal
the AC missed and keeps reality matching the schedule.

### Rule 7 — Offline backup
If the internet is gone for about a minute and the clock isn't set, the device
switches to **local‑only** mode: it runs the room on radar and shows **solid magenta**.
It returns to normal automatically when the internet/time comes back.

### Rule 8 — After a power cut
When power returns, the device reconnects, gets the time, and **immediately** applies
whatever the schedule says for **right now** (it doesn't wait for midnight). Inside a
block → AC comes on; otherwise it stays off.

---

## 11. Everyday use (the phone app / cloud)

Once set up, you mostly use the **app**. From there you can, in real time:

- **Set the temperature.**
- **Turn the AC on or off** (remember Rule 4 above about scheduled hours).
- **Turn radar automation on or off.**
- **Set the Eco values** — Eco temperature, Eco delay, Off delay.
- **Create or edit the daily schedule** — time blocks for each weekday, each with its
  own temperature and optional radar/eco settings. A change for **today** takes effect
  right away.
- **Start a firmware update** (Section 13).

When you send a command, the device does it, saves the setting, and sends back a
confirmation so the app can show it worked.

---

## 12. Saving energy automatically

You don't have to do anything special — it's automatic when radar automation is on:
- Empty room for a while → temperature is raised (Eco) → then the AC turns off.
- Someone returns → AC comes back to your normal comfort temperature.

You can tune how aggressive this is by changing **Eco Delay** (how soon it goes to
Eco) and **Off Delay** (how soon it switches off) in the app or Developer Mode.

**Default values:** Normal **24 °C** · Eco **26 °C** · Eco delay **2 min** · Off
delay **5 min** · Flap delay **10 s**.

---

## 13. Updating the device software (firmware)

There are two ways to update, and a built‑in safety net.

**Two ways:**
- **From the cloud/app:** the backend sends an update with a download link; the device
  downloads, installs, restarts, and reports the result.
- **From the dashboard:** Developer/Admin → **OTA Firmware Update** → upload a `.bin`
  file from your computer.

**Safety net (automatic rollback):** a new firmware boots "on trial." It only becomes
permanent after the device proves it's healthy (reconnects to the cloud). If a bad
update keeps crashing, the device **automatically goes back to the previous working
version**. A bad update will not "kill" the device.

**Notes:**
- Over‑the‑air updates need **WiFi** (not available on GSM).
- The device won't reinstall the same version it's already running unless the update is
  sent with "force."

---

## 14. Troubleshooting (when something looks wrong)

| What you see | What it means / what to do |
|--------------|----------------------------|
| **Light blinks amber/yellow and won't go green** | WiFi is fine but the internet/cloud isn't reachable. Check your internet. The device keeps retrying on its own. |
| **Light is solid magenta** | Offline backup mode — no internet. This is **safe**; the room still works on radar. It recovers by itself when the internet returns. |
| **Light keeps blinking blue** | Can't join WiFi. The WiFi name or password is probably wrong. Tap the button to enter Setup mode and re‑enter them. |
| **AC won't respond at all** | The AC remote may not be learned, or the device can't "see" the AC. Re‑do **IR Learning Center** (Protocol → ON/OFF → temperatures) and make sure nothing blocks the line of sight to the AC. |
| **AC turns on/off "by itself"** | That's the schedule + radar working. Check your schedule and radar setting in the app; adjust Eco/Off delays if needed. |
| **Radar reacts to the corridor / next room** | Reduce the **Detection Range** in Developer Mode and run **Auto‑Calibrate** with the room empty. |
| **AC flicks back on right after turning off** | Increase the **Flap Delay** so the radar is ignored a little longer after an OFF. |
| **My temperature change keeps resetting** | Inside scheduled hours the schedule wins. Change the temperature in the **schedule/app**, not on the handheld remote. |
| **Swing/fan setting reverts** | The periodic re‑confirm re‑sends the AC state; swing/fan may revert. The device manages **power and temperature**, not fan/swing. |
| **Red/blue police‑light strobe during learning** | It didn't catch the remote. Aim the remote straight at the device, get closer, and try again. |
| **Can't open `192.168.4.1`** | Make sure your phone is connected to `SmartAC-Setup` (device must be in Setup mode = blinking cyan). Type the address without `https`. |
| **Forgot WiFi / moved to a new place** | Tap the button to enter Setup mode, reconnect to `SmartAC-Setup`, and enter the new WiFi. Or use **Wipe WiFi Credentials** in System Admin. |
| **Want to start fresh** | System Admin → **Reset IR Memory** (forgets learned buttons) and/or **Wipe WiFi Credentials** (forgets WiFi). |
| **Nothing works / totally stuck** | Power the device off, wait 10 seconds, power it on. If still stuck, enter Setup mode and check your settings. |

---

## 15. Frequently asked questions

**Does the radar use a camera or record me?**
No. It senses movement/presence only. There is no camera and no recording.

**Will it work if my internet goes down?**
Yes. It switches to offline backup mode (solid magenta) and keeps running the room on
radar. It reconnects automatically.

**Do I have to learn every temperature button?**
No. Learning ON, OFF, and the protocol is enough; temperatures improve reliability.

**Can I still use my normal AC remote?**
Yes, but during scheduled hours the device may revert power/temperature changes to
match the schedule. Other buttons (fan, swing) are left alone.

**Why is the AC off even though I turned it on in the app?**
You probably turned it on **outside** scheduled hours. The schedule forces it off at
those times. Adjust the schedule instead.

**The presence light is off — is something broken?**
No. It's only on when radar automation is enabled **and** a person is detected. Empty
room or radar off = light off. Normal.

**Is solid magenta a fault?**
No — it means "no internet, running locally." It's a safe fallback, not an error.

**Does GSM/cellular do everything WiFi does?**
Almost. The main exception: **firmware updates over the air need WiFi.**

---

## 16. Do's and don'ts

**Do**
- Point the device so it can "see" the AC, with nothing blocking it.
- Aim the radar into the room, not at the doorway/corridor.
- Empty the room before pressing Auto‑Calibrate.
- Set the schedule in the app for how you actually live.

**Don't**
- Don't expect manual remote temperature changes to stick during scheduled hours.
- Don't put the radar facing a busy hallway (false triggers).
- Don't worry about "No Internet" when connected to `SmartAC-Setup` — that's normal.
- Don't keep power‑cycling during a firmware update; let it finish.

---

## 17. One‑page quick reference (print this)

**Setup hotspot:** connect phone to `SmartAC-Setup` / password `password123`
**Open dashboard:** browser → `192.168.4.1`
**Dashboard login:** `admin` / `admin`
**Developer Mode password:** `dev1234`

**Button:** tap = enter Setup mode · hold 5 s = exit Setup mode

**Status light:**
- Green flash every 5 s = healthy (WiFi) ✅
- Magenta flash every 5 s = healthy (cellular) ✅
- Solid magenta = no internet, running locally (safe)
- Blinking cyan = Setup mode (connect to set it up)
- Blinking blue = connecting to WiFi
- Blinking amber/yellow = on WiFi, waiting for cloud
- Fast red = error (power off/on)

**Defaults:** Normal 24 °C · Eco 26 °C · Eco delay 2 min · Off delay 5 min · Flap delay 10 s

**Golden rules:**
1. The schedule is the boss. Outside scheduled hours the AC stays off.
2. Empty room → Eco → Off, automatically. Someone returns → back on.
3. To "see" the AC and the room: keep the IR path clear and the radar aimed inside.

---

*If you get truly stuck, note the status light colour/pattern and what you were doing,
and share that with your installer or support — it tells them almost everything.*
