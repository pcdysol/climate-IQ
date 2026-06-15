/**
 * @file SensorManager.cpp
 * @brief Implementation of LD2412 radar + HDC1080 sensor management.
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

static const char *TAG = "SENSOR";

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

// LD2412 distance-gate width: 0.75 m per gate (datasheet). We use this fixed value
// instead of querying resolution (the LD2410 0xAB query isn't supported on LD2412).
#define RADAR_GATE_CM 75

// Read the radar's CURRENT stored max gate (its own flash is the source of truth)
// and cache the boundary for the dashboard. The params query briefly enters config
// mode, which drops engineering streaming, so we re-enable it right after.
static void readAndStoreRange()
{
    if (!globalState || !globalState->sensorReady)
        return;
    esp_task_wdt_reset();
    byte gate = radar.getRange(); // raw max-gate register value (via 0x0012 query)
    radar.enhancedMode();
    globalState->lastRadarDataTime = millis();
    // getRange() returns the raw register value we wrote; the radar's EFFECTIVE max
    // gate — what it physically detects and what the HLK app shows over BLE — is one
    // gate less. Subtract it so the dashboard matches the real boundary (gate * 0.75 m).
    int effGate = (gate >= 1) ? (gate - 1) : 0;
    globalState->radarRangeCm = effGate * RADAR_GATE_CM;
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

    const int res = RADAR_GATE_CM; // LD2412 fixed 0.75 m/gate
    // Target gate the user actually wants to reach (1-14), distance = gate * 0.75 m.
    // "set to 2 -> within 1.5 m" (2 * 0.75). So round(cm / 0.75 m), clamped 1-14.
    int gate = (cm + res / 2) / res;
    if (gate < 1) gate = 1;
    if (gate > 14) gate = 14;

    // The radar lands one gate SHORT of the value written (confirmed against the HLK
    // app, which reads the same register over BLE: writing gate N ends up stored as
    // N-1, e.g. requesting 6 m / gate 8 physically detected only to 5.25 m / gate 7).
    // Write gate+1 so the radar's stored boundary equals the target gate. setMaxGate
    // clamps the result to 14, so the very top end saturates at gate 13 (9.75 m).
    int writeGate = gate + 1;
    if (writeGate > 14) writeGate = 14;

    byte window = radar.getNoOneWindow();
    if (window == 0) window = 5; // preserve current "no-one" window, default 5s

    esp_task_wdt_reset();
    bool ok = radar.setMaxGate((byte)writeGate, (byte)writeGate, window);
    radar.enhancedMode(); // restore engineering stream killed by config mode
    globalState->lastRadarDataTime = millis();

    // Read back what the radar ACTUALLY stored (authoritative — this is what the
    // HLK app also shows). After the +1 compensation this should equal the target gate.
    readAndStoreRange();

    ESP_LOGI(TAG, "setMaxGate %s: req=%dcm target_gate=%d wrote=%d -> radar now reports %dcm",
             ok ? "OK" : "FAILED", cm, gate, writeGate, globalState->radarRangeCm.load());
}


namespace SensorManager
{
    /**
     * @brief Initialise the HDC1080 (I2C) and the LD2412 radar (UART), bind state.
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

        // 2. Initialize LD2412 Radar — RX=17, TX=16 @ 115200 (confirmed)
        sensorSerial.setRxBufferSize(512);
        sensorSerial.begin(115200, SERIAL_8N1, 16, 17);

        // Drain startup noise
        unsigned long settleStart = millis();
        while (millis() - settleStart < 1500)
        {
            while (sensorSerial.available()) sensorSerial.read();
            delay(50);
        }

        ESP_LOGI(TAG, "Waiting for LD2412 boot...");
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
            uint8_t nGates = mvSig.N + 1; // N is the highest gate index reported
            if (nGates > 14) nGates = 14;
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
        sensorSerial.begin(115200, SERIAL_8N1, 17, 16);
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
        // LD2412 "dynamic background correction": the radar learns the empty-room
        // noise floor itself. Per datasheet 2.2.14, the 0x1B status query reports a
        // flag — 1 = executing, 0 = not executing — NOT a percentage. So completion
        // is: we first SEE it executing, then wait for it to return to not-executing.
        ESP_LOGI(TAG, "Calibration: starting radar dynamic background correction...");
        globalState->radarCalStatus = 1; // in progress

        // Radar enters config mode now and stops emitting data frames, so poll()
        // can't refresh presence for the whole calibration. Clear the cached value
        // so the presence LED (pin 2) reflects "radar offline" instead of freezing
        // on its last reading. poll() repopulates it once streaming resumes.
        globalState->cachedPresence = false;

        esp_task_wdt_reset();
        // Resync to a known state (begin() forces config-mode OFF) so a previous
        // run can't leave the config state machine desynced — that was making the
        // *second* calibration fail.
        radar.begin();

        bool started = radar.autoThresholds();
        if (!started)
        {
            ESP_LOGW(TAG, "Calibration: radar did not accept the command.");
            globalState->radarCalStatus = 3; // failed
            radar.begin();
            radar.enhancedMode();
            globalState->lastRadarDataTime = millis();
            return;
        }

        bool sawExecuting = false;
        bool done = false;
        uint8_t notSetCount = 0;
        unsigned long deadline = millis() + 120000UL; // 2-min hard safety cap
        while (millis() < deadline)
        {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1000));
            // Keep the health watchdog from flagging "stale" while config-mode polling
            // suppresses normal data frames.
            globalState->lastRadarDataTime = millis();

            AutoStatus st = radar.getAutoStatus();
            if (st == AutoStatus::NOT_SET)
            {
                // Status query itself failed — fail fast rather than hang 2 min.
                if (++notSetCount >= 5)
                    break;
                continue;
            }
            notSetCount = 0;

            if (st == AutoStatus::IN_PROGRESS) // radar reports "executing"
                sawExecuting = true;
            else if (sawExecuting) // executing → not-executing = finished
            {
                done = true;
                break;
            }
        }

        globalState->radarCalStatus = done ? 2 : 3;
        ESP_LOGI(TAG, "%s", done ? "Calibration complete."
                                 : "Calibration timed out / failed.");

        // Full resync so live streaming resumes and the NEXT run starts clean.
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