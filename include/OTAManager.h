#pragma once
#include <Arduino.h>
#include "SharedState.h"

/**
 * @file OTAManager.h
 * @brief Server-based (pull) OTA firmware updates with trial-boot rollback.
 */

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
//
//  --- Backend status phases (published as {event:"ota", detail:"<phase>,k=v"}) ---
//  The device cannot stream a live % in production: MQTT is intentionally
//  disconnected during the download+flash to free heap. So progress is reported
//  as discrete PHASES, with a normal silent gap during the flash/reboot:
//
//    downloading,version=1.0.2          last msg before going offline
//        |   (offline: download + flash + reboot + reconnect — expected silence)
//        v
//    success,version=1.0.2              new firmware booted & confirmed healthy
//    failed,ret=-1,err=-104            flash failed; still on old firmware
//    rolled_back,version=1.0.0,failed_version=1.0.2   bad update auto-reverted
//
//  Pre-flight phases (no offline gap, sent immediately):
//    skipped,reason=not_newer,version=1.0.1
//    aborted,reason=no_wifi
//
//  Backend rule of thumb: after "downloading", expect a terminal phase within a
//  few minutes. If none arrives (and the device's normal telemetry also stops),
//  treat it as a stuck/failed update via a timeout — the gap itself is normal.
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
