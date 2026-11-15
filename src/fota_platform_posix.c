/**
 * @file fota_platform_posix.c
 * @brief POSIX reference implementation of the FOTA platform seam.
 *
 * Dependencies:
 *   - libcurl   (HTTP/HTTPS)
 *   - OpenSSL   (SHA-256, ECDSA P-256)
 *
 * Build example:
 *   gcc -std=c99 fota_platform_posix.c -lcurl -lssl -lcrypto -o fota_platform.o -c
 *
 * Environment variables:
 *   FOTA_PUBLIC_KEY_PEM  — path to a PEM file containing the EC P-256
 *                          public key used to verify firmware signatures.
 *                          If unset, the compiled-in TEST_PUBLIC_KEY_PEM
 *                          placeholder is used (development only).
 */

#define _POSIX_C_SOURCE 200809L  /* for strndup, etc. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <ctype.h>
#include <strings.h>

#include <curl/curl.h>

#include <openssl/evp.h>
#include <bzlib.h>
#include "bspatch.h"
#include "fota_hal.h"
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/err.h>

#include "../include/fota_platform.h"

/* ------------------------------------------------------------------ */
/* Internal: test / placeholder public key (P-256)                     */
/* Replace with your real production public key before shipping.       */
/* ------------------------------------------------------------------ */
static const char *TEST_PUBLIC_KEY_PEM =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEREPLABCDEFGHIJKLMNOPQRSTUVWX\n"
    "YZabcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ==\n"
    "-----END PUBLIC KEY-----\n";

static EVP_PKEY *load_public_key_from_env_or_default(void)
{
    const char *pem_source = getenv("FOTA_PUBLIC_KEY_PEM");
    EVP_PKEY *pkey = NULL;
    BIO *bio = NULL;

    if (pem_source && pem_source[0] != '\0') {
        if (strstr(pem_source, "-----BEGIN PUBLIC KEY-----") != NULL) {
            bio = BIO_new_mem_buf(pem_source, (int)strlen(pem_source));
            if (!bio) {
                fprintf(stderr, "[fota_ecdsa_verify] failed to allocate BIO for FOTA_PUBLIC_KEY_PEM contents\n");
                return NULL;
            }
            pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
            BIO_free(bio);
            return pkey;
        }

        FILE *fp = fopen(pem_source, "r");
        if (!fp) {
            fprintf(stderr, "[fota_ecdsa_verify] cannot open FOTA_PUBLIC_KEY_PEM path: %s\n", pem_source);
            return NULL;
        }
        pkey = PEM_read_PUBKEY(fp, NULL, NULL, NULL);
        fclose(fp);
        return pkey;
    }

    bio = BIO_new_mem_buf(TEST_PUBLIC_KEY_PEM, (int)strlen(TEST_PUBLIC_KEY_PEM));
    if (!bio) {
        fprintf(stderr, "[fota_ecdsa_verify] failed to allocate BIO for built-in test key\n");
        return NULL;
    }
    pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);
    fprintf(stderr,
            "[fota_ecdsa_verify] WARNING: using built-in test public key. "
            "Set FOTA_PUBLIC_KEY_PEM to a PEM file path or PEM contents for real verification.\n");
    return pkey;
}

/* ------------------------------------------------------------------ */
/* Internal: curl write callback                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t *buf;
    size_t   size;      /* allocated size */
    size_t   used;      /* bytes written  */
} curl_buf_t;

typedef struct {
    char   *location;
    size_t  capacity;
} redirect_loc_t;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    curl_buf_t *cb = (curl_buf_t *)userdata;
    size_t incoming = size * nmemb;

    if (cb->used + incoming > cb->size) {
        /* Truncate rather than overflow */
        incoming = cb->size - cb->used;
    }
    if (incoming == 0) {
        return 0; /* signal error to curl (buffer full) */
    }
    memcpy(cb->buf + cb->used, ptr, incoming);
    cb->used += incoming;
    return incoming;
}

