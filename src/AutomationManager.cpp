#include "AutomationManager.h"
#include "Indicator.h"
#include "Config.h"
#include "IRManager.h"
#include "Preferences.h"
#include "NetworkManager.h"

extern Preferences preferences;

namespace AutomationManager
{
    void executeACCommand(bool turnOn, int targetTemp, const char *triggerSource)
    {
        sysData.lastCommandTime = millis();
        xTimerReset(enforceTimer, 0);
        String customKey = turnOn ? ("ir_" + String(targetTemp)) : "ir_off";

        if (!IRManager::playCustomButton(customKey.c_str()))
        {
            IRManager::sendACFallback(turnOn, targetTemp);
        }
        // --- FLAP DELAY TRIGGER ---
        if (!turnOn) {
            sysData.flapDelayStart = millis();
            sysData.isFlapDelayActive = true;
            Serial.printf("[AUTOMATION] AC OFF sent. Ignoring radar for %lu seconds.\n", sysData.flapDelaySec);
        }
        Indicator::indicateIRSent();

        char detail[64];
        snprintf(detail, sizeof(detail), "state=%s,temp=%d", turnOn ? "ON" : "OFF", targetTemp);
        NetworkManager::publishACK(triggerSource, detail);

        Serial.printf("[%s] IR Transmitted: %s\n", triggerSource, detail);
    }

    // void loop()
    // {
    //     if (!sysData.radarAutoMode || !sysData.sensorReady)
    //         return;

    //     struct tm tinfo;
    //     bool clockValid = getLocalTime(&tinfo, 0);

    //     if (sysData.hasAnySchedule && clockValid)
    //     {
    //         if (!sysData.isInsideSchedule)
    //         {
    //             uint8_t policy = preferences.getUChar("radar_out_pol", 0);
    //             if (policy != 1)
    //                 return;
    //         }
    //     }

    //     if (sysData.cachedPresence)
    //     {
    //         sysData.lastPresenceTime = millis();

    //         if (sysData.acAutoState != AUTO_ON_NORMAL)
    //         {
    //             executeACCommand(true, sysData.currentNormalTemp, "radar_presence");
    //             sysData.acAutoState = AUTO_ON_NORMAL;
    //             NetworkManager::sendAutomationEvent("1000");
    //         }
    //     }
    //     else
    //     {
    //         unsigned long emptyDuration = millis() - sysData.lastPresenceTime;

    //         if (sysData.acAutoState == AUTO_ON_NORMAL && emptyDuration >= sysData.TEcoTime && emptyDuration < sysData.TOffTime)
    //         {
    //             executeACCommand(true, sysData.currentEcoTemp, "radar_eco");
    //             sysData.acAutoState = AUTO_ON_ECO;
    //             NetworkManager::sendAutomationEvent("2000");
    //         }
    //         else if (sysData.acAutoState != AUTO_OFF && emptyDuration >= sysData.TOffTime)
    //         {
    //             executeACCommand(false, 24, "radar_off");
    //             sysData.acAutoState = AUTO_OFF;
    //             NetworkManager::sendAutomationEvent("3000");
    //         }
    //     }
    // }

    // When the alarms ring, they just push a message to the queue to wake up the main task
    void EcoTimerCallback(TimerHandle_t xTimer)
    {
        SystemEvent event;
        event.type = EVENT_ECO_TRIGGER;
        xQueueSend(automationQueue, &event, 0);
    }

    void OffTimerCallback(TimerHandle_t xTimer)
    {
        SystemEvent event;
        event.type = EVENT_OFF_TRIGGER;
        xQueueSend(automationQueue, &event, 0);
    }

    void EnforceTimerCallback(TimerHandle_t xTimer)
    {
        SystemEvent event;
        event.type = EVENT_ENFORCE_TRIGGER;
        xQueueSend(automationQueue, &event, 0);
    }

