/**
 * @file SensorManager.cpp
 * @brief Implementation of LD2410C radar + HDC1080 sensor management.
 *
 * The sensor task is the SOLE owner of the radar UART. Normal operation streams
 * "enhanced mode" frames (presence + per-gate energy); maintenance operations
 * (calibration, factory reset, range set) briefly enter the radar's config mode,
 * which stops frames — so each one re-enables streaming afterward and pokes the
 * watchdog. Those operations are requested by other tasks via xTaskNotify bits
 * and only ever execute here. See SensorManager.h for the public contract.
 */
#include "SensorManager.h"
#include "Config.h"
#include "Indicator.h" // For flashing errors during calibration
#include <Wire.h>
#include "Adafruit_HDC1000.h"
#include "MyLD2410.h"
#include "board_select.h"
#include "WebDashboard.h"
#include "AutomationManager.h"
#include "IRManager.h"
#include "NetworkManager.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include <Preferences.h>

static const char *TAG = "SENSOR";

/// Shared NVS handle (namespace "ir_data"), defined in main.cpp. Used here for the
/// one-time radar-Bluetooth-disable flag (see disableRadarBluetooth()).
extern Preferences preferences;

// --- Private Objects ---
static Adafruit_HDC1000 hdc = Adafruit_HDC1000();
static MyLD2410 radar(sensorSerial);
static SystemData *globalState = nullptr;
// Set true once poll() sees a real data frame — gates the one-time boot range read.
static volatile bool radarStreamConfirmed = false;
// Add this near the top of SensorManager.cpp, after the includes
extern TaskHandle_t sensorsTaskHandle;

// --- Web-triggered IR learn mailbox (executed on the sensor task) ---
// The web task fills these via requestLearn() and signals notify bit 4; the sensor
// task runs the learn (sole owner of the IR receiver) and publishes the result.
static char             learnKey[24]    = {0};   ///< NVS key to learn into (e.g. "ir_24").
static bool             learnIsProtocol = false; ///< true = learn just the protocol.
static std::atomic<int> learnStatus{0};          ///< 0 idle, 1 in progress, 2 done.
static std::atomic<int> learnResult{1};          ///< 0 ok, 1 timeout, 2 unknown protocol.

// LD2410C distance-gate width: 0.75 m per gate (datasheet 2.2.16 default). We use this
// fixed value instead of querying resolution (the firmware never changes resolution).
#define RADAR_GATE_CM 75

// Read the radar's CURRENT stored max gate (its own flash is the source of truth)
// and cache the boundary for the dashboard. The params query briefly enters config
// mode, which drops engineering streaming, so we re-enable it right after.
static void readAndStoreRange()
{
    if (!globalState || !globalState->sensorReady)
        return;
    esp_task_wdt_reset();
    byte gate = radar.getRange(); // stored max-gate value (via the 0x0061 param query)
    // The same 0x0061 parameter query also refreshes the unmanned duration
    // (no-one window) — cache it here so the dashboard can show the current value.
    globalState->radarNoOneWindow = radar.getNoOneWindow();
    radar.enhancedMode();
    globalState->lastRadarDataTime = millis();
    // LD2410C datasheet 1.2.2 / 2.2.3: max detection gate N detects to N * 0.75 m
    // (e.g. gate 2 -> 1.5 m). The 0x0061 query returns that stored gate N directly,
    // so the boundary is simply gate * 0.75 m — no offset.
    globalState->radarRangeCm = gate * RADAR_GATE_CM;
}

// Snap a requested boundary distance (cm) to the nearest radar gate, apply it, and
// record the achieved range. MUST run on the sensor task (touches the radar UART
// via config mode). setMaxGate leaves the radar in plain run mode, so we re-enable
// enhancedMode() afterward to keep the per-gate live feed alive.
static void applyRadarRange(int cm)
{
    if (!globalState || !globalState->sensorReady || cm <= 0)
        return;

    // Resync the config state machine first (forces isConfig=false). Without this a
    // desynced flag can make the chained config commands below short-circuit and
    // leave the radar stuck in config mode — same guard the calibration path uses.
    esp_task_wdt_reset();
    radar.begin();

    const int res = RADAR_GATE_CM; // LD2410C fixed 0.75 m/gate
    // Target gate the user wants (1-8). LD2410C datasheet 1.2.2 / 2.2.3: max gate N
    // detects to N * 0.75 m ("set to 2 -> within 1.5 m" = 2 * 0.75). So write the
    // target gate DIRECTLY — round(cm / 0.75 m), clamped to the valid 1-8 range.
    int gate = (cm + res / 2) / res;
    if (gate < 1) gate = 1;
    if (gate > 8) gate = 8;

    byte window = radar.getNoOneWindow();
    if (window == 0) window = 5; // preserve current "no-one" window, default 5s

    esp_task_wdt_reset();
    bool ok = radar.setMaxGate((byte)gate, (byte)gate, window);
    globalState->lastRadarDataTime = millis();

    // Read back what the radar ACTUALLY stored (authoritative — this is what the
    // HLK app also shows). It should equal the target gate. readAndStoreRange()
    // also re-enables the engineering stream that config mode dropped, so no
    // separate enhancedMode() call is needed here.
    readAndStoreRange();

    ESP_LOGI(TAG, "setMaxGate %s: req=%dcm target_gate=%d -> radar now reports %dcm",
             ok ? "OK" : "FAILED", cm, gate, globalState->radarRangeCm.load());
}