static size_t curl_header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    redirect_loc_t *redir = (redirect_loc_t *)userdata;
    size_t incoming = size * nitems;

    if (!redir || !redir->location || redir->capacity == 0 || incoming < 10) {
        return incoming;
    }

    if (strncasecmp(buffer, "Location:", 9) != 0) {
        return incoming;
    }

    const char *value = buffer + 9;
    while (*value == ' ' || *value == '\t') {
        value++;
    }

    size_t len = incoming - (size_t)(value - buffer);
    while (len > 0 && (value[len - 1] == '\r' || value[len - 1] == '\n')) {
        len--;
    }

    if (len >= redir->capacity) {
        len = redir->capacity - 1;
    }

    memcpy(redir->location, value, len);
    redir->location[len] = '\0';
    return incoming;
}

/* ------------------------------------------------------------------ */
/* fota_http_get                                                        */
/* ------------------------------------------------------------------ */
int fota_http_get(const char  *url,
                  const char  *bearer_token,
                  uint32_t     range_start,
                  uint8_t     *out_buf,
                  size_t       buf_size)
{
    curl_buf_t cb;
    int ret = -1;
    char *request_url = NULL;
    int send_auth = 1;
    int redirects_remaining = 5;

    if (!url || !out_buf || buf_size == 0) {
        return -1;
    }

    cb.buf  = out_buf;
    cb.size = buf_size;
    cb.used = 0;

    request_url = strdup(url);
    if (!request_url) {
        fprintf(stderr, "[fota_http_get] out of memory duplicating URL\n");
        return -1;
    }

    while (redirects_remaining-- > 0) {
        CURL *curl = NULL;
        CURLcode res;
        struct curl_slist *headers = NULL;
        char auth_header[600];
        char range_header[64];
        char redirect_location[4096] = {0};
        redirect_loc_t redir = { redirect_location, sizeof(redirect_location) };
        long http_code = 0;
        char *next_url = NULL;
        char *redirect_url = NULL;

        cb.used = 0;

        curl = curl_easy_init();
        if (!curl) {
            fprintf(stderr, "[fota_http_get] curl_easy_init() failed\n");
            break;
        }

        curl_easy_setopt(curl, CURLOPT_URL, request_url);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cb);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &redir);

        if (send_auth && bearer_token && bearer_token[0] != '\0') {
            snprintf(auth_header, sizeof(auth_header),
                     "Authorization: Bearer %s", bearer_token);
            headers = curl_slist_append(headers, auth_header);
        }

        if (range_start > 0) {
            snprintf(range_header, sizeof(range_header),
                     "Range: bytes=%u-", (unsigned int)range_start);
            headers = curl_slist_append(headers, range_header);
        }

        if (headers) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "[fota_http_get] curl error: %s\n",
                    curl_easy_strerror(res));
            if (headers) curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            break;
        }

        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect_url);

        if (http_code >= 300 && http_code < 400) {
            const char *location = NULL;
            if (redirect_location[0] != '\0') {
                location = redirect_location;
            } else if (redirect_url && redirect_url[0] != '\0') {
                location = redirect_url;
            }

            if (!location) {
                fprintf(stderr, "[fota_http_get] redirect response missing Location header\n");
                if (headers) curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                break;
            }

            next_url = strdup(location);
            if (!next_url) {
                fprintf(stderr, "[fota_http_get] out of memory duplicating redirect URL\n");
                if (headers) curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                break;
            }

            free(request_url);
            request_url = next_url;
            send_auth = 0;

            if (headers) curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            continue;
        }

        if (http_code < 200 || http_code >= 300) {
            if (!(range_start > 0 && http_code == 206)) {
                fprintf(stderr, "[fota_http_get] HTTP %ld for %s\n", http_code, request_url);
                if (cb.used > 0) {
                    size_t preview_len = cb.used < 512 ? cb.used : 512;
                    char preview[513];
                    memcpy(preview, cb.buf, preview_len);
                    preview[preview_len] = '\0';
                    fprintf(stderr, "[fota_http_get] response body: %s\n", preview);
                }
                if (headers) curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                break;
            }
        }

        ret = (int)cb.used;
        if (headers) curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        break;
    }

    free(request_url);
    return ret;
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
    CURL *curl = NULL;
    CURLcode res;
    struct curl_slist *headers = NULL;
    curl_buf_t cb;
    char auth_header[600];
    long http_code = 0;
    int ret = -1;

    if (!url || !json_body || !resp_buf || resp_buf_size == 0) {
        return -1;
    }

    cb.buf  = (uint8_t *)resp_buf;
    cb.size = resp_buf_size - 1; /* reserve 1 byte for null terminator */
    cb.used = 0;

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "[fota_http_post_json] curl_easy_init() failed\n");
        return -1;
    }

    /* Always send Content-Type: application/json */
    headers = curl_slist_append(headers, "Content-Type: application/json");

    if (bearer_token && bearer_token[0] != '\0') {
        snprintf(auth_header, sizeof(auth_header),
                 "Authorization: Bearer %s", bearer_token);
        headers = curl_slist_append(headers, auth_header);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cb);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "[fota_http_post_json] curl error: %s\n",
                curl_easy_strerror(res));
        goto cleanup;
    }

    /* Null-terminate response */
    resp_buf[cb.used] = '\0';

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code >= 200 && http_code < 300) {
        ret = 0;
    } else {
        fprintf(stderr, "[fota_http_post_json] HTTP %ld — body: %.256s\n",
                http_code, resp_buf);
    }

