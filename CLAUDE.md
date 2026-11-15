<!-- Generated from docs/AI_CONTEXT.md by scripts/sync_ai_context.py. Edit that file, not this one. -->

# TinyOTA Edge SDK: context for AI coding assistants

You are helping a firmware developer add TinyOTA over-the-air (OTA) updates
to an embedded device. Use only the functions, types and endpoints in this
file and in `include/*.h`. Do not invent names. If something is missing, say
so and ask instead of guessing.

## 1. Pick the integration path first

| Developer's environment | Path | Start from |
|---|---|---|
| ESP32 + Arduino IDE or PlatformIO (Arduino framework) | Lightweight client, no C SDK. Uses Arduino `Update`, `HTTPClient`, mbedTLS | `examples/esp32-arduino/` |
| ESP32 + ESP-IDF | C SDK with `fota_hal_esp32.c` + `fota_platform_esp32.c` | `examples/esp-idf/` |
| nRF52 / nRF53 / nRF91 + Zephyr or nRF Connect SDK | C SDK with `fota_hal_nrf52.c` + `fota_platform_nrf52.c` + MCUboot | `examples/zephyr-nrf/` |
| Linux (Raspberry Pi, Yocto, OpenWrt, gateways) | Python `device-agent/` or POSIX C sample | `examples/linux/` |
| STM32, RP2040/Pico W, other MCUs | C SDK, write a new HAL + platform port | `PORTING_GUIDE.md`, copy the closest port |

## 2. Server facts

- Device API base URL: `https://api.tinyota.xyz` (not `https://tinyota.xyz`,
  which is the web console).
- All device endpoints are under `/api/v1/devices/`.
- Firmware is signed by the server with ECDSA P-256 over SHA-256 of the
  full target image. The signature is base64url-encoded DER.
- The signing public key comes from `GET /api/v1/devices/public-key`,
  which returns `{"public_key_pem":"-----BEGIN PUBLIC KEY-----\n..."}`. Embed it in the firmware at build time
  (`TINYOTA_PUBKEY_PEM`). Never download or replace it at runtime.

### Device lifecycle

1. **Register once.** The developer creates a *device model* in the console
   and copies its *registration token*. On first boot the device calls
   `POST /api/v1/devices/register`
   with `{"registration_token":"...","unit_serial":"<stable serial>"}`.
   Response: `{"unit_id":1,"model_key":"...","access_token":"<JWT>"}`.
   Store `access_token` in non-volatile storage (NVS, settings, file). Every
   later call sends `Authorization: Bearer <access_token>`.
2. **Check.** `POST /api/v1/devices/check` with
   `{"current_version":"1.0.0","hw_specs":{"ram_free":0,"storage_free":0}}`.
   Response when there is an update:
   `{"update_available":true,"current_version":"1.0.0","target_version":"1.1.0","payload_type":"FULL","download_url":"/api/v1/devices/download/firmware/12","checksum":"<sha256 hex of the payload>","signature":"<base64url DER>"}`.
   `download_url` is relative: prefix it with the base URL.
   Send `ram_free` 0 (KB) to always get FULL payloads. Larger values let the
   server pick DELTA, which only the POSIX port can apply.
3. **Download.** `GET <base><download_url>` with the Bearer header. Supports
   `Range: bytes=N-` for resume.
4. **Verify, then switch.** The SHA-256 of the payload must equal
   `checksum`, and the signature must verify against the embedded public
   key, before the boot slot changes. Missing checksum or signature means
   reject.
5. **Report.** `POST /api/v1/devices/status` with
   `{"status":"success|failed|...","version":"1.1.0","fsm_state":"REBOOT","error_code":0,"message":""}`.
   `"success"` makes the console show `version` as the device's firmware.

### Version strings

`current_version` in the firmware must match the version typed in the
console upload form exactly. `1.0.0` and `v1.0.0` are different versions,
and a mismatch makes the device download the same update forever.

