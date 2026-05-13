#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/timers.h>
#include <atomic>

// 1. Define the types of events that can wake up the Automation Manager
enum EventType {
    EVENT_PRESENCE_CHANGED,
    EVENT_MQTT_COMMAND,
    EVENT_SCHEDULE_TRIGGER,
    EVENT_ECO_TRIGGER, // <--- ADD THIS
    EVENT_OFF_TRIGGER,  // <--- ADD THIS
    EVENT_ENFORCE_TRIGGER // <--- ADD THIS
};

// 2. Define the message payload
struct SystemEvent {
    EventType type;
    int payload; // e.g., Target temperature, or 1/0 for presence
};

// 3. Declare Global RTOS Handles so other files can use them
extern QueueHandle_t automationQueue;
extern TimerHandle_t healthTimer;
extern TimerHandle_t enforceTimer;
extern TimerHandle_t ecoTimer; // <--- ADD THIS
extern TimerHandle_t offTimer; // <--- ADD THIS

// State machine for automation
enum AutoState {
  AUTO_OFF,
  AUTO_ON_NORMAL,
  AUTO_ON_ECO
};

struct ScheduleSegment
{
  uint16_t startMin; // minutes from midnight (0-1439)
  uint16_t endMin;   // minutes from midnight (1-1440, exclusive; 1440 = end of day)
  uint8_t temp;      // target temperature (16-32)
  uint8_t radar;     // 0=unset, 1=enable, 2=disable (per-segment)
  uint8_t eco;       // eco temp override, 0 = inherit global
  uint8_t teco;      // eco time minutes override, 0 = inherit global
  uint8_t toff;      // off time minutes override, 0 = inherit global
}; // 9 bytes per segment

// Represents the visual/network state of the system
enum SystemState {
    SYS_BOOTING,
    SYS_AP_MODE,
    SYS_WIFI_CONN,
    SYS_WIFI_OK,
    SYS_GSM_CONN,
    SYS_GSM_OK,
    SYS_ERROR
};

// Represents button interactions safely
enum ButtonEvent {
    BTN_NONE,
    BTN_SHORT_PRESS,
    BTN_LONG_PRESS
};

// The central data structure holding real-time variables
struct SystemData {
    // --- Network & Mode ---
    SystemState currentState = SYS_BOOTING;
    bool isAPMode = false;
    bool switch_gsm_wifi = true; // true = WiFi, false = GSM

    // --- Sensor Readings ---
    float currentTemp = 0.0;
    float currentHumidity = 0.0;
    bool hdcInitFailed = false;

    // --- Radar State ---
    bool sensorReady = false;
    bool cachedPresence = false;
    bool radarAutoMode = false;
    unsigned long lastRadarDataTime = 0;

    // --- Automation Settings ---
    int currentNormalTemp = 24;
    int currentEcoTemp = 26;
    unsigned long TEcoTime = 120000;
    unsigned long TOffTime = 300000;
    unsigned long lastPresenceTime = 0;
    bool radarManualOverride = false;
    bool radarManualValue = false;
    bool radarInitFailed = false;
    bool lastPresenceState = false;
    unsigned long lastStateChangeTime = 0;
    // --- Automation Settings ---
    unsigned long flapDelaySec = 10;       // Customizable seconds to ignore radar
    unsigned long flapDelayStart = 0;      // Timestamp of when OFF was sent
    bool isFlapDelayActive = false;        // Flag to enable the blind spot
    // --- State Machine & Scheduling ---
    AutoState acAutoState = AUTO_OFF;
    bool isInsideSchedule = false;
    bool hasAnySchedule = false;
    unsigned long lastCommandTime = 0;
    std::atomic<uint32_t> accumulatedPresenceMs{0};
};

// Expose the global state object to any file that includes this header
extern SystemData sysData;
// 3. At the bottom of the file (with the other externs), add:
extern SemaphoreHandle_t irMutex;
extern TaskHandle_t sensorsTaskHandle; // Expose the task handle