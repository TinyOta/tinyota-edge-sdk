/**
 * @file fota_platform_esp32.c
 * @brief FOTA platform seam implementation for ESP32-C3 / ESP32-S3 (ESP-IDF)
 *
 * Implements:
 *   fota_http_get()       — via esp_http_client (TLS via mbedTLS built-in)
 *   fota_http_post_json() — via esp_http_client
 *   fota_sha256()         — via ESP HW SHA accelerator (esp_rom_sha256 / mbedtls)
 *   fota_ecdsa_verify()   — via ESP-IDF mbedTLS
 *
 * Dependencies (ESP-IDF components):
 *   esp_http_client, mbedtls, nvs_flash
 *
 * Server CA certificate:
 *   Validated with ESP-IDF's certificate bundle (esp_crt_bundle_attach,
 *   CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y by default).
 *
 * Build:
 *   idf_component_register(SRCS "fota_platform_esp32.c" ...)
 */

#include "../include/fota_platform.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"
#include "mbedtls/base64.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "fota_platform";

/* ------------------------------------------------------------------ */
/* Server public key — replace with your production key before release */
/* ------------------------------------------------------------------ */
/*
 * Put TINYOTA_PUBKEY_PEM in a tinyota_config.h on your include path (see
 * examples/), or pass it with -D.  Get the PEM from
 * GET https://api.tinyota.xyz/api/v1/devices/public-key and embed it at
 * build time; never fetch it at runtime.
 */
#if defined(__has_include)
#  if __has_include("tinyota_config.h")
#    include "tinyota_config.h"
#  endif
#endif

#ifdef TINYOTA_PUBKEY_PEM
static const char *SERVER_PUBKEY_PEM = TINYOTA_PUBKEY_PEM;
#else
#warning "TINYOTA_PUBKEY_PEM is not set: signature checks will fail until you add it"
static const char *SERVER_PUBKEY_PEM =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE"
    "REPLACE_WITH_REAL_P256_PUBLIC_KEY_BASE64"
    "==\n"
    "-----END PUBLIC KEY-----\n";
#endif

/* ------------------------------------------------------------------ */
/* Internal: HTTP response accumulator                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t *buf;
    size_t   size;
    size_t   used;
} http_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_buf_t *hb = (http_buf_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (hb && evt->data_len > 0) {
            size_t room = hb->size - hb->used;
            size_t copy = (evt->data_len < room) ? evt->data_len : room;
            memcpy(hb->buf + hb->used, evt->data, copy);
            hb->used += copy;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* fota_http_get                                                        */
/* ------------------------------------------------------------------ */
int fota_http_get(const char *url,
                  const char *bearer_token,
                  uint32_t    range_start,
                  uint8_t    *out_buf,
                  size_t      buf_size)
{
    if (!url || !out_buf || buf_size == 0) return -1;

    http_buf_t hb = { .buf = out_buf, .size = buf_size, .used = 0 };

    esp_http_client_config_t cfg = {
        .url            = url,
        .timeout_ms     = 30000,
        .event_handler  = http_event_handler,
        .user_data      = &hb,
        /* Validate the server with ESP-IDF's built-in CA bundle
         * (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y, the default). */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    /* Authorization header */
    if (bearer_token && bearer_token[0]) {
        char auth[512];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer_token);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    /* Range header for resume support */
    if (range_start > 0) {
        char range[64];
        snprintf(range, sizeof(range), "bytes=%u-", (unsigned)range_start);
        esp_http_client_set_header(client, "Range", range);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fota_http_get HTTP error: %s", esp_err_to_name(err));
        return -1;
    }
    if (status != 200 && status != 206) {
        ESP_LOGE(TAG, "fota_http_get unexpected status: %d", status);
        return -1;
    }
    return (int)hb.used;
}

/* ------------------------------------------------------------------ */
/* fota_http_post_json                                                  */
/* ------------------------------------------------------------------ */
int fota_http_post_json(const char *url,
                        const char *bearer_token,
                        const char *json_body,
                        char       *resp_buf,
                        size_t      resp_buf_size)
{
    if (!url || !json_body || !resp_buf) return -1;

    http_buf_t hb = {
        .buf  = (uint8_t *)resp_buf,
        .size = resp_buf_size,
        .used = 0
    };

    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = 15000,
        .event_handler = http_event_handler,
        .user_data     = &hb,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_http_client_set_header(client, "Content-Type", "application/json");

    if (bearer_token && bearer_token[0]) {
        char auth[512];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer_token);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    esp_http_client_set_post_field(client, json_body, (int)strlen(json_body));

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGE(TAG, "fota_http_post_json failed: %s, status=%d",
                 esp_err_to_name(err), status);
        return -1;
    }

    /* Null-terminate response */
    if (hb.used < resp_buf_size) {
        resp_buf[hb.used] = '\0';
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* fota_sha256                                                          */
/* ------------------------------------------------------------------ */
void fota_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    /*
     * mbedtls_sha256 is hardware-accelerated on ESP32-C3 and ESP32-S3
     * via the SHA co-processor.  No extra configuration required.
     */
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0 /* is224=0 → SHA-256 */);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

/* ------------------------------------------------------------------ */
/* fota_ecdsa_verify                                                    */
/* ------------------------------------------------------------------ */
int fota_ecdsa_verify(const uint8_t *data, size_t data_len,
                      const char    *sig_b64)
{
    if (!data || !sig_b64) return 0;

    /* 1. SHA-256 digest */
    uint8_t hash[32];
    fota_sha256(data, data_len, hash);

    /* 2. Base64-decode signature → DER bytes */
    uint8_t sig_der[128];
    size_t  sig_len = 0;

    /* Server sends base64url; mbedTLS wants standard base64. */
    char sig_std[192];
    int sig_std_len = fota_b64url_to_b64(sig_b64, sig_std, sizeof(sig_std));
    if (sig_std_len < 0) return 0;

    int rc = mbedtls_base64_decode(sig_der, sizeof(sig_der), &sig_len,
                                   (const uint8_t *)sig_std,
                                   (size_t)sig_std_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "fota_ecdsa_verify: base64 decode failed: -0x%04x", -rc);
        return 0;
    }

    /* 3. Load public key */
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    rc = mbedtls_pk_parse_public_key(&pk,
             (const uint8_t *)SERVER_PUBKEY_PEM,
             strlen(SERVER_PUBKEY_PEM) + 1);
    if (rc != 0) {
        char errbuf[128];
        mbedtls_strerror(rc, errbuf, sizeof(errbuf));
        ESP_LOGE(TAG, "fota_ecdsa_verify: pk_parse_public_key failed: %s", errbuf);
        mbedtls_pk_free(&pk);
        return 0;
    }

    /* 4. Verify ECDSA signature */
    rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256,
                           hash, sizeof(hash),
                           sig_der, sig_len);
    mbedtls_pk_free(&pk);

    if (rc != 0) {
        ESP_LOGE(TAG, "fota_ecdsa_verify: signature verification FAILED: -0x%04x", -rc);
        return 0;
    }

    ESP_LOGI(TAG, "fota_ecdsa_verify: signature OK");
    return 1;
}
