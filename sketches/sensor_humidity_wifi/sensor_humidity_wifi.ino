#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>
#include "esp_sleep.h"
#include "load_info.h"
#include "wifi_bootstrap.h"
#include "mqtt_client_manager.h"   

// Persistente Deep Sleep
RTC_DATA_ATTR time_t nextWindowStartEpoch = 0;

// CONFIG
const int nodeId = 1;
const int numNodes = 50;
const unsigned long slotDurationMs = 1000;
const unsigned long windowDurationMs = slotDurationMs * numNodes;
const int timestampRetries = 3;
const int WAKE_AHEAD_SECONDS = 20;

// GLOBALS
LoadInfo info;
wifiBootstrap wifiBootstrap;
MqttClientManager mqttManager;   

std::vector<Reading> readings;

void addReading(int id, float hum, int raw) {
  for (Reading &r : readings) {
    if (r.nodeId == id) {
      r.humidity = hum;
      r.raw = raw;
      Serial.printf("Updated nodeId=%d hum=%.2f raw=%d\n", id, hum, raw);
      return;
    }
  }
  readings.push_back({id, hum, raw});
  Serial.printf("New reading nodeId=%d hum=%.2f raw=%d\n", id, hum, raw);
}

void clearBuffer() {
  readings.clear();
}

void onEspNowRecv(const esp_now_recv_info *infoRecv, const uint8_t *data, int len) {
  StaticJsonDocument<256> msg;
  if (deserializeJson(msg, data, len)) {
    Serial.println("Ignored packet (invalid JSON)");
    return;
  }

  if (!msg.containsKey("userId") ||
      !msg.containsKey("locationId") ||
      !msg.containsKey("nodeId") ||
      !msg.containsKey("humidity") ||
      !msg.containsKey("raw")) {
    Serial.println("Ignored packet (incomplete JSON)");
    return;
  }

  if (String(msg["userId"]) != info.userId ||
      String(msg["locationId"]) != info.thingName) {
    Serial.println("Ignored packet (belongs to different system)");
    return;
  }

  addReading(msg["nodeId"], msg["humidity"], msg["raw"]);
}

void storeOwnReading() {
  const int rawDry = 2259;
  const int rawWet = 939;

  int raw = analogRead(34);
  float hum = (rawDry - raw) * 100.0 / (rawDry - rawWet);
  hum = constrain(hum, 0, 100);

  addReading(nodeId, hum, raw);
}

void broadcastTimestamp() {
  StaticJsonDocument<64> msg;
  msg["timestamp"] = time(nullptr);

  char buffer[64];
  size_t len = serializeJson(msg, buffer);
  uint8_t bcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

  for (int i = 0; i < timestampRetries; i++) {
    esp_err_t r = esp_now_send(bcast, (uint8_t*)buffer, len);
    if (r == ESP_OK)
      Serial.printf("Broadcast timestamp %u (try %d)\n", time(nullptr), i+1);
    delay(1000);
  }
}

void goToDeepSleep(time_t nextWindowStart) {
  time_t nowEpoch = time(nullptr);
  long sleepSeconds = 0;

  if (nowEpoch < 1600000000) {
    Serial.println("Time invalid; sleeping short.");
    nextWindowStartEpoch = 0;
    sleepSeconds = 60;
  } else {
    long diff = (long)(nextWindowStart - WAKE_AHEAD_SECONDS - nowEpoch);
    if (diff < 1) diff = 1;
    const long maxSleep = 30L * 24 * 3600;
    if (diff > maxSleep) diff = maxSleep;
    sleepSeconds = diff;
  }

  Serial.printf("Deep sleep for %ld seconds...\n", sleepSeconds);

  mqttManager.disconnect();
  wifiBootstrap.disconnect();

  esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);
  Serial.flush();
  esp_deep_sleep_start();
}

// SETUP
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.printf("Wakeup cause: %d\n", (int)esp_sleep_get_wakeup_cause());

  if (!info.begin()) {
    Serial.println("ERROR loading metadata");
  }

  Serial.println("=== CONFIG ===");
  Serial.println("thingName: " + info.thingName);
  Serial.println("userId: " + info.userId);
  Serial.println("================");

  // ====== ESP-NOW ======
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK)
    Serial.println("ESP-NOW init ERROR");
  else
    Serial.println("ESP-NOW ready");

  esp_now_register_recv_cb(onEspNowRecv);

  // ====== WiFi ======
  bool wifiOk = wifiBootstrap.begin(info.ssid.c_str(), info.wifiPassword.c_str(), 15000);
  if (wifiOk) {
      int ch = wifiBootstrap.getChannel();
      if (ch > 0) {
          esp_now_peer_info_t peer = {
              .peer_addr = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
              .channel   = ch,
              .encrypt   = false
          };
          esp_now_add_peer(&peer);
          Serial.printf("ESP-NOW peer added on channel %d\n", ch);
      }
  }

  wifiBootstrap.syncTime(8000);

  // ====== MQTT INIT ======
  mqttManager.begin(&info);
  mqttManager.connect();

  Serial.println("Setup done.");
}


// LOOP
void loop() {
  if (!mqttManager.isConnected()) mqttManager.connect();
  mqttManager.loop();

  time_t nowEpoch = time(nullptr);

  if (nextWindowStartEpoch == 0) {
    if (nowEpoch < 1600000000) {
      Serial.println("Hora inválida en primer ciclo. Retry...");
      delay(1000);
      return;
    }

    nextWindowStartEpoch = (nowEpoch / 1800) * 1800 + 1800;

    time_t wake = nextWindowStartEpoch - WAKE_AHEAD_SECONDS;
    if (wake < nowEpoch) wake = nowEpoch + 1;

    Serial.printf("Primer ciclo, ventana: %s", asctime(localtime(&nextWindowStartEpoch)));
    Serial.printf("Wake programado: %s", asctime(localtime(&wake)));

    goToDeepSleep(wake);
  }

  if (nowEpoch >= nextWindowStartEpoch) {
    Serial.printf("\n--- PREPARANDO VENTANA (%s) ---\n", asctime(localtime(&nowEpoch)));

    broadcastTimestamp();
    storeOwnReading();

    while (time(nullptr) < nextWindowStartEpoch) {
      mqttManager.loop();
      delay(50);
    }

    Serial.printf("--- INICIO DE VENTANA (%s) ---\n",
                  asctime(localtime(&nextWindowStartEpoch)));

    unsigned long start = millis();
    while (millis() - start < windowDurationMs) {
      mqttManager.loop();
      delay(50);
    }

    if (mqttManager.publishReadings(readings))
        readings.clear();

    nextWindowStartEpoch += 1800;
    while (nextWindowStartEpoch <= time(nullptr))
      nextWindowStartEpoch += 1800;

    Serial.printf("Siguiente ventana: %s", asctime(localtime(&nextWindowStartEpoch)));

    time_t nextSleep = nextWindowStartEpoch - WAKE_AHEAD_SECONDS;
    goToDeepSleep(nextSleep);
  }

  delay(500);
}