cleanup:
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}

/* ------------------------------------------------------------------ */
/* fota_sha256                                                          */
/* ------------------------------------------------------------------ */
void fota_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    /* Use OpenSSL EVP_Digest for FIPS-compatibility */
    unsigned int digest_len = 32;
    EVP_Digest(data, len, out, &digest_len, EVP_sha256(), NULL);
}

/* ------------------------------------------------------------------ */
/* Internal: base64url → standard base64 + decode                      */
/* Returns heap-allocated DER buffer; caller must free().              */
/* Sets *out_len to the number of decoded bytes, or 0 on error.        */
/* ------------------------------------------------------------------ */
static unsigned char *b64url_decode(const char *b64url,
                                    size_t      b64url_len,
                                    size_t     *out_len)
{
    /* Convert base64url alphabet to standard base64 */
    char *b64 = (char *)malloc(b64url_len + 4);
    if (!b64) { *out_len = 0; return NULL; }

    size_t i;
    for (i = 0; i < b64url_len; i++) {
        if      (b64url[i] == '-') b64[i] = '+';
        else if (b64url[i] == '_') b64[i] = '/';
        else                       b64[i] = b64url[i];
    }
    /* Pad to multiple of 4 */
    size_t padded = b64url_len;
    while (padded % 4 != 0) b64[padded++] = '=';
    b64[padded] = '\0';

    BIO *bmem = BIO_new_mem_buf(b64, (int)padded);
    BIO *b64bio = BIO_new(BIO_f_base64());
    BIO_set_flags(b64bio, BIO_FLAGS_BASE64_NO_NL);
    bmem = BIO_push(b64bio, bmem);

    unsigned char *decoded = (unsigned char *)malloc(padded);
    if (!decoded) {
        BIO_free_all(bmem);
        free(b64);
        *out_len = 0;
        return NULL;
    }

    int n = BIO_read(bmem, decoded, (int)padded);
    BIO_free_all(bmem);
    free(b64);

    if (n <= 0) {
        free(decoded);
        *out_len = 0;
        return NULL;
    }
    *out_len = (size_t)n;
    return decoded;
}

