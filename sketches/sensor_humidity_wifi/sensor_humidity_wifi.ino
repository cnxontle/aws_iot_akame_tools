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

// Persistente entre deep sleeps
RTC_DATA_ATTR time_t nextWindowStartEpoch = 0; 

// ===== CONFIG =====
const int nodeId = 1; // Coordinador
const int numNodes = 50; // Número máximo de nodos esperados
const unsigned long slotDurationMs = 1000; // 1 segundo por slot
const unsigned long windowDurationMs = slotDurationMs * numNodes; // Ventana completa
const int timestampRetries = 3; // Intentos de broadcast de timestamp
const int WAKE_AHEAD_SECONDS = 20; 

LoadInfo info; // Carga info desde FS

WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

// ===== STRUCTS =====
struct Reading {
  int nodeId;
  float humidity;
  int raw;
};
std::vector<Reading> readings;

// ===== PROTOTYPES =====
String readFile(const char* path);
void addReading(int id, float hum, int raw);
void clearBuffer();
void onEspNowRecv(const esp_now_recv_info *info, const uint8_t *data, int len);
void publishMQTT();
void storeOwnReading();
void broadcastTimestamp();
bool syncTime(unsigned long timeoutMs = 10000);
void connectMQTT();
void goToDeepSleep(time_t nextWindowStart); 

// ===== BUFFER MANAGER =====
void addReading(int id, float hum, int raw) {
  for (Reading &r : readings) {
    if (r.nodeId == id) {
      r.humidity = hum;
      r.raw = raw;
      Serial.printf("Updated nodeId=%d hum=%.2f raw=%d\n", id, hum, raw);
      return;
    }
  }
  Reading r;
  r.nodeId = id;
  r.humidity = hum;
  r.raw = raw;
  readings.push_back(r);
  Serial.printf("New reading nodeId=%d hum=%.2f raw=%d\n", id, hum, raw);
}

void clearBuffer() { readings.clear(); }

// ===== ESP-NOW RECEIVE =====
void onEspNowRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
  StaticJsonDocument<256> msg;
  DeserializationError err = deserializeJson(msg, data, len); 
  if (err) { Serial.println("Ignored packet (invalid JSON)"); return; } 

  if (!msg.containsKey("userId") || !msg.containsKey("locationId") ||
      !msg.containsKey("nodeId") || !msg.containsKey("humidity") ||
      !msg.containsKey("raw")) {
    Serial.println("Ignored packet (incomplete JSON)");
    return;
  }

 if (String(msg["userId"]) != ::info.userId || String(msg["locationId"]) != ::info.thingName) { 
    Serial.println("Ignored packet (belongs to different system)");
    return;
  }

  int id = msg["nodeId"];
  float hum = msg["humidity"];
  int raw = msg["raw"];
  addReading(id, hum, raw);
}

// ===== PUBLISH MQTT =====
void publishMQTT() {
  if (!mqttClient.connected()) { Serial.println("MQTT client not connected — skipping publish."); return; }
  StaticJsonDocument<4096> doc;
  doc["userId"]      = info.userId;
  doc["locationId"]  = info.thingName;
  doc["timestamp"]   = time(nullptr);
  JsonArray arr = doc.createNestedArray("readings");
  for (Reading &r : readings) {
    JsonObject o = arr.createNestedObject();
    o["nodeId"] = r.nodeId;
    o["humidity"] = r.humidity;
    o["raw"] = r.raw;
  }
  char buffer[4096];
  serializeJson(doc, buffer, sizeof(buffer));
  Serial.print("Publishing: "); Serial.println(buffer);
  if (!mqttClient.publish(info.gatewayTopic.c_str(), buffer)) Serial.println("MQTT publish ERROR");
  else clearBuffer();
}

