#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_sleep.h"
#define ESPNOW_CHANNEL 1

RTC_DATA_ATTR time_t lastTimestamp = 0;

// --------------------- CONFIG ------------------------
const int nodeId = 2;                // <- cambia por nodo
const unsigned long slotDurationMs = 1000;
const int numNodes = 50;             // cantidad de nodos activos en la red
const int maxNodes = 255;            // capacidad máxima teórica del mesh
const long WINDOW_PERIOD_SEC = 1800;  // 30 minutos
const int rawdry = 2509;            // calibración sensor humedad
const int rawwet = 939;

// Flags
volatile bool timestampReceived = false;
volatile time_t receivedTimestamp = 0;
static bool firstTs = false;
bool timestampForwarded = false;
bool forwardedNode[maxNodes + 1];  

// Paquetes recibidos fuera de ISR
struct RawPkt {
  uint8_t data[128];
  int len;
};
QueueHandle_t espNowQueue;

// ISR
void IRAM_ATTR onEspNowRecv(const esp_now_recv_info *infoRecv,
                            const uint8_t *data, int len)
{
  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, data, len);

  if (!err && doc.containsKey("timestamp") && !firstTs) {
    receivedTimestamp = (time_t)doc["timestamp"];
    timestampReceived = true;
    firstTs = true;
}

  // Encolamos SIEMPRE el paquete para retransmitir
  RawPkt pkt;
  if (len > 128) len = 128;
  memcpy(pkt.data, data, len);
  pkt.len = len;

  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xQueueSendFromISR(espNowQueue, &pkt, &xHigherPriorityTaskWoken);
  if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

// sensor
void readHumidity(float &hum, int &raw) {
    int readings[5];
    for (int i = 0; i < 5; i++) {
        readings[i] = analogRead(34);
        delay(5);
    }
    // Ordenamos parcialmente para obtener la mediana
    std::sort(readings, readings + 5);
    raw = (readings[1] + readings[2] + readings[3]) / 3;  // 3 valores centrales

    float denominator = rawdry - rawwet;
    if (denominator == 0) {
        hum = 0;
        return;
    }
    hum = (rawdry - raw) * 100.0 / denominator;
    hum = constrain(hum, 0, 100);
}

// send reading
void sendReading(float hum, int raw) {
  StaticJsonDocument<128> doc;
  doc["nodeId"]   = nodeId;
  doc["humidity"] = hum;
  doc["raw"]      = raw;

  char buffer[128];
  size_t len = serializeJson(doc, buffer);

  uint8_t bcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  esp_now_send(bcast, (uint8_t*)buffer, len);
}

//  rebroadcast
void rebroadcastIncoming() {
    RawPkt pkt;
    
    while (xQueueReceive(espNowQueue, &pkt, 0) == pdTRUE) {
        StaticJsonDocument<128> doc;
        bool parsed = (deserializeJson(doc, pkt.data, pkt.len) == DeserializationError::Ok);

        // TIMESTAMP
        if (parsed && doc.containsKey("timestamp")) {
            if (!timestampForwarded) {
                timestampForwarded = true;
                uint8_t bcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
                esp_now_send(bcast, pkt.data, pkt.len);
            }
            continue; 
        }

        // MENSAJE DE NODO
        if (parsed && doc.containsKey("nodeId")) {
            int id = doc["nodeId"];
            if (id < 1 || id > maxNodes) continue;

            // Reenviar solo la primera vez
            if (!forwardedNode[id]) {
                forwardedNode[id] = true;
                uint8_t bcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
                esp_now_send(bcast, pkt.data, pkt.len);
            }
            continue; 
        }

        // PAQUETE DESCONOCIDO
        continue;
    }
}


// SETUP 
void setup() {
  Serial.begin(115200);
  delay(200);

  memset(forwardedNode, 0, sizeof(forwardedNode));
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  espNowQueue = xQueueCreate(64, sizeof(RawPkt));
  

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init FAIL");
  }

  esp_now_peer_info_t peerInfo = {};
  memset(peerInfo.peer_addr, 0xFF, 6);
  peerInfo.encrypt = false;
  peerInfo.channel = ESPNOW_CHANNEL;
  esp_now_add_peer(&peerInfo);

  esp_now_register_recv_cb(onEspNowRecv);

  Serial.printf("Nodo %d listo. Esperando timestamp...\n", nodeId);
}

// LOOP
void loop() {

  // 1. Esperar hasta recibir timestamp
  while (!timestampReceived) {
    rebroadcastIncoming();
    delay(10);
  }

  timestampReceived = false;
  lastTimestamp = receivedTimestamp;

  Serial.printf("Timestamp recibido: %ld\n", lastTimestamp);

  // Sincronizar hora interna
  struct timeval tv { lastTimestamp, 0 };
  settimeofday(&tv, nullptr);

  // 2. Calcular inicio/fin de ventana
  unsigned long windowStartMs = millis();  // comienzo exacto de ventana
  unsigned long mySlotMs = nodeId * slotDurationMs;            
  unsigned long windowEndMs = windowStartMs + slotDurationMs * numNodes;

  // 3. Esperar hasta mi slot exacto
  while (millis() < mySlotMs + windowStartMs) {
    rebroadcastIncoming();
    delay(5);
  } 

  // 4. Leer y enviar mis datos
  float hum; int raw;
  readHumidity(hum, raw);
  Serial.printf("Enviando lectura: hum=%.2f raw=%d\n", hum, raw);
  sendReading(hum, raw);

  // 5. Mantenerse despierto escuchando y retransmitiendo
  Serial.println("Esperando fin de ventana y retransmitiendo...");
  while (millis() < windowEndMs) {
    rebroadcastIncoming();
    delay(5);
  }

  Serial.println("Ventana terminada. Durmiendo hasta la siguiente...");

  // 6. Programar deep sleep hasta la próxima ventana
  time_t nextWake = lastTimestamp + WINDOW_PERIOD_SEC - 12;
  time_t now = time(nullptr);
  long sleepSec = nextWake - now;
  if (sleepSec < 5) sleepSec = 5;

  Serial.printf("Durmiendo %ld segundos...\n", sleepSec);

  esp_now_deinit();
  WiFi.mode(WIFI_OFF);

  esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
  Serial.flush();
  firstTs = false;
  timestampForwarded = false;
  memset(forwardedNode, 0, sizeof(forwardedNode));
  esp_deep_sleep_start();
}
