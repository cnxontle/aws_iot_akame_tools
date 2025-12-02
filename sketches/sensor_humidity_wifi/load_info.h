#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

class LoadInfo {
public:
    // Datos cargados desde memoria interna
    String thingName;
    String awsIotEndpoint;
    String gatewayTopic;
    String userId;
    String ssid;
    String wifiPassword;

    String caCert;
    String deviceCert;
    String privateKey;

    LoadInfo() {}
    bool begin();               // Inicializa FS y carga todo
    bool loadMetadata();        // Lee metadata.json
    bool loadCertificates();    // Lee PEMs y keys

private:
    String readFile(const char* path);
};
