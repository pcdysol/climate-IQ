// #include <Arduino.h>
// #include <WiFi.h>
// #include <PubSubClient.h>
// #include <ArduinoJson.h>
// #include <Wire.h>               // REQUIRED for HDC1080 (I2C)
// #include "Adafruit_HDC1000.h"   // New HDC1080 Library (install via library manager)
// #include <IRremoteESP8266.h>
// #include <IRsend.h>
// #include <time.h>

// // ===== WiFi Credentials =====
// const char* ssid = "HUAWEI-2.4G-q6WR";
// const char* password = "TTcgEa82";

// // ===== MQTT Broker =====
// const char* mqtt_server = "107.172.137.233";
// const int mqtt_port = 1883;

// // ===== NTP (Internet Time) =====
// const char* ntpServer = "pool.ntp.org";
// const long gmtOffset_sec = 5 * 3600;     // Pakistan UTC +5
// const int daylightOffset_sec = 0;

// // ===== Device / MQTT =====
// String macAddress;
// String mqttTopic;
// String device_id;

// // ===== HDC1080 Humidity/Temp Sensor (I2C) =====
// // The schematic shows HDC1080 connected to:
// // SDA -> IO21 (Default ESP32 I2C SDA)
// // SCL -> IO22 (Default ESP32 I2C SCL)
// Adafruit_HDC1000 hdc = Adafruit_HDC1000();

// // ===== IR Blaster =====
// // Verified in schematic: IR/TX is on IO16 (Pin 27 of ESP32D block)
// #define IR_PIN 16
// IRsend irsend(IR_PIN);

// // ===== Interval =====
// unsigned long data_delay_interval = 30000;

// // ===== MQTT =====
// WiFiClient espClient;
// PubSubClient client(espClient);

// // ===== Get Timestamp =====
// String getTimestamp() {
//   struct tm timeinfo;
//   if (!getLocalTime(&timeinfo)) {
//     return "1970-01-01T00:00:00";
//   }
//   char buffer[25];
//   strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
//   return String(buffer);
// }

// // ===== MQTT Callback (Listening for IR commands) =====
// void callback(char* topic, byte* payload, unsigned int length) {
//   Serial.println("\nMessage Received:");

//   String jsonStr;
//   for (unsigned int i = 0; i < length; i++) {
//     jsonStr += (char)payload[i];
//   }

//   Serial.println(jsonStr);

//   StaticJsonDocument<1024> doc;
//   if (deserializeJson(doc, jsonStr)) {
//     Serial.println("JSON Parse Failed");
//     return;
//   }

//   // ===== IR COMMAND =====
//   if (doc.containsKey("ir")) {
//     const char* irStr = doc["ir"];   // e.g., "0x00D0F2F"
//     uint32_t irCode = strtoul(irStr, NULL, 16);

//     Serial.print("NIKAI IR Code Received: ");
//     Serial.println(irStr);

//     // Blast the IR signal 3 times for robustness
//     for (int i = 0; i < 3; i++) {
//       irsend.sendNikai(irCode, 24);
//       delay(40);
//     }

//     Serial.println("IR Signal Sent!");
//   }
// }

// // ===== WiFi Setup =====
// void setup_wifi() {
//   Serial.print("Connecting to WiFi");
//   WiFi.begin(ssid, password);
//   while (WiFi.status() != WL_CONNECTED) {
//     delay(500);
//     Serial.print(".");
//   }
//   Serial.println("\nWiFi Connected!");
//   Serial.print("IP: ");
//   Serial.println(WiFi.localIP());
// }

// // ===== MQTT Reconnect =====
// void reconnect() {
//   while (!client.connected()) {
//     Serial.print("Connecting to MQTT...");
//     if (client.connect(device_id.c_str())) {
//       Serial.println("connected!");
//       client.subscribe(mqttTopic.c_str());
//       Serial.print("Subscribed: ");
//       Serial.println(mqttTopic);
//     } else {
//       Serial.print("failed, rc=");
//       Serial.println(client.state());
//       delay(5000);
//     }
//   }
// }

// // ===== Battery (Dummy) =====
// int batteryPercentage() {
//   // To make this real, you would read the BATTERY READ (net bat) pin in the schematic.
//   return 80;
// }

// // ===== Setup =====
// void setup() {
//   Serial.begin(115200);

//   // Use default I2C pins for ESP32: GPIO 21 (SDA) and GPIO 22 (SCL)
//   Wire.begin();

//   setup_wifi();

//   // ===== MAC Address =====
//   macAddress = WiFi.macAddress();
//   macAddress.replace(":", "");

//   device_id = macAddress;
//   mqttTopic = "/topic/" + macAddress;

//   Serial.print("Device ID: ");
//   Serial.println(device_id);
//   Serial.print("MQTT Topic: ");
//   Serial.println(mqttTopic);

