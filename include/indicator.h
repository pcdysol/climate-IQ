#pragma once
#include "SharedState.h"

namespace Indicator {
    void init();
    
    // Background LED blinker based on current system state
    void update(SystemState state, bool isRadarActive, bool isHumanPresent);
    
    // Quick interrupt flashes
    void indicateSuccess();
    void indicateError();
    void indicateIRSent();
    void indicateWarning();

    // Polls the button and returns an event if triggered
    ButtonEvent checkButton();
}