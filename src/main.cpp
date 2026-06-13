/**
 * @file main.cpp
 * @brief Firmware entry point: global definitions, setup() and loop().
 *
 * setup() initialises hardware and subsystems in a deliberate order (OTA
 * bookkeeping first so a crash-looping image can still be rolled back), loads
 * persisted settings from NVS, creates the shared queue/timers/mutex, and
 * spawns the FreeRTOS tasks that do the real work:
 *   ScheduleTask, AutomationTask, SensorsTask, WebTask, OTATask, NetworkTask.
 * loop() then runs at low priority: it pets the watchdog, drives the status
 * LED, and performs any deferred reboot.
 *
 * This translation unit also DEFINES the global handles/state declared extern
 * in SharedState.h (sysData, automationQueue, the timers, irMutex, etc.).
 */
#include <Arduino.h>
#include "Config.h"
#include "SharedState.h"
#include "Indicator.h"
#include "IRManager.h"
#include "SensorManager.h"
#include "WebDashboard.h"
#include "ScheduleManager.h"
#include "AutomationManager.h"
#include "HealthManager.h"
#include "CommandProcessor.h"
#include "NetworkManager.h"
#include "OTAManager.h"
#include <Preferences.h>
#include "esp_task_wdt.h"
#include "esp_log.h"

static const char *TAG = "MAIN";

// --- Global definitions (declared extern in SharedState.h / used across files) ---
SystemData sysData;          ///< The one shared real-time state instance.
Preferences preferences;     ///< Shared NVS handle (namespace "ir_data").

bool pendingReboot = false;      ///< Set by OTA/web to request a deferred reboot.
unsigned long rebootTime = 0;    ///< millis() at which the deferred reboot fires.
QueueHandle_t automationQueue;   ///< Event queue feeding the Automation task.
TimerHandle_t healthTimer;       ///< 30s health-check timer.
TimerHandle_t enforceTimer;      ///< 3-min AC re-assertion timer.
TimerHandle_t ecoTimer;          ///< One-shot room-empty -> eco timer.
TimerHandle_t offTimer;          ///< One-shot room-empty -> off timer.
TaskHandle_t sensorsTaskHandle = NULL; ///< Sensor task handle (xTaskNotify target).
SemaphoreHandle_t irMutex;       ///< Serialises all IR transmits across tasks.

// ==========================================
// Setup & Initialization
// ==========================================

/**
 * @brief One-time boot: init hardware/subsystems, load NVS, spawn all tasks.
 */
