#include "NetworkManager.h"
#include "config.h"
#include "CommandProcessor.h"
#include "HealthManager.h"
#include "Indicator.h"
#include "SensorManager.h"
#include "AutomationManager.h"
#include "ScheduleManager.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "esp_task_wdt.h"

extern Preferences preferences;

WiFiClient espClient;
PubSubClient client(espClient);
HardwareSerial SerialAT(1);

String currentSSID = "";
String currentPassword = "";
String macAddress;
String mqttTopic;
String device_id;
String macAddress_gsm;
String gsmClientId;
String pubTopic;
String subTopic;
unsigned long lastTelemetry = 0;
String lastSignalStrength = "N/A";

unsigned long lastWifiRetry = 0;
unsigned long wifiBackoffMs = 5000;
unsigned long lastMqttRetry = 0;
unsigned long mqttBackoffMs = 5000;
bool ntpSyncedThisBoot = false;

static SystemData *sysDataPtr = nullptr;

namespace NetworkManager
{
    String getTimestamp()
    {
        struct tm timeinfo;
        if (!getLocalTime(&timeinfo, 0))
            return "1970-01-01T00:00:00";
        char buffer[25];
        strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
        return String(buffer);
    }

    void callback(char *topic, byte *payload, unsigned int length)
    {
        Serial.println("\n[WiFi] Message Received:");
        Serial.write(payload, length);
        Serial.println();

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload, length);
        if (error)
        {
            Serial.println("JSON Parse Failed");
            return;
        }

