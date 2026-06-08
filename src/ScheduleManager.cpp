/**
 * @file ScheduleManager.cpp
 * @brief Implementation of the NVS-backed 7-day time-segment scheduler.
 *
 * Segments are packed to 9 bytes and stored per weekday (see save/loadSegment).
 * TaskSchedule sleeps until each exact minute boundary, then applies the active
 * segment's settings and sends IR as needed; it also distinguishes a "boot run"
 * (first evaluation after power-up, which may need to catch up the AC state) from
 * steady-state minute transitions. handleScheduleCommand() ingests backend
 * "segments" updates (validate -> sort -> overlap-check -> persist -> apply).
 * An offline failsafe forces radar control when no network time is available.
 */
#include <Arduino.h>
#include "config.h"
#include "SharedState.h"
#include "Preferences.h"
#include "ScheduleManager.h"
#include "IRManager.h"
#include "AutomationManager.h"
#include "Indicator.h"
#include "NetworkManager.h"
#include "esp_log.h"

static const char *TAG = "SCHED";

extern Preferences preferences;

namespace ScheduleManager
{
    int lastScheduledMin = -1;   ///< Minute-of-day of the last evaluation (-1 = not yet run).
    int lastScheduledWday = -1;  ///< Weekday of the last evaluation (-1 = not yet run).

    /// @return Weekday index 0(Sun)-6(Sat) for an English day name, or -1 if unknown.
    int dayNameToWday(const String &day)
    {
        String d = day;
        d.toLowerCase();
        if (d == "sunday")
            return 0;
        if (d == "monday")
            return 1;
        if (d == "tuesday")
            return 2;
        if (d == "wednesday")
            return 3;
        if (d == "thursday")
            return 4;
        if (d == "friday")
            return 5;
        if (d == "saturday")
            return 6;
        return -1;
    }

    /// @return Stored segment count for @p wday (NVS key "sch_cnt_<wday>"), 0 if none.
    uint8_t loadSegmentCount(int wday)
    {
        char key[13];
        snprintf(key, sizeof(key), "sch_cnt_%d", wday);
        return preferences.getUChar(key, 0);
    }

    /// Set sysData.hasAnySchedule true if any of the 7 days has >=1 segment.
    void refreshHasAnySchedule()
    {
        for (int d = 0; d < 7; d++)
        {
            if (loadSegmentCount(d) > 0)
            {
                sysData.hasAnySchedule = true;
                ESP_LOGI(TAG, "hasAnySchedule = true (at least one day configured)");
                return;
            }
        }
        sysData.hasAnySchedule = false;
        ESP_LOGI(TAG, "hasAnySchedule = false (no segments — radar runs unconditionally)");
    }

    /**
     * @brief Load one segment from the packed per-day NVS blob.
     * @param wday Weekday 0-6. @param idx Segment index. @param out Filled on success.
     * @return true if the blob holds at least @p idx+1 segments.
     */
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

    /// @return Temperature of the segment covering @p minute on @p wday, or 0 if none.
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

    /// Find the segment covering @p minute on @p wday into @p out. @return true if found.
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

    /// @return true if every field of the two segments is identical.
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

    /**
     * @brief Persist a day's segments (packed) plus its optional IR code/temp to NVS.
     * @param wday  Weekday 0-6.       @param segs  Sorted segment array.
     * @param count Number of segments. @param irHex Raw IR code/command for the day.
     * @param irTemp Temperature the stored IR code corresponds to.
     */
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

