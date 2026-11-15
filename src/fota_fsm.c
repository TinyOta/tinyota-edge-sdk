/**
 * @file fota_fsm.c
 * @brief FOTA Agent FSM — Reference Stub Implementation (C99)
 *
 * This file provides the FSM skeleton.  HTTP, delta patching, and
 * cryptographic verification are called through well-defined seams so
 * manufacturers can replace each subsystem independently.
 *
 * Build dependencies (stubbed here — link your own implementations):
 *   fota_http_get()          — HTTP GET with Range header support.
 *   fota_http_post_json()    — HTTP POST (JSON body → JSON response).
 *   fota_bspatch()           — Apply a bsdiff4 delta patch.
 *   fota_ecdsa_verify()      — Verify ECDSA P-256 / SHA-256 signature.
 *   fota_sha256()            — Compute SHA-256 over a buffer.
 *   hal_flash_*()            — From fota_hal.h.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "fota_fsm.h"
#include "fota_hal.h"
#include "fota_platform.h"

/* ------------------------------------------------------------------ */
/* Forward declarations of platform seams (implement in your port)     */
/* ------------------------------------------------------------------ */

/**
 * Perform HTTP GET (with optional Range header) and write response body
 * to @p out_buf.  If @p range_start > 0 the request includes
 * "Range: bytes=<range_start>-".
 *
 * @return Number of bytes received, or negative on error.
 */
extern int fota_http_get(const char *url,
                         const char *bearer_token,
                         uint32_t range_start,
                         uint8_t *out_buf,
                         size_t buf_size);

/**
 * POST JSON body to @p url, placing the response into @p resp_buf.
 * @return 0 on success (HTTP 2xx), negative on network/HTTP error.
 */
extern int fota_http_post_json(const char *url,
                               const char *bearer_token,
                               const char *json_body,
                               char *resp_buf,
                               size_t resp_buf_size);

/**
 * Apply a bsdiff4 delta patch.
 * Reads source data from FOTA_SLOT_ACTIVE via hal_flash_read(),
 * writes patched data to FOTA_SLOT_INACTIVE via hal_flash_write().
 * @return 0 on success, negative on failure.
 */
extern int fota_bspatch(const uint8_t *delta, size_t delta_len);

/**
 * Verify ECDSA P-256 / SHA-256 signature.
 * @param data        Binary data that was signed.
 * @param data_len    Length of @p data.
 * @param sig_b64     base64url-encoded DER signature string.
 * @return 1 if valid, 0 if invalid, negative on error.
 */
extern int fota_ecdsa_verify(const uint8_t *data, size_t data_len,
                              const char *sig_b64);

/**
 * Compute SHA-256 digest of @p data.
 * @param out  Must point to a 32-byte buffer.
 */
extern void fota_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* ------------------------------------------------------------------ */
/* Internal FSM context                                                 */
/* ------------------------------------------------------------------ */

struct fota_ctx {
    fota_config_t    cfg;
    fota_state_t     state;
    fota_error_t     last_error;
    fota_update_info_t update_info;
    int              update_info_valid;

    /* Download buffer — allocated during DOWNLOADING, freed after PATCHING */
    uint8_t         *download_buf;
    size_t           download_buf_size;
    uint32_t         bytes_received;

    /* Size of the image written to the inactive slot (set in PATCHING). */
    uint32_t         image_size;
};

/*
 * Development escape hatch.  Production builds MUST leave this undefined:
 * without it, an update with no checksum or no signature is rejected.
 */
#ifdef FOTA_ALLOW_UNSIGNED
#define FOTA_REQUIRE_INTEGRITY 0
#else
#define FOTA_REQUIRE_INTEGRITY 1
#endif

static uint32_t _aligned_erase_len(uint32_t len)
{
    uint32_t sector = hal_flash_sector_size();
    if (sector == 0) {
        sector = 4096U;
    }
    if (len == 0) {
        return sector;
    }
    return (uint32_t)((len + sector - 1U) / sector * sector);
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static const char *_state_name(fota_state_t st)
{
    switch (st) {
    case FOTA_STATE_IDLE:        return "IDLE";
    case FOTA_STATE_CHECKING:    return "CHECKING";
    case FOTA_STATE_DOWNLOADING: return "DOWNLOADING";
    case FOTA_STATE_PATCHING:    return "PATCHING";
    case FOTA_STATE_VERIFYING:   return "VERIFYING";
    case FOTA_STATE_SWITCHING:   return "SWITCHING";
    case FOTA_STATE_REBOOT:      return "REBOOT";
    default:                     return "ERROR";
    }
}

/* Compare a raw 32-byte digest with a 64-char hex string (any case). */
static int _digest_matches_hex(const uint8_t digest[32], const char *hex)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    if (!hex || strlen(hex) != 64) return 0;
    for (i = 0; i < 32; i++) {
        char hi = hex[2 * i], lo = hex[2 * i + 1];
        if (hi >= 'A' && hi <= 'F') hi = (char)(hi - 'A' + 'a');
        if (lo >= 'A' && lo <= 'F') lo = (char)(lo - 'A' + 'a');
        if (hi != digits[digest[i] >> 4] || lo != digits[digest[i] & 0x0F]) {
            return 0;
        }
    }
    return 1;
}

