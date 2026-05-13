#include "HealthManager.h"
#include "SensorManager.h"
#include "NetworkManager.h"
#include <rom/rtc.h>
#include "esp_task_wdt.h"

namespace HealthManager {

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

    // Change this signature:
    void HealthTimerCallback(TimerHandle_t xTimer) {
        // You NO LONGER NEED lastHealthCheck or millis() logic!
        // FreeRTOS guarantees this function only executes exactly every 30 seconds.
        
        SensorManager::checkHealth();

        uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < 12000) {
            char detail[48];
            snprintf(detail, sizeof(detail), "free_heap=%u bytes", freeHeap);
            NetworkManager::publishHealthAlert("low_heap", detail);
        }
    }
}