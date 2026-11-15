/**
 * @file fota_platform.h
 * @brief FOTA Platform Seam Declarations — C99
 *
 * This header declares the platform-specific functions that fota_fsm.c calls
 * via extern linkage.  Integrators must provide a translation unit that
 * implements every function listed here for their target platform.
 *
 * A ready-to-use POSIX reference implementation is provided in
 * src/fota_platform_posix.c (requires libcurl + OpenSSL).
 *
 * Return-value conventions (unless noted per-function):
 *   >= 0   Success (bytes transferred, etc.)
 *   <  0   Error
 *
 * Thread safety: the FSM calls these functions from a single task/thread.
 * Implementations need not be re-entrant unless your RTOS requires it.
 */

#ifndef FOTA_PLATFORM_H
#define FOTA_PLATFORM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* HTTP primitives                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Perform an HTTP GET request, optionally with a Range header.
 *
 * Used by the DOWNLOADING state to fetch firmware payloads.  When
 * @p range_start is greater than zero, the implementation MUST add the
 * header "Range: bytes=<range_start>-" to enable resume-on-reconnect.
 *
 * @param url         Null-terminated URL (http:// or https://).
 * @param bearer_token  Device JWT; the implementation adds
 *                    "Authorization: Bearer <bearer_token>" header.
 *                    Pass NULL to omit the header.
 * @param range_start Byte offset to start from (0 = beginning of file).
 * @param out_buf     Caller-allocated buffer that receives the response body.
 * @param buf_size    Size of @p out_buf in bytes.
 * @return            Number of bytes written into @p out_buf on success,
 *                    or a negative error code on failure.
 */
int fota_http_get(const char *url,
                  const char *bearer_token,
                  uint32_t    range_start,
                  uint8_t    *out_buf,
                  size_t      buf_size);

/**
 * @brief Perform an HTTP POST request with a JSON body.
 *
 * Used by the CHECKING state (POST /api/v1/devices/check) and by
 * fota_register().  The implementation MUST set the header
 * "Content-Type: application/json".
 *
 * @param url           Null-terminated URL.
 * @param bearer_token  Device JWT (added as Authorization header).
 *                      Pass NULL to omit.
 * @param json_body     Null-terminated JSON string to send as the request body.
 * @param resp_buf      Caller-allocated buffer that receives the response body.
 * @param resp_buf_size Size of @p resp_buf in bytes.
 * @return              0 if the server returned an HTTP 2xx status code,
 *                      or a negative error code on failure / non-2xx response.
 */
int fota_http_post_json(const char *url,
                        const char *bearer_token,
                        const char *json_body,
                        char       *resp_buf,
                        size_t      resp_buf_size);

/* ------------------------------------------------------------------ */
/* Cryptography                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Compute a SHA-256 digest.
 *
 * @param data  Input data buffer.
 * @param len   Length of @p data in bytes.
 * @param out   32-byte output buffer that receives the raw digest.
 */
void fota_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/**
 * @brief Verify an ECDSA P-256 signature over @p data.
 *
 * The signature is provided as a base64url-encoded DER blob (the format
 * returned by the FOTA server's firmware manifest).  The implementation
 * loads the vendor public key from the environment variable
 * FOTA_PUBLIC_KEY_PEM. The value may be either a PEM file path or the
 * PEM contents themselves. If unset, the POSIX reference port falls back
 * to a compiled-in test key.
 *
 * @param data      Buffer whose authenticity is being verified.
 * @param data_len  Length of @p data in bytes.
 * @param sig_b64   Null-terminated base64url DER signature string.
 * @return           1  — signature is valid.
 *                   0  — signature is invalid (tampered or wrong key).
 *                   <0 — internal error (key load failure, parse error, etc.).
 */
int fota_ecdsa_verify(const uint8_t *data,
                      size_t         data_len,
                      const char    *sig_b64);

/* ------------------------------------------------------------------ */
/* Delta patching                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Apply a bsdiff4-format binary delta patch.
 *
 * The delta is applied in-place against the currently active flash slot.
 * The patched result is written to the inactive slot via the HAL.
 *
 * @param delta      Buffer containing the full delta patch.
 * @param delta_len  Length of @p delta in bytes.
 * @return           Size in bytes of the patched image written to the
 *                   inactive slot (> 0) on success, 0 on success when the
 *                   size is unknown (the FSM then verifies the whole
 *                   partition), or a negative error code on failure.
 *
 * @note The POSIX reference port implements this with bzip2.  Other ports
 *       get a weak default from fota_common.c that returns -1, so they are
 *       FULL-only: set fota_config_t::ram_free_kb to 0 and the server will
 *       always send FULL payloads.
 */
int fota_bspatch(const uint8_t *delta, size_t delta_len);

/* ------------------------------------------------------------------ */
/* Minimal JSON field extraction                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Extract a string field from a flat JSON object.
 *
 * Searches the JSON text for the pattern  "key":"value"  or
 * "key": "value"  and copies the unquoted value into @p out.
 *
 * @param json      Null-terminated JSON string.
 * @param key       Field name to look up.
 * @param out       Destination buffer.
 * @param out_size  Size of @p out in bytes (including null terminator).
 * @return          0 if the field was found and copied, -1 if not found
 *                  or if @p out is too small.
 */
int fota_json_get_string(const char *json,
                         const char *key,
                         char       *out,
                         size_t      out_size);

/**
 * @brief Extract an integer field from a flat JSON object.
 *
 * Searches for the pattern  "key":number  (no quotes around value).
 *
 * @param json  Null-terminated JSON string.
 * @param key   Field name to look up.
 * @param out   Receives the parsed integer value.
 * @return      0 if found, -1 if not found or not a valid integer.
 */
int fota_json_get_int(const char *json, const char *key, int *out);

/**
 * @brief Extract a boolean field from a flat JSON object.
 *
 * Searches for  "key":true  or  "key":false  (case-sensitive, lowercase).
 *
 * @param json  Null-terminated JSON string.
 * @param key   Field name to look up.
 * @param out   Receives 1 for true, 0 for false.
 * @return      0 if found, -1 if not found.
 */
int fota_json_get_bool(const char *json, const char *key, int *out);

/* ------------------------------------------------------------------ */
/* Device registration                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Register a device with the FOTA server and obtain a JWT.
 *
 * Sends a POST request to  <server_base_url>/api/v1/devices/register
 * with body:
 *
 *   {"registration_token":"<registration_token>","unit_serial":"<serial>"}
 *
 * On success the server returns a JSON object containing an
 * "access_token" field.  That token is copied into @p out_token and
 * must be supplied to fota_config_t::device_token on all subsequent
 * FSM calls.
 *
 * @param server_base_url    Base URL, e.g. "https://api.fota.example.com"
 *                           (no trailing slash).
 * @param registration_token One-time provisioning token shown on the
 *                           device-model page of the FOTA dashboard.
 * @param serial             Device serial number string (printable ASCII,
 *                           max 64 characters recommended).
 * @param out_token          Caller-allocated buffer that receives the JWT
 *                           (null-terminated).  Provide at least 512 bytes.
 * @param out_token_size     Size of @p out_token in bytes.
 * @return                   0 on success, negative error code on failure.
 */
int fota_register(const char *server_base_url,
                  const char *registration_token,
                  const char *serial,
                  char       *out_token,
                  size_t      out_token_size);

/* ------------------------------------------------------------------ */
/* Signature encoding helper                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Convert the server's base64url signature to standard base64.
 *
 * The server sends signatures as base64url ('-' and '_').  mbedTLS
 * mbedtls_base64_decode() only accepts standard base64, so ports call this
 * first.  Padding is added when missing.  Implemented in fota_common.c.
 *
 * @return Length written (excluding NUL), or -1 if @p out is too small.
 */
int fota_b64url_to_b64(const char *in, char *out, size_t out_size);

/* ------------------------------------------------------------------ */
/* Status reporting                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Report update progress or a result to the FOTA server.
 *
 * Sends POST <server_base_url>/api/v1/devices/status with body
 *   {"status":"...","version":"...","fsm_state":"...","error_code":N,"message":"..."}
 *
 * The FSM calls this itself: "failed" on every error except "no update",
 * and "success" right before rebooting into the new image.  Applications
 * can call it too (for example "rollback" after a failed self-test).
 * A status of "success" makes the console show @p version as the device's
 * current firmware.
 *
 * Implemented in fota_common.c on top of fota_http_post_json().
 * Characters that would break a JSON string are dropped from the inputs.
 *
 * @return 0 if the server accepted the report, -1 otherwise.  Reporting is
 *         best-effort; the FSM never fails an update because of it.
 */
int fota_report_status(const char *server_base_url,
                       const char *device_token,
                       const char *status,
                       const char *version,
                       const char *fsm_state,
                       int         error_code,
                       const char *message);

#ifdef __cplusplus
}
#endif

#endif /* FOTA_PLATFORM_H */
