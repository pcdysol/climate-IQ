#include "GSMManager.h"
#include "config.h"
#include "CommandProcessor.h"
#include "HealthManager.h"
#include "NetworkManager.h"
#include "esp_task_wdt.h"
#include "WiFiManager.h"

extern SystemData sysData;
unsigned long lastTelemetrygsm = 0;
String lastSignalStrength = "N/A";

namespace GSMManager
{
    HardwareSerial SerialAT(1);

    String macAddress_gsm;
    String gsmClientId;
    String pubTopic;
    String subTopic;

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
        String currentId = gsmClientId;
        JsonObject data = doc[currentId].to<JsonObject>();

        data["timestamp"] = getGSMTime();
        data["auto_event"] = eventCode;
        char buffer[256];
        serializeJson(doc, buffer);

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

    int batteryPercentage() { return 80; }

    void publishACK(const char *action, const char *detail)
    {
        JsonDocument doc;
        String id = gsmClientId;
        JsonObject obj = doc[id].to<JsonObject>();
        obj["ack"] = "ok";
        obj["action"] = action;
        obj["detail"] = detail;
        char buf[256];
        serializeJson(doc, buf);

        String cmd = "AT+QMTPUB=0,1,1,0,\"" + pubTopic + "\"";
        String resp = sendAT(cmd, 3000);
        if (resp.indexOf(">") != -1)
        {
            SerialAT.print(buf);
            SerialAT.write(0x1A);
        }

        Serial.printf("[ACK] action=%s detail=%s\n", action, detail);
    }

    void publishHealthAlert(const char *event, const char *detail)
    {
        JsonDocument doc;
        String id = gsmClientId;
        JsonObject obj = doc[id].to<JsonObject>();
        obj["event"] = event;
        obj["detail"] = detail;
        char buf[192];
        serializeJson(doc, buf);

        String cmd = "AT+QMTPUB=0,1,1,0,\"" + pubTopic + "\"";
        String resp = sendAT(cmd, 5000);
        if (resp.indexOf(">") != -1)
        {
            SerialAT.print(buf);
            SerialAT.write(0x1A);
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

    void init()
    {
        macAddress_gsm = WiFiManager::getChipMAC();
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
            if (sysData.isAPMode)
                break;

            sysData.currentState = SYS_GSM_CONN;
            Serial.println("\n[STEP 1] Checking Modem...");
            sendAT("AT", 1000);
            sendAT("ATE0", 1000);
            sendAT("AT+CGMI", 2000);

            Serial.println("\n[STEP 2] Checking SIM...");
            bool simReady = false;

            while (!simReady)
            {
                if (sysData.isAPMode)
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

            if (sysData.isAPMode)
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
                if (sysData.isAPMode)
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
            sysData.currentState = SYS_GSM_OK;
            setSystemTimeFromGSM();

            JsonDocument bootDoc;
            JsonObject bd = bootDoc[gsmClientId].to<JsonObject>();
            bd["event"] = "boot";
            bd["reset_reason"] = HealthManager::getResetReason();
            bd["hdc_init"] = sysData.hdcInitFailed ? "FAILED" : "OK";
            bd["radar_init"] = sysData.radarInitFailed ? "FAILED" : "OK";
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

    void loop()
    {
        static bool bootReasonReported = false;
        // GSM Routine
        checkIncomingData();
        // --- ADD THIS BLOCK: 30-Minute RTC Sync ---
        static unsigned long lastGsmTimeSync = 0;
        if (millis() - lastGsmTimeSync > 1800000)
        {
            lastGsmTimeSync = millis();
            Serial.println("[GSM] 30-min periodic RTC sync triggered.");
            setSystemTimeFromGSM(); // Fetches network time via AT commands and overwrites RTC
        }
        // ------------------------------------------

        if (millis() - lastTelemetrygsm >= TELEMETRY_INTERVAL)
        {
            unsigned long now = millis();

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

            lastTelemetrygsm = millis();
            float temperature = sysData.currentTemp;
            float humidity = sysData.currentHumidity;
            lastSignalStrength = getSignalStrength();

            JsonDocument doc;
            JsonObject data = doc[gsmClientId].to<JsonObject>();
            data["timestamp"] = getGSMTime();

            data["radar_auto_mode"] = sysData.radarAutoMode ? "Enabled" : "Disabled";
            if (sysData.radarAutoMode)
            {
                data["presence_seconds"] = presence_secs;
                data["empty_seconds"] = empty_secs;
            }

            data["currentEcoTemp"] = sysData.currentEcoTemp;
            data["TEcoTime"] = sysData.TEcoTime / 60000;
            data["TOffTime"] = sysData.TOffTime / 60000;
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

    // void publishACK(const char *action, const char *detail)
    // {
    // Build JSON and AT+QMTPUB
    // }

    // ... Implement sendAutomationEvent and publishHealthAlert
}