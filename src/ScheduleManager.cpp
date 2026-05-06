#include <Arduino.h>
#include "config.h"
#include "SharedState.h"
#include "Preferences.h"
#include "ScheduleManager.h"
#include "IRManager.h"
#include "AutomationManager.h"
#include "Indicator.h"
#include "NetworkManager.h"

extern Preferences preferences;

namespace ScheduleManager
{
    int lastScheduledMin = -1;  
    int lastScheduledWday = -1; 

    int dayNameToWday(const String &day)
    {
        String d = day;
        d.toLowerCase();
        if (d == "sunday") return 0;
        if (d == "monday") return 1;
        if (d == "tuesday") return 2;
        if (d == "wednesday") return 3;
        if (d == "thursday") return 4;
        if (d == "friday") return 5;
        if (d == "saturday") return 6;
        return -1;
    }

    uint8_t loadSegmentCount(int wday)
    {
        char key[13];
        snprintf(key, sizeof(key), "sch_cnt_%d", wday);
        return preferences.getUChar(key, 0);
    }

    void refreshHasAnySchedule()
    {
        for (int d = 0; d < 7; d++)
        {
            if (loadSegmentCount(d) > 0)
            {
                sysData.hasAnySchedule = true;
                Serial.println("[SCHED] hasAnySchedule = true (at least one day configured)");
                return;
            }
        }
        sysData.hasAnySchedule = false;
        Serial.println("[SCHED] hasAnySchedule = false (no segments — radar runs unconditionally)");
    }

    bool loadSegment(int wday, int idx, ScheduleSegment &out)
    {
        char key[13];
        snprintf(key, sizeof(key), "sch_seg_%d", wday);
        size_t len = preferences.getBytesLength(key);
        if (len < (size_t)((idx + 1) * 9))
            return false;
        uint8_t buf[MAX_SEGS_PER_DAY * 9];
        preferences.getBytes(key, buf, len);
        int o = idx * 9;
        out.startMin = (uint16_t)(buf[o] | (buf[o + 1] << 8));
        out.endMin = (uint16_t)(buf[o + 2] | (buf[o + 3] << 8));
        out.temp = buf[o + 4];
        out.radar = buf[o + 5];
        out.eco = buf[o + 6];
        out.teco = buf[o + 7];
        out.toff = buf[o + 8];
        return true;
    }

    int findSegmentTemp(int wday, int minute)
    {
        uint8_t count = loadSegmentCount(wday);
        for (uint8_t i = 0; i < count; i++)
        {
            ScheduleSegment seg;
            if (!loadSegment(wday, i, seg))
                continue;
            if (minute >= (int)seg.startMin && minute < (int)seg.endMin)
                return seg.temp;
        }
        return 0;
    }

    bool findSegment(int wday, int minute, ScheduleSegment &out)
    {
        uint8_t count = loadSegmentCount(wday);
        for (uint8_t i = 0; i < count; i++)
        {
            if (!loadSegment(wday, i, out))
                continue;
            if (minute >= (int)out.startMin && minute < (int)out.endMin)
                return true;
        }
        return false;
    }

    bool sameSegment(const ScheduleSegment &a, const ScheduleSegment &b)
    {
        return a.startMin == b.startMin &&
               a.endMin == b.endMin &&
               a.temp == b.temp &&
               a.radar == b.radar &&
               a.eco == b.eco &&
               a.teco == b.teco &&
               a.toff == b.toff;
    }

