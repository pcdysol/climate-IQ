#include "NetworkManager.h"
#include "WiFiManager.h"
#include "GSMManager.h"
#include "esp_task_wdt.h"

namespace NetworkManager {

    void init(SystemData* state) {
        if (state->switch_gsm_wifi) {
            WiFiManager::init();
        } else {
            GSMManager::init();
        }
    }

    void loop() {
        if (sysData.switch_gsm_wifi) {
            WiFiManager::loop();
        } else {
            GSMManager::loop();
        }
    }

    void publishACK(const char *action, const char *detail) {
        if (sysData.switch_gsm_wifi) {
            WiFiManager::publishACK(action, detail);
        } else {
            GSMManager::publishACK(action, detail);
        }
    }

    void publishHealthAlert(const char *event, const char *detail) {
        if (sysData.switch_gsm_wifi) {
            WiFiManager::publishHealthAlert(event, detail);
        } else {
            GSMManager::publishHealthAlert(event, detail);
        }
    }

    void sendAutomationEvent(String eventCode) {
        if (sysData.switch_gsm_wifi) {
            WiFiManager::sendAutomationEvent(eventCode);
        } else {
            GSMManager::sendAutomationEvent(eventCode);
        }
    }

    void TaskNetwork(void *pvParameters) {
        for (;;) {
            if (!sysData.isAPMode) {
                loop();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}