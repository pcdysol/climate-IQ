#include <Arduino.h>
#include "config.h"
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
#include <Preferences.h>
#include "esp_task_wdt.h"

SystemData sysData;
Preferences preferences;

bool pendingReboot = false;
unsigned long rebootTime = 0;
// 1. Define the globals
QueueHandle_t automationQueue;
TimerHandle_t healthTimer;
TimerHandle_t enforceTimer;
TimerHandle_t ecoTimer;
TimerHandle_t offTimer;
TaskHandle_t sensorsTaskHandle = NULL;
// Add this near the top with your other globals:
SemaphoreHandle_t irMutex;

// ==========================================
// Setup & Initialization
// ==========================================

void setup() {
    Serial.begin(115200);
    
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

void loop() {
    esp_task_wdt_reset();

    // Handle reboots triggered by OTA or Web Dashboard safely
    if (pendingReboot && millis() > rebootTime) {
        Serial.println("Rebooting now...");
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