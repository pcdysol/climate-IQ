#include "OTAManager.h"
#include "config.h"
#include "NetworkManager.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <AsyncMqttClient.h>
#include <atomic>
#include "esp_ota_ops.h"

// AsyncMqttClient instance lives in WiFiManager.cpp (global). We disconnect it
// just before flashing to free heap and avoid TLS + async-TCP contention.
extern AsyncMqttClient mqttClient;

namespace OTAManager {

    // --- Pending-request mailbox (written by MQTT cb, read by TaskOTA) ---
    static char     reqUrl[256]     = {0};
    static char     reqVersion[24]  = {0};
    static bool     reqForce        = false;
    static std::atomic<bool> reqPending{false};
    static std::atomic<bool> otaInProgress{false};

    // --- Trial / post-boot state ---
    // onTrial: we are running a freshly-flashed image that has NOT yet proven itself.
    // It becomes permanent only once health is confirmed (MQTT reconnect).
    static std::atomic<bool> onTrial{false};
    static uint32_t trialStartMs       = 0;
    static bool     bootReportPending  = false;   // publish an ota result once MQTT is up
    static bool     bootRolledBack     = false;   // this boot is a recovery after a bad update
    static char     bootReportVersion[24] = {0};

    const char *currentVersion() { return FW_VERSION; }

    // Compare dotted versions: returns >0 if a>b, 0 if equal, <0 if a<b.
    // Non-numeric / missing parts are treated as 0, so "1.2" == "1.2.0".
    static int versionCompare(const char *a, const char *b) {
        while (*a || *b) {
            long na = strtol(a, (char **)&a, 10);
            long nb = strtol(b, (char **)&b, 10);
            if (na != nb) return (na < nb) ? -1 : 1;
            if (*a == '.') a++;
            if (*b == '.') b++;
            else if (!*b && !*a) break;
        }
        return 0;
    }

    static void clearTrialState() {
        Preferences p;
        p.begin("ota", false);
        p.putString("state", "");
        p.putInt("boots", 0);
        p.end();
    }

    // Switch the boot partition back to the OTHER app slot (the previous, known-good
    // firmware) and reboot. Works WITHOUT CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE because
    // it sets the boot partition explicitly in software rather than relying on the
    // bootloader's automatic PENDING_VERIFY revert.
    static void rollbackToPrevious() {
        const esp_partition_t *prev = esp_ota_get_next_update_partition(NULL);
        if (prev == NULL) {
            Serial.println("[OTA] Rollback aborted: no alternate partition found.");
            return;
        }
        esp_err_t err = esp_ota_set_boot_partition(prev);
        if (err != ESP_OK) {
            Serial.printf("[OTA] Rollback aborted: set_boot_partition err=%d (no valid previous image?).\n", err);
            return;
        }
        Serial.printf("[OTA] Rolling back to previous firmware on '%s'. Rebooting.\n", prev->label);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();   // does not return
    }

    void init() {
        // Runs as the FIRST thing in setup() (before any risky subsystem init) so the
        // trial boot counter advances even if the new firmware crashes during init.
        Preferences p;
        p.begin("ota", false);
        String state = p.getString("state", "");

        if (state == "rolledback") {
            // We are the OLD firmware, just booted after reverting a bad update.
            String fv = p.getString("ver", "unknown");
            strncpy(bootReportVersion, fv.c_str(), sizeof(bootReportVersion) - 1);
            bootReportVersion[sizeof(bootReportVersion) - 1] = '\0';
            bootRolledBack    = true;
            bootReportPending = true;
            p.putString("state", "");
            p.end();
            esp_ota_mark_app_valid_cancel_rollback();   // this (old) image is trusted
            Serial.printf("[OTA] Recovered to %s after failed update to %s.\n",
                          FW_VERSION, bootReportVersion);
            return;
        }

        if (state == "trial") {
            int boots = p.getInt("boots", 0) + 1;
            p.putInt("boots", boots);
            String tv = p.getString("ver", FW_VERSION);
            p.end();

            if (boots > OTA_MAX_TRIAL_BOOTS) {
                // The freshly-flashed image keeps rebooting without confirming health
                // (it crashes / boot-loops). Mark it bad and revert.
                Serial.printf("[OTA] Trial firmware failed to confirm after %d boots — rolling back.\n", boots);
                Preferences p2;
                p2.begin("ota", false);
                p2.putString("state", "rolledback");   // so the old image reports it
                p2.putString("ver", tv);                // the version that failed
                p2.putInt("boots", 0);
                p2.end();
                rollbackToPrevious();                   // reboots into the old image
                // If we reach here, rollback was not possible — stop trialling and run.
                Serial.println("[OTA] Rollback unavailable — staying on new firmware.");
                clearTrialState();
                return;
            }

            // Within the trial budget: run, but require a health confirmation.
            onTrial.store(true);
            trialStartMs = millis();
            strncpy(bootReportVersion, tv.c_str(), sizeof(bootReportVersion) - 1);
            bootReportVersion[sizeof(bootReportVersion) - 1] = '\0';
            bootReportPending = true;
            Serial.printf("[OTA] Booted firmware %s on TRIAL (attempt %d/%d) — awaiting health confirmation.\n",
                          FW_VERSION, boots, OTA_MAX_TRIAL_BOOTS);
            return;
        }
        p.end();

        // Normal boot of an already-trusted image: mark it valid (harmless no-op
        // unless the bootloader was built with rollback enabled).
        esp_ota_mark_app_valid_cancel_rollback();
    }

