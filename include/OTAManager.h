#pragma once
#include <Arduino.h>
#include "SharedState.h"

// ====================================================================
//  OTAManager — server-based (pull) Over-The-Air firmware updates.
//
//  Flow:
//    1. Backend publishes an MQTT command:
//         { "command": "ota_update",
//           "url": "http://107.172.137.233/firmware/firmware.bin",
//           "version": "1.1.0",
//           "force": false }            // optional, default false
//    2. CommandProcessor calls OTAManager::requestUpdate(...). That call is
//       NON-BLOCKING — it only records the request and wakes TaskOTA.
//    3. TaskOTA validates (WiFi mode, connected, version newer / forced),
//       streams the binary into the inactive OTA partition over HTTP(S),
//       verifies it, and reboots into the new firmware.
//    4. On the next boot the new firmware confirms success back over MQTT.
//
//  A failed/aborted update is harmless: the device keeps running the
//  current firmware and MQTT reconnects on its own.
// ====================================================================
namespace OTAManager {

    // Call once in setup() AFTER preferences/NVS is available, BEFORE
    // network init. Marks the running image valid and remembers whether
    // we just came back from an update (for post-boot confirmation).
    void init();

    // Non-blocking. Safe to call from the MQTT receive callback.
    // Returns false if the request is rejected outright (e.g. GSM mode).
    bool requestUpdate(const char *url, const char *version, bool force);

    // The worker task. Spawn it from main.cpp.
    void TaskOTA(void *pvParameters);

    // Called by WiFiManager once MQTT is connected, so a completed update
    // can be confirmed to the backend exactly once.
    void reportBootResultIfPending();

    // Current running firmware version (FW_VERSION).
    const char *currentVersion();
}
