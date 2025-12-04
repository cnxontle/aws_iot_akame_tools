#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <time.h>

class wifiBootstrap {
public:
    wifiBootstrap();

    bool begin(const String& ssid, const String& password, unsigned long timeoutMs = 15000);
    bool syncTime(unsigned long timeoutMs = 8000);

    int getChannel();              // Devuelve canal actual (o -1 si no hay conexión)
    void disconnect();             // Desconexión completa de radios

    bool isConnected();

private:
    String _ssid;
    String _password;
    int _channel;
};


