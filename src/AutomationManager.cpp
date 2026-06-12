/**
 * @file AutomationManager.cpp
 * @brief Implementation of the presence/schedule-driven AC state machine.
 *
 * Runs as a single FreeRTOS task (TaskAutomation, core 1) that drains the
 * global automationQueue. All AC changes funnel through executeACCommand();
 * the eco/off/enforce FreeRTOS timers only enqueue events, never act directly.
 * See AutomationManager.h for the module overview and the SystemEvent /
 * AutoState definitions in SharedState.h.
 */
#include "AutomationManager.h"
#include "Indicator.h"
#include "Config.h"
#include "IRManager.h"
#include "Preferences.h"
#include "NetworkManager.h"
#include "esp_log.h"

static const char *TAG = "AUTO";

extern Preferences preferences;

namespace AutomationManager
{
    /**
     * @brief Send an AC command over IR and record the action.
     *
     * The single choke point for every AC change (see header). Resets the
     * enforce timer so the next periodic re-assertion is measured from now,
     * tries the learned per-temperature button before the universal fallback,
     * and on an OFF opens the radar "flap delay" window — a short blind spot
     * that stops the just-turned-off unit's airflow from being misread as
     * motion and immediately turning the AC back on.
     *
     * @param turnOn        true = power on at @p targetTemp, false = power off.
     * @param targetTemp    Setpoint in °C (16–32); ignored when turning off.
     * @param triggerSource Caller label for serial logs and the MQTT ACK.
     *
     * @note Called from several tasks (automation, schedule, MQTT/web). The
     *       actual transmit is serialised by irMutex inside IRManager, so
     *       concurrent calls are safe but will block briefly on the mutex.
     */
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
            ESP_LOGI(TAG, "AC OFF sent. Ignoring radar for %lu seconds.", sysData.flapDelaySec);
        }
        Indicator::indicateIRSent();

        char detail[64];
        snprintf(detail, sizeof(detail), "state=%s,temp=%d", turnOn ? "ON" : "OFF", targetTemp);
        NetworkManager::publishACK(triggerSource, detail);

        ESP_LOGI(TAG, "IR Transmitted [%s]: %s", triggerSource, detail);
    }

    // The timer callbacks run in the FreeRTOS timer-service context, so they do
    // no real work — each just posts an event to wake TaskAutomation, where the
    // AC logic runs with full access to shared state.

    /// One-shot eco countdown elapsed: ask the task to drop to the eco setpoint.
    void EcoTimerCallback(TimerHandle_t xTimer)
    {
        SystemEvent event;
        event.type = EVENT_ECO_TRIGGER;
        xQueueSend(automationQueue, &event, 0);
    }

    /// One-shot off countdown elapsed: ask the task to turn the AC off.
    void OffTimerCallback(TimerHandle_t xTimer)
    {
        SystemEvent event;
        event.type = EVENT_OFF_TRIGGER;
        xQueueSend(automationQueue, &event, 0);
    }

    /// Periodic (3-min) tick: ask the task to re-assert the current AC state.
    void EnforceTimerCallback(TimerHandle_t xTimer)
    {
        SystemEvent event;
        event.type = EVENT_ENFORCE_TRIGGER;
        xQueueSend(automationQueue, &event, 0);
    }

    /**
     * @brief Re-assert the AC to the state the system believes it should be in.
     *
     * Shared by the 3-minute enforce timer and the immediate manual-override
     * reaction (when a user presses the physical remote). Re-sends the IR for
     * the current AutoState so a missed frame or a manual change is corrected
     * back to the intended state.
     *
     * @param clockValid Whether the system clock is trustworthy (NTP/GSM synced).
     *                   Computed once per event-loop iteration and passed in so
     *                   the schedule check is skipped on an offline boot rather
     *                   than overriding the NVS-loaded state with a wrong time.
     */
    static void enforceCurrentState(bool clockValid)
    {
        // Don't enforce (send any IR) until the system has made its first boot
        // decision — either the schedule's boot evaluation ran (scheduleBootDone)
        // or the offline failsafe engaged radar control (isOfflineFailsafeActive).
        // Before that the AC state is just the power-on default (AUTO_OFF), so
        // enforcing it would blast a spurious OFF while we are still waiting for
        // WiFi / time sync. Once the decision is made, enforce resumes its normal
        // 3-minute re-assertion.
        if (!sysData.scheduleBootDone.load() && !sysData.isOfflineFailsafeActive)
            return;

        // Outside schedule hours (and time is known): force OFF.
        if (clockValid && sysData.hasAnySchedule && !sysData.isInsideSchedule)
        {
            executeACCommand(false, 24, "enforce_off");
            sysData.acAutoState = AUTO_OFF;
            return;
        }
        // Inside schedule (or no schedule, or no valid time): re-assert current state.
        if (sysData.radarAutoMode && sysData.acAutoState == AUTO_ON_ECO)
            return;
        if (sysData.acAutoState == AUTO_ON_NORMAL)
            executeACCommand(true, sysData.currentNormalTemp, "enforce_normal");
        else if (sysData.acAutoState == AUTO_ON_ECO)
            executeACCommand(true, sysData.currentEcoTemp, "enforce_eco");
        else if (sysData.acAutoState == AUTO_OFF)
            executeACCommand(false, 24, "enforce_off");
    }

    /**
     * @brief FreeRTOS task: the AC automation event loop.
     *
     * Blocks on automationQueue forever. For every event it first computes:
     *   - @c clockValid — false until NTP/GSM sets the epoch past 2021, so the
     *     schedule (which depends on wall-clock time) is ignored on a fresh
     *     offline boot.
     *   - @c blockRadar — true when a schedule exists, the time is known, and we
     *     are currently OUTSIDE all segments (unless the "radar outside policy"
     *     pref opts in). While set, presence/eco/off events are dropped so radar
     *     cannot run the AC outside scheduled hours.
     *
     * It then dispatches on event type to drive the AutoState machine and emits
     * the matching auto_event telemetry code (1000 normal-on, 2000 eco, 3000
     * off, 4000 manual override).
     *
     * @note Spawned once from setup() (core 1) and never returns.
     */
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

                // "Schedule is king": radar must not drive the AC until the schedule has
                // made its first authoritative decision this boot. The sole exception is
                // the offline failsafe, which deliberately hands control to radar when
                // there is no network time for the schedule to act on.
                bool radarReady = sysData.scheduleBootDone.load() || sysData.isOfflineFailsafeActive;

                switch (incomingEvent.type)
                {
                case EVENT_PRESENCE_CHANGED:
                    // Only process presence if radar automation is actually enabled
                    if (!sysData.radarAutoMode || blockRadar || !radarReady)
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
                    if (!sysData.radarAutoMode || blockRadar || !radarReady)
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
                    if (!sysData.radarAutoMode || blockRadar || !radarReady)
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
                    // Periodic (3-min) re-assertion. When clock is invalid (offline
                    // boot) the helper skips the schedule check so the AC state loaded
                    // from NVS/radar is not overridden until time is available.
                    enforceCurrentState(clockValid);
                    break;

                case EVENT_MANUAL_OVERRIDE:
                    // A user pressed the physical/phone IR remote. Report it to the
                    // cloud (code 4000) and immediately re-assert the correct state
                    // instead of waiting for the next 3-min enforce tick.
                    NetworkManager::sendAutomationEvent("4000");
                    enforceCurrentState(clockValid);
                    break;

                case EVENT_MQTT_COMMAND:
                    // Reserved: MQTT commands are currently handled directly in
                    // CommandProcessor, not routed through this queue.
                    break;
                }
            }
        }
    }

    /**
     * @brief Accumulate occupied time into the presence counter for telemetry.
     *
     * Adds the elapsed delta to accumulatedPresenceMs whenever the room was
     * occupied; the network task atomically exchanges the total to 0 each
     * telemetry interval.
     *
     * @note Must run ONLY on the sensor task — it is the sole writer of
     *       lastPresenceState and lastStateChangeTime, which is what makes the
     *       lock-free accumulation safe.
     */
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