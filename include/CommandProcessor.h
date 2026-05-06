#pragma once
#include <ArduinoJson.h>
#include "SharedState.h"

namespace CommandProcessor {
    // Parses incoming MQTT JSON and triggers the correct system actions
    bool processJSON(JsonDocument &doc);
}