    bool requestUpdate(const char *url, const char *version, bool force) {
        if (!sysData.switch_gsm_wifi) {
            Serial.println("[OTA] Rejected: device is in GSM mode (OTA is WiFi-only).");
            return false;
        }
        if (!url || strlen(url) == 0 || strlen(url) >= sizeof(reqUrl)) {
            Serial.println("[OTA] Rejected: missing or oversized URL.");
            return false;
        }
        if (otaInProgress.load() || reqPending.load()) {
            Serial.println("[OTA] Rejected: an update is already queued/running.");
            return false;
        }
        strncpy(reqUrl, url, sizeof(reqUrl) - 1);
        reqUrl[sizeof(reqUrl) - 1] = '\0';
        reqVersion[0] = '\0';
        if (version) {
            strncpy(reqVersion, version, sizeof(reqVersion) - 1);
            reqVersion[sizeof(reqVersion) - 1] = '\0';
        }
        reqForce = force;
        reqPending.store(true);   // set LAST — publishes the request to TaskOTA
        Serial.printf("[OTA] Queued update -> %s (target ver '%s', force=%d)\n",
                      reqUrl, reqVersion, reqForce);
        return true;
    }

    static void performUpdate() {
        otaInProgress.store(true);

        // 1. Guard rails ------------------------------------------------
        if (!sysData.switch_gsm_wifi || sysData.isAPMode ||
            WiFi.status() != WL_CONNECTED) {
            Serial.println("[OTA] Abort: WiFi not connected / not in station mode.");
            NetworkManager::publishHealthAlert("ota", "abort_no_wifi");
            otaInProgress.store(false);
            return;
        }

        // 2. Version gate ----------------------------------------------
        if (reqVersion[0] != '\0' && !reqForce &&
            versionCompare(reqVersion, FW_VERSION) <= 0) {
            Serial.printf("[OTA] Skip: target %s not newer than running %s.\n",
                          reqVersion, FW_VERSION);
            char d[64];
            snprintf(d, sizeof(d), "skip_not_newer_%s", reqVersion);
            NetworkManager::publishHealthAlert("ota", d);
            otaInProgress.store(false);
            return;
        }

        // 3. Announce + flush, then free the async-MQTT/TLS heap --------
        char startMsg[64];
        snprintf(startMsg, sizeof(startMsg), "start_%s",
                 reqVersion[0] ? reqVersion : "unknown");
        NetworkManager::publishHealthAlert("ota", startMsg);
        vTaskDelay(pdMS_TO_TICKS(800));    // let the async publish leave
        mqttClient.disconnect();           // frees AsyncTCP buffers for TLS
        vTaskDelay(pdMS_TO_TICKS(300));

        // 4. Pick transport based on URL scheme ------------------------
        bool isHttps = (strncmp(reqUrl, "https://", 8) == 0);
        WiFiClientSecure secureClient;
        WiFiClient plainClient;
        WiFiClient *client;
        if (isHttps) {
            if (sizeof(OTA_ROOT_CA) > 1)   // CA pinned in config.h
                secureClient.setCACert(OTA_ROOT_CA);
            else
                secureClient.setInsecure(); // transport-only TLS, no pinning
            secureClient.setTimeout(OTA_HTTP_TIMEOUT_MS / 1000);
            client = &secureClient;
        } else {
            client = &plainClient;
        }

        // 5. Run it -----------------------------------------------------
        httpUpdate.rebootOnUpdate(false);  // we reboot ourselves, cleanly
        httpUpdate.setLedPin(-1);
        httpUpdate.onProgress([](int cur, int total) {
            if (total > 0)
                Serial.printf("[OTA] %d%%\r", (cur * 100) / total);
        });

        Serial.printf("[OTA] Downloading from %s ...\n", reqUrl);
        t_httpUpdate_return ret = httpUpdate.update(*client, reqUrl);

        // 6. Handle outcome --------------------------------------------
        if (ret == HTTP_UPDATE_OK) {
            // The image was streamed into the inactive partition AND its appended
            // SHA256 was verified by the Update library. Persist a "trial" marker so
            // the NEXT boot runs it provisionally until health is confirmed.
            Preferences p;
            p.begin("ota", false);
            p.putString("state", "trial");
            p.putString("ver", reqVersion[0] ? reqVersion : FW_VERSION);
            p.putInt("boots", 0);
            p.end();
            Serial.println("\n[OTA] SUCCESS — rebooting into new firmware (on trial).");
            vTaskDelay(pdMS_TO_TICKS(500));
            ESP.restart();                 // does not return
        }

        // Failure: current firmware is untouched. WiFiManager::loop() will
        // reconnect MQTT on its own backoff; wait briefly so we can report.
        int err = httpUpdate.getLastError();
        Serial.printf("\n[OTA] FAILED (ret=%d, err=%d) %s\n",
                      ret, err, httpUpdate.getLastErrorString().c_str());

        for (int i = 0; i < 40 && !mqttClient.connected(); i++)
            vTaskDelay(pdMS_TO_TICKS(500)); // up to ~20s for MQTT to return
        char d[80];
        snprintf(d, sizeof(d), "failed_ret%d_err%d", ret, err);
        NetworkManager::publishHealthAlert("ota", d);

        otaInProgress.store(false);
    }

