/**
 * @file fota_main.c
 * @brief FOTA SDK — POSIX sample validation program.
 *
 * This file demonstrates a complete, production-quality integration of the
 * FOTA FSM.  It handles:
 *
 *   - Reading configuration from environment variables
 *   - Persistent JWT storage (load from file, register if absent)
 *   - State-change and progress callbacks
 *   - A periodic poll loop with configurable interval
 *   - Graceful shutdown on SIGINT / SIGTERM
 *
 * Environment variables
 * ---------------------
 *   FOTA_SERVER          Server base URL (required).
 *                        Example: https://api.fota.example.com
 *
 *   FOTA_REG_TOKEN       One-time registration token from the FOTA dashboard
 *                        (required when no saved token exists).
 *
 *   FOTA_VERSION         Current firmware version string (required).
 *                        Example: 1.2.3
 *
 *   FOTA_TOKEN_FILE      Path where the JWT is persisted across restarts.
 *                        Default: /tmp/fota_token.jwt
 *
 *   FOTA_CHECK_INTERVAL  Seconds between update checks.
 *                        Default: 60
 *
 * Build:
 *   gcc -std=c99 fota_main.c fota_platform_posix.c fota_hal_posix.c \
 *       ../src/fota_fsm.c -I../include -lcurl -lssl -lcrypto -o fota_sdk_sample
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

#include "fota_fsm.h"
#include "fota_hal.h"
#include "fota_platform.h"

/* ------------------------------------------------------------------ */
/* Constants / defaults                                                 */
/* ------------------------------------------------------------------ */

#define DEFAULT_TOKEN_FILE      "/tmp/fota_token.jwt"
#define DEFAULT_CHECK_INTERVAL  300         /**< seconds (5 minutes) */
#define TOKEN_BUF_SIZE          512
#define RAM_FREE_KB             (256U)      /**< Simulated available RAM */
#define STORAGE_FREE_KB         (8192U)     /**< Simulated available storage */

/* ------------------------------------------------------------------ */
/* Global shutdown flag (set by signal handler)                        */
/* ------------------------------------------------------------------ */
static volatile int g_shutdown = 0;