/* ------------------------------------------------------------------ */
/* fota_ecdsa_verify                                                    */
/* ------------------------------------------------------------------ */
int fota_ecdsa_verify(const uint8_t *data,
                      size_t         data_len,
                      const char    *sig_b64)
{
    EVP_PKEY       *pkey     = NULL;
    EVP_MD_CTX     *mdctx    = NULL;
    unsigned char  *sig_der  = NULL;
    size_t          sig_len  = 0;
    int             ret      = -1;

    if (!data || !sig_b64) {
        return -1;
    }

    /* --- Load public key --- */
    pkey = load_public_key_from_env_or_default();

    if (!pkey) {
        fprintf(stderr,
                "[fota_ecdsa_verify] failed to load public key. "
                "Provide FOTA_PUBLIC_KEY_PEM as a PEM file path or PEM contents.\n");
        ERR_print_errors_fp(stderr);
        return -1;
    }

    /* --- Decode base64url DER signature --- */
    sig_der = b64url_decode(sig_b64, strlen(sig_b64), &sig_len);
    if (!sig_der) {
        fprintf(stderr, "[fota_ecdsa_verify] base64url decode failed\n");
        EVP_PKEY_free(pkey);
        return -1;
    }

    /* --- Verify with EVP (SHA-256 + ECDSA) --- */
    mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        ret = -1;
        goto cleanup;
    }

    if (EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, pkey) != 1) {
        fprintf(stderr, "[fota_ecdsa_verify] EVP_DigestVerifyInit failed\n");
        goto cleanup;
    }

    if (EVP_DigestVerifyUpdate(mdctx, data, data_len) != 1) {
        fprintf(stderr, "[fota_ecdsa_verify] EVP_DigestVerifyUpdate failed\n");
        goto cleanup;
    }

    int verify_result = EVP_DigestVerifyFinal(mdctx, sig_der, sig_len);
    if (verify_result == 1) {
        ret = 1; /* valid */
    } else if (verify_result == 0) {
        ret = 0; /* invalid signature */
    } else {
        ret = -1; /* internal error */
        ERR_print_errors_fp(stderr);
    }

cleanup:
    if (mdctx)   EVP_MD_CTX_free(mdctx);
    if (sig_der) free(sig_der);
    if (pkey)    EVP_PKEY_free(pkey);
    return ret;
}

/* ------------------------------------------------------------------ */
/* fota_bspatch — internal helpers                                      */
/* ------------------------------------------------------------------ */

static int64_t _offtin(const uint8_t *buf)
{
    int64_t y = (int64_t)(buf[7] & 0x7F);
    y = y*256; y += buf[6];
    y = y*256; y += buf[5];
    y = y*256; y += buf[4];
    y = y*256; y += buf[3];
    y = y*256; y += buf[2];
    y = y*256; y += buf[1];
    y = y*256; y += buf[0];
    if (buf[7] & 0x80) y = -y;
    return y;
}

/**
 * Apply a bsdiff40 patch using three independent buffer streams
 * (ctrl, diff, extra).  This matches the original Colin Percival
 * bsdiff algorithm without requiring mendsley's interleaved stream.
 */
