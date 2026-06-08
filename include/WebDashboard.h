#pragma once
#include <Arduino.h>
#include "SharedState.h"

/**
 * @file WebDashboard.h
 * @brief SoftAP configuration portal + local web dashboard.
 *
 * Serves a single-page dashboard (PROGMEM HTML/JS) over a WebServer on port 80
 * while the device is in AP mode: WiFi/GSM setup, IR learning, radar
 * calibration/range, live dev diagnostics, and local-file OTA. Runs on its own
 * task (TaskWeb, core 0). See WebDashboard.cpp for the route table.
 */
namespace WebDashboard {
    /// Register all HTTP routes. Called once in setup().
    void init();

    /// Start the SoftAP and begin serving the dashboard.
    void startAPMode();
    /// Tear down the SoftAP and return to station/off mode.
    void stopAPMode();

    /// Pump the web server; call continuously while in AP mode.
    void handleClient();
    /// FreeRTOS task: services the web server (and deferred AP exit) in AP mode.
    void TaskWeb(void *pvParameters);
}
