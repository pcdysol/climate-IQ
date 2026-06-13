#pragma once
#include "SharedState.h"
#include <ArduinoJson.h>

/**
 * @file ScheduleManager.h
 * @brief 7-day time-segment scheduler persisted in NVS.
 *
 * Each weekday holds up to MAX_SEGS_PER_DAY ScheduleSegments (9 bytes each,
 * stored under "sch_seg_<wday>" with a count in "sch_cnt_<wday>"). A dedicated
 * task wakes exactly at each minute boundary, finds the active segment, and
 * applies its temp/radar/eco/off settings (sending IR as needed). Also handles
 * inbound "segments" schedule-update commands and an offline failsafe that
 * forces radar control when no network time is available.
 */
namespace ScheduleManager {
    /// FreeRTOS task: minute-aligned schedule evaluation + offline failsafe.
    void TaskSchedule(void *pvParameters);

    /// Recompute sysData.hasAnySchedule by scanning all 7 days' segment counts.
    void refreshHasAnySchedule();

    /// Validate, sort, persist and immediately apply an inbound schedule-update command.
    void handleScheduleCommand(JsonDocument &doc);

    /// @return Number of stored segments for @p wday (0-6).
    uint8_t loadSegmentCount(int wday);
    /// Load segment @p idx for @p wday into @p out. @return true if it exists.
    bool loadSegment(int wday, int idx, ScheduleSegment &out);

    /// @return the active segment's radar field (1 enable, 2 disable) for the current
    ///         local time, or -1 if outside all segments / no synced clock.
    int currentSegmentRadar();
}
