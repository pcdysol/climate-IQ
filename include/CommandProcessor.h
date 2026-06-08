#pragma once
#include <ArduinoJson.h>
#include "SharedState.h"

/**
 * @file CommandProcessor.h
 * @brief Parser/dispatcher for inbound MQTT JSON commands from the backend.
 *
 * Both transports (WiFiManager and GSMManager) deserialize a received MQTT
 * payload into a JsonDocument and hand it to processJSON(). This is the single
 * place the cloud->device command vocabulary is interpreted.
 *
 * ---------------------------------------------------------------------------
 *  INBOUND COMMAND VOCABULARY (the JSON the backend may publish)
 * ---------------------------------------------------------------------------
 *  Temperature / schedule (key "command":"temperature_control"):
 *    - With "segments": [...]        -> a schedule update (routed to ScheduleManager).
 *    - With "temperature_setting":N  -> set + send the normal setpoint (one-shot
 *                                       only while inside a schedule).
 *    - With "ir":N                   -> N in 3..17 maps to temp (N+13); overrides above.
 *
 *  Power (key "command":"power_control"):
 *    - "power_status": true|false    -> AC on (at normal temp) / off.
 *
 *  Radar (key "command":"radar_control"):
 *    - "radar": "0100"|"256"         -> enable radar automation.
 *    - "radar": "0200"|"512"         -> disable radar automation.
 *      (Sets a manual override that survives until the next schedule update.)
 *
 *  Eco parameters (any of these top-level keys, no "command" needed):
 *    - "eco":N   -> eco setpoint °C.
 *    - "teco":N  -> eco delay in minutes (stored as ms).
 *    - "toff":N  -> off delay in minutes (clamped to > teco).
 *
 *  OTA (key "command":"ota_update"):  see OTAManager.h for the full flow.
 *    - "url", "version", optional "force".
 *
 *  Dynamic IR (top-level "protocol"):
 *    - With "state":[...] (+ optional "size") -> send a raw state array.
 *    - With "code":"hex"  (+ optional "bits") -> send a numeric code.
 *
 * ---------------------------------------------------------------------------
 *  Related outbound telemetry "auto_event" codes (emitted elsewhere):
 *    "1000" normal-on   "2000" eco   "3000" off   "4000" manual remote override.
 * ---------------------------------------------------------------------------
 */
namespace CommandProcessor {
    /**
     * @brief Parse one inbound MQTT JSON document and trigger the matching action.
     * @param doc Deserialized command payload (see the vocabulary above).
     * @return true if @p doc matched a known command and was acted on; false otherwise.
     */
    bool processJSON(JsonDocument &doc);
}