static int _apply_bsdiff(const uint8_t *old_buf, int64_t old_size,
                          uint8_t *new_buf, int64_t new_size,
                          const uint8_t *ctrl_blk,  size_t ctrl_len,
                          const uint8_t *diff_blk,  size_t diff_len,
                          const uint8_t *extra_blk, size_t extra_len)
{
    size_t ctrl_pos = 0, diff_pos = 0, extra_pos = 0;
    int64_t old_pos = 0, new_pos = 0;
    uint8_t buf[8];

    /* bsdiff ctrl triple order: (x=diff_len, y=extra_len, z=old_seek)
     * Ref: Colin Percival bspatch.c — ctrl[0]=diff, ctrl[1]=extra, ctrl[2]=seek */
    while (new_pos < new_size) {
        int64_t ctrl[3];
        for (int i = 0; i < 3; i++) {
            if (ctrl_pos + 8 > ctrl_len) return -1;
            memcpy(buf, ctrl_blk + ctrl_pos, 8);
            ctrl_pos += 8;
            ctrl[i] = _offtin(buf);
        }

        /* ctrl[0]: add diff bytes to old bytes */
        if (new_pos + ctrl[0] > new_size) return -1;
        for (int64_t i = 0; i < ctrl[0]; i++) {
            if (diff_pos >= diff_len) return -1;
            uint8_t d = diff_blk[diff_pos++];
            uint8_t o = (old_pos + i >= 0 && old_pos + i < old_size)
                        ? old_buf[old_pos + i] : 0;
            new_buf[new_pos + i] = (uint8_t)(d + o);
        }
        new_pos += ctrl[0];
        old_pos += ctrl[0];

        /* ctrl[1]: copy extra bytes verbatim */
        if (ctrl[1] > 0) {
            if (new_pos + ctrl[1] > new_size) return -1;
            if (extra_pos + (size_t)ctrl[1] > extra_len) return -1;
            memcpy(new_buf + new_pos, extra_blk + extra_pos, (size_t)ctrl[1]);
            extra_pos += (size_t)ctrl[1];
            new_pos   += ctrl[1];
        }

        /* ctrl[2]: seek in old (signed) */
        old_pos += ctrl[2];
    }
    (void)diff_pos; /* may not consume all diff bytes — that's OK */
    return 0;
}

/**
 * Apply a bsdiff delta patch.
 *
 * Reads the active firmware slot in full, applies the patch in memory,
 * erases the inactive slot, then writes the result.
 *
 * @param delta      Delta payload in memory.
 * @param delta_len  Size of the delta payload.
 * @return Patched image size (> 0) on success, -1 on failure.
 */
