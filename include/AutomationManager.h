#pragma once
#include "SharedState.h"

/**
 * @file AutomationManager.h
 * @brief Presence/schedule-driven air-conditioner control.
 *
 * AutomationManager owns the AC state machine (AutoState: AUTO_OFF /
 * AUTO_ON_NORMAL / AUTO_ON_ECO). It does not poll any hardware itself —
 * instead it runs an event loop (TaskAutomation) that drains the global
 * @c automationQueue. Events arrive from:
 *   - SensorManager  — presence changes and manual IR-remote overrides
 *   - the FreeRTOS timers below — eco/off countdowns and the periodic enforce
 *   - CommandProcessor / ScheduleManager — indirectly, via executeACCommand()
 *
 * The actual IR blast is centralised in executeACCommand(), which every other
 * module calls so that logging, ACKs, the enforce-timer reset and the radar
 * "flap delay" all happen in exactly one place.
 */
namespace AutomationManager
{
    /**
     * @brief FreeRTOS task: the AC automation event loop.
     *
     * Blocks on @c automationQueue and reacts to each SystemEvent (presence
     * change, eco/off/enforce timer, manual override). Schedule-based blocking
     * is only applied when the clock is valid (NTP/GSM time has synced).
     * Spawned once from setup(); never returns.
     */
    void TaskAutomation(void *pvParameters);

    /**
     * @brief Accumulate time the room has been occupied, for telemetry.
     *
     * Called every sensor tick (~20 ms) from the sensor task — the sole writer
     * of the presence accumulator. The network task harvests and zeroes the
     * total each telemetry interval.
     */
    void trackPresenceTime();

    /**
     * @brief Timer callback (3-min period): queues an EVENT_ENFORCE_TRIGGER.
     *
     * Drives periodic re-assertion of the AC state so a missed/garbled IR frame
     * is eventually corrected. Runs in the timer-service context, so it only
     * enqueues an event — the real work happens on TaskAutomation.
     */
    void EnforceTimerCallback(TimerHandle_t xTimer);

    /**
     * @brief One-shot timer callback: queues an EVENT_ECO_TRIGGER.
     *
     * Armed when the room empties; firing means "empty long enough — raise the
     * setpoint to the eco temperature".
     */
    void EcoTimerCallback(TimerHandle_t xTimer);

    /**
     * @brief One-shot timer callback: queues an EVENT_OFF_TRIGGER.
     *
     * Armed when the room empties; firing means "empty long enough — turn the
     * AC off".
     */
    void OffTimerCallback(TimerHandle_t xTimer);

    /**
     * @brief Send an AC command over IR and record the action.
     *
     * The single choke point for every AC change in the system. Tries the
     * learned custom button for this temperature first
     * (IRManager::playCustomButton), falling back to the universal protocol.
     * Also resets the enforce timer, starts the radar flap-delay window on an
     * OFF, flashes the indicator, and publishes an MQTT ACK.
     *
     * Exposed (not static) so ScheduleManager, CommandProcessor and the web
     * dashboard route all AC actions through the same path.
     *
     * @param turnOn        true = power on at @p targetTemp, false = power off.
     * @param targetTemp    Target setpoint in °C (16–32). Ignored when turning off.
     * @param triggerSource Short label identifying the caller (e.g.
     *                       "radar_presence", "manual_temp", "enforce_off");
     *                       used in serial logs and the MQTT ACK detail.
     */
    void executeACCommand(bool turnOn, int targetTemp, const char *triggerSource);
}
