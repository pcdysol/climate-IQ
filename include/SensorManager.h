#pragma once
#include "SharedState.h"
#include "board_select.h"
#include "MyLD2410.h"

/**
 * @file SensorManager.h
 * @brief LD2412 radar + HDC1080 climate sensor management.
 *
 * Owns both sensors and, critically, the radar UART — so ALL radar config
 * (calibration, factory reset, range changes) must happen on the sensor task.
 * Other tasks request those operations through the request*() functions below,
 * which validate and signal the sensor task via xTaskNotify(); the sensor task
 * is the only code that touches the radar link. Progress/results come back
 * through SystemData (radarCalStatus, radarRangeCm).
 */
namespace SensorManager {
    /// Initialise HDC1080 + radar and bind the shared state. Called once in setup().
    void init(SystemData* state);

    /// Read new radar/HDC frames and update shared state (called from the sensor task).
    void poll();

    /// Health check: flag the radar for recovery if its data has gone stale.
    void checkHealth();

    /// Re-init the HDC1080 over I2C after repeated failed reads.
    void attemptHDCRecovery();
    /// Re-establish the radar UART link and re-enable streaming.
    void attemptRadarRecovery();

    // --- Web-triggered radar maintenance ---
    // These only signal the sensor task (the sole owner of the radar UART) and
    // return immediately. Progress/result is reported via SystemData::radarCalStatus
    // (poll getCalStatus()).

    /// Request auto-threshold calibration (room must be empty). @return true if queued.
    bool requestCalibration();
    /// Request a radar factory-defaults reset. @return true if queued.
    bool requestRadarFactoryReset();
    /// @return Maintenance status: 0 idle, 1 in progress, 2 success, 3 failed.
    int  getCalStatus();

    // --- Detection-range boundary ---
    // The radar stores the range in its own flash; the ESP32 does not persist it.

    /// Request a new detection boundary in cm (validated, snapped on the sensor task). @return true if queued.
    bool requestSetRange(int cm);
    /// @return Last detection-range boundary (cm) read back from the radar.
    int  getRangeCm();

    // --- Web-triggered IR learning ---
    // The IR receiver is owned by the sensor task (it runs the always-on remote
    // listener), so learning a code/protocol — which also touches the receiver —
    // MUST run there too, never on the web task. requestLearn() signals the sensor
    // task; the web handler then polls getLearnStatus() and consumeLearnResult().

    /// Queue an IR learn (key + protocol/raw) onto the sensor task. @return true if accepted.
    bool requestLearn(const char* storageKey, bool isProtocol);
    /// @return Learn progress: 0 idle, 1 in progress, 2 done (result ready).
    int  getLearnStatus();
    /// Read the finished learn result (0 ok, 1 timeout, 2 unknown protocol) and reset to idle.
    int  consumeLearnResult();

    /// FreeRTOS task: poll loop + notification-driven radar maintenance + IR-RX listener.
    void TaskSensors(void *pvParameters);
}
