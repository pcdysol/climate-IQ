#pragma once
#include <Arduino.h>
#include "SharedState.h"

namespace WebDashboard {
    // Initialize the server (called once in setup)
    void init();

    void startTask(uint8_t core, uint8_t priority); // Add this!
    // Hotspot management
    void startAPMode();
    void stopAPMode();

    // Must be called continuously in loop() when in AP mode
    void handleClient();
    void TaskWeb(void *pvParameters);
}