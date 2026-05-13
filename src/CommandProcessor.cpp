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
                // It is a Manual Temperature change
                if (doc["temperature_setting"]) {
                    sysData.currentNormalTemp = doc["temperature_setting"].as<int>();
                    preferences.putInt("normal_temp", sysData.currentNormalTemp);
                }
                
                // Blast the IR code (using the backend 'ir' code if provided)
                if (doc["ir"]) {
                    int cmdNum = doc["ir"].as<int>();
                    // Convert cmdNum to actual temp based on your old logic, or just use the setting directly
                    int targetTemp = (cmdNum >= 3 && cmdNum <= 17) ? (cmdNum + 13) : sysData.currentNormalTemp;
                    AutomationManager::executeACCommand(true, targetTemp, "manual_temp");
                } else {
                    // Fallback to sending the temperature setting directly if 'ir' is missing
                    AutomationManager::executeACCommand(true, sysData.currentNormalTemp, "manual_temp");
                }
                
                sysData.acAutoState = AUTO_ON_NORMAL;
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