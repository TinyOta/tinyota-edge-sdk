// Copy to tinyota_config.h and fill in. Do not commit tinyota_config.h.
#pragma once

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

// Root CA of api.tinyota.xyz, registered at TLS tag 42 at boot. Get it with
//   openssl s_client -showcerts -connect api.tinyota.xyz:443 </dev/null
// and keep the last certificate in the chain.
#define TINYOTA_ROOT_CA_PEM \
    "-----BEGIN CERTIFICATE-----\n" \
    "PASTE_ROOT_CA\n" \
    "-----END CERTIFICATE-----\n"