// ===== STORE COORDINATOR READING =====
void storeOwnReading() {
  const int rawDry = 2259;
  const int rawWet = 939;
  int raw = analogRead(34);
  float hum = (rawDry - raw) * 100.0 / (rawDry - rawWet);
  hum = constrain(hum, 0, 100);
  addReading(nodeId, hum, raw); // Solo almacena en buffer
}

// ===== BROADCAST TIMESTAMP =====
void broadcastTimestamp() {
  StaticJsonDocument<64> msg;
  msg["timestamp"] = time(nullptr);
  char buffer[64];
  size_t len = serializeJson(msg, buffer);
  uint8_t bcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  for (int i = 0; i < timestampRetries; i++) {
    esp_err_t res = esp_now_send(bcast, (uint8_t*)buffer, len);
    if (res == ESP_OK) Serial.printf("Broadcast timestamp %u (try %d)\n", time(nullptr), i+1);
    delay(1000);
  }
}

// ===== TIME SYNC =====
bool syncTime(unsigned long timeoutMs) {
  Serial.println("Syncing time via NTP...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  unsigned long start = millis();
  time_t nowSec;
  do {
    nowSec = time(nullptr);
    if ((millis() - start) > timeoutMs) { Serial.println("NTP timeout"); return false; }
    delay(200);
  } while (nowSec < 1600000000);
  Serial.println("Time synced");
  return true;
}

// ===== MQTT CONNECT =====
void connectMQTT() {
  if (mqttClient.connected()) return;
  if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi not connected — skipping MQTT connect"); return; }

  wifiClient.setCACert(info.caCert.c_str());
  if (info.deviceCert.length()) wifiClient.setCertificate(info.deviceCert.c_str());
  if (info.privateKey.length()) wifiClient.setPrivateKey(info.privateKey.c_str());
  mqttClient.setServer(info.awsIotEndpoint.c_str(), 8883);

  Serial.println("Connecting to AWS IoT MQTT...");
  if (mqttClient.connect(info.thingName.c_str())) Serial.println("MQTT connected");
  else Serial.printf("MQTT connect error: %d\n", mqttClient.state());
}

// ===== DEEP SLEEP MANAGER =====
void goToDeepSleep(time_t nextWindowStart) {  
  time_t nowEpoch = time(nullptr); 
  long sleepSeconds = 0; 

  // Si la hora no es válida, no programamos un sleep largo: dormimos poco y forzamos resync.
  if (nowEpoch < 1600000000) { // tiempo inválido -> NTP no sincronizado
    Serial.println("Time NOT valid. Sleeping short to allow resync on wake. Clearing nextWindowStartEpoch.");
    nextWindowStartEpoch = 0; 
    sleepSeconds = 60; 
  } else {
    long diff = (long)(nextWindowStart - WAKE_AHEAD_SECONDS - nowEpoch);
    if (diff < 1) diff = 1;
    // cap razonable (ej. 30 días en segundos) para evitar valores absurdos 
    const long maxSleep = 30L * 24 * 3600;
    if (diff > maxSleep) diff = maxSleep;
    sleepSeconds = diff;
  }

  Serial.printf("Deep sleep for %ld seconds...\n", sleepSeconds);

  // desconectar / apagar radios antes de dormir 
  if (mqttClient.connected()) mqttClient.disconnect(); 
  WiFi.disconnect(true); 
  esp_wifi_stop();
  WiFi.mode(WIFI_OFF); 
  btStop();
  delay(20); 

  // habilitar wakeup por timer
  esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);
  Serial.flush();
  esp_deep_sleep_start();
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(200);

  // Mostrar causa de wakeup para debug 
  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause(); 
  Serial.printf("Wakeup cause: %d\n", (int)wakeCause); 

  // ===== USAR LoadInfo =====
  if (!info.begin()) {
    Serial.println("ERROR loading metadata or certs");
  }
  Serial.println("=== CONFIG ===");
  Serial.println("thingName: " + info.thingName);
  Serial.println("userId: " + info.userId);
  Serial.println("================");
  
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) Serial.println("ESP-NOW init ERROR");
  else Serial.println("ESP-NOW ready");

  esp_now_register_recv_cb(onEspNowRecv);

  WiFi.begin(info.ssid.c_str(), info.wifiPassword.c_str());
  Serial.println("Connecting WiFi...");
  // esperar WiFi con timeout para no quedarnos bloqueados indefinidamente
  unsigned long wifiStart = millis(); 
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) { 
    delay(200); Serial.print("."); 
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected");
    wifi_ap_record_t apInfo;
    if (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) { 
      uint8_t wifiChannel = apInfo.primary;
      Serial.print("WiFi channel = "); Serial.println(wifiChannel);
      esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
      // Broadcast peer
      esp_now_peer_info_t peerInfo;
      memset(&peerInfo, 0, sizeof(peerInfo));
      memset(peerInfo.peer_addr, 0xFF, 6);
      peerInfo.channel = wifiChannel;
      peerInfo.encrypt = false;
      esp_now_add_peer(&peerInfo);
    } else {
      Serial.println("No AP info available");
    }
  } else {
    Serial.println("WiFi failed to connect within timeout"); 
  }

  // sincronizar hora (si falla, lo manejamos en goToDeepSleep con sleeps cortos) 
  syncTime(8000); 
  connectMQTT();
  Serial.println("Setup done.");
}