    void saveSchedule(int wday, const ScheduleSegment segs[], uint8_t count,
                      const String &irHex, int irTemp)
    {
        char key[13];

        uint8_t buf[MAX_SEGS_PER_DAY * 9];
        for (uint8_t i = 0; i < count; i++)
        {
            int o = i * 9;
            buf[o] = segs[i].startMin & 0xFF;
            buf[o + 1] = (segs[i].startMin >> 8) & 0xFF;
            buf[o + 2] = segs[i].endMin & 0xFF;
            buf[o + 3] = (segs[i].endMin >> 8) & 0xFF;
            buf[o + 4] = segs[i].temp;
            buf[o + 5] = segs[i].radar;
            buf[o + 6] = segs[i].eco;
            buf[o + 7] = segs[i].teco;
            buf[o + 8] = segs[i].toff;
        }
        snprintf(key, sizeof(key), "sch_cnt_%d", wday);
        preferences.putUChar(key, count);
        snprintf(key, sizeof(key), "sch_seg_%d", wday);
        preferences.putBytes(key, buf, count * 9);

        snprintf(key, sizeof(key), "sch_ir_%d", wday);
        preferences.putString(key, irHex);
        snprintf(key, sizeof(key), "sch_irt_%d", wday);
        preferences.putInt(key, irTemp);
    }

    bool sendScheduleIR(int wday, int targetTemp)
    {
        char key[13];
        snprintf(key, sizeof(key), "sch_irt_%d", wday);
        int storedTemp = preferences.getInt(key, -1);
        if (storedTemp != targetTemp)
            return false;

        snprintf(key, sizeof(key), "sch_ir_%d", wday);
        String irHex = preferences.getString(key, "");
        if (irHex.length() == 0)
            return false;

        bool digitsOnly = true;
        for (size_t i = 0; i < irHex.length(); i++)
        {
            if (!isDigit((unsigned char)irHex[i]))
            {
                digitsOnly = false;
                break;
            }
        }
        if (digitsOnly && irHex.length() <= 4)
        {
            int cmdNum = irHex.toInt();
            if (cmdNum >= 1 && cmdNum <= 17)
            {
                if (cmdNum == 1)
                {
                    if (!IRManager::playCustomButton("ir_on"))
                        IRManager::sendACFallback(true, targetTemp);
                }
                else if (cmdNum == 2)
                {
                    if (!IRManager::playCustomButton("ir_off"))
                        IRManager::sendACFallback(false, 24);
                }
                else
                {
                    int cmdTemp = cmdNum + 13;
                    String customKey = "ir_" + String(cmdTemp);
                    if (!IRManager::playCustomButton(customKey.c_str()))
                        IRManager::sendACFallback(true, cmdTemp);
                }
                Serial.printf("[SCHED] Dashboard IR command %s sent for %dC schedule.\n",
                              irHex.c_str(), targetTemp);
                return true;
            }
        }

        String proto = "";
        if (preferences.isKey("protocol_name"))
        {
            proto = preferences.getString("protocol_name", "");
        }

        uint64_t code = strtoull(irHex.c_str(), NULL, 16);
        bool success = IRManager::sendDynamicCode(proto.c_str(), code, 32);

        if (success)
        {
            Serial.printf("[SCHED] Schedule IR sent: 0x%s at %d°C\n", irHex.c_str(), targetTemp);
            return true;
        }
        return false;
    }

    void applySegmentRadar(uint8_t radarSetting)
    {
        if (sysData.radarManualOverride)
            return;
        if (radarSetting == 0)
            return; 

        if (radarSetting == 1 && !sysData.radarAutoMode)
        {
            sysData.radarAutoMode = true;
            sysData.lastPresenceTime = millis();
            preferences.putBool("radar_auto", true);
            Serial.println("[SCHED] Segment enabled radar automation.");
        }
        else if (radarSetting == 2 && sysData.radarAutoMode)
        {
            sysData.radarAutoMode = false;
            preferences.putBool("radar_auto", false);
            Serial.println("[SCHED] Segment disabled radar automation.");
        }
    }

    void applySegmentParams(const ScheduleSegment &seg)
    {
        if (seg.eco > 0)
        {
            sysData.currentEcoTemp = seg.eco;
            preferences.putInt("eco_temp", sysData.currentEcoTemp);
        }
        if (seg.teco > 0)
        {
            sysData.TEcoTime = (unsigned long)seg.teco * 60000UL;
            preferences.putULong("eco_time", sysData.TEcoTime);
        }
        if (seg.toff > 0)
        {
            sysData.TOffTime = (unsigned long)seg.toff * 60000UL;
            if (sysData.TOffTime <= sysData.TEcoTime)
            {
                sysData.TOffTime = sysData.TEcoTime + 60000UL;
                Serial.println("[SCHED] TOffTime auto-corrected.");
            }
            preferences.putULong("off_time", sysData.TOffTime);
        }
    }

