/**
 * @file selftest.cpp
 * @brief Standalone PCB self-test / factory diagnostic firmware.
 *
 * Flash this image (PlatformIO env "selftest") onto an assembled board to probe
 * every component the production firmware uses and print a PASS/FAIL/CHECK table
 * over the serial monitor (115200 baud), headed by the device MAC address.
 *
 *   pio run -e selftest -t upload && pio device monitor
 *
 * It is completely independent of the normal firmware: only THIS file is compiled
 * for the selftest env (see platformio.ini build_src_filter), so none of the
 * managers/tasks run. When you are done, re-flash the normal firmware:
 *
 *   pio run -e esp32doit-devkit-v1 -t upload
 *
 * Test coverage and method:
 *   - ESP32 core ...... chip id, flash size, free heap (always passes if it boots)
 *   - NVS / flash ..... write a magic value to a scratch namespace and read it back
 *   - HDC1080 ......... I2C bus scan + begin(0x40) + a sane-range temp/humidity read
 *   - IR TX+MOSFET+RX . LOOPBACK: power the IR front-end, transmit a code, and decode
 *                       it on our own receiver. One pass proves emitter, MOSFET power
 *                       switch and receiver together. Falls back to a 10s "press your
 *                       remote" listen so the receiver can still be confirmed alone.
 *   - LD2410C radar ... begin() then watch for streaming data frames (presence/dist)
 *   - GSM modem ....... only probed if the radar is absent (they SHARE UART 16/17):
 *                       send AT and look for OK
 *   - WiFi radio ...... scanNetworks() — proves the radio without any credentials
 *   - RGB status LED .. cycles red/green/blue/white  (needs a human to eyeball it)
 *   - Presence LED .... blinks GPIO2                 (needs a human to eyeball it)
 *   - Button .......... prompts and waits for a press (interactive)
 *
 * After the run the on-board RGB LED shows the verdict: solid GREEN if every
 * AUTOMATIC test passed, slow-blinking RED otherwise. Press the button any time
 * to re-run the whole suite.
 */
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <Adafruit_HDC1000.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include "MyLD2410.h"
#include "board_select.h" // provides `sensorSerial` (Serial1) for the radar/GSM UART

// --- Pin map (mirrors include/Config.h; duplicated so this tool is self-contained) ---
#define BUTTON_PIN   13
#define LED_PIN      2    // presence LED (also the on-board LED on many devkits)
#define MOSFET_PIN   27   // powers the IR receiver + emitter front-end
#define RECV_PIN     19   // IR receiver data
#define IR_PIN       18   // IR emitter
#define RADAR_RX_PIN 16   // shared with GSM_RX
#define RADAR_TX_PIN 17   // shared with GSM_TX
#define GSM_RX_PIN   16
#define GSM_TX_PIN   17
#define RED_PIN      33   // RGB LED is common-anode: LOW = on
#define GREEN_PIN    32
#define BLUE_PIN     4

// --- Result model ---------------------------------------------------------------
enum Result { R_PASS, R_FAIL, R_CHECK, R_SKIP };

static const char *resultStr(Result r) {
    switch (r) {
        case R_PASS:  return "PASS";
        case R_FAIL:  return "FAIL";
        case R_CHECK: return "CHECK";  // drove the part; a human must confirm
        default:      return "SKIP";
    }
}

// Running tally of AUTOMATIC tests only (CHECK/SKIP don't count toward the verdict).
static int autoPass = 0;
static int autoFail = 0;

static void printRow(const char *component, Result r, const char *detail) {
    if (r == R_PASS) autoPass++;
    else if (r == R_FAIL) autoFail++;
    // Fixed-width columns so the table lines up in the serial monitor.
    Serial.printf("  %-22s | %-5s | %s\n", component, resultStr(r), detail ? detail : "");
}

// --- Common-anode RGB helpers (LOW = on) ----------------------------------------
static void rgb(bool r, bool g, bool b) {
    digitalWrite(RED_PIN,   r ? LOW : HIGH);
    digitalWrite(GREEN_PIN, g ? LOW : HIGH);
    digitalWrite(BLUE_PIN,  b ? LOW : HIGH);
}
static void rgbOff() { rgb(false, false, false); }

