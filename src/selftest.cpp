// ECO PLUS AC factory test firmware v1.2.0  |  serial 115200
// libs: Adafruit HDC1000, IRremoteESP8266, MyLD2410, ESP32 BLE

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <Adafruit_HDC1000.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <MyLD2410.h>
#include <esp_system.h>

HardwareSerial sensorSerial(1);

#define TEST_FW_VERSION "1.2.0"

#define BUTTON_PIN 13
#define LED_PIN 2
#define MOSFET_PIN 27
#define RECV_PIN 19
#define IR_PIN 18
#define RADAR_RX_PIN 16
#define RADAR_TX_PIN 17
#define RED_PIN 33
#define GREEN_PIN 32
#define BLUE_PIN 4
#define RED_PIN1 23
#define GREEN_PIN1 12
#define BLUE_PIN1 15
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

enum Result
{
    R_PASS,
    R_FAIL,
    R_CHECK,
    R_SKIP
};

static int autoPass = 0;
static int autoFail = 0;
static int testNum = 0;
static uint32_t runCount = 0;
static uint32_t minHeap = UINT32_MAX;
static unsigned long runStartMs = 0;
static char deviceMac[13] = {0};
static bool radarPresent = false;
static bool testsRunning = false;

static Adafruit_HDC1000 hdc;
static MyLD2410 radar(sensorSerial);
static IRsend irsend(IR_PIN);
static IRrecv irrecv(RECV_PIN, 1024, 50, true);

static const char *resultStr(Result r)
{
    switch (r)
    {
    case R_PASS:
        return "PASS";
    case R_FAIL:
        return "FAIL";
    case R_CHECK:
        return "CHECK";
    default:
        return "SKIP";
    }
}

static const char *resetReasonStr()
{
    switch (esp_reset_reason())
    {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "int-wdt";
    case ESP_RST_TASK_WDT:
        return "task-wdt";
    case ESP_RST_WDT:
        return "wdt";
    case ESP_RST_DEEPSLEEP:
        return "deepsleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    default:
        return "other";
    }
}

static void trackHeap()
{
    uint32_t h = ESP.getFreeHeap();
    if (h < minHeap)
        minHeap = h;
}

static void printRow(const char *component, Result r, const char *detail)
{
    testNum++;
    if (r == R_PASS)
        autoPass++;
    else if (r == R_FAIL)
        autoFail++;
    trackHeap();
    Serial.printf("  T%02d %-18s | %-5s | %s\n", testNum, component, resultStr(r),
                  detail ? detail : "");
}

static void rgb(bool r, bool g, bool b)
{
    digitalWrite(RED_PIN, r ? LOW : HIGH);
    digitalWrite(GREEN_PIN, g ? LOW : HIGH);
    digitalWrite(BLUE_PIN, b ? LOW : HIGH);
}
static void rgbOff() { rgb(false, false, false); }

static void rgb1(bool r1, bool g1, bool b1)
{
    digitalWrite(RED_PIN1, r1 ? LOW : HIGH);
    digitalWrite(GREEN_PIN1, g1 ? LOW : HIGH);
    digitalWrite(BLUE_PIN1, b1 ? LOW : HIGH);
}
static void rgbOff1() { rgb1(false, false, false); }

