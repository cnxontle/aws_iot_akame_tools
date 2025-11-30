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

RTC_DATA_ATTR time_t nextWindowStartEpoch = 0;
RTC_DATA_ATTR bool windowExecuted = false;

// ===== CONFIG =====
const int nodeId = 1;
const int numNodes = 100;
const unsigned long slotDurationMs = 1000;
const unsigned long windowDurationMs = slotDurationMs * numNodes;
const int timestampRetries = 5;
const int WAKE_AHEAD_SECONDS = 20;

// WiFi / AWS
String thingName, awsIotEndpoint, gatewayTopic, userId, ssid, wifiPassword;
String caCert, deviceCert, privateKey;

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
String readFile(const char *path);
void addReading(int id, float hum, int raw);
void clearBuffer();
void onEspNowRecv(const esp_now_recv_info *info, const uint8_t *data, int len);
void publishMQTT();
void storeOwnReading();
void broadcastTimestamp();
bool syncTime(unsigned long timeoutMs = 10000);
void connectMQTT();
void goToDeepSleep(time_t nextWindowStart);

// FILE SYSTEM
String readFile(const char *path) {
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

// BUFFER MANAGER
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

void clearBuffer() { readings.clear(); }

// ESP-NOW RECEIVE
void onEspNowRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
  StaticJsonDocument<256> msg;
  if (deserializeJson(msg, data, len)) {
    Serial.println("Ignored packet (invalid JSON)");
    return;
  }
  if (!msg.containsKey("userId") || !msg.containsKey("locationId") ||
      !msg.containsKey("nodeId") || !msg.containsKey("humidity") ||
      !msg.containsKey("raw")) {
    Serial.println("Ignored packet (incomplete JSON)");
    return;
  }

  if (String(msg["userId"]) != userId || String(msg["locationId"]) != thingName) {
    Serial.println("Ignored packet (belongs to different system)");
    return;
  }

  addReading(msg["nodeId"], msg["humidity"], msg["raw"]);
}


// PUBLISH MQTT
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
  char *buffer = (char *)malloc(len);
  serializeJson(doc, buffer, len);
  Serial.print("Publishing: ");
  Serial.println(buffer);

  if (!mqttClient.publish(gatewayTopic.c_str(), buffer))
    Serial.println("MQTT publish ERROR");
  else
    clearBuffer();

  free(buffer);
}


// STORE OWN READING
void storeOwnReading() {
  const int rawDry = 2259;
  const int rawWet = 939;
  int raw = analogRead(34);
  float hum = (rawDry - raw) * 100.0 / (rawDry - rawWet);
  hum = constrain(hum, 0, 100);
  addReading(nodeId, hum, raw);
}


// BROADCAST TIMESTAMP
void broadcastTimestamp() {
  StaticJsonDocument<64> msg;
  msg["timestamp"] = time(nullptr);
  char buffer[64];
  size_t len = serializeJson(msg, buffer);
  uint8_t bcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  for (int i = 0; i < timestampRetries; i++) {
    esp_err_t res = esp_now_send(bcast, (uint8_t *)buffer, len);
    if (res == ESP_OK)
      Serial.printf("Broadcast timestamp %u (try %d)\n", time(nullptr), i + 1);
    delay(1000);
  }
}


// TIME SYNC
bool syncTime(unsigned long timeoutMs) {
  Serial.println("Syncing time via NTP...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  unsigned long start = millis();
  time_t nowSec;

  do {
    nowSec = time(nullptr);
    if (millis() - start > timeoutMs) {
      Serial.println("NTP timeout");
      return false;
    }
    delay(200);
  } while (nowSec < 1600000000);

  Serial.println("Time synced");
  return true;
}

// CONNECT MQTT
void connectMQTT() {
  if (mqttClient.connected())
    return;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected — skipping MQTT connect");
    return;
  }

  wifiClient.setCACert(caCert.c_str());
  wifiClient.setCertificate(deviceCert.c_str());
  wifiClient.setPrivateKey(privateKey.c_str());
  mqttClient.setServer(awsIotEndpoint.c_str(), 8883);

  Serial.println("Connecting to AWS IoT MQTT...");
  if (mqttClient.connect(thingName.c_str()))
    Serial.println("MQTT connected");
  else
    Serial.printf("MQTT connect error: %d\n", mqttClient.state());
}


