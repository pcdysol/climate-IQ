#pragma once

/**
 * @file config.h
 * @brief Compile-time configuration: pins, credentials, timings, OTA settings.
 *
 * Central place for all tunable constants and hardware mapping. Grouped into:
 * 1) hardware pins, 2) network/broker, 3) sensor/health timings,
 * 4) automation defaults, 5) MQTT telemetry, 6) server-based OTA.
 */

#include <Arduino.h> // Needed for standard types like uint8_t and unsigned long

// ====================================================================
// ==================== 1. HARDWARE CONFIGURATION =====================
// ====================================================================

// --- Core Pins ---
#define BUTTON_PIN      13
#define LED_PIN         2
#define MOSFET_PIN      27

// --- IR Transceiver ---
#define RECV_PIN        19
#define IR_PIN          18

// --- GSM Modem ---
#define GSM_RX_PIN      16
#define GSM_TX_PIN      17

// --- Radar LD2412 (UART) ---
#define RADAR_RX_PIN    16
#define RADAR_TX_PIN    17

// --- RGB LED (Common Anode) ---
#define RED_PIN         33
#define GREEN_PIN       32
#define BLUE_PIN        4

// Second RGB LED (Common Anode) for future use (e.g. status vs. alerts). Currently unpopulated on the PCB.
#define RED_PIN1 23
#define GREEN_PIN1 12
#define BLUE_PIN1 15

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
#define MQTT_SERVER     "mqtt.eco-pulse.co"
#define MQTT_PORT       1883

#define MQTT_USERNAME   "ecopulse_device"
#define MQTT_PASSWORD   "Mgj5KbQaYWp9rwz0nGUZZtvTOJLhATN0"

// ===== NTP (Internet Time) =====
// Three servers for resilience: pool.ntp.org is preferred but is frequently slow
// or blocked on captive/filtered networks. Google's and Cloudflare's time servers
// use fixed anycast IPs that resolve where the pool often won't, so the clock
// still syncs (and the schedule can run) on those networks.
#define NTP_SERVER      "pool.ntp.org"
#define NTP_SERVER2     "time.google.com"
#define NTP_SERVER3     "time.cloudflare.com"
#define GMT_OFFSET_SEC  (5 * 3600)
#define DAYLIGHT_OFFSET_SEC 0

// ====================================================================
// ===================== 3.SENSOR & HEALTH TIMINGS ====================
// ====================================================================

// ===== Radar LD2412 Settings =====
#define RADAR_STALE_MS          30000 // 30s without a frame = radar is hung

// Radar Bluetooth control. The radar persists BT state in its OWN flash, so the setting
// survives reboots regardless of this firmware — disabling it once keeps it off even if
// you stop calling the disable code. To get BLE back you must ACTIVELY re-enable it.
//   0 = PRODUCTION: keep BT OFF so no external BLE app (HLK tool) can hijack the radar's
//       config mode and freeze the UART stream (the "frozen radar" hang).
//   1 = BENCH TESTING: actively turn BT ON at boot so the HLK phone app can connect.
//       Set back to 0 and reflash for deployment.
#define RADAR_ENABLE_BT         0

// ===== HDC1080 Settings =====
#define HDC_REINIT_THRESHOLD    3         // Attempt I2C reset after this many consecutive NaN reads

// ===== Max gap between WiFi / MQTT reconnect attempts =====
// The backoff doubles (5s, 10s, 20s, ...) but is capped here. This device is
// mains-powered, not battery, so there is no power reason to back off for
// minutes — keep retrying often so it rejoins WiFi/broker quickly.
#define MAX_BACKOFF_MS          60000UL // 1 min cap

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

// ====================================================================
// ===================== 6. OTA (SERVER-BASED) ========================
// ====================================================================

// Running firmware version. BUMP THIS every release you build & upload
// to the server. The device refuses to install a build whose version
// string equals the one it is already running (unless "force":true).
#define FW_VERSION              "1.0.16"

// Max time (ms) the whole download+flash is allowed to take before abort.
#define OTA_HTTP_TIMEOUT_MS     60000

// --- Trial-boot rollback ---
// After a successful flash the new image boots "on trial". It is committed
// permanently only once health is confirmed (MQTT reconnects). If the new image
// instead crashes and reboots this many times without confirming, the device
// automatically reverts to the previous firmware (the other OTA partition).
#define OTA_MAX_TRIAL_BOOTS     3

// Optional time-based rollback (ms). If a trial image stays up but never confirms
// health within this window, revert anyway. 0 = disabled (recommended): rely on
// the boot-loop counter so a transient broker outage cannot revert a healthy
// device. Set e.g. 300000 (5 min) if you also want "boots-but-never-phones-home"
// builds to auto-revert.
#define OTA_TRIAL_CONFIRM_MS    0

// TLS server-certificate validation. Only used for https:// URLs; the
// deployment server (http://107.172.137.233/firmware/firmware.bin) is plain
// HTTP, so this stays empty.
//  - Leave OTA_ROOT_CA empty ("") to skip cert validation (client.setInsecure()).
//    Integrity is still guaranteed by the ESP32 image SHA256 the ROM bootloader
//    verifies, but it is NOT protected against an active MITM that serves a
//    *validly-signed* malicious binary.
//  - For production over HTTPS, paste your server's root CA PEM here to pin it.
#define OTA_ROOT_CA             ""
