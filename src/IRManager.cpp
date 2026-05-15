#include "IRManager.h"
#include "Config.h" // Needed for IR_PIN, RECV_PIN, MOSFET_PIN
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRutils.h>
#include <IRac.h>
#include <IRrecv.h>
#include <Preferences.h>
#include "Indicator.h"

// --- Private Objects (Hidden from the rest of the system) ---
static IRsend irsend(IR_PIN);
static IRac ac(IR_PIN);
static IRrecv irrecv(RECV_PIN, 2048, 100, true);
extern Preferences preferences; // Forward declaration, defined in main.cpp
// static Preferences preferences;

namespace IRManager {

void init() {
    pinMode(MOSFET_PIN, OUTPUT);
    digitalWrite(MOSFET_PIN, LOW);
    
    // preferences.begin("ir_data", false);
    irsend.begin();
    Serial.println("[IR] Hardware Initialized.");
}

bool playCustomButton(const char* storageKey) {
    if (preferences.getBool((String("has_") + storageKey).c_str(), false)) {
        size_t len = preferences.getBytesLength(storageKey);
        if (len > 0) {
            uint16_t elements = len / sizeof(uint16_t);
            uint16_t rawData[elements];
            preferences.getBytes(storageKey, rawData, len);

            // REPLACE the priority logic with this:
            if (xSemaphoreTake(irMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                // --- FIRST BLAST ---
                irsend.sendRaw(rawData, elements, 38);
                
                // --- NON-BLOCKING DELAY ---
                // Wait 150ms so the AC receiver can distinguish the two signals
                vTaskDelay(pdMS_TO_TICKS(150)); 
                
                // --- SECOND BLAST ---
                irsend.sendRaw(rawData, elements, 38);

                xSemaphoreGive(irMutex);
                Serial.printf("[IR] Sent Custom Signal: %s\n", storageKey);
                return true;
            } else {
                Serial.println("[IR] ERROR: Failed to acquire IR Mutex!");
            }

            // Serial.printf("[IR] Sent Custom Signal: %s\n", storageKey);
            // return true;
        }
    }
    return false;
}

void sendACFallback(bool turnOn, int targetTemp) {
    String savedProto = preferences.getString("protocol_name", "");
    decode_type_t protocol = decode_type_t::UNKNOWN;

    if (savedProto != "") {
        protocol = strToDecodeType(savedProto.c_str());
    }

    if (protocol == decode_type_t::UNKNOWN) {
        Serial.println("[IR] No valid protocol saved. Falling back to ELECTRA_AC.");
        protocol = decode_type_t::ELECTRA_AC; 
    }

    ac.next.protocol = protocol;
    ac.next.power = turnOn;
    ac.next.degrees = targetTemp;
    ac.next.mode = stdAc::opmode_t::kCool;
    ac.next.fanspeed = stdAc::fanspeed_t::kAuto;

    // REPLACE the priority logic with this:
    if (xSemaphoreTake(irMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        // --- FIRST BLAST ---
        ac.sendAc();
        
        // --- NON-BLOCKING DELAY ---
        // Wait 150ms so the AC receiver can reset
        vTaskDelay(pdMS_TO_TICKS(150));
        
        // --- SECOND BLAST ---
        ac.sendAc();

        xSemaphoreGive(irMutex);
    } else {
        Serial.println("[IR] ERROR: Failed to acquire IR Mutex!");
    }
    
    Serial.printf("[IR] Sent Universal Signal (%s): %s at %dC\n", 
                  typeToString(protocol).c_str(), turnOn ? "ON" : "OFF", targetTemp);
}

bool sendACCommand(bool turnOn, int targetTemp) {
    String customKey = turnOn ? ("ir_" + String(targetTemp)) : "ir_off";

    if (!playCustomButton(customKey.c_str())) {
        sendACFallback(turnOn, targetTemp);
        return false; // Used Universal
    }
    return true; // Used Custom
}

int learnCommand(const char* storageKey, bool isProtocol) {
    digitalWrite(MOSFET_PIN, HIGH);
    vTaskDelay(pdMS_TO_TICKS(100));
    irrecv.enableIRIn();

    decode_results results;
    unsigned long startTime = millis();
    bool signalReceived = false;

    Serial.printf("[IR] Listening for %s...\n", storageKey);

    while (millis() - startTime < 10000) {
        if (irrecv.decode(&results)) {
            if (results.rawlen < 30) {
                irrecv.resume();
                continue;
            }
            if (isProtocol && results.decode_type == UNKNOWN) {
                irrecv.resume();
                continue;
            }
            signalReceived = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    irrecv.disableIRIn();
    digitalWrite(MOSFET_PIN, LOW);

    if (!signalReceived) return 1; // Timeout

    if (isProtocol) {
        String protocolName = typeToString(results.decode_type, results.repeat);
        if (protocolName == "UNKNOWN") return 2; // Unknown Protocol
        
        preferences.putString("protocol_name", protocolName);
        Serial.printf("[IR] ✓ PROTOCOL DETECTED: %s\n", protocolName.c_str());
    } else {
        uint16_t* raw_array = resultToRawArray(&results);
        uint16_t raw_length = getCorrectedRawLength(&results);
        size_t arrayBytes = raw_length * sizeof(uint16_t);

        preferences.putBytes(storageKey, raw_array, arrayBytes);
        preferences.putBool((String("has_") + storageKey).c_str(), true);
        delete[] raw_array;
        
        Serial.printf("[IR] ✓ CUSTOM BUTTON [%s] SAVED!\n", storageKey);
    }
    return 0; // Success
}

bool sendDynamicState(const char* protocolStr, uint8_t* stateArray, uint16_t size) {
    decode_type_t irProtocol = strToDecodeType(protocolStr);
    if (irProtocol == decode_type_t::UNKNOWN) return false;
    
    if (xSemaphoreTake(irMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        irsend.send(irProtocol, stateArray, size);
        xSemaphoreGive(irMutex);
        return true;
    }
    return false;
}

bool sendDynamicCode(const char* protocolStr, uint64_t irCode, uint16_t bits) {
    decode_type_t irProtocol = strToDecodeType(protocolStr);
    if (irProtocol == decode_type_t::UNKNOWN) return false;
    
    if (xSemaphoreTake(irMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        irsend.send(irProtocol, irCode, bits);
        xSemaphoreGive(irMutex);
        return true;
    }
    return false;
}

void wipeMemory() {
    // 1. Remove the recognized protocol
    preferences.remove("protocol_name");

    // 2. Remove standard ON/OFF custom buttons
    preferences.remove("ir_on");  preferences.remove("has_ir_on");
    preferences.remove("ir_off"); preferences.remove("has_ir_off");

    // 3. Loop through and remove ALL possible custom temperature buttons (16°C to 32°C)
    for (int i = 16; i <= 32; i++) {
      String key = "ir_" + String(i);
      String hasKey = "has_" + key;
      preferences.remove(key.c_str());
      preferences.remove(hasKey.c_str());
    }

    Indicator::indicateSuccess(); delay(100); Indicator::indicateSuccess();
    Serial.println("[IR] Memory wiped completely.");
}

} // end namespace