//   // ===== Time Sync =====
//   configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
//   Serial.print("Syncing time");
//   struct tm timeinfo;
//   while (!getLocalTime(&timeinfo)) {
//     Serial.print(".");
//     delay(500);
//   }
//   Serial.println("\nTime synchronized!");

//   // ===== New HDC1080 Sensor Initialization =====
//   if (!hdc.begin(0x40)) { // 0x40 is the typical default I2C address
//     Serial.println("Error: HDC1080 sensor not found! Check wiring.");
//     // Optional: add a 'while(1);' here if you want to stop if the sensor is missing.
//   } else {
//     Serial.println("HDC1080 sensor initialized.");
//   }

//   // Verified from schematic: Pin 16 used for IR/TX
//   irsend.begin();

//   client.setServer(mqtt_server, mqtt_port);
//   client.setCallback(callback);
// }

// // ===== Loop =====
// void loop() {
//   if (!client.connected()) {
//     reconnect();
//   }
//   client.loop();

//   static unsigned long lastMsg = 0;
//   if (millis() - lastMsg > data_delay_interval) {
//     lastMsg = millis();

//     // ===== Reading data from HDC1080 =====
//     float temperature = hdc.readTemperature();
//     float humidity = hdc.readHumidity();
//     int rssi = WiFi.RSSI();

//     // Note: The HDC library returns NAN on read failure, similar to DHT
//     if (isnan(temperature) || isnan(humidity)) {
//       Serial.println("HDC1080 Read Failed!");
//       return;
//     }

//     StaticJsonDocument<512> doc;
//     JsonObject mac = doc.createNestedObject(device_id);

//     mac["timestamp"] = getTimestamp();
//     mac["temperature"] = temperature;
//     mac["humidity"] = humidity;
//     mac["status"] = "Online";
//     mac["device_type"] = "climate_sensor"; // Keeps the same type description
//     mac["battery_level"] = batteryPercentage();
//     mac["wifi_signal_strength"] = rssi;

//     char buffer[512];
//     serializeJson(doc, buffer);
//     client.publish(mqttTopic.c_str(), buffer);

//     Serial.println("\nJSON Sent:");
//     serializeJsonPretty(doc, Serial);
//     Serial.println();
//   }
// }

// #include <Arduino.h>
// #include <WiFi.h>
// #include <PubSubClient.h>
// #include <ArduinoJson.h>
// #include <Wire.h>
// #include "Adafruit_HDC1000.h"
// #include <IRremoteESP8266.h>
// #include <IRsend.h>
// #include <time.h>
// #include "esp_mac.h"
// #include <IRutils.h>
// #include <ir_Electra.h>  // Include the specific AC protocol

// bool switch_gsm_wifi = true;

// // ===== WiFi Credentials =====
// // const char *ssid = "HUAWEI-2.4G-q6WR";
// // const char *password = "TTcgEa82";

// const char *ssid = "Realme GT MASTER";
// const char *password = "Waleed2024";

// // ===== MQTT Broker =====
// const char *mqtt_server = "107.172.137.233";
// const int mqtt_port = 1883;

// // ===== NTP (Internet Time) =====
// const char *ntpServer = "pool.ntp.org";
// const long gmtOffset_sec = 5 * 3600;
// const int daylightOffset_sec = 0;

// // ===== Device / MQTT =====
// String macAddress;
// String mqttTopic;
// String device_id;

// // ===== HDC1080 =====
// Adafruit_HDC1000 hdc = Adafruit_HDC1000();

// // ===== IR Blaster =====
// #define IR_PIN 18
// const uint16_t kMosfetPin = 27; // Define the MOSFET control pin;
// IRsend irsend(IR_PIN);
// IRElectraAc ac(IR_PIN);

// // ===== Interval =====
// unsigned long data_delay_interval = 10000;
// const unsigned long TELEMETRY_INTERVAL = 10000;

// // ===== Get Timestamp =====
// String getTimestamp()
// {
//   struct tm timeinfo;
//   if (!getLocalTime(&timeinfo))
//   {
//     return "1970-01-01T00:00:00";
//   }
//   char buffer[25];
//   strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
//   return String(buffer);
// }

// // ===== WiFi MQTT =====
// WiFiClient espClient;
// PubSubClient client(espClient);

// void callback(char *topic, byte *payload, unsigned int length)
// {
//   Serial.println("\nMessage Received:");
//   String jsonStr;
//   for (unsigned int i = 0; i < length; i++)
//   {
//     jsonStr += (char)payload[i];
//   }
//   Serial.println(jsonStr);

//   StaticJsonDocument<1024> doc;
//   if (deserializeJson(doc, jsonStr))
//   {
//     Serial.println("JSON Parse Failed");
//     return;
//   }

