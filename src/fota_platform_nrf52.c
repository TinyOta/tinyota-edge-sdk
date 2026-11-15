/**
 * @file fota_platform_nrf52.c
 * @brief FOTA platform seam implementation for nRF52840 (Zephyr RTOS)
 *
 * Implements:
 *   fota_http_get()       — via Zephyr HTTP client over TLS (net_app / zsock)
 *   fota_http_post_json() — via Zephyr HTTP client
 *   fota_sha256()         — via Zephyr mbedTLS (CryptoCell-310 HW accelerated)
 *   fota_ecdsa_verify()   — via Zephyr mbedTLS
 *
 * Network dependencies:
 *   nRF52840 has no on-chip IP stack.  This file assumes one of:
 *     (A) nRF7002 Wi-Fi companion (nRF Connect SDK >= 2.5)
 *     (B) UART-AT modem (SIM7080G, ESP32 AT firmware, etc.)
 *         with Zephyr's CONFIG_MODEM_GSM_PPP or similar.
 *
 * Zephyr Kconfig requirements:
 *   CONFIG_NETWORKING=y
 *   CONFIG_NET_TCP=y
 *   CONFIG_NET_SOCKETS=y
 *   CONFIG_NET_SOCKETS_POSIX_NAMES=y
 *   CONFIG_MBEDTLS=y
 *   CONFIG_MBEDTLS_BUILTIN=y
 *   CONFIG_NET_SOCKETS_ENABLE_DTLS=y  (if using DTLS)
 *   CONFIG_HTTP_CLIENT=y
 *
 * Add to CMakeLists.txt:
 *   target_sources(app PRIVATE fota_platform_nrf52.c)
 */

#include "../include/fota_platform.h"

#include <zephyr/kernel.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/logging/log.h>

#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/base64.h>
#include <mbedtls/error.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

LOG_MODULE_REGISTER(fota_platform, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* TLS credential tag — register your CA cert at this tag              */
/* See: tls_credential_add(FOTA_TLS_TAG, ...)                         */
/* ------------------------------------------------------------------ */
#define FOTA_TLS_TAG  42

/* Hosted API default port */
#define FOTA_HTTPS_PORT  "443"

/* ------------------------------------------------------------------ */
/* Server public key — replace before production release               */
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
/* Internal: response buffer accumulator for http_client callback      */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t *buf;
    size_t   size;
    size_t   used;
} resp_buf_t;

static void http_response_cb(struct http_response *rsp,
                              enum http_final_call final,
                              void *user_data)
{
    resp_buf_t *rb = (resp_buf_t *)user_data;
    if (rb && rsp->body_found && rsp->body_frag_len > 0) {
        size_t room = rb->size - rb->used;
        size_t copy = (rsp->body_frag_len < room) ? rsp->body_frag_len : room;
        memcpy(rb->buf + rb->used, rsp->body_frag_start, copy);
        rb->used += copy;
    }
}

/* ------------------------------------------------------------------ */
/* Internal: parse host and path from a URL                            */
/* e.g. "https://api.example.com/api/v1/path" → host / path           */
/* ------------------------------------------------------------------ */
static int parse_url(const char *url, char *host, size_t host_sz,
                     char *path, size_t path_sz)
{
    /* Skip scheme */
    const char *p = strstr(url, "://");
    if (!p) return -1;
    p += 3;

    /* Host ends at next '/' */
    const char *slash = strchr(p, '/');
    size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hlen >= host_sz) return -1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    strncpy(path, slash ? slash : "/", path_sz - 1);
    path[path_sz - 1] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* Internal: open a TLS socket to host:port                            */