int fota_bspatch(const uint8_t *delta, size_t delta_len)
{
    uint32_t part_size = hal_flash_partition_size(FOTA_SLOT_ACTIVE);

    /* Read active slot into memory */
    uint8_t *old_buf = (uint8_t *)malloc(part_size);
    if (!old_buf) {
        fprintf(stderr, "[fota_bspatch] OOM allocating old_buf (%u bytes)\n",
                (unsigned)part_size);
        return -1;
    }
    if (hal_flash_read(FOTA_SLOT_ACTIVE, 0, old_buf, part_size) != FOTA_HAL_OK) {
        fprintf(stderr, "[fota_bspatch] failed to read active slot\n");
        free(old_buf);
        return -1;
    }

    /* Allocate output buffer (same size as partition) */
    uint8_t *new_buf = (uint8_t *)malloc(part_size);
    if (!new_buf) {
        fprintf(stderr, "[fota_bspatch] OOM allocating new_buf\n");
        free(old_buf);
        return -1;
    }

    /* ---- Parse BSDIFF40 header (standard bsdiff4 format with bzip2) ----
     *   [0:8]   magic      "BSDIFF40"
     *   [8:16]  ctrl_bz2_len  int64 LE  (compressed ctrl block size)
     *   [16:24] diff_bz2_len  int64 LE  (compressed diff block size)
     *   [24:32] new_size      int64 LE
     *   [32:]   bzip2(ctrl) || bzip2(diff) || bzip2(extra)
     */
    if (delta_len < 32 || memcmp(delta, "BSDIFF40", 8) != 0) {
        fprintf(stderr, "[fota_bspatch] unrecognised format (expected BSDIFF40)\n");
        free(old_buf); free(new_buf);
        return -1;
    }
    int64_t ctrl_bz2_len = 0, diff_bz2_len = 0, new_size = 0;
    for (int i = 0; i < 8; i++) {
        ctrl_bz2_len |= (int64_t)delta[ 8 + i] << (8 * i);
        diff_bz2_len |= (int64_t)delta[16 + i] << (8 * i);
        new_size     |= (int64_t)delta[24 + i] << (8 * i);
    }
    if (new_size <= 0 || (uint64_t)new_size > part_size) {
        fprintf(stderr, "[fota_bspatch] unreasonable new_size %lld\n", (long long)new_size);
        free(old_buf); free(new_buf);
        return -1;
    }

    /* Decompress ctrl, diff, extra blocks independently via bzip2.
     * Upper bounds: ctrl ≤ new_size/8*24+1024, diff ≤ new_size, extra ≤ new_size */
    unsigned int ctrl_out  = (unsigned int)(new_size / 8 * 24 + 4096);
    unsigned int diff_out  = (unsigned int)(new_size + 1);
    unsigned int extra_out = (unsigned int)(new_size + 1);

    uint8_t *ctrl_raw  = (uint8_t *)malloc(ctrl_out);
    uint8_t *diff_raw  = (uint8_t *)malloc(diff_out);
    uint8_t *extra_raw = (uint8_t *)malloc(extra_out);
    if (!ctrl_raw || !diff_raw || !extra_raw) {
        fprintf(stderr, "[fota_bspatch] OOM for decompression buffers\n");
        free(ctrl_raw); free(diff_raw); free(extra_raw);
        free(old_buf); free(new_buf);
        return -1;
    }

    const uint8_t *p = delta + 32;
    size_t extra_bz2_len = delta_len - 32 - (size_t)ctrl_bz2_len - (size_t)diff_bz2_len;

    if (BZ2_bzBuffToBuffDecompress((char *)ctrl_raw,  &ctrl_out,
                                   (char *)p, (unsigned int)ctrl_bz2_len, 0, 0) != BZ_OK ||
        BZ2_bzBuffToBuffDecompress((char *)diff_raw,  &diff_out,
                                   (char *)(p + ctrl_bz2_len),
                                   (unsigned int)diff_bz2_len, 0, 0) != BZ_OK ||
        BZ2_bzBuffToBuffDecompress((char *)extra_raw, &extra_out,
                                   (char *)(p + ctrl_bz2_len + diff_bz2_len),
                                   (unsigned int)extra_bz2_len, 0, 0) != BZ_OK) {
        fprintf(stderr, "[fota_bspatch] bzip2 decompression failed\n");
        free(ctrl_raw); free(diff_raw); free(extra_raw);
        free(old_buf); free(new_buf);
        return -1;
    }

    /* Apply patch using three independent streams (ctrl, diff, extra) */
    int bsp_ret = _apply_bsdiff(old_buf, (int64_t)part_size,
                                 new_buf, new_size,
                                 ctrl_raw, ctrl_out,
                                 diff_raw, diff_out,
                                 extra_raw, extra_out);
    free(ctrl_raw); free(diff_raw); free(extra_raw);
    if (bsp_ret != 0) {
        fprintf(stderr, "[fota_bspatch] _apply_bsdiff() failed\n");
        free(old_buf); free(new_buf);
        return -1;
    }
    free(old_buf);

    /* Erase inactive slot and write patched firmware */
    uint32_t sector = hal_flash_sector_size();
    uint32_t erase_len = (uint32_t)(((size_t)new_size + sector - 1) / sector * sector);
    if (hal_flash_erase(FOTA_SLOT_INACTIVE, 0, erase_len) != FOTA_HAL_OK) {
        fprintf(stderr, "[fota_bspatch] failed to erase inactive slot\n");
        free(new_buf);
        return -1;
    }
    if (hal_flash_write(FOTA_SLOT_INACTIVE, 0,
                        new_buf, (size_t)new_size) != FOTA_HAL_OK) {
        fprintf(stderr, "[fota_bspatch] failed to write inactive slot\n");
        free(new_buf);
        return -1;
    }

    free(new_buf);
    fprintf(stdout, "[fota_bspatch] patch applied: %lld bytes → inactive slot\n",
            (long long)new_size);
    /* Positive return = patched image size, so the FSM verifies exactly it. */
    return (new_size > 0x7FFFFFFF) ? 0 : (int)new_size;
}

/* JSON helpers and fota_register() live in fota_common.c (shared by all ports). */
