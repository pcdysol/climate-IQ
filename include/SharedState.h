#pragma once
#include <Arduino.h>

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
    unsigned long accumulatedPresenceMs = 0;
    unsigned long lastStateChangeTime = 0;
    // --- State Machine & Scheduling ---
    AutoState acAutoState = AUTO_OFF;
    bool isInsideSchedule = false;
    bool hasAnySchedule = false;
    unsigned long lastCommandTime = 0;
};

// Expose the global state object to any file that includes this header
extern SystemData sysData;