#include "load_info.h"

// ---------------------------------------------------------------------
//  Leer archivo completo desde LittleFS
// ---------------------------------------------------------------------
String LoadInfo::readFile(const char* path) {
    File f = LittleFS.open(path, "r");
    if (!f) {
        Serial.printf("Error opening %s\n", path);
        return "";
    }
    String content = f.readString();
    f.close();
    Serial.printf("File %s read OK (%u bytes)\n", path, content.length());
    return content;
}

// ---------------------------------------------------------------------
//  Iniciar FS y cargar todo
// ---------------------------------------------------------------------
bool LoadInfo::begin() {
    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount ERROR");
        return false;
    }

    bool metaOK = loadMetadata();
    bool certOK = loadCertificates();

    return metaOK && certOK;
}

// ---------------------------------------------------------------------
//  Cargar metadata.json
// ---------------------------------------------------------------------
bool LoadInfo::loadMetadata() {
    String metadata = readFile("/metadata.json");
    if (!metadata.length()) {
        Serial.println("metadata.json missing or empty");
        return false;
    }

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, metadata);
    if (err) {
        Serial.println("metadata.json invalid");
        return false;
    }

    thingName      = doc["thingName"].as<String>();
    awsIotEndpoint = doc["awsIotEndpoint"].as<String>();
    gatewayTopic   = doc["gatewayTopic"].as<String>();
    userId         = doc["userId"].as<String>();
    ssid           = doc["SSID"].as<String>();
    wifiPassword   = doc["WiFiPassword"].as<String>();

    Serial.println("Metadata loaded OK.");
    return true;
}

// ---------------------------------------------------------------------
//  Cargar certificados y llaves
// ---------------------------------------------------------------------
bool LoadInfo::loadCertificates() {
    caCert     = readFile("/AmazonRootCA1.pem");
    deviceCert = readFile("/certificate.pem");
    privateKey = readFile("/private.key");

    if (!caCert.length() || !deviceCert.length() || !privateKey.length()) {
        Serial.println("Certificate files missing or empty");
        return false;
    }

    Serial.println("Certificates loaded OK.");
    return true;
}