// Set the radar's "unmanned duration" / no-one window (seconds): how long it keeps
// reporting presence after a target leaves (datasheet 2.2.5; the radar's own minimum is
// 5 s, but we accept 1 s+ and let the read-back show what it stored). MUST run on the
// sensor task (touches the radar UART via config mode).
//
// We deliberately do NOT call radar.setNoOneWindow(): that helper derives the max gate
// from movingThresholds.N, which this firmware never populates (it only issues the
// 0x0061 basic-parameter query, never the threshold query). With N == 0 it would
// write max gate = 1 and silently collapse the detection range to ~0.75 m. Instead we
// re-send the CURRENT stored max gate (from getRange()) unchanged and only swap the
// duration — the same explicit approach applyRadarRange() uses — so the range is preserved.
static void applyNoOneWindow(int seconds)
{
    if (!globalState || !globalState->sensorReady)
        return;
    if (seconds < 1)   seconds = 1;    // allow short holds (1 s+)
    if (seconds > 255) seconds = 255;  // setMaxGate's noOneWindow arg is a byte
    // NOTE: the datasheet states an unmanned-duration minimum of 5 s, so the radar may
    // clamp values below 5 internally. We still send what was asked and let the read-back
    // (radarNoOneWindow) report whatever the radar actually stored — no surprises.

    // Resync the config state machine first (forces isConfig=false) so a desynced flag
    // can't short-circuit the chained config commands — same guard the range path uses.
    esp_task_wdt_reset();
    radar.begin();

    byte curGate = radar.getRange(); // authoritative stored max gate (0x0061 query)
    if (curGate < 1) curGate = 8;    // safety: never let a bad read collapse the range

    esp_task_wdt_reset();
    bool ok = radar.setMaxGate(curGate, curGate, (byte)seconds);
    globalState->lastRadarDataTime = millis();

    // Re-enable the engineering stream (killed by config mode) and read back what the
    // radar actually stored — range is unchanged, only the duration moved.
    readAndStoreRange();

    ESP_LOGI(TAG, "setNoOneWindow %s: req=%ds gate_preserved=%d -> radar now reports %ds",
             ok ? "OK" : "FAILED", seconds, curGate, globalState->radarNoOneWindow.load());
}

// Disable the radar's Bluetooth so no external BLE app (e.g. HLKRadarTool) can grab the
// module's single config-mode state machine over the air and starve our UART stream —
// the root cause of the "frozen radar value" hang. Notes (LD2410C datasheet 2.2.12):
//   - Command word 0x00A4, value 0x0000 = OFF (requestBToff() sends exactly this). The
//     setting is persistent (kept across power-down) and takes effect after a module
//     REBOOT, so on success we reboot and re-link.
//   - We still verify the ACK: a config frame can be lost amongst the live data stream,
//     so a false negative is possible even though the command is supported.
//   - ANTI-HANG GUARANTEE: a failed/short-circuited requestBToff() can leave the module
//     in config mode (no data frames). So we ALWAYS finish by forcing the module back
//     into enhanced streaming — whether BT-off succeeded, failed, or rebooted.
// MUST run on the sensor task (sole owner of the radar UART). @return true if BT-off was
// accepted by the module.
static bool disableRadarBluetooth()
{
    if (!globalState || !globalState->sensorReady)
        return false;

    esp_task_wdt_reset();
    radar.begin(); // resync the config flag (forces isConfig=false) before the chain

    bool accepted = radar.requestBToff();
    if (accepted)
    {
        ESP_LOGI(TAG, "Radar Bluetooth disabled; rebooting module to apply.");
        radar.requestReboot();           // BT change only applies after a reboot
        vTaskDelay(pdMS_TO_TICKS(1500));  // let the module restart
    }
    else
    {
        ESP_LOGW(TAG, "Radar BT-off not ACKed (lost in data stream?) — will retry next boot.");
    }

    // ALWAYS force streaming back on so a failed/partial/reboot attempt can never leave
    // the radar stuck in config mode with no data frames.
    esp_task_wdt_reset();
    radar.begin();
    radar.enhancedMode();
    globalState->sensorReady = true;
    globalState->lastRadarDataTime = millis();
    return accepted;
}

