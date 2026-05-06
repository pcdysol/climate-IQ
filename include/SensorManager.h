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

    // Calibration commands (returns true on success)
    bool calibrateRadarAuto();
    bool calibrateRadarReset();
    // Add these to fetch the live radar gate arrays
    const MyLD2410::ValuesArray& getMovingSignals();
    const MyLD2410::ValuesArray& getStationarySignals();
    void TaskSensors(void *pvParameters);
}