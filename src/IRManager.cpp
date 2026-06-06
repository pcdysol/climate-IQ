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

// --- Always-on remote-detection state -------------------------------------
// IR receiver and sender share the MOSFET power switch (pin 27). We keep it HIGH
// permanently now so the receiver can listen continuously (and the sender is
// always powered). learnCommand() temporarily takes exclusive control via the
// learnActive flag so the background listener backs off.
#define IR_TX_COOLDOWN_MS 1500  // ignore decodes for this long after WE transmit
#define IR_REPEAT_MS      750   // collapse held-button repeat frames into one event
#define MIN_PRESS_GAP_MS  1200  // min spacing between two ACCEPTED presses (any value)

static volatile bool     learnActive    = false; // true while learnCommand() owns the receiver
static volatile uint32_t lastIrTxTime   = 0;      // millis() of our most recent transmission
static uint64_t          lastSeenValue  = 0;      // last decoded value (repeat debounce)
static uint32_t          lastSeenTime   = 0;      // millis() of last seen frame
static uint32_t          lastAcceptTime = 0;      // millis() of last ACCEPTED press

// Call right after every transmission so the listener ignores our own blast + echoes.
static inline void markTransmit() { lastIrTxTime = millis(); }

namespace IRManager {

void init() {
    pinMode(MOSFET_PIN, OUTPUT);
    digitalWrite(MOSFET_PIN, HIGH); // power IR receiver + sender continuously

    // preferences.begin("ir_data", false);
    irsend.begin();
    irrecv.enableIRIn(); // start listening for remote presses immediately
    Serial.println("[IR] Hardware Initialized (receiver listening).");
}

bool pollRemoteListener(RemotePress &out) {
    // learnCommand() owns the receiver while a learn session is active.
    if (learnActive) return false;

    decode_results results;
    if (!irrecv.decode(&results)) return false;
    irrecv.resume(); // re-arm for the next frame no matter what

    // Reject noise / partial captures (same threshold learnCommand uses).
    if (results.rawlen < 30) return false;

    // Ignore OUR OWN transmissions and their reflections.
    if (millis() - lastIrTxTime < IR_TX_COOLDOWN_MS) return false;

    // Debounce: a held remote button emits repeat frames — count them as one press.
    uint64_t v = results.value;
    if (v == lastSeenValue && (millis() - lastSeenTime) < IR_REPEAT_MS) {
        lastSeenTime = millis();
        return false;
    }
    lastSeenValue = v;
    lastSeenTime  = millis();

    // Global accept-throttle: bounds the event rate regardless of value. This is
    // what tames UNKNOWN AC frames, whose decoded `value` is a noisy hash that
    // changes every frame and would otherwise slip past the repeat debounce.
    if (millis() - lastAcceptTime < MIN_PRESS_GAP_MS) return false;
    lastAcceptTime = millis();

    String p = typeToString(results.decode_type, results.repeat);
    strncpy(out.proto, p.c_str(), sizeof(out.proto) - 1);
    out.proto[sizeof(out.proto) - 1] = '\0';
    out.value = (uint32_t)(results.value & 0xFFFFFFFFULL);
    return true;
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
                markTransmit(); // gate the listener for the whole send, not just after
                // --- FIRST BLAST ---
                irsend.sendRaw(rawData, elements, 38);
                
                // --- NON-BLOCKING DELAY ---
                // Wait 150ms so the AC receiver can distinguish the two signals
                vTaskDelay(pdMS_TO_TICKS(150)); 
                
                // --- SECOND BLAST ---
                irsend.sendRaw(rawData, elements, 38);

                xSemaphoreGive(irMutex);
                markTransmit(); // tell the listener to ignore this blast + echoes
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
        markTransmit(); // gate the listener for the whole send, not just after
        // --- FIRST BLAST ---
        ac.sendAc();
        
        // --- NON-BLOCKING DELAY ---
        // Wait 150ms so the AC receiver can reset
        vTaskDelay(pdMS_TO_TICKS(150));
        
        // --- SECOND BLAST ---
        ac.sendAc();

        xSemaphoreGive(irMutex);
        markTransmit(); // tell the listener to ignore this blast + echoes
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
    // Take exclusive ownership of the receiver so the background listener
    // (pollRemoteListener) stops touching irrecv while we learn.
    learnActive = true;
    vTaskDelay(pdMS_TO_TICKS(20)); // let an in-flight listener tick finish
    irrecv.enableIRIn();
    irrecv.resume(); // discard anything already buffered

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

    // Hand the receiver back to the always-on listener (do NOT disable it or cut
    // MOSFET power — both must stay live for continuous remote detection).
    irrecv.resume();
    lastSeenValue = 0; // don't let the just-learned code suppress the next real press
    learnActive = false;

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
        markTransmit();
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
        markTransmit();
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