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
#include "Config.h"
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

    /// @return the active segment's radar field for the current local time
    ///         (1 enable, 2 disable), or -1 if outside all segments / no synced clock.
    int currentSegmentRadar()
    {
        struct tm timeinfo;
        if (!getLocalTime(&timeinfo, 0))
            return -1; // no synced clock — caller treats as "not in schedule"
        ScheduleSegment seg;
        int minute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
        if (!findSegment(timeinfo.tm_wday, minute, seg))
            return -1; // outside all segments
        return (int)seg.radar;
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
        // A dashboard command is a small integer 1-17 (1=ON, 2=OFF, 3-17=temp).
        // The backend zero-pads it INCONSISTENTLY ("0011" but also "00013"), so we
        // must classify by VALUE, not string length — the same way CommandProcessor
        // reads it via doc["ir"].as<int>(). A genuine raw IR code (long, and usually
        // containing hex letters a-f) fails the isDigit scan above or the 1-17 range
        // check below, so it still drops correctly to the raw-hex path that follows.
        if (digitsOnly)
        {
            int cmdNum = irHex.toInt(); // "00013" -> 13, "0011" -> 11
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
                // Capture the setpoint currently applied BEFORE the segment overwrites it,
                // so both branches below can tell whether the temperature actually changed.
                int prevTemp = sysData.currentNormalTemp;

                applySegmentParams(seg);
                applySegmentRadar(seg.radar);
                sysData.currentNormalTemp = seg.temp;
                preferences.putInt("normal_temp", seg.temp);

                // Manual power hold: a pushed schedule update must not power the AC on while
                // the user has held it off (manualPowerAllowed == false). Settings are applied
                // above for a later resume; here we keep the AC OFF and ACK the held state.
                if (!sysData.manualPowerAllowed.load())
                {
                    sysData.acAutoState = AUTO_OFF;
                    NetworkManager::publishACK("schedule_update", "held_off");
                    ESP_LOGI(TAG, "Schedule update: manual power hold active — AC kept OFF.");
                }
                else
                {

                // Decide by OCCUPANCY, not by the prior AC state. The schedule defines
                // the baseline ("inside a segment => AC should be ON at seg.temp"), but
                // radar's energy saving overrides that baseline: if radar is managing and
                // the room is empty we must NOT blast the AC on into an empty room —
                // preserve whatever eco/off state radar already chose. Radar turns the AC
                // on at the new seg.temp the instant someone enters.
                //
                // This is what makes AUTO_OFF and AUTO_ON_ECO symmetric: previously eco
                // was preserved but a radar-driven OFF got turned back on. Now both
                // radar-managed empty-room states are left alone.
                //
                // Note: while a flap delay is active cachedPresence is forced false, but a
                // flap delay only ever follows an OFF (state is AUTO_OFF), so it lands in
                // the "already off, leave it off" case below — exactly what we want.
                bool radarManagingEmpty = sysData.radarAutoMode && !sysData.cachedPresence;

                if (radarManagingEmpty)
                {
                    // Preserve the radar-chosen state for the empty room; no IR sent.
                    AutoState st = sysData.acAutoState.load();
                    if (st == AUTO_ON_ECO)
                    {
                        // Eco already running; (re)arm the off countdown with the (possibly
                        // updated) TOffTime so it eventually powers down.
                        if (sysData.TOffTime > 0)
                            xTimerChangePeriod(offTimer, pdMS_TO_TICKS(sysData.TOffTime), 0);
                        ESP_LOGI(TAG, "Schedule update: radar managing empty room (eco). Settings applied, no IR.");
                    }
                    else if (st == AUTO_ON_NORMAL)
                    {
                        // AC is already running and cooling the (empty) room. A temperature
                        // change should retune the unit NOW: raising the setpoint on an
                        // already-on AC isn't "cooling an empty room" (the guard's concern) —
                        // it only saves energy — so unlike the OFF/ECO cases we DO send here.
                        // If the temp is unchanged we still send nothing, exactly as before.
                        if (prevTemp != seg.temp)
                        {
                            AutomationManager::executeACCommand(true, seg.temp, "schedule_retune");
                            ESP_LOGI(TAG, "Schedule update: AC on, room empty — retuned %d°C -> %d°C.",
                                     prevTemp, seg.temp);
                        }
                        // Room is empty: (re)arm the eco/off countdown so it still powers down.
                        if (sysData.TEcoTime > 0)
                            xTimerChangePeriod(ecoTimer, pdMS_TO_TICKS(sysData.TEcoTime), 0);
                        if (sysData.TOffTime > 0)
                            xTimerChangePeriod(offTimer, pdMS_TO_TICKS(sysData.TOffTime), 0);
                    }
                    else
                    {
                        // AUTO_OFF: radar already powered the AC off for the empty room.
                        // Leave it off (the fix) — do NOT turn it back on.
                        ESP_LOGI(TAG, "Schedule update: radar managing empty room (off). Settings applied, no IR.");
                    }
                }
                else
                {
                    // Room occupied, or radar disabled: assert the schedule's baseline ON
                    // state at the new setpoint. No countdown should run while turning on,
                    // and we don't need to re-arm any (occupied => no countdown; radar off
                    // => radar isn't driving eco/off at all).
                    xTimerStop(ecoTimer, 0);
                    xTimerStop(offTimer, 0);

                    // Only (re)send IR when the resulting state isn't already in effect:
                    // a temperature change, or the AC not already running at normal. Saving a
                    // schedule whose setpoint didn't change skips the redundant blast (no AC
                    // beep / flap twitch); the 3-min enforce timer still re-asserts regardless.
                    if (sysData.acAutoState != AUTO_ON_NORMAL || prevTemp != seg.temp)
                    {
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
                    }
                    else
                    {
                        // Already ON at this exact setpoint: skip the IR but still confirm the
                        // resulting state to the backend so observability is unchanged.
                        char detail[32];
                        snprintf(detail, sizeof(detail), "state=ON,temp=%d", seg.temp);
                        NetworkManager::publishACK("schedule_update", detail);
                        ESP_LOGI(TAG, "Schedule update: already ON at %d°C — no IR resent.", seg.temp);
                    }
                    sysData.acAutoState = AUTO_ON_NORMAL;
                }
                } // end: manual-power-hold else (room occupied / radar-managed branch)
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

            // This update authoritatively set today's AC state, which also means the
            // schedule task's boot run (gated on lastScheduledMin == -1) will now be
            // skipped — so open the radar gate here, or it would stay shut forever.
            sysData.scheduleBootDone = true;
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
     * automated locally (magenta LED). Once time is valid it polls every second and
     * evaluates the current minute, acting at most once per minute via the dedup
     * guard (so a clock correction is reacted to within ~1s, not up to a minute):
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
                // A missing clock does NOT mean we are offline. The broker can be
                // connected and telemetry flowing while NTP is merely slow/blocked
                // (common on captive or filtered networks). SYS_WIFI_OK / SYS_GSM_OK
                // mean MQTT is actually connected, so use that — not the clock — to
                // decide whether we are truly isolated.
                bool brokerConnected = (sysData.currentState == SYS_WIFI_OK ||
                                        sysData.currentState == SYS_GSM_OK);

                if (brokerConnected)
                {
                    // Online, just waiting for time: do NOT engage the failsafe (the
                    // backend is in control). Keep the 60s grace window fresh so a
                    // LATER real disconnect starts counting from zero, and clear any
                    // failsafe left over from a previous offline spell.
                    offlineBootStart = millis();
                    offlineFailsafeTriggered = false;
                    sysData.isOfflineFailsafeActive = false;
                }
                // --- ROBUST OFFLINE FAILSAFE ---
                // Only when genuinely offline (broker unreachable) for 60s: prioritize
                // local automation and force the radar to take over.
                else if (!offlineFailsafeTriggered && (millis() - offlineBootStart > 60000))
                {
                    ESP_LOGW(TAG, "Offline timeout! Prioritizing local automation. Forcing radar ON.");
                    sysData.radarAutoMode = true;
                    offlineFailsafeTriggered = true;
                    sysData.isOfflineFailsafeActive = true; // <--- ADD THIS: Turn on Magenta

                    // Kick the AC on for an already-present occupant — but ONLY when no
                    // schedule exists. "Schedule is king": if a schedule is configured it
                    // must make the first AC decision once the clock syncs, so we must not
                    // pre-empt it with a radar command at boot. (Radar still takes over on a
                    // genuine enter/re-enter transition via poll(), since the failsafe leaves
                    // radarAutoMode + isOfflineFailsafeActive on.) Without a schedule there is
                    // nothing to defer to, so cool the room immediately as before.
                    if (!sysData.hasAnySchedule && sysData.manualPowerAllowed.load())
                    {
                        SystemEvent event;
                        event.type = EVENT_PRESENCE_CHANGED;
                        event.payload = sysData.cachedPresence ? 1 : 0;
                        xQueueSend(automationQueue, &event, 0);
                    }
                }

                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }

            // Once time is valid, reset the tracker so we know we are online
            offlineFailsafeTriggered = false;

            sysData.isOfflineFailsafeActive = false; // <--- ADD THIS: Turn off Magenta

            // 2. Fixed 1-second poll. We deliberately do NOT sleep all the way to the
            // next :00 boundary: a clock correction (NTP/GSM resync, which this device
            // does routinely) landing mid-sleep would otherwise go unnoticed for up to
            // a full minute. Polling every second reacts to any clock step within ~1s
            // and lets the schedule make its first boot decision promptly. The cost is
            // negligible — see the dedup early-out below: ~98% of ticks do nothing but
            // read the clock and return, and the NVS-backed findSegment() reads still
            // run only on a genuine minute change (~once/min), exactly as before.
            vTaskDelay(pdMS_TO_TICKS(1000));

            // --- Evaluate the current minute (acted on at most once, see dedup) ---

            struct tm timeinfo;
            if (!getLocalTime(&timeinfo, 0))
                continue;

            int currentMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
            int currentWday = timeinfo.tm_wday;

            // CRITICAL (do not remove): with the 1s poll above this is what makes the
            // schedule act at most ONCE per minute. Without it, every tick inside the
            // same minute would re-fire the transition logic (and IR). Skip if we have
            // already processed this exact minute.
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

                    // No schedule for today => the AC must be OFF. Send the OFF
                    // command UNCONDITIONALLY (same as the in-schedule "gap" case
                    // below), so a unit left physically on is corrected at boot
                    // regardless of whether radar automation is enabled.
                    if (!IRManager::playCustomButton("ir_off"))
                        IRManager::sendACFallback(false, 24);
                    sysData.acAutoState = AUTO_OFF;
                    Indicator::indicateIRSent();

                    // FAILSAFE RECOVERY: if the offline failsafe had forced radar
                    // on, undo it now that we know today has no schedule.
                    if (sysData.radarAutoMode)
                    {
                        sysData.radarAutoMode = false;
                        preferences.putBool("radar_auto", false);
                        ESP_LOGI(TAG, "Recovery: No schedules for today. Radar disabled.");
                    }

                    ESP_LOGI(TAG, "Boot %02d:%02d — no schedule today, AC OFF",
                             timeinfo.tm_hour, timeinfo.tm_min);
                    sysData.scheduleBootDone = true; // schedule has spoken: radar may take over
                    continue;
                }
                ScheduleSegment seg;
                if (findSegment(currentWday, currentMin, seg))
                {
                    // Capture the setpoint radar may have already used (the NVS value)
                    // BEFORE the segment overwrites it, so we can tell whether a radar
                    // boot command already left the AC at the temperature we now want.
                    int prevNormalTemp = sysData.currentNormalTemp;

                    sysData.isInsideSchedule = true;
                    applySegmentParams(seg);
                    applySegmentRadar(seg.radar);
                    sysData.currentNormalTemp = seg.temp;
                    preferences.putInt("normal_temp", seg.temp);

                    // Manual power hold: keep the AC OFF through this segment. Settings are
                    // applied above so a later resume uses the right temp; skip all IR.
                    if (!sysData.manualPowerAllowed.load())
                    {
                        sysData.acAutoState = AUTO_OFF;
                        ESP_LOGI(TAG, "Boot %02d:%02d — manual power hold active, AC kept OFF",
                                 timeinfo.tm_hour, timeinfo.tm_min);
                        sysData.scheduleBootDone = true;
                        continue;
                    }

                    // Did radar already drive the AC during the offline boot wait? At boot the
                    // only other actor is radar, so lastCommandTime > 0 means radar already sent
                    // something. AUTO_ON_ECO is only ever set by radar; AUTO_OFF means radar drove
                    // the AC off; AUTO_ON_NORMAL means radar turned it on for an occupant — all
                    // before the schedule's first evaluation got a chance to run.
                    AutoState curState = sysData.acAutoState.load();
                    bool radarManaged = sysData.radarAutoMode && sysData.lastCommandTime > 0;

                    if (radarManaged && (curState == AUTO_ON_ECO || curState == AUTO_OFF))
                    {
                        // Radar drove the AC to eco/off. Apply schedule settings silently so the
                        // correct temp/eco/off values are ready when the room is occupied again.
                        // Re-arm offTimer only if currently in eco (eco already fired, off hasn't).
                        if (curState == AUTO_ON_ECO && sysData.TOffTime > 0)
                            xTimerChangePeriod(offTimer, pdMS_TO_TICKS(sysData.TOffTime), 0);

                        ESP_LOGI(TAG, "Boot %02d:%02d — radar managed (%s), schedule params applied, no IR",
                                 timeinfo.tm_hour, timeinfo.tm_min,
                                 curState == AUTO_ON_ECO ? "eco" : "off");
                    }
                    else if (radarManaged && curState == AUTO_ON_NORMAL)
                    {
                        // Radar already turned the AC ON for an occupant during the boot wait.
                        // Only re-blast if the scheduled temp differs from what radar used —
                        // otherwise the AC is already in the correct state, so skip the IR and
                        // avoid the duplicate boot command (radar_presence + boot_schedule_on).
                        if (prevNormalTemp != seg.temp)
                        {
                            AutomationManager::executeACCommand(true, seg.temp, "boot_schedule_retune");
                            ESP_LOGI(TAG, "Boot %02d:%02d — radar had AC ON, retuned %d°C -> %d°C",
                                     timeinfo.tm_hour, timeinfo.tm_min, prevNormalTemp, seg.temp);
                        }
                        else
                        {
                            ESP_LOGI(TAG, "Boot %02d:%02d — radar already turned AC ON at %d°C, no IR",
                                     timeinfo.tm_hour, timeinfo.tm_min, seg.temp);
                        }
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

                // Schedule has now made its first authoritative decision for this boot —
                // radar may take over from here ("schedule is king").
                sysData.scheduleBootDone = true;
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

                // Manual power hold: keep settings current but leave the AC OFF.
                if (!sysData.manualPowerAllowed.load())
                {
                    if (!hasPrevSeg || currentSeg.temp != prevSeg.temp)
                    {
                        sysData.currentNormalTemp = currentSeg.temp;
                        preferences.putInt("normal_temp", currentSeg.temp);
                    }
                    sysData.acAutoState = AUTO_OFF;
                    ESP_LOGI(TAG, "%02d:%02d -> manual power hold active, AC kept OFF",
                             timeinfo.tm_hour, timeinfo.tm_min);
                    continue;
                }

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