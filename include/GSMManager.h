#pragma once
#include <Arduino.h>
#include "SharedState.h"
#include <ArduinoJson.h>

/**
 * @file GSMManager.h
 * @brief GSM/cellular MQTT transport via a Quectel-style modem (AT commands).
 *
 * The fallback transport behind NetworkManager when WiFi mode is off. Drives
 * the modem over a HardwareSerial link with raw AT commands: SIM/network
 * bring-up, PDP context, MQTT open/connect/subscribe (QMTOPEN/QMTCONN/QMTPUB),
 * telemetry, and RTC sync from the cell network. Mirrors WiFiManager's public
 * publish API so NetworkManager can dispatch to either transport.
 *
 * @note Uses GSM_RX_PIN / GSM_TX_PIN (16/17) — the same UART pins as the radar,
 *       so GSM mode and the radar cannot both run (the WiFi-vs-GSM split).
 */
namespace GSMManager {
    /// Bring up the modem, register, open + connect MQTT, subscribe, send boot alert.
    void init();
    /// Service tick: poll incoming data, periodic RTC resync, publish telemetry.
    void loop();
    /// Publish a command acknowledgement {ack, action, detail}.
    void publishACK(const char *action, const char *detail);
    /// Publish an immediate automation event (auto_event code).
    void sendAutomationEvent(String eventCode);
    /// Publish a health/diagnostic alert {event, detail}.
    void publishHealthAlert(const char *event, const char *detail);

    /// Send a raw AT command and return the modem's response (waits up to @p timeoutMs).
    String sendAT(String command, uint32_t timeoutMs);
    /// Drain the modem UART; parse any +QMTRECV MQTT payload into CommandProcessor.
    void checkIncomingData();
    /// @return Signal quality (CSQ value) as a string, or "N/A".
    String getSignalStrength();
    /// @return Modem clock (AT+CCLK) raw string, or "N/A".
    String getModemTime();
    /// @return Local ISO-8601 timestamp derived from the modem clock + GMT offset.
    String getGSMTime();

    /// @return Battery percentage placeholder (currently a fixed value).
    int batteryPercentage();

    /// Read the modem clock and set the ESP32 system time from it.
    void setSystemTimeFromGSM();
}
