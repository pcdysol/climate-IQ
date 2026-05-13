// #include "NetworkManager.h"
// #include "config.h"
// #include "CommandProcessor.h"
// #include "HealthManager.h"
// #include "Indicator.h"
// #include "SensorManager.h"
// #include "AutomationManager.h"
// #include "ScheduleManager.h"
// #include <WiFi.h>
// #include <PubSubClient.h>
// #include <Preferences.h>
// #include "esp_task_wdt.h"

// extern Preferences preferences;

// WiFiClient espClient;
// PubSubClient client(espClient);
// HardwareSerial SerialAT(1);

// String currentSSID = "";
// String currentPassword = "";
// String macAddress;
// String mqttTopic;
// String device_id;
// String macAddress_gsm;
// String gsmClientId;
// String pubTopic;
// String subTopic;

// namespace NetworkManager
// {
//     void TaskNetwork(void *pvParameters)
//     {
//         for (;;)
//         {
//             // NetworkManager handles its own blocking. If GSM takes 5 seconds,
//             // it only blocks this specific task.
//             if (!sysData.isAPMode)
//             {
//                 loop();
//             }
//             vTaskDelay(pdMS_TO_TICKS(100));
//         }
//     }
// }

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