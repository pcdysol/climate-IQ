#pragma once
#include "SharedState.h"

namespace AutomationManager
{
    void init(SystemData *state);
    void TaskAutomation(void *pvParameters);
    void trackPresenceTime();
    void EnforceTimerCallback(TimerHandle_t xTimer);
    void EcoTimerCallback(TimerHandle_t xTimer);     // <--- ADD THIS
    void OffTimerCallback(TimerHandle_t xTimer);     // <--- ADD THIS
    // Exposed so the ScheduleManager and WebDashboard can also trigger AC actions
    void executeACCommand(bool turnOn, int targetTemp, const char *triggerSource);
}