    void handleScheduleCommand(JsonDocument &doc)
    {
        const char *day = doc["day"];
        if (!day)
        {
            Serial.println("[SCHED] Missing 'day', ignoring.");
            return;
        }

        int wday = dayNameToWday(String(day));
        if (wday < 0)
        {
            Serial.printf("[SCHED] Unknown day '%s', ignoring.\n", day);
            return;
        }

        if (!doc["segments"])
        {
            Serial.println("[SCHED] Missing 'segments' key, ignoring.");
            return;
        }

        JsonArray segsArray = doc["segments"].as<JsonArray>();

        ScheduleSegment segs[MAX_SEGS_PER_DAY];
        uint8_t count = 0;

        for (JsonVariant v : segsArray)
        {
            if (count >= MAX_SEGS_PER_DAY)
            {
                Serial.printf("[SCHED] WARNING: More than %d segments — truncating.\n", MAX_SEGS_PER_DAY);
                break;
            }
            int start = v["start"] | -1;
            int end = v["end"] | -1;
            int temp = v["temp"] | 0;

            if (start < 0 || start > 1439 || end <= start) continue;
            
            if (end > 1440) end = 1440; 
            
            if (temp < 16 || temp > 32) continue;
            
            uint8_t segRadar = 0;
            if (v["radar"])
            {
                String r = v["radar"].as<String>();
                if (r == "0100" || r == "256" || r == "on")
                    segRadar = 1;
                else if (r == "0200" || r == "512" || r == "off")
                    segRadar = 2;
            }

            uint8_t segEco = (uint8_t)(v["eco"] | 0);
            uint8_t segTeco = (uint8_t)(v["teco"] | 0);
            uint8_t segToff = (uint8_t)(v["toff"] | 0);

            segs[count++] = {(uint16_t)start, (uint16_t)end, (uint8_t)temp, segRadar, segEco, segTeco, segToff};
        }

        if (count > 0)
        {
            for (int i = 1; i < count; i++)
            {
                ScheduleSegment tmp = segs[i];
                int j = i - 1;
                while (j >= 0 && segs[j].startMin > tmp.startMin)
                {
                    segs[j + 1] = segs[j];
                    j--;
                }
                segs[j + 1] = tmp;
            }

            for (int i = 0; i < count - 1; i++)
            {
                if (segs[i].endMin > segs[i + 1].startMin)
                {
                    Serial.println("[SCHED] ERROR: Overlapping segments detected — schedule rejected.");
                    return;
                }
            }
        }
        else
        {
            Serial.printf("[SCHED] Empty segments array received. Wiping schedule for %s.\n", day);
        }

        String irHex = doc["ir"] | "";
        int irTemp = doc["temperature_setting"] | 0;

        saveSchedule(wday, segs, count, irHex, irTemp);

        refreshHasAnySchedule();

        sysData.radarManualOverride = false;
        preferences.putBool("rad_ovr", false);

        struct tm timeinfo;
        if (getLocalTime(&timeinfo) && timeinfo.tm_wday == wday)
        {
            lastScheduledMin = -1;
            lastScheduledWday = -1;
        }

        Serial.printf("[SCHED] %d segment(s) saved for %s (wday=%d)\n", count, day, wday);
        Indicator::indicateSuccess();

        NetworkManager::publishACK(count == 0 ? "schedule_cleared" : "schedule_saved", day);
    }

