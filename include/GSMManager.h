#pragma once
#include <Arduino.h>
#include "SharedState.h"
#include <ArduinoJson.h>

namespace GSMManager {
    void init();
    void loop();
    void publishACK(const char *action, const char *detail);
    void sendAutomationEvent(String eventCode);
    void publishHealthAlert(const char *event, const char *detail);
    String sendAT(String command, uint32_t timeoutMs);
    void checkIncomingData();
    String getSignalStrength();
    String getModemTime();
    String getGSMTime();
    void sendAutomationEvent(String eventCode);
    int batteryPercentage();
    void publishACK(const char *action, const char *detail);
    void publishHealthAlert(const char *event, const char *detail);
    void setSystemTimeFromGSM();
}