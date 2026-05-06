#pragma once
#include "SharedState.h"
#include <Arduino.h>

namespace HealthManager {
    void loop();
    String getResetReason();
}