static void readDeviceMac()
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    sprintf(deviceMac, "%02X%02X%02X%02X%02X%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static uint32_t bumpRunCount()
{
    Preferences p;
    runCount = 1;
    if (p.begin("selftest", false))
    {
        runCount = p.getUInt("runs", 0) + 1;
        p.putUInt("runs", runCount);
        p.end();
    }
    return runCount;
}

static void printHeader()
{
    bumpRunCount();
    Serial.println();
    Serial.println("================================================================");
    Serial.println("       ECO PLUS AC  |  FACTORY HARDWARE TEST SUITE");
    Serial.println("================================================================");
    Serial.printf("  Test firmware     : v%s\n", TEST_FW_VERSION);
    Serial.printf("  Device MAC (ID)   : %s\n", deviceMac);
    Serial.printf("  Run number        : %lu\n", (unsigned long)runCount);
    Serial.printf("  Reset reason      : %s\n", resetReasonStr());
    Serial.printf("  Chip              : %s rev %d, %d cores @ %d MHz\n",
                  ESP.getChipModel(), ESP.getChipRevision(),
                  ESP.getChipCores(), ESP.getCpuFreqMHz());
    Serial.printf("  Flash             : %u KB @ %u MHz\n",
                  ESP.getFlashChipSize() / 1024, ESP.getFlashChipSpeed() / 1000000);
    Serial.printf("  PSRAM             : %u KB\n", ESP.getPsramSize() / 1024);
    Serial.printf("  Free heap         : %u bytes\n", ESP.getFreeHeap());
    Serial.println("----------------------------------------------------------------");
    Serial.printf("  %-22s | %-5s | %s\n", "TEST ITEM", "STATE", "DETAIL");
    Serial.println("----------------------------------------------------------------");
}

static void testESP32Core()
{
    char d[96];
    snprintf(d, sizeof(d), "die=%.1fC heap=%u min=%u",
             temperatureRead(), ESP.getFreeHeap(), minHeap);
    printRow("ESP32 core", R_PASS, d);
}

static void testNVS()
{
    Preferences p;
    if (!p.begin("selftest", false))
    {
        printRow("NVS storage", R_FAIL, "open failed");
        return;
    }
    const uint32_t magic = 0xC0FFEE42;
    p.putUInt("magic", magic);
    uint32_t got = p.getUInt("magic", 0);
    p.remove("magic");
    p.end();
    printRow("NVS storage", got == magic ? R_PASS : R_FAIL,
             got == magic ? "read/write OK" : "verify failed");
}

static void testI2CBus()
{
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setTimeOut(150);

    int found = 0;
    char addrs[48] = "";
    for (uint8_t addr = 1; addr < 127; addr++)
    {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0)
        {
            found++;
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "%s0x%02X", found > 1 ? "," : "", addr);
            strncat(addrs, tmp, sizeof(addrs) - strlen(addrs) - 1);
        }
    }

    char d[80];
    if (found == 0)
        printRow("I2C bus", R_FAIL, "no devices on SDA/SCL");
    else
    {
        snprintf(d, sizeof(d), "%d found [%s]", found, addrs);
        printRow("I2C bus", R_PASS, d);
    }
}

static void testHDC1080()
{
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setTimeOut(150);

    if (!hdc.begin(0x40))
    {
        printRow("HDC1080 sensor", R_FAIL, "missing at 0x40");
        return;
    }

    delay(30);
    float t1 = hdc.readTemperature();
    float h1 = hdc.readHumidity();
    delay(80);
    float t2 = hdc.readTemperature();
    float h2 = hdc.readHumidity();

    char d[96];
    if (isnan(t1) || isnan(h1) || t1 < -40 || t1 > 125 || h1 < 0 || h1 > 100)
    {
        snprintf(d, sizeof(d), "invalid sample t=%.1f h=%.1f", t1, h1);
        printRow("HDC1080 sensor", R_FAIL, d);
        return;
    }

    float tDelta = fabs(t1 - t2);
    if (tDelta > 3.0f)
    {
        snprintf(d, sizeof(d), "unstable t1=%.1f t2=%.1f", t1, t2);
        printRow("HDC1080 sensor", R_FAIL, d);
        return;
    }

    snprintf(d, sizeof(d), "%.1fC / %.1f%%RH, drift=%.1fC", t1, h1, tDelta);
    printRow("HDC1080 sensor", R_PASS, d);

    if (fabs(h1 - h2) > 8.0f)
        printRow("HDC1080 stability", R_CHECK, "humidity drift high");
    else
        printRow("HDC1080 stability", R_PASS, "2-sample stable");
}

static void testButtonIdle()
{
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    if (digitalRead(BUTTON_PIN) == LOW)
        printRow("Button idle", R_FAIL, "GPIO13 stuck LOW");
    else
        printRow("Button idle", R_PASS, "GPIO13 HIGH when released");
}

