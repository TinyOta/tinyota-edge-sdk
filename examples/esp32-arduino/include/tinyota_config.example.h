// Copy to tinyota_config.h and fill in. Do not commit tinyota_config.h.
#pragma once

#define WIFI_SSID            "your-wifi"
#define WIFI_PASSWORD        "your-password"

// Device API, not the web console URL.
#define TINYOTA_API          "https://api.tinyota.xyz"

// Console > Device Models > your model > registration token.
#define TINYOTA_REG_TOKEN    "paste-registration-token"

// Must match the version you type when uploading this build to the console.
// No leading "v".
#define FW_VERSION           "1.0.0"

// From https://api.tinyota.xyz/api/v1/devices/public-key ("public_key_pem").
// Keep the \n line breaks.
#define TINYOTA_PUBKEY_PEM \
    "-----BEGIN PUBLIC KEY-----\n" \
    "PASTE_BASE64_LINE_1\n" \
    "PASTE_BASE64_LINE_2\n" \
    "-----END PUBLIC KEY-----\n"

// Optional: PEM of the root CA for api.tinyota.xyz. When NULL the TLS
// certificate is not checked (development only; the firmware signature is
// still verified, so a forged image cannot be installed).
#define TINYOTA_ROOT_CA      NULL
