#pragma once
#include "SharedState.h"
#include "board_select.h"
#include "MyLD2410.h"

namespace SensorManager {
    // Boot up sensors and map them to the central state
    void init(SystemData* state);
    
    // Process new data from Radar and HDC
    void poll();
    // Add this under poll()
    void checkHealth();

    // Recovery functions
    void attemptHDCRecovery();
    void attemptRadarRecovery();

    // Web-triggered radar maintenance. These only signal the sensor task (the only
    // owner of the radar UART) and return immediately. Progress/result is reported
    // through SystemData::radarCalStatus (poll getCalStatus()).
    bool requestCalibration();        // auto-threshold calibration (room must be empty)
    bool requestRadarFactoryReset();  // reset radar to factory defaults
    int  getCalStatus();              // 0 idle, 1 in progress, 2 success, 3 failed

    void TaskSensors(void *pvParameters);
}