// Re-enable the radar's Bluetooth (for bench testing with the HLK phone app). Mirrors
// disableRadarBluetooth(): the setting is persistent in the radar's OWN flash and only
// applies after a reboot, so NOT calling the disable does NOT turn BT back on once it was
// disabled on an earlier boot — this actively sends BT-on (0x00A4 value 0x0001) and
// reboots the module. Always restores enhanced streaming afterward (anti-hang guarantee).
// MUST run on the sensor task. @return true if BT-on was accepted by the module.
static bool enableRadarBluetooth()
{
    if (!globalState || !globalState->sensorReady)
        return false;

    esp_task_wdt_reset();
    radar.begin(); // resync the config flag (forces isConfig=false) before the chain

    bool accepted = radar.requestBTon();
    if (accepted)
    {
        ESP_LOGI(TAG, "Radar Bluetooth ENABLED; rebooting module to apply.");
        radar.requestReboot();           // BT change only applies after a reboot
        vTaskDelay(pdMS_TO_TICKS(1500));  // let the module restart
    }
    else
    {
        ESP_LOGW(TAG, "Radar BT-on not ACKed (lost in data stream?) — will retry next boot.");
    }

    // ALWAYS force streaming back on so a failed/partial/reboot attempt can never leave
    // the radar stuck in config mode with no data frames.
    esp_task_wdt_reset();
    radar.begin();
    radar.enhancedMode();
    globalState->sensorReady = true;
    globalState->lastRadarDataTime = millis();
    return accepted;
}


namespace SensorManager
{
    /**
     * @brief Initialise the HDC1080 (I2C) and the LD2410C radar (UART), bind state.
     *
     * Keeps the radar bring-up deliberately minimal: drain startup noise, retry
     * begin() a few times, then enable streaming only. Config-mode reads are
     * avoided here — at boot the radar is still settling and a missed ACK can
     * leave it stuck in config mode. @param state The shared state to bind to.
     */
    void init(SystemData *state)
    {
        globalState = state;

        // 1. Initialize HDC1080
        Wire.begin();
        Wire.setTimeOut(150);
        if (!hdc.begin(0x40))
        {
            globalState->hdcInitFailed = true;
            ESP_LOGE(TAG, "HDC1080 Not Found!");
        }
        else
        {
            ESP_LOGI(TAG, "HDC1080 Initialized.");
        }

        // 2. Initialize LD2410C Radar — RX=17, TX=16 @ 256000 (LD2410C factory default)
        sensorSerial.setRxBufferSize(512);
        sensorSerial.begin(256000, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);

        // Drain startup noise
        unsigned long settleStart = millis();
        while (millis() - settleStart < 1500)
        {
            while (sensorSerial.available()) sensorSerial.read();
            delay(50);
        }

        ESP_LOGI(TAG, "Waiting for LD2410C boot...");
        bool began = false;
        for (int attempt = 1; attempt <= 3 && !began; attempt++)
        {
            began = radar.begin();
            if (!began) delay(500);
        }

        if (began)
        {
            // Keep boot minimal and proven: just enable engineering streaming. Do NOT
            // do config-mode reads here — at boot the radar is still settling and a
            // missed ACK can desync the config flag and leave it stuck in config mode
            // (no data frames). The saved range is applied later by the sensor task,
            // once streaming is confirmed.
            radar.enhancedMode();
            globalState->sensorReady = true;
            globalState->lastRadarDataTime = millis();
            ESP_LOGI(TAG, "Radar boot: success");

            // Bluetooth lockout vs. testing — controlled by RADAR_ENABLE_BT in Config.h.
            // The BT state is persistent in the radar's own flash (datasheet 2.2.19), so a
            // "radar_bt_off" NVS flag tracks what we last applied and skips the (radar-
            // rebooting) command once it matches. A lost ACK leaves the flag unchanged so
            // it retries next boot; a radar factory reset re-enables BT and re-runs this.
#if RADAR_ENABLE_BT
            // TESTING: actively turn BT back ON so the HLK phone app can connect. Required
            // because a previous BT-off persisted in the radar — simply not disabling it is
            // not enough. Clear the flag only once BT-on is actually ACKed.
            if (preferences.getBool("radar_bt_off", false))
            {
                if (enableRadarBluetooth())
                    preferences.putBool("radar_bt_off", false);
            }
#else
            // PRODUCTION: lock out external BLE config (HLKRadarTool) so the ESP32 is the
            // sole master of the radar — a phone can't enter config mode over Bluetooth and
            // freeze the UART data stream. Done once; the flag skips it on later boots.
            if (!preferences.getBool("radar_bt_off", false))
            {
                if (disableRadarBluetooth())
                    preferences.putBool("radar_bt_off", true);
            }
#endif
        }
        else
        {
            globalState->radarInitFailed = true;
            globalState->sensorReady = false;
            ESP_LOGE(TAG, "Radar boot: failed");
        }

        // --- DEBUG: scan all baud/pin combos (uncomment if radar stops working) ---
        // const uint32_t bauds[]  = {256000, 115200, 57600, 38400, 19200, 9600};
        // const uint8_t  rxPins[] = {16, 17};
        // const uint8_t  txPins[] = {17, 16};
        // const int nBauds = sizeof(bauds) / sizeof(bauds[0]);
        // const int nPins  = sizeof(rxPins) / sizeof(rxPins[0]);
        // bool began = false;
        // for (int p = 0; p < nPins && !began; p++) {
        //     for (int b = 0; b < nBauds && !began; b++) {
        //         Serial.printf("[SENSOR] Trying RX=%d TX=%d @ %lu baud... ", rxPins[p], txPins[p], bauds[b]);
        //         sensorSerial.end(); delay(100);
        //         sensorSerial.begin(bauds[b], SERIAL_8N1, rxPins[p], txPins[p]);
        //         unsigned long settle = millis();
        //         while (millis() - settle < 1000) { while (sensorSerial.available()) sensorSerial.read(); delay(50); }
        //         began = radar.begin();
        //         Serial.println(began ? "✓ OK" : "✗");
        //     }
        // }
        // ---------------------------------------------------------------------------
    }

