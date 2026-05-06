#include "Indicator.h"
#include "Config.h"

namespace Indicator
{

    // -- -INTERRUPT VARIABLES-- -
    // Volatile tells the CPU "this can change at any time from hardware"
    volatile bool buttonPressed = false;
    volatile unsigned long lastInterruptTime = 0;
    // Change 200 to 50 for a more responsive button
    const unsigned long DEBOUNCE_DELAY = 50;

    void IRAM_ATTR isrHandleButton()
    {
        unsigned long interruptTime = millis();
        // Only process if enough time has passed since the last VALID trigger
        if (interruptTime - lastInterruptTime > DEBOUNCE_DELAY)
        {
            buttonPressed = true;
            // Move this inside the block!
            lastInterruptTime = interruptTime;
        }
    }

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
        attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), isrHandleButton, FALLING);
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
        // We no longer need pressStartTime!
        static bool trackingPress = false;
        static bool longPressTriggered = false;

        // 1. Interrupt fired: start tracking
        if (buttonPressed)
        {
            buttonPressed = false;
            // Add this guard to ignore release bounces!
            if (!trackingPress)
            {
                trackingPress = true;
                longPressTriggered = false;
            }
        }

        if (trackingPress)
        {
            bool currentState = digitalRead(BUTTON_PIN);

            // CRITICAL FIX: Calculate duration using the exact
            // timestamp recorded by the hardware interrupt!
            unsigned long duration = millis() - lastInterruptTime;

            // 2. Check for Long Press while button is still held
            if (currentState == LOW)
            {
                if (!longPressTriggered && duration >= 5000)
                {
                    longPressTriggered = true;
                    return BTN_LONG_PRESS;
                }
            }
            // 3. Button released: check for Short Press
            else
            {
                trackingPress = false;

                // Because we use the hardware timestamp, even if a GSM
                // delay blocked this loop for 5 seconds, the duration
                // will correctly read 5000+ ms and trigger the press!
                if (!longPressTriggered && duration > 50)
                {
                    return BTN_SHORT_PRESS;
                }
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