// --- Shared peripheral objects --------------------------------------------------
static Adafruit_HDC1000 hdc = Adafruit_HDC1000();
static MyLD2410         radar(sensorSerial);
static IRsend           irsend(IR_PIN);
static IRrecv           irrecv(RECV_PIN, 1024, 50, true);
static bool             radarPresent = false; // set by testRadar(); gates the GSM probe

// ================================================================================
//  Individual tests
// ================================================================================

/// Print chip identity + the MAC the production firmware uses as its device id.
static void printHeader() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macStr[13];
    sprintf(macStr, "%02X%02X%02X%02X%02X%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    Serial.println();
    Serial.println("================================================================");
    Serial.println("            ClimateIQ  -  PCB SELF-TEST");
    Serial.println("================================================================");
    Serial.printf ("  Device MAC (ID) : %s\n", macStr);
    Serial.printf ("  Chip model      : %s rev %d, %d core(s)\n",
                   ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
    Serial.printf ("  CPU freq        : %d MHz\n", ESP.getCpuFreqMHz());
    Serial.printf ("  Flash size      : %u KB\n", ESP.getFlashChipSize() / 1024);
    Serial.printf ("  Free heap       : %u bytes\n", ESP.getFreeHeap());
    Serial.println("----------------------------------------------------------------");
    Serial.printf ("  %-22s | %-5s | %s\n", "COMPONENT", "STATE", "DETAIL");
    Serial.println("----------------------------------------------------------------");
}

/// NVS round-trip: write a magic value to a scratch namespace and read it back.
static void testNVS() {
    Preferences p;
    if (!p.begin("selftest", false)) {
        printRow("NVS / flash", R_FAIL, "begin() failed");
        return;
    }
    const uint32_t magic = 0xC0FFEE42;
    p.putUInt("magic", magic);
    uint32_t got = p.getUInt("magic", 0);
    p.remove("magic");
    p.end();
    if (got == magic) printRow("NVS / flash", R_PASS, "write/read OK");
    else              printRow("NVS / flash", R_FAIL, "readback mismatch");
}

/// I2C scan + HDC1080 init + a plausibility-checked temperature/humidity read.
static void testHDC1080() {
    Wire.begin();           // default SDA=21, SCL=22
    Wire.setTimeOut(150);

    // Quick bus scan so a wiring/address fault is visible even if begin() fails.
    int found = 0;
    bool sawHdc = false;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            found++;
            if (addr == 0x40) sawHdc = true;
        }
    }

    if (!hdc.begin(0x40)) {
        char d[48];
        snprintf(d, sizeof(d), "not found (I2C devices on bus: %d)", found);
        printRow("HDC1080 (temp/hum)", R_FAIL, d);
        return;
    }

    float t = hdc.readTemperature();
    float h = hdc.readHumidity();
    char d[64];
    if (isnan(t) || isnan(h) || t < -40 || t > 125 || h < 0 || h > 100) {
        snprintf(d, sizeof(d), "init ok but bad read t=%.1f h=%.1f", t, h);
        printRow("HDC1080 (temp/hum)", R_FAIL, d);
    } else {
        snprintf(d, sizeof(d), "%.1f C, %.1f %%RH%s", t, h, sawHdc ? "" : " (addr not in scan)");
        printRow("HDC1080 (temp/hum)", R_PASS, d);
    }
}

