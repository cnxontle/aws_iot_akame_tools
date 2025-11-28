#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>

// ===== Variables obtenidas desde metadata.json =====
String thingName;
String awsIotEndpoint;
String gatewayTopic;
String userId;
String SSID;
String WiFiPassword;

// ===== SSL Buffers =====
String caCert;
String deviceCert;
String privateKey;
String publicKey;

WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

// ========= Función para leer archivo de LittleFS =========
String leerArchivo(const char* ruta) {
  File f = LittleFS.open(ruta, "r");
  if (!f) {
    Serial.printf(" Error abriendo %s\n", ruta);
    return "";
  }
  String contenido = f.readString();
  f.close();
  Serial.printf(" Archivo %s leído correctamente (%d bytes)\n",
                ruta, contenido.length());
  return contenido;
}

// ========= Sincronizar hora NTP =========
void syncTime() {
  Serial.println("Sincronizando hora por NTP...");

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  time_t nowSec;
  do {
    delay(500);
    nowSec = time(nullptr);
  } while (nowSec < 1700000000);

  Serial.println(" Hora sincronizada correctamente\n");
}

// ========= Conectar MQTT =========
void conectarMQTT() {
  Serial.println("Conectando a MQTT...");
  while (!mqttClient.connected()) {

    // conexion reconexion SSL
    wifiClient.stop();
    delay(200);

    Serial.println("Iniciando sesión SSL...");

    if (!wifiClient.connect(awsIotEndpoint.c_str(), 8883)) {
      Serial.println("Error al iniciar SSL. Reintentando en 2s...");
      delay(2000);
      continue;
    }
    Serial.println("SSL listo, intentando MQTT...");


    // conexion reconexion mqtt
    if (mqttClient.connect(thingName.c_str())) {
      Serial.println("MQTT conectado a AWS IoT Core");
    } else {
      Serial.print(" Error MQTT: ");
      Serial.println(mqttClient.state());
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // ===== Filesystem =====
  if (!LittleFS.begin()) {
    Serial.println(" Error montando LittleFS");
    return;
  }

  // ===== Leer archivos =====
  caCert = leerArchivo("/AmazonRootCA1.pem");
  deviceCert = leerArchivo("/certificate.pem");
  privateKey = leerArchivo("/private.key");
  publicKey = leerArchivo("/public.key");

  String metadata = leerArchivo("/metadata.json");

  // ===== Parsear metadata =====
  StaticJsonDocument<512> meta;
  deserializeJson(meta, metadata);

  thingName = meta["thingName"].as<String>();
  awsIotEndpoint = meta["awsIotEndpoint"].as<String>();
  gatewayTopic = meta["gatewayTopic"].as<String>();
  userId = meta["userId"].as<String>();
  SSID = meta["SSID"].as<String>();
  WiFiPassword = meta["WiFiPassword"].as<String>();

  Serial.println("\n=== METADATA CARGADA ===");
  Serial.println("thingName: " + thingName);
  Serial.println("endpoint:  " + awsIotEndpoint);
  Serial.println("topic:     " + gatewayTopic);
  Serial.println("userId:    " + userId);
  Serial.println("SSID:    " + SSID);
  Serial.println("WiFiPassword:    " + WiFiPassword);
  Serial.println("=========================\n");

  // ===== Conexión WiFi =====
  WiFi.begin(SSID, WiFiPassword);
  Serial.print("Conectando WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(400);
  }
  Serial.println("\n WiFi conectado");

  // ===== NTP =====
  syncTime();

  // ===== Cargar certificados SSL =====
  wifiClient.setCACert(caCert.c_str());
  wifiClient.setCertificate(deviceCert.c_str());
  wifiClient.setPrivateKey(privateKey.c_str());

  // ===== Configurar MQTT =====
  mqttClient.setServer(awsIotEndpoint.c_str(), 8883);
  conectarMQTT();
}

void loop() {
  // --- Reconexion WiFi ---
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(" WiFi desconectado, intentando reconectar...");
    WiFi.reconnect();
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi reconectado");
    } else {
      Serial.println("No se pudo reconectar WiFi");
    }
  }
  if (!mqttClient.connected()) {
    conectarMQTT();
  }
  mqttClient.loop();
  static unsigned long lastSend = 0;
  if (millis() - lastSend > 10000) {


    const int rawSeco = 2259;
    const int rawMojado = 939;
    int raw = analogRead(34);

    // Convertir a % humedad
    float humidity = (rawSeco - raw) * 100.0 / (rawSeco - rawMojado);
    if (humidity < 0) humidity = 0;
    if (humidity > 100) humidity = 100;

    StaticJsonDocument<128> doc;
    doc["humidity"] = humidity;
    doc["raw"] = raw;
    doc["timestamp"] = time(nullptr);


    char buffer[128];
    serializeJson(doc, buffer);

    if (mqttClient.publish(gatewayTopic.c_str(), buffer)) {
      Serial.print("Publicado: ");
      Serial.println(buffer);
    } else {
      Serial.println("Error publicando mensaje");
    }
    lastSend = millis();
  }
}
