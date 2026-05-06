#include "HealthManager.h"
#include "SensorManager.h"
#include "NetworkManager.h"
#include <rom/rtc.h>

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

    void loop() {
        static unsigned long lastHealthCheck = 0;
        static unsigned long lastTimeSave = 0;
        const unsigned long HEALTH_INTERVAL = 30000;     
        const unsigned long TIME_SAVE_INTERVAL = 600000; 
        const uint32_t HEAP_WARN_BYTES = 12000;          

        if (millis() - lastTimeSave >= TIME_SAVE_INTERVAL) {
            lastTimeSave = millis();
        }

        if (millis() - lastHealthCheck < HEALTH_INTERVAL) return;
        lastHealthCheck = millis();

        SensorManager::checkHealth();

        uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < HEAP_WARN_BYTES) {
            char detail[48];
            snprintf(detail, sizeof(detail), "free_heap=%u bytes", freeHeap);
            NetworkManager::publishHealthAlert("low_heap", detail);
            Serial.printf("[HEALTH] Low heap warning: %u bytes free\n", freeHeap);
        }
    }
}