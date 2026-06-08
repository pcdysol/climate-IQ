#pragma once
#include <Arduino.h>
#include "SharedState.h"
#include <ArduinoJson.h>

/**
 * @file NetworkManager.h
 * @brief Thin transport-abstraction layer over WiFiManager and GSMManager.
 *
 * Every call dispatches to the active transport based on
 * sysData.switch_gsm_wifi (true = WiFi, false = GSM), so the rest of the code
 * publishes telemetry/ACKs/events without caring which link is up.
 */
namespace NetworkManager {
    /// Initialise the active transport (WiFi or GSM). Called once at boot.
    void init(SystemData* state);
    /// Service the active transport (called from TaskNetwork).
    void loop();
    /// Forward an automation event (auto_event code) to the active transport.
    void sendAutomationEvent(String eventCode);
    /// Forward a command acknowledgement to the active transport.
    void publishACK(const char *action, const char *detail);
    /// Forward a health/diagnostic alert to the active transport.
    void publishHealthAlert(const char *event, const char *detail);
    /// FreeRTOS task: runs loop() on the active transport while not in AP mode.
    void TaskNetwork(void *pvParameters);
}