        CommandProcessor::processJSON(doc);
    }

    void setup_wifi()
    {
        sysDataPtr->currentState = SYS_WIFI_CONN;
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
            Indicator::update(sysDataPtr->currentState, sysDataPtr->radarAutoMode, sysDataPtr->cachedPresence);
            AutomationManager::loop();
            HealthManager::loop();
            if (sysDataPtr->isAPMode)
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
        sysDataPtr->currentState = SYS_WIFI_OK;
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
    }

    void reconnect()
    {
        if (sysDataPtr->isAPMode)
            return;
        if (WiFi.status() != WL_CONNECTED)
            return;
        if (client.connected())
            return;

        Serial.print("Connecting to MQTT...");
        String lwtTopic = "/status/" + device_id;
        String lwtMessage = "{\"status\": \"Offline\", \"reason\": \"Connection Lost\"}";

        if (client.connect(device_id.c_str(), NULL, NULL, lwtTopic.c_str(), 1, true, lwtMessage.c_str()))
        {
            Serial.println("connected!");
            String onlineMsg = "{\"status\": \"Online\", \"reason\": \"Connected\"}";
            client.publish(lwtTopic.c_str(), onlineMsg.c_str(), true);
            client.subscribe(mqttTopic.c_str());

            static bool bootAlertSent = false;
            if (!bootAlertSent)
            {
                JsonDocument bootDoc;
                JsonObject bd = bootDoc[device_id].to<JsonObject>();
                bd["event"] = "boot";
                bd["reset_reason"] = HealthManager::getResetReason();
                bd["hdc_init"] = sysDataPtr->hdcInitFailed ? "FAILED" : "OK";
                bd["radar_init"] = sysDataPtr->radarInitFailed ? "FAILED" : "OK";
                char bootBuf[256];
                serializeJson(bootDoc, bootBuf);
                client.publish(mqttTopic.c_str(), bootBuf);
                bootAlertSent = true;
                Serial.println("[BOOT] Boot alert sent via MQTT.");
            }
        }
        else
        {
            Serial.print("failed, rc=");
            Serial.println(client.state());
        }
    }

    String getChipMAC()
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char buf[13];
        sprintf(buf, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return String(buf);
    }

    String sendAT(String command, uint32_t timeoutMs = 2000)
    {
        Serial.print(">> ");
        Serial.println(command);
        SerialAT.println(command);
        String response = "";
        response.reserve(256);
        uint32_t start = millis();
        while (millis() - start < timeoutMs)
        {
            while (SerialAT.available())
            {
                char c = SerialAT.read();
                response += c;
            }
            if (response.indexOf("\r\nOK\r\n") != -1 || response.indexOf("\r\nERROR\r\n") != -1 || response.indexOf(">") != -1)
                break;
        }
        Serial.print(response);
        return response;
    }

    void checkIncomingData()
    {
        static String incoming;
        static bool initialized = false;
        if (!initialized)
        {
            incoming.reserve(256);
            initialized = true;
        }

        if (SerialAT.available())
        {
            incoming = SerialAT.readStringUntil('\n');
            {
                Serial.print("\n[ASYNC] << ");
                Serial.println(incoming);
                if (incoming.indexOf("+QMTRECV:") != -1)
                {
                    int jsonStart = incoming.indexOf('{');
                    int jsonEnd = incoming.lastIndexOf('}');
                    if (jsonStart != -1 && jsonEnd != -1 && jsonEnd > jsonStart)
                    {
                        String jsonStr = incoming.substring(jsonStart, jsonEnd + 1);
                        JsonDocument doc;
                        if (!deserializeJson(doc, jsonStr))
                        {
                            Serial.println("\n[GSM] Message Received.");
                            CommandProcessor::processJSON(doc);
                        }
                        else
                        {
                            Serial.println("Failed to parse JSON from MQTT message.");
                        }
                    }
                }
            }
        }
    }

    String getSignalStrength()
    {
        String resp = sendAT("AT+CSQ", 2000);
        int idx = resp.indexOf("+CSQ: ");
        if (idx != -1)
        {
            int commaIdx = resp.indexOf(",", idx);
            if (commaIdx != -1)
            {
                String csqVal = resp.substring(idx + 6, commaIdx);
                csqVal.trim();
                return csqVal;
            }
        }
        return "N/A";
    }

    String getModemTime()
    {
        String resp = sendAT("AT+CCLK?", 2000);
        int idx = resp.indexOf("+CCLK: \"");
        if (idx != -1)
        {
            int endIdx = resp.indexOf("\"", idx + 8);
            if (endIdx != -1)
                return resp.substring(idx + 8, endIdx);
        }
        return "N/A";
    }

    String getGSMTime()
    {
        String response = sendAT("AT+CCLK?", 2000);
        int first = response.indexOf('"');
        int last = response.lastIndexOf('"');
        if (first != -1 && last != -1)
        {
            String t = response.substring(first + 1, last);
            struct tm timeinfo;
            timeinfo.tm_year = t.substring(0, 2).toInt() + 100;
            timeinfo.tm_mon = t.substring(3, 5).toInt() - 1;
            timeinfo.tm_mday = t.substring(6, 8).toInt();
            timeinfo.tm_hour = t.substring(9, 11).toInt();
            timeinfo.tm_min = t.substring(12, 14).toInt();
            timeinfo.tm_sec = t.substring(15, 17).toInt();
            time_t utc_time = mktime(&timeinfo);
            utc_time += (GMT_OFFSET_SEC);
            struct tm *local_tm = gmtime(&utc_time);
            char buffer[25];
            strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", local_tm);
            return String(buffer);
        }
        return "1970-01-01T00:00:00";
    }

    void sendAutomationEvent(String eventCode)
    {
        Serial.println("\n[EVENT] Sending immediate automation event: " + eventCode);
        JsonDocument doc;
        String currentId = sysDataPtr->switch_gsm_wifi ? device_id : gsmClientId;
        JsonObject data = doc[currentId].to<JsonObject>();

        data["timestamp"] = sysDataPtr->switch_gsm_wifi ? getTimestamp() : getGSMTime();
        data["auto_event"] = eventCode;
        char buffer[256];
        serializeJson(doc, buffer);

        if (sysDataPtr->switch_gsm_wifi)
        {
            if (client.connected())
            {
                if (client.publish(mqttTopic.c_str(), buffer))
                    Serial.println("  -> Event sent via WiFi MQTT");
                else
                {
                    Serial.println("  -> Event publish FAILED! Forcing reconnect...");
                    client.disconnect();
                }
            }
        }
        else
        {
            String cmd = "AT+QMTPUB=0,1,1,0,\"" + pubTopic + "\"";
            String pubResp = sendAT(cmd, 5000);
            if (pubResp.indexOf(">") != -1)
            {
                SerialAT.print(buffer);
                SerialAT.write(0x1A);
                Serial.println("  -> Event sent via GSM MQTT");
            }
            else
            {
                Serial.println("  -> Failed to get GSM prompt for event.");
            }
        }
    }

    int batteryPercentage() { return 80; }

    void publishACK(const char *action, const char *detail)
    {
        JsonDocument doc;
        String id = sysDataPtr->switch_gsm_wifi ? device_id : gsmClientId;
        JsonObject obj = doc[id].to<JsonObject>();
        obj["ack"] = "ok";
        obj["action"] = action;
        obj["detail"] = detail;
        char buf[256];
        serializeJson(doc, buf);

        if (sysDataPtr->switch_gsm_wifi)
        {
            if (client.connected())
                client.publish(mqttTopic.c_str(), buf);
        }
        else
        {
            String cmd = "AT+QMTPUB=0,1,1,0,\"" + pubTopic + "\"";
            String resp = sendAT(cmd, 3000);
            if (resp.indexOf(">") != -1)
            {
                SerialAT.print(buf);
                SerialAT.write(0x1A);
            }
        }
        Serial.printf("[ACK] action=%s detail=%s\n", action, detail);
    }

    void publishHealthAlert(const char *event, const char *detail)
    {
        JsonDocument doc;
        String id = sysDataPtr->switch_gsm_wifi ? device_id : gsmClientId;
        JsonObject obj = doc[id].to<JsonObject>();
        obj["event"] = event;
        obj["detail"] = detail;
        char buf[192];
        serializeJson(doc, buf);

        if (sysDataPtr->switch_gsm_wifi)
        {
            if (client.connected())
                client.publish(mqttTopic.c_str(), buf);
        }
        else
        {
            String cmd = "AT+QMTPUB=0,1,1,0,\"" + pubTopic + "\"";
            String resp = sendAT(cmd, 5000);
            if (resp.indexOf(">") != -1)
            {
                SerialAT.print(buf);
                SerialAT.write(0x1A);
            }
        }
        Serial.printf("[HEALTH ALERT] %s — %s\n", event, detail);
    }

    void setSystemTimeFromGSM()
    {
        String response = sendAT("AT+CCLK?", 2000);
        int first = response.indexOf('"');
        int last = response.lastIndexOf('"');
        if (first == -1 || last == -1)
            return;
        String t = response.substring(first + 1, last);
        if (t.length() < 17)
            return;
        struct tm timeinfo = {};
        timeinfo.tm_year = t.substring(0, 2).toInt() + 100;
        timeinfo.tm_mon = t.substring(3, 5).toInt() - 1;
        timeinfo.tm_mday = t.substring(6, 8).toInt();
        timeinfo.tm_hour = t.substring(9, 11).toInt();
        timeinfo.tm_min = t.substring(12, 14).toInt();
        timeinfo.tm_sec = t.substring(15, 17).toInt();
        time_t utc = mktime(&timeinfo);
        utc += (GMT_OFFSET_SEC);
        struct timeval tv = {utc, 0};
        settimeofday(&tv, nullptr);
        Serial.println("[SCHED] System clock synced from GSM modem.");
    }

    void init(SystemData *state)
    {
        sysDataPtr = state;

        currentSSID = preferences.getString("wifi_ssid", "");
        currentPassword = preferences.getString("wifi_pass", "");

        if (sysDataPtr->switch_gsm_wifi)
        {
            macAddress = WiFi.macAddress();
            macAddress.replace(":", "");
            device_id = macAddress;
            mqttTopic = "/topic/" + macAddress;
            Serial.println("Device ID: " + device_id);

            setup_wifi();

            if (WiFi.status() == WL_CONNECTED)
            {
                configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
                struct tm timeinfo;
                unsigned long ntpStart = millis();
                const unsigned long NTP_TIMEOUT = 8000;
                while (!getLocalTime(&timeinfo) && (millis() - ntpStart < NTP_TIMEOUT))
                {
                    esp_task_wdt_reset();
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
                if (getLocalTime(&timeinfo))
                {
                    Serial.println("[NTP] Time sync OK.");
                    ntpSyncedThisBoot = true;
                }
                else
                {
                    Serial.println("[NTP] Time sync timed out.");
                }
            }
            else
            {
                Serial.println("[BOOT] WiFi offline.");
            }

            client.setBufferSize(512);
            client.setServer(MQTT_SERVER, MQTT_PORT);
            client.setCallback(callback);
        }
        else
        {
            macAddress_gsm = getChipMAC();
            gsmClientId = macAddress_gsm;
            pubTopic = "/topic/" + macAddress_gsm;
            subTopic = "/topic/" + macAddress_gsm;
            Serial.print("GSM Client ID: ");
            Serial.println(gsmClientId);
            Serial.print("Pub Topic: ");
            Serial.println(pubTopic);
            Serial.print("Sub Topic: ");
            Serial.println(subTopic);
            SerialAT.begin(115200, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);

            int connFailCount = 0;
            const int MAX_CONN_RETRIES = 3;
            bool mqttConnected = false;
            while (!mqttConnected && connFailCount < MAX_CONN_RETRIES)
            {
                esp_task_wdt_reset();
                if (sysDataPtr->isAPMode)
                    break;

                sysDataPtr->currentState = SYS_GSM_CONN;
                Serial.println("\n[STEP 1] Checking Modem...");
                sendAT("AT", 1000);
                sendAT("ATE0", 1000);
                sendAT("AT+CGMI", 2000);

                Serial.println("\n[STEP 2] Checking SIM...");
                bool simReady = false;

                while (!simReady)
                {
                    if (sysDataPtr->isAPMode)
                        break;

                    String simResp = sendAT("AT+CPIN?", 3000);

                    if (simResp.indexOf("READY") != -1)
                    {
                        simReady = true;
                        Serial.println("✓ SIM is inserted and ready!");
                    }
                    else
                    {
                        Serial.println("⚠ SIM not inserted or not ready!");
                        Serial.println("Waiting 10 seconds for user to insert SIM...");
                        vTaskDelay(pdMS_TO_TICKS(10000));

                        Serial.println("\nRestarting modem radio to scan the physical SIM slot...");
                        sendAT("AT+CFUN=0", 5000);
                        vTaskDelay(pdMS_TO_TICKS(3000));
                        sendAT("AT+CFUN=1", 5000);
                        vTaskDelay(pdMS_TO_TICKS(8000));
                    }
                }

                if (sysDataPtr->isAPMode)
                    break;

                Serial.println("\n[DIAGNOSTICS] Checking Signal and Radio...");
                sendAT("AT+CFUN=1", 3000);
                vTaskDelay(pdMS_TO_TICKS(2000));
                sendAT("AT+CSQ", 2000);
                sendAT("AT+COPS?", 2000);

                Serial.println("\n[STEP 3] Waiting for Network Registration...");
                bool registered = false;
                for (int i = 0; i < 30; i++)
                {
                    if (sysDataPtr->isAPMode)
                        break;
                    String regResp = sendAT("AT+CREG?", 2000);
                    if (regResp.indexOf(",1") != -1 || regResp.indexOf(",5") != -1)
                    {
                        registered = true;
                        Serial.println("✓ Network registered!");
                        break;
                    }
                    Serial.println("  ...still searching...");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }

                if (!registered)
                {
                    Serial.println("✗ Network registration failed!");
                    connFailCount++;
                    if (connFailCount >= MAX_CONN_RETRIES)
                    {
                        Serial.println("\nMAX RETRIES — Restarting modem via AT+CFUN...");
                        sendAT("AT+CFUN=0", 5000);
                        vTaskDelay(pdMS_TO_TICKS(3000));
                        sendAT("AT+CFUN=1", 5000);
                        vTaskDelay(pdMS_TO_TICKS(10000));
                        connFailCount = 0;
                    }
                    continue;
                }

                Serial.println("\n[STEP 4] Activating PDP Context...");
                sendAT("AT+QIACT=1", 10000);
                vTaskDelay(pdMS_TO_TICKS(2000));
                sendAT("AT+QIACT?", 3000);

                Serial.println("\n[STEP 5] Opening MQTT Connection...");
                sendAT("AT+QMTCLOSE=0", 3000);
                vTaskDelay(pdMS_TO_TICKS(1000));

                String lwtCmd = "AT+QMTCFG=\"will\",0,1,1,1,\"/status/" + gsmClientId + "\",\"Offline\"";
                sendAT(lwtCmd, 2000);

                String cmd = "AT+QMTOPEN=0,\"" + String(MQTT_SERVER) + "\"," + String(MQTT_PORT);
                sendAT(cmd, 5000);

                Serial.println("  Waiting for +QMTOPEN URC...");
                bool openSuccess = false;
                uint32_t waitStart = millis();
                String urcBuffer = "";
                while (millis() - waitStart < 15000)
                {
                    while (SerialAT.available())
                    {
                        char c = SerialAT.read();
                        urcBuffer += c;
                        Serial.print(c);
                    }
                    if (urcBuffer.indexOf("+QMTOPEN: 0,0") != -1)
                    {
                        openSuccess = true;
                        Serial.println("\n✓ MQTT TCP connection opened!");
                        break;
                    }
                    if (urcBuffer.indexOf("+QMTOPEN: 0,-1") != -1 || urcBuffer.indexOf("ERROR") != -1)
                    {
                        Serial.println("\n✗ MQTT TCP connection failed!");
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                }

                if (!openSuccess)
                {
                    connFailCount++;
                    if (connFailCount >= MAX_CONN_RETRIES)
                    {
                        Serial.println("\n⚠⚠⚠ MAX RETRIES — Restarting modem...");
                        sendAT("AT+CFUN=0", 5000);
                        vTaskDelay(pdMS_TO_TICKS(3000));
                        sendAT("AT+CFUN=1", 5000);
                        vTaskDelay(pdMS_TO_TICKS(10000));
                        connFailCount = 0;
                    }
                    continue;
                }

                vTaskDelay(pdMS_TO_TICKS(2000));

                Serial.println("\n[STEP 6] Logging into MQTT Broker...");
                cmd = "AT+QMTCONN=0,\"" + gsmClientId + "\"";
                String connResp = sendAT(cmd, 5000);

                String connURC = "";
                waitStart = millis();
                while (millis() - waitStart < 10000)
                {
                    while (SerialAT.available())
                    {
                        char c = SerialAT.read();
                        connURC += c;
                        Serial.print(c);
                    }
                    if (connURC.indexOf("+QMTCONN: 0,0,0") != -1)
                    {
                        mqttConnected = true;
                        Serial.println("\n✓ MQTT broker connected!");
                        break;
                    }
                    if (connResp.indexOf("+CME ERROR") != -1 || connURC.indexOf("+CME ERROR") != -1)
                    {
                        Serial.println("\n✗ +CME ERROR detected!");
                        break;
                    }
                    if (connURC.indexOf("ERROR") != -1 || connURC.indexOf("+QMTCONN: 0,") != -1)
                    {
                        Serial.println("\n✗ MQTT connect failed!");
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                }

                if (!mqttConnected)
                {
                    connFailCount++;
                    if (connFailCount >= MAX_CONN_RETRIES)
                    {
                        Serial.println("\n⚠⚠⚠ 3 FAILURES — RESTARTING MODEM ⚠⚠⚠");
                        sendAT("AT+QMTCLOSE=0", 3000);
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        sendAT("AT+CFUN=0", 5000);
                        vTaskDelay(pdMS_TO_TICKS(3000));
                        sendAT("AT+CFUN=1", 5000);
                        vTaskDelay(pdMS_TO_TICKS(10000));
                        connFailCount = 0;
                    }
                    else
                    {
                        vTaskDelay(pdMS_TO_TICKS(3000));
                    }
                }
            }

            if (mqttConnected)
            {
                vTaskDelay(pdMS_TO_TICKS(2000));
                Serial.println("\n[STEP 7] Subscribing to Topic...");
                String cmd = "AT+QMTSUB=0,1,\"" + subTopic + "\",0";
                sendAT(cmd, 5000);
                uint32_t waitStart = millis();
                while (millis() - waitStart < 5000)
                {
                    checkIncomingData();
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                Serial.println("\n============= SETUP COMPLETE =============");
                Serial.println("✓ MQTT connected & subscribed!");
                sysDataPtr->currentState = SYS_GSM_OK;
                setSystemTimeFromGSM();

                JsonDocument bootDoc;
                JsonObject bd = bootDoc[gsmClientId].to<JsonObject>();
                bd["event"] = "boot";
                bd["reset_reason"] = HealthManager::getResetReason();
                bd["hdc_init"] = sysDataPtr->hdcInitFailed ? "FAILED" : "OK";
                bd["radar_init"] = sysDataPtr->radarInitFailed ? "FAILED" : "OK";
                char bootBuf[256];
                serializeJson(bootDoc, bootBuf);
                String bootCmd = "AT+QMTPUB=0,1,1,0,\"" + pubTopic + "\"";
                String bootResp = sendAT(bootCmd, 5000);
                if (bootResp.indexOf(">") != -1)
                {
                    SerialAT.print(bootBuf);
                    SerialAT.write(0x1A);
                    Serial.println("[BOOT] GSM boot alert sent.");
                }
            }
            else
            {
                Serial.println("\n✗✗✗ FAILED to establish MQTT ✗✗✗");
            }
        }
    }

    void loop()
    {
        if (!sysDataPtr)
            return;
        static bool bootReasonReported = false;

        if (sysDataPtr->switch_gsm_wifi)
        {
            if (WiFi.status() != WL_CONNECTED)
            {
                if (sysDataPtr->currentState != SYS_WIFI_CONN)
                {
                    sysDataPtr->currentState = SYS_WIFI_CONN;
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

            if (sysDataPtr->currentState == SYS_WIFI_CONN)
            {
                Serial.println("\n[WIFI] Reconnected!");
                sysDataPtr->currentState = SYS_WIFI_OK;
                wifiBackoffMs = 5000;
                if (!ntpSyncedThisBoot)
                {
                    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
                    struct tm tinfo;
                    if (getLocalTime(&tinfo))
                    {
                        ntpSyncedThisBoot = true;
                        Serial.println("[NTP] Late time sync OK.");
                    }
                }
            }

            if (!client.connected())
            {
                if (millis() - lastMqttRetry >= mqttBackoffMs)
                {
                    lastMqttRetry = millis();
                    reconnect();
                    if (client.connected())
                    {
                        mqttBackoffMs = 5000;
                    }
                    else
                    {
                        mqttBackoffMs = min(mqttBackoffMs * 2, MAX_BACKOFF_MS);
                    }
                }
                return;
            }
            client.loop();

            if (millis() - lastTelemetry > TELEMETRY_INTERVAL)
            {
                unsigned long now = millis();
                lastTelemetry = now;

                if (sysDataPtr->lastPresenceState == true)
                {
                    sysDataPtr->accumulatedPresenceMs += (now - sysDataPtr->lastStateChangeTime);
                }

                int total_interval_secs = round(TELEMETRY_INTERVAL / 1000.0);
                int presence_secs = round(sysDataPtr->accumulatedPresenceMs / 1000.0);

                if (presence_secs > total_interval_secs)
                    presence_secs = total_interval_secs;
                int empty_secs = total_interval_secs - presence_secs;

                sysDataPtr->accumulatedPresenceMs = 0;
                sysDataPtr->lastStateChangeTime = now;

                float temperature = sysDataPtr->currentTemp;
                float humidity = sysDataPtr->currentHumidity;
                int rssi = WiFi.RSSI();

                JsonDocument doc;
                JsonObject mac = doc[device_id].to<JsonObject>();
                mac["timestamp"] = getTimestamp();
                mac["radar_auto_mode"] = sysDataPtr->radarAutoMode ? "Enabled" : "Disabled";
                if (sysDataPtr->radarAutoMode)
                {
                    mac["presence_seconds"] = presence_secs;
                    mac["empty_seconds"] = empty_secs;
                }
                mac["currentEcoTemp"] = sysDataPtr->currentEcoTemp;
                mac["TEcoTime"] = sysDataPtr->TEcoTime / 60000;
                mac["TOffTime"] = sysDataPtr->TOffTime / 60000;
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

                char buffer[768];
                size_t written = serializeJson(doc, buffer, sizeof(buffer));
                if (written >= sizeof(buffer))
                {
                    Serial.println("[MQTT] ERROR: JSON payload truncated!");
                }
                else if (client.publish(mqttTopic.c_str(), buffer))
                {
                    Serial.println("\n[MQTT] Telemetry Sent:");
                }
                else
                {
                    Serial.println("\n[MQTT] FAILED to send Telemetry! Forcing reconnect...");
                    client.disconnect();
                }
                serializeJsonPretty(doc, Serial);
                Serial.println();
            }
        }
        else
        {
            // GSM Routine
            checkIncomingData();

            if (millis() - lastTelemetry >= TELEMETRY_INTERVAL)
            {
                unsigned long now = millis();

                if (sysDataPtr->lastPresenceState == true)
                {
                    sysDataPtr->accumulatedPresenceMs += (now - sysDataPtr->lastStateChangeTime);
                }

                int total_interval_secs = round(TELEMETRY_INTERVAL / 1000.0);
                int presence_secs = round(sysDataPtr->accumulatedPresenceMs / 1000.0);

                if (presence_secs > total_interval_secs)
                    presence_secs = total_interval_secs;
                int empty_secs = total_interval_secs - presence_secs;

                sysDataPtr->accumulatedPresenceMs = 0;
                sysDataPtr->lastStateChangeTime = now;

                lastTelemetry = millis();
                float temperature = sysDataPtr->currentTemp;
                float humidity = sysDataPtr->currentHumidity;
                lastSignalStrength = getSignalStrength();

                JsonDocument doc;
                JsonObject data = doc[gsmClientId].to<JsonObject>();
                data["timestamp"] = getGSMTime();

                data["radar_auto_mode"] = sysDataPtr->radarAutoMode ? "Enabled" : "Disabled";
                if (sysDataPtr->radarAutoMode)
                {
                    data["presence_seconds"] = presence_secs;
                    data["empty_seconds"] = empty_secs;
                }

                data["currentEcoTemp"] = sysDataPtr->currentEcoTemp;
                data["TEcoTime"] = sysDataPtr->TEcoTime / 60000;
                data["TOffTime"] = sysDataPtr->TOffTime / 60000;
                data["temperature"] = isnan(temperature) ? 0.0 : temperature;
                data["humidity"] = isnan(humidity) ? 0.0 : humidity;
                data["status"] = "Online";

                if (!bootReasonReported)
                {
                    data["last_reset_reason"] = HealthManager::getResetReason();
                    bootReasonReported = true;
                }

                data["device_type"] = "climate_sensor";
                data["battery_level"] = batteryPercentage();
                data["cellular_signal_strength"] = lastSignalStrength;
                data["uptime_s"] = millis() / 1000;
                data["free_heap"] = ESP.getFreeHeap();

                static bool lastSensorFaultGSM = false;
                bool currentSensorFaultGSM = isnan(temperature) || isnan(humidity);
                if (currentSensorFaultGSM != lastSensorFaultGSM)
                {
                    JsonDocument alertDoc;
                    JsonObject al = alertDoc[gsmClientId].to<JsonObject>();
                    al["event"] = currentSensorFaultGSM ? "sensor_fault" : "sensor_recovered";
                    al["sensor_status"] = currentSensorFaultGSM ? "FAULT" : "OK";
                    char alertBuf[128];
                    serializeJson(alertDoc, alertBuf);
                    String alertCmd = "AT+QMTPUB=0,1,1,0,\"" + pubTopic + "\"";
                    String alertResp = sendAT(alertCmd, 5000);
                    if (alertResp.indexOf(">") != -1)
                    {
                        SerialAT.print(alertBuf);
                        SerialAT.write(0x1A);
                    }
                    lastSensorFaultGSM = currentSensorFaultGSM;
                }
                if (currentSensorFaultGSM)
                {
                    Serial.println("HDC1080 Read Failed!");
                    data["sensor_status"] = "FAULT";
                    data["temperature"] = nullptr;
                    data["humidity"] = nullptr;
                }
                else
                {
                    data["sensor_status"] = "OK";
                    data["temperature"] = temperature;
                    data["humidity"] = humidity;
                }

                char jsonBuffer[768];
                size_t gsmWritten = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
                if (gsmWritten >= sizeof(jsonBuffer))
                {
                    Serial.println("[GSM] ERROR: JSON payload truncated!");
                }
                else
                {
                    String cmd = "AT+QMTPUB=0,1,1,0,\"" + pubTopic + "\"";
                    String pubResp = sendAT(cmd, 5000);

                    if (pubResp.indexOf(">") != -1)
                    {
                        SerialAT.print(jsonBuffer);
                        SerialAT.write(0x1A);
                        Serial.println(">> Payload sent via GSM!");
                    }
                    else if (pubResp.indexOf("ERROR") != -1)
                    {
                        Serial.println("\n[FATAL ERROR] Modem disconnected or crashed!");
                        Serial.println("Rebooting ESP32 to re-establish clean connection...");
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        ESP.restart();
                    }
                    else
                    {
                        Serial.println("✗ No '>' prompt received! QMTPUB failed.");
                        sendAT("AT+QMTCONN?", 3000);
                    }
                }
            }
        }
    }
    void TaskNetwork(void *pvParameters)
    {
        for (;;)
        {
            // NetworkManager handles its own blocking. If GSM takes 5 seconds,
            // it only blocks this specific task.
            if (!sysData.isAPMode)
            {
                loop();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}