#pragma once

#include <Arduino.h>

// Note: We don't include the heavy IR libraries here in the header to keep 
// compilation fast and prevent naming collisions in other files.

namespace IRManager {
    
    // Setup the hardware (called once in setup)
    void init();

    // Sends the AC command (tries custom first, falls back to Universal)
    // Returns true if a custom button was used, false if universal was used.
    bool sendACCommand(bool turnOn, int targetTemp);

    // Blast a specific stored custom button (returns true if successful)
    bool playCustomButton(const char* storageKey);

    // Send the Universal AC fallback signal
    void sendACFallback(bool turnOn, int targetTemp);

    // Enters blocking listen mode to learn a new code
    // Returns 0 on success, 1 on timeout, 2 on unknown protocol
    int learnCommand(const char* storageKey, bool isProtocol);

    // Add these to handle dynamic MQTT protocol blasts
    bool sendDynamicState(const char* protocolStr, uint8_t* stateArray, uint16_t size);
    bool sendDynamicCode(const char* protocolStr, uint64_t irCode, uint16_t bits);
    // Wipes all learned IR data from the NVS memory
    void wipeMemory();
}