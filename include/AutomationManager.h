#pragma once
#include "SharedState.h"

namespace AutomationManager {
    void init(SystemData* state);
    void loop();
    void trackPresenceTime();
    void enforceACState();
    
    // Exposed so the ScheduleManager and WebDashboard can also trigger AC actions
    void executeACCommand(bool turnOn, int targetTemp, const char *triggerSource);
}