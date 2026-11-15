/**
 * @file fota_common.c
 * @brief Portable helpers shared by every platform port (C99, no OS deps).
 *
 * Contents:
 *   - fota_json_get_string / _int / _bool  (flat JSON field extraction)
 *   - fota_register()       POST /api/v1/devices/register
 *   - fota_report_status()  POST /api/v1/devices/status
 *   - weak fota_bspatch()   FULL-only default for ports without delta support
 *
 * These only depend on fota_http_post_json(), which each port implements.
 * Before this file existed the helpers lived in fota_platform_posix.c, so the
 * ESP32 and nRF52 ports could not link.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <ctype.h>

#include "fota_platform.h"

#ifdef FOTA_ENABLE_STDIO
#define FOTA_LOGE(...) fprintf(stderr, __VA_ARGS__)
#else
#define FOTA_LOGE(...) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Internal JSON helpers                                                */
/* ------------------------------------------------------------------ */

/**
 * Locate  "key":  (with optional whitespace after colon) inside @p json.
 * Returns pointer to the character immediately after the colon+spaces,
 * or NULL if not found.
 */
static const char *json_find_value_start(const char *json, const char *key)
{
    if (!json || !key) return NULL;

    /* Build search pattern  "key":  */
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    size_t key_len = strlen(pattern);

    const char *p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        const char *after_key = p + key_len;
        /* Skip whitespace */
        while (*after_key == ' ' || *after_key == '\t') after_key++;
        if (*after_key == ':') {
            after_key++;  /* step over colon */
            /* Skip more whitespace */
            while (*after_key == ' ' || *after_key == '\t') after_key++;
            return after_key;
        }
        p++;  /* keep searching in case this was a prefix match */
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* fota_json_get_string                                                 */
/* ------------------------------------------------------------------ */
int fota_json_get_string(const char *json,
                         const char *key,
                         char       *out,
                         size_t      out_size)
{
    const char *v = json_find_value_start(json, key);
    if (!v || *v != '"') return -1;

    v++; /* skip opening quote */
    size_t i = 0;
    while (*v != '\0' && *v != '"' && i < out_size - 1) {
        /* Handle simple escape sequences */
        if (*v == '\\' && *(v + 1) != '\0') {
            v++;
            switch (*v) {
                case 'n':  out[i++] = '\n'; break;
                case 't':  out[i++] = '\t'; break;
                case 'r':  out[i++] = '\r'; break;
                case '"':  out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                default:   out[i++] = *v;   break;
            }
        } else {
            out[i++] = *v;
        }
        v++;
    }
    if (*v != '"') return -1; /* unterminated string or truncated */
    out[i] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* fota_json_get_int                                                    */
/* ------------------------------------------------------------------ */
int fota_json_get_int(const char *json, const char *key, int *out)
{
    const char *v = json_find_value_start(json, key);
    if (!v) return -1;

    /* Accept optional minus sign */
    if (*v != '-' && !isdigit((unsigned char)*v)) return -1;

    char *end;
    long val = strtol(v, &end, 10);
    if (end == v) return -1; /* no digits consumed */

    *out = (int)val;
    return 0;
}

/* ------------------------------------------------------------------ */
/* fota_json_get_bool                                                   */
/* ------------------------------------------------------------------ */
int fota_json_get_bool(const char *json, const char *key, int *out)
{
    const char *v = json_find_value_start(json, key);
    if (!v) return -1;

    if (strncmp(v, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if (strncmp(v, "false", 5) == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* fota_register                                                        */
/* ------------------------------------------------------------------ */
int fota_register(const char *server_base_url,
                  const char *registration_token,
                  const char *serial,
                  char       *out_token,
                  size_t      out_token_size)
{
    char url[512];
    char body[512];
    char resp[2048];
    int ret;

    if (!server_base_url || !registration_token || !serial ||
        !out_token || out_token_size < 2) {
        return -1;
    }

    /* Build endpoint URL */
    snprintf(url, sizeof(url),
             "%s/api/v1/devices/register", server_base_url);

    /* Build JSON body */
    snprintf(body, sizeof(body),
             "{\"registration_token\":\"%s\",\"unit_serial\":\"%s\"}",
             registration_token, serial);

    /* POST — no bearer token required at registration time */
    ret = fota_http_post_json(url, NULL, body, resp, sizeof(resp));
    if (ret != 0) {
        FOTA_LOGE("[fota_register] POST to %s failed\n", url);
        return -1;
    }

    /* Extract access_token from response */
    ret = fota_json_get_string(resp, "access_token", out_token, out_token_size);
    if (ret != 0) {
        FOTA_LOGE("[fota_register] 'access_token' not found in response: %.256s\n",
                  resp);
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* fota_b64url_to_b64                                                   */
/* ------------------------------------------------------------------ */
int fota_b64url_to_b64(const char *in, char *out, size_t out_size)
{
    size_t i, n;
    if (!in || !out) return -1;
    n = strlen(in);
    /* worst case: n chars + up to 3 '=' + NUL */
    if (out_size < n + 4) return -1;
    for (i = 0; i < n; i++) {
        char c = in[i];
        out[i] = (c == '-') ? '+' : (c == '_') ? '/' : c;
    }
    while (i % 4 != 0) out[i++] = '=';
    out[i] = '\0';
    return (int)i;
}

/* ------------------------------------------------------------------ */
/* fota_report_status                                                   */
/* ------------------------------------------------------------------ */

/* Copy @p in into @p out, dropping characters that would break a JSON string. */
static void _json_safe_copy(char *out, size_t out_size, const char *in)
{
    size_t i = 0;
    if (out_size == 0) return;
    if (in) {
        for (; *in != '\0' && i < out_size - 1; in++) {
            unsigned char c = (unsigned char)*in;
            if (c == '"' || c == '\\' || c < 0x20) continue;
            out[i++] = (char)c;
        }
    }
    out[i] = '\0';
}

int fota_report_status(const char *server_base_url,
                       const char *device_token,
                       const char *status,
                       const char *version,
                       const char *fsm_state,
                       int         error_code,
                       const char *message)
{
    char url[512];
    char body[512];
    char resp[256];
    char s_status[32], s_version[48], s_state[16], s_msg[160];

    if (!server_base_url || !device_token || !status || !version) {
        return -1;
    }

    _json_safe_copy(s_status, sizeof(s_status), status);
    _json_safe_copy(s_version, sizeof(s_version), version);
    _json_safe_copy(s_state, sizeof(s_state), fsm_state ? fsm_state : "");
    _json_safe_copy(s_msg, sizeof(s_msg), message ? message : "");

    snprintf(url, sizeof(url), "%s/api/v1/devices/status", server_base_url);
    snprintf(body, sizeof(body),
             "{\"status\":\"%s\",\"version\":\"%s\",\"fsm_state\":\"%s\","
             "\"error_code\":%d,\"message\":\"%s\"}",
             s_status, s_version, s_state, error_code, s_msg);

    return fota_http_post_json(url, device_token, body, resp, sizeof(resp)) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Default fota_bspatch (FULL-only ports)                               */
/* ------------------------------------------------------------------ */

/*
 * Ports that support delta updates (the POSIX reference port) provide a
 * strong fota_bspatch().  Everyone else gets this weak default, so a port
 * links without delta support.  Devices on such ports should report
 * ram_free_kb = 0 in fota_config_t so the server always sends FULL payloads.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
int fota_bspatch(const uint8_t *delta, size_t delta_len)
{
    (void)delta;
    (void)delta_len;
    return -1;
}