## 3. C SDK API (ESP-IDF, Zephyr, POSIX, new ports)

FSM: `IDLE -> CHECKING -> DOWNLOADING -> PATCHING -> VERIFYING -> SWITCHING -> REBOOT`.
On any error it reports `failed` and returns to `IDLE` without switching.

### `include/fota_fsm.h` (do not modify)

```c
typedef struct {
    const char *server_base_url;   /* "https://api.tinyota.xyz" */
    const char *device_token;      /* access_token from fota_register() */
    const char *current_version;   /* e.g. "1.0.0", must match the console */
    uint32_t    ram_free_kb;       /* 0 = always FULL payloads */
    uint32_t    storage_free_kb;
    fota_state_cb_t    on_state_change;  /* may be NULL */
    fota_progress_cb_t on_progress;      /* may be NULL */
    void       *user_data;
} fota_config_t;

fota_ctx_t  *fota_fsm_init(const fota_config_t *cfg);
void         fota_fsm_destroy(fota_ctx_t *ctx);
void         fota_fsm_trigger(fota_ctx_t *ctx);   /* IDLE -> CHECKING */
fota_state_t fota_fsm_run(fota_ctx_t *ctx);       /* one step, call in a loop */
fota_state_t fota_fsm_state(const fota_ctx_t *ctx);
fota_error_t fota_fsm_last_error(const fota_ctx_t *ctx);
const fota_update_info_t *fota_fsm_update_info(const fota_ctx_t *ctx);
```

Error codes: `FOTA_ERR_NONE` 0, `FOTA_ERR_NETWORK` -1, `FOTA_ERR_NO_UPDATE` -2,
`FOTA_ERR_DOWNLOAD` -3, `FOTA_ERR_HASH_MISMATCH` -4, `FOTA_ERR_SIG_INVALID` -5,
`FOTA_ERR_FLASH_WRITE` -6, `FOTA_ERR_PATCH_APPLY` -7, `FOTA_ERR_BOOT_SWITCH` -8,
`FOTA_ERR_INSUFFICIENT_SPACE` -9.

Typical loop:

```c
fota_ctx_t *ctx = fota_fsm_init(&cfg);
for (;;) {
    sleep_hours(1);
    fota_fsm_trigger(ctx);
    while (fota_fsm_state(ctx) != FOTA_STATE_IDLE) {
        fota_fsm_run(ctx);        /* reboots inside REBOOT on success */
        sleep_ms(20);
    }
}
```

### `include/fota_hal.h`: the 8 functions a port implements

```c
int      hal_flash_read(int slot, uint32_t offset, uint8_t *buf, size_t len);
int      hal_flash_write(int slot, uint32_t offset, const uint8_t *buf, size_t len);
int      hal_flash_erase(int slot, uint32_t offset, size_t len);
int      hal_set_boot_active_slot(int slot);
void     hal_system_reboot(void);              /* must not return */
uint32_t hal_flash_sector_size(void);
uint32_t hal_flash_partition_size(int slot);
int      hal_get_serial(char *out, size_t out_size);  /* stable across reboots */
```

Slots: `FOTA_SLOT_ACTIVE` 0 (running), `FOTA_SLOT_INACTIVE` 1 (update target).
Return `FOTA_HAL_OK` 0 or `FOTA_HAL_ERR_IO` -1, `FOTA_HAL_ERR_PARAM` -2,
`FOTA_HAL_ERR_ALIGN` -3, `FOTA_HAL_ERR_PROTECTED` -4, `FOTA_HAL_ERR_TIMEOUT` -5.
`hal_flash_write` and `hal_flash_erase` must refuse the active slot with
`FOTA_HAL_ERR_PROTECTED`.

### `include/fota_platform.h`

A port implements:

