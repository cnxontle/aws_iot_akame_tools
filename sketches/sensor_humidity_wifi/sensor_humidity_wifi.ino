#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>

// ===== ID de este dispositivo =====
const int id_esp32 = 1;    // ESTE COORDINADOR

// ===== Variables desde metadata =====
String thingName;
String awsIotEndpoint;
String gatewayTopic;
String userId;
String SSID;
String WiFiPassword;

// ===== Certificados (leídos desde LittleFS) =====
String caCert, deviceCert, privateKey;

WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

// ---------- BUFFER DINÁMICO DE LECTURAS ----------
struct Reading {
  int id_esp32;
  float humidity;
  int raw;
};
std::vector<Reading> readings;

// Broadcast peer agregado flag (para no reinyectar el peer cada envío)
bool broadcastPeerAdded = false;

// Prototipos
String leerArchivo(const char* ruta);
void agregarLectura(int id, float hum, int raw);
void limpiarBuffer();
void onDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len);
void publicarMQTT();
void enviarLecturaPropia();
void conectarWiFi(unsigned long timeoutMs = 10000);
bool syncTime(unsigned long timeoutMs = 10000);
void conectarMQTT();

// ---------------- Funciones ----------------

String leerArchivo(const char* ruta) {
  File f = LittleFS.open(ruta, "r");
  if (!f) {
    Serial.printf("Error abriendo %s\n", ruta);
    return "";
  }
  String contenido = f.readString();
  f.close();
  Serial.printf("Archivo %s leído correctamente (%u bytes)\n", ruta, (unsigned)contenido.length());
  return contenido;
}

// ---------- Manejo del buffer -----------
void agregarLectura(int id, float hum, int raw) {
  // Si ya existe → reemplazar
  for (Reading &r : readings) {
    if (r.id_esp32 == id) {
      r.humidity = hum;
      r.raw = raw;
      Serial.printf("✔ Actualizado id=%d hum=%.2f raw=%d\n", id, hum, raw);
      return;
    }
  }

  // Si no existe → agregar
  Reading r;
  r.id_esp32 = id;
  r.humidity = hum;
  r.raw = raw;
  readings.push_back(r);

  Serial.printf("✔ Nueva lectura id=%d hum=%.2f raw=%d\n", id, hum, raw);
}

void limpiarBuffer() {
  readings.clear();
}

// --------- ESP-NOW recepción ------------
void onDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
  // opcional: imprimir MAC remitente
  if (info != nullptr) {
    char macStr[18];
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
            info->src_addr[0], info->src_addr[1], info->src_addr[2],
            info->src_addr[3], info->src_addr[4], info->src_addr[5]);
    Serial.print("ESP-NOW desde: ");
    Serial.println(macStr);
  }

  StaticJsonDocument<256> msg;
  DeserializationError err = deserializeJson(msg, data, len);
  if (err) {
    Serial.println("Paquete ignorado (no es JSON válido)");
    return;
  }

  // validar encabezados mínimos
  if (!msg.containsKey("userId") ||
      !msg.containsKey("thingName") ||
      !msg.containsKey("id_esp32") ||
      !msg.containsKey("humidity") ||
      !msg.containsKey("raw")) {

    Serial.println("Paquete ignorado (JSON incompleto)");
    return;
  }

  if (String(msg["userId"]) != userId ||
      String(msg["thingName"]) != thingName) {

    Serial.println("Paquete ignorado (no pertenece a este sistema)");
    return;
  }

  int id   = msg["id_esp32"];
  float hum = msg["humidity"];
  int raw  = msg["raw"];

  agregarLectura(id, hum, raw);
}

// --------- Publicar a AWS IoT MQTT ----------
void publicarMQTT() {
  if (!mqttClient.connected()) {
    Serial.println("⚠ Cliente MQTT no conectado, no se publica.");
    return;
  }

  // Crear JSON
  StaticJsonDocument<512> doc;
  doc["gatewayId"] = thingName;   // este ESP32 coordinador actúa como gateway
  doc["userId"] = userId;
  doc["timestamp"] = time(nullptr);

  JsonArray arr = doc.createNestedArray("readings");

  for (Reading &r : readings) {
    JsonObject o = arr.createNestedObject();
    o["id"] = r.id_esp32;
    o["humidity"] = r.humidity;
    o["raw"] = r.raw;
  }

  // Calcular tamaño
  size_t len = measureJson(doc) + 1;
  char *buffer = (char*)malloc(len);

  serializeJson(doc, buffer, len);

  Serial.print("📤 Publicando: ");
  Serial.println(buffer);

  if (!mqttClient.publish(gatewayTopic.c_str(), buffer)) {
    Serial.println("❌ Error publicando");
  } else {
    Serial.println("✅ Publicación exitosa");
    limpiarBuffer();  // <<<<< ahora limpiamos después de publicar
  }

  free(buffer);
}


// --------- El coordinador genera su propia lectura ---------
void enviarLecturaPropia() {
  const int rawSeco = 2259;
  const int rawMojado = 939;
  int raw = analogRead(34);

  float hum = (rawSeco - raw) * 100.0 / (rawSeco - rawMojado);
  hum = constrain(hum, 0, 100);

  // crear JSON para broadcast
  StaticJsonDocument<128> doc;
  doc["userId"] = userId;
  doc["thingName"] = thingName;
  doc["id_esp32"] = id_esp32;
  doc["humidity"] = hum;
  doc["raw"] = raw;

  // serializar
  char buffer[128];
  size_t len = serializeJson(doc, buffer);

  // enviar en broadcast (añadimos peer broadcast UNA VEZ en setup)
  uint8_t bcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  esp_err_t res = esp_now_send(bcast, (uint8_t*)buffer, len);
  if (res == ESP_OK) {
    Serial.printf("✔ Emitida lectura propia id=%d hum=%.2f raw=%d (esp-now)\n", id_esp32, hum, raw);
  } else {
    Serial.printf("❌ Error esp_now_send: %d\n", res);
  }

  // también la añadimos al buffer local (reemplaza si existe)
  agregarLectura(id_esp32, hum, raw);
}

