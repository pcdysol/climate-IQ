#pragma once
#include <Arduino.h>
#include "SharedState.h"
#include <ArduinoJson.h>
#include <AsyncMqttClient.h>

namespace WiFiManager {
    String getTimestamp();
    // void callback(char *topic, byte *payload, unsigned int length);
    // New Async Callbacks
    void onMqttConnect(bool sessionPresent);
    void onMqttDisconnect(AsyncMqttClientDisconnectReason reason);
    void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total);
    void setup_wifi();
    void reconnect();
    String getChipMAC();
    void init();
    void loop();
    void publishACK(const char *action, const char *detail);
    void sendAutomationEvent(String eventCode);
    void publishHealthAlert(const char *event, const char *detail);
}