static void _transition(fota_ctx_t *ctx, fota_state_t next)
{
    fota_state_t prev = ctx->state;
    ctx->state = next;
    if (ctx->cfg.on_state_change) {
        ctx->cfg.on_state_change(ctx, prev, next, ctx->cfg.user_data);
    }
}

static void _fail(fota_ctx_t *ctx, fota_error_t err)
{
    ctx->last_error = err;
    /* Best-effort report so the console shows why the update stopped. */
    if (err != FOTA_ERR_NO_UPDATE && ctx->cfg.device_token) {
        fota_report_status(ctx->cfg.server_base_url, ctx->cfg.device_token,
                           "failed", ctx->cfg.current_version,
                           _state_name(ctx->state), (int)err, NULL);
    }
    /* Free download buffer if allocated */
    free(ctx->download_buf);
    ctx->download_buf = NULL;
    /* Return to IDLE so the agent can retry later */
    _transition(ctx, FOTA_STATE_IDLE);
}

/* ------------------------------------------------------------------ */
/* State handlers                                                       */
/* ------------------------------------------------------------------ */

static void _handle_checking(fota_ctx_t *ctx)
{
    /*
     * POST /api/v1/devices/check
     * Body: { "current_version": "...", "hw_specs": { "ram_free": N, "storage_free": N } }
     */
    char url[512];
    snprintf(url, sizeof(url), "%s/api/v1/devices/check",
             ctx->cfg.server_base_url);

    char body[256];
    snprintf(body, sizeof(body),
        "{\"current_version\":\"%s\","
        "\"hw_specs\":{\"ram_free\":%u,\"storage_free\":%u}}",
        ctx->cfg.current_version,
        (unsigned)ctx->cfg.ram_free_kb,
        (unsigned)ctx->cfg.storage_free_kb);

    char resp[1024];
    int rc = fota_http_post_json(url, ctx->cfg.device_token, body,
                                 resp, sizeof(resp));
    if (rc < 0) {
        _fail(ctx, FOTA_ERR_NETWORK);
        return;
    }

    /*
     * Minimal JSON parsing — a real implementation should use a proper
     * JSON library (e.g. cJSON, jsmn).
     */
    if (strstr(resp, "\"update_available\":false") ||
        strstr(resp, "\"update_available\": false")) {
        _fail(ctx, FOTA_ERR_NO_UPDATE);
        return;
    }

    /* Parse response JSON into update_info */
    ctx->update_info_valid = 0;
    fota_update_info_t *info = &ctx->update_info;
    memset(info, 0, sizeof(*info));

    if (fota_json_get_string(resp, "target_version",
                             info->target_version, sizeof(info->target_version)) != 0) {
        _fail(ctx, FOTA_ERR_NETWORK);
        return;
    }

    if (fota_json_get_string(resp, "download_url",
                             info->download_url, sizeof(info->download_url)) != 0) {
        _fail(ctx, FOTA_ERR_NETWORK);
        return;
    }

    /* Optional metadata fields */
    fota_json_get_string(resp, "checksum",
                         info->checksum_sha256, sizeof(info->checksum_sha256));
    fota_json_get_string(resp, "signature",
                         info->signature_b64, sizeof(info->signature_b64));

    char payload_type_str[16] = {0};
    fota_json_get_string(resp, "payload_type", payload_type_str, sizeof(payload_type_str));
    info->payload_type = (strncmp(payload_type_str, "DELTA", 5) == 0)
                         ? FOTA_PAYLOAD_DELTA : FOTA_PAYLOAD_FULL;

    int payload_size = 0;
    fota_json_get_int(resp, "payload_size", &payload_size);
    info->payload_size = (uint32_t)(payload_size > 0 ? payload_size : 0);

    /* download_url may be a relative path — prepend server base URL if needed */
    if (info->download_url[0] == '/') {
        char full_url[sizeof(info->download_url)];
        int written = snprintf(full_url, sizeof(full_url), "%s%s",
                               ctx->cfg.server_base_url, info->download_url);
        if (written < 0 || (size_t)written >= sizeof(full_url)) {
            _fail(ctx, FOTA_ERR_NETWORK);
            return;
        }
        strncpy(info->download_url, full_url, sizeof(info->download_url) - 1);
    }

    ctx->update_info_valid = 1;

    _transition(ctx, FOTA_STATE_DOWNLOADING);
}

