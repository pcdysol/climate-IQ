/**
 * @file WiFiManager.cpp
 * @brief Implementation of the WiFi station + AsyncMqttClient transport.
 *
 * Drives the WiFi connection (with a non-fatal boot timeout so the device runs
 * offline), NTP time sync, and the asynchronous MQTT lifecycle: LWT/online
 * messages, topic subscription, the one-shot boot alert, and exponential
 * backoff reconnection. loop() also publishes the periodic telemetry payload.
 * The global `mqttClient` defined here is shared with OTAManager. See
 * WiFiManager.h for the public API.
 */
#include "WiFiManager.h"
#include "Config.h"
#include "CommandProcessor.h"
#include "HealthManager.h"
#include <WiFi.h>
#include <AsyncMqttClient.h>
#include <Preferences.h>
#include "SharedState.h"
#include "esp_task_wdt.h"
#include "Indicator.h"
#include "GSMManager.h"
#include "OTAManager.h"
#include "esp_sntp.h"
#include <atomic>
#include "esp_log.h"

static const char *TAG = "WIFI";

extern Preferences preferences;
extern SystemData sysData;
AsyncMqttClient mqttClient;            ///< Shared async MQTT client (also used by OTAManager).
unsigned long lastWifiRetry = 0;       ///< millis() of the last WiFi reconnect attempt.
unsigned long wifiBackoffMs = 5000;    ///< Current WiFi reconnect backoff (doubles to a cap).
unsigned long lastMqttRetry = 0;       ///< millis() of the last MQTT reconnect attempt.
unsigned long mqttBackoffMs = 5000;    ///< Current MQTT reconnect backoff (doubles to a cap).
unsigned long lastTelemetry = 0;       ///< millis() of the last telemetry publish.

namespace WiFiManager
{
    String currentSSID;
    String currentPassword;
    String macAddress;
    String mqttTopic;
    String device_id;
    // --- ADD THESE TWO LINES ---
    String globalLwtTopic;
    String globalLwtMessage;
    // --- ADD THESE TWO FOR THE ONLINE MESSAGE ---
    String globalOnlineTopic;
    String globalOnlineMessage;

    /// @return ISO-8601 local timestamp, or "Syncing..." until NTP time is valid.
    String getTimestamp()
    {
        struct tm timeinfo;
        // getLocalTime returns true if time is synced, false if not
        if (!getLocalTime(&timeinfo, 0))
        {
            return "Syncing...";
        }

        // Extra safety check: Is the year still 1970?
        if (timeinfo.tm_year < 120) // 120 = year 2020 (years since 1900)
        {
            return "Syncing...";
        }

        char buffer[25];
        strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
        return String(buffer);
    }

