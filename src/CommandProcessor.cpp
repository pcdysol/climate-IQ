#include "CommandProcessor.h"
#include "AutomationManager.h"
#include "ScheduleManager.h"
#include "IRManager.h"
#include "Indicator.h"
#include "NetworkManager.h"
#include <Preferences.h>

extern Preferences preferences;

namespace CommandProcessor {

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

                // IR code overrides the target temp when provided
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
            Serial.println("Received Radar Value: " + radarStr);
            const char *detail = "unchanged";

            if (radarStr == "0100" || radarStr == "256") {
                if (!sysData.radarAutoMode) {
                    sysData.radarAutoMode = true;
                    preferences.putBool("radar_auto", true);
                    sysData.lastPresenceTime = millis();
                    Serial.println("Radar Automation: ENABLED");
                    Indicator::indicateSuccess();
                }
                detail = "enabled";
            } else if (radarStr == "0200" || radarStr == "512") {
                if (sysData.radarAutoMode) {
                    sysData.radarAutoMode = false;
                    preferences.putBool("radar_auto", false);
                    Serial.println("Radar Automation: DISABLED");
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