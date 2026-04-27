#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include "Adafruit_HDC1000.h"
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <time.h>
#include "esp_mac.h"
#include <IRutils.h>
#include <ir_Electra.h> // You can delete this line
#include <IRac.h>       // ADD THIS LINE INSTEAD
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <IRrecv.h>
#include <rom/rtc.h>
#include "MyLD2410.h"
#include "board_select.h"
#include "esp_task_wdt.h"

// HardwareSerial sensorSerial(2);
MyLD2410 sensor(sensorSerial);
bool sensorReady = false;

// ===== Radar State & Auto Mode =====
bool radarAutoMode = false; // Disabled by default, overridden by MQTT
bool cachedPresence = false;
uint8_t cachedMovingSignal = 0;
uint8_t cachedStationarySignal = 0;

// ===== Variance Filtering (From Node Code) =====
const int VARIANCE_SAMPLES = 20;
const uint8_t VARIANCE_THRESHOLD = 2;
uint8_t statHistory[VARIANCE_SAMPLES];
uint8_t historyIdx = 0;
bool historyFull = false;
bool isVarianceNoise = false;
// ===== Radar Presence Tracking =====
bool lastPresenceState = false;
unsigned long lastStateChangeTime = 0;
unsigned long accumulatedPresenceMs = 0;

// ===== Automation Timers =====
unsigned long lastPresenceTime = 0;
// Default Parameters (Internal time is tracked in milliseconds)
int currentNormalTemp = 24; // <--- ADD THIS: Remembers the scheduled/manual temp
int TEcoTemp = 26;          // Default Eco Temperature
bool hdcInitFailed = false;
bool radarInitFailed = false;

// ===== Runtime Health Tracking =====
unsigned long lastRadarDataTime = 0;        // Last time a DATA frame arrived from radar
bool radarStaleAlerted = false;             // Prevents repeat alerts for same stale event
int hdcFaultCount = 0;                      // Consecutive NaN reads; triggers I2C re-init at threshold
const unsigned long RADAR_STALE_MS = 30000; // 30s without a frame = radar is hung
const int HDC_REINIT_THRESHOLD = 3;         // Attempt I2C reset after this many consecutive NaN reads
unsigned long TEcoTime = 120000;            // 2 mins (in ms) to trigger Eco Mode
unsigned long TOffTime = 300000;            // 5 mins (in ms) to turn OFF

// State machine for automation
enum AutoState
{
  AUTO_OFF,
  AUTO_ON_NORMAL,
  AUTO_ON_ECO
};
AutoState acAutoState = AUTO_OFF;

// ===== Local Scheduler =====
bool radarManualOverride = false; // true = standalone radar cmd won; schedule radar blocked
bool radarManualValue    = false;
int  lastScheduledHour   = -1;   // edge-trigger: fires once per hour change

// ===== System Mode Flag =====
bool switch_gsm_wifi; // Will be set dynamically from memory
bool isAPMode = false;

// ===== Hardware Configuration =====
#define BUTTON_PIN 26
#define LED_PIN 2
#define RECV_PIN 19
#define IR_PIN 18
#define MOSFET_PIN 27
#define GSM_RX_PIN 16
#define GSM_TX_PIN 17
// ===== RGB LED Configuration (Common Anode) =====
#define RED_PIN 32
#define GREEN_PIN 33
#define BLUE_PIN 4

// ===== System State Tracking =====
enum SystemState
{
  SYS_BOOTING,
  SYS_AP_MODE,
  SYS_WIFI_CONN,
  SYS_WIFI_OK,
  SYS_GSM_CONN,
  SYS_GSM_OK,
  SYS_ERROR
};
SystemState currentSysState = SYS_BOOTING;

// ===== WiFi Credentials (Now Dynamic) =====
String currentSSID = "";
String currentPassword = "";

// ===== Web Dashboard Admin Credentials =====
const char *www_username = "admin";
const char *www_password = "admin";

// ===== SoftAP Setup =====
const char *AP_SSID = "SmartAC-Setup";
const char *AP_PASSWORD = "password123";

// At the top of your file
bool pendingReboot = false;
unsigned long rebootTime = 0;

// ===== MQTT Broker =====
const char *mqtt_server = "107.172.137.233";
const int mqtt_port = 1883;

// ===== NTP (Internet Time) =====
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 5 * 3600;
const int daylightOffset_sec = 0;

// ===== Globals =====
String macAddress;
String mqttTopic;
String device_id;
String macAddress_gsm;
String gsmClientId;
String pubTopic;
String subTopic;
unsigned long lastTelemetry = 0;
String lastSignalStrength = "N/A";
unsigned long data_delay_interval = 10000;
const unsigned long TELEMETRY_INTERVAL = 10000;
// Later use this value
//  Change from 10000 to 900000 (15 minutes in milliseconds)
// const unsigned long TELEMETRY_INTERVAL = 900000;

// ===== Objects =====
Adafruit_HDC1000 hdc = Adafruit_HDC1000();
IRsend irsend(IR_PIN);
IRac ac(IR_PIN); // Universal AC object
WiFiClient espClient;
PubSubClient client(espClient);
HardwareSerial SerialAT(1);
WebServer server(80);
Preferences preferences;
IRrecv irrecv(RECV_PIN, 2048, 100, true);

// ===== Forward Declarations =====
void startAPMode();
void stopAPMode();
void setupWebServer();
void updateLED();
// void indicateSuccess();
// void indicateError();
void handleButton();
// ---> ADD THESE THREE LINES <---
void sensorLoop();
void automationLoop();
void trackPresenceTime();
void healthLoop();
void publishHealthAlert(const char *event, const char *detail);
void calibrateRadarAuto();
void calibrateRadarReset();
void scheduleLoop();
void handleScheduleCommand(JsonDocument &doc);
void setSystemTimeFromGSM();
void processJSON(JsonDocument &doc);

// ===== Non-Blocking Delay for Responsiveness =====
// Replaces standard delay() in blocking loops so the button always works.
void smartDelay(unsigned long ms)
{
  unsigned long start = millis();
  while (millis() - start < ms)
  {
    handleButton();
    updateLED(); // <--- ADD THIS HERE
    if (isAPMode)
      break; // Break out immediately if AP mode is triggered
    delay(50);
  }
}

String getResetReason()
{
  RESET_REASON reason = rtc_get_reset_reason(0); // Check CPU 0
  switch (reason)
  {
  case 1:
    return "Power On";
  case 3:
    return "Software Reset";
  case 4:
    return "Watchdog Reset";
  case 12:
    return "Software Watchdog";
  case 14:
    return "Panic / Hard Crash";
  default:
    return "Unknown (" + String(reason) + ")";
  }
}

