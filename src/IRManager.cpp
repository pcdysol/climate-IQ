/**
 * @file IRManager.cpp
 * @brief Implementation of IR send/receive, AC control, learning, and detection.
 *
 * The sender (IRsend/IRac) and receiver (IRrecv) share the MOSFET power switch,
 * which is held HIGH permanently so the receiver can listen continuously. All
 * transmits take irMutex and call markTransmit() so the always-on listener
 * ignores our own blasts and their echoes; held-button repeats and noise are
 * filtered by the debounce constants below. See IRManager.h for the API.
 */
#include "IRManager.h"
#include "Config.h" // Needed for IR_PIN, RECV_PIN, MOSFET_PIN
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRutils.h>
#include <IRac.h>
#include <IRrecv.h>
#include <Preferences.h>
#include "Indicator.h"
#include "esp_log.h"

static const char *TAG = "IR";

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

/// Initialise IR sender + receiver and power them continuously (MOSFET HIGH).
void init() {
    pinMode(MOSFET_PIN, OUTPUT);
    digitalWrite(MOSFET_PIN, HIGH); // power IR receiver + sender continuously

    // preferences.begin("ir_data", false);
    irsend.begin();
    irrecv.enableIRIn(); // start listening for remote presses immediately
    ESP_LOGI(TAG, "Hardware Initialized (receiver listening).");
}

/**
 * @brief Poll the receiver for a genuine foreign remote press (non-blocking).
 *
 * Filters out everything that is not a real user press: noise/partial captures,
 * our own transmissions (TX cooldown), held-button repeat frames, and a global
 * accept-throttle that tames UNKNOWN AC frames whose decoded value changes every
 * frame. @param out Filled with protocol/value on success. @return true on a real press.
 */
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

/**
 * @brief Replay a learned raw IR button stored under @p storageKey.
 *
 * Sends the stored waveform twice (double-blast, 150ms apart) so the AC reliably
 * registers it, under irMutex. @return true if the button existed and was sent.
 */
bool playCustomButton(const char* storageKey) {
    if (preferences.getBool((String("has_") + storageKey).c_str(), false)) {
        size_t len = preferences.getBytesLength(storageKey);
        if (len > 0) {
            uint16_t elements = len / sizeof(uint16_t);
            uint16_t rawData[elements];
            preferences.getBytes(storageKey, rawData, len);

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
                ESP_LOGI(TAG, "Sent Custom Signal: %s", storageKey);
                return true;
            } else {
                ESP_LOGE(TAG, "Failed to acquire IR Mutex!");
            }
        }
    }
    return false;
}

/**
 * @brief Send a generic AC command via the universal protocol fallback.
 *
 * Used when no learned button matches. Builds the frame from the saved protocol
 * (or ELECTRA_AC default) at the requested power/temperature and double-blasts
 * it under irMutex.
 */
void sendACFallback(bool turnOn, int targetTemp) {
    String savedProto = preferences.getString("protocol_name", "");
    decode_type_t protocol = decode_type_t::UNKNOWN;

    if (savedProto != "") {
        protocol = strToDecodeType(savedProto.c_str());
    }

    if (protocol == decode_type_t::UNKNOWN) {
        ESP_LOGW(TAG, "No valid protocol saved. Falling back to ELECTRA_AC.");
        protocol = decode_type_t::ELECTRA_AC;
    }

    ac.next.protocol = protocol;
    ac.next.power = turnOn;
    ac.next.degrees = targetTemp;
    ac.next.mode = stdAc::opmode_t::kCool;
    ac.next.fanspeed = stdAc::fanspeed_t::kAuto;

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
        ESP_LOGE(TAG, "Failed to acquire IR Mutex!");
    }

    ESP_LOGI(TAG, "Sent Universal Signal (%s): %s at %dC",
             typeToString(protocol).c_str(), turnOn ? "ON" : "OFF", targetTemp);
}

/// Send an AC command: learned button first, universal fallback otherwise.
/// @return true if a custom button was used, false if the universal fallback was.
bool sendACCommand(bool turnOn, int targetTemp) {
    String customKey = turnOn ? ("ir_" + String(targetTemp)) : "ir_off";

    if (!playCustomButton(customKey.c_str())) {
        sendACFallback(turnOn, targetTemp);
        return false; // Used Universal
    }
    return true; // Used Custom
}

/**
 * @brief Blocking listen (up to 10s) to learn a remote code or protocol.
 *
 * Takes exclusive ownership of the receiver (the background listener backs off),
 * waits for a valid frame, then stores either the protocol name or the raw
 * waveform under @p storageKey.
 * @param isProtocol true = learn only the protocol, false = learn a raw button.
 * @return 0 success, 1 timeout, 2 unknown protocol.
 */
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

    ESP_LOGI(TAG, "Listening for %s...", storageKey);

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
        ESP_LOGI(TAG, "PROTOCOL DETECTED: %s", protocolName.c_str());
    } else {
        uint16_t* raw_array = resultToRawArray(&results);
        uint16_t raw_length = getCorrectedRawLength(&results);
        size_t arrayBytes = raw_length * sizeof(uint16_t);

        preferences.putBytes(storageKey, raw_array, arrayBytes);
        preferences.putBool((String("has_") + storageKey).c_str(), true);
        delete[] raw_array;
        
        ESP_LOGI(TAG, "CUSTOM BUTTON [%s] SAVED!", storageKey);
    }
    return 0; // Success
}

/// Send an MQTT-supplied AC state array for the named protocol. @return true on success.
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

/// Send an MQTT-supplied numeric IR code for the named protocol. @return true on success.
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

/// Erase all learned IR data: protocol, ON/OFF, and every 16-32°C custom button.
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
    ESP_LOGI(TAG, "Memory wiped completely.");
}

} // end namespace