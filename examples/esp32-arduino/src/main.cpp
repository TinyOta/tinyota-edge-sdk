#include <Arduino.h>
#include <WiFi.h>
#include <TinyOTA.h>

#include "tinyota_config.h"  // copy tinyota_config.example.h and fill it in

TinyOTA ota(TINYOTA_API, TINYOTA_REG_TOKEN, FW_VERSION, TINYOTA_PUBKEY_PEM, TINYOTA_ROOT_CA);

const unsigned long CHECK_EVERY_MS = 60UL * 60UL * 1000UL;  // 1 hour
unsigned long lastCheck = 0;

void setup() {
    Serial.begin(115200);
    Serial.printf("\nfirmware %s\n", FW_VERSION);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\nWi-Fi connected, IP %s\n", WiFi.localIP().toString().c_str());

    if (!ota.begin()) {
        Serial.println("TinyOTA: registration failed, check TINYOTA_REG_TOKEN");
    }
    ota.onProgress([](size_t done, size_t total) {
        Serial.printf("OTA %u / %u bytes\r", (unsigned)done, (unsigned)total);
    });
    lastCheck = millis() - CHECK_EVERY_MS;  // check right after boot
}

void loop() {
    if (millis() - lastCheck >= CHECK_EVERY_MS) {
        lastCheck = millis();
        TinyOTA::Result r = ota.checkForUpdate();  // reboots on success
        Serial.printf("TinyOTA check: %d\n", (int)r);
    }

    // ... your application code ...
    delay(10);
}