static void testMOSFET()
{
    pinMode(MOSFET_PIN, OUTPUT);
    Serial.println("  [manual] IR rail ON 2s, OFF 1s");
    digitalWrite(MOSFET_PIN, HIGH);
    delay(2000);
    digitalWrite(MOSFET_PIN, LOW);
    delay(1000);
    printRow("MOSFET IR rail", R_CHECK, "confirm IR board power cycle");
}

static void testIR()
{
    pinMode(MOSFET_PIN, OUTPUT);
    digitalWrite(MOSFET_PIN, HIGH);
    delay(50);
    irsend.begin();
    irrecv.enableIRIn();
    delay(50);

    decode_results results;
    while (irrecv.decode(&results))
        irrecv.resume();

    bool got = false;
    uint16_t freq = 0;
    for (int attempt = 0; attempt < 3 && !got; attempt++)
    {
        irsend.sendNEC(0x00FFE01FUL, 32);
        unsigned long start = millis();
        while (millis() - start < 250)
        {
            if (irrecv.decode(&results))
            {
                if (results.rawlen > 10)
                {
                    got = true;
                    freq = results.rawlen;
                    break;
                }
                irrecv.resume();
            }
            delay(5);
        }
    }

    if (got)
    {
        char d[64];
        snprintf(d, sizeof(d), "loopback OK, %d raw ticks", freq);
        printRow("IR TX/RX path", R_PASS, d);
        return;
    }

    Serial.println("  loopback failed, press any remote within 10s...");
    irrecv.resume();
    bool sawRemote = false;
    unsigned long start = millis();
    while (millis() - start < 10000)
    {
        if (irrecv.decode(&results))
        {
            if (results.rawlen > 10)
            {
                sawRemote = true;
                break;
            }
            irrecv.resume();
        }
        delay(10);
    }
    if (sawRemote)
        printRow("IR TX/RX path", R_CHECK, "RX only verified");
    else
        printRow("IR TX/RX path", R_FAIL, "no IR activity");
}

static void testRadar()
{
    sensorSerial.setRxBufferSize(512);
    sensorSerial.begin(115200, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);

    unsigned long s = millis();
    while (millis() - s < 800)
    {
        while (sensorSerial.available())
            sensorSerial.read();
        delay(20);
    }

    bool began = false;
    for (int i = 0; i < 3 && !began; i++)
    {
        began = radar.begin();
        if (!began)
            delay(300);
    }
    if (!began)
    {
        printRow("LD2412 radar", R_FAIL, "no UART response");
        return;
    }

    radar.enhancedMode();
    radarPresent = true;

    String fw = "unknown";
    if (radar.requestFirmware())
        fw = radar.getFirmware();

    bool gotData = false;
    s = millis();
    while (millis() - s < 3000 && !gotData)
    {
        if (radar.check() == MyLD2410::DATA)
            gotData = true;
        delay(20);
    }

    char d[96];
    if (gotData)
    {
        snprintf(d, sizeof(d), "fw=%s pres=%d dist=%dcm",
                 fw.c_str(), radar.presenceDetected() ? 1 : 0,
                 (int)radar.detectedDistance());
        printRow("LD2412 radar", R_PASS, d);
    }
    else
    {
        snprintf(d, sizeof(d), "fw=%s, no frames", fw.c_str());
        printRow("LD2412 radar", R_FAIL, d);
    }
}

static void testWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);

    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macFmt[18];
    snprintf(macFmt, sizeof(macFmt), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    int n = WiFi.scanNetworks(false, true);
    char d[96];
    if (n < 0)
    {
        printRow("WiFi radio", R_FAIL, "scan failed");
    }
    else
    {
        int bestRssi = (n > 0) ? WiFi.RSSI(0) : 0;
        int bestCh = (n > 0) ? WiFi.channel(0) : 0;
        snprintf(d, sizeof(d), "MAC %s, %d APs, best %d dBm ch%d",
                 macFmt, n, bestRssi, bestCh);
        printRow("WiFi radio", R_PASS, d);
    }
    WiFi.scanDelete();
}