//   // ========== OPTIMIZED AC LIBRARY ROUTER ==========
//   if (doc.containsKey("ir"))
//   {
//     const char *irCmdStr = doc["ir"];

//     // Convert the string "0008" into the integer 8
//     int cmdNum = atoi(irCmdStr);

//     Serial.print("Received Command Code: ");
//     Serial.println(cmdNum);

//     // Code 1: Turn On
//     if (cmdNum == 1)
//     {
//       Serial.println("Action: Turn ON");
//       ac.on();
//       ac.send();
//     }
//     // Code 2: Turn Off
//     else if (cmdNum == 2)
//     {
//       Serial.println("Action: Turn OFF");
//       ac.off();
//       ac.send();
//     }
//     // Codes 3 through 17: Temperatures 16 through 30
//     else if (cmdNum >= 3 && cmdNum <= 17)
//     {
//       // Calculate the target temperature
//       int targetTemp = cmdNum + 13;

//       Serial.print("Action: Set Temp to ");
//       Serial.println(targetTemp);

//       ac.on();                // Make sure the virtual state is ON
//       ac.setTemp(targetTemp); // Set the calculated temperature
//       ac.send();              // Blast the signal!
//     }
//     else
//     {
//       Serial.println("ERROR: Command out of range. Must be 0001 to 0017.");
//     }
//   }
// }

// void setup_wifi()
// {
//   Serial.print("Connecting to WiFi");
//   WiFi.begin(ssid, password);
//   while (WiFi.status() != WL_CONNECTED)
//   {
//     delay(500);
//     Serial.print(".");
//   }
//   Serial.println("\nWiFi Connected!");
//   Serial.print("IP: ");
//   Serial.println(WiFi.localIP());
// }

// void reconnect()
// {
//   while (!client.connected())
//   {
//     Serial.print("Connecting to MQTT...");
//     if (client.connect(device_id.c_str()))
//     {
//       Serial.println("connected!");
//       client.subscribe(mqttTopic.c_str());
//       Serial.print("Subscribed: ");
//       Serial.println(mqttTopic);
//     }
//     else
//     {
//       Serial.print("failed, rc=");
//       Serial.println(client.state());
//       delay(5000);
//     }
//   }
// }

// // ======================= GSM Configuration START============= =====
// #define GSM_RX_PIN 16
// #define GSM_TX_PIN 17
// HardwareSerial SerialAT(1);

// // ===== Utility: MAC without WiFi =====
// String getChipMAC()
// {
//   uint8_t mac[6];
//   esp_read_mac(mac, ESP_MAC_WIFI_STA);
//   char buf[13];
//   sprintf(buf, "%02X%02X%02X%02X%02X%02X",
//           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
//   return String(buf);
// }

// // ===== GSM globals =====
// // NOTE: Don't store .c_str() of a String as a global pointer — it can dangle!
// String macAddress_gsm;  // Will be set in setup()
// String gsmClientId;     // Safe String for client ID
// String pubTopic;
// String subTopic;
// unsigned long lastTelemetry = 0;
// String lastSignalStrength = "N/A"; // Cached signal strength

// // ===== Helper to send AT commands =====
// String sendAT(String command, uint32_t timeoutMs = 2000)
// {
//   Serial.print(">> ");
//   Serial.println(command);

//   SerialAT.println(command);

//   String response = "";
//   uint32_t start = millis();
//   while (millis() - start < timeoutMs)
//   {
//     while (SerialAT.available())
//     {
//       char c = SerialAT.read();
//       response += c;
//     }
//     if (response.indexOf("\r\nOK\r\n") != -1 ||
//         response.indexOf("\r\nERROR\r\n") != -1 ||
//         response.indexOf(">") != -1)
//     {
//       break;
//     }
//   }

//   Serial.print(response);
//   return response;
// }

// // ===== Read incoming async data (AND DECODE IR COMMANDS) =====
// void checkIncomingData()
// {
//   if (SerialAT.available())
//   {
//     String incoming = SerialAT.readString();
//     if (incoming.length() > 0)
//     {
//       Serial.print("\n[ASYNC] << ");
//       Serial.println(incoming);

//       // Check if this is an incoming MQTT message from the Quectel Modem
//       // Usually formatted as: +QMTRECV: 0,0,"topic","{json payload}"
//       if (incoming.indexOf("+QMTRECV:") != -1)
//       {
//         // Extract just the JSON part between the brackets { ... }
//         int jsonStart = incoming.indexOf('{');
//         int jsonEnd = incoming.lastIndexOf('}');

//         if (jsonStart != -1 && jsonEnd != -1 && jsonEnd > jsonStart)
//         {
//           String jsonStr = incoming.substring(jsonStart, jsonEnd + 1);
//           Serial.println("Extracted JSON: " + jsonStr);