// DEEP SLEEP
void goToDeepSleep(time_t nextWindowStart) {
  time_t nowEpoch = time(nullptr);
  long sleepSeconds = 0;

  if (nowEpoch < 1600000000) {
    Serial.println("Time NOT valid. Sleeping short for resync.");
    nextWindowStartEpoch = 0;
    sleepSeconds = 60;
  } else {
    long diff = (long)(nextWindowStart - WAKE_AHEAD_SECONDS - nowEpoch);
    if (diff < 1)
      diff = 1;

    const long maxSleep = 30L * 24 * 3600;
    if (diff > maxSleep)
      diff = maxSleep;

    sleepSeconds = diff;
  }

  Serial.printf("Deep sleep for %ld seconds...\n", sleepSeconds);

  if (mqttClient.connected())
    mqttClient.disconnect();
  WiFi.disconnect(true);
  esp_wifi_stop();
  WiFi.mode(WIFI_OFF);
  btStop();
  delay(20);

  esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);
  Serial.flush();
  esp_deep_sleep_start();
}


// SETUP
void setup() {
  Serial.begin(115200);
  delay(200);
  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  Serial.printf("Wakeup cause: %d\n", (int)wakeCause);
  if (wakeCause != ESP_SLEEP_WAKEUP_TIMER) {
    windowExecuted = false;
  }

  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount ERROR");
    return;
  }

  caCert = readFile("/AmazonRootCA1.pem");
  deviceCert = readFile("/certificate.pem");
  privateKey = readFile("/private.key");

  // ---- Cargar metadata ------------------------------------------------
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
    }
  }

  Serial.println("=== CONFIG ===");
  Serial.println("thingName: " + thingName);
  Serial.println("awsIotEndpoint: " + awsIotEndpoint);
  Serial.println("gatewayTopic: " + gatewayTopic);
  Serial.println("userId: " + userId);
  Serial.println("================");

  // Inicializar ESP-NOW
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK)
    Serial.println("ESP-NOW init ERROR");
  else
    Serial.println("ESP-NOW ready");
  esp_now_register_recv_cb(onEspNowRecv);
  // WIFI
  WiFi.begin(ssid.c_str(), wifiPassword.c_str());
  Serial.println("Connecting WiFi...");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected");
    wifi_ap_record_t apInfo;
    if (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) {

      uint8_t wifiChannel = apInfo.primary;
      Serial.print("WiFi channel = ");
      Serial.println(wifiChannel);

      esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);

      esp_now_peer_info_t peerInfo;
      memset(&peerInfo, 0, sizeof(peerInfo));
      memset(peerInfo.peer_addr, 0xFF, 6);
      peerInfo.channel = wifiChannel;
      peerInfo.encrypt = false;
      esp_now_add_peer(&peerInfo);
    }
  }

  syncTime(8000);
  connectMQTT();

  Serial.println("Setup done.");
}

// LOOP
void loop() {
  if (!mqttClient.connected())
    connectMQTT();
  mqttClient.loop();

  time_t nowEpoch = time(nullptr);

  if (nextWindowStartEpoch == 0) {

    if (nowEpoch < 1600000000) {
      Serial.println("Hora inválida. Esperando 1s...");
      delay(1000);
      return;
    }

    struct tm *lt = localtime(&nowEpoch);
    long secsSinceMidnight =
        lt->tm_sec + lt->tm_min * 60 + lt->tm_hour * 3600;

    const long halfHourSecs = 1800;
    long secsUntilNextHalfHour =
        halfHourSecs - (secsSinceMidnight % halfHourSecs);

    nextWindowStartEpoch = nowEpoch + secsUntilNextHalfHour;

    Serial.printf("Primer ciclo. Siguiente ventana: %s",
                  asctime(localtime(&nextWindowStartEpoch)));

    goToDeepSleep(nextWindowStartEpoch);
  }

  if (!windowExecuted && nowEpoch >= nextWindowStartEpoch) {
    windowExecuted = true;  
    Serial.printf("\n--- INICIO DE VENTANA (%s) ---\n",
                  asctime(localtime(&nowEpoch)));
    broadcastTimestamp();
    storeOwnReading();
    Serial.println("Recibiendo lecturas nodos...");
    unsigned long windowStart = millis();
    while (millis() - windowStart < windowDurationMs) {
      mqttClient.loop();
      delay(50);
    }
    publishMQTT();
    nextWindowStartEpoch += 1800;
    while (nextWindowStartEpoch <= time(nullptr))
      nextWindowStartEpoch += 1800;

    Serial.printf("Siguiente ventana: %s",
                  asctime(localtime(&nextWindowStartEpoch)));

    goToDeepSleep(nextWindowStartEpoch);
  }

}