/// IR loopback: power the front-end, transmit, and decode our own blast.
/// Proves emitter + MOSFET + receiver together. Falls back to a manual remote listen.
static void testIR() {
    pinMode(MOSFET_PIN, OUTPUT);
    digitalWrite(MOSFET_PIN, HIGH);  // power the IR receiver + emitter
    delay(50);
    irsend.begin();
    irrecv.enableIRIn();
    delay(50);

    decode_results results;
    while (irrecv.decode(&results)) irrecv.resume(); // flush any startup noise

    // Transmit a known NEC code a few times and watch our own receiver for it.
    bool got = false;
    for (int attempt = 0; attempt < 3 && !got; attempt++) {
        irsend.sendNEC(0x00FFE01FUL, 32);
        unsigned long start = millis();
        while (millis() - start < 250) {
            if (irrecv.decode(&results)) {
                if (results.rawlen > 10) { got = true; break; }
                irrecv.resume();
            }
            delay(5);
        }
    }

    if (got) {
        printRow("IR TX+MOSFET+RX", R_PASS, "loopback decoded (emitter+receiver OK)");
        return;
    }

    // Loopback silent (emitter may point away from the receiver). Confirm at least
    // the receiver by asking for a real remote press.
    Serial.println("  IR loopback silent -> point ANY remote at the device and press a");
    Serial.println("  button within 10s to confirm the receiver...");
    irrecv.resume();
    bool sawRemote = false;
    unsigned long start = millis();
    while (millis() - start < 10000) {
        if (irrecv.decode(&results)) {
            if (results.rawlen > 10) { sawRemote = true; break; }
            irrecv.resume();
        }
        delay(10);
    }
    if (sawRemote)
        printRow("IR TX+MOSFET+RX", R_CHECK, "RX OK via remote; emitter UNVERIFIED");
    else
        printRow("IR TX+MOSFET+RX", R_FAIL, "no loopback and no remote seen");
}

/// LD2410C radar: establish the link, then watch for streaming data frames.
static void testRadar() {
    sensorSerial.setRxBufferSize(512);
    sensorSerial.begin(256000, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);

    // Drain boot noise.
    unsigned long s = millis();
    while (millis() - s < 800) { while (sensorSerial.available()) sensorSerial.read(); delay(20); }

    bool began = false;
    for (int i = 0; i < 3 && !began; i++) { began = radar.begin(); if (!began) delay(300); }
    if (!began) {
        printRow("LD2410C radar", R_FAIL, "no response on UART 16/17");
        return; // radarPresent stays false -> the GSM probe will run
    }

    radar.enhancedMode();
    radarPresent = true;

    // Look for at least one real data frame within ~3s.
    bool gotData = false;
    s = millis();
    while (millis() - s < 3000 && !gotData) {
        if (radar.check() == MyLD2410::Response::DATA) gotData = true;
        delay(20);
    }

    if (gotData) {
        char d[64];
        snprintf(d, sizeof(d), "streaming; presence=%d dist=%dcm",
                 radar.presenceDetected() ? 1 : 0, (int)radar.detectedDistance());
        printRow("LD2410C radar", R_PASS, d);
    } else {
        printRow("LD2410C radar", R_FAIL, "link up but no data frames");
    }
}

/// GSM modem: only meaningful if the radar is NOT fitted (shared UART). Send AT.
static void testGSM() {
    if (radarPresent) {
        printRow("GSM modem", R_SKIP, "radar fitted (shares UART 16/17)");
        return;
    }
    sensorSerial.end();
    delay(100);
    sensorSerial.begin(115200, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
    delay(200);
    while (sensorSerial.available()) sensorSerial.read();

    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        sensorSerial.println("AT");
        String resp = "";
        unsigned long start = millis();
        while (millis() - start < 1500) {
            while (sensorSerial.available()) resp += (char)sensorSerial.read();
            if (resp.indexOf("OK") != -1) { ok = true; break; }
        }
    }
    if (ok) printRow("GSM modem", R_PASS, "AT -> OK");
    else    printRow("GSM modem", R_FAIL, "no AT response (modem absent?)");
}

/// WiFi radio: a network scan proves the radio without needing any credentials.
static void testWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    int n = WiFi.scanNetworks();
    if (n < 0) {
        printRow("WiFi radio", R_FAIL, "scan failed");
    } else {
        char d[64];
        int best = (n > 0) ? WiFi.RSSI(0) : 0;
        snprintf(d, sizeof(d), "%d network(s), strongest %d dBm", n, best);
        printRow("WiFi radio", R_PASS, d);
    }
    WiFi.scanDelete();
}