//           StaticJsonDocument<1024> doc;
//           DeserializationError error = deserializeJson(doc, jsonStr);

//           if (!error)
//           {
//             // ========== UNIVERSAL IR DECODER ==========
//             if (doc.containsKey("protocol"))
//             {
//               const char *protoStr = doc["protocol"];
//               decode_type_t irProtocol = strToDecodeType(protoStr);

//               if (irProtocol == decode_type_t::UNKNOWN)
//               {
//                 Serial.println("ERROR: Unknown Protocol.");
//                 return;
//               }

//               // PATH A: AC STATE ARRAY (For massive >64 bit codes like ELECTRA_AC)
//               if (doc.containsKey("state"))
//               {
//                 JsonArray stateArray = doc["state"].as<JsonArray>();
//                 uint16_t size = doc.containsKey("size") ? doc["size"].as<uint16_t>() : stateArray.size();

//                 uint8_t ac_state[size];
//                 for (int i = 0; i < size; i++)
//                 {
//                   ac_state[i] = stateArray[i].as<uint8_t>();
//                 }

//                 Serial.print("GSM: Sending AC State Array via Protocol: ");
//                 Serial.println(protoStr);

//                 irsend.send(irProtocol, ac_state, size);
//                 Serial.println("AC Signal Sent!");
//               }
//               // PATH B: STANDARD HEX CODE (For short codes <=64 bit)
//               else if (doc.containsKey("code"))
//               {
//                 const char *codeStr = doc["code"];
//                 uint16_t bits = doc.containsKey("bits") ? doc["bits"].as<uint16_t>() : 32;

//                 uint64_t irCode = strtoull(codeStr, NULL, 16);

//                 Serial.print("GSM: Sending Short Code via Protocol: ");
//                 Serial.println(protoStr);

//                 irsend.send(irProtocol, irCode, bits);
//                 Serial.println("Short Signal Sent!");
//               }
//             }
//           }
//           else
//           {
//             Serial.print("JSON Parse Failed: ");
//             Serial.println(error.c_str());
//           }
//         }
//       }
//     }
//   }
// }

// // ===== Parse CSQ signal value =====
// String getSignalStrength()
// {
//   String resp = sendAT("AT+CSQ", 2000);
//   // Response format: +CSQ: 18,0
//   int idx = resp.indexOf("+CSQ: ");
//   if (idx != -1)
//   {
//     int commaIdx = resp.indexOf(",", idx);
//     if (commaIdx != -1)
//     {
//       String csqVal = resp.substring(idx + 6, commaIdx);
//       csqVal.trim();
//       return csqVal;
//     }
//   }
//   return "N/A";
// }

// // ===== Get modem time (if available) =====
// String getModemTime()
// {
//   String resp = sendAT("AT+CCLK?", 2000);
//   int idx = resp.indexOf("+CCLK: \"");
//   if (idx != -1)
//   {
//     int endIdx = resp.indexOf("\"", idx + 8);
//     if (endIdx != -1)
//     {
//       return resp.substring(idx + 8, endIdx);
//     }
//   }
//   return "N/A";
// }

// // 1. Function to Sync Time with NTP Server (Run this once in setup)
// void syncNTPViaGSM() {
//   Serial.println("\n[NTP] Syncing time with pool.ntp.org...");
//   sendAT("AT+QNTP=\"pool.ntp.org\"", 5000);
//   delay(3000); // Wait for the modem to fetch time from the internet
// }

// String getGSMTime() {
//   String response = sendAT("AT+CCLK?", 2000);

//   int first = response.indexOf('"');
//   int last = response.lastIndexOf('"');

//   if (first != -1 && last != -1) {
//     // Extract raw string: "24/05/21,12:00:00+00"
//     String t = response.substring(first + 1, last);

//     // Parse the UTC time into a time structure
//     struct tm timeinfo;
//     timeinfo.tm_year = t.substring(0, 2).toInt() + 100; // Years since 1900
//     timeinfo.tm_mon  = t.substring(3, 5).toInt() - 1;   // Months 0-11
//     timeinfo.tm_mday = t.substring(6, 8).toInt();
//     timeinfo.tm_hour = t.substring(9, 11).toInt();
//     timeinfo.tm_min  = t.substring(12, 14).toInt();
//     timeinfo.tm_sec  = t.substring(15, 17).toInt();

//     // Convert to Unix timestamp and ADD 5 HOURS (5 * 3600 seconds)
//     time_t utc_time = mktime(&timeinfo);
//     utc_time += (5 * 3600);

//     // Convert back to human-readable format
//     struct tm *local_tm = gmtime(&utc_time);
//     char buffer[25];
//     strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", local_tm);

//     return String(buffer);
//   }
//   return "1970-01-01T00:00:00"; // Fallback
// }

