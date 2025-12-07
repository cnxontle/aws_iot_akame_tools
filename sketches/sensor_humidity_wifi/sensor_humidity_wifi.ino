#include <WiFi.h>
#include <ArduinoJson.h>
#include <vector>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>
#include "esp_sleep.h"
#include "load_info.h"
#include "wifi_bootstrap.h"
#include "mqtt_client_manager.h" 

// ESTADO PERSISTENTE
RTC_DATA_ATTR time_t nextWindowStartEpoch = 0;

// CONFIGURACIÓN
const int nodeId = 1;
const int numNodes = 50;
const unsigned long slotDurationMs = 1000;
const unsigned long windowDurationMs = slotDurationMs * numNodes + 2000; // añadir margen de 2 segundos
const int timestampRetries = 3;
const long WINDOW_PERIOD_SECONDS = 300; // 5 minutos
const int PRE_WAKE_SECONDS = 5;        // Despertar 5 segundos antes de la ventana
const int rawdry = 2509;
const int rawwet = 939;

// VARIABLES GLOBALES
LoadInfo info;
wifiBootstrap wifiBootstrap;
MqttClientManager mqttManager; 

std::vector<Reading> readings;

// ESTRUCTURA Y COLA PARA COMUNICACIÓN ISR
struct RawPkt {
  uint8_t data[128];
  int len;
};
QueueHandle_t espNowQueue = NULL;   

// MANEJO DE LECTURAS
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

// ISR: SOLO COPIAR Y ENCOLAR (ISR-safe)
void IRAM_ATTR onEspNowRecv(const esp_now_recv_info *infoRecv, const uint8_t *data, int len) {
  RawPkt pkt;
  if (len > (int)sizeof(pkt.data)) len = sizeof(pkt.data);
  memcpy(pkt.data, data, len);
  pkt.len = len;
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  BaseType_t sent = xQueueSendFromISR(espNowQueue, &pkt, &xHigherPriorityTaskWoken);
  if (sent != pdTRUE) {
    // posible log de error (cola llena)
  }
  if (xHigherPriorityTaskWoken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

// Leer el propio sensor
void storeOwnReading() {
    int readings[5];
    for (int i = 0; i < 5; i++) {
        readings[i] = analogRead(34);
        delay(5);  
    }
    std::sort(readings, readings + 5);
    int raw = (readings[1] + readings[2] + readings[3]) / 3; 
    float denominator = rawdry - rawwet;
    float hum;
    if (denominator == 0) {
        hum = 0;
    } else {
        hum = (rawdry - raw) * 100.0 / denominator;
        hum = constrain(hum, 0, 100);
    }
    addReading(nodeId, hum, raw);
}


//  Broadcast timestamp seguro
void broadcastTimestamp() {
  StaticJsonDocument<256> msg;
  msg["timestamp"] = (uint64_t)time(nullptr);
  char buffer[64];
  size_t len = serializeJson(msg, buffer, sizeof(buffer));
  if (len == 0 || len >= sizeof(buffer)) {
    Serial.println("Error serializing timestamp");
    return;
  }
  uint8_t bcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

  for (int i = 0; i < timestampRetries; i++) {
    esp_err_t r = esp_now_send(bcast, (uint8_t*)buffer, len);
    if (r == ESP_OK) {
      Serial.printf("Broadcast timestamp %u (try %d)\n", (unsigned)time(nullptr), i+1);
    } else {
      Serial.printf("esp_now_send ERR %d (try %d)\n", r, i+1);
    }
    delay(1000);
  }
}

// Programar deep sleep
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
  // Desregistrar callback para evitar que se llame durante teardown
  esp_now_register_recv_cb(NULL);
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

  // Reservar espacio para evitar reallocs frecuentes
  readings.reserve(numNodes);

  // Crear la cola ANTES de registrar la ISR
  espNowQueue = xQueueCreate(64, sizeof(RawPkt)); 
  if (!espNowQueue) { 
    Serial.println("ERROR: Queue not created"); 
  } 

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
  peerInfo.channel  = 1;  
  peerInfo.encrypt  = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("Error agregando broadcast peer");
  } else {
      Serial.println("Broadcast peer added");
  }

  // registrar la ISR (ahora encola RAW pkt)
  esp_now_register_recv_cb(onEspNowRecv); 
  Serial.println("Setup done. Waiting for window start...");
}