    /**
     * @brief Read pending sensor data and update shared state.
     *
     * Polls the HDC1080 every ~10s, then drains one radar frame: updates
     * presence/distance and the per-gate energy arrays, applies the flap-delay
     * suppression window, and posts an EVENT_PRESENCE_CHANGED when presence
     * flips. Runs continuously on the sensor task between maintenance requests.
     */
    void poll()
    {
        if (!globalState)
            return;

        // --- Poll Climate ---
        // Only poll HDC periodically in your main loop to avoid blocking I2C
        // Or you can poll it here every 10 seconds via a non-blocking timer
        static unsigned long lastHDCRead = 0;
        if (millis() - lastHDCRead > 10000)
        {
            globalState->currentTemp = hdc.readTemperature();
            globalState->currentHumidity = hdc.readHumidity();
            lastHDCRead = millis();
        }

        // --- Poll Radar ---
        if (!globalState->sensorReady)
            return;

        if (radar.check() == MyLD2410::Response::DATA)
        {
            globalState->lastRadarDataTime = millis();
            radarStreamConfirmed = true; // gate the one-time boot range apply

            bool rawPresence = radar.presenceDetected();
            // detectedDistance() = live target distance in cm. getRange() returns the
            // max-gate *config* value (never populated here), so it was always 0.
            globalState->radarDistance = radar.detectedDistance();

            // --- Per-gate energy for the developer live feed (enhanced mode only) ---
            const MyLD2410::ValuesArray &mvSig = radar.getMovingSignals();
            const MyLD2410::ValuesArray &stSig = radar.getStationarySignals();
            uint8_t nGates = mvSig.N + 1; // N is the highest gate index reported (LD2410C: 8)
            if (nGates > 9) nGates = 9;   // gates 0-8 = 9 energy values
            for (uint8_t i = 0; i < nGates; i++)
            {
                globalState->radarMovingEnergy[i] = mvSig.values[i];
                globalState->radarStaticEnergy[i] = (i <= stSig.N) ? stSig.values[i] : 0;
            }
            globalState->radarGateCount = nGates;

            // --- FLAP DELAY SUPPRESSION ---
            bool flapJustExpired = false;
            if (globalState->isFlapDelayActive) {
                if (millis() - globalState->flapDelayStart < (globalState->flapDelaySec * 1000)) {
                    rawPresence = false;
                } else {
                    globalState->isFlapDelayActive = false;
                    flapJustExpired = true;
                    ESP_LOGI(TAG, "Flap delay expired. Resuming radar detection.");
                }
            }
            // ------------------------------

            bool newPresence = rawPresence;
            bool presenceChanged = (newPresence != globalState->cachedPresence);

            if (presenceChanged || flapJustExpired)
            {
                globalState->cachedPresence = newPresence;

                SystemEvent event;
                event.type = EVENT_PRESENCE_CHANGED;
                event.payload = newPresence ? 1 : 0;

                xQueueSend(automationQueue, &event, 0);
            }
        }
    }

