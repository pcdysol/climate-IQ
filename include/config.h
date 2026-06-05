#pragma once

#include <Arduino.h> // Needed for standard types like uint8_t and unsigned long

// ====================================================================
// ==================== 1. HARDWARE CONFIGURATION =====================
// ====================================================================

// --- Core Pins ---
#define BUTTON_PIN      26
#define LED_PIN         2
#define MOSFET_PIN      27

// --- IR Transceiver ---
#define RECV_PIN        19
#define IR_PIN          18

// --- GSM Modem ---
#define GSM_RX_PIN      16
#define GSM_TX_PIN      17

// --- RGB LED (Common Anode) ---
#define RED_PIN         32
#define GREEN_PIN       33
#define BLUE_PIN        4

// ====================================================================
// ===================== 2. NETWORK CONFIGURATION ====================
// ====================================================================

// ===== Web Dashboard Admin Credentials =====
#define WWW_USERNAME    "admin"
#define WWW_PASSWORD    "admin"
#define DEV_PASSWORD    "dev1234"

// ===== SoftAP Setup =====
#define AP_SSID         "SmartAC-Setup"
#define AP_PASSWORD     "password123"

// ===== MQTT Broker =====
#define MQTT_SERVER     "107.172.137.233"
#define MQTT_PORT       1883

// ===== NTP (Internet Time) =====
#define NTP_SERVER      "pool.ntp.org"
#define GMT_OFFSET_SEC  (5 * 3600)
#define DAYLIGHT_OFFSET_SEC 0

// ====================================================================
// ===================== 3.SENSOR & HEALTH TIMINGS ====================
// ====================================================================

// ===== Radar LD2412 Settings =====
#define RADAR_STALE_MS          30000 // 30s without a frame = radar is hung

// ===== HDC1080 Settings =====
#define HDC_REINIT_THRESHOLD    3         // Attempt I2C reset after this many consecutive NaN reads

// ===== Time after it stops retrying for wifi reconnection =====
#define MAX_BACKOFF_MS          300000UL // 5 min cap

// ====================================================================
// ===================== 4. AUTOMATION SETTINGS =======================
// ====================================================================

#define MAX_SEGS_PER_DAY        7   // Maximum number of time segments per day

// Default time segments 
#define DEFAULT_ECO_TIME_MS     120000 // 2 mins (in ms) to trigger Eco Mode
#define DEFAULT_OFF_TIME_MS     300000 // 5 mins (in ms) to turn off
#define DEFAULT_NORMAL_TEMP     24     // Remembers the scheduled/manual temp
#define DEFAULT_ECO_TEMP        26     // Default Eco Temperature

// ====================================================================
// ===================== 5. MQTT TELEMETRY SETTINGS ===================
// ====================================================================
#define data_delay_interval 10000
#define TELEMETRY_INTERVAL  10000
