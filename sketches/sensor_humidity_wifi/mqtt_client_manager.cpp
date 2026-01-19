#include "mqtt_client_manager.h"

MqttClientManager::MqttClientManager()
    : wifiClient(), mqttClient(wifiClient), info(nullptr) {}

void MqttClientManager::begin(LoadInfo *infoPtr) {
    info = infoPtr;
    mqttClient.setServer(info->awsIotEndpoint.c_str(), 8883);
}

void MqttClientManager::loop() {
    if (mqttClient.connected())
        mqttClient.loop();
}

bool MqttClientManager::connect() {
    if (!info) return false;

    if (mqttClient.connected()) return true;

    wifiClient.setCACert(info->caCert.c_str());
    wifiClient.setCertificate(info->deviceCert.c_str());
    wifiClient.setPrivateKey(info->privateKey.c_str());

    Serial.println("Connecting to AWS IoT MQTT...");
    bool ok = mqttClient.connect(info->thingName.c_str());

    if (ok)
        Serial.println("MQTT connected");
    else
        Serial.printf("MQTT connect error: %d\n", mqttClient.state());

    return ok;
}

bool MqttClientManager::isConnected() {
    return mqttClient.connected();
}

void MqttClientManager::disconnect() {
    if (mqttClient.connected())
        mqttClient.disconnect();
}

bool MqttClientManager::publishReadings(const std::vector<Reading> &readings) {
    if (!mqttClient.connected()) {
        Serial.println("MQTT not connected — skipping publish");
        return false;
    }

    DynamicJsonDocument doc(8192);
    //doc["userId"] = info->userId;
    doc["meshId"] = info->thingName;
    doc["timestamp"] = time(nullptr);

    JsonArray arr = doc.createNestedArray("readings");

    for (const Reading &r : readings) {
        JsonObject o = arr.createNestedObject();
        o["nodeId"] = r.nodeId;
        o["humidity"] = r.humidity;
        o["raw"] = r.raw;
    }

    String payload;
    serializeJson(doc, payload);

    Serial.print("Publishing: ");
    Serial.println(payload);
     Serial.printf("Payload length = %d bytes\n", payload.length());

    return mqttClient.publish(
        info->gatewayTopic.c_str(),
        (uint8_t*)payload.c_str(),
        payload.length()
    );
}
