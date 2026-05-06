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

        if (doc["command"] == "temperature_control" && doc["segments"]) {
            ScheduleManager::handleScheduleCommand(doc); 
            return true;
        }

        if (doc["radar"]) {
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

        if (doc["temperature_setting"]) {
            sysData.currentNormalTemp = doc["temperature_setting"].as<int>();
            preferences.putInt("normal_temp", sysData.currentNormalTemp);
            if (sysData.acAutoState == AUTO_ON_NORMAL) {
                AutomationManager::executeACCommand(true, sysData.currentNormalTemp, "update_temp");
            } else {
                char detail[32];
                snprintf(detail, sizeof(detail), "temp=%d", sysData.currentNormalTemp);
                NetworkManager::publishACK("temperature_setting", detail);
            }
            isValidCommand = true;
        }

        if (doc["eco"]) {
            sysData.currentEcoTemp = doc["eco"].as<int>();
            preferences.putInt("eco_temp", sysData.currentEcoTemp);
            if (sysData.acAutoState == AUTO_ON_ECO) {
                AutomationManager::executeACCommand(true, sysData.currentEcoTemp, "update_eco");
            } else {
                char detail[32];
                snprintf(detail, sizeof(detail), "eco_temp=%d", sysData.currentEcoTemp);
                NetworkManager::publishACK("eco", detail);
            }
            isValidCommand = true;
        }

        if (doc["teco"]) {
            sysData.TEcoTime = doc["teco"].as<unsigned long>() * 60000;
            preferences.putULong("eco_time", sysData.TEcoTime);
            Serial.printf("Updated TEcoTime: %lu ms\n", sysData.TEcoTime);
            char detail[32];
            snprintf(detail, sizeof(detail), "teco=%lu_min", doc["teco"].as<unsigned long>());
            NetworkManager::publishACK("teco", detail);
            isValidCommand = true;
        }

        if (doc["toff"]) {
            sysData.TOffTime = doc["toff"].as<unsigned long>() * 60000;
            if (sysData.TOffTime <= sysData.TEcoTime) {
                sysData.TOffTime = sysData.TEcoTime + 60000;
                Serial.println("WARNING: TOffTime was <= TEcoTime. Auto-corrected.");
            }
            preferences.putULong("off_time", sysData.TOffTime);
            Serial.printf("Updated TOffTime: %lu ms\n", sysData.TOffTime);
            char detail[32];
            snprintf(detail, sizeof(detail), "toff=%lu_min", doc["toff"].as<unsigned long>());
            NetworkManager::publishACK("toff", detail);
            isValidCommand = true;
        }

        if (doc["ir"]) {
            int cmdNum = doc["ir"].as<int>();
            Serial.printf("Received IR Command Code: %d\n", cmdNum);
            char detail[32];

            if (cmdNum == 1) {
                AutomationManager::executeACCommand(true, sysData.currentNormalTemp, "manual_on");
                sysData.acAutoState = AUTO_ON_NORMAL;
                snprintf(detail, sizeof(detail), "ir_on");
            } else if (cmdNum == 2) {
                AutomationManager::executeACCommand(false, 24, "manual_off");
                sysData.acAutoState = AUTO_OFF;
                snprintf(detail, sizeof(detail), "ir_off");
            } else if (cmdNum >= 3 && cmdNum <= 17) {
                int targetTemp = cmdNum + 13;
                AutomationManager::executeACCommand(true, targetTemp, "manual_temp");
                sysData.acAutoState = AUTO_ON_NORMAL;
                snprintf(detail, sizeof(detail), "ir_temp=%d", targetTemp);
            } else {
                snprintf(detail, sizeof(detail), "ir_invalid=%d", cmdNum);
                NetworkManager::publishACK("ir", detail);
            }
            isValidCommand = true;
        }

        if (doc["protocol"]) {
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