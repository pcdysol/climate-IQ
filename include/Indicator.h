#pragma once
#include "SharedState.h"

/**
 * @file indicator.h
 * @brief RGB status LED, presence LED, and the physical config button.
 */
namespace Indicator {
    /// Configure LED + button pins. Called once in setup().
    void init();

    /**
     * @brief Drive the status/presence LEDs from the current system state.
     *
     * Called every main-loop tick. Handles the per-state colour/blink pattern,
     * the presence LED, and the offline-failsafe magenta override.
     * @param state          Current SystemState (sets colour/blink).
     * @param isRadarActive  Radar automation enabled (gates the presence LED).
     * @param isHumanPresent Latest presence reading (lights the presence LED).
     */
    void update(SystemState state, bool isRadarActive, bool isHumanPresent);

    /// Blocking flash: two green blinks (operation succeeded).
    void indicateSuccess();
    /// Blocking flash: three red blinks (operation failed).
    void indicateError();
    /// Brief white flash signalling an IR frame was transmitted.
    void indicateIRSent();
    /// Blocking flash: three yellow blinks (warning).
    void indicateWarning();
    /// Blocking flash: distinctive purple strobe — IR learn failed / remote not recognised.
    void indicateLearnFail();

    /**
     * @brief Blocking LED pinout self-test: light RED, then GREEN, then BLUE alone.
     *
     * Drives each RGB channel by itself for ~1s with a serial note of which GPIO it
     * is driving, so you can confirm the physical colour matches the pin. If the
     * wrong colour lights, swap the offending *_PIN defines in Config.h.
     */
    void selfTest();

    /// @return Debounced button event (short = enter AP, long-hold = exit AP).
    ButtonEvent checkButton();
}
