#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>

// Configuration
const int nodeId = 1;
String thingName;
String awsIotEndpoint;
String gatewayTopic;
String userId;
String ssid;
String wifiPassword;
String caCert, deviceCert, privateKey;

WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

// Reading structure
struct Reading {
  int nodeId;
  float humidity;
  int raw;
};
std::vector<Reading> readings;

// Prototypes
String readFile(const char* path);
void addReading(int id, float hum, int raw);
void clearBuffer();
void onEspNowRecv(const esp_now_recv_info *info, const uint8_t *data, int len);
void publishMQTT();
void sendOwnReading();
bool syncTime(unsigned long timeoutMs = 10000);
void connectMQTT();

// File system handler
String readFile(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.printf("Error opening %s\n", path);
    return "";
  }
  String content = f.readString();
  f.close();
  Serial.printf("File %s read OK (%u bytes)\n", path, (unsigned)content.length());
  return content;
}

// Buffer manager
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

void clearBuffer() {
  readings.clear();
}

// ESP-NOW receive
void onEspNowRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {

  StaticJsonDocument<256> msg;
  DeserializationError err = deserializeJson(msg, data, len);
  if (err) {
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

  if (String(msg["userId"]) != userId ||
      String(msg["locationId"]) != thingName) {

    Serial.println("Ignored packet (belongs to different system)");
    return;
  }

  int id = msg["nodeId"];
  float hum = msg["humidity"];
  int raw = msg["raw"];

  addReading(id, hum, raw);
}

// Publish via AWS IoT MQTT
void publishMQTT() {
  if (!mqttClient.connected()) {
    Serial.println("MQTT client not connected — skipping publish.");
    return;
  }

  StaticJsonDocument<512> doc;
  doc["userId"] = userId;
  doc["locationId"] = thingName;
  doc["timestamp"] = time(nullptr);

  JsonArray arr = doc.createNestedArray("readings");
  for (Reading &r : readings) {
    JsonObject o = arr.createNestedObject();
    o["nodeId"] = r.nodeId;
    o["humidity"] = r.humidity;
    o["raw"] = r.raw;
  }

  size_t len = measureJson(doc) + 1;
  char *buffer = (char*)malloc(len);
  serializeJson(doc, buffer, len);

  Serial.print("Publishing: ");
  Serial.println(buffer);

  if (!mqttClient.publish(gatewayTopic.c_str(), buffer)) {
    Serial.println("MQTT publish ERROR");
  } else {
    Serial.println("MQTT publish OK");
    clearBuffer();
  }

  free(buffer);
}

// Send coordinator reading via ESP-NOW
void sendOwnReading() {
  const int rawDry = 2259;
  const int rawWet = 939;
  int raw = analogRead(34);
  float hum = (rawDry - raw) * 100.0 / (rawDry - rawWet);
  hum = constrain(hum, 0, 100);

  StaticJsonDocument<128> doc;
  doc["userId"] = userId;
  doc["locationId"] = thingName;
  doc["nodeId"] = nodeId;
  doc["humidity"] = hum;
  doc["raw"] = raw;

  char buffer[128];
  size_t len = serializeJson(doc, buffer);

  uint8_t bcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  esp_err_t res = esp_now_send(bcast, (uint8_t*)buffer, len);

  if (res == ESP_OK) {
    Serial.printf("Broadcast own reading nodeId=%d hum=%.2f raw=%d\n", nodeId, hum, raw);
  } else {
    Serial.printf("esp_now_send ERROR: %d\n", res);
  }

  addReading(nodeId, hum, raw);
}

// Time sync
bool syncTime(unsigned long timeoutMs) {
  Serial.println("Syncing time via NTP...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  unsigned long start = millis();
  time_t nowSec;

  do {
    nowSec = time(nullptr);
    if ((millis() - start) > timeoutMs) {
      Serial.println("NTP timeout");
      return false;
    }
    delay(200);
  } while (nowSec < 1600000000);

  Serial.println("Time synced");
  return true;
}

// MQTT connect
void connectMQTT() {
  if (mqttClient.connected()) return;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected — skipping MQTT connect");
    return;
  }

  wifiClient.setCACert(caCert.c_str());
  if (deviceCert.length()) wifiClient.setCertificate(deviceCert.c_str());
  if (privateKey.length()) wifiClient.setPrivateKey(privateKey.c_str());
  mqttClient.setServer(awsIotEndpoint.c_str(), 8883);

  Serial.println("Connecting to AWS IoT MQTT...");

  if (mqttClient.connect(thingName.c_str())) {
    Serial.println("MQTT connected");
  } else {
    Serial.printf("MQTT connect error: %d\n", mqttClient.state());
  }
}

// SETUP
void setup() {
  Serial.begin(115200);
  delay(200);

  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount ERROR");
    return;
  }

  caCert = readFile("/AmazonRootCA1.pem");
  deviceCert = readFile("/certificate.pem");
  privateKey = readFile("/private.key");

  String metadata = readFile("/metadata.json");
  if (metadata.length()) {
    StaticJsonDocument<512> meta;
    if (!deserializeJson(meta, metadata)) {
      thingName = meta["thingName"].as<String>();
      awsIotEndpoint = meta["awsIotEndpoint"].as<String>();
      gatewayTopic = meta["gatewayTopic"].as<String>();
      userId = meta["userId"].as<String>();
      ssid = meta["SSID"].as<String>();
      wifiPassword = meta["WiFiPassword"].as<String>();
    } else {
      Serial.println("metadata.json invalid");
    }
  } else {
    Serial.println("metadata.json missing or empty");
  }

  Serial.println("=== CONFIG ===");
  Serial.println("thingName: " + thingName);
  Serial.println("awsIotEndpoint: " + awsIotEndpoint);
  Serial.println("gatewayTopic: " + gatewayTopic);
  Serial.println("userId: " + userId);
  Serial.println("SSID: " + ssid);
  Serial.println("================");

  // ESP-NOW + WiFi
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init ERROR");
  } else {
    Serial.println("ESP-NOW ready");
  }

  esp_now_register_recv_cb(onEspNowRecv);

  WiFi.begin(ssid.c_str(), wifiPassword.c_str());
  Serial.println("Connecting WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  wifi_ap_record_t apInfo;
  esp_wifi_sta_get_ap_info(&apInfo);
  uint8_t wifiChannel = apInfo.primary;

  Serial.print("WiFi channel = ");
  Serial.println(wifiChannel);

  esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);

  // Broadcast peer
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memset(peerInfo.peer_addr, 0xFF, 6);
  peerInfo.channel = wifiChannel;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  // MQTT
  syncTime(8000);
  connectMQTT();

  Serial.println("Setup done.");
}

// LOOP
void loop() {
  static unsigned long lastLocalSend = 0;
  static unsigned long lastMQTTSend = 0;

  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();

  if (millis() - lastLocalSend > 10000) {
    sendOwnReading();
    lastLocalSend = millis();
  }

  if (millis() - lastMQTTSend > 60000) {
    publishMQTT();
    lastMQTTSend = millis();
  }
}