// ===== LOOP =====
void loop() {
    // Mantener conexión MQTT
    if (!mqttClient.connected()) connectMQTT();
    mqttClient.loop();

    // Obtener hora actual
    time_t nowEpoch = time(nullptr);

    // Calcular primer ciclo si no hay ventana programada
    if (nextWindowStartEpoch == 0) {
        if (nowEpoch < 1600000000) { 
            Serial.println("Hora inválida en primer ciclo. Esperando 1s para reintentar loop.");
            delay(1000);
            return;
        }

        nextWindowStartEpoch = (nowEpoch / 1800) * 1800 + 1800;

        time_t sleepUntil = nextWindowStartEpoch - WAKE_AHEAD_SECONDS; // despertar antes
        if (sleepUntil < nowEpoch) sleepUntil = nowEpoch + 1; // evitar valores negativos

        Serial.printf("Primer ciclo. Ventana a iniciar en: %s", asctime(localtime(&nextWindowStartEpoch)));
        Serial.printf("Despertar programado para: %s", asctime(localtime(&sleepUntil)));

        goToDeepSleep(sleepUntil);
    }

    // Verificar si es momento de preparar la ventana
    time_t wakeTime = nextWindowStartEpoch;
    if (nowEpoch >= wakeTime) {
        Serial.printf("\n--- PREPARANDO VENTANA (%s) ---\n", asctime(localtime(&nowEpoch)));

        // Broadcast y lectura propia
        broadcastTimestamp();
        storeOwnReading();

        // Esperar hasta inicio exacto de la ventana
        while (time(nullptr) < nextWindowStartEpoch) {
            mqttClient.loop();
            delay(50);
        }

        // Inicio de ventana
        Serial.printf("--- INICIO DE VENTANA (%s) ---\n", asctime(localtime(&nextWindowStartEpoch)));
        unsigned long windowStart = millis();
        while (millis() - windowStart < windowDurationMs) {
            mqttClient.loop();
            delay(50);
        }

        // Publicar lecturas
        publishMQTT();

        // Programar siguiente ventana
        nextWindowStartEpoch += 1800; 
        while (nextWindowStartEpoch <= time(nullptr)) nextWindowStartEpoch += 1800;
        Serial.printf("Siguiente ventana programada: %s", asctime(localtime(&nextWindowStartEpoch)));

        // Dormir hasta próxima ventana con anticipación
        time_t nextSleep = nextWindowStartEpoch - WAKE_AHEAD_SECONDS;
        goToDeepSleep(nextSleep);
    }

    delay(500);
}