    /**
     * @brief MQTT message callback: null-terminate, parse JSON, dispatch.
     * @note AsyncMqttClient payloads are NOT null-terminated, so the bytes are
     *       copied into a local buffer before deserialization.
     */
    void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)
    {
        // AsyncMqttClient payload is NOT null-terminated. We must copy it.
        char msgBuffer[len + 1];
        memcpy(msgBuffer, payload, len);
        msgBuffer[len] = '\0';
        // Debug-level: the full inbound JSON is verbose (every telemetry echo +
        // command). Compiled out at CORE_DEBUG_LEVEL=3; raise to 4 to see it.
        ESP_LOGD(TAG, "Message received: %s", msgBuffer);

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, msgBuffer);
        if (error)
        {
            ESP_LOGW(TAG, "JSON parse failed");
            return;
        }

        CommandProcessor::processJSON(doc);
    }

    /**
     * @brief Connect to the stored SSID, blocking up to a boot timeout.
     * @note On timeout it returns and the device continues offline; loop() then
     *       retries in the background. Keeps the LED/sensors responsive while waiting.
     */
    void setup_wifi()
    {
        sysData.currentState = SYS_WIFI_CONN;
        ESP_LOGI(TAG, "Connecting to WiFi...");
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.persistent(true);
        WiFi.begin(currentSSID.c_str(), currentPassword.c_str());

        const unsigned long WIFI_BOOT_TIMEOUT = 10000;
        unsigned long start = millis();

        while (WiFi.status() != WL_CONNECTED)
        {
            esp_task_wdt_reset();
            // VITAL FIX: Keep the logic running so the device can act on
            // the background sensor data while waiting for the router!
            Indicator::update(sysData.currentState, sysData.radarAutoMode, sysData.cachedPresence);
            if (sysData.isAPMode)
                return;
            if (millis() - start > WIFI_BOOT_TIMEOUT)
            {
                ESP_LOGW(TAG, "Boot timeout — continuing offline. Will retry in background.");
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        ESP_LOGI(TAG, "WiFi connected");
        // WiFi is up but MQTT hasn't connected yet — show amber (not green) until
        // the broker session is actually established. loop() promotes this to
        // SYS_WIFI_OK only once mqttClient.connected() is true.
        sysData.currentState = SYS_MQTT_DOWN;
        ESP_LOGI(TAG, "IP: %s", WiFi.localIP().toString().c_str());
    }

    /**
     * @brief MQTT connected callback: subscribe, publish online + boot alert, confirm OTA.
     *
     * Resets the MQTT backoff, subscribes to the command topic, publishes the
     * retained "Online" status, sends a one-time boot report, and lets
     * OTAManager confirm/commit a pending firmware update now that the link is up.
     */
    void onMqttConnect(bool sessionPresent)
    {
        ESP_LOGI(TAG, "Connected to MQTT broker");

        // Reset the backoff timer because we successfully connected!
        mqttBackoffMs = 5000;

        // Subscribe to your topic
        mqttClient.subscribe(mqttTopic.c_str(), 1);

        // --- USE THE GLOBALS HERE ---
        mqttClient.publish(globalOnlineTopic.c_str(), 1, true, globalOnlineMessage.c_str());

        // Send Boot Alert once
        static bool bootAlertSent = false;
        if (!bootAlertSent)
        {
            JsonDocument bootDoc;
            JsonObject bd = bootDoc[device_id].to<JsonObject>();
            bd["event"] = "boot";
            bd["reset_reason"] = HealthManager::getResetReason();
            bd["hdc_init"] = sysData.hdcInitFailed ? "FAILED" : "OK";
            bd["radar_init"] = sysData.radarInitFailed ? "FAILED" : "OK";
            bd["fw_version"] = OTAManager::currentVersion();
            char bootBuf[256];
            size_t len = serializeJson(bootDoc, bootBuf);

            // Note the new publish signature! (topic, qos, retain, payload)
            mqttClient.publish(mqttTopic.c_str(), 0, false, bootBuf, len);
            bootAlertSent = true;
            ESP_LOGI(TAG, "Boot alert sent via MQTT.");
        }

        // If we just rebooted from a successful OTA, confirm it to the
        // backend exactly once now that the link is back up.
        OTAManager::reportBootResultIfPending();
    }

    /// MQTT disconnected callback (logs only; loop() drives the reconnect backoff).
    void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
    {
        ESP_LOGW(TAG, "Disconnected from MQTT broker");
    }

    /// Kick off one asynchronous MQTT connect, if WiFi is up and not already connected.
    void reconnect()
    {
        if (sysData.isAPMode)
            return;
        if (WiFi.status() != WL_CONNECTED)
            return;
        if (mqttClient.connected())
            return;

        ESP_LOGI(TAG, "Connecting to MQTT...");
        mqttClient.connect(); // This is asynchronous. It takes no arguments!
    }

    /// @return The WiFi-STA MAC as a 12-char uppercase hex string (used as device id).
    String getChipMAC()
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char buf[13];
        sprintf(buf, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return String(buf);
    }

    /**
     * @brief Initialise the transport: connect WiFi, configure NTP, set up MQTT.
     *
     * Derives the device id/topics from the MAC, connects WiFi, requests NTP
     * sync (with a 30-min resync interval), and configures the MQTT client
     * (server, client id, LWT, online message, callbacks). Called once at boot.
     */
    void init()
    {
        currentSSID = preferences.getString("wifi_ssid", "");
        currentPassword = preferences.getString("wifi_pass", "");

        macAddress = WiFi.macAddress();
        macAddress.replace(":", "");
        device_id = macAddress;
        mqttTopic = "/topic/" + macAddress;
        ESP_LOGI(TAG, "Device ID: %s", device_id.c_str());

        setup_wifi();

        if (WiFi.status() == WL_CONNECTED)
        {
            // 1. Register the asynchronous callback BEFORE configuring time
            // sntp_set_time_sync_notification_cb(timeSyncCallback);
            // --- ADD THIS LINE: Tells the native SNTP client to resync every 30 mins ---
            sntp_set_sync_interval(30000);

            // 2. Fire and forget - LwIP handles internal retries
            configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER, NTP_SERVER2, NTP_SERVER3);
            ESP_LOGI(TAG, "Time sync requested via LwIP...");
        }
        else
        {
            ESP_LOGW(TAG, "WiFi offline.");
        }

        mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
        mqttClient.setClientId(device_id.c_str());
        // Detect a dead / half-open broker socket fast: without this the client
        // can believe it is still connected (publish() returns success) while the
        // bytes never reach the broker. A short keepalive forces a PINGREQ and
        // tears the session down within ~15s so reconnect/backoff can recover.
        mqttClient.setKeepAlive(15);
        // 1. Setup the real Last Will (Broker sends this automatically if device dies)
        globalLwtTopic = "/status/" + device_id;
        globalLwtMessage = "{\"" + device_id + "\": {\"status\": \"Offline\", \"reason\": \"Connection Lost\"}}";
        mqttClient.setWill(globalLwtTopic.c_str(), 1, true, globalLwtMessage.c_str());

        // --- ADD THIS: Prepare the online message permanently ---
        globalOnlineTopic = "/status/" + device_id;
        globalOnlineMessage = "{\"" + device_id + "\": {\"status\": \"Online\"}}";

        // Register the Async Callbacks
        mqttClient.onConnect(onMqttConnect);
        mqttClient.onDisconnect(onMqttDisconnect);
        mqttClient.onMessage(onMqttMessage);
    }

    /// @return Battery percentage placeholder (fixed; no battery gauge fitted).
    int batteryPercentage() { return 80; }

    /**
     * @brief Service tick: WiFi/MQTT backoff reconnection + periodic telemetry.
     *
     * Run from TaskNetwork. Reconnects WiFi then MQTT on exponential backoff
     * (reset on success), re-asserts NTP after a WiFi reconnect, and once every
     * TELEMETRY_INTERVAL publishes the full telemetry payload (sensor readings,
     * presence/empty seconds, heap, RSSI, fw version, etc.).
     */
    void loop()
    {
        static bool bootReasonReported = false;

        if (WiFi.status() != WL_CONNECTED)
        {
            if (sysData.currentState != SYS_WIFI_CONN)
            {
                sysData.currentState = SYS_WIFI_CONN;
                ESP_LOGW(TAG, "Connection lost — running offline. Background retry active.");
            }
            if (millis() - lastWifiRetry >= wifiBackoffMs)
            {
                lastWifiRetry = millis();
                ESP_LOGI(TAG, "Retry attempt (next in ~%lus if this fails)", wifiBackoffMs / 1000);
                WiFi.disconnect();
                WiFi.begin(currentSSID.c_str(), currentPassword.c_str());
                wifiBackoffMs = min(wifiBackoffMs * 2, MAX_BACKOFF_MS);
            }
            return;
        }

        if (sysData.currentState == SYS_WIFI_CONN)
        {
            ESP_LOGI(TAG, "WiFi reconnected!");
            wifiBackoffMs = 5000;
            // WiFi just came back. The MQTT backoff may have grown to the cap while
            // we were offline; if we don't reset it, MQTT sits idle waiting out that
            // stale timer (we saw ~33s of dead air after a WiFi recovery). Reset the
            // backoff and clear lastMqttRetry so MQTT reconnects immediately now.
            mqttBackoffMs = 5000;
            lastMqttRetry = 0;
            // Re-assert SNTP servers upon reconnect to ensure LwIP resumes
            configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER, NTP_SERVER2, NTP_SERVER3);
        }

        if (!mqttClient.connected())
        {
            // WiFi is associated but the MQTT broker session is down, so we are
            // NOT delivering data. Show this as its own amber state instead of the
            // green "OK" — green must only ever mean "actually sending".
            sysData.currentState = SYS_MQTT_DOWN;
            if (millis() - lastMqttRetry >= mqttBackoffMs)
            {
                lastMqttRetry = millis();
                reconnect();
                // Just double it. If it successfully connects, onMqttConnect will reset it.
                mqttBackoffMs = min(mqttBackoffMs * 2, MAX_BACKOFF_MS);
            }
            return;
        }

        // Past both guards: WiFi up AND MQTT connected — the only true "online and
        // delivering" state, and the only one that shows the green LED.
        sysData.currentState = SYS_WIFI_OK;

        if (millis() - lastTelemetry > sysData.telemetryIntervalMs.load())
        {
            unsigned long now = millis();
            lastTelemetry = now;

            int total_interval_secs = round(sysData.telemetryIntervalMs.load() / 1000.0);

            // Sensor task (trackPresenceTime) accumulates every 20ms — just harvest here.
            uint32_t currentAccumulatedMs = sysData.accumulatedPresenceMs.exchange(0, std::memory_order_relaxed);
            int presence_secs = round(currentAccumulatedMs / 1000.0);

            if (presence_secs > total_interval_secs)
                presence_secs = total_interval_secs;
            int empty_secs = total_interval_secs - presence_secs;

            float temperature = sysData.currentTemp;
            float humidity = sysData.currentHumidity;
            int rssi = WiFi.RSSI();

            JsonDocument doc;
            JsonObject mac = doc[device_id].to<JsonObject>();
            mac["timestamp"] = getTimestamp();
            mac["radar_auto_mode"] = sysData.radarAutoMode ? "Enabled" : "Disabled";
            mac["manual_off_hold"] = !sysData.manualPowerAllowed.load();
            if (sysData.radarAutoMode)
            {
                mac["presence_seconds"] = presence_secs;
                mac["empty_seconds"] = empty_secs;
            }
            mac["currentEcoTemp"] = sysData.currentEcoTemp;
            mac["TEcoTime"] = sysData.TEcoTime / 60000;
            mac["TOffTime"] = sysData.TOffTime / 60000;
            mac["temperature"] = temperature;
            mac["humidity"] = humidity;
            mac["radar_distance"] = sysData.radarDistance;
            mac["status"] = "Online";

            if (!bootReasonReported)
            {
                mac["last_reset_reason"] = HealthManager::getResetReason();
                bootReasonReported = true;
            }
            mac["device_type"] = "climate_sensor";
            mac["fw_version"] = OTAManager::currentVersion();
            mac["battery_level"] = batteryPercentage();
            mac["wifi_signal_strength"] = rssi;
            mac["uptime_s"] = millis() / 1000;
            mac["free_heap"] = ESP.getFreeHeap();
            mac["max_alloc_heap"] = ESP.getMaxAllocHeap(); // The largest single vehicle that can park

            static char buffer[768];
            size_t written = serializeJson(doc, buffer, sizeof(buffer));
            if (written >= sizeof(buffer))
            {
                ESP_LOGE(TAG, "JSON payload truncated!");
            }
            else if (mqttClient.publish(mqttTopic.c_str(), 0, false, buffer, written))
            {
                // Telemetry fires every 10s — keep the full payload at DEBUG so
                // normal operation stays quiet but it's there when you need it.
                ESP_LOGD(TAG, "Telemetry sent: %s", buffer);
            }
            else
            {
                ESP_LOGW(TAG, "Failed to send telemetry! Forcing reconnect...");
                mqttClient.disconnect();
            }
        }
    }

    /// Publish a command acknowledgement {ack:"ok", action, detail} to the device topic.
    void publishACK(const char *action, const char *detail)
    {
        JsonDocument doc;
        String id = device_id;
        JsonObject obj = doc[id].to<JsonObject>();
        obj["ack"] = "ok";
        obj["action"] = action;
        obj["detail"] = detail;

        // --- ADD 'static' HERE ---
        char buf[256];
        size_t len = serializeJson(doc, buf);

        if (mqttClient.connected())
        {
            mqttClient.publish(mqttTopic.c_str(), 0, false, buf, len);
        }
        ESP_LOGI(TAG, "ACK action=%s detail=%s", action, detail);
    }

    /// Publish a health/diagnostic alert {event, detail} to the device topic.
    void publishHealthAlert(const char *event, const char *detail)
    {
        JsonDocument doc;
        String id = device_id;
        JsonObject obj = doc[id].to<JsonObject>();
        obj["event"] = event;
        obj["detail"] = detail;

        // --- ADD 'static' HERE ---
        char buf[192];
        size_t len = serializeJson(doc, buf);

        if (mqttClient.connected())
        {
            mqttClient.publish(mqttTopic.c_str(), 0, false, buf, len);
        }
        ESP_LOGW(TAG, "HEALTH ALERT %s — %s", event, detail);
    }

    /// Publish an immediate automation event {timestamp, auto_event:eventCode}.
    void sendAutomationEvent(String eventCode)
    {
        ESP_LOGI(TAG, "Sending immediate automation event: %s", eventCode.c_str());
        JsonDocument doc;
        String currentId = device_id;
        JsonObject data = doc[currentId].to<JsonObject>();

        data["timestamp"] = getTimestamp();
        data["auto_event"] = eventCode;

        // --- ADD 'static' HERE ---
        char buffer[256];
        size_t len = serializeJson(doc, buffer);

        if (mqttClient.connected())
        {
            if (mqttClient.publish(mqttTopic.c_str(), 0, false, buffer, len))
                ESP_LOGI(TAG, "  -> Event sent via WiFi MQTT");
            else
            {
                ESP_LOGW(TAG, "  -> Event publish FAILED! Forcing reconnect...");
                mqttClient.disconnect();
            }
        }
    }
} // End of WiFiManager namespace
