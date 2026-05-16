#include "SensorManager.h"
#include "Config.h"
#include "Indicator.h" // For flashing errors during calibration
#include <Wire.h>
#include "Adafruit_HDC1000.h"
#include "MyLD2410.h"
#include "board_select.h"
#include "WebDashboard.h"
#include "AutomationManager.h"
#include "esp_task_wdt.h"

// --- Private Objects ---
static Adafruit_HDC1000 hdc = Adafruit_HDC1000();
static MyLD2410 radar(sensorSerial);
static SystemData *globalState = nullptr;
// Add this near the top of SensorManager.cpp, after the includes
extern TaskHandle_t sensorsTaskHandle;

// --- Private Variance Filter Variables ---
static uint8_t statHistory[VARIANCE_SAMPLES];
static uint8_t historyIdx = 0;
static bool historyFull = false;
static bool isVarianceNoise = false;

namespace SensorManager
{

    void init(SystemData *state)
    {
        globalState = state;

        // 1. Initialize HDC1080
        Wire.begin();
        Wire.setTimeOut(150);
        if (!hdc.begin(0x40))
        {
            globalState->hdcInitFailed = true;
            Serial.println("[SENSOR] HDC1080 Not Found!");
        }
        else
        {
            Serial.println("[SENSOR] HDC1080 Initialized.");
        }

        // 2. Initialize LD2410 Radar (Assuming RX=16, TX=17 or standard Serial2)
        // Update these pins dynamically based on your board config
        sensorSerial.setRxBufferSize(512);
        sensorSerial.begin(256000, SERIAL_8N1, 16, 17); // Use specific pins here if defined

        Serial.print("[SENSOR] Waiting for LD2410 boot...");
        unsigned long settleStart = millis();
        while (millis() - settleStart < 1500)
        {
            while (sensorSerial.available())
                sensorSerial.read(); // Drain startup bytes
            delay(50);
        }

        bool began = false;
        for (int attempt = 1; attempt <= 3 && !began; attempt++)
        {
            began = radar.begin();
            if (!began)
                delay(500);
        }

        if (began)
        {
            radar.enhancedMode();
            globalState->sensorReady = true;
            globalState->lastRadarDataTime = millis();
            Serial.println(" ✓ Success.");
        }
        else
        {
            globalState->radarInitFailed = true;
            globalState->sensorReady = false;
            Serial.println(" ✗ Failed!");
        }
    }

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

            bool rawPresence = radar.presenceDetected();
            uint8_t movingSig = radar.movingTargetSignal();
            uint8_t statSig = radar.stationaryTargetSignal();
            // --- FLAP DELAY SUPPRESSION ---
            bool flapJustExpired = false;
            if (globalState->isFlapDelayActive) {
                if (millis() - globalState->flapDelayStart < (globalState->flapDelaySec * 1000)) {
                    // Force the radar values to 0 while the flap is closing
                    rawPresence = false;
                    movingSig = 0;
                    statSig = 0;
                } else {
                    // Timer expired, resume normal radar operation
                    globalState->isFlapDelayActive = false;
                    flapJustExpired = true;
                    Serial.println("[SENSOR] Flap delay expired. Resuming radar detection.");
                }
            }
            // ------------------------------

            // Variance Filter
            if (rawPresence && statSig > 0)
            {
                if (movingSig > 0)
                {
                    historyIdx = 0;
                    historyFull = false;
                    isVarianceNoise = false;
                }
                statHistory[historyIdx++] = statSig;
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

            bool newPresence = isVarianceNoise ? false : rawPresence;
            bool presenceChanged = (newPresence != globalState->cachedPresence);

            // Fire EVENT_PRESENCE_CHANGED when:
            //  (a) the presence state actually changed, OR
            //  (b) the flap delay just expired — this guarantees downstream timers
            //      are reconciled with reality even when cachedPresence was already
            //      false (e.g. room was empty during a schedule delete+create), so
            //      the eco/off cycle still starts correctly per the user's settings.
            if (presenceChanged || flapJustExpired)
            {
                globalState->cachedPresence = newPresence;

                SystemEvent event;
                event.type = EVENT_PRESENCE_CHANGED;
                event.payload = newPresence ? 1 : 0;

                // Push it to the queue (don't block if full)
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
            Serial.println("[HEALTH] HDC1080 recovered!");
        }
    }

    void attemptRadarRecovery()
    {
        sensorSerial.end();
        delay(200);
        sensorSerial.begin(256000, SERIAL_8N1, 16, 17);
        if (radar.begin())
        {
            radar.enhancedMode();
            globalState->sensorReady = true;
            globalState->lastRadarDataTime = millis();
            Serial.println("[HEALTH] Radar recovered!");
        }
    }

    bool calibrateRadarAuto()
    {
        if (!globalState->sensorReady)
            return false;

        globalState->sensorReady = false; // Pause automation
        if (!radar.autoThresholds(10))
        {
            globalState->sensorReady = true;
            radar.enhancedMode();
            return false;
        }

        unsigned long startTime = millis();
        bool success = false;
        while (millis() - startTime < 120000)
        {
            radar.check();
            byte status = radar.getStatus();
            if (status == 5)
            {
                success = true;
                break;
            } // Done
            if (status == 6)
            {
                break;
            } // Failed
            delay(200);
        }

        radar.enhancedMode();
        globalState->sensorReady = true;
        globalState->lastRadarDataTime = millis();
        return success;
    }

    bool calibrateRadarReset()
    {
        if (!globalState->sensorReady)
            return false;
        globalState->sensorReady = false;

        if (!radar.requestReset())
        {
            globalState->sensorReady = true;
            return false;
        }

        radar.requestReboot();
        delay(2500);

        if (radar.begin())
        {
            radar.enhancedMode();
            globalState->sensorReady = true;
            globalState->lastRadarDataTime = millis();
            return true;
        }
        return false;
    }

    const MyLD2410::ValuesArray &getMovingSignals()
    {
        return radar.getMovingSignals();
    }

    const MyLD2410::ValuesArray &getStationarySignals()
    {
        return radar.getStationarySignals();
    }

    void checkHealth()
    {
        if (!globalState)
            return;

        if (globalState->sensorReady && globalState->lastRadarDataTime > 0 &&
            (millis() - globalState->lastRadarDataTime > RADAR_STALE_MS))
        {
            Serial.println("[HEALTH] Radar data is stale — notifying task for recovery...");
            if (sensorsTaskHandle != NULL)
            {
                xTaskNotify(sensorsTaskHandle, (1 << 0), eSetBits);
            }
        }
    }
    // Global button processor (no longer needed inside a blocking smartDelay)
    void processButton()
    {
        ButtonEvent btn = Indicator::checkButton();
        if (btn == BTN_SHORT_PRESS)
        {
            Serial.println("Short Button Press Detected: Switching to AP Mode");
            WebDashboard::startAPMode();
        }
        else if (btn == BTN_LONG_PRESS)
        {
            Serial.println("Long Button Press Detected: Factory Reset");
            WebDashboard::stopAPMode();
        }
    }

    void TaskSensors(void *pvParameters)
    {
        uint32_t notificationValue;

        for (;;)
        {
            // Non-blocking check for health recovery signal
            if (xTaskNotifyWait(0x00, 0xFFFFFFFF, &notificationValue, 0) == pdTRUE)
            {
                if (notificationValue & (1 << 0))
                {
                    globalState->sensorReady = false;
                    globalState->cachedPresence = false;
                    attemptRadarRecovery();
                }
            }
            else
            {
                poll();
            }

            processButton();
            AutomationManager::trackPresenceTime();

            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

} // end namespace