static void _handle_downloading(fota_ctx_t *ctx)
{
    fota_update_info_t *info = &ctx->update_info;

    /* Allocate download buffer (or reuse if resuming) */
    if (!ctx->download_buf) {
        uint32_t partition_size = hal_flash_partition_size(FOTA_SLOT_INACTIVE);
        ctx->download_buf_size = partition_size ? partition_size : (4 * 1024 * 1024);
        ctx->download_buf = (uint8_t *)malloc(ctx->download_buf_size);
        if (!ctx->download_buf) {
            _fail(ctx, FOTA_ERR_INSUFFICIENT_SPACE);
            return;
        }
        ctx->bytes_received = info->resume_offset;
    }

    /* HTTP GET with Range header for resume support */
    int n = fota_http_get(
        info->download_url,
        ctx->cfg.device_token,
        ctx->bytes_received,
        ctx->download_buf + ctx->bytes_received,
        ctx->download_buf_size - ctx->bytes_received);

    if (n < 0) {
        /* Network error — keep bytes_received; caller can retry */
        _fail(ctx, FOTA_ERR_NETWORK);
        return;
    }

    ctx->bytes_received += (uint32_t)n;
    if (ctx->cfg.on_progress) {
        ctx->cfg.on_progress(ctx->bytes_received, info->payload_size,
                             ctx->cfg.user_data);
    }

    /* Check if download is complete */
    if (info->payload_size > 0 && ctx->bytes_received < info->payload_size) {
        /* Not yet complete — stay in DOWNLOADING, caller retries */
        return;
    }

    _transition(ctx, FOTA_STATE_PATCHING);
}

static void _handle_patching(fota_ctx_t *ctx)
{
    fota_update_info_t *info = &ctx->update_info;
    int rc;

    /* 1. SHA-256 of the downloaded payload (FULL image or DELTA patch),
     *    checked before anything is written to flash. */
    if (info->checksum_sha256[0] != '\0') {
        uint8_t digest[32];
        fota_sha256(ctx->download_buf, ctx->bytes_received, digest);
        if (!_digest_matches_hex(digest, info->checksum_sha256)) {
            _fail(ctx, FOTA_ERR_HASH_MISMATCH);
            return;
        }
    } else if (FOTA_REQUIRE_INTEGRITY) {
        _fail(ctx, FOTA_ERR_HASH_MISMATCH);
        return;
    }

    /* Erase inactive partition */
    uint32_t part_size = hal_flash_partition_size(FOTA_SLOT_INACTIVE);
    uint32_t erase_len = part_size ? part_size : ctx->bytes_received;
    rc = hal_flash_erase(FOTA_SLOT_INACTIVE, 0, _aligned_erase_len(erase_len));
    if (rc != FOTA_HAL_OK) {
        _fail(ctx, FOTA_ERR_FLASH_WRITE);
        return;
    }

    if (info->payload_type == FOTA_PAYLOAD_DELTA) {
        /* Apply bsdiff4 delta: reads active slot, writes inactive slot */
        rc = fota_bspatch(ctx->download_buf, ctx->bytes_received);
        if (rc < 0) {
            _fail(ctx, FOTA_ERR_PATCH_APPLY);
            return;
        }
        /* rc > 0 is the patched image size; 0 means "unknown". */
        ctx->image_size = (rc > 0) ? (uint32_t)rc
                                   : hal_flash_partition_size(FOTA_SLOT_INACTIVE);
    } else {
        /* Full firmware: write directly to inactive slot */
        rc = hal_flash_write(FOTA_SLOT_INACTIVE, 0,
                             ctx->download_buf, ctx->bytes_received);
        if (rc != FOTA_HAL_OK) {
            _fail(ctx, FOTA_ERR_FLASH_WRITE);
            return;
        }
        ctx->image_size = ctx->bytes_received;
    }

    /* Free download buffer — no longer needed */
    free(ctx->download_buf);
    ctx->download_buf = NULL;

    _transition(ctx, FOTA_STATE_VERIFYING);
}

