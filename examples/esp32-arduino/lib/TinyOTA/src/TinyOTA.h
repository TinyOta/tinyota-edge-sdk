// TinyOTA client for ESP32 + Arduino (Arduino IDE or PlatformIO).
//
// Flow: register once (token kept in NVS) -> POST /devices/check ->
// stream FULL image into the next OTA slot while hashing -> compare SHA-256
// with "checksum" -> verify ECDSA P-256 "signature" -> Update.end(true) ->
// report "success" -> reboot. Any failure aborts the update and reports
// "failed"; the running firmware is untouched.
#pragma once

#include <Arduino.h>

class TinyOTA {
public:
    enum Result {
        UP_TO_DATE = 0,   // no update available
        UPDATED,          // never returned: the device reboots
        ERR_NETWORK,
        ERR_REGISTER,
        ERR_DOWNLOAD,
        ERR_HASH,
        ERR_SIGNATURE,
        ERR_FLASH,
    };

    // apiBase:  "https://api.tinyota.xyz"
    // regToken: registration token of the device model (console)
    // version:  version of the running firmware, same string as in the console
    // pubKeyPem: server signing key, embedded at build time
    // rootCaPem: root CA for TLS, or NULL to skip certificate checks (dev only)
    TinyOTA(const char *apiBase, const char *regToken, const char *version,
            const char *pubKeyPem, const char *rootCaPem = nullptr);

    // Loads the stored device token, registering if there is none.
    bool begin();

    // One check. Safe to call every few minutes or more (API limit: 30/min).
    Result checkForUpdate();

    // Optional progress callback: bytes written, total (0 if unknown).
    void onProgress(void (*cb)(size_t done, size_t total)) { _progress = cb; }

    // Report anything else to the console, e.g. a failed self-test.
    bool reportStatus(const char *status, const char *version,
                      const char *fsmState, int errorCode, const char *message);

    const String &deviceToken() const { return _token; }

private:
    bool registerDevice();
    int  postJson(const String &path, const String &body, String &response);
    Result fail(Result r, const char *fsmState, const char *message);

    const char *_api, *_regToken, *_version, *_pubKey, *_rootCa;
    String _token;
    void (*_progress)(size_t, size_t) = nullptr;
};
