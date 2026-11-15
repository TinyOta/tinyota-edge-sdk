// TinyOTA client for ESP32 + Arduino. See TinyOTA.h for the flow.
//
// Written against the TinyOTA device API (/api/v1/devices/*).
// Not yet tested on hardware; please report issues.

#include "TinyOTA.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

namespace {

// Same codes as fota_error_t in the C SDK, so the console shows one scheme.
int sdkErrorCode(TinyOTA::Result r) {
    switch (r) {
    case TinyOTA::ERR_NETWORK:   return -1;
    case TinyOTA::ERR_REGISTER:  return -1;
    case TinyOTA::ERR_DOWNLOAD:  return -3;
    case TinyOTA::ERR_HASH:      return -4;
    case TinyOTA::ERR_SIGNATURE: return -5;
    case TinyOTA::ERR_FLASH:     return -6;
    default:                     return 0;
    }
}

String deviceSerial() {
    uint64_t mac = ESP.getEfuseMac();  // burned in, stable across reboots
    char buf[17];
    snprintf(buf, sizeof(buf), "%04X%08lX", (unsigned)(uint16_t)(mac >> 32), (unsigned long)(uint32_t)mac);
    return String(buf);
}

bool hexEquals(const uint8_t digest[32], const char *hex) {
    static const char *digits = "0123456789abcdef";
    if (!hex || strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        if (tolower(hex[2 * i]) != digits[digest[i] >> 4]) return false;
        if (tolower(hex[2 * i + 1]) != digits[digest[i] & 0x0F]) return false;
    }
    return true;
}

// signature: base64url DER over SHA-256 of the full image.
bool verifySignature(const char *pubKeyPem, const uint8_t digest[32], const char *sigB64url) {
    if (!pubKeyPem || !sigB64url || !*sigB64url) return false;

    // base64url -> base64 (mbedTLS only decodes the standard alphabet)
    String b64(sigB64url);
    b64.replace('-', '+');
    b64.replace('_', '/');
    while (b64.length() % 4) b64 += '=';

    uint8_t der[128];
    size_t derLen = 0;
    if (mbedtls_base64_decode(der, sizeof(der), &derLen,
                              (const uint8_t *)b64.c_str(), b64.length()) != 0) {
        return false;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = mbedtls_pk_parse_public_key(&pk, (const uint8_t *)pubKeyPem,
                                          strlen(pubKeyPem) + 1) == 0 &&
              mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, 32, der, derLen) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

}  // namespace

TinyOTA::TinyOTA(const char *apiBase, const char *regToken, const char *version,
                 const char *pubKeyPem, const char *rootCaPem)
    : _api(apiBase), _regToken(regToken), _version(version),
      _pubKey(pubKeyPem), _rootCa(rootCaPem) {}

bool TinyOTA::begin() {
    Preferences prefs;
    prefs.begin("tinyota", true);
    _token = prefs.getString("token", "");
    prefs.end();
    return _token.length() > 0 || registerDevice();
}

bool TinyOTA::registerDevice() {
    JsonDocument req;
    req["registration_token"] = _regToken;
    req["unit_serial"] = deviceSerial();
    String body, resp;
    serializeJson(req, body);

    String saved = _token;
    _token = "";  // register is called without a bearer token
    if (postJson("/api/v1/devices/register", body, resp) != 200) {
        _token = saved;
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, resp) || !doc["access_token"].is<const char *>()) return false;
    _token = doc["access_token"].as<String>();

    Preferences prefs;
    prefs.begin("tinyota", false);
    prefs.putString("token", _token);
    prefs.end();
    return true;
}

// Returns the HTTP status code (or a negative HTTPClient error).
int TinyOTA::postJson(const String &path, const String &body, String &response) {
    WiFiClientSecure tls;
    WiFiClient plain;
    bool https = strncmp(_api, "https://", 8) == 0;
    if (https) {
        if (_rootCa) tls.setCACert(_rootCa);
        else tls.setInsecure();  // development only, see tinyota_config.h
    }
    HTTPClient http;
    if (!http.begin(https ? (WiFiClient &)tls : plain, String(_api) + path)) return -1;
    http.addHeader("Content-Type", "application/json");
    if (_token.length()) http.addHeader("Authorization", "Bearer " + _token);
    int code = http.POST(body);
    if (code > 0) response = http.getString();
    http.end();
    return code;
}

