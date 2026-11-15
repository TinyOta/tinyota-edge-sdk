/**
 * @file fota_fsm.h
 * @brief FOTA Agent Finite State Machine — C99 Interface
 *
 * Implements the 7-state update lifecycle defined in PRD v1.0:
 *
 *   IDLE → CHECKING → DOWNLOADING → PATCHING → VERIFYING → SWITCHING → REBOOT
 *
 * The caller drives the FSM by invoking fota_fsm_run() periodically
 * (e.g., from a dedicated RTOS task or a timer ISR continuation).
 *
 * Thread safety: not re-entrant.  Drive from one task only.
 */

#ifndef FOTA_FSM_H
#define FOTA_FSM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* States                                                               */
/* ------------------------------------------------------------------ */
typedef enum {
    FOTA_STATE_IDLE        = 0,  /**< No update activity. */
    FOTA_STATE_CHECKING    = 1,  /**< Querying server for available update. */
    FOTA_STATE_DOWNLOADING = 2,  /**< Downloading payload (Full or Delta). */
    FOTA_STATE_PATCHING    = 3,  /**< Applying delta patch to inactive slot. */
    FOTA_STATE_VERIFYING   = 4,  /**< Block-level SHA-256 + ECDSA signature check. */
    FOTA_STATE_SWITCHING   = 5,  /**< Setting inactive slot as next-boot target. */
    FOTA_STATE_REBOOT      = 6,  /**< Triggering system reboot. */
    FOTA_STATE_ERROR       = 7,  /**< Unrecoverable error — rolls back, returns to IDLE. */
} fota_state_t;

/* ------------------------------------------------------------------ */
/* Payload types                                                        */
/* ------------------------------------------------------------------ */
typedef enum {
    FOTA_PAYLOAD_FULL  = 0,  /**< Full firmware binary. */
    FOTA_PAYLOAD_DELTA = 1,  /**< Binary delta patch (bsdiff4 format). */
} fota_payload_type_t;

/* ------------------------------------------------------------------ */
/* Error codes                                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    FOTA_ERR_NONE            =  0,
    FOTA_ERR_NETWORK         = -1,  /**< HTTP request failed. */
    FOTA_ERR_NO_UPDATE       = -2,  /**< Server reports no update available. */
    FOTA_ERR_DOWNLOAD        = -3,  /**< Download incomplete or corrupt. */
    FOTA_ERR_HASH_MISMATCH   = -4,  /**< SHA-256 verification failed. */
    FOTA_ERR_SIG_INVALID     = -5,  /**< ECDSA signature verification failed. */
    FOTA_ERR_FLASH_WRITE     = -6,  /**< Flash write/erase error. */
    FOTA_ERR_PATCH_APPLY     = -7,  /**< Delta patch application failed. */
    FOTA_ERR_BOOT_SWITCH     = -8,  /**< hal_set_boot_active_slot() failed. */
    FOTA_ERR_INSUFFICIENT_SPACE = -9, /**< Payload too large for partition. */
} fota_error_t;

/* ------------------------------------------------------------------ */
/* Update descriptor (populated by CHECKING state)                     */
/* ------------------------------------------------------------------ */
typedef struct {
    char     target_version[32];    /**< Target firmware version string. */
    char     download_url[2048];    /**< Authenticated download URL (presigned URLs can be long). */
    char     checksum_sha256[65];   /**< Expected SHA-256 hex string of payload. */
    char     signature_b64[512];    /**< ECDSA P-256 base64url DER signature. */
    fota_payload_type_t payload_type;
    uint32_t payload_size;          /**< Expected payload size in bytes (0 = unknown). */
    uint32_t resume_offset;         /**< Bytes already downloaded (for Range resume). */
} fota_update_info_t;

/* ------------------------------------------------------------------ */
/* FSM context                                                          */
/* ------------------------------------------------------------------ */
typedef struct fota_ctx fota_ctx_t;

/**
 * @brief Callback invoked on every state transition.
 *
 * @param ctx       FSM context.
 * @param old_state Previous state.
 * @param new_state New state.
 * @param user_data Application-supplied pointer from fota_fsm_init().
 */
typedef void (*fota_state_cb_t)(fota_ctx_t *ctx,
                                fota_state_t old_state,
                                fota_state_t new_state,
                                void *user_data);

/**
 * @brief Callback invoked during download to report progress.
 *
 * @param bytes_received Bytes downloaded so far.
 * @param bytes_total    Total payload size (0 if unknown).
 * @param user_data      Application-supplied pointer.
 */
typedef void (*fota_progress_cb_t)(uint32_t bytes_received,
                                   uint32_t bytes_total,
                                   void *user_data);

/* ------------------------------------------------------------------ */
/* Configuration passed to fota_fsm_init()                             */
/* ------------------------------------------------------------------ */
typedef struct {
    /** Base URL of the FOTA server, e.g. "https://api.fota.example.com". */
    const char *server_base_url;

    /** Device JWT bearer token (obtained from /devices/register). */
    const char *device_token;

    /** Current firmware version string, e.g. "1.2.3". */
    const char *current_version;

    /** Free RAM available for delta patching, in KB. */
    uint32_t ram_free_kb;

    /** Free storage available, in KB. */
    uint32_t storage_free_kb;

    /** Called on every state transition (may be NULL). */
    fota_state_cb_t on_state_change;

    /** Called during download progress (may be NULL). */
    fota_progress_cb_t on_progress;

    /** Passed as-is to callbacks. */
    void *user_data;
} fota_config_t;

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Allocate and initialise an FSM context.
 *
 * @param cfg  Configuration (copied internally; caller may free after return).
 * @return Pointer to a newly allocated fota_ctx_t, or NULL on OOM.
 */
fota_ctx_t *fota_fsm_init(const fota_config_t *cfg);

/**
 * @brief Free an FSM context and all internal resources.
 */
void fota_fsm_destroy(fota_ctx_t *ctx);

/**
 * @brief Trigger an update check cycle (transitions from IDLE → CHECKING).
 *
 * Non-blocking: the actual work is performed inside fota_fsm_run().
 * Has no effect if the FSM is not in IDLE state.
 */
void fota_fsm_trigger(fota_ctx_t *ctx);

/**
 * @brief Advance the FSM by one step.
 *
 * Call this in a loop or from a periodic task.  Returns quickly when there
 * is nothing to do (IDLE state).
 *
 * @param ctx FSM context.
 * @return Current state after processing.
 */
fota_state_t fota_fsm_run(fota_ctx_t *ctx);

/**
 * @brief Return the current FSM state.
 */
fota_state_t fota_fsm_state(const fota_ctx_t *ctx);

/**
 * @brief Return the last error code (FOTA_ERR_NONE if no error).
 */
fota_error_t fota_fsm_last_error(const fota_ctx_t *ctx);

/**
 * @brief Return the update descriptor populated during CHECKING.
 *
 * Valid only after the FSM has passed through CHECKING successfully.
 * Returns NULL if not yet populated.
 */
const fota_update_info_t *fota_fsm_update_info(const fota_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* FOTA_FSM_H */