    // Called by WiFiManager every time MQTT (re)connects. The first call after a
    // trial boot is the HEALTH GATE: WiFi + broker + app are all proven working, so
    // we commit the new firmware permanently. Also publishes the one-shot ota result.
    void reportBootResultIfPending() {
        if (onTrial.exchange(false)) {
            esp_ota_mark_app_valid_cancel_rollback();   // forward-compat with IDF rollback
            clearTrialState();
            Serial.printf("[OTA] Health confirmed (MQTT up) — committed firmware %s.\n", FW_VERSION);
        }

        if (!bootReportPending) return;
        bootReportPending = false;
        char d[80];
        if (bootRolledBack)
            snprintf(d, sizeof(d), "rolled_back_to_%s_from_%s", FW_VERSION, bootReportVersion);
        else
            snprintf(d, sizeof(d), "success_now_%s", FW_VERSION);
        NetworkManager::publishHealthAlert("ota", d);
        Serial.printf("[OTA] Reported '%s' to backend.\n", d);
    }

    void TaskOTA(void *pvParameters) {
        for (;;) {
            if (reqPending.exchange(false)) {
                performUpdate();           // blocks THIS task only
            }

            // Optional time-based safety net (disabled when OTA_TRIAL_CONFIRM_MS == 0).
            // If a trial image stays up but never confirms health within the window,
            // revert even though it hasn't crashed. Off by default so a transient
            // broker outage can't revert an otherwise-healthy device.
            if (OTA_TRIAL_CONFIRM_MS > 0 && onTrial.load() &&
                (millis() - trialStartMs > (uint32_t)OTA_TRIAL_CONFIRM_MS)) {
                Serial.println("[OTA] Trial confirm window elapsed without health — rolling back.");
                onTrial.store(false);
                Preferences p;
                p.begin("ota", false);
                p.putString("state", "rolledback");
                p.putInt("boots", 0);
                p.end();
                rollbackToPrevious();
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
