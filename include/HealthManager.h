#pragma once
#include "SharedState.h"
#include <Arduino.h>

/**
 * @file HealthManager.h
 * @brief Periodic system-health checks and reset-reason reporting.
 */
namespace HealthManager {
    /**
     * @brief Timer callback (30s period): run sensor staleness + low-heap checks.
     * @note Runs in the FreeRTOS timer-service context.
     */
    void HealthTimerCallback(TimerHandle_t xTimer);

    /// @return Human-readable cause of the last reset (e.g. "Watchdog Reset").
    String getResetReason();
}