    /**
     * @brief Send the day's stored IR command for @p targetTemp, if one matches.
     *
     * Handles both forms of stored code: a small dashboard command number
     * (1=ON, 2=OFF, 3-17=temperature) routed to the learned custom buttons, or
     * a raw hex code sent via the saved protocol.
     * @return true if a matching IR command was actually sent.
     */
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
                ESP_LOGI(TAG, "Dashboard IR command %s sent for %dC schedule.",
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
            ESP_LOGI(TAG, "Schedule IR sent: 0x%s at %d°C", irHex.c_str(), targetTemp);
            return true;
        }
        return false;
    }

    /**
     * @brief Apply a segment's per-segment radar setting (unless user-overridden).
     * @param radarSetting 0 = leave as-is, 1 = enable radar auto, 2 = disable.
     */
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
            ESP_LOGI(TAG, "Segment enabled radar automation.");
        }
        else if (radarSetting == 2 && sysData.radarAutoMode)
        {
            sysData.radarAutoMode = false;
            preferences.putBool("radar_auto", false);
            ESP_LOGI(TAG, "Segment disabled radar automation.");
        }
    }

    /**
     * @brief Apply a segment's eco temp / eco time / off time overrides to state + NVS.
     *
     * Each override only takes effect when non-zero (0 = inherit the global value).
     * Off time is auto-corrected to always exceed eco time.
     */
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
                ESP_LOGI(TAG, "TOffTime auto-corrected.");
            }
            preferences.putULong("off_time", sysData.TOffTime);
        }
    }

    /**
     * @brief Ingest and apply a backend "segments" schedule-update command.
     *
     * Steps: resolve the day, parse + validate each segment (range/temp checks),
     * insertion-sort by start time, reject overlaps, persist, and refresh
     * hasAnySchedule. If the updated day is today, immediately re-evaluate the
     * current minute so the AC/radar reflect the new schedule at once (taking
     * care not to disturb a state radar is already managing). Clears any prior
     * manual radar override.
     */
    void handleScheduleCommand(JsonDocument &doc)
    {
        const char *day = doc["day"];
        if (!day)
        {
            ESP_LOGW(TAG, "Missing 'day', ignoring.");
            return;
        }

        int wday = dayNameToWday(String(day));
        if (wday < 0)
        {
            ESP_LOGW(TAG, "Unknown day '%s', ignoring.", day);
            return;
        }

        if (!doc["segments"])
        {
            ESP_LOGW(TAG, "Missing 'segments' key, ignoring.");
            return;
        }

        JsonArray segsArray = doc["segments"].as<JsonArray>();

        ScheduleSegment segs[MAX_SEGS_PER_DAY];
        uint8_t count = 0;

        for (JsonVariant v : segsArray)
        {
            if (count >= MAX_SEGS_PER_DAY)
            {
                ESP_LOGW(TAG, "More than %d segments — truncating.", MAX_SEGS_PER_DAY);
                break;
            }
            int start = v["start"] | -1;
            int end = v["end"] | -1;
            int temp = v["temp"] | 0;

            if (start < 0 || start > 1439 || end <= start)
                continue;

            if (end > 1440)
                end = 1440;

            if (temp < 16 || temp > 32)
                continue;

            uint8_t segRadar = 0;
            if (v["radar"])
            {
                String r = v["radar"].as<String>();
                if (r == "0100" || r == "256" || r == "on")
                    segRadar = 1;
                else if (r == "0200" || r == "512" || r == "off")
                    segRadar = 2;
            }

            // Backend may send these as strings ("1") or integers (1) — handle both.
            auto jToU8 = [](JsonVariant val) -> uint8_t
            {
                if (val.is<int>())
                    return (uint8_t)val.as<int>();
                const char *s = val | "";
                return (uint8_t)atoi(s);
            };
            uint8_t segEco = jToU8(v["eco"]);
            uint8_t segTeco = jToU8(v["teco"]);
            uint8_t segToff = jToU8(v["toff"]);

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
                    ESP_LOGE(TAG, "Overlapping segments detected — schedule rejected.");
                    return;
                }
            }
        }
        else
        {
            ESP_LOGI(TAG, "Empty segments array received. Wiping schedule for %s.", day);
        }

        String irHex = doc["ir"] | "";
        int irTemp = doc["temperature_setting"] | 0;

        saveSchedule(wday, segs, count, irHex, irTemp);

        refreshHasAnySchedule();

        sysData.radarManualOverride = false;
        preferences.putBool("rad_ovr", false);

        // Immediately re-evaluate if this schedule is for today
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 0) && timeinfo.tm_wday == wday)
        {
            int currentMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
            ScheduleSegment seg;
            bool insideSeg = findSegment(wday, currentMin, seg);
            sysData.isInsideSchedule = insideSeg;

            if (insideSeg)
            {
                applySegmentParams(seg);
                applySegmentRadar(seg.radar);
                sysData.currentNormalTemp = seg.temp;
                preferences.putInt("normal_temp", seg.temp);

                if (sysData.acAutoState == AUTO_ON_ECO)
                {
                    // Radar already drove AC to eco — silently update settings, let radar continue managing.
                    // (Only AUTO_ON_ECO is "managed by radar"; AUTO_OFF after a delete must be re-entered.)
                    ESP_LOGI(TAG, "Schedule updated while in eco mode. Settings applied, no IR sent.");
                }
                else
                {
                    // AUTO_OFF (e.g. after a delete) or AUTO_ON_NORMAL: enter/re-assert segment normally.
                    // Stop any stale eco/off timers — when AC is turning ON, no countdown should be running.
                    // These will be re-started below ONLY if the room is genuinely empty.
                    xTimerStop(ecoTimer, 0);
                    xTimerStop(offTimer, 0);

                    if (!sendScheduleIR(wday, seg.temp))
                    {
                        AutomationManager::executeACCommand(true, seg.temp, "schedule_update");
                    }
                    else
                    {
                        xTimerReset(enforceTimer, 0);
                        Indicator::indicateIRSent();
                        char detail[32];
                        snprintf(detail, sizeof(detail), "state=ON,temp=%d", seg.temp);
                        NetworkManager::publishACK("schedule_update", detail);
                        sysData.lastCommandTime = millis();
                    }
                    sysData.acAutoState = AUTO_ON_NORMAL;

                    // Start eco/off timers ONLY if room is genuinely empty.
                    // If flap delay is active (e.g. delete just ran), cachedPresence is artificially
                    // forced to false and we cannot trust it. SensorManager will fire a presence event
                    // when the flap delay expires, which will start timers correctly if needed.
                    if (sysData.radarAutoMode && !sysData.cachedPresence && !sysData.isFlapDelayActive)
                    {
                        if (sysData.TEcoTime > 0)
                            xTimerChangePeriod(ecoTimer, pdMS_TO_TICKS(sysData.TEcoTime), 0);
                        if (sysData.TOffTime > 0)
                            xTimerChangePeriod(offTimer, pdMS_TO_TICKS(sysData.TOffTime), 0);
                    }
                }
            }
            else
            {
                AutomationManager::executeACCommand(false, 24, "schedule_update_off");
                sysData.acAutoState = AUTO_OFF;
                if (sysData.radarAutoMode)
                {
                    sysData.radarAutoMode = false;
                    preferences.putBool("radar_auto", false);
                    ESP_LOGI(TAG, "Schedule update: outside segments, radar disabled.");
                }
                sysData.radarManualOverride = false;
                preferences.putBool("rad_ovr", false);
            }

            // Sync task memory so it doesn't re-process this same minute
            lastScheduledMin = currentMin;
            lastScheduledWday = wday;
        }

        ESP_LOGI(TAG, "%d segment(s) saved for %s (wday=%d)", count, day, wday);
        Indicator::indicateSuccess();

        NetworkManager::publishACK(count == 0 ? "schedule_cleared" : "schedule_saved", day);
    }

    /**
     * @brief FreeRTOS task: minute-aligned schedule evaluation + offline failsafe.
     *
     * While the clock is invalid (NTP/GSM not yet synced) it waits, and after 60s
     * offline it engages the failsafe — forcing radar control so the room is still
     * automated locally (magenta LED). Once time is valid it sleeps precisely until
     * the next :00 second, then evaluates the current minute:
     *   - Boot run (first evaluation): catch the AC up to where the schedule says it
     *     should be, while respecting any decision radar already made during boot.
     *   - Steady state: act only on real segment transitions (enter/leave/change).
     * @note Spawned once; never returns.
     */
    void TaskSchedule(void *pvParameters)
    {
        // Track boot time for the offline failsafe
        static unsigned long offlineBootStart = millis();
        static bool offlineFailsafeTriggered = false;

        for (;;)
        {
            struct timeval tv;
            gettimeofday(&tv, NULL);

            // 1. Time validity check: If epoch is before 2021, NTP hasn't synced.
            if (tv.tv_sec < 1609459200)
            {
                // --- ROBUST OFFLINE FAILSAFE ---
                // If 60 seconds have passed and we still have no network time,
                // prioritize local automation and force the radar to take over.
                if (!offlineFailsafeTriggered && (millis() - offlineBootStart > 60000))
                {
                    ESP_LOGW(TAG, "Offline timeout! Prioritizing local automation. Forcing radar ON.");
                    sysData.radarAutoMode = true;
                    offlineFailsafeTriggered = true;
                    sysData.isOfflineFailsafeActive = true; // <--- ADD THIS: Turn on Magenta

                    // Push an event to the queue immediately.
                    // This ensures the AC turns on if you are already standing in the room!
                    SystemEvent event;
                    event.type = EVENT_PRESENCE_CHANGED;
                    event.payload = sysData.cachedPresence ? 1 : 0;
                    xQueueSend(automationQueue, &event, 0);
                }

                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }

            // Once time is valid, reset the tracker so we know we are online
            offlineFailsafeTriggered = false;

            sysData.isOfflineFailsafeActive = false; // <--- ADD THIS: Turn off Magenta

            // 2. Calculate EXACT milliseconds until the top of the next minute (00 seconds)
            int current_sec = tv.tv_sec % 60;
            int current_ms = tv.tv_usec / 1000;
            uint32_t ms_to_next_minute = 60000 - ((current_sec * 1000) + current_ms);

            // 3. Sleep dynamically. CPU uses 0% power here.
            vTaskDelay(pdMS_TO_TICKS(ms_to_next_minute));

            // --- WAKING UP: It is now exactly XX:XX:00 ---

            struct tm timeinfo;
            if (!getLocalTime(&timeinfo, 0))
                continue;

            int currentMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
            int currentWday = timeinfo.tm_wday;

            bool isBootRun = (lastScheduledMin == -1 || lastScheduledWday == -1);
            if (!isBootRun && currentMin == lastScheduledMin && currentWday == lastScheduledWday)
                continue;

            int prevMin = lastScheduledMin;
            int prevWday = lastScheduledWday;

            lastScheduledMin = currentMin;
            lastScheduledWday = currentWday;

            // ... The rest of your existing logic starts here
            if (isBootRun)
            {
                uint8_t count = loadSegmentCount(currentWday);
                if (count == 0)
                {
                    sysData.isInsideSchedule = false;

                    // --- FAILSAFE RECOVERY SHUTDOWN ---
                    // If the failsafe was running, but today has no schedules at all,
                    // we must explicitly kill the radar and turn the AC off.
                    if (sysData.radarAutoMode)
                    {
                        sysData.radarAutoMode = false;
                        preferences.putBool("radar_auto", false);
                        ESP_LOGI(TAG, "Recovery: No schedules for today. Radar disabled.");

                        if (!IRManager::playCustomButton("ir_off"))
                            IRManager::sendACFallback(false, 24);

                        sysData.acAutoState = AUTO_OFF;
                        Indicator::indicateIRSent();
                    }
                    // ----------------------------------

                    continue;
                }
                ScheduleSegment seg;
                if (findSegment(currentWday, currentMin, seg))
                {
                    sysData.isInsideSchedule = true;
                    applySegmentParams(seg);
                    applySegmentRadar(seg.radar);
                    sysData.currentNormalTemp = seg.temp;
                    preferences.putInt("normal_temp", seg.temp);

                    // Check if radar already made an AC decision during the offline wait.
                    // AUTO_ON_ECO is only ever set by radar — schedule always enters at NORMAL.
                    // AUTO_OFF with lastCommandTime > 0 means radar drove the AC off (not boot default).
                    AutoState curState = sysData.acAutoState.load();
                    bool radarDroveOff = sysData.radarAutoMode &&
                                         (curState == AUTO_ON_ECO ||
                                          (curState == AUTO_OFF && sysData.lastCommandTime > 0));

                    if (radarDroveOff)
                    {
                        // Radar is managing. Apply schedule settings silently so the correct
                        // temp/eco/off values are ready when the room becomes occupied again.
                        // Re-arm offTimer only if currently in eco (eco already fired, off hasn't).
                        if (curState == AUTO_ON_ECO && sysData.TOffTime > 0)
                            xTimerChangePeriod(offTimer, pdMS_TO_TICKS(sysData.TOffTime), 0);

                        ESP_LOGI(TAG, "Boot %02d:%02d — radar managed (%s), schedule params applied, no IR",
                                 timeinfo.tm_hour, timeinfo.tm_min,
                                 curState == AUTO_ON_ECO ? "eco" : "off");
                    }
                    else
                    {
                        // Normal boot: no radar decision yet — turn AC on per schedule.
                        if (!sendScheduleIR(currentWday, seg.temp))
                        {
                            AutomationManager::executeACCommand(true, seg.temp, "boot_schedule_on");
                        }
                        else
                        {
                            xTimerReset(enforceTimer, 0);
                            Indicator::indicateIRSent();
                            char detail[32];
                            snprintf(detail, sizeof(detail), "state=ON,temp=%d", seg.temp);
                            NetworkManager::publishACK("boot_schedule_on", detail);
                            sysData.lastCommandTime = millis();
                        }
                        sysData.acAutoState = AUTO_ON_NORMAL;
                        if (sysData.radarAutoMode && !sysData.cachedPresence)
                        {
                            ESP_LOGI(TAG, "AC ON via schedule, but room is empty. Starting timers.");
                            if (sysData.TEcoTime > 0)
                                xTimerChangePeriod(ecoTimer, pdMS_TO_TICKS(sysData.TEcoTime), 0);
                            if (sysData.TOffTime > 0)
                                xTimerChangePeriod(offTimer, pdMS_TO_TICKS(sysData.TOffTime), 0);
                        }
                        ESP_LOGI(TAG, "Boot %02d:%02d — inside segment, AC ON at %d°C",
                                 timeinfo.tm_hour, timeinfo.tm_min, seg.temp);
                    }
                }
                else
                {
                    sysData.isInsideSchedule = false;
                    if (!IRManager::playCustomButton("ir_off"))
                        IRManager::sendACFallback(false, 24);
                    sysData.acAutoState = AUTO_OFF;
                    // ---> ADD THIS BLOCK: Hard toggle radar OFF <---
                    if (sysData.radarAutoMode)
                    {
                        sysData.radarAutoMode = false;
                        preferences.putBool("radar_auto", false);
                        ESP_LOGI(TAG, "Schedule gap reached: Radar automation disabled.");
                    }
                    // -----------------------------------------------
                    Indicator::indicateIRSent();
                    ESP_LOGI(TAG, "Boot %02d:%02d — outside segments, AC OFF",
                             timeinfo.tm_hour, timeinfo.tm_min);
                }
                continue;
            }

            ScheduleSegment currentSeg;
            ScheduleSegment prevSeg;
            bool hasCurrentSeg = findSegment(currentWday, currentMin, currentSeg);
            bool hasPrevSeg = findSegment(prevWday, prevMin, prevSeg);
            sysData.isInsideSchedule = hasCurrentSeg;

            if (!hasCurrentSeg && !hasPrevSeg)
                continue;
            if (hasCurrentSeg && hasPrevSeg && sameSegment(currentSeg, prevSeg))
                continue;

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
                        xTimerReset(enforceTimer, 0);
                        Indicator::indicateIRSent();
                        char detail[32];
                        snprintf(detail, sizeof(detail), "state=ON,temp=%d", currentSeg.temp);
                        NetworkManager::publishACK("schedule_on", detail);
                        sysData.lastCommandTime = millis();
                    }
                    sysData.acAutoState = AUTO_ON_NORMAL;
                    // ---> THE FIX: Jumpstart timers if room is already empty on boot <---
                    if (sysData.radarAutoMode && !sysData.cachedPresence)
                    {
                        ESP_LOGI(TAG, "AC ON via boot schedule, but room is empty. Starting timers.");
                        if (sysData.TEcoTime > 0)
                            xTimerChangePeriod(ecoTimer, pdMS_TO_TICKS(sysData.TEcoTime), 0);
                        if (sysData.TOffTime > 0)
                            xTimerChangePeriod(offTimer, pdMS_TO_TICKS(sysData.TOffTime), 0);
                    }
                    ESP_LOGI(TAG, "%02d:%02d -> ON at %dC",
                             timeinfo.tm_hour, timeinfo.tm_min, currentSeg.temp);
                }
                else
                {
                    // sysData.acAutoState = AUTO_ON_NORMAL;  //This state is already set from the previous segment, no need to set again
                    ESP_LOGI(TAG, "%02d:%02d -> segment updated, AC remains at %dC",
                             timeinfo.tm_hour, timeinfo.tm_min, currentSeg.temp);
                }
            }
            else
            {
                AutomationManager::executeACCommand(false, 24, "schedule_off");
                sysData.acAutoState = AUTO_OFF;
                if (sysData.radarAutoMode)
                {
                    sysData.radarAutoMode = false;
                    preferences.putBool("radar_auto", false);
                    ESP_LOGI(TAG, "Schedule end: Radar automation disabled.");
                }
                sysData.radarManualOverride = false;
                preferences.putBool("rad_ovr", false);
                ESP_LOGI(TAG, "%02d:%02d -> AC OFF (gap/end)",
                         timeinfo.tm_hour, timeinfo.tm_min);
            }
        }
    }
}