// // ================= GSM Configuration END =========================

// int batteryPercentage()
// {
//   return 80;
// }

// void setup()
// {
//   if (switch_gsm_wifi)
//   {
//     Serial.begin(115200);
//     Wire.begin();
//     setup_wifi();

//     macAddress = WiFi.macAddress();
//     macAddress.replace(":", "");
//     device_id = macAddress;
//     mqttTopic = "/topic/" + macAddress;

//     // Initialize the IR Blaster
//     ac.begin();

//     // Set the default baseline state for the AC
//     ac.on();
//     ac.setFan(kElectraAcFanAuto);
//     ac.setMode(kElectraAcCool);

//     Serial.print("Device ID: ");
//     Serial.println(device_id);
//     Serial.print("MQTT Topic: ");
//     Serial.println(mqttTopic);

//     configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
//     Serial.print("Syncing time");
//     struct tm timeinfo;
//     while (!getLocalTime(&timeinfo))
//     {
//       Serial.print(".");
//       delay(500);
//     }
//     Serial.println("\nTime synchronized!");

//     if (!hdc.begin(0x40))
//     {
//       Serial.println("Error: HDC1080 sensor not found!");
//     }
//     else
//     {
//       Serial.println("HDC1080 sensor initialized.");
//     }

//     irsend.begin();
//     client.setServer(mqtt_server, mqtt_port);
//     client.setCallback(callback);
//   }
//   else
//   {
//     Serial.begin(115200);
//     pinMode(kMosfetPin, OUTPUT);
//     digitalWrite(kMosfetPin, HIGH); // Ensure MOSFET is off initially

//     // ===== IMPORTANT: Initialize I2C for HDC1080 in GSM mode too! =====
//     Wire.begin();

//     if (!hdc.begin(0x40))
//     {
//       Serial.println("Warning: HDC1080 sensor not found! Check wiring.");
//     }
//     else
//     {
//       Serial.println("HDC1080 sensor initialized.");
//     }

//     Serial.println("GSM Mode Active - Skipping WiFi/MQTT setup.");
//     Serial.println("\n============ QUECTEL NATIVE MQTT TEST ============");

//     // ===== Set GSM globals safely =====
//     macAddress_gsm = getChipMAC();
//     gsmClientId = macAddress_gsm;
//     pubTopic = "/topic/" + macAddress_gsm;
//     subTopic = "/topic/" + macAddress_gsm;

//     Serial.print("GSM Client ID: ");
//     Serial.println(gsmClientId);
//     Serial.print("Pub Topic: ");
//     Serial.println(pubTopic);

//     SerialAT.begin(115200, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
//     delay(3000);

//     // ===== MODEM INIT WITH RESTART SUPPORT =====
//     int connFailCount = 0;
//     const int MAX_CONN_RETRIES = 3;
//     bool mqttConnected = false;

//     while (!mqttConnected && connFailCount < MAX_CONN_RETRIES)
//     {
//       // ----- STEP 1: Basic Modem Check -----
//       Serial.println("\n[STEP 1] Checking Modem...");
//       sendAT("AT", 1000);
//       sendAT("ATE0", 1000);
//       sendAT("AT+CGMI", 2000);

//       // ----- STEP 2: Check SIM -----
//       Serial.println("\n[STEP 2] Checking SIM...");
//       String simResp = sendAT("AT+CPIN?", 3000);
//       if (simResp.indexOf("READY") == -1)
//       {
//         Serial.println("⚠ SIM not ready, waiting 5s...");
//         delay(5000);
//         sendAT("AT+CPIN?", 3000);
//       }

//       // ----- STEP 3: Wait for Network Registration -----
//       Serial.println("\n[STEP 3] Waiting for Network Registration...");
//       bool registered = false;
//       for (int i = 0; i < 30; i++)
//       {
//         String regResp = sendAT("AT+CREG?", 2000);
//         if (regResp.indexOf(",1") != -1 || regResp.indexOf(",5") != -1)
//         {
//           registered = true;
//           Serial.println("✓ Network registered!");
//           break;
//         }
//         Serial.println("  ...still searching...");
//         delay(2000);
//       }

//       if (!registered)
//       {
//         Serial.println("✗ Network registration failed!");
//         connFailCount++;
//         Serial.print("Failure count: ");
//         Serial.print(connFailCount);
//         Serial.print("/");
//         Serial.println(MAX_CONN_RETRIES);

//         if (connFailCount >= MAX_CONN_RETRIES)
//         {
//           Serial.println("\n⚠⚠⚠ MAX RETRIES — Restarting modem via AT+CFUN...");
//           sendAT("AT+CFUN=0", 5000);
//           delay(3000);
//           sendAT("AT+CFUN=1", 5000);
//           delay(10000);
//           connFailCount = 0;
//           Serial.println("Modem restarted. Retrying...\n");
//         }
//         continue;
//       }

