/**
 * @file NetworkManager.cpp
 * @brief Transport dispatcher: routes every call to WiFiManager or GSMManager.
 *
 * Each function checks sysData.switch_gsm_wifi and forwards to the active
 * transport, so callers never branch on the link type. TaskNetwork drives the
 * active transport's loop() while the device is not in AP mode.
 */
#include "NetworkManager.h"
#include "WiFiManager.h"
#include "GSMManager.h"
#include "esp_task_wdt.h"

namespace NetworkManager {

    /// Initialise whichever transport is selected by sysData.switch_gsm_wifi.
    void init(SystemData* state) {
        if (state->switch_gsm_wifi) {
            WiFiManager::init();
        } else {
            GSMManager::init();
        }
    }

    /// Service the active transport's loop().
    void loop() {
        if (sysData.switch_gsm_wifi) {
            WiFiManager::loop();
        } else {
            GSMManager::loop();
        }
    }

    /// Forward a command ACK to the active transport.
    void publishACK(const char *action, const char *detail) {
        if (sysData.switch_gsm_wifi) {
            WiFiManager::publishACK(action, detail);
        } else {
            GSMManager::publishACK(action, detail);
        }
    }

    /// Forward a health/diagnostic alert to the active transport.
    void publishHealthAlert(const char *event, const char *detail) {
        if (sysData.switch_gsm_wifi) {
            WiFiManager::publishHealthAlert(event, detail);
        } else {
            GSMManager::publishHealthAlert(event, detail);
        }
    }

    /// Forward an automation event (auto_event code) to the active transport.
    void sendAutomationEvent(String eventCode) {
        if (sysData.switch_gsm_wifi) {
            WiFiManager::sendAutomationEvent(eventCode);
        } else {
            GSMManager::sendAutomationEvent(eventCode);
        }
    }

    /// FreeRTOS task: pump the active transport's loop() unless in AP mode.
    void TaskNetwork(void *pvParameters) {
        for (;;) {
            if (!sysData.isAPMode) {
                loop();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}