/// RGB status LED: cycle the channels so a human can confirm each colour.
static void testRGBLed() {
    struct { const char *name; bool r, g, b; } steps[] = {
        {"RED",   1, 0, 0}, {"GREEN", 0, 1, 0}, {"BLUE", 0, 0, 1}, {"WHITE", 1, 1, 1},
    };
    Serial.println("  Watch the RGB LED cycle: RED -> GREEN -> BLUE -> WHITE");
    for (auto &s : steps) {
        Serial.printf("    -> %s\n", s.name);
        rgb(s.r, s.g, s.b);
        delay(700);
    }
    rgbOff();
    printRow("RGB status LED", R_CHECK, "cycled R/G/B/W - confirm visually");
}

/// Presence LED (GPIO2): blink so a human can confirm it.
static void testPresenceLed() {
    Serial.println("  Watch the PRESENCE LED (GPIO2) blink 5x");
    for (int i = 0; i < 5; i++) {
        digitalWrite(LED_PIN, HIGH); delay(200);
        digitalWrite(LED_PIN, LOW);  delay(200);
    }
    printRow("Presence LED (GPIO2)", R_CHECK, "blinked 5x - confirm visually");
}

/// Button (GPIO13, INPUT_PULLUP): wait for a press.
static void testButton() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    Serial.println("  Press the BUTTON within 8s...");
    bool pressed = false;
    unsigned long start = millis();
    while (millis() - start < 8000) {
        if (digitalRead(BUTTON_PIN) == LOW) { pressed = true; break; }
        delay(10);
    }
    if (pressed) {
        // wait for release so it doesn't immediately re-trigger the loop's re-run
        while (digitalRead(BUTTON_PIN) == LOW) delay(10);
        printRow("Button (GPIO13)", R_PASS, "press detected");
    } else {
        printRow("Button (GPIO13)", R_CHECK, "no press within 8s (not necessarily faulty)");
    }
}

// ================================================================================
//  Orchestration
// ================================================================================

static void runAllTests() {
    autoPass = autoFail = 0;
    radarPresent = false;

    printHeader();

    // Automatic tests first.
    testNVS();
    testHDC1080();
    testRadar();
    testGSM();
    testIR();
    testWiFi();

    // Interactive / visual tests.
    testRGBLed();
    testPresenceLed();
    testButton();

    Serial.println("----------------------------------------------------------------");
    Serial.printf ("  AUTOMATIC tests: %d passed, %d failed.\n", autoPass, autoFail);
    Serial.println("  (CHECK = driven OK, confirm by eye/hand; SKIP = not applicable)");
    Serial.println("  Press the button to run the suite again.");
    Serial.println("================================================================");
    Serial.println();

    // Verdict on the LED: solid green if all automatic tests passed, else blink red.
    if (autoFail == 0) rgb(false, true, false);
    else               rgb(true, false, false);
}

void setup() {
    Serial.begin(115200);
    delay(300);

    pinMode(LED_PIN, OUTPUT);
    pinMode(RED_PIN, OUTPUT);
    pinMode(GREEN_PIN, OUTPUT);
    pinMode(BLUE_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    rgb(true, true, true); // white = booting / test in progress

    runAllTests();
}

void loop() {
    // Re-run the whole suite whenever the button is pressed.
    if (digitalRead(BUTTON_PIN) == LOW) {
        delay(50); // debounce
        if (digitalRead(BUTTON_PIN) == LOW) {
            while (digitalRead(BUTTON_PIN) == LOW) delay(10); // wait for release
            runAllTests();
        }
    }

    // Keep the red verdict blinking so a failure is obvious from across the bench.
    static unsigned long lastBlink = 0;
    if (autoFail != 0 && millis() - lastBlink > 400) {
        lastBlink = millis();
        static bool on = false;
        on = !on;
        if (on) rgb(true, false, false);
        else    rgbOff();
    }
    delay(20);
}