//       // ----- STEP 4: Activate PDP Context -----
//       Serial.println("\n[STEP 4] Activating PDP Context...");
//       sendAT("AT+QIACT=1", 10000);
//       delay(2000);
//       sendAT("AT+QIACT?", 3000);

//       // ----- STEP 5: Open MQTT Connection -----
//       Serial.println("\n[STEP 5] Opening MQTT Connection...");
//       sendAT("AT+QMTCLOSE=0", 3000);
//       delay(1000);

//       String cmd = "AT+QMTOPEN=0,\"" + String(mqtt_server) + "\"," + String(mqtt_port);
//       sendAT(cmd, 5000);

//       Serial.println("  Waiting for +QMTOPEN URC...");
//       bool openSuccess = false;
//       uint32_t waitStart = millis();
//       String urcBuffer = "";
//       while (millis() - waitStart < 15000)
//       {
//         while (SerialAT.available())
//         {
//           char c = SerialAT.read();
//           urcBuffer += c;
//           Serial.print(c);
//         }
//         if (urcBuffer.indexOf("+QMTOPEN: 0,0") != -1)
//         {
//           openSuccess = true;
//           Serial.println("\n✓ MQTT TCP connection opened!");
//           break;
//         }
//         if (urcBuffer.indexOf("+QMTOPEN: 0,-1") != -1 ||
//             urcBuffer.indexOf("ERROR") != -1)
//         {
//           Serial.println("\n✗ MQTT TCP connection failed!");
//           break;
//         }
//         delay(100);
//       }

//       if (!openSuccess)
//       {
//         connFailCount++;
//         if (connFailCount >= MAX_CONN_RETRIES)
//         {
//           Serial.println("\n⚠⚠⚠ MAX RETRIES — Restarting modem...");
//           sendAT("AT+CFUN=0", 5000);
//           delay(3000);
//           sendAT("AT+CFUN=1", 5000);
//           delay(10000);
//           connFailCount = 0;
//         }
//         continue;
//       }

//       delay(2000);

//       // ----- STEP 6: MQTT CONNECT -----
//       Serial.println("\n[STEP 6] Logging into MQTT Broker...");
//       cmd = "AT+QMTCONN=0,\"" + gsmClientId + "\"";
//       String connResp = sendAT(cmd, 5000);

//       String connURC = "";
//       waitStart = millis();
//       while (millis() - waitStart < 10000)
//       {
//         while (SerialAT.available())
//         {
//           char c = SerialAT.read();
//           connURC += c;
//           Serial.print(c);
//         }
//         if (connURC.indexOf("+QMTCONN: 0,0,0") != -1)
//         {
//           mqttConnected = true;
//           Serial.println("\n✓ MQTT broker connected!");
//           break;
//         }
//         if (connResp.indexOf("+CME ERROR") != -1 ||
//             connURC.indexOf("+CME ERROR") != -1)
//         {
//           Serial.println("\n✗ +CME ERROR detected!");
//           break;
//         }
//         if (connURC.indexOf("ERROR") != -1 ||
//             connURC.indexOf("+QMTCONN: 0,") != -1)
//         {
//           Serial.println("\n✗ MQTT connect failed!");
//           break;
//         }
//         delay(100);
//       }

//       if (!mqttConnected)
//       {
//         connFailCount++;
//         if (connFailCount >= MAX_CONN_RETRIES)
//         {
//           Serial.println("\n⚠⚠⚠ 3 FAILURES — RESTARTING MODEM ⚠⚠⚠");
//           sendAT("AT+QMTCLOSE=0", 3000);
//           delay(1000);
//           sendAT("AT+CFUN=0", 5000);
//           delay(3000);
//           sendAT("AT+CFUN=1", 5000);
//           delay(10000);
//           connFailCount = 0;
//         }
//         else
//         {
//           delay(3000);
//         }
//       }
//     } // end while

//     if (mqttConnected)
//     {
//       delay(2000);
//       Serial.println("\n[STEP 7] Subscribing to Topic...");
//       String cmd = "AT+QMTSUB=0,1,\"" + subTopic + "\",0";
//       sendAT(cmd, 5000);
//       uint32_t waitStart = millis();
//       while (millis() - waitStart < 5000)
//       {
//         checkIncomingData();
//         delay(100);
//       }
//       Serial.println("\n============= SETUP COMPLETE =============");
//       Serial.println("✓ MQTT connected & subscribed!");
//     }
//     else
//     {
//       Serial.println("\n✗✗✗ FAILED to establish MQTT ✗✗✗");
//     }
//   }
// }