static void testBluetooth()
{
    bool ok = false;
    BLEDevice::init("ECOPLUS-TEST");
    delay(100);
    if (BLEDevice::getInitialized())
    {
        ok = true;
        BLEDevice::deinit(false);
        delay(100);
    }
    printRow("BLE radio", ok ? R_PASS : R_FAIL, ok ? "controller init OK" : "init failed");
}

static void testRGBLed()
{
    struct
    {
        const char *name;
        bool r, g, b;
    } steps[] = {
        {"RED", 1, 0, 0},
        {"GREEN", 0, 1, 0},
        {"BLUE", 0, 0, 1},
        {"WHITE", 1, 1, 1},
    };
    Serial.println("  [manual] RGB cycle R -> G -> B -> W");
    for (auto &s : steps)
    {
        Serial.printf("    %s\n", s.name);
        rgb(s.r, s.g, s.b);
        delay(700);
    }
    rgbOff();
    printRow("RGB status LED", R_CHECK, "verify all 4 colours");
}

static void testRGBLed1()
{
    struct
    {
        const char *name;
        bool r, g, b;
    } steps[] = {
        {"RED", 1, 0, 0},
        {"GREEN", 0, 1, 0},
        {"BLUE", 0, 0, 1},
        {"WHITE", 1, 1, 1},
    };
    Serial.println("  [manual] RGB1 cycle R -> G -> B -> W");
    for (auto &s : steps)
    {
        Serial.printf("    %s\n", s.name);
        rgb1(s.r, s.g, s.b);
        delay(700);
    }
    rgbOff1();
    printRow("RGB1 status LED", R_CHECK, "verify all 4 colours");
}

static void testPresenceLed()
{
    Serial.println("  [manual] presence LED blink x5");
    for (int i = 0; i < 5; i++)
    {
        digitalWrite(LED_PIN, HIGH);
        delay(200);
        digitalWrite(LED_PIN, LOW);
        delay(200);
    }
    printRow("Presence LED", R_CHECK, "GPIO2 blink x5");
}

static void testButtonPress()
{
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    Serial.println("  press button within 8s...");
    bool pressed = false;
    unsigned long start = millis();
    while (millis() - start < 8000)
    {
        if (digitalRead(BUTTON_PIN) == LOW)
        {
            pressed = true;
            break;
        }
        delay(10);
    }
    if (pressed)
    {
        while (digitalRead(BUTTON_PIN) == LOW)
            delay(10);
        printRow("Button press", R_PASS, "GPIO13 press detected");
    }
    else
    {
        printRow("Button press", R_CHECK, "no press detected");
    }
}

static void printSummary()
{
    unsigned long elapsed = millis() - runStartMs;
    Serial.println("----------------------------------------------------------------");
    Serial.println("  RUN SUMMARY");
    Serial.printf("  Device ID        : %s\n", deviceMac);
    Serial.printf("  Test firmware    : v%s\n", TEST_FW_VERSION);
    Serial.printf("  Run number       : %lu\n", (unsigned long)runCount);
    Serial.printf("  Duration         : %lu ms\n", elapsed);
    Serial.printf("  Automatic result : %d pass / %d fail\n", autoPass, autoFail);
    Serial.printf("  Min free heap    : %u bytes\n", minHeap);
    Serial.printf("  Final verdict    : %s\n", autoFail == 0 ? "PASS" : "FAIL");
    Serial.printf("  QA_RESULT,%s,v%s,%lu,%s,%d,%d,%lu\n",
                  deviceMac, TEST_FW_VERSION, (unsigned long)runCount,
                  autoFail == 0 ? "PASS" : "FAIL", autoPass, autoFail, elapsed);
    Serial.println("  Controls: button or 'A' rerun | 'H' help");
    Serial.println("================================================================");
    Serial.println();
}