static void _handle_verifying(fota_ctx_t *ctx)
{
    fota_update_info_t *info = &ctx->update_info;
    uint32_t fw_size = ctx->image_size;

    /*
     * 2. ECDSA P-256 signature over the final image in the inactive slot.
     *    The server signs the full target firmware, so this also covers
     *    images produced by a DELTA patch.  MUST pass before the boot slot
     *    changes; a missing signature is a failure unless the build opts
     *    out with FOTA_ALLOW_UNSIGNED (development only).
     */
    if (info->signature_b64[0] == '\0') {
        if (FOTA_REQUIRE_INTEGRITY) {
            hal_flash_erase(FOTA_SLOT_INACTIVE, 0, _aligned_erase_len(fw_size));
            _fail(ctx, FOTA_ERR_SIG_INVALID);
            return;
        }
        _transition(ctx, FOTA_STATE_SWITCHING);
        return;
    }

    if (fw_size == 0) {
        _fail(ctx, FOTA_ERR_DOWNLOAD);
        return;
    }

    /* Read back the written image for verification.
     * Note: this needs fw_size bytes of heap (see PORTING_GUIDE.md, RAM). */
    uint8_t *verify_buf = (uint8_t *)malloc(fw_size);
    if (!verify_buf) {
        _fail(ctx, FOTA_ERR_INSUFFICIENT_SPACE);
        return;
    }

    if (hal_flash_read(FOTA_SLOT_INACTIVE, 0, verify_buf, fw_size) != FOTA_HAL_OK) {
        free(verify_buf);
        _fail(ctx, FOTA_ERR_FLASH_WRITE);
        return;
    }

    int sig_ok = fota_ecdsa_verify(verify_buf, fw_size, info->signature_b64);
    free(verify_buf);
    if (sig_ok != 1) {
        /* Erase the invalid firmware to prevent any accidental boot */
        hal_flash_erase(FOTA_SLOT_INACTIVE, 0, _aligned_erase_len(fw_size));
        _fail(ctx, FOTA_ERR_SIG_INVALID);
        return;
    }

    _transition(ctx, FOTA_STATE_SWITCHING);
}

static void _handle_switching(fota_ctx_t *ctx)
{
    /* PRD: only switch boot slot AFTER successful signature verification */
    if (hal_set_boot_active_slot(FOTA_SLOT_INACTIVE) != FOTA_HAL_OK) {
        _fail(ctx, FOTA_ERR_BOOT_SWITCH);
        return;
    }
    /* Same convention as device-agent: "success" is sent right before the
     * reboot, with the version the device is about to run. */
    if (ctx->cfg.device_token) {
        fota_report_status(ctx->cfg.server_base_url, ctx->cfg.device_token,
                           "success", ctx->update_info.target_version,
                           "REBOOT", 0, NULL);
    }
    _transition(ctx, FOTA_STATE_REBOOT);
}

static void _handle_reboot(fota_ctx_t *ctx)
{
    (void)ctx;
    hal_system_reboot();
    /* hal_system_reboot() must not return */
    while (1) { /* unreachable */ }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

fota_ctx_t *fota_fsm_init(const fota_config_t *cfg)
{
    if (!cfg) return NULL;
    fota_ctx_t *ctx = (fota_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->cfg = *cfg;
    ctx->state = FOTA_STATE_IDLE;
    ctx->last_error = FOTA_ERR_NONE;
    return ctx;
}

void fota_fsm_destroy(fota_ctx_t *ctx)
{
    if (!ctx) return;
    free(ctx->download_buf);
    free(ctx);
}

void fota_fsm_trigger(fota_ctx_t *ctx)
{
    if (ctx && ctx->state == FOTA_STATE_IDLE) {
        ctx->last_error = FOTA_ERR_NONE;
        _transition(ctx, FOTA_STATE_CHECKING);
    }
}

fota_state_t fota_fsm_run(fota_ctx_t *ctx)
{
    if (!ctx) return FOTA_STATE_ERROR;

    switch (ctx->state) {
    case FOTA_STATE_CHECKING:    _handle_checking(ctx);    break;
    case FOTA_STATE_DOWNLOADING: _handle_downloading(ctx); break;
    case FOTA_STATE_PATCHING:    _handle_patching(ctx);    break;
    case FOTA_STATE_VERIFYING:   _handle_verifying(ctx);   break;
    case FOTA_STATE_SWITCHING:   _handle_switching(ctx);   break;
    case FOTA_STATE_REBOOT:      _handle_reboot(ctx);      break;
    case FOTA_STATE_IDLE:
    case FOTA_STATE_ERROR:
    default:
        break;
    }
    return ctx->state;
}

fota_state_t fota_fsm_state(const fota_ctx_t *ctx)
{
    return ctx ? ctx->state : FOTA_STATE_ERROR;
}

fota_error_t fota_fsm_last_error(const fota_ctx_t *ctx)
{
    return ctx ? ctx->last_error : FOTA_ERR_NONE;
}

const fota_update_info_t *fota_fsm_update_info(const fota_ctx_t *ctx)
{
    if (!ctx || !ctx->update_info_valid) return NULL;
    return &ctx->update_info;
}
