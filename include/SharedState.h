#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/timers.h>
#include <atomic>

/**
 * @file SharedState.h
 * @brief Central shared state, event/enum definitions, and global RTOS handles.
 *
 * Everything the FreeRTOS tasks need to communicate lives here:
 *   - SystemEvent / EventType  — messages passed through automationQueue.
 *   - The global RTOS handles   — queue, timers, IR mutex, task handles.
 *   - SystemData (sysData)      — the one global struct holding all real-time
 *                                 state. Fields touched by more than one task
 *                                 are std::atomic; the rest follow a documented
 *                                 single-writer convention (see per-field notes).
 *
 * @note There is exactly one instance, the global `sysData`, defined in main.cpp.
 */

/**
 * @brief Event kinds that can wake the Automation task via automationQueue.
 */
enum EventType {
    EVENT_PRESENCE_CHANGED,   ///< Radar presence went true/false (payload 1/0).
    EVENT_MQTT_COMMAND,       ///< Reserved — MQTT commands are handled in CommandProcessor.
    EVENT_SCHEDULE_TRIGGER,   ///< Reserved — schedule changes act via ScheduleManager.
    EVENT_ECO_TRIGGER,        ///< Eco countdown elapsed (room empty long enough).
    EVENT_OFF_TRIGGER,        ///< Off countdown elapsed (room empty even longer).
    EVENT_ENFORCE_TRIGGER,    ///< Periodic 3-min re-assertion of the AC state.
    EVENT_MANUAL_OVERRIDE     ///< A foreign IR remote press was detected on the receiver.
};

/// How many recent manual remote presses to keep for the dashboard "Remote Activity" card.
#define REMOTE_LOG_SIZE 5

/**
 * @brief Message payload carried on automationQueue.
 */
struct SystemEvent {
    EventType type;  ///< Which event occurred.
    int payload;     ///< Event-specific data, e.g. target temperature, or 1/0 for presence.
};

// --- Global RTOS handles (defined in main.cpp, declared here for all tasks) ---
extern QueueHandle_t automationQueue; ///< Inbound event queue for the Automation task.
extern TimerHandle_t healthTimer;     ///< 30s periodic health check.
extern TimerHandle_t enforceTimer;    ///< 3-min periodic AC re-assertion.
extern TimerHandle_t ecoTimer;        ///< One-shot: room-empty -> eco setpoint.
extern TimerHandle_t offTimer;        ///< One-shot: room-empty -> AC off.

/**
 * @brief AC automation state machine.
 */
enum AutoState {
  AUTO_OFF,        ///< AC is off.
  AUTO_ON_NORMAL,  ///< AC on at the normal setpoint.
  AUTO_ON_ECO      ///< AC on at the raised eco setpoint (room empty a while).
};

/**
 * @brief One time slice of a daily schedule, packed to 9 bytes for NVS storage.
 *
 * Stored/loaded by ScheduleManager (see saveSchedule/loadSegment). Fields with
 * a 0 "unset" sentinel inherit the corresponding global value at apply time.
 */
struct ScheduleSegment
{
  uint16_t startMin; ///< Minutes from midnight (0-1439).
  uint16_t endMin;   ///< Minutes from midnight (1-1440, exclusive; 1440 = end of day).
  uint8_t temp;      ///< Target temperature (16-32).
  uint8_t radar;     ///< Per-segment radar: 0=unset, 1=enable, 2=disable.
  uint8_t eco;       ///< Eco temp override; 0 = inherit global.
  uint8_t teco;      ///< Eco time (minutes) override; 0 = inherit global.
  uint8_t toff;      ///< Off time (minutes) override; 0 = inherit global.
}; // 9 bytes per segment

/**
 * @brief Visual/network state of the system, used to drive the status LED.
 */
enum SystemState {
    SYS_BOOTING,    ///< Power-on, before any connection attempt.
    SYS_AP_MODE,    ///< SoftAP configuration portal active.
    SYS_WIFI_CONN,  ///< WiFi connecting / reconnecting.
    SYS_WIFI_OK,    ///< WiFi up AND MQTT connected — actually delivering data.
    SYS_MQTT_DOWN,  ///< WiFi associated but MQTT broker session down (NOT sending).
    SYS_GSM_CONN,   ///< GSM modem connecting.
    SYS_GSM_OK,     ///< GSM + MQTT up.
    SYS_ERROR       ///< Unrecoverable/error indication.
};

/**
 * @brief Debounced button interaction reported by Indicator::checkButton().
 */
enum ButtonEvent {
    BTN_NONE,         ///< No event this poll.
    BTN_SHORT_PRESS,  ///< Press < 5s released: enter AP mode.
    BTN_LONG_PRESS    ///< Held >= 5s: exit AP mode.
};

/**
 * @brief The central real-time state object (single global instance: sysData).
 *
 * Concurrency convention: std::atomic fields may be read/written from any task;
 * plain fields are written by a single owning task (noted inline) and only read
 * elsewhere, where torn reads are harmless (diagnostics) or naturally aligned.
 */
struct SystemData {
    // --- Network & Mode ---
    SystemState currentState = SYS_BOOTING; ///< Current connection/visual state.
    bool isAPMode = false;                  ///< True while the SoftAP config portal is up.
    bool switch_gsm_wifi = true;            ///< Transport select: true = WiFi, false = GSM.