void setup() {
    Serial.begin(115200);

    // OTA trial/rollback bookkeeping. MUST run first — before any subsystem init —
    // so the trial boot counter advances (and a crash-looping new image can be
    // reverted) even if the firmware later crashes during hardware init.
    OTAManager::init();

    // 1. Hardware Initialization
    Indicator::init();
    // Add this inside setup(), BEFORE IRManager::init():
    irMutex = xSemaphoreCreateMutex();
    IRManager::init();
    SensorManager::init(&sysData);
    WebDashboard::init();
    
    // 2. Preferences & State Load
    preferences.begin("ir_data", false);
    sysData.currentEcoTemp = preferences.getInt("eco_temp", 26);
    sysData.TEcoTime = preferences.getULong("eco_time", 120000);
    sysData.TOffTime = preferences.getULong("off_time", 300000);
    sysData.currentNormalTemp = preferences.getInt("normal_temp", 24);
    sysData.radarAutoMode = preferences.getBool("radar_auto", false);
    sysData.radarManualOverride = preferences.getBool("rad_ovr", false);
    sysData.radarManualValue = preferences.getBool("rad_ovr_v", false);
    sysData.lastPresenceTime = millis();
    sysData.switch_gsm_wifi = preferences.getBool("use_wifi", true);
    sysData.flapDelaySec = preferences.getULong("flap_delay", 10);
    sysData.enforcementEnabled = preferences.getBool("enforce_en", true);
    sysData.remoteIrEnabled = preferences.getBool("remote_ir_en", true);
    // Radar detection range is NOT stored on the ESP32 — the radar keeps it in its
    // own flash. The sensor task reads it back for display once streaming is up.
    
    ScheduleManager::refreshHasAnySchedule();

    // 2. Create the Queue (Holds up to 10 events)
    automationQueue = xQueueCreate(10, sizeof(SystemEvent));
    
    healthTimer = xTimerCreate("HealthTmr", pdMS_TO_TICKS(30000), pdTRUE, (void *)0, HealthManager::HealthTimerCallback);
    enforceTimer = xTimerCreate("EnforceTmr", pdMS_TO_TICKS(180000), pdTRUE, (void *)1, AutomationManager::EnforceTimerCallback);
    
    // --- ADD THESE TWO TIMERS ---
    // Note: pdFALSE means they only run exactly once per trigger.
    ecoTimer = xTimerCreate("EcoTmr", pdMS_TO_TICKS(120000), pdFALSE, (void *)0, AutomationManager::EcoTimerCallback);
    offTimer = xTimerCreate("OffTmr", pdMS_TO_TICKS(300000), pdFALSE, (void *)0, AutomationManager::OffTimerCallback);

    xTimerStart(healthTimer, 0);
    xTimerStart(enforceTimer, 0);

    // 4. Create New Tasks
    xTaskCreatePinnedToCore(ScheduleManager::TaskSchedule, "SchedTask", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(AutomationManager::TaskAutomation, "AutoTask", 4096, NULL, 4, NULL, 1);

    // 4. Spin up FreeRTOS Tasks
    // Arguments: Function, Name, Stack Size, Params, Priority, Task Handle, Core
    // NOTE: capture the handle (&sensorsTaskHandle) — health recovery and the web
    // calibrate/reset triggers all xTaskNotify() this task; with NULL it stays unset.
    xTaskCreatePinnedToCore(SensorManager::TaskSensors, "SensorsTask", 4096, NULL, 3, &sensorsTaskHandle, 1);
    xTaskCreatePinnedToCore(WebDashboard::TaskWeb,     "WebTask",     4096, NULL, 1, NULL, 0);

    // OTA worker: idle until an MQTT ota_update command queues a job. 10 KB stack
    // covers the TLS handshake + HTTPUpdate call chain. Low priority so it never
    // starves sensors/automation; it only does real work when a command arrives.
    xTaskCreatePinnedToCore(OTAManager::TaskOTA,       "OTATask",     10240, NULL, 2, NULL, 1);

    // 3. Network Boot
    if (!sysData.isAPMode && sysData.switch_gsm_wifi) {
        String currentSSID = preferences.getString("wifi_ssid", "");
        if (currentSSID.length() > 0) {
            Indicator::indicateSuccess();
        } else {
            Indicator::indicateError();
            WebDashboard::startAPMode();
        }
    }

    if (!sysData.isAPMode) {
        NetworkManager::init(&sysData);
    }

    xTaskCreatePinnedToCore(NetworkManager::TaskNetwork, "NetworkTask", 8192, NULL, 1, NULL, 1); 

    // Enable Watchdog for the main loop
    esp_task_wdt_init(120, true);
    esp_task_wdt_add(NULL);
}

// ==========================================
// Main Application Loop (Core 1, Priority 1)
// ==========================================

/**
 * @brief Low-priority main loop: watchdog reset, status LED, deferred reboot.
 *
 * The functional work lives in the FreeRTOS tasks; this loop only keeps the
 * task watchdog fed, refreshes the indicator LED from shared state, and
 * executes a pending reboot once its scheduled time arrives.
 */
void loop() {
    esp_task_wdt_reset();

    // Handle reboots triggered by OTA or Web Dashboard safely
    if (pendingReboot && millis() > rebootTime) {
        ESP_LOGI(TAG, "Rebooting now...");
        ESP.restart();
    }

    // Update LED states based on shared system state
    Indicator::update(sysData.currentState, sysData.radarAutoMode, sysData.cachedPresence);

    static bool trackerInitialized = false;
    if (!trackerInitialized) {
        sysData.lastStateChangeTime = millis();
        trackerInitialized = true;
    }

    // Yield to FreeRTOS scheduler to prevent Task Watchdog Timeout
    vTaskDelay(pdMS_TO_TICKS(100)); 
}