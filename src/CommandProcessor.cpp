/**
 * @file CommandProcessor.cpp
 * @brief Implementation of the inbound MQTT JSON command dispatcher.
 *
 * Called from both transports' receive paths. See CommandProcessor.h for the
 * full command vocabulary this file recognises. Each branch validates the
 * relevant keys, performs the action (often via AutomationManager /
 * ScheduleManager / IRManager), persists any settings to NVS, and ACKs.
 */
#include "CommandProcessor.h"
#include "AutomationManager.h"
#include "ScheduleManager.h"
#include "IRManager.h"
#include "Indicator.h"
#include "NetworkManager.h"
#include "OTAManager.h"
#include <Preferences.h>
#include "esp_log.h"

static const char *TAG = "CMD";

extern Preferences preferences;

namespace CommandProcessor {

    /**
     * @brief Parse one inbound MQTT JSON document and trigger the matching action.
     * @param doc Deserialized command payload.
     * @return true if a known command matched and was acted on; false otherwise.
     */
    bool processJSON(JsonDocument &doc) {
        bool isValidCommand = false;

        // Extract the main command key sent by the backend
        String cmd = doc["command"].as<String>();

        // =========================================================
        // 1. TEMPERATURE CONTROL OR SCHEDULE
        // =========================================================
        if (cmd == "temperature_control") {
            if (doc["segments"]) {
                // It is a Schedule update
                ScheduleManager::handleScheduleCommand(doc); 
                isValidCommand = true;
            } else {
                // Resolve target temperature from the command
                int targetTemp = sysData.currentNormalTemp;
                if (doc["temperature_setting"]) {
                    targetTemp = doc["temperature_setting"].as<int>();
                    // Inside a schedule the schedule owns currentNormalTemp — updating it here
                    // would cause radar to re-enter at the wrong temperature after the room empties.
                    // Send the AC at the requested temp (one-shot) but leave the stored value intact.
                    if (!sysData.isInsideSchedule) {
                        sysData.currentNormalTemp = targetTemp;
                        preferences.putInt("normal_temp", targetTemp);
                    }
                }

                // IR code overrides the target temp when provided.
                // Backend "ir" command map: 3..17 -> temperature (cmdNum + 13),
                // i.e. 3=16°C .. 17=30°C. (1/2 mean ON/OFF in the schedule path.)
                if (doc["ir"]) {
                    int cmdNum = doc["ir"].as<int>();
                    targetTemp = (cmdNum >= 3 && cmdNum <= 17) ? (cmdNum + 13) : targetTemp;
                }
                AutomationManager::executeACCommand(true, targetTemp, "manual_temp");

                sysData.acAutoState = AUTO_ON_NORMAL;
                xTimerStop(ecoTimer, 0);
                xTimerStop(offTimer, 0);
                if (sysData.radarAutoMode && !sysData.cachedPresence) {
                    if (sysData.TEcoTime > 0) xTimerChangePeriod(ecoTimer, pdMS_TO_TICKS(sysData.TEcoTime), 0);
                    if (sysData.TOffTime > 0) xTimerChangePeriod(offTimer, pdMS_TO_TICKS(sysData.TOffTime), 0);
                }
                // Outside schedule hours: immediately revert the manual ON
                if (sysData.hasAnySchedule && !sysData.isInsideSchedule) {
                    AutomationManager::executeACCommand(false, 24, "outside_schedule");
                    sysData.acAutoState = AUTO_OFF;
                    xTimerStop(ecoTimer, 0);
                    xTimerStop(offTimer, 0);
                }
                isValidCommand = true;
            }
        }
        // =========================================================
        // 2. POWER CONTROL (ON / OFF)
        // =========================================================
        else if (cmd == "power_control") {
            bool turnOn = doc["power_status"].as<bool>();
            
            if (turnOn) {
                AutomationManager::executeACCommand(true, sysData.currentNormalTemp, "manual_on");
                sysData.acAutoState = AUTO_ON_NORMAL;
                xTimerStop(ecoTimer, 0);
                xTimerStop(offTimer, 0);
                if (sysData.radarAutoMode && !sysData.cachedPresence) {
                    if (sysData.TEcoTime > 0) xTimerChangePeriod(ecoTimer, pdMS_TO_TICKS(sysData.TEcoTime), 0);
                    if (sysData.TOffTime > 0) xTimerChangePeriod(offTimer, pdMS_TO_TICKS(sysData.TOffTime), 0);
                }
                // Outside schedule hours: immediately revert the manual ON
                if (sysData.hasAnySchedule && !sysData.isInsideSchedule) {
                    AutomationManager::executeACCommand(false, 24, "outside_schedule");
                    sysData.acAutoState = AUTO_OFF;
                    xTimerStop(ecoTimer, 0);
                    xTimerStop(offTimer, 0);
                }
            } else {
                AutomationManager::executeACCommand(false, 24, "manual_off");
                sysData.acAutoState = AUTO_OFF;
            }
            isValidCommand = true;
        }
        // =========================================================
        // 3. RADAR CONTROL
        // =========================================================
        else if (cmd == "radar_control") {
            String radarStr = doc["radar"].as<String>();
            ESP_LOGI(TAG, "Received Radar Value: %s", radarStr.c_str());
            const char *detail = "unchanged";

            if (radarStr == "0100" || radarStr == "256") {
                if (!sysData.radarAutoMode) {
                    sysData.radarAutoMode = true;
                    preferences.putBool("radar_auto", true);
                    sysData.lastPresenceTime = millis();
                    ESP_LOGI(TAG, "Radar Automation: ENABLED");
                    Indicator::indicateSuccess();
                }
                detail = "enabled";
            } else if (radarStr == "0200" || radarStr == "512") {
                if (sysData.radarAutoMode) {
                    sysData.radarAutoMode = false;
                    preferences.putBool("radar_auto", false);
                    ESP_LOGI(TAG, "Radar Automation: DISABLED");
                    Indicator::indicateSuccess();
                }
                detail = "disabled";
            }
            
            sysData.radarManualOverride = true;
            sysData.radarManualValue = sysData.radarAutoMode;
            preferences.putBool("rad_ovr", true);
            preferences.putBool("rad_ovr_v", sysData.radarAutoMode);
            NetworkManager::publishACK("radar", detail);
            isValidCommand = true;
        }
        // =========================================================
        // 4. ECO PARAMETERS (Catch-all for variables)
        // =========================================================
        else if (doc["eco"] || doc["teco"] || doc["toff"]) {
            if (doc["eco"]) {
                sysData.currentEcoTemp = doc["eco"].as<int>();
                preferences.putInt("eco_temp", sysData.currentEcoTemp);
                char detail[32];
                snprintf(detail, sizeof(detail), "eco_temp=%d", sysData.currentEcoTemp);
                NetworkManager::publishACK("eco", detail);
            }
            if (doc["teco"]) {
                sysData.TEcoTime = doc["teco"].as<unsigned long>() * 60000;
                preferences.putULong("eco_time", sysData.TEcoTime);
                char detail[32];
                snprintf(detail, sizeof(detail), "teco=%lu_min", doc["teco"].as<unsigned long>());
                NetworkManager::publishACK("teco", detail);
            }
            if (doc["toff"]) {
                sysData.TOffTime = doc["toff"].as<unsigned long>() * 60000;
                if (sysData.TOffTime <= sysData.TEcoTime) {
                    sysData.TOffTime = sysData.TEcoTime + 60000;
                }
                preferences.putULong("off_time", sysData.TOffTime);
                char detail[32];
                snprintf(detail, sizeof(detail), "toff=%lu_min", doc["toff"].as<unsigned long>());
                NetworkManager::publishACK("toff", detail);
            }
            isValidCommand = true;
        }
        // =========================================================
        // 4b. SERVER-BASED OTA FIRMWARE UPDATE
        // =========================================================
        else if (cmd == "ota_update") {
            const char *url     = doc["url"]     | "";
            const char *version = doc["version"] | "";
            bool force          = doc["force"]   | false;

            bool accepted = OTAManager::requestUpdate(url, version, force);
            // Runs asynchronously in TaskOTA — do NOT block the MQTT callback.
            NetworkManager::publishACK("ota_update",
                                       accepted ? "queued" : "rejected");
            isValidCommand = true;
        }
        // =========================================================
        // 5. DYNAMIC IR PROTOCOL INJECTION
        // =========================================================
        else if (doc["protocol"]) {
            const char *protoStr = doc["protocol"];
            bool success = false;
            if (doc["state"]) {
                JsonArray stateArray = doc["state"].as<JsonArray>();
                uint16_t size = doc["size"] ? doc["size"].as<uint16_t>() : stateArray.size();
                if (size <= 256) {
                    uint8_t ac_state[size];
                    for (int i = 0; i < size; i++) ac_state[i] = stateArray[i].as<uint8_t>();
                    success = IRManager::sendDynamicState(protoStr, ac_state, size);
                }
            } else if (doc["code"]) {
                const char *codeStr = doc["code"];
                uint16_t bits = doc["bits"] ? doc["bits"].as<uint16_t>() : 32;
                uint64_t irCode = strtoull(codeStr, NULL, 16);
                success = IRManager::sendDynamicCode(protoStr, irCode, bits);
            }
            if (success) {
                Indicator::indicateIRSent();
                isValidCommand = true;
            }
        }

        return isValidCommand;
    }
}