static void showVerdictLed()
{
    if (autoFail == 0)
    {
        rgb(false, true, false);
        rgb1(false, true, false);
    }
    else
    {
        rgb(true, false, false);
        rgb1(true, false, false);
    }
}

static void printMenu()
{
    Serial.println();
    Serial.println("  H = help");
    Serial.println("  A = full test suite");
    Serial.println("  1 core  2 NVS  3 I2C  4 HDC  5 button idle");
    Serial.println("  6 MOSFET  7 IR  8 radar  W WiFi  B BLE  0 UI");
}

static void runAllTests()
{
    testsRunning = true;
    autoPass = autoFail = 0;
    testNum = 0;
    minHeap = UINT32_MAX;
    radarPresent = false;
    runStartMs = millis();
    rgb(false, false, false);
    rgb1(false, false, false);
    readDeviceMac();
    printHeader();

    testESP32Core();
    testNVS();
    testI2CBus();
    testHDC1080();
    testButtonIdle();
    testRadar();
    testMOSFET();
    testIR();
    testWiFi();
    testBluetooth();
    testRGBLed();
    testRGBLed1();
    testPresenceLed();
    testButtonPress();

    printSummary();
    showVerdictLed();
    testsRunning = false;
}

static void runSingleTest(char key)
{
    testsRunning = true;
    autoPass = autoFail = 0;
    testNum = 0;
    minHeap = UINT32_MAX;
    radarPresent = false;
    runStartMs = millis();
    rgb(true, true, true);

    readDeviceMac();
    printHeader();

    switch (key)
    {
    case '1':
        testESP32Core();
        break;
    case '2':
        testNVS();
        break;
    case '3':
        testI2CBus();
        break;
    case '4':
        testHDC1080();
        break;
    case '5':
        testButtonIdle();
        break;
    case '6':
        testMOSFET();
        break;
    case '7':
        testIR();
        break;
    case '8':
        testRadar();
        break;
    case 'W':
    case 'w':
        testWiFi();
        break;
    case 'B':
    case 'b':
        testBluetooth();
        break;
    case '0':
        testRGBLed();
        testRGBLed1();
        testPresenceLed();
        testButtonPress();
        break;
    default:
        Serial.println("  unknown key, send H for help");
        testsRunning = false;
        showVerdictLed();
        return;
    }
    printSummary();
    showVerdictLed();
    testsRunning = false;
}

void setup()
{
    Serial.begin(115200);
    delay(300);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    pinMode(RED_PIN, OUTPUT);
    pinMode(GREEN_PIN, OUTPUT);
    pinMode(BLUE_PIN, OUTPUT);
    pinMode(RED_PIN1, OUTPUT);
    pinMode(GREEN_PIN1, OUTPUT);
    pinMode(BLUE_PIN1, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    rgb(false, false, false);
    rgb1(false, false, false);

    readDeviceMac();
    printMenu();
    runAllTests();
}

void loop()
{
    if (Serial.available())
    {
        char c = Serial.read();
        while (Serial.available())
            Serial.read();
        if (c == 'A' || c == 'a')
            runAllTests();
        else if (c == 'H' || c == 'h')
            printMenu();
        else if (c >= '0' && c <= '9')
            runSingleTest(c);
        else if (c == 'W' || c == 'w' || c == 'B' || c == 'b')
            runSingleTest(c);
    }

    if (!testsRunning && digitalRead(BUTTON_PIN) == LOW)
    {
        delay(50);
        if (digitalRead(BUTTON_PIN) == LOW)
        {
            while (digitalRead(BUTTON_PIN) == LOW)
                delay(10);
            runAllTests();
        }
    }

    static unsigned long lastBlink = 0;
    if (!testsRunning && autoFail != 0 && millis() - lastBlink > 400)
    {
        lastBlink = millis();
        static bool on = false;
        on = !on;
        if (on)
        {
            rgb(true, false, false);
            rgb1(true, false, false);
        }
        else
        {
            rgbOff();
            rgbOff1();
        }
    }
    delay(20);
}
