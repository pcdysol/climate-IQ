#include "AutomationManager.h"
#include "Indicator.h"
#include "Config.h"
#include "IRManager.h"
#include "Preferences.h"
#include "NetworkManager.h"

extern Preferences preferences;

namespace AutomationManager
{
    void executeACCommand(bool turnOn, int targetTemp, const char *triggerSource)
    {
        sysData.lastCommandTime = millis();
        String customKey = turnOn ? ("ir_" + String(targetTemp)) : "ir_off";

        if (!IRManager::playCustomButton(customKey.c_str()))
        {
            IRManager::sendACFallback(turnOn, targetTemp);
        }

        Indicator::indicateIRSent();

        char detail[64];
        snprintf(detail, sizeof(detail), "state=%s,temp=%d", turnOn ? "ON" : "OFF", targetTemp);
        NetworkManager::publishACK(triggerSource, detail);

        Serial.printf("[%s] IR Transmitted: %s\n", triggerSource, detail);
    }

    void loop()
    {
        if (!sysData.radarAutoMode || !sysData.sensorReady)
            return;

        struct tm tinfo;
        bool clockValid = getLocalTime(&tinfo, 0);

        if (sysData.hasAnySchedule && clockValid)
        {
            if (!sysData.isInsideSchedule)
            {
                uint8_t policy = preferences.getUChar("radar_out_pol", 0);
                if (policy != 1)
                    return;
            }
        }

        if (sysData.cachedPresence)
        {
            sysData.lastPresenceTime = millis();

            if (sysData.acAutoState != AUTO_ON_NORMAL)
            {
                executeACCommand(true, sysData.currentNormalTemp, "radar_presence");
                sysData.acAutoState = AUTO_ON_NORMAL;
                NetworkManager::sendAutomationEvent("1000");
            }
        }
        else
        {
            unsigned long emptyDuration = millis() - sysData.lastPresenceTime;

            if (sysData.acAutoState == AUTO_ON_NORMAL && emptyDuration >= sysData.TEcoTime && emptyDuration < sysData.TOffTime)
            {
                executeACCommand(true, sysData.currentEcoTemp, "radar_eco");
                sysData.acAutoState = AUTO_ON_ECO;
                NetworkManager::sendAutomationEvent("2000");
            }
            else if (sysData.acAutoState != AUTO_OFF && emptyDuration >= sysData.TOffTime)
            {
                executeACCommand(false, 24, "radar_off");
                sysData.acAutoState = AUTO_OFF;
                NetworkManager::sendAutomationEvent("3000");
            }
        }
    }

    void trackPresenceTime()
    {
        if (!sysData.sensorReady)
            return;

        unsigned long now = millis();
        if (sysData.cachedPresence != sysData.lastPresenceState)
        {
            if (sysData.lastPresenceState == true)
            {
                sysData.accumulatedPresenceMs += (now - sysData.lastStateChangeTime);
            }
            sysData.lastPresenceState = sysData.cachedPresence;
            sysData.lastStateChangeTime = now;
        }
    }

    void enforceACState()
    {
        const unsigned long ENFORCE_INTERVAL = 900000;

        if (millis() - sysData.lastCommandTime >= ENFORCE_INTERVAL)
        {
            sysData.lastCommandTime = millis();

            if (sysData.acAutoState == AUTO_ON_NORMAL)
                executeACCommand(true, sysData.currentNormalTemp, "enforce_normal");
            else if (sysData.acAutoState == AUTO_ON_ECO)
                executeACCommand(true, sysData.TEcoTime, "enforce_eco");
            else if (sysData.acAutoState == AUTO_OFF)
                executeACCommand(false, 24, "enforce_off");
        }
    }
}