    void attemptHDCRecovery()
    {
        Wire.end();
        delay(100);
        Wire.begin();
        Wire.setTimeOut(500);
        if (hdc.begin(0x40))
        {
            globalState->hdcInitFailed = false;
            ESP_LOGI(TAG, "HDC1080 recovered!");
        }
    }

    void attemptRadarRecovery()
    {
        sensorSerial.end();
        delay(200);
        sensorSerial.begin(256000, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);
        if (radar.begin())
        {
            radar.enhancedMode();
            globalState->sensorReady = true;
            globalState->lastRadarDataTime = millis();
            // No range re-apply needed — the radar keeps its own range in flash.
            ESP_LOGI(TAG, "Radar recovered!");
        }
    }

    // --- Web-triggered maintenance, RUN ONLY on the sensor task ---------------
    // These touch the radar UART directly, so they must never be called from the
    // web task. The web task signals them via task notifications (bits 1 and 2).

    static void runAutoCalibration()
    {
        // LD2410C "background noise detection + automatic sensitivity": the radar waits a
        // short "leave the room" window, then measures the empty-room noise floor and sets
        // per-gate sensitivities from it.
        //
        // HOW WE KNOW IT FINISHED -- the important part. The LD2410C data frame's target-
        // state byte is ONLY ever 0-3 (no target / moving / static / both); there is NO
        // 4/5/6 "calibration in progress/done" code (that was a wrong assumption carried
        // over from the LD2412). So the live stream can NEVER tell us when calibration ends.
        // The ONLY documented completion signal is the 0x001B status query (datasheet):
        //   0 = not in progress, 1 = in progress, 2 = detection completed.
        // We poll that until the radar itself reports completion -- however long it takes.
        // There is deliberately NO fixed time assumption: a real run can exceed two minutes,
        // so we track the radar's actual status instead of guessing a duration.
        ESP_LOGI(TAG, "Calibration: starting radar background noise detection...");
        globalState->radarCalStatus = 1; // in progress

        // While calibrating, presence reporting is meaningless -- clear the cached value so
        // the presence LED (pin 2) doesn't freeze on its last reading. poll() repopulates
        // it once normal streaming resumes.
        globalState->cachedPresence = false;

        esp_task_wdt_reset();
        // Resync to a known state (begin() forces config-mode OFF) so a previous run can't
        // leave the config state machine desynced.
        radar.begin();

        // 0x000B value = "time to leave the room" before the measurement begins (datasheet;
        // matches the HLK app's ~10 s countdown). The module then runs the measurement for
        // its own internal, variable-length window.
        bool started = radar.autoThresholds(10);
        globalState->lastRadarDataTime = millis();

        if (!started)
        {
            ESP_LOGW(TAG, "Calibration: radar did not accept the command.");
            globalState->radarCalStatus = 3; // failed
            radar.begin();
            radar.enhancedMode();
            globalState->lastRadarDataTime = millis();
            return;
        }

        // Poll 0x001B until the radar reports completion. We wait a few seconds between
        // polls (draining the data stream so the UART RX buffer can't overflow) to keep
        // config-mode toggling to a minimum during the measurement.
        bool done = false;
        bool sawInProgress = false;
        int  idleStreak = 0;
        // Pure safety net, far beyond any real run (which can exceed 2 minutes). The NORMAL
        // exit is the status transition below, NOT this cap.
        unsigned long deadline = millis() + 240000UL; // 4 min hard stop
        while (millis() < deadline)
        {
            // Let the measurement run undisturbed for ~4 s, draining frames, then poll once.
            for (int i = 0; i < 80 && millis() < deadline; i++)
            {
                radar.check();
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            AutoStatus st = radar.getAutoStatus(); // 0x001B query (brief config-mode toggle)
            globalState->lastRadarDataTime = millis();

            if (st == AutoStatus::COMPLETED)
            {
                done = true; // explicit "detection completed"
                break;
            }
            else if (st == AutoStatus::IN_PROGRESS)
            {
                sawInProgress = true;
                idleStreak = 0;
            }
            else if (st == AutoStatus::NOT_IN_PROGRESS)
            {
                // 0 also means "not started yet" during the leave window, so only treat it
                // as finished once we have actually seen it running, confirmed by two
                // consecutive idle reads (guards against a single transient).
                if (sawInProgress && ++idleStreak >= 2)
                {
                    done = true;
                    break;
                }
            }
            else // NOT_SET: the status query was lost in the data stream -- resync and retry.
            {
                radar.begin();
            }
        }

        globalState->radarCalStatus = done ? 2 : 3;
        ESP_LOGI(TAG, "%s", done ? "Calibration complete (radar reported finished)."
                                 : "Calibration timed out (no completion within 4 min).");

        // ONLY NOW (measurement finished) is it safe to resync and resume the engineering
        // stream for the live per-gate feed, and leave the next run a clean state machine.
        radar.begin();
        radar.enhancedMode();
        globalState->lastRadarDataTime = millis();
    }

    static void runRadarFactoryReset()
    {
        ESP_LOGI(TAG, "Resetting radar to factory defaults...");
        globalState->radarCalStatus = 1; // in progress

        // Radar reboots and stops streaming during the reset, so poll() can't refresh
        // presence. Clear the cached value so the presence LED (pin 2) doesn't freeze
        // on its last reading; poll() repopulates it once streaming resumes.
        globalState->cachedPresence = false;

        esp_task_wdt_reset();
        bool ok = radar.requestReset(); // ACK from the radar = real confirmation
        if (ok)
        {
            radar.requestReboot();
            vTaskDelay(pdMS_TO_TICKS(1500)); // let the module reboot
            esp_task_wdt_reset();
            radar.begin();          // re-establish the link
            radar.enhancedMode();   // re-enable live energy streaming
            globalState->sensorReady = true;

            // A factory reset restores the module's defaults — which turns Bluetooth back
            // ON. Reconcile BT with our policy (Config.h RADAR_ENABLE_BT) and keep the
            // "radar_bt_off" flag truthful for the init()-time gate.
#if RADAR_ENABLE_BT
            // TESTING: the reset already enabled BT — leave it on, just record the state.
            preferences.putBool("radar_bt_off", false);
#else
            // PRODUCTION: re-disable so the external-BLE lockout survives the reset
            // (otherwise this very feature silently re-opens the freeze hole).
            preferences.putBool("radar_bt_off", disableRadarBluetooth());
#endif

            // A factory reset reverts the radar to its defaults (max gate 14, unmanned
            // duration 5 s), but the driver still holds the PRE-reset values cached and
            // getRange()/getNoOneWindow() won't re-query while the cache is non-zero. Force
            // a fresh 0x0061 read, then refresh the dashboard mirrors (range + no-one
            // window) — otherwise the UI shows the stale values until the next reboot.
            radar.requestParameters();
            readAndStoreRange();

            ESP_LOGI(TAG, "Radar factory reset complete.");
        }
        else
        {
            ESP_LOGW(TAG, "Radar rejected the factory-reset command.");
        }

        globalState->radarCalStatus = ok ? 2 : 3;
        globalState->lastRadarDataTime = millis();
    }

    // --- Public triggers (called from the web task) ---------------------------
    bool requestCalibration()
    {
        if (!globalState || !globalState->sensorReady || sensorsTaskHandle == NULL)
            return false;
        if (globalState->radarCalStatus.load() == 1)
            return false; // a maintenance op is already running
        globalState->radarCalStatus = 1;
        xTaskNotify(sensorsTaskHandle, (1 << 1), eSetBits);
        return true;
    }

    bool requestRadarFactoryReset()
    {
        if (!globalState || sensorsTaskHandle == NULL)
            return false;
        if (globalState->radarCalStatus.load() == 1)
            return false;
        globalState->radarCalStatus = 1;
        xTaskNotify(sensorsTaskHandle, (1 << 2), eSetBits);
        return true;
    }

    int getCalStatus()
    {
        return globalState ? globalState->radarCalStatus.load() : 0;
    }

    // --- Detection-range triggers --------------------------------------------
    bool requestSetRange(int cm)
    {
        if (!globalState || !globalState->sensorReady || sensorsTaskHandle == NULL)
            return false;
        if (globalState->radarCalStatus.load() == 1)
            return false; // a calibration/reset is running — don't collide on the UART
        globalState->radarDesiredCm = cm;
        xTaskNotify(sensorsTaskHandle, (1 << 3), eSetBits);
        return true;
    }

    int getRangeCm()
    {
        return globalState ? globalState->radarRangeCm.load() : 0;
    }

    // --- Unmanned-duration triggers ------------------------------------------
    bool requestSetNoOneWindow(int seconds)
    {
        if (!globalState || !globalState->sensorReady || sensorsTaskHandle == NULL)
            return false;
        if (globalState->radarCalStatus.load() == 1)
            return false; // a calibration/reset is running — don't collide on the UART
        globalState->radarDesiredNoOne = seconds;
        xTaskNotify(sensorsTaskHandle, (1 << 5), eSetBits);
        return true;
    }

    int getNoOneWindow()
    {
        return globalState ? globalState->radarNoOneWindow.load() : 0;
    }

    // --- IR learn triggers (executed on the sensor task) ----------------------
    bool requestLearn(const char* storageKey, bool isProtocol)
    {
        if (sensorsTaskHandle == NULL || storageKey == nullptr)
            return false;
        if (learnStatus.load() == 1)
            return false; // a learn is already running
        // Don't collide with a radar config op: both block the same task, and a
        // long calibration would stall the learn far past the web client's wait.
        if (globalState && globalState->radarCalStatus.load() == 1)
            return false;

        strncpy(learnKey, storageKey, sizeof(learnKey) - 1);
        learnKey[sizeof(learnKey) - 1] = '\0';
        learnIsProtocol = isProtocol;
        learnResult.store(1); // default to timeout until the learn proves otherwise
        learnStatus.store(1); // in progress
        xTaskNotify(sensorsTaskHandle, (1 << 4), eSetBits);
        return true;
    }

    int getLearnStatus()
    {
        return learnStatus.load();
    }

    int consumeLearnResult()
    {
        int r = learnResult.load();
        learnStatus.store(0); // back to idle so the next learn can be queued
        return r;
    }

    /**
     * @brief Flag the radar for recovery if no frame has arrived for RADAR_STALE_MS.
     *
     * Called from the 30s health timer. Does not touch the UART itself — it just
     * notifies the sensor task (bit 0), which performs the actual recovery.
     */
    void checkHealth()
    {
        if (!globalState)
            return;

        if (globalState->sensorReady && globalState->lastRadarDataTime > 0 &&
            (millis() - globalState->lastRadarDataTime > RADAR_STALE_MS))
        {
            ESP_LOGW(TAG, "Radar data is stale — notifying task for recovery...");
            if (sensorsTaskHandle != NULL)
            {
                xTaskNotify(sensorsTaskHandle, (1 << 0), eSetBits);
            }
        }
    }
    /// Poll the physical button; short press enters AP mode, long press exits it.
    void processButton()
    {
        ButtonEvent btn = Indicator::checkButton();
        if (btn == BTN_SHORT_PRESS)
        {
            ESP_LOGI(TAG, "Short button press detected: switching to AP mode");
            WebDashboard::startAPMode();
        }
        else if (btn == BTN_LONG_PRESS)
        {
            ESP_LOGI(TAG, "Long button press detected: exiting AP mode");
            WebDashboard::stopAPMode();
        }
    }

    /**
     * @brief FreeRTOS task: the sensor service loop.
     *
     * Each iteration either handles a pending maintenance notification OR polls
     * the sensors. The notification bitmask selects the operation, all of which
     * must run here because they touch the radar UART:
     *   - bit 0: radar stale -> recover the link
     *   - bit 1: run auto-calibration
     *   - bit 2: factory-reset the radar
     *   - bit 3: apply a new detection range
     *   - bit 4: learn an IR code/protocol (touches the IR receiver this task owns)
     *   - bit 5: apply a new unmanned duration (no-one window)
     * It also runs the always-on IR-remote listener, the one-time boot range
     * read, the button poll, and presence-time accounting every tick.
     * @note Spawned once; never returns. Sole owner of the radar UART.
     */
    void TaskSensors(void *pvParameters)
    {
        uint32_t notificationValue;

        for (;;)
        {
            // Non-blocking check for signals from other tasks (health/cal/reset)
            if (xTaskNotifyWait(0x00, 0xFFFFFFFF, &notificationValue, 0) == pdTRUE)
            {
                if (notificationValue & (1 << 0)) // health: radar stale → recover
                {
                    globalState->sensorReady = false;
                    globalState->cachedPresence = false;
                    attemptRadarRecovery();
                }
                if (notificationValue & (1 << 1)) // web: auto-calibrate
                {
                    runAutoCalibration();
                }
                if (notificationValue & (1 << 2)) // web: factory reset radar
                {
                    runRadarFactoryReset();
                }
                if (notificationValue & (1 << 3)) // web: set detection range
                {
                    applyRadarRange(globalState->radarDesiredCm.load());
                }
                if (notificationValue & (1 << 5)) // web: set unmanned duration (no-one window)
                {
                    applyNoOneWindow(globalState->radarDesiredNoOne.load());
                }
                if (notificationValue & (1 << 4)) // web: learn an IR code/protocol
                {
                    // Runs on THIS task, which also owns pollRemoteListener() — so the
                    // receiver is never touched from two cores at once (the old bug).
                    int r = IRManager::learnCommand(learnKey, learnIsProtocol);
                    learnResult.store(r);
                    learnStatus.store(2); // done — web handler consumes the result
                }
            }
            else
            {
                poll();
            }

            // --- Always-on remote detection ---
            // Catch any USER press on the AC/phone IR remote. A genuine foreign
            // frame is logged for the dashboard and triggers an immediate enforce
            // so the schedule/automation re-asserts the correct AC state.
            // pollRemoteListener() is always called so it keeps draining the IR ring
            // buffer (and its debounce state stays fresh); when the backend has
            // disabled the listener via remote_ir_control we simply ignore the press
            // — nothing is logged and no override event is queued.
            IRManager::RemotePress press;
            if (IRManager::pollRemoteListener(press))
            {
                if (globalState->remoteIrEnabled.load())
                {
                    uint8_t idx = globalState->remoteLogHead;
                    globalState->remoteLog[idx].atMillis = millis();
                    snprintf(globalState->remoteLog[idx].text,
                             sizeof(globalState->remoteLog[idx].text),
                             "%s 0x%lX", press.proto, (unsigned long)press.value);
                    globalState->remoteLogHead = (idx + 1) % REMOTE_LOG_SIZE;
                    globalState->remoteOverrideCount.fetch_add(1, std::memory_order_relaxed);

                    ESP_LOGI(TAG, "Manual remote press detected: %s 0x%lX",
                             press.proto, (unsigned long)press.value);

                    // Revert ONLY when it's our AC's protocol and the user actually moved
                    // power or temperature away from the schedule. Power and temp are
                    // independent; temp is compared to the NORMAL setpoint, never eco.
                    // Swing/fan/mode presses (power+temp unchanged) are respected — no
                    // command is sent, so the user's adjustment sticks.
                    if (press.isOurAc)
                    {
                        AutoState st     = globalState->acAutoState.load();
                        bool intendedPwr = (st != AUTO_OFF);
                        bool powerChanged = (press.power != intendedPwr);
                        bool tempChanged  = (press.power &&
                                             press.temp != globalState->currentNormalTemp);
                        if (powerChanged || tempChanged)
                        {
                            SystemEvent ev;
                            ev.type = EVENT_MANUAL_OVERRIDE;
                            ev.payload = 0;
                            xQueueSend(automationQueue, &ev, 0);
                        }
                    }
                }
                else if (!globalState->enforcementEnabled.load() && press.isOurAc)
                {
                    // BOTH OFF (remote_ir + enforcement): the device no longer reverts
                    // manual changes, so we REPORT them to the cloud instead — while
                    // treating the schedule's state as READ ONLY. We never write
                    // currentNormalTemp / acAutoState here. A separate dedup baseline
                    // (manualRpt*) suppresses the AC-state re-sends that fan/mode frames
                    // repeat; it is re-armed by executeACCommand() on any device command.
                    AutoState st      = globalState->acAutoState.load();
                    bool intendedOn   = (st != AUTO_OFF);
                    bool powerChanged = (press.power != intendedOn);
                    bool tempChanged  = (press.power &&
                                         press.temp != globalState->currentNormalTemp);

                    int lastP = globalState->manualRptPower.load();
                    int lastT = globalState->manualRptTemp.load();
                    bool dupe = (lastP == (int)press.power) &&
                                (!press.power || lastT == press.temp);

                    if ((powerChanged || tempChanged) && !dupe)
                    {
                        char detail[40];
                        snprintf(detail, sizeof(detail), "state=%s,temp=%d",
                                 press.power ? "ON" : "OFF", press.power ? press.temp : 0);
                        NetworkManager::publishACK("manual_remote_change", detail);
                        ESP_LOGI(TAG, "Manual remote change reported (schedule untouched): %s", detail);

                        // Update the dedup baseline ONLY — never the schedule's setpoint.
                        globalState->manualRptPower = (int)press.power;
                        globalState->manualRptTemp  = press.power ? press.temp : -1;
                    }
                }
            }

            // One-time: read the radar's stored range for the dashboard AFTER
            // streaming is confirmed healthy (never during the fragile boot window).
            static bool bootRangeRead = false;
            if (!bootRangeRead && radarStreamConfirmed)
            {
                readAndStoreRange();
                bootRangeRead = true;
            }

            processButton();
            AutomationManager::trackPresenceTime();

            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

} // end namespace1````````````````````````````````````````````````