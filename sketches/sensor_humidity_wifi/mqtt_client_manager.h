#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <vector>
#include "load_info.h"

struct Reading {
    int nodeId;
    float humidity;
    int raw;
};

class MqttClientManager {
public:
    MqttClientManager();

    void begin(LoadInfo *info);
    void loop();
    bool connect();
    bool isConnected();
    void disconnect();
    bool publishReadings(const std::vector<Reading> &readings);

private:
    LoadInfo *info;
    WiFiClientSecure wifiClient;
    PubSubClient mqttClient;
};
