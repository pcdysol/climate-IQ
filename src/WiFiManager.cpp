#include "WiFiManager.h"
#include "config.h"
#include "CommandProcessor.h"
#include "HealthManager.h"
#include <WiFi.h>
// #include <PubSubClient.h>
#include <AsyncMqttClient.h>
#include <Preferences.h>
#include "SharedState.h"
#include "esp_task_wdt.h"
#include "indicator.h"
#include "GSMManager.h"
#include "esp_sntp.h"
#include <atomic>

extern Preferences preferences;
extern SystemData sysData;
// WiFiClient espClient;
// PubSubClient client(espClient);
AsyncMqttClient mqttClient;
unsigned long lastWifiRetry = 0;
unsigned long wifiBackoffMs = 5000;
unsigned long lastMqttRetry = 0;
unsigned long mqttBackoffMs = 5000;
// std::atomic<bool> timeValid{false};
unsigned long lastTelemetry = 0;

// The thread-safe callback
// void timeSyncCallback(struct timeval *tv)
// {
//     Serial.println("\n[NTP] Time synchronization event! Real time acquired.");
//     timeValid.store(true, std::memory_order_relaxed);
// }

namespace WiFiManager
{
    // WiFiClient espClient;
    // PubSubClient client(espClient);

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

    void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)
    {
        Serial.println("\n[WiFi] Message Received:");

        // AsyncMqttClient payload is NOT null-terminated. We must copy it.
        char msgBuffer[len + 1];
        memcpy(msgBuffer, payload, len);
        msgBuffer[len] = '\0';
        Serial.println(msgBuffer);

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, msgBuffer);
        if (error)
        {
            Serial.println("JSON Parse Failed");
            return;
        }

        CommandProcessor::processJSON(doc);
    }

    void setup_wifi()
    {
        sysData.currentState = SYS_WIFI_CONN;
        Serial.print("Connecting to WiFi");
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
                Serial.println("\n[WIFI] Boot timeout — continuing offline. Will retry in background.");
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            Serial.print(".");
        }
        Serial.println("\nWiFi Connected!");
        sysData.currentState = SYS_WIFI_OK;
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
    }

    void onMqttConnect(bool sessionPresent)
    {
        Serial.println("\n[MQTT] Connected to Broker!");

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
            char bootBuf[256];
            size_t len = serializeJson(bootDoc, bootBuf);

            // Note the new publish signature! (topic, qos, retain, payload)
            mqttClient.publish(mqttTopic.c_str(), 0, false, bootBuf, len);
            bootAlertSent = true;
            Serial.println("[BOOT] Boot alert sent via MQTT.");
        }
    }

    void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
    {
        Serial.println("\n[MQTT] Disconnected from Broker.");
    }

    void reconnect()
    {
        if (sysData.isAPMode)
            return;
        if (WiFi.status() != WL_CONNECTED)
            return;
        if (mqttClient.connected())
            return;

        Serial.println("Connecting to MQTT...");
        mqttClient.connect(); // This is asynchronous. It takes no arguments!
    }

    String getChipMAC()
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char buf[13];
        sprintf(buf, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return String(buf);
    }

    void init()
    {
        currentSSID = preferences.getString("wifi_ssid", "");
        currentPassword = preferences.getString("wifi_pass", "");

        macAddress = WiFi.macAddress();
        macAddress.replace(":", "");
        device_id = macAddress;
        mqttTopic = "/topic/" + macAddress;
        Serial.println("Device ID: " + device_id);

        setup_wifi();

        if (WiFi.status() == WL_CONNECTED)
        {
            // 1. Register the asynchronous callback BEFORE configuring time
            // sntp_set_time_sync_notification_cb(timeSyncCallback);
            // --- ADD THIS LINE: Tells the native SNTP client to resync every 30 mins ---
            sntp_set_sync_interval(30000);

            // 2. Fire and forget - LwIP handles internal retries
            configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
            Serial.println("[NTP] Time sync requested via LwIP...");
        }
        else
        {
            Serial.println("[BOOT] WiFi offline.");
        }

        mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
        mqttClient.setClientId(device_id.c_str());
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

    int batteryPercentage() { return 80; }

    void loop()
    {
        static bool bootReasonReported = false;

        if (WiFi.status() != WL_CONNECTED)
        {
            if (sysData.currentState != SYS_WIFI_CONN)
            {
                sysData.currentState = SYS_WIFI_CONN;
                Serial.println("\n[WIFI] Connection lost — running offline. Background retry active.");
            }
            if (millis() - lastWifiRetry >= wifiBackoffMs)
            {
                lastWifiRetry = millis();
                Serial.printf("[WIFI] Retry attempt (next in ~%lus if this fails)\n", wifiBackoffMs / 1000);
                WiFi.disconnect();
                WiFi.begin(currentSSID.c_str(), currentPassword.c_str());
                wifiBackoffMs = min(wifiBackoffMs * 2, MAX_BACKOFF_MS);
            }
            return;
        }

        if (sysData.currentState == SYS_WIFI_CONN)
        {
            Serial.println("\n[WIFI] Reconnected!");
            sysData.currentState = SYS_WIFI_OK;
            wifiBackoffMs = 5000;
            // Re-assert SNTP servers upon reconnect to ensure LwIP resumes
            configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
        }

        if (!mqttClient.connected())
        {
            if (millis() - lastMqttRetry >= mqttBackoffMs)
            {
                lastMqttRetry = millis();
                reconnect();
                // Just double it. If it successfully connects, onMqttConnect will reset it.
                mqttBackoffMs = min(mqttBackoffMs * 2, MAX_BACKOFF_MS);
            }
            return;
        }

        if (millis() - lastTelemetry > TELEMETRY_INTERVAL)
        {
            unsigned long now = millis();
            lastTelemetry = now;

            // Replace the old accumulatedPresenceMs lines with this:
            if (sysData.lastPresenceState == true)
            {
                uint32_t delta = (now - sysData.lastStateChangeTime);
                sysData.accumulatedPresenceMs.fetch_add(delta, std::memory_order_relaxed);
            }

            int total_interval_secs = round(TELEMETRY_INTERVAL / 1000.0);

            // ATOMICALLY read and reset to 0
            uint32_t currentAccumulatedMs = sysData.accumulatedPresenceMs.exchange(0, std::memory_order_relaxed);
            int presence_secs = round(currentAccumulatedMs / 1000.0);

            if (presence_secs > total_interval_secs)
                presence_secs = total_interval_secs;
            int empty_secs = total_interval_secs - presence_secs;

            sysData.accumulatedPresenceMs = 0;
            sysData.lastStateChangeTime = now;

            float temperature = sysData.currentTemp;
            float humidity = sysData.currentHumidity;
            int rssi = WiFi.RSSI();

            JsonDocument doc;
            JsonObject mac = doc[device_id].to<JsonObject>();
            // Thread-safe read
            // bool isTimeValid = timeValid.load(std::memory_order_relaxed);
            mac["timestamp"] = getTimestamp();
            mac["radar_auto_mode"] = sysData.radarAutoMode ? "Enabled" : "Disabled";
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
            mac["status"] = "Online";

            if (!bootReasonReported)
            {
                mac["last_reset_reason"] = HealthManager::getResetReason();
                bootReasonReported = true;
            }
            mac["device_type"] = "climate_sensor";
            mac["battery_level"] = batteryPercentage();
            mac["wifi_signal_strength"] = rssi;
            mac["uptime_s"] = millis() / 1000;
            mac["free_heap"] = ESP.getFreeHeap();
            mac["max_alloc_heap"] = ESP.getMaxAllocHeap(); // The largest single vehicle that can park

            static char buffer[768];
            size_t written = serializeJson(doc, buffer, sizeof(buffer));
            if (written >= sizeof(buffer))
            {
                Serial.println("[MQTT] ERROR: JSON payload truncated!");
            }
            else if (mqttClient.publish(mqttTopic.c_str(), 0, false, buffer, written))
            {
                Serial.println("\n[MQTT] Telemetry Sent:");
            }
            else
            {
                Serial.println("\n[MQTT] FAILED to send Telemetry! Forcing reconnect...");
                mqttClient.disconnect();
            }
            serializeJsonPretty(doc, Serial);
            Serial.println();
        }
    }

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
        Serial.printf("[ACK] action=%s detail=%s\n", action, detail);
    }

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
        Serial.printf("[HEALTH ALERT] %s — %s\n", event, detail);
    }

    void sendAutomationEvent(String eventCode)
    {
        Serial.println("\n[EVENT] Sending immediate automation event: " + eventCode);
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
                Serial.println("  -> Event sent via WiFi MQTT");
            else
            {
                Serial.println("  -> Event publish FAILED! Forcing reconnect...");
                mqttClient.disconnect();
            }
        }
    }
} // End of WiFiManager namespace