    void loop()
    {
        struct tm timeinfo;
        if (!getLocalTime(&timeinfo, 0))
            return; 

        int currentMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
        int currentWday = timeinfo.tm_wday; 

        bool isBootRun = (lastScheduledMin == -1 || lastScheduledWday == -1);
        if (!isBootRun && currentMin == lastScheduledMin && currentWday == lastScheduledWday)
            return; 

        int prevMin = lastScheduledMin;
        int prevWday = lastScheduledWday;

        lastScheduledMin = currentMin;
        lastScheduledWday = currentWday;

        if (isBootRun)
        {
            uint8_t count = loadSegmentCount(currentWday);
            if (count == 0)
                return; 
            ScheduleSegment seg;
            if (findSegment(currentWday, currentMin, seg))
            {
                applySegmentRadar(seg.radar);
                applySegmentParams(seg);
                sysData.currentNormalTemp = seg.temp;
                preferences.putInt("normal_temp", seg.temp);
                if (!sendScheduleIR(currentWday, seg.temp))
                {
                    AutomationManager::executeACCommand(true, seg.temp, "boot_schedule_on");
                }
                else
                {
                    Indicator::indicateIRSent();
                    char detail[32];
                    snprintf(detail, sizeof(detail), "state=ON,temp=%d", seg.temp);
                    NetworkManager::publishACK("boot_schedule_on", detail);
                    sysData.lastCommandTime = millis();
                }
                sysData.acAutoState = AUTO_ON_NORMAL;
                Serial.printf("[SCHED] Boot %02d:%02d — inside segment, AC ON at %d°C\n",
                              timeinfo.tm_hour, timeinfo.tm_min, seg.temp);
            }
            else
            {
                if (!IRManager::playCustomButton("ir_off"))
                    IRManager::sendACFallback(false, 24);
                sysData.acAutoState = AUTO_OFF;
                Indicator::indicateIRSent();
                Serial.printf("[SCHED] Boot %02d:%02d — outside segments, AC OFF\n",
                              timeinfo.tm_hour, timeinfo.tm_min);
            }
            return;
        }

        ScheduleSegment currentSeg;
        ScheduleSegment prevSeg;
        bool hasCurrentSeg = findSegment(currentWday, currentMin, currentSeg);
        bool hasPrevSeg = findSegment(prevWday, prevMin, prevSeg);
        sysData.isInsideSchedule = hasCurrentSeg;

        if (!hasCurrentSeg && !hasPrevSeg)
            return;
        if (hasCurrentSeg && hasPrevSeg && sameSegment(currentSeg, prevSeg))
            return;

        if (hasCurrentSeg)
        {
            applySegmentRadar(currentSeg.radar);
            applySegmentParams(currentSeg);

            if (!hasPrevSeg || currentSeg.temp != prevSeg.temp)
            {
                sysData.currentNormalTemp = currentSeg.temp;
                preferences.putInt("normal_temp", currentSeg.temp);
                if (!sendScheduleIR(currentWday, currentSeg.temp))
                {
                    AutomationManager::executeACCommand(true, currentSeg.temp, "schedule_on");
                }
                else
                {
                    Indicator::indicateIRSent();
                    char detail[32];
                    snprintf(detail, sizeof(detail), "state=ON,temp=%d", currentSeg.temp);
                    NetworkManager::publishACK("schedule_on", detail);
                    sysData.lastCommandTime = millis(); 
                }
                sysData.acAutoState = AUTO_ON_NORMAL;
                Serial.printf("[SCHED] %02d:%02d -> ON at %dC\n",
                              timeinfo.tm_hour, timeinfo.tm_min, currentSeg.temp);
            }
            else
            {
                sysData.acAutoState = AUTO_ON_NORMAL;
                Serial.printf("[SCHED] %02d:%02d -> segment updated, AC remains at %dC\n",
                              timeinfo.tm_hour, timeinfo.tm_min, currentSeg.temp);
            }
        }
        else
        {
            AutomationManager::executeACCommand(false, 24, "schedule_off");
            sysData.acAutoState = AUTO_OFF;
            Serial.printf("[SCHED] %02d:%02d -> AC OFF (gap/end)\n",
                          timeinfo.tm_hour, timeinfo.tm_min);
        }
    }
}