/**
 * @file HealthManager.cpp
 * @brief Implementation of periodic health checks and reset-reason reporting.
 */
#include "HealthManager.h"
#include "SensorManager.h"
#include "NetworkManager.h"
#include <rom/rtc.h>
#include "esp_task_wdt.h"

namespace HealthManager {

    /// @return Human-readable cause of the last CPU reset (core 0 reset reason).
    String getResetReason() {
        RESET_REASON reason = rtc_get_reset_reason(0);
        switch (reason) {
            case 1: return "Power On";
            case 3: return "Software Reset";
            case 4: return "Watchdog Reset";
            case 12: return "Software Watchdog";
            case 14: return "Panic / Hard Crash";
            default: return "Unknown (" + String(reason) + ")";
        }
    }

    /**
     * @brief 30s periodic health check (FreeRTOS timer callback).
     *
     * Runs the radar staleness check and publishes a low_heap alert if free heap
     * drops below the threshold. The timer guarantees the 30s cadence, so no
     * millis() bookkeeping is needed here.
     */
    void HealthTimerCallback(TimerHandle_t xTimer) {
        SensorManager::checkHealth();

        uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < 12000) {
            char detail[48];
            snprintf(detail, sizeof(detail), "free_heap=%u bytes", freeHeap);
            NetworkManager::publishHealthAlert("low_heap", detail);
        }
    }
}