// ====================================================================
// ==================== DASHBOARD & AP MODE LOGIC =====================
// ====================================================================

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">  <meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>AC Controller Dashboard</title>
<style>
  :root { --bg: #0f1724; --card: #111827; --text: #f8fafc; --acc: #3b82f6; --ok: #10b981; --err: #ef4444; --wait: #eab308; }
  body { font-family: system-ui, -apple-system, sans-serif; background: var(--bg); color: var(--text); padding: 20px; margin: 0; }
  .dashboard-grid { display: grid; grid-template-columns: 300px 1fr 280px; gap: 20px; max-width: 1200px; margin: 0 auto; }
  .card { background: var(--card); border-radius: 12px; padding: 20px; box-shadow: 0 4px 6px -1px rgba(0,0,0,0.2); border: 1px solid #1f2937; }
  .card h3 { margin-top: 0; border-bottom: 1px solid #1f2937; padding-bottom: 10px; color: #94a3b8; font-size: 1.1rem; }
  .status-panel { background: #000; padding: 15px; border-radius: 8px; border: 1px solid #334155; font-family: monospace; }
  .status-row { margin-bottom: 12px; border-bottom: 1px dashed #334155; padding-bottom: 8px; }
  .status-row:last-child { border-bottom: none; margin-bottom: 0; padding-bottom: 0; }
  .status-label { font-size: 0.85rem; color: #94a3b8; text-transform: uppercase; letter-spacing: 0.5px; margin-bottom: 6px; }
  .badge { background: #1f2937; padding: 4px 10px; border-radius: 6px; display: inline-block; font-size: 0.9rem; margin: 2px; color: white; }
  .badge.yellow { background: var(--wait); color: #000; font-weight: bold; }
  .badge.green { background: var(--ok); color: white; font-weight: bold; }
  .badge.red { background: var(--err); color: white; font-weight: bold; }
  #action-log { color: var(--ok); margin-top: 5px; font-size: 0.95rem; }
  button { width: 100%; padding: 12px; margin-bottom: 10px; border: none; border-radius: 8px; font-weight: 600; font-size: 1rem; cursor: pointer; transition: 0.2s; color: white; }
  button:active { transform: scale(0.98); }
  button:disabled { opacity: 0.5; cursor: not-allowed; }
  .btn-primary { background: var(--acc); }
  .btn-purple { background: #6366f1; }
  .btn-danger { background: var(--err); }
  .btn-warn { background: #d97706; }
  .btn-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
  @media (max-width: 900px) { .dashboard-grid { grid-template-columns: 1fr; } }
</style>
<script>
  function setUIState(stateText, colorClass) {
    const el = document.getElementById('sys-mode');
    el.innerText = stateText; el.className = "badge " + colorClass;
  }
  async function refreshStatus(isIdle = true) {
    if(isIdle) setUIState("System Ready", "green");
    try {
      const res = await fetch('/status'); const data = await res.json();
      document.getElementById('sys-protocol').innerText = data.protocol || "None Detected";
      // ---> NEW: Set the dropdown to current mode <---
      if(document.getElementById('sys-mode-select')) {
        document.getElementById('sys-mode-select').value = data.mode;
      }
      // NEW: Update the WiFi status badge
      const wifiBadge = document.getElementById('sys-wifi-status');
      if (data.has_credentials) {
        wifiBadge.innerText = "Saved in Memory";
        wifiBadge.className = "badge green";
      } else {
        wifiBadge.innerText = "Missing / Empty";
        wifiBadge.className = "badge yellow";
      }
      const keysDiv = document.getElementById('sys-keys');
      if (data.keys.length > 0) { keysDiv.innerHTML = data.keys.map(k => `<span class="badge">${k}</span>`).join(''); } 
      else { keysDiv.innerHTML = "<span style='color: #64748b;'>No Buttons Saved</span>"; }
    } catch(e) { setUIState("Disconnected", "red"); }
  }
  async function doAction(action, endpoint, btn) {
    const actionLog = document.getElementById('action-log');
    const allBtns = document.querySelectorAll('button');
    allBtns.forEach(b => b.disabled = true);
    setUIState("Listening...", "yellow");
    actionLog.innerText = '> Point remote & press ' + action; actionLog.style.color = 'var(--wait)';
    try {
      const res = await fetch(endpoint); const text = await res.text();
      actionLog.innerText = '> ' + text;
      if(res.ok) { actionLog.style.color = 'var(--ok)'; setUIState("Success", "green"); } 
      else { actionLog.style.color = 'var(--err)'; setUIState("Failed", "red"); }
    } catch (e) {
      actionLog.innerText = '> Network Error!'; actionLog.style.color = 'var(--err)'; setUIState("Error", "red");
    }
    allBtns.forEach(b => b.disabled = false); refreshStatus(false);
    setTimeout(() => { setUIState("System Ready", "green"); actionLog.innerText = '> Ready'; actionLog.style.color = 'var(--ok)'; }, 3000);
  }
  async function saveSettings() {
    const mode = document.getElementById('sys-mode-select').value;
    const s = document.getElementById('new-ssid').value;
    const p = document.getElementById('new-pass').value;
    
    if(mode === 'wifi' && !s) return alert('SSID cannot be empty for WiFi mode');
    
    if(confirm('Save settings and reboot device?')) {
      const url = `/setwifi?mode=${mode}&ssid=${encodeURIComponent(s)}&pass=${encodeURIComponent(p)}`;
      await fetch(url, { method: 'POST' });
      alert('Saved! Device is rebooting.');
    }
  }
  window.onload = refreshStatus;
  // Calibration uses a separate helper so the action-log message is appropriate.
  async function doCalibrate(endpoint, btn) {
    const actionLog = document.getElementById('action-log');
    const allBtns = document.querySelectorAll('button');
    allBtns.forEach(b => b.disabled = true);
    setUIState("Calibrating...", "yellow");
    const isAuto = endpoint.includes('auto');
    actionLog.innerText = isAuto
      ? '> EMPTY the room now — calibration takes up to 120s...'
      : '> Sending factory reset to radar...';
    actionLog.style.color = 'var(--wait)';
    try {
      const res = await fetch(endpoint);
      const text = await res.text();
      actionLog.innerText = '> ' + text;
      if(res.ok) { actionLog.style.color = 'var(--ok)'; setUIState("Done", "green"); }
      else { actionLog.style.color = 'var(--err)'; setUIState("Failed", "red"); }
    } catch(e) {
      actionLog.innerText = '> Network Error!'; actionLog.style.color = 'var(--err)'; setUIState("Error", "red");
    }
    allBtns.forEach(b => b.disabled = false);
    setTimeout(() => { setUIState("System Ready", "green"); actionLog.innerText = '> Ready'; actionLog.style.color = 'var(--ok)'; }, 5000);
  }
</script>
</head>
<body>
  <div class="dashboard-grid">
    <div class="card">
      <h3>System Overview</h3>
      <div class="status-panel">
        <div class="status-row"><div class="status-label">Current State</div><div id="sys-mode" class="badge green">Loading...</div><div id="action-log">> Ready</div></div>
        <div class="status-row">
        <div class="status-label">WiFi Credentials</div>
        <div id="sys-wifi-status" class="badge">Checking...</div>
        </div>
        <div class="status-row"><div class="status-label">AC Protocol</div><div id="sys-protocol" style="color: var(--acc); font-size: 1.1rem; font-weight: bold;">Loading...</div></div>
        <div class="status-row" style="border: none;"><div class="status-label">Saved Commands</div><div id="sys-keys">Loading...</div></div>
      </div>
    </div>
    <div class="card">
      <h3>IR Learning Center</h3>
      <button class="btn-primary" style="margin-bottom: 20px; padding: 15px;" onclick="doAction('ANY Button', '/learn/protocol', this)">Detect AC Protocol</button>
      <div style="font-size: 0.85rem; color: #64748b; margin-bottom: 10px; text-transform: uppercase;">Custom Buttons</div>
      <div class="btn-grid">
        <button class="btn-purple" onclick="doAction('ON', '/learn/on', this)">Learn ON</button>
        <button class="btn-purple" onclick="doAction('OFF', '/learn/off', this)">Learn OFF</button>
        <button class="btn-purple" style="background: #8b5cf6;" onclick="doAction('24°C', '/learn/24', this)">Learn 24 °C</button>
        <button class="btn-purple" style="background: #8b5cf6;" onclick="doAction('26°C', '/learn/26', this)">Learn 26 °C</button>
        <button class="btn-purple" style="background: #a855f7;" onclick="doAction('28°C', '/learn/28', this)">Learn 28 °C</button>
        <button class="btn-purple" style="background: #a855f7;" onclick="doAction('30°C', '/learn/30', this)">Learn 30 °C</button>
      </div>
    </div>
    <div class="card">
      <h3>Network Settings</h3>
      <div style="margin-top: 15px;">
        <div style="font-size: 0.85rem; color: #94a3b8; margin-bottom: 5px;">SYSTEM MODE</div>
        <select id="sys-mode-select" style="width: 100%; box-sizing: border-box; padding: 10px; margin-bottom: 15px; border-radius: 6px; border: 1px solid #334155; background: #1f2937; color: white;">
          <option value="wifi">WiFi Mode</option>
          <option value="gsm">GSM Mode</option>
        </select>

        <div style="font-size: 0.85rem; color: #94a3b8; margin-bottom: 5px;">WIFI SSID</div>
        <input type="text" id="new-ssid" placeholder="Enter Router Name" style="width: 100%; box-sizing: border-box; padding: 10px; margin-bottom: 15px; border-radius: 6px; border: 1px solid #334155; background: #1f2937; color: white;">
        
        <div style="font-size: 0.85rem; color: #94a3b8; margin-bottom: 5px;">WIFI PASSWORD</div>
        <input type="password" id="new-pass" placeholder="Enter Password" style="width: 100%; box-sizing: border-box; padding: 10px; margin-bottom: 15px; border-radius: 6px; border: 1px solid #334155; background: #1f2937; color: white;">
        
        <button class="btn-warn" onclick="saveSettings()">Save & Reboot</button>
      </div>
    </div>
    <div class="card">
      <h3>System Admin</h3>
      <div style="margin-top: 15px;">
        <button class="btn-danger" onclick="if(confirm('Wipe all saved IR data?')) doAction('Reset', '/reset', this)">Reset Memory</button>
        <button class="btn-warn" onclick="window.location.href='/update'">OTA Update</button>
      </div>
    </div>
    <div class="card">
      <h3>Radar Calibration</h3>
      <p style="font-size:0.85rem; color:#94a3b8; margin-top:0;">
        <b>Auto-Calibrate:</b> Room must be completely empty. The sensor learns the ambient noise floor and sets thresholds automatically (firmware &ge; 2.44). Takes up to 120 seconds.<br><br>
        <b>Factory Reset:</b> Wipes learned thresholds and restores HLK defaults. Use this if Auto-Calibrate produces false triggers.
      </p>
      <button class="btn-primary" onclick="if(confirm('EMPTY the room first. Calibration will take up to 120s. Continue?')) doCalibrate('/calibrate/auto', this)">Auto-Calibrate (Recommended)</button>
      <button class="btn-warn" onclick="if(confirm('Reset radar to factory defaults?')) doCalibrate('/calibrate/reset', this)">Factory Reset Radar</button>
    </div>
  </div>
</body>
</html>
)rawliteral";

// ====================================================================
// ========================= RGB LED LOGIC ============================
// ====================================================================

// Base helper to set colors (Common Anode: LOW = ON, HIGH = OFF)
void setColor(bool r, bool g, bool b)
{
  digitalWrite(RED_PIN, r ? LOW : HIGH);
  digitalWrite(GREEN_PIN, g ? LOW : HIGH);
  digitalWrite(BLUE_PIN, b ? LOW : HIGH);
}
void ledOff() { setColor(false, false, false); }

// ----- Action Indicators (Quick Interrupts) -----
void indicateSuccess()
{
  ledOff();
  for (int i = 0; i < 2; i++)
  { // 2x Quick Green
    setColor(false, true, false);
    delay(150);
    ledOff();
    delay(150);
  }
}

void indicateError()
{
  ledOff();
  for (int i = 0; i < 3; i++)
  { // 3x Quick Red
    setColor(true, false, false);
    delay(150);
    ledOff();
    delay(150);
  }
}

void indicateIRSent()
{
  setColor(true, true, true); // 1x Quick White Flash
  delay(100);
  ledOff();
}

// ----- Background State Blinker -----
void updateLED()
{
  // ---> NEW: OVERRIDE ALL BLINKING ONLY IF RADAR IS ENABLED AND HUMAN DETECTED <---
  // if (radarAutoMode && cachedPresence)
  // {
  //   setColor(false, false, true); // Solid BLUE when a human is present
  //   return;                       // Exit the function so it doesn't blink the background status
  // }
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  unsigned long now = millis();

  int interval = 500;
  bool r = 0, g = 0, b = 0;

  // Determine color and speed based on current state
  switch (currentSysState)
  {
  case SYS_BOOTING:
    r = 1;
    g = 1;
    b = 1;
    interval = 0;
    break; // Solid White
  case SYS_AP_MODE:
    r = 0;
    g = 1;
    b = 1;
    interval = 500;
    break; // Cyan Blink
  case SYS_WIFI_CONN:
    r = 0;
    g = 0;
    b = 1;
    interval = 500;
    break; // Blue Blink
  case SYS_WIFI_OK:
    r = 0;
    g = 1;
    b = 0;
    interval = 5000;
    break; // Green Heartbeat
  case SYS_GSM_CONN:
    r = 1;
    g = 0;
    b = 1;
    interval = 500;
    break; // Purple Blink
  case SYS_GSM_OK:
    r = 1;
    g = 0;
    b = 1;
    interval = 5000;
    break; // Purple Heartbeat
  case SYS_ERROR:
    r = 1;
    g = 0;
    b = 0;
    interval = 200;
    break; // Fast Red Blink
  }

  // Apply the lighting pattern
  if (interval == 0)
  {
    setColor(r, g, b); // Solid
  }
  else if (interval == 5000)
  {
    // Heartbeat logic: flash for 50ms every 5000ms
    if (now - lastBlink > 5000)
      lastBlink = now;
    if (now - lastBlink < 50)
      setColor(r, g, b);
    else
      ledOff();
  }
  else
  {
    // Standard alternating blink logic
    if (now - lastBlink > interval)
    {
      lastBlink = now;
      ledState = !ledState;
    }
    if (ledState)
      setColor(r, g, b);
    else
      ledOff();
  }
}

void learnIRCommand(const char *storageKey, const char *displayName, bool isProtocol = false)
{
  digitalWrite(MOSFET_PIN, HIGH);
  delay(100);
  irrecv.enableIRIn();

  decode_results results;
  unsigned long startTime = millis();
  bool signalReceived = false;

  while (millis() - startTime < 10000)
  {
    if (irrecv.decode(&results))
    {
      // 1. Reject obvious ambient noise (spikes shorter than 30 ticks)
      if (results.rawlen < 30)
      {
        Serial.println("Ignored noise spike. Still listening...");
        irrecv.resume();
        continue;
      }

      // 2. If hunting for a protocol, ignore UNKNOWN signals
      if (isProtocol && results.decode_type == UNKNOWN)
      {
        Serial.println("Ignored unknown protocol. Still listening...");
        irrecv.resume();
        continue;
      }

      // If we reach here, it is a valid, heavy IR payload
      signalReceived = true;
      break;
    }
    delay(10);
  }

  irrecv.disableIRIn();
  digitalWrite(MOSFET_PIN, LOW);

  if (signalReceived)
  {
    if (isProtocol)
    {
      String protocolName = typeToString(results.decode_type, results.repeat);
      if (protocolName == "UNKNOWN")
      {
        Serial.println("PROTOCOL NOT RECOGNIZED!");
        indicateError();
        server.send(400, "text/plain", "Brand not recognized. Please use Custom Buttons");
      }
      else
      {
        preferences.putString("protocol_name", protocolName);
        Serial.printf("✓ PROTOCOL DETECTED: %s\n", protocolName.c_str());
        server.send(200, "text/plain", "Protocol [" + protocolName + "] Saved!");
      }
    }
    else
    {
      // 1. Convert internal ticks to microseconds
      uint16_t *raw_array = resultToRawArray(&results);
      uint16_t raw_length = getCorrectedRawLength(&results);
      size_t arrayBytes = raw_length * sizeof(uint16_t);

      // 2. Save the converted array
      preferences.putBytes(storageKey, raw_array, arrayBytes);
      preferences.putBool((String("has_") + storageKey).c_str(), true);

      // 3. Free the memory allocated by resultToRawArray to prevent heap leaks
      delete[] raw_array;

      Serial.printf("✓ CUSTOM BUTTON [%s] SAVED!\n", displayName);
      server.send(200, "text/plain", String(displayName) + " Saved Successfully!");
    }
    indicateSuccess();
  }
  else
  {
    Serial.println("✗ TIMEOUT: No IR signal detected.");
    indicateError();
    server.send(408, "text/plain", "Timeout: Try again.");
  }
}

String getTimestamp()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return "1970-01-01T00:00:00";
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  return String(buffer);
}

void setupWebServer()
{
  // Require Admin Login for Dashboard
  server.on("/", HTTP_GET, []()
            { 
    if (!server.authenticate(www_username, www_password)) {
      return server.requestAuthentication();
    }
    server.send(200, "text/html", FPSTR(DASHBOARD_HTML)); });

  server.on("/setwifi", HTTP_POST, []()
            {
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
  
  if (server.hasArg("mode")) {
      String modeStr = server.arg("mode");
      bool isWiFi = (modeStr == "wifi");
      String newSSID = server.hasArg("ssid") ? server.arg("ssid") : "";
      String newPass = server.hasArg("pass") ? server.arg("pass") : "";

      // Ensure WiFi mode isn't saved without an SSID
      if (isWiFi && newSSID.length() == 0) {
        server.send(400, "text/plain", "Error: No SSID provided for WiFi mode!");
        return;
      }

      Serial.println("\n[SYSTEM] Saving New Settings:");
      Serial.println("Mode: " + modeStr);
      Serial.println("SSID: " + newSSID);
      
      // Save Mode and Credentials
      preferences.putBool("use_wifi", isWiFi);
      preferences.putString("wifi_ssid", newSSID);
      preferences.putString("wifi_pass", newPass);
      
      // In the Web Handler
      server.send(200, "text/plain", "Settings Saved. Rebooting...");
      pendingReboot = true;
      rebootTime = millis() + 2000;
    } else {
      server.send(400, "text/plain", "Error: Incomplete request!");
    } });
  server.onNotFound([]()
                    { server.send(404, "text/plain", "Not Found"); });

  server.on("/status", HTTP_GET, []()
            {
  JsonDocument doc;
  
  // 1. Protocol
  doc["protocol"] = preferences.isKey("protocol_name") ? preferences.getString("protocol_name") : "None";
  
  // 2. Keys Array
  JsonArray keys = doc["keys"].to<JsonArray>();
  
  auto addKey = [&](const char* keyId, const char* name) {
    if (preferences.getBool((String("has_") + keyId).c_str(), false)) {
      keys.add(name);
    }
  };
  
  addKey("ir_on", "ON"); 
  addKey("ir_off", "OFF");
  addKey("ir_24", "24°C"); 
  addKey("ir_26", "26°C");
  addKey("ir_28", "28°C"); 
  addKey("ir_30", "30°C");

  // NEW: Check if credentials exist in memory
  doc["has_credentials"] = (preferences.getString("wifi_ssid", "").length() > 0);
  
  // 3. Mode
  doc["mode"] = switch_gsm_wifi ? "wifi" : "gsm";
  
  // 4. Serialize securely (Allocates exact required memory once)
  String json;
  serializeJson(doc, json);
  
  server.send(200, "application/json", json); });

  server.on("/learn/protocol", HTTP_GET, []()
            { learnIRCommand("ir_protocol", "Protocol", true); });
  server.on("/learn/on", HTTP_GET, []()
            { learnIRCommand("ir_on", "ON", false); });
  server.on("/learn/off", HTTP_GET, []()
            { learnIRCommand("ir_off", "OFF", false); });
  server.on("/learn/24", HTTP_GET, []()
            { learnIRCommand("ir_24", "24°C", false); });
  server.on("/learn/26", HTTP_GET, []()
            { learnIRCommand("ir_26", "26°C", false); });
  server.on("/learn/28", HTTP_GET, []()
            { learnIRCommand("ir_28", "28°C", false); });
  server.on("/learn/30", HTTP_GET, []()
            { learnIRCommand("ir_30", "30°C", false); });

  server.on("/reset", HTTP_GET, []()
            {
    preferences.clear();
    indicateSuccess(); delay(100); indicateSuccess();
    server.send(200, "text/plain", "Memory Wiped Clean."); });

  server.on("/calibrate/auto",  HTTP_GET, []() { calibrateRadarAuto(); });
  server.on("/calibrate/reset", HTTP_GET, []() { calibrateRadarReset(); });

  server.on("/update", HTTP_GET, []()
            { server.send(200, "text/html", "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Upload'></form>"); });
  server.on("/update", HTTP_POST, []()
            {
    server.send(200, "text/plain", Update.hasError() ? "OTA FAILED" : "OTA SUCCESS - Rebooting");
    delay(1000); ESP.restart(); }, []()
            {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) { Update.begin(UPDATE_SIZE_UNKNOWN); } 
    else if (upload.status == UPLOAD_FILE_WRITE) { Update.write(upload.buf, upload.currentSize); } 
    else if (upload.status == UPLOAD_FILE_END) { Update.end(true); } });
}

void startAPMode()
{
  if (isAPMode)
    return;
  currentSysState = SYS_AP_MODE; // <--- ADD THIS LINE
  Serial.println("\n--- SWITCHING TO AP (DASHBOARD) MODE ---");

  // Cleanly disconnect current systems
  if (switch_gsm_wifi)
  {
    client.disconnect();
    WiFi.disconnect(true);
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.println("✓ HOTSPOT ACTIVE: Connect to " + String(AP_SSID));

  setupWebServer();
  server.begin();
  isAPMode = true;
  indicateSuccess();
}

void stopAPMode()
{
  if (!isAPMode)
    return;
  Serial.println("\n--- SWITCHING TO NORMAL MODE ---");

  server.stop();
  WiFi.softAPdisconnect(true);

  // Let the main loop handle the reconnection process
  if (switch_gsm_wifi)
  {
    WiFi.mode(WIFI_STA);
  }
  else
  {
    WiFi.mode(WIFI_OFF);
  }

  isAPMode = false;
  indicateSuccess();
  delay(100);
  indicateSuccess();
}

void handleButton()
{
  static bool lastState = HIGH;
  static unsigned long pressStart = 0;
  static bool longPressHandled = false;

  bool currentState = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  if (currentState == LOW && lastState == HIGH)
  {
    pressStart = now;
    longPressHandled = false;
    delay(50);
  }
  else if (currentState == LOW && lastState == LOW)
  {
    // LONG PRESS (5 SECONDS) -> Exit AP Mode / Enter Normal Mode
    if (!longPressHandled && (now - pressStart) >= 5000)
    {
      stopAPMode();
      longPressHandled = true;
    }
  }
  else if (currentState == HIGH && lastState == LOW)
  {
    // SHORT PRESS -> Enter AP Mode
    if (!longPressHandled && (now - pressStart) > 50)
    {
      startAPMode();
    }
    delay(50);
  }
  lastState = currentState;
}

// ====================================================================
// ==================== NORMAL SYSTEM/NETWORK LOGIC ===================
// ====================================================================

bool playCustomButton(const char *storageKey)
{
  // Check if we learned this button
  if (preferences.getBool((String("has_") + storageKey).c_str(), false))
  {
    size_t len = preferences.getBytesLength(storageKey);
    if (len > 0)
    {
      uint16_t elements = len / sizeof(uint16_t);
      uint16_t rawData[elements];

      // Pull data from flash memory
      preferences.getBytes(storageKey, rawData, len);

      // Blast the raw signal (38kHz is standard for 99% of remotes)
      irsend.sendRaw(rawData, elements, 38);
      Serial.printf("Sent Custom Signal: %s\n", storageKey);
      return true;
    }
  }
  return false; // Return false if no custom button was saved
}

void sendACFallback(bool turnOn, int targetTemp)
{
  // 1. Get the detected protocol from flash memory
  String savedProto = preferences.getString("protocol_name", "");
  decode_type_t protocol = decode_type_t::UNKNOWN;

  if (savedProto != "")
  {
    protocol = strToDecodeType(savedProto.c_str());
  }

  // 2. Default to Electra if memory is empty
  if (protocol == decode_type_t::UNKNOWN)
  {
    protocol = decode_type_t::ELECTRA_AC;
  }

  // 3. Configure the universal AC state dynamically
  ac.next.protocol = protocol;
  ac.next.power = turnOn;
  ac.next.degrees = targetTemp;
  ac.next.mode = stdAc::opmode_t::kCool;
  ac.next.fanspeed = stdAc::fanspeed_t::kAuto;

  // 4. Blast the specific brand's signal!
  ac.sendAc();
}

// ====================================================================
// ==================== UNIFIED JSON COMMAND PARSER ===================
// ====================================================================
// Single source of truth for all MQTT commands.
// Called by both callback() (WiFi) and checkIncomingData() (GSM).
void processJSON(JsonDocument &doc)
{
  // Schedule command: "command" + "hours" array → store and return early.
  if (doc["command"] == "temperature_control" && doc["hours"])
  {
    handleScheduleCommand(doc);
    return;
  }

  if (doc["radar"])
  {
    String radarStr = doc["radar"].as<String>();
    Serial.println("Received Radar Value: " + radarStr);

    if (radarStr == "0100" || radarStr == "256")
    {
      if (!radarAutoMode)
      {
        radarAutoMode = true;
        preferences.putBool("radar_auto", true);
        lastPresenceTime = millis();
        Serial.println("Radar Automation: ENABLED");
        indicateSuccess();
      }
    }
    else if (radarStr == "0200" || radarStr == "512")
    {
      if (radarAutoMode)
      {
        radarAutoMode = false;
        preferences.putBool("radar_auto", false);
        Serial.println("Radar Automation: DISABLED");
        indicateSuccess();
      }
    }
    // Standalone radar → mark as manual override so schedule can't revert it.
    radarManualOverride = true;
    radarManualValue    = radarAutoMode;
    preferences.putBool("rad_ovr",   true);
    preferences.putBool("rad_ovr_v", radarAutoMode);
  }

  if (doc["temperature_setting"])
  {
    currentNormalTemp = doc["temperature_setting"].as<int>();
    preferences.putInt("normal_temp", currentNormalTemp);
    Serial.printf("Updated Normal Temp: %d°C\n", currentNormalTemp);
  }

  if (doc["eco"])
  {
    TEcoTemp = doc["eco"].as<int>();
    preferences.putInt("eco_temp", TEcoTemp);
    Serial.printf("Updated TEcoTemp: %d°C\n", TEcoTemp);
  }

  if (doc["teco"])
  {
    TEcoTime = doc["teco"].as<unsigned long>() * 60000;
    preferences.putULong("eco_time", TEcoTime);
    Serial.printf("Updated TEcoTime: %lu ms\n", TEcoTime);
  }

  if (doc["toff"])
  {
    TOffTime = doc["toff"].as<unsigned long>() * 60000;
    if (TOffTime <= TEcoTime)
    {
      TOffTime = TEcoTime + 60000;
      Serial.println("WARNING: TOffTime was <= TEcoTime. Auto-corrected.");
    }
    preferences.putULong("off_time", TOffTime);
    Serial.printf("Updated TOffTime: %lu ms\n", TOffTime);
  }

  if (doc["ir"])
  {
    int cmdNum = doc["ir"].as<int>();
    Serial.printf("Received IR Command Code: %d\n", cmdNum);

    if (cmdNum == 1)
    {
      if (!playCustomButton("ir_on")) sendACFallback(true, 24);
      indicateIRSent();
      acAutoState = AUTO_ON_NORMAL;
    }
    else if (cmdNum == 2)
    {
      if (!playCustomButton("ir_off")) sendACFallback(false, 24);
      indicateIRSent();
      acAutoState = AUTO_OFF;
    }
    else if (cmdNum >= 3 && cmdNum <= 17)
    {
      int targetTemp = cmdNum + 13;
      String customKey = "ir_" + String(targetTemp);
      Serial.printf("Action: Set Temp to %d°C\n", targetTemp);
      if (!playCustomButton(customKey.c_str())) sendACFallback(true, targetTemp);
      indicateIRSent();
      acAutoState = AUTO_ON_NORMAL;
    }
    else
    {
      Serial.println("ERROR: IR command code out of range.");
    }
  }

  if (doc["protocol"])
  {
    const char *protoStr = doc["protocol"];
    decode_type_t irProtocol = strToDecodeType(protoStr);
    if (irProtocol == decode_type_t::UNKNOWN) return;

    if (doc["state"])
    {
      JsonArray stateArray = doc["state"].as<JsonArray>();
      uint16_t size = doc["size"] ? doc["size"].as<uint16_t>() : stateArray.size();
      if (size > 256) { Serial.println("Error: AC state array too large"); return; }
      uint8_t ac_state[size];
      for (int i = 0; i < size; i++) ac_state[i] = stateArray[i].as<uint8_t>();
      irsend.send(irProtocol, ac_state, size);
      indicateIRSent();
    }
    else if (doc["code"])
    {
      const char *codeStr = doc["code"];
      uint16_t bits = doc["bits"] ? doc["bits"].as<uint16_t>() : 32;
      uint64_t irCode = strtoull(codeStr, NULL, 16);
      irsend.send(irProtocol, irCode, bits);
      indicateIRSent();
    }
  }
}

void callback(char *topic, byte *payload, unsigned int length)
{
  Serial.println("\n[WiFi] Message Received:");
  Serial.write(payload, length);
  Serial.println();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) { Serial.println("JSON Parse Failed"); return; }

  processJSON(doc);
}

void setup_wifi()
{
  currentSysState = SYS_WIFI_CONN; // <--- ADD THIS LINE (Trying to connect)
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(currentSSID.c_str(), currentPassword.c_str());
  while (WiFi.status() != WL_CONNECTED)
  {
    handleButton();
    sensorLoop();     // KEEP RADAR ALIVE
    automationLoop(); // KEEP AUTOMATION ALIVE
    trackPresenceTime();
    if (isAPMode)
      return; // Escape to AP mode immediately if pressed
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  currentSysState = SYS_WIFI_OK; // <--- ADD THIS LINE (Connected!)
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

void reconnect()
{
  while (!client.connected())
  {
    handleButton();
    if (isAPMode)
      return; // Escape to AP mode immediately if pressed

    Serial.print("Connecting to MQTT...");
    // Inside reconnect() for WiFi:
    String lwtTopic = "/status/" + device_id;
    String lwtMessage = "{\"status\": \"Offline\", \"reason\": \"Connection Lost\"}";

    // Connect with LWT: (ClientID, Username, Password, LWT Topic, QoS, Retain, LWT Message)
    if (client.connect(device_id.c_str(), NULL, NULL, lwtTopic.c_str(), 1, true, lwtMessage.c_str()))
    {
      Serial.println("connected!");

      // Immediately publish an "Online" message to overwrite any previous Offline state
      String onlineMsg = "{\"status\": \"Online\", \"reason\": \"Connected\"}";
      client.publish(lwtTopic.c_str(), onlineMsg.c_str(), true); // Retained message

      client.subscribe(mqttTopic.c_str());

      // One-time boot alert: sends immediately on first connect, not buried in telemetry
      static bool bootAlertSent = false;
      if (!bootAlertSent)
      {
        JsonDocument bootDoc;
        JsonObject bd = bootDoc[device_id].to<JsonObject>();
        bd["event"] = "boot";
        bd["reset_reason"] = getResetReason();
        bd["hdc_init"] = hdcInitFailed ? "FAILED" : "OK";
        bd["radar_init"] = radarInitFailed ? "FAILED" : "OK";
        char bootBuf[256];
        serializeJson(bootDoc, bootBuf);
        client.publish(mqttTopic.c_str(), bootBuf);
        bootAlertSent = true;
        Serial.println("[BOOT] Boot alert sent via MQTT.");
      }
    }
    else
    {
      Serial.print("failed, rc=");
      Serial.println(client.state());
      smartDelay(5000); // Replaced standard delay so button stays responsive
    }
  }
}

// GSM Utility Functions
String getChipMAC()
{
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[13];
  sprintf(buf, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

String sendAT(String command, uint32_t timeoutMs = 2000)
{
  Serial.print(">> ");
  Serial.println(command);
  SerialAT.println(command);
  String response = "";
  response.reserve(256); // Pre-allocate buffer to avoid fragmentation
  uint32_t start = millis();
  while (millis() - start < timeoutMs)
  {
    while (SerialAT.available())
    {
      char c = SerialAT.read();
      response += c;
    }
    if (response.indexOf("\r\nOK\r\n") != -1 || response.indexOf("\r\nERROR\r\n") != -1 || response.indexOf(">") != -1)
      break;
  }
  Serial.print(response);
  return response;
}

void checkIncomingData()
{
  static String incoming;
  static bool initialized = false;
  if (!initialized)
  {
    incoming.reserve(256); // Allocate 256 bytes once permanently
    initialized = true;
  }

  if (SerialAT.available())
  {
    incoming = SerialAT.readStringUntil('\n');
    {
      Serial.print("\n[ASYNC] << ");
      Serial.println(incoming);
      if (incoming.indexOf("+QMTRECV:") != -1)
      {
        int jsonStart = incoming.indexOf('{');
        int jsonEnd = incoming.lastIndexOf('}');
        if (jsonStart != -1 && jsonEnd != -1 && jsonEnd > jsonStart)
        {
          String jsonStr = incoming.substring(jsonStart, jsonEnd + 1);
          JsonDocument doc;
          if (!deserializeJson(doc, jsonStr))
          {
            Serial.println("\n[GSM] Message Received.");
            processJSON(doc);
          }
          else
          {
            Serial.println("Failed to parse JSON from MQTT message.");
          }
        }
      }
    }
  }
}

String getSignalStrength()
{
  String resp = sendAT("AT+CSQ", 2000);
  int idx = resp.indexOf("+CSQ: ");
  if (idx != -1)
  {
    int commaIdx = resp.indexOf(",", idx);
    if (commaIdx != -1)
    {
      String csqVal = resp.substring(idx + 6, commaIdx);
      csqVal.trim();
      return csqVal;
    }
  }
  return "N/A";
}

String getModemTime()
{
  String resp = sendAT("AT+CCLK?", 2000);
  int idx = resp.indexOf("+CCLK: \"");
  if (idx != -1)
  {
    int endIdx = resp.indexOf("\"", idx + 8);
    if (endIdx != -1)
      return resp.substring(idx + 8, endIdx);
  }
  return "N/A";
}

String getGSMTime()
{
  String response = sendAT("AT+CCLK?", 2000);
  int first = response.indexOf('"');
  int last = response.lastIndexOf('"');
  if (first != -1 && last != -1)
  {
    String t = response.substring(first + 1, last);
    struct tm timeinfo;
    timeinfo.tm_year = t.substring(0, 2).toInt() + 100;
    timeinfo.tm_mon = t.substring(3, 5).toInt() - 1;
    timeinfo.tm_mday = t.substring(6, 8).toInt();
    timeinfo.tm_hour = t.substring(9, 11).toInt();
    timeinfo.tm_min = t.substring(12, 14).toInt();
    timeinfo.tm_sec = t.substring(15, 17).toInt();
    time_t utc_time = mktime(&timeinfo);
    utc_time += (5 * 3600);
    struct tm *local_tm = gmtime(&utc_time);
    char buffer[25];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", local_tm);
    return String(buffer);
  }
  return "1970-01-01T00:00:00";
}

// ====================================================================
// ==================== IMMEDIATE EVENT PUBLISHER =====================
// ====================================================================
void sendAutomationEvent(String eventCode)
{
  Serial.println("\n[EVENT] Sending immediate automation event: " + eventCode);

  JsonDocument doc;
  String currentId = switch_gsm_wifi ? device_id : gsmClientId;
  JsonObject data = doc[currentId].to<JsonObject>();

  // Tag it with the exact time it happened
  data["timestamp"] = switch_gsm_wifi ? getTimestamp() : getGSMTime();
  data["auto_event"] = eventCode; // "1000", "2000", or "3000"

  char buffer[256];
  serializeJson(doc, buffer);

  // Route to the active connection method
  if (switch_gsm_wifi)
  {
    if (client.connected())
    {
      if (client.publish(mqttTopic.c_str(), buffer))
        Serial.println("  -> Event sent via WiFi MQTT");
      else
      {
        Serial.println("  -> Event publish FAILED! Forcing reconnect...");
        client.disconnect();
      }
    }
  }
  else
  {
    // GSM Mode - Request publish prompt
    String cmd = "AT+QMTPUB=0,1,1,0,\"" + pubTopic + "\"";
    String pubResp = sendAT(cmd, 5000);
    if (pubResp.indexOf(">") != -1)
    {
      SerialAT.print(buffer);
      SerialAT.write(0x1A); // Ctrl+Z
      Serial.println("  -> Event sent via GSM MQTT");
    }
    else
    {
      Serial.println("  -> Failed to get GSM prompt for event.");
    }
  }
}

int batteryPercentage() { return 80; }

void sensorLoop()
{
  if (!sensorReady)
    return;

  if (sensor.check() == MyLD2410::Response::DATA)
  {
    lastRadarDataTime = millis(); // Track freshness of radar data
    bool rawPresence = sensor.presenceDetected();
    cachedMovingSignal = sensor.movingTargetSignal();
    cachedStationarySignal = sensor.stationaryTargetSignal();

    // Variance filter to ignore static noise (like swaying fans)
    if (rawPresence && cachedStationarySignal > 0)
    {
      if (cachedMovingSignal > 0)
      {
        historyIdx = 0;
        historyFull = false;
        isVarianceNoise = false;
      }
      statHistory[historyIdx++] = cachedStationarySignal;
      if (historyIdx >= VARIANCE_SAMPLES)
      {
        historyIdx = 0;
        historyFull = true;
      }

      if (historyFull)
      {
        uint8_t minSig = 255, maxSig = 0;
        for (int i = 0; i < VARIANCE_SAMPLES; i++)
        {
          if (statHistory[i] < minSig)
            minSig = statHistory[i];
          if (statHistory[i] > maxSig)
            maxSig = statHistory[i];
        }
        isVarianceNoise = ((maxSig - minSig) <= VARIANCE_THRESHOLD);
      }
    }
    else if (!rawPresence)
    {
      historyFull = false;
      historyIdx = 0;
      isVarianceNoise = false;
    }

    cachedPresence = isVarianceNoise ? false : rawPresence;

    // ---> NEW: Only turn on LED 2 if automation is ENABLED and a human is PRESENT <---
    if (radarAutoMode && cachedPresence)
    {
      digitalWrite(LED_PIN, HIGH);
    }
    else
    {
      digitalWrite(LED_PIN, LOW);
    }
  }
}

// Sends a small alert JSON to whichever transport is active (WiFi or GSM).
void publishHealthAlert(const char *event, const char *detail)
{
  JsonDocument doc;
  String id = switch_gsm_wifi ? device_id : gsmClientId;
  JsonObject obj = doc[id].to<JsonObject>();
  obj["event"] = event;
  obj["detail"] = detail;
  char buf[192];
  serializeJson(doc, buf);

  if (switch_gsm_wifi)
  {
    if (client.connected())
      client.publish(mqttTopic.c_str(), buf);
  }
  else
  {
    String cmd = "AT+QMTPUB=0,1,1,0,\"" + pubTopic + "\"";
    String resp = sendAT(cmd, 5000);
    if (resp.indexOf(">") != -1)
    {
      SerialAT.print(buf);
      SerialAT.write(0x1A);
    }
  }
  Serial.printf("[HEALTH ALERT] %s — %s\n", event, detail);
}

// Called from loop() every HEALTH_INTERVAL ms. Checks radar freshness and heap.
void healthLoop()
{
  static unsigned long lastHealthCheck = 0;
  const unsigned long HEALTH_INTERVAL = 30000; // check every 30 seconds
  const uint32_t HEAP_WARN_BYTES = 12000;      // alert below 12 KB free

  if (millis() - lastHealthCheck < HEALTH_INTERVAL)
    return;
  lastHealthCheck = millis();

  // --- 1. Radar staleness check ---
  if (sensorReady && lastRadarDataTime > 0 &&
      (millis() - lastRadarDataTime > RADAR_STALE_MS))
  {
    if (!radarStaleAlerted)
    {
      Serial.println("[HEALTH] Radar data is stale — attempting re-init...");
      sensorReady = false; // Stop automation acting on frozen presence data
      cachedPresence = false;

      // Try to recover the radar
      sensorSerial.end();
      delay(200);
      sensorSerial.begin(256000, SERIAL_8N1, RX_PIN, TX_PIN);
      
      Serial.println("[HEALTH] Waiting for radar UART heartbeat...");
      unsigned long startWait = millis();
      bool radarAlive = false;

      // Active Listening: Wait up to 5s for the radar to wake back up
      while (millis() - startWait < 5000)
      {
        esp_task_wdt_reset(); 
        handleButton(); // <--- Keep the physical button responsive during recovery
        
        if (sensorSerial.available())
        {
          radarAlive = true;
          break;
        }
        delay(10);
      }

      if (radarAlive)
      {
        while(sensorSerial.available()) sensorSerial.read(); // Clear garbage bytes
        
        if (sensor.begin())
        {
          sensor.enhancedMode();
          sensorReady = true;
          radarStaleAlerted = false;
          lastRadarDataTime = millis();
          publishHealthAlert("radar_recovered", "Re-init succeeded");
          Serial.println("[HEALTH] Radar re-init SUCCESS.");
        }
        else
        {
          radarStaleAlerted = true;
          publishHealthAlert("radar_stale", "UART active, protocol failed");
          Serial.println("[HEALTH] Radar re-init FAILED (Protocol error).");
        }
      }
      else
      {
        radarStaleAlerted = true;
        publishHealthAlert("radar_dead", "No UART heartbeat after 5s");
        Serial.println("[HEALTH] Radar re-init FAILED (No hardware response).");
        
        // OPTIONAL: If you want the ESP32 to completely reboot itself when 
        // the radar dies during runtime, you can uncomment the line below.
        // ESP.restart(); 
      }
    }
  }
  else if (sensorReady)
  {
    radarStaleAlerted = false; // Clear alert once data is flowing again
  }

  // --- 2. Heap check ---
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < HEAP_WARN_BYTES)
  {
    char detail[48];
    snprintf(detail, sizeof(detail), "free_heap=%u bytes", freeHeap);
    publishHealthAlert("low_heap", detail);
    Serial.printf("[HEALTH] Low heap warning: %u bytes free\n", freeHeap);
  }
}

// LD2410 auto-threshold calibration — room must be empty.
// Blocks until the sensor reports success/fail/timeout (up to 120s).
void calibrateRadarAuto()
{
  if (!sensorReady)
  {
    server.send(503, "text/plain", "Error: Radar not ready. Check connection.");
    return;
  }

  sensorReady = false; // Pause automation so stale data doesn't trigger AC commands

  Serial.println("[CAL] Starting auto-threshold calibration...");
  if (!sensor.autoThresholds(10)) // 10s grace period for user to leave room
  {
    sensorReady = true;
    sensor.enhancedMode();
    indicateError();
    server.send(500, "text/plain", "Error: Calibration command rejected. Is radar firmware >= 2.44?");
    return;
  }

  // Poll for result — status 4 = in progress, 5 = success, 6 = failed
  unsigned long startTime = millis();
  bool done = false;
  byte finalStatus = 0;
  while (millis() - startTime < 120000)
  {
    esp_task_wdt_reset();
    sensor.check();
    finalStatus = sensor.getStatus();
    if (finalStatus == 5 || finalStatus == 6)
    {
      done = true;
      break;
    }
    delay(200);
  }

  sensor.enhancedMode(); // Return to streaming mode regardless of result
  sensorReady       = true;
  lastRadarDataTime = millis(); // Reset staleness timer

  if (done && finalStatus == 5)
  {
    indicateSuccess();
    server.send(200, "text/plain", "Calibration SUCCESSFUL! Thresholds saved to sensor.");
    Serial.println("[CAL] Auto-calibration SUCCESS.");
  }
  else if (done && finalStatus == 6)
  {
    indicateError();
    server.send(200, "text/plain", "Calibration FAILED. Was the room completely empty?");
    Serial.println("[CAL] Auto-calibration FAILED.");
  }
  else
  {
    indicateError();
    server.send(408, "text/plain", "Calibration TIMED OUT after 120s. Try again.");
    Serial.println("[CAL] Auto-calibration TIMEOUT.");
  }
}

// LD2410 factory reset — wipes all learned thresholds back to HLK defaults.
void calibrateRadarReset()
{
  if (!sensorReady)
  {
    server.send(503, "text/plain", "Error: Radar not ready.");
    return;
  }

  sensorReady = false;
  Serial.println("[CAL] Resetting radar to factory defaults...");

  if (!sensor.requestReset())
  {
    sensorReady = true;
    indicateError();
    server.send(500, "text/plain", "Error: Reset command rejected.");
    return;
  }

  sensor.requestReboot(); // Sensor applies the reset on reboot
  delay(2500);            // Wait for sensor to restart

  esp_task_wdt_reset();

  if (sensor.begin())
  {
    sensor.enhancedMode();
    sensorReady       = true;
    lastRadarDataTime = millis();
    indicateSuccess();
    server.send(200, "text/plain", "Factory reset done. Sensor reinitialized with default thresholds.");
    Serial.println("[CAL] Radar factory reset SUCCESS.");
  }
  else
  {
    indicateError();
    server.send(500, "text/plain", "Reset done but reinit failed. Reboot the device.");
    Serial.println("[CAL] Radar reinit after reset FAILED.");
  }
}

void automationLoop()
{
  if (!radarAutoMode || !sensorReady)
    return;

  if (cachedPresence)
  {
    lastPresenceTime = millis();

    // If someone enters and it's not in normal mode, turn it ON to 24°C
    if (acAutoState != AUTO_ON_NORMAL)
    {
      String customKey = "ir_" + String(currentNormalTemp);

      // Try to play custom temp button (e.g., ir_26), fallback to universal IR code
      if (!playCustomButton(customKey.c_str()))
      {
        sendACFallback(true, currentNormalTemp);
      }
      acAutoState = AUTO_ON_NORMAL;
      indicateIRSent();
      Serial.println("[AUTO] Presence detected! AC turned ON (Normal Mode).");

      // NEW: Send Immediate "Turn ON" Event
      sendAutomationEvent("1000");
    }
  }
  else
  {
    unsigned long emptyDuration = millis() - lastPresenceTime;

    // Check ECO condition FIRST to ensure it doesn't get skipped if times overlap
    if (acAutoState == AUTO_ON_NORMAL && emptyDuration >= TEcoTime && emptyDuration < TOffTime)
    {
      String customKey = "ir_" + String(TEcoTemp);

      // Try to play custom Eco temp button, fallback to universal IR code
      if (!playCustomButton(customKey.c_str()))
      {
        sendACFallback(true, TEcoTemp);
      }
      acAutoState = AUTO_ON_ECO;
      indicateIRSent();
      Serial.printf("[AUTO] Room empty for TEcoTime! AC set to Eco Temp: %d°C\n", TEcoTemp);

      // NEW: Send Immediate "ECO" Event
      sendAutomationEvent("2000");
    }
    // Check OFF condition NEXT (Longest duration)
    else if (acAutoState != AUTO_OFF && emptyDuration >= TOffTime)
    {
      if (!playCustomButton("ir_off"))
        sendACFallback(false, 24);
      acAutoState = AUTO_OFF;
      indicateIRSent();
      Serial.println("[AUTO] Room empty for TOffTime! AC turned OFF.");

      // NEW: Send Immediate "Turn OFF" Event
      sendAutomationEvent("3000");
    }
  }
}

void trackPresenceTime()
{
  if (!sensorReady)
    return;

  unsigned long now = millis();
  if (cachedPresence != lastPresenceState)
  {
    // If the state just changed, and it WAS occupied, add the duration to our bucket
    if (lastPresenceState == true)
    {
      accumulatedPresenceMs += (now - lastStateChangeTime);
    }
    lastPresenceState = cachedPresence;
    lastStateChangeTime = now;
  }
}

// ====================================================================
// ========================= LOCAL SCHEDULER ==========================
// ====================================================================

// Set system POSIX clock from GSM modem AT+CCLK (enables getLocalTime in GSM mode).
void setSystemTimeFromGSM()
{
  String response = sendAT("AT+CCLK?", 2000);
  int first = response.indexOf('"');
  int last  = response.lastIndexOf('"');
  if (first == -1 || last == -1) return;
  String t = response.substring(first + 1, last);
  if (t.length() < 17) return;
  struct tm timeinfo = {};
  timeinfo.tm_year  = t.substring(0, 2).toInt() + 100;
  timeinfo.tm_mon   = t.substring(3, 5).toInt() - 1;
  timeinfo.tm_mday  = t.substring(6, 8).toInt();
  timeinfo.tm_hour  = t.substring(9, 11).toInt();
  timeinfo.tm_min   = t.substring(12, 14).toInt();
  timeinfo.tm_sec   = t.substring(15, 17).toInt();
  time_t utc        = mktime(&timeinfo);
  utc              += (gmtOffset_sec); // apply same offset as NTP path
  struct timeval tv = {utc, 0};
  settimeofday(&tv, nullptr);
  Serial.println("[SCHED] System clock synced from GSM modem.");
}

// "monday" -> 1, "sunday" -> 0, etc.  Returns -1 on unknown input.
int dayNameToWday(const String &day)
{
  String d = day;
  d.toLowerCase();
  if (d == "sunday")    return 0;
  if (d == "monday")    return 1;
  if (d == "tuesday")   return 2;
  if (d == "wednesday") return 3;
  if (d == "thursday")  return 4;
  if (d == "friday")    return 5;
  if (d == "saturday")  return 6;
  return -1;
}

// Persist one day's schedule to NVS.
void saveSchedule(int wday, const uint8_t slots[24],
                  uint8_t radarSetting, const String &irHex, int irTemp)
{
  char key[13];
  snprintf(key, sizeof(key), "sch_h_%d", wday);
  preferences.putBytes(key, slots, 24);

  snprintf(key, sizeof(key), "sch_r_%d", wday);
  preferences.putUChar(key, radarSetting); // 0=unset, 1=enable, 2=disable

  snprintf(key, sizeof(key), "sch_ir_%d", wday);
  preferences.putString(key, irHex);

  snprintf(key, sizeof(key), "sch_irt_%d", wday);
  preferences.putInt(key, irTemp);
}

// Send the schedule-specific raw IR code for this day if temp matches.
// Returns false if not stored or temp doesn't match — caller falls through to normal IR path.
bool sendScheduleIR(int wday, int targetTemp)
{
  char key[13];
  snprintf(key, sizeof(key), "sch_irt_%d", wday);
  int storedTemp = preferences.getInt(key, -1);
  if (storedTemp != targetTemp) return false;

  snprintf(key, sizeof(key), "sch_ir_%d", wday);
  String irHex = preferences.getString(key, "");
  if (irHex.length() == 0) return false;

  String proto = preferences.getString("protocol_name", "");
  if (proto.length() == 0) return false;

  decode_type_t protocol = strToDecodeType(proto.c_str());
  if (protocol == decode_type_t::UNKNOWN) return false;

  uint64_t code = strtoull(irHex.c_str(), NULL, 16);
  irsend.send(protocol, code, 32);
  Serial.printf("[SCHED] Schedule IR sent: 0x%s at %d°C\n", irHex.c_str(), targetTemp);
  return true;
}

// Apply today's schedule radar setting — skipped entirely if manual override is active.
void applyScheduleRadar(int wday)
{
  if (radarManualOverride) return;

  char key[13];
  snprintf(key, sizeof(key), "sch_r_%d", wday);
  uint8_t setting = preferences.getUChar(key, 0);

  if (setting == 1 && !radarAutoMode)
  {
    radarAutoMode = true;
    lastPresenceTime = millis();
    preferences.putBool("radar_auto", true);
    Serial.println("[SCHED] Schedule enabled radar automation.");
  }
  else if (setting == 2 && radarAutoMode)
  {
    radarAutoMode = false;
    preferences.putBool("radar_auto", false);
    Serial.println("[SCHED] Schedule disabled radar automation.");
  }
}

// Parse and store a schedule command received via MQTT.
void handleScheduleCommand(JsonDocument &doc)
{
  const char *day = doc["day"];
  if (!day)
  {
    Serial.println("[SCHED] Missing 'day' field, ignoring.");
    return;
  }

  int wday = dayNameToWday(String(day));
  if (wday < 0)
  {
    Serial.printf("[SCHED] Unknown day '%s', ignoring.\n", day);
    return;
  }

  JsonArray hoursArray = doc["hours"].as<JsonArray>();
  if (!hoursArray || hoursArray.size() != 24)
  {
    Serial.println("[SCHED] 'hours' must be an array of exactly 24 elements, ignoring.");
    return;
  }

  uint8_t slots[24];
  for (int i = 0; i < 24; i++)
  {
    JsonVariant v = hoursArray[i];
    if (v.is<const char *>())
    {
      slots[i] = 0; // "off" string -> 0
    }
    else
    {
      int t = v.as<int>();
      slots[i] = (t >= 16 && t <= 32) ? (uint8_t)t : 0; // clamp out-of-range to off
    }
  }

  // Parse radar: accept "0100"/"256" (enable), "0200"/"512"/"off" (disable), absent (unset).
  uint8_t radarSetting = 0;
  if (doc["radar"])
  {
    String r = doc["radar"].as<String>();
    if      (r == "0100" || r == "256")               radarSetting = 1;
    else if (r == "0200" || r == "512" || r == "off") radarSetting = 2;
  }

  String irHex  = doc["ir"]                  | "";
  int    irTemp = doc["temperature_setting"] | 0;

  saveSchedule(wday, slots, radarSetting, irHex, irTemp);

  // New schedule is authoritative — clear any manual override and apply radar immediately.
  radarManualOverride = false;
  preferences.putBool("rad_ovr", false);

  if (radarSetting == 1)
  {
    radarAutoMode = true;
    lastPresenceTime = millis();
    preferences.putBool("radar_auto", true);
    Serial.println("[SCHED] Schedule applied: radar ENABLED.");
  }
  else if (radarSetting == 2)
  {
    radarAutoMode = false;
    preferences.putBool("radar_auto", false);
    Serial.println("[SCHED] Schedule applied: radar DISABLED.");
  }

  // Update global automation params if supplied (same as normal command path).
  if (doc["temperature_setting"])
  {
    currentNormalTemp = doc["temperature_setting"].as<int>();
    preferences.putInt("normal_temp", currentNormalTemp);
  }
  if (doc["eco"])
  {
    TEcoTemp = doc["eco"].as<int>();
    preferences.putInt("eco_temp", TEcoTemp);
  }
  if (doc["teco"])
  {
    TEcoTime = doc["teco"].as<unsigned long>() * 60000;
    preferences.putULong("eco_time", TEcoTime);
  }
  if (doc["toff"])
  {
    TOffTime = doc["toff"].as<unsigned long>() * 60000;
    if (TOffTime <= TEcoTime)
    {
      TOffTime = TEcoTime + 60000;
      Serial.println("[SCHED] WARNING: TOffTime <= TEcoTime, auto-corrected.");
    }
    preferences.putULong("off_time", TOffTime);
  }

  Serial.printf("[SCHED] Schedule saved for %s (wday=%d). Radar setting: %d\n", day, wday, radarSetting);
  indicateSuccess();
}

// Called every loop iteration. Fires the scheduled slot once per hour change.
void scheduleLoop()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return; // No valid time yet — skip silently.

  int currentHour = timeinfo.tm_hour;
  int currentWday = timeinfo.tm_wday; // 0=Sun, 1=Mon, ..., 6=Sat

  if (currentHour == lastScheduledHour) return; // Already fired this hour.
  lastScheduledHour = currentHour;

  char key[13];
  snprintf(key, sizeof(key), "sch_h_%d", currentWday);
  if (preferences.getBytesLength(key) != 24) return; // No schedule saved for today.

  uint8_t slots[24];
  preferences.getBytes(key, slots, 24);
  uint8_t slotTemp = slots[currentHour];

  applyScheduleRadar(currentWday); // Radar first — before the IR command.

  if (slotTemp == 0)
  {
    Serial.printf("[SCHED] %02d:00 -> scheduled OFF\n", currentHour);
    if (!playCustomButton("ir_off"))
      sendACFallback(false, 24);
    acAutoState = AUTO_OFF;
    indicateIRSent();
  }
  else
  {
    int targetTemp = (int)slotTemp;
    currentNormalTemp = targetTemp;
    preferences.putInt("normal_temp", currentNormalTemp);

    Serial.printf("[SCHED] %02d:00 -> scheduled %d°C\n", currentHour, targetTemp);

    if (!sendScheduleIR(currentWday, targetTemp))
    {
      String customKey = "ir_" + String(targetTemp);
      if (!playCustomButton(customKey.c_str()))
        sendACFallback(true, targetTemp);
    }
    acAutoState = AUTO_ON_NORMAL;
    indicateIRSent();
  }
}

// ====================================================================
// ========================= MAIN SETUP ===============================
// ====================================================================

void setup()
{
  Serial.begin(115200);

  // Hardware watchdog: 120s covers worst-case GSM modem boot + registration.
  // Any blocking hang longer than this triggers an automatic reboot.
  esp_task_wdt_init(120, true);
  esp_task_wdt_add(NULL);

  preferences.begin("ir_data", false);

  currentSSID = preferences.getString("wifi_ssid", "");
  currentPassword = preferences.getString("wifi_pass", "");

  // ---> NEW: Load Automation Parameters from Flash <---
  // The second argument is the default value if the key doesn't exist yet
  TEcoTemp = preferences.getInt("eco_temp", 26);
  TEcoTime = preferences.getULong("eco_time", 120000);
  TOffTime = preferences.getULong("off_time", 300000);
  currentNormalTemp = preferences.getInt("normal_temp", 24);

  radarAutoMode       = preferences.getBool("radar_auto", false);
  radarManualOverride = preferences.getBool("rad_ovr",   false);
  radarManualValue    = preferences.getBool("rad_ovr_v", false);
  lastScheduledHour   = -1;    // force scheduleLoop() to fire on first valid tick after boot
  lastPresenceTime    = millis(); // defensive: prevents emptyDuration from being huge if acAutoState guard is ever removed

  Serial.printf("\n[BOOT] Loaded Automation Settings:\n - Eco Temp: %d°C\n - Eco Time: %lu ms\n - Off Time: %lu ms\n", TEcoTemp, TEcoTime, TOffTime);

  // ---> NEW: Load Mode Preference (DefSault to true/WiFi if not set) <---
  switch_gsm_wifi = preferences.getBool("use_wifi", true);

  // ADD THIS DEBUG OUTPUT
  Serial.println("\n=== STORED CREDENTIALS ===");
  Serial.println("SSID: '" + currentSSID + "'");
  Serial.println("SSID Length: " + String(currentSSID.length()));
  Serial.println("Pass Length: " + String(currentPassword.length()));

  // Initialize unified hardware configurations
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT); // <--- ADD THIS LINE
  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);
  currentSysState = SYS_BOOTING; // Start with solid white
  pinMode(MOSFET_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(MOSFET_PIN, LOW);

  Wire.begin();
  Wire.setTimeOut(150); // <--- ADD THIS LINE to prevent permanent I2C hangs

  if (!hdc.begin(0x40))
  {
    hdcInitFailed = true;
    Serial.println("Warning: HDC1080 sensor not found!");
  }
  else
  {
    Serial.println("HDC1080 sensor initialized.");
  }

  // Initialize Radar
  sensorSerial.begin(256000, SERIAL_8N1, RX_PIN, TX_PIN);
  Serial.print("Waiting for LD2410 UART heartbeat (up to 5s)");
  
  unsigned long startWait = millis();
  bool radarAlive = false;

  // Listen for actual data on the RX line
  while (millis() - startWait < 5000)
  {
    esp_task_wdt_reset(); // Keep watchdog happy during the wait
    
    if (sensorSerial.available())
    {
      radarAlive = true;
      break;
    }
    
    if ((millis() - startWait) % 500 == 0) Serial.print("."); // Print dots every 500ms
    delay(10); 
  }
  Serial.println();

  if (radarAlive)
  {
    // Drain any garbage bytes from the hardware boot sequence
    while(sensorSerial.available()) sensorSerial.read(); 
    
    if (sensor.begin())
    {
      sensorReady = true;
      sensor.enhancedMode();
      Serial.println("✓ LD2410 Radar initialized successfully.");
    }
    else
    {
      radarInitFailed = true;
      Serial.println("⚠ Warning: Radar UART active, but begin() sequence failed.");
    }
  }
  else
  {
    Serial.println("✗ FATAL ERROR: No UART data from Radar after 5 seconds!");
    Serial.println("Rebooting ESP32 to attempt hardware recovery...");
    delay(1000);
    ESP.restart();
  }

  // ac.begin();
  // ac.on();
  // ac.setFan(kElectraAcFanAuto);
  // ac.setMode(kElectraAcCool);
  irsend.begin();

  // Allow a very brief window during boot to enter AP mode if the button is held
  unsigned long bootStart = millis();
  while (millis() - bootStart < 500)
  {
    handleButton();
  }

  // ---> NEW: CREDENTIAL CHECK & LED INDICATION <---
  if (!isAPMode && switch_gsm_wifi)
  {
    if (currentSSID.length() > 0)
    {
      Serial.println("WiFi Credentials Found in Memory.");
      indicateSuccess(); // 2 Quick Green Flashes
    }
    else
    {
      Serial.println("No WiFi Credentials Found! Forcing AP Mode.");

      // Flash Yellow 3 times to indicate missing credentials
      ledOff();
      for (int i = 0; i < 3; i++)
      {
        setColor(1, 1, 0); // Yellow (Red + Green)
        delay(150);
        ledOff();
        delay(150);
      }

      // Automatically start the Hotspot so the user can enter credentials
      startAPMode();
    }
  }

  // Execute normal setups if we haven't been forced into AP mode
  if (!isAPMode)
  {
    if (switch_gsm_wifi)
    {
      macAddress = WiFi.macAddress();
      macAddress.replace(":", "");
      device_id = macAddress;
      mqttTopic = "/topic/" + macAddress;
      Serial.println("Device ID: " + device_id);

      setup_wifi();

      if (WiFi.status() == WL_CONNECTED)
      {
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        struct tm timeinfo;
        while (!getLocalTime(&timeinfo))
        {
          delay(500);
        }
      }

      client.setBufferSize(512); // <--- ADD THIS LINE to prevent silent packet drops
      client.setServer(mqtt_server, mqtt_port);
      client.setCallback(callback);
    }
    else
    {
      macAddress_gsm = getChipMAC();
      gsmClientId = macAddress_gsm;
      pubTopic = "/topic/" + macAddress_gsm;
      subTopic = "/topic/" + macAddress_gsm;
      Serial.print("GSM Client ID: ");
      Serial.println(gsmClientId);
      Serial.print("Pub Topic: ");
      Serial.println(pubTopic);
      Serial.print("Sub Topic: ");
      Serial.println(subTopic);
      SerialAT.begin(115200, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);

      int connFailCount = 0;
      const int MAX_CONN_RETRIES = 3;
      bool mqttConnected = false;
      while (!mqttConnected && connFailCount < MAX_CONN_RETRIES)
      {
        esp_task_wdt_reset(); // Keep watchdog alive during multi-minute GSM setup
        handleButton();
        if (isAPMode)
          break; // Break out if button pressed

        // ----- STEP 1: Basic Modem Check -----
        currentSysState = SYS_GSM_CONN; // Trying to connect
        Serial.println("\n[STEP 1] Checking Modem...");
        sendAT("AT", 1000);
        sendAT("ATE0", 1000);
        sendAT("AT+CGMI", 2000);

        // ----- STEP 2: Check SIM (Continuous Hot-Swap Logic) -----
        Serial.println("\n[STEP 2] Checking SIM...");
        bool simReady = false;

        while (!simReady)
        {
          handleButton();
          if (isAPMode)
            break; // Allow user to escape to AP mode using the physical button

          String simResp = sendAT("AT+CPIN?", 3000);

          if (simResp.indexOf("READY") != -1)
          {
            simReady = true;
            Serial.println("✓ SIM is inserted and ready!");
          }
          else
          {
            Serial.println("⚠ SIM not inserted or not ready!");
            Serial.println("Waiting 10 seconds for user to insert SIM...");
            smartDelay(10000); // Wait exactly 10 seconds, keeping the button responsive

            Serial.println("\nRestarting modem radio to scan the physical SIM slot...");
            sendAT("AT+CFUN=0", 5000); // Turn radio off
            smartDelay(3000);
            sendAT("AT+CFUN=1", 5000); // Turn radio on (forces hardware re-read)
            smartDelay(8000);          // Give the modem time to boot its internal OS
          }
        }

        if (isAPMode)
          break; // Failsafe exit if the loop was broken by the button

        // --- NEW DIAGNOSTICS ---
        Serial.println("\n[DIAGNOSTICS] Checking Signal and Radio...");
        sendAT("AT+CFUN=1", 3000); // Force Radio ON
        smartDelay(2000);
        sendAT("AT+CSQ", 2000);   // Check Antenna Signal (0-31 is good, 99 is blind)
        sendAT("AT+COPS?", 2000); // Check if it sees any network operators
        // -----------------------

        // ----- STEP 3: Wait for Network Registration -----
        Serial.println("\n[STEP 3] Waiting for Network Registration...");
        bool registered = false;
        for (int i = 0; i < 30; i++)
        {
          handleButton();
          if (isAPMode)
            break;
          String regResp = sendAT("AT+CREG?", 2000);
          if (regResp.indexOf(",1") != -1 || regResp.indexOf(",5") != -1)
          {
            registered = true;
            Serial.println("✓ Network registered!");
            break;
          }
          Serial.println("  ...still searching...");
          smartDelay(2000);
        }

        if (!registered)
        {
          Serial.println("✗ Network registration failed!");
          connFailCount++;
          Serial.print("Failure count: ");
          Serial.print(connFailCount);
          Serial.print("/");
          Serial.println(MAX_CONN_RETRIES);
          if (connFailCount >= MAX_CONN_RETRIES)
          {
            Serial.println("\nMAX RETRIES — Restarting modem via AT+CFUN...");
            sendAT("AT+CFUN=0", 5000);
            delay(3000);
            sendAT("AT+CFUN=1", 5000);
            delay(10000);
            connFailCount = 0;
            Serial.println("Modem restarted. Retrying...\n");
          }
          continue;
        }

        // ----- STEP 4: Activate PDP Context -----
        Serial.println("\n[STEP 4] Activating PDP Context...");
        sendAT("AT+QIACT=1", 10000);
        smartDelay(2000);
        sendAT("AT+QIACT?", 3000);

        // ----- STEP 5: Open MQTT Connection -----
        Serial.println("\n[STEP 5] Opening MQTT Connection...");
        sendAT("AT+QMTCLOSE=0", 3000);
        delay(1000);

        // GSM LWT — must use a quote-free will message; Quectel AT parsers do not support
        // backslash escaping inside string parameters, so JSON with internal quotes is rejected.
        String lwtCmd = "AT+QMTCFG=\"will\",0,1,1,1,\"/status/" + gsmClientId + "\",\"Offline\"";
        sendAT(lwtCmd, 2000);

        String cmd = "AT+QMTOPEN=0,\"" + String(mqtt_server) + "\"," + String(mqtt_port);
        sendAT(cmd, 5000);

        Serial.println("  Waiting for +QMTOPEN URC...");
        bool openSuccess = false;
        uint32_t waitStart = millis();
        String urcBuffer = "";
        while (millis() - waitStart < 15000)
        {
          while (SerialAT.available())
          {
            char c = SerialAT.read();
            urcBuffer += c;
            Serial.print(c);
          }
          if (urcBuffer.indexOf("+QMTOPEN: 0,0") != -1)
          {
            openSuccess = true;
            Serial.println("\n✓ MQTT TCP connection opened!");
            break;
          }
          if (urcBuffer.indexOf("+QMTOPEN: 0,-1") != -1 ||
              urcBuffer.indexOf("ERROR") != -1)
          {
            Serial.println("\n✗ MQTT TCP connection failed!");
            break;
          }
          delay(100);
        }

        if (!openSuccess)
        {
          connFailCount++;
          if (connFailCount >= MAX_CONN_RETRIES)
          {
            Serial.println("\n⚠⚠⚠ MAX RETRIES — Restarting modem...");
            sendAT("AT+CFUN=0", 5000);
            delay(3000);
            sendAT("AT+CFUN=1", 5000);
            delay(10000);
            connFailCount = 0;
          }
          continue;
        }

        delay(2000);

        // ----- STEP 6: MQTT CONNECT -----
        Serial.println("\n[STEP 6] Logging into MQTT Broker...");
        cmd = "AT+QMTCONN=0,\"" + gsmClientId + "\"";
        String connResp = sendAT(cmd, 5000);

        String connURC = "";
        waitStart = millis();
        while (millis() - waitStart < 10000)
        {
          while (SerialAT.available())
          {
            char c = SerialAT.read();
            connURC += c;
            Serial.print(c);
          }
          if (connURC.indexOf("+QMTCONN: 0,0,0") != -1)
          {
            mqttConnected = true;
            Serial.println("\n✓ MQTT broker connected!");
            break;
          }
          if (connResp.indexOf("+CME ERROR") != -1 ||
              connURC.indexOf("+CME ERROR") != -1)
          {
            Serial.println("\n✗ +CME ERROR detected!");
            break;
          }
          if (connURC.indexOf("ERROR") != -1 ||
              connURC.indexOf("+QMTCONN: 0,") != -1)
          {
            Serial.println("\n✗ MQTT connect failed!");
            break;
          }
          delay(100);
        }

        if (!mqttConnected)
        {
          connFailCount++;
          if (connFailCount >= MAX_CONN_RETRIES)
          {
            Serial.println("\n⚠⚠⚠ 3 FAILURES — RESTARTING MODEM ⚠⚠⚠");
            sendAT("AT+QMTCLOSE=0", 3000);
            delay(1000);
            sendAT("AT+CFUN=0", 5000);
            delay(3000);
            sendAT("AT+CFUN=1", 5000);
            delay(10000);
            connFailCount = 0;
          }
          else
          {
            delay(3000);
          }
        }
      } // end while

      if (mqttConnected)
      {
        delay(2000);
        Serial.println("\n[STEP 7] Subscribing to Topic...");
        String cmd = "AT+QMTSUB=0,1,\"" + subTopic + "\",0";
        sendAT(cmd, 5000);
        uint32_t waitStart = millis();
        while (millis() - waitStart < 5000)
        {
          checkIncomingData();
          delay(100);
        }
        Serial.println("\n============= SETUP COMPLETE =============");
        Serial.println("✓ MQTT connected & subscribed!");
        currentSysState = SYS_GSM_OK;
        setSystemTimeFromGSM(); // sync POSIX clock so scheduleLoop() works in GSM mode

        // One-time GSM boot alert — mirrors the WiFi reconnect() boot alert
        JsonDocument bootDoc;
        JsonObject bd = bootDoc[gsmClientId].to<JsonObject>();
        bd["event"] = "boot";
        bd["reset_reason"] = getResetReason();
        bd["hdc_init"] = hdcInitFailed ? "FAILED" : "OK";
        bd["radar_init"] = radarInitFailed ? "FAILED" : "OK";
        char bootBuf[256];
        serializeJson(bootDoc, bootBuf);
        String bootCmd = "AT+QMTPUB=0,1,1,0,\"" + pubTopic + "\"";
        String bootResp = sendAT(bootCmd, 5000);
        if (bootResp.indexOf(">") != -1)
        {
          SerialAT.print(bootBuf);
          SerialAT.write(0x1A);
          Serial.println("[BOOT] GSM boot alert sent.");
        }
      }
      else
      {
        Serial.println("\n✗✗✗ FAILED to establish MQTT ✗✗✗");
      }
    }
  }
}

// ========================= MAIN LOOP ================================
void loop()
{
  esp_task_wdt_reset(); // Kick the hardware watchdog every loop iteration

  // Initialize the timer on the very first loop iteration
  static bool trackerInitialized = false;
  if (!trackerInitialized)
  {
    lastStateChangeTime = millis();
    trackerInitialized = true;
  }

  static bool bootReasonReported = false;

  // 1. ALWAYS check for a pending reboot first!
  if (pendingReboot && millis() > rebootTime)
  {
    Serial.println("Rebooting now...");
    ESP.restart();
  }

  handleButton();
  updateLED();

  // 2. Call the radar + health functions continuously
  sensorLoop();
  automationLoop();
  trackPresenceTime();
  healthLoop();
  scheduleLoop();

  // 3. DASHBOARD MODE (Web Server takes over entirely)
  if (isAPMode)
  {
    server.handleClient();
    return; // Bypass normal loop functions
  }

  // 4. NORMAL OPERATION MODE
  if (switch_gsm_wifi)
  {
    // --- WiFi / MQTT Routine ---
    if (WiFi.status() != WL_CONNECTED)
    {
      currentSysState = SYS_WIFI_CONN; // <--- ADD THIS LINE (Lost connection)
      Serial.println("\nWiFi connection lost! Reconnecting...");
      WiFi.disconnect();
      WiFi.begin(currentSSID.c_str(), currentPassword.c_str());

      int abc = 0;
      while (WiFi.status() != WL_CONNECTED)
      {
        esp_task_wdt_reset();
        handleButton();
        sensorLoop();     // KEEP RADAR ALIVE
        automationLoop(); // KEEP AUTOMATION ALIVE
        trackPresenceTime();
        if (isAPMode)
          return; // Exit loop immediately if button pressed

        delay(500);
        Serial.print(".");
        if (abc++ > 20)
        {
          WiFi.disconnect();
          WiFi.begin(currentSSID.c_str(), currentPassword.c_str());
          smartDelay(2000); // Replaced standard delay
          abc = 0;
        }
      }
      Serial.println("\nWiFi Reconnected!");
      currentSysState = SYS_WIFI_OK;
    }

    if (!client.connected())
      reconnect();
    client.loop();

    if (millis() - lastTelemetry > TELEMETRY_INTERVAL)
    {
      // ===== CALCULATE PRESENCE MINUTES =====
      unsigned long now = millis();
      lastTelemetry = now; // <--- ADD THIS LINE HERE!

      // If currently occupied, add the running time to the bucket before calculating
      if (lastPresenceState == true)
      {
        accumulatedPresenceMs += (now - lastStateChangeTime);
      }

      // Convert accumulated milliseconds to rounded seconds (FOR TESTING)
      int total_interval_secs = round(TELEMETRY_INTERVAL / 1000.0);
      int presence_secs = round(accumulatedPresenceMs / 1000.0);

      if (presence_secs > total_interval_secs)
        presence_secs = total_interval_secs;
      int empty_secs = total_interval_secs - presence_secs;

      // Reset the tracking bucket for the next 15-minute window
      accumulatedPresenceMs = 0;
      lastStateChangeTime = now;
      // ======================================
      float temperature = hdc.readTemperature();
      float humidity = hdc.readHumidity();
      int rssi = WiFi.RSSI();

      JsonDocument doc;
      JsonObject mac = doc[device_id].to<JsonObject>();
      mac["timestamp"] = getTimestamp();
      // ---> APPEND RADAR DATA TO JSON <---
      mac["radar_auto_mode"] = radarAutoMode ? "Enabled" : "Disabled";
      if (radarAutoMode)
      {
        mac["presence_seconds"] = presence_secs;
        mac["empty_seconds"] = empty_secs;
      }
      // ---> ADD THE THREE NEW PARAMETERS <---
      mac["TEcoTemp"] = TEcoTemp;
      mac["TEcoTime"] = TEcoTime / 60000; // Send as minutes
      mac["TOffTime"] = TOffTime / 60000; // Send as minutes
      mac["temperature"] = temperature;
      mac["humidity"] = humidity;
      mac["status"] = "Online";
      // Only send the reset reason on the very first telemetry ping
      if (!bootReasonReported)
      {
        mac["last_reset_reason"] = getResetReason();
        bootReasonReported = true;
      }
      mac["device_type"] = "climate_sensor";
      mac["battery_level"] = batteryPercentage();
      mac["wifi_signal_strength"] = rssi;
      mac["uptime_s"] = millis() / 1000;
      mac["free_heap"] = ESP.getFreeHeap();

      // Sensor Fault Logic — latch so state-change fires a standalone alert, not every cycle
      static bool lastSensorFault = false;
      bool currentSensorFault = isnan(temperature) || isnan(humidity);

      // I2C recovery: attempt re-init after HDC_REINIT_THRESHOLD consecutive NaN reads
      if (currentSensorFault)
      {
        hdcFaultCount++;
        if (hdcFaultCount >= HDC_REINIT_THRESHOLD)
        {
          Serial.println("[HEALTH] HDC1080 persistent fault — attempting I2C re-init...");
          Wire.end();
          delay(100);
          Wire.begin();
          Wire.setTimeOut(500); // cap each I2C transaction at 500ms; prevents blocking if bus is stuck
          bool recovered = hdc.begin(0x40);
          hdcFaultCount = 0;
          if (recovered)
          {
            hdcInitFailed = false;
            Serial.println("[HEALTH] HDC1080 re-init SUCCESS.");
            JsonDocument rDoc;
            JsonObject ro = rDoc[device_id].to<JsonObject>();
            ro["event"] = "hdc_reinit_success";
            char rBuf[128];
            serializeJson(rDoc, rBuf);
            client.publish(mqttTopic.c_str(), rBuf);
          }
          else
          {
            Serial.println("[HEALTH] HDC1080 re-init FAILED. Sensor may be physically dead.");
          }
        }
      }
      else
      {
        hdcFaultCount = 0; // Clear counter on good read
      }

      if (currentSensorFault != lastSensorFault)
      {
        JsonDocument alertDoc;
        JsonObject al = alertDoc[device_id].to<JsonObject>();
        al["event"] = currentSensorFault ? "sensor_fault" : "sensor_recovered";
        al["sensor_status"] = currentSensorFault ? "FAULT" : "OK";
        char alertBuf[128];
        serializeJson(alertDoc, alertBuf);
        client.publish(mqttTopic.c_str(), alertBuf);
        lastSensorFault = currentSensorFault;
      }
      if (currentSensorFault)
      {
        Serial.println("HDC1080 Read Failed!");
        mac["sensor_status"] = "FAULT";
        mac["temperature"] = nullptr;
        mac["humidity"] = nullptr;
      }
      else
      {
        mac["sensor_status"] = "OK";
        mac["temperature"] = temperature;
        mac["humidity"] = humidity;
      }

      char buffer[768];
      size_t written = serializeJson(doc, buffer, sizeof(buffer));
      if (written >= sizeof(buffer))
      {
        Serial.println("[MQTT] ERROR: JSON payload truncated! Skipping publish to avoid broken JSON.");
      }
      else if (client.publish(mqttTopic.c_str(), buffer))
      {
        Serial.println("\n[MQTT] Telemetry Sent:");
      }
      else
      {
        Serial.println("\n[MQTT] FAILED to send Telemetry! Forcing reconnect...");
        client.disconnect(); // triggers reconnect() on the next loop iteration
      }
      serializeJsonPretty(doc, Serial);
      Serial.println();
      // client.publish(mqttTopic.c_str(), buffer);
    }
  }
  else
  {
    // --- GSM Modem Routine ---
    checkIncomingData();

    if (millis() - lastTelemetry >= TELEMETRY_INTERVAL)
    {
      // ===== CALCULATE PRESENCE MINUTES =====
      unsigned long now = millis();

      // If currently occupied, add the running time to the bucket before calculating
      if (lastPresenceState == true)
      {
        accumulatedPresenceMs += (now - lastStateChangeTime);
      }

      // Convert accumulated milliseconds to rounded seconds (FOR TESTING)
      int total_interval_secs = round(TELEMETRY_INTERVAL / 1000.0);
      int presence_secs = round(accumulatedPresenceMs / 1000.0);

      if (presence_secs > total_interval_secs)
        presence_secs = total_interval_secs;
      int empty_secs = total_interval_secs - presence_secs;

      // Reset the tracking bucket for the next 15-minute window
      accumulatedPresenceMs = 0;
      lastStateChangeTime = now;
      // ======================================

      lastTelemetry = millis();
      float temperature = hdc.readTemperature();
      float humidity = hdc.readHumidity();
      lastSignalStrength = getSignalStrength();
      String modemTime = getModemTime();

      JsonDocument doc;
      JsonObject data = doc[gsmClientId].to<JsonObject>();
      data["timestamp"] = getGSMTime();

      // ---> APPEND RADAR DATA TO JSON <---
      data["radar_auto_mode"] = radarAutoMode ? "Enabled" : "Disabled";
      if (radarAutoMode)
      {
        data["presence_seconds"] = presence_secs;
        data["empty_seconds"] = empty_secs;
      }
      // ---> ADD THE THREE NEW PARAMETERS <---
      data["TEcoTemp"] = TEcoTemp;
      data["TEcoTime"] = TEcoTime / 60000; // Send as minutes
      data["TOffTime"] = TOffTime / 60000; // Send as minutes
      data["temperature"] = isnan(temperature) ? 0.0 : temperature;
      data["humidity"] = isnan(humidity) ? 0.0 : humidity;
      data["status"] = "Online";

      if (!bootReasonReported)
      {
        data["last_reset_reason"] = getResetReason();
        bootReasonReported = true;
      }

      data["device_type"] = "climate_sensor";
      data["battery_level"] = batteryPercentage();
      data["cellular_signal_strength"] = lastSignalStrength;
      data["uptime_s"] = millis() / 1000;
      data["free_heap"] = ESP.getFreeHeap();

      // Sensor Fault Logic — latch fires a standalone GSM alert on state change only
      static bool lastSensorFaultGSM = false;
      bool currentSensorFaultGSM = isnan(temperature) || isnan(humidity);
      if (currentSensorFaultGSM != lastSensorFaultGSM)
      {
        JsonDocument alertDoc;
        JsonObject al = alertDoc[gsmClientId].to<JsonObject>();
        al["event"] = currentSensorFaultGSM ? "sensor_fault" : "sensor_recovered";
        al["sensor_status"] = currentSensorFaultGSM ? "FAULT" : "OK";
        char alertBuf[128];
        serializeJson(alertDoc, alertBuf);
        String alertCmd = "AT+QMTPUB=0,1,1,0,\"" + pubTopic + "\"";
        String alertResp = sendAT(alertCmd, 5000);
        if (alertResp.indexOf(">") != -1)
        {
          SerialAT.print(alertBuf);
          SerialAT.write(0x1A);
        }
        lastSensorFaultGSM = currentSensorFaultGSM;
      }
      if (currentSensorFaultGSM)
      {
        Serial.println("HDC1080 Read Failed!");
        data["sensor_status"] = "FAULT";
        data["temperature"] = nullptr;
        data["humidity"] = nullptr;
      }
      else
      {
        data["sensor_status"] = "OK";
        data["temperature"] = temperature;
        data["humidity"] = humidity;
      }

      char jsonBuffer[768];
      size_t gsmWritten = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
      if (gsmWritten >= sizeof(jsonBuffer))
      {
        Serial.println("[GSM] ERROR: JSON payload truncated! Skipping publish to avoid broken JSON.");
      }
      else
      {

        String cmd = "AT+QMTPUB=0,1,1,0,\"" + pubTopic + "\"";
        String pubResp = sendAT(cmd, 5000);

        if (pubResp.indexOf(">") != -1)
        {
          SerialAT.print(jsonBuffer);
          SerialAT.write(0x1A);
          Serial.println(">> Payload sent via GSM!");
        }
        else if (pubResp.indexOf("ERROR") != -1)
        {
          Serial.println("\n[FATAL ERROR] Modem disconnected or crashed!");
          Serial.println("Rebooting ESP32 to re-establish clean connection...");
          delay(2000);
          ESP.restart();
        }
        else
        {
          Serial.println("✗ No '>' prompt received! QMTPUB failed.");
          sendAT("AT+QMTCONN?", 3000);
        }

      } // end overflow guard
    }
  }
}