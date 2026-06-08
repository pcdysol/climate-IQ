#pragma once
#include <Arduino.h>
#include "SharedState.h"
#include <ArduinoJson.h>
#include <AsyncMqttClient.h>

/**
 * @file WiFiManager.h
 * @brief WiFi station + asynchronous MQTT transport (AsyncMqttClient).
 *
 * One of the two transports behind NetworkManager (the other is GSMManager).
 * Owns the WiFi connection, NTP time sync, the MQTT client lifecycle (LWT,
 * subscribe, boot alert), backoff reconnection and periodic telemetry. The
 * global `mqttClient` instance is defined in WiFiManager.cpp and shared with
 * OTAManager (which disconnects it before flashing).
 */
namespace WiFiManager {
    /// @return ISO-8601 local timestamp, or "Syncing..." until NTP has synced.
    String getTimestamp();

    /// MQTT connected callback: subscribe, publish online/boot, confirm pending OTA.
    void onMqttConnect(bool sessionPresent);
    /// MQTT disconnected callback (logging; reconnect is driven by loop()).
    void onMqttDisconnect(AsyncMqttClientDisconnectReason reason);
    /// MQTT message callback: copies + parses the payload into CommandProcessor.
    void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total);

    /// Connect to the stored SSID with a boot timeout (non-fatal: continues offline).
    void setup_wifi();
    /// Attempt one asynchronous MQTT (re)connect if WiFi is up and not already connected.
    void reconnect();
    /// @return The chip's WiFi-STA MAC as a 12-char uppercase hex string (device id).
    String getChipMAC();

    /// Initialise WiFi + MQTT (client id, topics, LWT, callbacks). Called once.
    void init();
    /// Service tick: WiFi/MQTT backoff reconnection + periodic telemetry publish.
    void loop();

    /// Publish a command acknowledgement {ack, action, detail}.
    void publishACK(const char *action, const char *detail);
    /// Publish an immediate automation event (auto_event code, e.g. "1000").
    void sendAutomationEvent(String eventCode);
    /// Publish a health/diagnostic alert {event, detail}.
    void publishHealthAlert(const char *event, const char *detail);
}