/* ------------------------------------------------------------------ */
static int open_tls_socket(const char *host, const char *port)
{
    struct zsock_addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct zsock_addrinfo *res = NULL;
    int rc = zsock_getaddrinfo(host, port, &hints, &res);
    if (rc || !res) {
        LOG_ERR("getaddrinfo(%s) failed: %d", host, rc);
        return -1;
    }

    sec_tag_t sec_tags[] = { FOTA_TLS_TAG };
    int fd = zsock_socket(res->ai_family, SOCK_STREAM, IPPROTO_TLS_1_2);
    if (fd < 0) {
        LOG_ERR("socket() failed: %d", errno);
        zsock_freeaddrinfo(res);
        return -1;
    }

    rc = zsock_setsockopt(fd, SOL_TLS, TLS_SEC_TAG_LIST,
                          sec_tags, sizeof(sec_tags));
    if (rc) {
        LOG_ERR("setsockopt TLS_SEC_TAG_LIST: %d", errno);
    }

    rc = zsock_setsockopt(fd, SOL_TLS, TLS_HOSTNAME,
                          host, strlen(host));
    if (rc) {
        LOG_ERR("setsockopt TLS_HOSTNAME: %d", errno);
    }

    if (zsock_connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        LOG_ERR("connect(%s:%s) failed: %d", host, port, errno);
        zsock_close(fd);
        zsock_freeaddrinfo(res);
        return -1;
    }

    zsock_freeaddrinfo(res);
    return fd;
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

    char host[128] = {0};
    char path[256] = {0};
    if (parse_url(url, host, sizeof(host), path, sizeof(path))) return -1;

    int fd = open_tls_socket(host, FOTA_HTTPS_PORT);
    if (fd < 0) return -1;

    /* Build extra headers */
    char extra_headers[512] = {0};
    int hlen = 0;
    if (bearer_token && bearer_token[0]) {
        hlen += snprintf(extra_headers + hlen, sizeof(extra_headers) - hlen,
                         "Authorization: Bearer %s\r\n", bearer_token);
    }
    if (range_start > 0) {
        hlen += snprintf(extra_headers + hlen, sizeof(extra_headers) - hlen,
                         "Range: bytes=%u-\r\n", (unsigned)range_start);
    }

    resp_buf_t rb = { .buf = out_buf, .size = buf_size, .used = 0 };
    uint8_t recv_buf[1024];

    struct http_request req = {
        .method       = HTTP_GET,
        .url          = path,
        .host         = host,
        .protocol     = "HTTP/1.1",
        .response     = http_response_cb,
        .user_data    = &rb,
        .recv_buf     = recv_buf,
        .recv_buf_len = sizeof(recv_buf),
    };

    if (hlen > 0) {
        req.optional_headers = extra_headers;
    }

    int rc = http_client_req(fd, &req, 30000 /* ms */, NULL);
    zsock_close(fd);

    if (rc < 0) {
        LOG_ERR("fota_http_get: http_client_req failed: %d", rc);
        return -1;
    }
    return (int)rb.used;
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

    char host[128] = {0};
    char path[256] = {0};
    if (parse_url(url, host, sizeof(host), path, sizeof(path))) return -1;

    int fd = open_tls_socket(host, FOTA_HTTPS_PORT);
    if (fd < 0) return -1;

    char auth_header[520] = {0};
    if (bearer_token && bearer_token[0]) {
        snprintf(auth_header, sizeof(auth_header),
                 "Authorization: Bearer %s\r\n", bearer_token);
    }

    resp_buf_t rb = {
        .buf  = (uint8_t *)resp_buf,
        .size = resp_buf_size,
        .used = 0
    };
    uint8_t recv_buf[512];

    const char *content_type_hdr = "Content-Type: application/json\r\n";

    struct http_request req = {
        .method          = HTTP_POST,
        .url             = path,
        .host            = host,
        .protocol        = "HTTP/1.1",
        .payload         = json_body,
        .payload_len     = strlen(json_body),
        .content_type_id = 0,   /* use optional_headers instead */
        .response        = http_response_cb,
        .user_data       = &rb,
        .recv_buf        = recv_buf,
        .recv_buf_len    = sizeof(recv_buf),
        .optional_headers = (strlen(auth_header) > 0) ? auth_header : NULL,
    };

    /* Prepend content-type; for simplicity we build a combined header str */
    char combined[600];
    snprintf(combined, sizeof(combined), "%s%s", content_type_hdr, auth_header);
    req.optional_headers = combined;

    int rc = http_client_req(fd, &req, 15000, NULL);
    zsock_close(fd);

    if (rc < 0) {
        LOG_ERR("fota_http_post_json: http_client_req failed: %d", rc);
        return -1;
    }

    /* Null-terminate */
    if (rb.used < resp_buf_size) {
        resp_buf[rb.used] = '\0';
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* fota_sha256                                                          */
/* ------------------------------------------------------------------ */
void fota_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    /*
     * On nRF52840, mbedTLS SHA-256 is accelerated by CryptoCell-310
     * when CONFIG_MBEDTLS_ENABLE_HEAP=y and nrf_cc310 is linked.
     */
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
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

    uint8_t hash[32];
    fota_sha256(data, data_len, hash);

    /* Base64-decode DER signature */
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
        LOG_ERR("fota_ecdsa_verify: base64 decode error: -0x%04x", -rc);
        return 0;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    rc = mbedtls_pk_parse_public_key(&pk,
             (const uint8_t *)SERVER_PUBKEY_PEM,
             strlen(SERVER_PUBKEY_PEM) + 1);
    if (rc != 0) {
        char errbuf[128];
        mbedtls_strerror(rc, errbuf, sizeof(errbuf));
        LOG_ERR("fota_ecdsa_verify: pk_parse_public_key failed: %s", errbuf);
        mbedtls_pk_free(&pk);
        return 0;
    }

    rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256,
                           hash, sizeof(hash),
                           sig_der, sig_len);
    mbedtls_pk_free(&pk);

    if (rc != 0) {
        LOG_ERR("fota_ecdsa_verify: FAILED -0x%04x", -rc);
        return 0;
    }

    LOG_INF("fota_ecdsa_verify: signature OK");
    return 1;
}
