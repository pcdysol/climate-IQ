#pragma once
#include <Arduino.h>
#include "SharedState.h"
#include <ArduinoJson.h>

namespace NetworkManager {
    void init(SystemData* state);
    void loop();
    void publishTelemetry();
    void sendAutomationEvent(String eventCode); 
    void publishACK(const char *action, const char *detail);
    void publishHealthAlert(const char *event, const char *detail);
    String getTimestamp();
    void TaskNetwork(void *pvParameters);
}