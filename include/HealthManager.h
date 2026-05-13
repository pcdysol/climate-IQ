#pragma once
#include "SharedState.h"
#include <Arduino.h>

namespace HealthManager {
    // Change this signature:
    void HealthTimerCallback(TimerHandle_t xTimer);
    String getResetReason();
}