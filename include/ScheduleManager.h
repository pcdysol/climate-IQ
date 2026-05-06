#pragma once
#include "SharedState.h"
#include <ArduinoJson.h>

namespace ScheduleManager {
    void init(SystemData* state);
    void loop();
    void refreshHasAnySchedule();
    void handleScheduleCommand(JsonDocument &doc);
    
    // Optional: Expose these if the WebDashboard needs them directly
    uint8_t loadSegmentCount(int wday);
    bool loadSegment(int wday, int idx, ScheduleSegment &out);
}