// // ===== Loop =====
// void loop()
// {
//   if (switch_gsm_wifi)
//   {
//     // 1. Check and Reconnect WiFi First
//     if (WiFi.status() != WL_CONNECTED)
//     {
//       Serial.println("\nWiFi connection lost! Reconnecting...");

//       // Explicitly disconnect and restart the connection process
//       WiFi.disconnect();
//       WiFi.begin(ssid, password);

//       int abc=0;
//       // Block until reconnected (matches your setup_wifi logic)
//       while (WiFi.status() != WL_CONNECTED)
//       {
//         delay(500);
//         Serial.print(".");
//         if(abc++ > 20) { // After 10 seconds of trying, restart the connection attempt
//           Serial.println("\nStill not connected, retrying WiFi...");
//           WiFi.disconnect();
//           WiFi.begin(ssid, password);
//           delay(2000);
//         }
//       }
//       Serial.println("\nWiFi Reconnected!");
//       Serial.print("New IP: ");
//       Serial.println(WiFi.localIP());
//     }
//     // ==================== WiFi MODE ====================
//     if (!client.connected())
//     {
//       reconnect();
//     }
//     client.loop();

//     static unsigned long lastMsg = 0;
//     if (millis() - lastMsg > TELEMETRY_INTERVAL)
//     {
//       lastMsg = millis();

//       float temperature = hdc.readTemperature();
//       float humidity = hdc.readHumidity();
//       int rssi = WiFi.RSSI();

//       if (isnan(temperature) || isnan(humidity))
//       {
//         Serial.println("HDC1080 Read Failed!");
//         return;
//       }

//       StaticJsonDocument<512> doc;
//       JsonObject mac = doc.createNestedObject(device_id);
//       mac["timestamp"] = getTimestamp();
//       mac["temperature"] = temperature;
//       mac["humidity"] = humidity;
//       mac["status"] = "Online";
//       mac["device_type"] = "climate_sensor";
//       mac["battery_level"] = batteryPercentage();
//       mac["wifi_signal_strength"] = rssi;

//       char buffer[512];
//       serializeJson(doc, buffer);
//       client.publish(mqttTopic.c_str(), buffer);

//       Serial.println("\nJSON Sent:");
//       serializeJsonPretty(doc, Serial);
//       Serial.println();
//     }
//   }
//   else
//   {
//     // ==================== GSM MODE ====================
//     checkIncomingData();

//     if (millis() - lastTelemetry >= TELEMETRY_INTERVAL)
//     {
//       lastTelemetry = millis();

//       Serial.println("\n[TELEMETRY] Publishing data via GSM...");

//       // ===== FIX 1: Read sensor BEFORE starting QMTPUB =====
//       float temperature = hdc.readTemperature();
//       float humidity = hdc.readHumidity();

//       // ===== FIX 2: Get signal strength BEFORE starting QMTPUB =====
//       lastSignalStrength = getSignalStrength();

//       // ===== FIX 3: Get modem time BEFORE starting QMTPUB =====
//       String modemTime = getModemTime();

//       // ===== FIX 4: Build JSON payload BEFORE starting QMTPUB =====
//       StaticJsonDocument<512> doc;
//       JsonObject data = doc.createNestedObject(gsmClientId);

//       data["timestamp"] = getGSMTime();
//       data["temperature"] = isnan(temperature) ? 0.0 : temperature;
//       data["humidity"] = isnan(humidity) ? 0.0 : humidity;
//       data["status"] = "Online";
//       data["device_type"] = "climate_sensor";
//       data["battery_level"] = batteryPercentage();
//       data["cellular_signal_strength"] = lastSignalStrength;

//       char jsonBuffer[512];
//       serializeJson(doc, jsonBuffer);

//       Serial.print("Payload: ");
//       Serial.println(jsonBuffer);

//       // ===== FIX 5: Now start QMTPUB and wait for '>' prompt =====
//       String cmd = "AT+QMTPUB=0,1,1,0,\"" + pubTopic + "\"";
//       String pubResp = sendAT(cmd, 5000);

//       // Check if we got the '>' prompt
//       if (pubResp.indexOf(">") != -1)
//       {
//         // ===== FIX 6: Send payload via SerialAT (NOT client.publish!) =====
//         SerialAT.print(jsonBuffer);
//         SerialAT.write(0x1A); // Ctrl+Z to end payload

//         Serial.println(">> Payload sent via GSM!");

