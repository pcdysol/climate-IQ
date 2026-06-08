#pragma once

#include <Arduino.h>

/**
 * @file IRManager.h
 * @brief IR send/receive: AC control, learning, and always-on remote detection.
 *
 * Handles transmitting AC commands (learned custom buttons with a universal
 * protocol fallback), learning new codes from a remote, sending dynamic codes
 * from MQTT, and continuously listening for foreign remote presses so the
 * system can react to manual overrides. All transmits are serialised by irMutex.
 *
 * @note The heavy IRremoteESP8266 headers are deliberately NOT included here —
 *       keeping them in the .cpp speeds compilation and avoids name collisions.
 */
namespace IRManager {

    /**
     * @brief A detected foreign remote press, filled by pollRemoteListener().
     */
    struct RemotePress {
        char proto[16];  ///< Protocol name, e.g. "COOLIX" / "UNKNOWN".
        uint32_t value;  ///< Low 32 bits of the decoded value (display/debounce only).
    };

    /// Initialise IR hardware (sender, receiver, MOSFET power). Called once in setup().
    void init();

    /**
     * @brief Non-blocking poll of the always-on receiver for a genuine USER press.
     *
     * Our own transmissions, repeat frames, noise and learn-mode are all
     * filtered out, so a true result indicates a real manual remote press.
     * @param out Filled with the protocol/value on success.
     * @return true if a genuine foreign frame was detected.
     */
    bool pollRemoteListener(RemotePress &out);

    /**
     * @brief Send an AC command (tries the learned custom button, else universal).
     * @return true if a custom button was used, false if the universal fallback was.
     */
    bool sendACCommand(bool turnOn, int targetTemp);

    /**
     * @brief Blast a specific stored custom button by its NVS key.
     * @param storageKey e.g. "ir_24", "ir_on", "ir_off".
     * @return true if the button existed and was sent.
     */
    bool playCustomButton(const char* storageKey);

    /// Send the universal AC fallback signal for the saved (or default) protocol.
    void sendACFallback(bool turnOn, int targetTemp);

    /**
     * @brief Blocking listen to learn and store a new code or protocol.
     * @param storageKey NVS key to store under.
     * @param isProtocol true = learn just the protocol, false = learn a raw button.
     * @return 0 on success, 1 on timeout, 2 on unknown protocol.
     */
    int learnCommand(const char* storageKey, bool isProtocol);

    /// Send a dynamic AC state array (MQTT-driven). @return true on success.
    bool sendDynamicState(const char* protocolStr, uint8_t* stateArray, uint16_t size);
    /// Send a dynamic numeric IR code (MQTT-driven). @return true on success.
    bool sendDynamicCode(const char* protocolStr, uint64_t irCode, uint16_t bits);

    /// Wipe all learned IR data (protocol + all custom buttons) from NVS.
    void wipeMemory();
}
