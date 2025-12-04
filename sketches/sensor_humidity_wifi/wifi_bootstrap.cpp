#include "wifi_bootstrap.h"

wifiBootstrap::wifiBootstrap() : _channel(-1) {}

bool wifiBootstrap::begin(const String& ssid, const String& password, unsigned long timeoutMs) {
    _ssid = ssid;
    _password = password;

    WiFi.mode(WIFI_STA);
    WiFi.begin(_ssid.c_str(), _password.c_str());

    Serial.println("connecting to WiFi...");

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
        delay(200);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi connection FAILED");
        _channel = -1;
        return false;
    }

    Serial.println("WiFi connected");

    wifi_ap_record_t apInfo;
    if (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) {
        _channel = apInfo.primary;
        Serial.print("AP channel = ");
        Serial.println(_channel);
    } else {
        _channel = -1;
        Serial.println("Could not read AP info");
    }

    return true;
}

bool wifiBootstrap::syncTime(unsigned long timeoutMs) {
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

    Serial.println("Time OK");
    return true;
}

int wifiBootstrap::getChannel() {
    return _channel;
}

bool wifiBootstrap::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

void wifiBootstrap::disconnect() {
    Serial.println("Disconnecting WiFi completely...");

    WiFi.disconnect(true);  // olvidar redes
    esp_wifi_stop();        // detener stack WiFi
    WiFi.mode(WIFI_OFF);    // apagar radios WiFi
    btStop();               // apagar Bluetooth si estuviera activo

    delay(10);
    _channel = -1;
}