//  Procesar paquetes (fuera de ISR) 
void processIncomingPackets() { 
  RawPkt pkt; 
  while (xQueueReceive(espNowQueue, &pkt, 0) == pdTRUE) {
    StaticJsonDocument<384> doc;
    DeserializationError err = deserializeJson(doc, pkt.data, pkt.len);
    if (err != DeserializationError::Ok) {
      continue;
    }
    if (!doc.containsKey("nodeId") || !doc.containsKey("humidity") || !doc.containsKey("raw")) {
      continue;
    }
    int nid = doc["nodeId"];
    float hum = doc["humidity"];
    int raw = doc["raw"];
    addReading(nid, hum, raw);
  } 
} 

// LOOP
void loop() {
  time_t nowEpoch = time(nullptr);

  // LÓGICA DE PRIMER CICLO/SIN HORA
  if (nextWindowStartEpoch == 0) {
    Serial.println("Primer ciclo o sin hora. Conectando WiFi para sincronización...");
    bool wifiOk = wifiBootstrap.begin(info.ssid.c_str(), info.wifiPassword.c_str(), 15000);
    if (!wifiOk) {
      Serial.println("ERROR: WiFi falló en el primer ciclo. Reintentando...");
      goToDeepSleep(nowEpoch + 60);
    }
    wifiBootstrap.syncTime(8000);
    nowEpoch = time(nullptr);

    if (nowEpoch < 1600000000) {
      Serial.println("Hora inválida después de sincronización. Reintentando...");
      goToDeepSleep(nowEpoch + 60);
    }
    // Calcular inicio de la siguiente ventana
    nextWindowStartEpoch =
        (nowEpoch / WINDOW_PERIOD_SECONDS) * WINDOW_PERIOD_SECONDS + WINDOW_PERIOD_SECONDS;
    Serial.printf("Primer ciclo, ventana: %s", asctime(localtime(&nextWindowStartEpoch)));
    goToDeepSleep(nextWindowStartEpoch);
  }


  // INICIO DE VENTANA DE RECOLECCIÓN (ESP-NOW)
  Serial.printf("Esperando al inicio exacto de la ventana... ahora=%ld objetivo=%ld\n",
                nowEpoch, nextWindowStartEpoch);
  while (time(nullptr) < nextWindowStartEpoch) {
      delay(20);
  }
  nowEpoch = time(nullptr);
  // EXACTAMENTE EL INICIO DE LA VENTANA
  Serial.printf("\n--- INICIO DE VENTANA DE RECOLECCIÓN (%s) ---\n",
                asctime(localtime(&nowEpoch)));
  broadcastTimestamp();
  storeOwnReading();


  unsigned long start = millis();
  while (millis() - start < windowDurationMs) { // windowDurationMs = 50000
    processIncomingPackets();
    delay(5); // Mantener el delay bajo para procesar la cola rápidamente
  }
  processIncomingPackets(); 

  Serial.printf("--- FIN DE VENTANA DE RECOLECCIÓN (%s) ---\n",
                asctime(localtime(&nowEpoch)));
  esp_now_register_recv_cb(NULL);
  esp_now_deinit();

  // PUBLICACIÓN (WIFI/MQTT)
  Serial.println("Conectando WiFi/MQTT para publicación...");
  bool wifiOk = wifiBootstrap.begin(info.ssid.c_str(), info.wifiPassword.c_str(), 15000);
  if (wifiOk) {
    wifiBootstrap.syncTime(8000);
    mqttManager.begin(&info);
    if (mqttManager.connect()) {
      Serial.printf("Publicando %d lecturas via MQTT...\n", (int)readings.size());
      if (mqttManager.publishReadings(readings)) {
        unsigned long t0 = millis();
        while (millis() - t0 < 2000) {
          mqttManager.loop();
          delay(10);
        }
        readings.clear();
      }
      mqttManager.disconnect();
    }
  }
  wifiBootstrap.disconnect();
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  // PROGRAMAR SIGUIENTE VENTANA
  nextWindowStartEpoch += WINDOW_PERIOD_SECONDS;
  while (nextWindowStartEpoch <= time(nullptr))
    nextWindowStartEpoch += WINDOW_PERIOD_SECONDS;

  Serial.printf("Siguiente ventana programada: %s",
                asctime(localtime(&nextWindowStartEpoch)));
  goToDeepSleep(nextWindowStartEpoch);
}