    // --- Sensor Readings ---
    float currentTemp = 0.0;       ///< Last HDC1080 temperature (°C); NaN on fault.
    float currentHumidity = 0.0;   ///< Last HDC1080 relative humidity (%); NaN on fault.
    bool hdcInitFailed = false;    ///< HDC1080 failed to initialise at boot.

    // --- Radar State ---
    bool sensorReady = false;                    ///< Radar link established and streaming.
    std::atomic<bool> cachedPresence{false};     ///< Latest debounced presence reading.
    std::atomic<bool> radarAutoMode{false};      ///< Radar-driven automation enabled.
    unsigned long lastRadarDataTime = 0;         ///< millis() of last radar frame (staleness check).
    float radarDistance = 0.0;                   ///< Live target distance (cm).

    // --- Radar Live Engineering Data (per-gate energy, LD2412 = 14 gates) ---
    // Written by the sensor task in poll(), read by the web task for the dev feed.
    // Torn reads are harmless here (diagnostic only, values 0-100).
    uint8_t radarMovingEnergy[14] = {0};  ///< Per-gate moving-target energy.
    uint8_t radarStaticEnergy[14] = {0};  ///< Per-gate static-target energy.
    uint8_t radarGateCount = 0;           ///< Number of valid gates in the arrays above.

    // --- Web-triggered radar maintenance (executed on the sensor task) ---
    std::atomic<int> radarCalStatus{0};   ///< 0 idle, 1 in progress, 2 success, 3 failed.

    // --- Radar detection-range control (executed on the sensor task) ---
    // Web writes radarDesiredCm + notifies the sensor task (bit 3); the sensor task
    // snaps it to the nearest gate, applies setMaxGate(), and writes the achieved
    // boundary back to radarRangeCm for the dashboard to display.
    std::atomic<int> radarDesiredCm{0}; ///< Requested boundary in cm (web -> sensor task).
    std::atomic<int> radarRangeCm{0};   ///< Last range read back from the radar (display).

    // --- Manual remote-press detection log (IR receiver is always listening) ---
    // Written by the sensor task when a foreign IR frame is detected, read by the
    // web task for the "Remote Activity" dashboard card. Torn reads are harmless
    // here (diagnostic only). atMillis == 0 marks an empty slot.
    struct RemotePressEntry {
        uint32_t atMillis = 0;  ///< millis() when detected; 0 = empty slot.
        char text[24] = {0};    ///< Human-readable, e.g. "COOLIX 0xB2BF40".
    } remoteLog[REMOTE_LOG_SIZE];
    uint8_t remoteLogHead = 0;                    ///< Next slot to write (ring buffer).
    std::atomic<uint32_t> remoteOverrideCount{0}; ///< Total presses since boot.

    // --- Automation Settings ---
    int currentNormalTemp = 24;            ///< Active "on" setpoint (°C).
    int currentEcoTemp = 26;               ///< Active eco setpoint (°C).
    unsigned long TEcoTime = 120000;       ///< Room-empty delay before eco (ms).
    unsigned long TOffTime = 300000;       ///< Room-empty delay before off (ms).
    unsigned long lastPresenceTime = 0;    ///< millis() of last detected presence.
    bool radarManualOverride = false;      ///< User MQTT command pinned radar on/off.
    bool radarManualValue = false;         ///< The pinned radar value (with the flag above).
    bool radarInitFailed = false;          ///< Radar failed to initialise at boot.
    bool lastPresenceState = false;        ///< Prev presence (sensor task only; presence-time accounting).
    unsigned long lastStateChangeTime = 0; ///< Timestamp anchor for presence-time accounting.
    bool isOfflineFailsafeActive = false;  ///< Offline failsafe engaged (magenta LED).
    unsigned long flapDelaySec = 10;       ///< Seconds to ignore radar after an OFF.
    unsigned long flapDelayStart = 0;      ///< millis() when the flap delay began.
    bool isFlapDelayActive = false;        ///< Flap-delay blind spot currently active.

    // --- State Machine & Scheduling ---
    std::atomic<AutoState> acAutoState{AUTO_OFF};      ///< Current AC state machine state.
    std::atomic<bool> isInsideSchedule{false};         ///< Now within a configured segment.
    std::atomic<bool> scheduleBootDone{false};         ///< Schedule has made its first authoritative AC decision this boot. Until then radar may NOT drive the AC ("schedule is king"); the offline failsafe bypasses this.
    bool hasAnySchedule = false;                       ///< Any day has >=1 segment configured.
    unsigned long lastCommandTime = 0;                 ///< millis() of the last AC command sent.
    std::atomic<uint32_t> accumulatedPresenceMs{0};    ///< Occupied-time accumulator (harvested by telemetry).
};

/// The single global state instance (defined in main.cpp).
extern SystemData sysData;
/// IR transmit mutex — serialises all sends across tasks (defined in main.cpp).
extern SemaphoreHandle_t irMutex;
/// Sensor task handle — target of xTaskNotify() for health/cal/reset/range triggers.
extern TaskHandle_t sensorsTaskHandle;