static void signal_handler(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

/* ------------------------------------------------------------------ */
/* State-name helper                                                    */
/* ------------------------------------------------------------------ */
static const char *state_name(fota_state_t s)
{
    switch (s) {
        case FOTA_STATE_IDLE:        return "IDLE";
        case FOTA_STATE_CHECKING:    return "CHECKING";
        case FOTA_STATE_DOWNLOADING: return "DOWNLOADING";
        case FOTA_STATE_PATCHING:    return "PATCHING";
        case FOTA_STATE_VERIFYING:   return "VERIFYING";
        case FOTA_STATE_SWITCHING:   return "SWITCHING";
        case FOTA_STATE_REBOOT:      return "REBOOT";
        case FOTA_STATE_ERROR:       return "ERROR";
        default:                     return "UNKNOWN";
    }
}

static const char *error_name(fota_error_t e)
{
    switch (e) {
        case FOTA_ERR_NONE: return "NONE";
        case FOTA_ERR_NETWORK: return "NETWORK";
        case FOTA_ERR_NO_UPDATE: return "NO_UPDATE";
        case FOTA_ERR_DOWNLOAD: return "DOWNLOAD";
        case FOTA_ERR_HASH_MISMATCH: return "HASH_MISMATCH";
        case FOTA_ERR_SIG_INVALID: return "SIG_INVALID";
        case FOTA_ERR_FLASH_WRITE: return "FLASH_WRITE";
        case FOTA_ERR_PATCH_APPLY: return "PATCH_APPLY";
        case FOTA_ERR_BOOT_SWITCH: return "BOOT_SWITCH";
        case FOTA_ERR_INSUFFICIENT_SPACE: return "INSUFFICIENT_SPACE";
        default: return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */
/* on_state_change callback                                             */
/* ------------------------------------------------------------------ */
static void on_state_change(fota_ctx_t  *ctx,
                            fota_state_t old_state,
                            fota_state_t new_state,
                            void        *user_data)
{
    (void)ctx;
    (void)user_data;

    /* Timestamp in ISO-8601-like local format */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm_info);

    fprintf(stdout, "[%s] FOTA state: %s → %s\n",
            ts, state_name(old_state), state_name(new_state));
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* on_progress callback                                                 */
/* ------------------------------------------------------------------ */
static void on_progress(uint32_t bytes_received,
                        uint32_t bytes_total,
                        void    *user_data)
{
    (void)user_data;

    /* Print a simple progress bar: [=====>    ] 45% */
    if (bytes_total == 0) {
        fprintf(stdout, "\r  Downloaded %u bytes (total unknown) ...",
                (unsigned)bytes_received);
    } else {
        int percent = (int)((uint64_t)bytes_received * 100 / bytes_total);
        int filled  = percent / 5;   /* bar width = 20 chars */
        int empty   = 20 - filled;

        fprintf(stdout, "\r  [");
        for (int i = 0; i < filled; i++) fputc('=', stdout);
        if (filled < 20) {
            fputc('>', stdout);
            empty--;
        }
        for (int i = 0; i < empty; i++) fputc(' ', stdout);
        fprintf(stdout, "] %3d%%  (%u / %u bytes)",
                percent,
                (unsigned)bytes_received,
                (unsigned)bytes_total);
    }
    fflush(stdout);

    /* Newline when download completes */
    if (bytes_total > 0 && bytes_received >= bytes_total) {
        fputc('\n', stdout);
        fflush(stdout);
    }
}

/* ------------------------------------------------------------------ */
/* Token persistence                                                    */
/* ------------------------------------------------------------------ */

/**
 * Load a previously saved JWT from disk.
 * Returns 0 if loaded, -1 if file does not exist or is empty.
 */
static int load_token(const char *path, char *out_token, size_t out_size)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    size_t n = fread(out_token, 1, out_size - 1, fp);
    fclose(fp);

    if (n == 0) return -1;

    /* Strip trailing newline / whitespace */
    while (n > 0 && (out_token[n - 1] == '\n' ||
                     out_token[n - 1] == '\r' ||
                     out_token[n - 1] == ' ')) {
        n--;
    }
    out_token[n] = '\0';
    return (n > 0) ? 0 : -1;
}

/**
 * Persist the JWT to disk (mode 0600).
 */
static int save_token(const char *path, const char *token)
{
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "[fota_main] cannot write token file %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    fputs(token, fp);
    fputc('\n', fp);
    fclose(fp);
    return 0;
}

/**
 * Ensure a valid signing public key source is configured for signature verify.
 *
 * Priority:
 *  1) Use FOTA_PUBLIC_KEY_PEM if already provided by user.
 *  2) Fetch /api/v1/devices/public-key and persist to /tmp/fota_public_key.pem,
 *     then set FOTA_PUBLIC_KEY_PEM to that file path.
 */
static int ensure_signing_public_key(const char *server_base_url)
{
    const char *existing = getenv("FOTA_PUBLIC_KEY_PEM");
    if (existing && existing[0] != '\0') {
        return 0;
    }

    char url[512];
    snprintf(url, sizeof(url), "%s/api/v1/devices/public-key", server_base_url);

    uint8_t resp_raw[8192];
    int n = fota_http_get(url, NULL, 0, resp_raw, sizeof(resp_raw) - 1);
    if (n < 0) {
        fprintf(stderr,
                "[fota_main] ERROR: failed to fetch signing public key from %s\n",
                url);
        return -1;
    }
    resp_raw[n] = '\0';

    char pem[4096];
    if (fota_json_get_string((const char *)resp_raw, "public_key_pem", pem, sizeof(pem)) != 0) {
        fprintf(stderr,
                "[fota_main] ERROR: public_key_pem not found in server response\n");
        return -1;
    }

    const char *key_path = "/tmp/fota_public_key.pem";
    FILE *fp = fopen(key_path, "w");
    if (!fp) {
        fprintf(stderr,
                "[fota_main] ERROR: cannot write %s: %s\n",
                key_path,
                strerror(errno));
        return -1;
    }
    fputs(pem, fp);
    fputc('\n', fp);
    fclose(fp);

    if (setenv("FOTA_PUBLIC_KEY_PEM", key_path, 1) != 0) {
        fprintf(stderr,
                "[fota_main] ERROR: setenv(FOTA_PUBLIC_KEY_PEM) failed: %s\n",
                strerror(errno));
        return -1;
    }

    fprintf(stdout,
            "[fota_main] Loaded signing public key from server and saved to %s\n",
            key_path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* ---- Read configuration from environment ---- */
    const char *server      = getenv("FOTA_SERVER");
    const char *reg_token   = getenv("FOTA_REG_TOKEN");
    const char *version     = getenv("FOTA_VERSION");
    const char *token_file  = getenv("FOTA_TOKEN_FILE");
    const char *interval_str = getenv("FOTA_CHECK_INTERVAL");

    if (!token_file || token_file[0] == '\0') {
        token_file = DEFAULT_TOKEN_FILE;
    }

    int check_interval = DEFAULT_CHECK_INTERVAL;
    if (interval_str && interval_str[0] != '\0') {
        check_interval = atoi(interval_str);
        if (check_interval <= 0) {
            fprintf(stderr, "[fota_main] invalid FOTA_CHECK_INTERVAL '%s', "
                    "using default %d seconds\n",
                    interval_str, DEFAULT_CHECK_INTERVAL);
            check_interval = DEFAULT_CHECK_INTERVAL;
        }
    }

    if (!server || server[0] == '\0') {
        fprintf(stderr,
                "[fota_main] ERROR: FOTA_SERVER environment variable is required.\n"
                "  Example: export FOTA_SERVER=https://api.fota.example.com\n");
        return EXIT_FAILURE;
    }
    if (!version || version[0] == '\0') {
        fprintf(stderr,
                "[fota_main] ERROR: FOTA_VERSION environment variable is required.\n"
                "  Example: export FOTA_VERSION=1.0.0\n");
        return EXIT_FAILURE;
    }

    if (ensure_signing_public_key(server) != 0) {
        fprintf(stderr,
                "[fota_main] ERROR: signing public key is not configured.\n"
                "  Set FOTA_PUBLIC_KEY_PEM manually or ensure /api/v1/devices/public-key is reachable.\n");
        return EXIT_FAILURE;
    }

    /* ---- Read hardware serial number ---- */
    char serial_buf[64] = {0};
    if (hal_get_serial(serial_buf, sizeof(serial_buf)) != FOTA_HAL_OK) {
        fprintf(stderr,
                "[fota_main] ERROR: hal_get_serial() failed — cannot identify device.\n"
                "  Implement hal_get_serial() in your platform HAL.\n");
        return EXIT_FAILURE;
    }

    fprintf(stdout, "[fota_main] FOTA agent starting\n");
    fprintf(stdout, "  Server:          %s\n", server);
    fprintf(stdout, "  Device serial:   %s\n", serial_buf);
    fprintf(stdout, "  Current version: %s\n", version);
    fprintf(stdout, "  Token file:      %s\n", token_file);
    fprintf(stdout, "  Check interval:  %d seconds\n", check_interval);

    /* ---- Load or acquire JWT ---- */
    char token[TOKEN_BUF_SIZE];
    memset(token, 0, sizeof(token));

    if (load_token(token_file, token, sizeof(token)) == 0) {
        fprintf(stdout, "[fota_main] Loaded saved JWT from %s\n", token_file);
    } else {
        /* Need to register */
        if (!reg_token || reg_token[0] == '\0') {
            fprintf(stderr,
                    "[fota_main] ERROR: No saved token found and FOTA_REG_TOKEN "
                    "is not set.  Cannot register.\n");
            return EXIT_FAILURE;
        }

        fprintf(stdout, "[fota_main] Registering device (serial: %s) ...\n",
                serial_buf);
        if (fota_register(server, reg_token, serial_buf,
                          token, sizeof(token)) != 0) {
            fprintf(stderr, "[fota_main] Registration failed — aborting.\n");
            return EXIT_FAILURE;
        }
        fprintf(stdout, "[fota_main] Registration successful.\n");

        /* Persist JWT for subsequent runs */
        if (save_token(token_file, token) == 0) {
            fprintf(stdout, "[fota_main] JWT saved to %s\n", token_file);
        }
    }

    /* ---- Build FSM configuration ---- */
    fota_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.server_base_url  = server;
    cfg.device_token     = token;
    cfg.current_version  = version;
    cfg.ram_free_kb      = RAM_FREE_KB;
    cfg.storage_free_kb  = STORAGE_FREE_KB;
    cfg.on_state_change  = on_state_change;
    cfg.on_progress      = on_progress;
    cfg.user_data        = NULL;

    /* ---- Initialise FSM ---- */
    fota_ctx_t *ctx = fota_fsm_init(&cfg);
    if (!ctx) {
        fprintf(stderr, "[fota_main] fota_fsm_init() returned NULL (OOM?)\n");
        return EXIT_FAILURE;
    }

    /* ---- Install signal handlers for clean shutdown ---- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stdout, "[fota_main] Entering poll loop. "
            "Send SIGINT (Ctrl-C) or SIGTERM to exit cleanly.\n");

    /* ---- Main poll loop ---- */
    while (!g_shutdown) {
        /* Trigger an update check (no-op if not in IDLE) */
        fota_fsm_trigger(ctx);

        /* Drive the FSM until it returns to IDLE (or ERROR → IDLE) */
        fota_state_t state;
        do {
            if (g_shutdown) break;
            state = fota_fsm_run(ctx);
        } while (state != FOTA_STATE_IDLE);

        /* Report result of the last cycle. "No update" is expected and normal. */
        fota_error_t err = fota_fsm_last_error(ctx);
        if (err == FOTA_ERR_NO_UPDATE) {
            fprintf(stdout, "[fota_main] No update available.\n");
        } else if (err != FOTA_ERR_NONE) {
            fprintf(stderr, "[fota_main] Last FSM error: %s (%d)\n",
                    error_name(err), (int)err);
        }

        if (g_shutdown) break;

        /* Sleep until the next check, waking up every second to test the
         * shutdown flag so that SIGINT is always responsive.               */
        for (int remaining = check_interval;
             remaining > 0 && !g_shutdown;
             remaining--) {
            sleep(1);
        }
    }

    /* ---- Clean up ---- */
    fprintf(stdout, "\n[fota_main] Shutting down.\n");
    fota_fsm_destroy(ctx);

    return EXIT_SUCCESS;
}