// ---------------- utilidades WiFi / NTP / MQTT ----------------


bool syncTime(unsigned long timeoutMs) {
  Serial.println("Sincronizando hora NTP...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  unsigned long start = millis();
  time_t nowSec;
  do {
    nowSec = time(nullptr);
    if ((millis() - start) > timeoutMs) {
      Serial.println("❌ Timeout NTP");
      return false;
    }
    delay(200);
  } while (nowSec < 1600000000);
  Serial.println("✔ Hora sincronizada");
  return true;
}

void conectarMQTT() {
  if (mqttClient.connected()) return;

  // asegurar que WiFi esté conectado
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ No conectado a WiFi; intentar conectar WiFi antes de MQTT");
    return;
  }

  // establecer certificados en el cliente TLS
  wifiClient.setCACert(caCert.c_str());
  // deviceCert/privateKey setting is optional if using client certificate auth
  if (deviceCert.length()) wifiClient.setCertificate(deviceCert.c_str());
  if (privateKey.length()) wifiClient.setPrivateKey(privateKey.c_str());

  mqttClient.setServer(awsIotEndpoint.c_str(), 8883);

  Serial.println("Conectando a MQTT (AWS IoT)...");
  // intentar conectar con clientId = thingName
  if (mqttClient.connect(thingName.c_str())) {
    Serial.println("✔ MQTT conectado");
  } else {
    Serial.printf("❌ Error MQTT connect: %d\n", mqttClient.state());
  }
}

// ---------------- SETUP -----------------
void setup() {
  Serial.begin(115200);
  delay(200);

  // iniciar LittleFS
  if (!LittleFS.begin()) {
    Serial.println("❌ Error montando LittleFS");
    // continuamos en caso de que el programa deba probar sin FS, pero muchas cosas fallarán
  }

  // leer certificados y metadata (si existen)
  caCert = leerArchivo("/AmazonRootCA1.pem");
  deviceCert = leerArchivo("/certificate.pem");
  privateKey = leerArchivo("/private.key");

  String metadata = leerArchivo("/metadata.json");
  if (metadata.length()) {
    StaticJsonDocument<512> meta;
    DeserializationError merr = deserializeJson(meta, metadata);
    if (!merr) {
      thingName = meta["thingName"].as<String>();
      awsIotEndpoint = meta["awsIotEndpoint"].as<String>();
      gatewayTopic = meta["gatewayTopic"].as<String>();
      userId = meta["userId"].as<String>();
      SSID = meta["SSID"].as<String>();
      WiFiPassword = meta["WiFiPassword"].as<String>();
    } else {
      Serial.println("❌ metadata.json inválido");
    }
  } else {
    Serial.println("❌ metadata.json no encontrado o vacío");
  }

  Serial.println("=== CONFIG ===");
  Serial.println("thingName: " + thingName);
  Serial.println("awsEndpoint: " + awsIotEndpoint);
  Serial.println("gatewayTopic: " + gatewayTopic);
  Serial.println("userId: " + userId);
  Serial.println("SSID: " + SSID);
  Serial.println("================");


  // 2) Conectar WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID.c_str(), WiFiPassword.c_str());
  Serial.println("Conectando WiFi...");

  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    Serial.print(".");
  }
  Serial.println("\n✔ WiFi conectado");

  // Obtener canal real
  wifi_ap_record_t apInfo;
  esp_wifi_sta_get_ap_info(&apInfo);
  uint8_t wifi_channel = apInfo.primary;

  Serial.print("Canal WiFi = ");
  Serial.println(wifi_channel);

  // ========== 2) Fijar canal para ESP-NOW ==========
  esp_wifi_set_channel(wifi_channel, WIFI_SECOND_CHAN_NONE);


  // 1) Preparar modo STA y ESP-NOW (antes de conectar al WiFi)
 if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error iniciando ESP-NOW");
  } else {
    Serial.println("✔ ESP-NOW iniciado");
  }

  esp_now_register_recv_cb(onDataRecv);

  // Peer broadcast
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memset(peerInfo.peer_addr, 0xFF, 6);
  peerInfo.channel = wifi_channel;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);


  
  // 3) Sincronizar NTP (intentar, no bloquear indefinidamente)
  syncTime(8000);

  // 4) Conectar MQTT (si WiFi disponible)
  conectarMQTT();

  // listo
  Serial.println("Setup completado.");
}

// ---------------- LOOP -----------------
void loop() {
  static unsigned long lastLocalSend = 0;
  static unsigned long lastMQTTSend = 0;

  // mantener MQTT
  if (!mqttClient.connected()) {
    conectarMQTT();
  }
  mqttClient.loop();

  // 1) El coordinador envía su propia lectura cada 10s
  if (millis() - lastLocalSend > 10000) {
    enviarLecturaPropia();
    lastLocalSend = millis();
  }

  // 2) Publicar lecturas acumuladas cada 60s
  if (millis() - lastMQTTSend > 60000) {
    publicarMQTT();
    lastMQTTSend = millis();
  }
}