    // 2. The Main Automation Task
    void TaskAutomation(void *pvParameters)
    {
        SystemEvent incomingEvent;

        for (;;)
        {
            if (xQueueReceive(automationQueue, &incomingEvent, portMAX_DELAY) == pdPASS)
            {
                // Clock validity: epoch < 2021 means NTP has never synced (fresh boot offline).
                // Without valid time we cannot trust isInsideSchedule, so schedule-based
                // blocking is suspended until time is known.
                struct timeval _tv;
                gettimeofday(&_tv, NULL);
                bool clockValid = (_tv.tv_sec > 1609459200UL);

                // Radar policy: only block outside-schedule hours when time is actually known.
                bool blockRadar = false;
                if (sysData.hasAnySchedule && clockValid)
                {
                    if (!sysData.isInsideSchedule)
                    {
                        uint8_t policy = preferences.getUChar("radar_out_pol", 0);
                        if (policy != 1)
                        {
                            blockRadar = true;
                        }
                    }
                }
                switch (incomingEvent.type)
                {
                case EVENT_PRESENCE_CHANGED:
                    // Only process presence if radar automation is actually enabled
                    if (!sysData.radarAutoMode || blockRadar)
                        break;
                    if (incomingEvent.payload == 1)
                    {
                        // --- HUMAN ENTERED ---
                        // 1. Cancel the Eco and Off alarms!
                        xTimerStop(ecoTimer, 0);
                        xTimerStop(offTimer, 0);

                        // 2. Turn AC ON
                        if (sysData.acAutoState != AUTO_ON_NORMAL)
                        {
                            executeACCommand(true, sysData.currentNormalTemp, "radar_presence");
                            sysData.acAutoState = AUTO_ON_NORMAL;
                            NetworkManager::sendAutomationEvent("1000");
                        }
                    }
                    else
                    {
                        // --- ROOM EMPTY ---
                        // 1. Start the Eco and Off alarm countdowns using the user's settings!
                        if (sysData.TEcoTime > 0)
                        {
                            xTimerChangePeriod(ecoTimer, pdMS_TO_TICKS(sysData.TEcoTime), 0);
                        }
                        if (sysData.TOffTime > 0)
                        {
                            xTimerChangePeriod(offTimer, pdMS_TO_TICKS(sysData.TOffTime), 0);
                        }
                    }
                    break;

                // --- THE ALARMS RING! ---
                case EVENT_ECO_TRIGGER:
                    if (!sysData.radarAutoMode || blockRadar)
                        break;
                    // Only trigger Eco if the AC is currently running normally
                    if (sysData.acAutoState == AUTO_ON_NORMAL)
                    {
                        executeACCommand(true, sysData.currentEcoTemp, "radar_eco");
                        sysData.acAutoState = AUTO_ON_ECO;
                        NetworkManager::sendAutomationEvent("2000");
                    }
                    break;

                case EVENT_OFF_TRIGGER:
                    // Add the missing guard check!
                    if (!sysData.radarAutoMode || blockRadar)
                        break;
                    // Turn it off
                    if (sysData.acAutoState != AUTO_OFF)
                    {
                        executeACCommand(false, 24, "radar_off");
                        sysData.acAutoState = AUTO_OFF;
                        NetworkManager::sendAutomationEvent("3000");
                    }
                    break;

                case EVENT_ENFORCE_TRIGGER:
                    // Outside schedule hours (and time is known): enforce OFF every 3 min.
                    // When clock is invalid (offline boot) skip the schedule check so the AC
                    // state loaded from NVS/radar is not overridden until time is available.
                    if (clockValid && sysData.hasAnySchedule && !sysData.isInsideSchedule) {
                        executeACCommand(false, 24, "enforce_off");
                        sysData.acAutoState = AUTO_OFF;
                        break;
                    }
                    // Inside schedule (or no schedule, or no valid time): re-assert current state.
                    if (sysData.radarAutoMode && sysData.acAutoState == AUTO_ON_ECO)
                        break;
                    if (sysData.acAutoState == AUTO_ON_NORMAL)
                        executeACCommand(true, sysData.currentNormalTemp, "enforce_normal");
                    else if (sysData.acAutoState == AUTO_ON_ECO)
                        executeACCommand(true, sysData.currentEcoTemp, "enforce_eco");
                    else if (sysData.acAutoState == AUTO_OFF)
                        executeACCommand(false, 24, "enforce_off");
                    break;

                case EVENT_MQTT_COMMAND:
                    // Execute Manual Override
                    break;
                }
            }
        }
    }

    void trackPresenceTime()
    {
        if (!sysData.sensorReady)
            return;

        unsigned long now = millis();

        // Accumulate unconditionally every 20ms so the network task only needs exchange(0).
        // Sensor task is the sole writer of lastPresenceState and lastStateChangeTime.
        if (sysData.lastPresenceState)
        {
            uint32_t delta = now - sysData.lastStateChangeTime;
            sysData.accumulatedPresenceMs.fetch_add(delta, std::memory_order_relaxed);
        }
        sysData.lastStateChangeTime = now;
        sysData.lastPresenceState = sysData.cachedPresence.load(std::memory_order_relaxed);
    }
}