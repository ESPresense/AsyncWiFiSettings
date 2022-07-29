/*
    AsyncWiFiSettings basic example

    Source and further documentation available at
    https://github.com/ESPresense/AsyncWiFiSettings

    Note: this example is written for ESP32.
    For ESP8266, use LittleFS.begin() instead of SPIFFS.begin(true).
*/

#include <SPIFFS.h>
#include <AsyncWiFiSettings.h>

void setup() {
    Serial.begin(115200);
    SPIFFS.begin(true);  // Will format on the first run after failing to mount

    // Use stored credentials to connect to your WiFi access point.
    // If no credentials are stored or if the access point is out of reach,
    // an access point will be started with a captive portal to configure WiFi.
    AsyncWiFiSettings.connect();
}

void loop() {
    // Your loop code here
}