```c
int  fota_http_get(const char *url, const char *bearer_token, uint32_t range_start,
                   uint8_t *out_buf, size_t buf_size);          /* bytes read or <0 */
int  fota_http_post_json(const char *url, const char *bearer_token, const char *json_body,
                         char *resp_buf, size_t resp_buf_size); /* 0 on HTTP 2xx */
void fota_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
int  fota_ecdsa_verify(const uint8_t *data, size_t data_len, const char *sig_b64); /* 1 = valid */
int  fota_bspatch(const uint8_t *delta, size_t delta_len);     /* optional, DELTA only */
```

Already provided by `src/fota_common.c` (do not reimplement):

```c
int fota_json_get_string(const char *json, const char *key, char *out, size_t out_size);
int fota_json_get_int(const char *json, const char *key, int *out);
int fota_json_get_bool(const char *json, const char *key, int *out);
int fota_register(const char *server_base_url, const char *registration_token,
                  const char *serial, char *out_token, size_t out_token_size);
int fota_report_status(const char *server_base_url, const char *device_token,
                       const char *status, const char *version, const char *fsm_state,
                       int error_code, const char *message);
int fota_b64url_to_b64(const char *in, char *out, size_t out_size);
```

`fota_ecdsa_verify` receives the base64url signature. With mbedTLS, call
`fota_b64url_to_b64()` before `mbedtls_base64_decode()`.

### Build-time configuration

Ports read an optional `tinyota_config.h` from the include path:

```c
#define TINYOTA_API          "https://api.tinyota.xyz"
#define TINYOTA_REG_TOKEN    "<registration token from the console>"
#define FW_VERSION           "1.0.0"
#define TINYOTA_PUBKEY_PEM   "-----BEGIN PUBLIC KEY-----\n...\n-----END PUBLIC KEY-----\n"
```

`FOTA_ALLOW_UNSIGNED` disables the checksum/signature requirement. Only use
it for local development, never in a release build.

### RAM

The current FSM buffers the whole payload in heap and reads the whole image
back for signature verification. Free heap must be at least the firmware
size. If the target cannot afford that, say so instead of shrinking buffers.

## 4. Rules the generated code must follow

1. Never switch the boot slot or call `Update.end(true)` before both the
   SHA-256 and the signature check pass.
2. Never write to the running slot.
3. Keep the registration token and the public key in `tinyota_config.h`,
   not scattered in source. Do not commit real tokens.
4. The device serial must be stable across reboots (eFuse MAC, chip ID).
5. Registering again with the same serial returns the same device and a
   fresh token, so the simplest correct approach is to call register on
   every boot (limit: 5 per minute). Device tokens expire after 365 days:
   if an API call returns 401, register again.
6. Check for updates at most every 5 minutes (the API is rate limited to
   30 requests per minute).
7. On MCUboot, call `boot_write_img_confirmed()` after the new image boots
   and passes its self-test. On ESP-IDF with rollback enabled, call
   `esp_ota_mark_app_valid_cancel_rollback()`.
8. Release builds: TLS certificate verification on, `FOTA_ALLOW_UNSIGNED`
   off.

## 5. What to upload to the console

| Environment | File |
|---|---|
| Arduino IDE | Sketch > Export Compiled Binary, then `<sketch>.ino.bin` (not the `.merged.bin` or `.bootloader.bin`) |
| PlatformIO | `.pio/build/<env>/firmware.bin` |
| ESP-IDF | `build/<project>.bin` |
| Zephyr + MCUboot | `zephyr.signed.bin` (`build/<app>/zephyr/` with sysbuild, `build/zephyr/` without) |
| Linux | the application binary or bundle the agent installs |

Upload with the same version string as `FW_VERSION` in the new build.

## 6. When you finish

List: the files you changed, values the developer must fill in
(`TINYOTA_REG_TOKEN`, `TINYOTA_PUBKEY_PEM`, board settings), assumptions
you made, and which items of `docs/CONFORMANCE_CHECKLIST.md` need a manual
test on hardware.
