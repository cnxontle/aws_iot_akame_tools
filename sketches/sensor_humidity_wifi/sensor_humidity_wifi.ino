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
const long WINDOW_PERIOD_SECONDS = 1800; // 30 minutos
const int PRE_WAKE_SECONDS = 5;        // Despertar 5 segundos antes de la ventana

// GLOBALS
LoadInfo info;
wifiBootstrap wifiBootstrap;
MqttClientManager mqttManager; 

std::vector<Reading> readings;

// Funciones addReading, clearBuffer, onEspNowRecv, storeOwnReading, broadcastTimestamp 
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
  StaticJsonDocument<256> msg;
  msg["timestamp"] = time(nullptr); // La hora debe estar sincronizada antes de llamar a esto
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

// La función programa el despertar directamente para el inicio de la ventana.
void goToDeepSleep(time_t nextWindowStart) {
  time_t nowEpoch = time(nullptr);
  long sleepSeconds = 0;
  
  // Si la hora es inválida (primer despertar sin hora válida), duerme corto.
  if (nowEpoch < 1600000000) { 
    Serial.println("Time invalid; sleeping short (60s).");
    nextWindowStartEpoch = 0;
    sleepSeconds = 60;
  } else {
    // Dormir hasta el inicio exacto de la próxima ventana.
    long diff = (nextWindowStart - nowEpoch) - PRE_WAKE_SECONDS;
    if (diff < 1) diff = 1;
    const long maxSleep = 30L * 24 * 3600;
    if (diff > maxSleep) diff = maxSleep;
    sleepSeconds = diff;
  }
  Serial.printf("Deep sleep for %ld seconds, waking at %s", sleepSeconds, asctime(localtime(&nextWindowStart)));
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
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

  // Inicializar ESP-NOW
  WiFi.mode(WIFI_STA); 
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK)
    Serial.println("ESP-NOW init ERROR");
  else
    Serial.println("ESP-NOW ready");
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memset(peerInfo.peer_addr, 0xFF, 6);
  peerInfo.channel  = 1;   // mismo canal donde iniciaste ESP-NOW
  peerInfo.encrypt  = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("Error agregando broadcast peer");
  } else {
      Serial.println("Broadcast peer added");
  }
  esp_now_register_recv_cb(onEspNowRecv);
  Serial.println("Setup done. Waiting for window start...");
}

// LOOP
void loop() {
  time_t nowEpoch = time(nullptr);

  // === LÓGICA DE PRIMER CICLO/SIN HORA ===
  if (nextWindowStartEpoch == 0) {
    
    // Conectar WiFi y sincronizar hora para el primer cálculo de ventana.
    Serial.println("Primer ciclo o sin hora. Conectando WiFi para sincronización...");
    bool wifiOk = wifiBootstrap.begin(info.ssid.c_str(), info.wifiPassword.c_str(), 15000);
    if (!wifiOk) {
      Serial.println("ERROR: WiFi falló en el primer ciclo. Reintentando...");
      goToDeepSleep(nowEpoch + 60);   // Dormir corto si no hay conexión para reintentar.
    }
    wifiBootstrap.syncTime(8000);
    nowEpoch = time(nullptr);
    if (nowEpoch < 1600000000) {       // Si la hora sigue siendo inválida después de la sincronización.
      Serial.println("Hora inválida después de sincronización. Reintentando...");
      goToDeepSleep(nowEpoch + 60);
    }
    // Calcular el inicio de la próxima ventana de 30 minutos.
    nextWindowStartEpoch = (nowEpoch / WINDOW_PERIOD_SECONDS) * WINDOW_PERIOD_SECONDS + WINDOW_PERIOD_SECONDS;
    Serial.printf("Primer ciclo, ventana: %s", asctime(localtime(&nextWindowStartEpoch)));
    goToDeepSleep(nextWindowStartEpoch);
  }

  // === INICIO DE VENTANA DE RECOLECCIÓN (ESP-NOW) ===
  if (abs(nowEpoch - nextWindowStartEpoch) < 5) {     // nowEpoch debe ser aproximadamente igual a nextWindowStartEpoch
    Serial.printf("\n--- INICIO DE VENTANA DE RECOLECCIÓN (%s) ---\n", 
                  asctime(localtime(&nowEpoch)));
    broadcastTimestamp();
    storeOwnReading();

    // 3. Esperar la duración de la ventana para recibir mensajes de los nodos.
    unsigned long start = millis();
    while (millis() - start < windowDurationMs) {
      delay(50);
    }
    Serial.printf("--- FIN DE VENTANA DE RECOLECCIÓN (%s) ---\n", 
                  asctime(localtime(&nowEpoch)));
    esp_now_deinit();
    
    // === PUBLICACIÓN (WIFI/MQTT) ===
    Serial.println("Conectando WiFi/MQTT para publicación...");
    bool wifiOk = wifiBootstrap.begin(info.ssid.c_str(), info.wifiPassword.c_str(), 15000);
    if (wifiOk) {
    wifiBootstrap.syncTime(8000); // Sincronizar hora
    mqttManager.begin(&info);

    if (mqttManager.connect()) {
        Serial.printf("Publicando %d lecturas via MQTT...\n", readings.size());

        if (mqttManager.publishReadings(readings)) {
            unsigned long t0 = millis();
            while (millis() - t0 < 2000) {
                mqttManager.loop();   // permite reintentos internos del cliente MQTT
                delay(10);
            }
            readings.clear();
        }

        mqttManager.disconnect();
    }
}
    wifiBootstrap.disconnect();
    
    // === PROGRAMAR EL SIGUIENTE SUEÑO ===
    nextWindowStartEpoch += WINDOW_PERIOD_SECONDS; 
    while (nextWindowStartEpoch <= time(nullptr))
      nextWindowStartEpoch += WINDOW_PERIOD_SECONDS;
    Serial.printf("Siguiente ventana programada: %s", asctime(localtime(&nextWindowStartEpoch)));
    goToDeepSleep(nextWindowStartEpoch);
  }

  // Esto es un fallback, no debería ejecutarse con la lógica de Deep Sleep
  delay(500); 
}