bool TinyOTA::reportStatus(const char *status, const char *version,
                           const char *fsmState, int errorCode, const char *message) {
    JsonDocument req;
    req["status"] = status;
    req["version"] = version;
    req["fsm_state"] = fsmState;
    req["error_code"] = errorCode;
    req["message"] = message ? message : "";
    String body, resp;
    serializeJson(req, body);
    return postJson("/api/v1/devices/status", body, resp) == 200;
}

TinyOTA::Result TinyOTA::fail(Result r, const char *fsmState, const char *message) {
    reportStatus("failed", _version, fsmState, sdkErrorCode(r), message);
    return r;
}

TinyOTA::Result TinyOTA::checkForUpdate() {
    if (WiFi.status() != WL_CONNECTED) return ERR_NETWORK;
    if (!_token.length() && !registerDevice()) return ERR_REGISTER;

    // 1. Check. ram_free = 0 asks the server for FULL images only.
    JsonDocument req;
    req["current_version"] = _version;
    req["hw_specs"]["ram_free"] = 0;
    String body, resp;
    serializeJson(req, body);
    int code = postJson("/api/v1/devices/check", body, resp);
    if (code == 401) {  // token expired or device removed: register again
        if (!registerDevice()) return ERR_REGISTER;
        code = postJson("/api/v1/devices/check", body, resp);
    }
    if (code != 200) return ERR_NETWORK;

    JsonDocument info;
    if (deserializeJson(info, resp)) return ERR_NETWORK;
    if (!info["update_available"].as<bool>()) return UP_TO_DATE;

    String target   = info["target_version"] | "";
    String url      = info["download_url"] | "";
    String checksum = info["checksum"] | "";
    String sig      = info["signature"] | "";
    if (!url.length() || checksum.length() != 64 || !sig.length()) {
        return fail(ERR_SIGNATURE, "CHECKING", "update has no checksum or signature");
    }
    if (url.startsWith("/")) url = String(_api) + url;

    // 2. Download straight into the next OTA slot, hashing as we go.
    WiFiClientSecure tls;
    WiFiClient plain;
    bool https = url.startsWith("https://");
    if (https) {
        if (_rootCa) tls.setCACert(_rootCa);
        else tls.setInsecure();
    }
    HTTPClient http;
    if (!http.begin(https ? (WiFiClient &)tls : plain, url)) return fail(ERR_NETWORK, "DOWNLOADING", "begin");
    http.addHeader("Authorization", "Bearer " + _token);
    if (http.GET() != 200) {
        http.end();
        return fail(ERR_DOWNLOAD, "DOWNLOADING", "HTTP error");
    }
    int total = http.getSize();  // -1 when the server does not send a length
    if (!Update.begin(total > 0 ? (size_t)total : UPDATE_SIZE_UNKNOWN)) {
        http.end();
        return fail(ERR_FLASH, "DOWNLOADING", Update.errorString());
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[2048];
    size_t done = 0;
    uint32_t lastData = millis();
    while (http.connected() && (total < 0 || done < (size_t)total)) {
        size_t avail = stream->available();
        if (!avail) {
            if (millis() - lastData > 15000) break;  // stalled
            delay(1);
            continue;
        }
        int n = stream->readBytes(buf, avail < sizeof(buf) ? avail : sizeof(buf));
        if (n <= 0) break;
        lastData = millis();
        mbedtls_sha256_update(&sha, buf, n);
        if (Update.write(buf, n) != (size_t)n) {
            mbedtls_sha256_free(&sha);
            Update.abort();
            http.end();
            return fail(ERR_FLASH, "DOWNLOADING", Update.errorString());
        }
        done += n;
        if (_progress) _progress(done, total > 0 ? total : 0);
    }
    http.end();

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (total > 0 && done != (size_t)total) {
        Update.abort();
        return fail(ERR_DOWNLOAD, "DOWNLOADING", "incomplete download");
    }

    // 3. Verify before the boot slot changes.
    if (!hexEquals(digest, checksum.c_str())) {
        Update.abort();
        return fail(ERR_HASH, "VERIFYING", "SHA-256 mismatch");
    }
    if (!verifySignature(_pubKey, digest, sig.c_str())) {
        Update.abort();
        return fail(ERR_SIGNATURE, "VERIFYING", "signature invalid");
    }

    // 4. Switch slot, report, reboot.
    if (!Update.end(true)) {
        return fail(ERR_FLASH, "SWITCHING", Update.errorString());
    }
    reportStatus("success", target.c_str(), "REBOOT", 0, "");
    delay(200);
    ESP.restart();
    return UPDATED;  // not reached
}
