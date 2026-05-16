#include "Indicator.h"
#include "Config.h"

namespace Indicator
{
    static void setColor(bool r, bool g, bool b)
    {
        // Common Anode: LOW = ON, HIGH = OFF
        digitalWrite(RED_PIN, r ? LOW : HIGH);
        digitalWrite(GREEN_PIN, g ? LOW : HIGH);
        digitalWrite(BLUE_PIN, b ? LOW : HIGH);
    }

    static void ledOff() { setColor(false, false, false); }

    void init()
    {
        pinMode(BUTTON_PIN, INPUT_PULLUP);
        // Attach the hardware interrupt to trigger when the pin goes LOW (pressed)
        // attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), isrHandleButton, FALLING);
        pinMode(LED_PIN, OUTPUT);
        pinMode(RED_PIN, OUTPUT);
        pinMode(GREEN_PIN, OUTPUT);
        pinMode(BLUE_PIN, OUTPUT);

        // digitalWrite(LED_PIN, LOW);
        ledOff();
    }

    void indicateSuccess()
    {
        ledOff();
        for (int i = 0; i < 2; i++)
        {
            setColor(false, true, false);
            delay(150);
            ledOff();
            delay(150);
        }
    }

    void indicateError()
    {
        ledOff();
        for (int i = 0; i < 3; i++)
        {
            setColor(true, false, false);
            delay(150);
            ledOff();
            delay(150);
        }
    }

    void indicateIRSent()
    {
        setColor(true, true, true);
        delay(100);
        ledOff();
    }

    void update(SystemState state, bool isRadarActive, bool isHumanPresent)
    {
        // Physical hardware LED 2 for human presence indication
        if (isRadarActive && isHumanPresent)
        {
            digitalWrite(LED_PIN, HIGH);
        }
        else
        {
            digitalWrite(LED_PIN, LOW);
        }
        // ---> ADD PRIORITY OVERRIDE HERE <---
        if (sysData.isOfflineFailsafeActive) {
            setColor(1, 0, 1); // Solid Magenta (Red + Blue)
            return; // Exit early to guarantee nothing overlaps this!
        }

        static unsigned long lastBlink = 0;
        static bool ledState = false;
        unsigned long now = millis();

        int interval = 500;
        bool r = 0, g = 0, b = 0;

        switch (state)
        {
        case SYS_BOOTING:
            r = 1;
            g = 1;
            b = 1;
            interval = 0;
            break;
        case SYS_AP_MODE:
            r = 0;
            g = 1;
            b = 1;
            interval = 500;
            break;
        case SYS_WIFI_CONN:
            r = 0;
            g = 0;
            b = 1;
            interval = 500;
            break;
        case SYS_WIFI_OK:
            r = 0;
            g = 1;
            b = 0;
            interval = 5000;
            break;
        case SYS_GSM_CONN:
            r = 1;
            g = 0;
            b = 1;
            interval = 500;
            break;
        case SYS_GSM_OK:
            r = 1;
            g = 0;
            b = 1;
            interval = 5000;
            break;
        case SYS_ERROR:
            r = 1;
            g = 0;
            b = 0;
            interval = 200;
            break;
        }

        if (interval == 0)
        {
            setColor(r, g, b);
        }
        else if (interval == 5000)
        { // Heartbeat
            if (now - lastBlink > 5000)
                lastBlink = now;
            if (now - lastBlink < 50)
                setColor(r, g, b);
            else
                ledOff();
        }
        else
        { // Standard Blink
            if (now - lastBlink > interval)
            {
                lastBlink = now;
                ledState = !ledState;
            }
            if (ledState)
                setColor(r, g, b);
            else
                ledOff();
        }
    }

    ButtonEvent checkButton()
    {
        static unsigned long pressedTime = 0;
        static bool isPressed = false;
        static bool longPressTriggered = false;

        // digitalRead is LOW when the button is pressed (INPUT_PULLUP)
        bool currentState = (digitalRead(BUTTON_PIN) == LOW); 

        if (currentState && !isPressed) {
            // State 1: Button was JUST pressed down
            isPressed = true;
            pressedTime = millis();
            longPressTriggered = false;
        } 
        else if (currentState && isPressed) {
            // State 2: Button is BEING HELD down
            if (!longPressTriggered && (millis() - pressedTime >= 5000)) {
                longPressTriggered = true;
                return BTN_LONG_PRESS;
            }
        } 
        else if (!currentState && isPressed) {
            // State 3: Button was JUST released
            isPressed = false;
            unsigned long duration = millis() - pressedTime;
            
            // If it was held for more than 50ms (debounce) but didn't trigger a long press
            if (!longPressTriggered && duration > 50) { 
                return BTN_SHORT_PRESS;
            }
        }

        return BTN_NONE;
    }

    void indicateWarning()
    {
        ledOff();
        for (int i = 0; i < 3; i++)
        {
            setColor(1, 1, 0); // Yellow (Red + Green)
            delay(150);
            ledOff();
            delay(150);
        }
    }
} // end namespace