//         // Wait for +QMTPUB: 0,1,0 confirmation
//         uint32_t start = millis();
//         String pubURC = "";
//         while (millis() - start < 5000)
//         {
//           while (SerialAT.available())
//           {
//             char c = SerialAT.read();
//             pubURC += c;
//             Serial.print(c);
//           }
//           if (pubURC.indexOf("+QMTPUB: 0,1,0") != -1)
//           {
//             Serial.println("\n✓ Publish confirmed by broker!");
//             break;
//           }
//           if (pubURC.indexOf("ERROR") != -1 ||
//               pubURC.indexOf("+QMTPUB: 0,1,1") != -1 ||
//               pubURC.indexOf("+QMTPUB: 0,1,2") != -1)
//           {
//             Serial.println("\n✗ Publish failed!");
//             break;
//           }
//           delay(50);
//         }
//       }
//       else
//       {
//         Serial.println("✗ No '>' prompt received! QMTPUB failed.");
//         Serial.println("  Checking connection status...");
//         sendAT("AT+QMTCONN?", 3000);
//       }

//       Serial.println();
//       serializeJsonPretty(doc, Serial);
//       Serial.println();
//     }
//   }
// }


#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "DHT.h"
#include <IRremoteESP8266.h>
#include <IRsend.h>

// ===== WiFi Credentials =====
const char* ssid = "HUAWEI-G2yg";
const char* password = "4a26hpE9";

// ===== MQTT Broker Settings =====
const char* mqtt_server = "107.172.137.233";
const int mqtt_port = 1883;

// ===== Device / MQTT =====
String macAddress;
String mqttTopic;
String device_id;

// ===== DHT11 Settings =====
#define DHTPIN 17
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);
// ===== delay interval =====
int data_delay_interval=30000;

// ===== IR Blaster Settings =====
#define IR_PIN 16
IRsend irsend(IR_PIN);

WiFiClient espClient;
PubSubClient client(espClient);

// ===== MQTT Callback =====
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.println("\nMessage Received:");

  String jsonStr;
  for (unsigned int i = 0; i < length; i++) {
    jsonStr += (char)payload[i];
  }

  Serial.println("Raw JSON:");
  Serial.println(jsonStr);

  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, jsonStr);

  if (error) {
    Serial.print("JSON Parse Failed: ");
    Serial.println(error.f_str());
    return;
  }

  // ===== IR COMMAND (NIKAI – 24 BIT) =====
  if (doc.containsKey("ir")) {
    const char* powerCode = doc["ir"];
    Serial.print("NIKAI IR Code Received: ");
    Serial.println(powerCode);
    irsend.sendNikai(strtoul(powerCode, NULL, 16), 24);
    delay(120);
    Serial.println("IR Signal Sent!");
  }

  // ===== Device JSON =====
  if (doc.containsKey(device_id)) {
    JsonObject device = doc[device_id];
    Serial.println("Parsed Sensor Data:");
    Serial.print("Temperature: ");
    Serial.println(device["temperature"].as<float>());
    Serial.print("Humidity: ");
    Serial.println(device["humidity"].as<float>());
    Serial.print("Status: ");
    Serial.println(device["status"].as<const char*>());
  }
}

// ===== WiFi Setup =====
void setup_wifi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

// ===== MQTT Reconnect =====
void reconnect() {
  while (!client.connected()) {
    Serial.print("Connecting to MQTT...");
    if (client.connect(device_id.c_str())) {
      Serial.println("connected!");

      client.subscribe(mqttTopic.c_str());
      Serial.print("Subscribed to: ");
      Serial.println(mqttTopic);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      delay(5000); 
    }
  }
}

int batteryPercentage()
{
  digitalWrite(34,LOW);
  return analogRead(34);
}

// ===== Setup =====
void setup() {
  Serial.begin(9600);

  // ===== Get MAC Address =====
  macAddress = WiFi.macAddress();   // e.g. E0:5A:1B:31:53:5C
  macAddress.replace(":", "");      // E05A1B31535C

  device_id = macAddress;
  mqttTopic = "/topic/" + macAddress;

  Serial.print("Device ID: ");
  Serial.println(device_id);
  Serial.print("MQTT Topic: ");
  Serial.println(mqttTopic);

  dht.begin();
  irsend.begin();
  setup_wifi();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

// ===== Main Loop =====
void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  static unsigned long lastMsg = 0;
  if (millis() - lastMsg > data_delay_interval) {
    lastMsg = millis();

    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();
    int rssi1 = WiFi.RSSI();

    if (isnan(temperature) || isnan(humidity)) {
      Serial.println("Failed to read from DHT11!");
      return;
    }

    StaticJsonDocument<512> doc;
    JsonObject mac = doc.createNestedObject(device_id);

    mac["timestamp"] =0;
    mac["temperature"] = temperature;
    mac["humidity"] = humidity;
    mac["status"] = "Online";
    mac["device_type"] = "climate_sensor";
    mac["battery_level"] = batteryPercentage();
    mac["wifi_signal_strength"] = rssi1;

    char buffer[512];
    serializeJson(doc, buffer);
    client.publish(mqttTopic.c_str(), buffer);

    Serial.println("\nJSON Sent:");
    serializeJsonPretty(doc, Serial);
    Serial.println();
  }
}
