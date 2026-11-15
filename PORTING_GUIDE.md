# FOTA SDK — Porting Guide

This guide explains how to integrate the FOTA SDK into your embedded product.
Partner delivery assumes you received public headers, a prebuilt SDK library, and the documents in this repository.
After completing the steps below your device will be able to:

1. Register itself with the FOTA Cloud server.
2. Check for firmware updates, receiving a DELTA or FULL payload decision.
3. Download the payload with automatic resume (HTTP Range requests).
4. Apply delta patches in-place using A/B partition streaming.
5. Verify the ECDSA signature before switching the boot slot.
6. Reboot into the new firmware without any downtime to the running slot.

---

## Prerequisites

Start with the minimum set first. DELTA optimization can be added later.

### Required (must have)

| Requirement | Why you need it |
|:---|:---|
| C99 compiler | Build the SDK (`GCC`, `Clang`, `IAR`, `ARMCC`) |
| A/B flash partitions | One active slot + one inactive slot for safe updates |
| HTTPS client stack | Call FOTA API and download firmware securely |
| SHA-256 + ECDSA verify | Validate firmware integrity/signature before slot switch |

### RAM budget (current SDK)

The FSM downloads the whole payload into a heap buffer sized to the inactive
partition, and the signature check reads the whole image back into RAM.
Plan for **free heap ≥ firmware image size** (plus TLS buffers). This fits
ESP32 with PSRAM and Linux targets; on small MCUs keep the image small or
wait for the streaming verifier on the roadmap.

### Optional (only for DELTA updates)

| Requirement | When needed |
|:---|:---|
| ~256 KB free RAM (recommended) | Recommended for DELTA patching on-device |
| `fota_bspatch()` implementation | Only the POSIX port ships one (bzip2) |

If you want the easiest first integration, run **FULL-only** updates first and enable DELTA later.
Ports without `fota_bspatch()` get a weak default from `src/fota_common.c`
that returns -1. Set `.ram_free_kb = 0` in `fota_config_t` so the server
always sends FULL payloads.

### What the SDK checks for you

- SHA-256 of the downloaded payload must match the server `checksum`
  before anything is written to flash (`FOTA_ERR_HASH_MISMATCH`).
- The ECDSA P-256 signature over the final image must verify before the
  boot slot changes (`FOTA_ERR_SIG_INVALID`). A missing checksum or
  signature is a failure. Only define `FOTA_ALLOW_UNSIGNED` in local
  development builds.
- Every failure is reported to `POST /api/v1/devices/status` as `failed`
  with the FOTA error code, and `success` is reported right before the
  reboot into the new image. You can also call `fota_report_status()`.

---

## Step 1 — Add the SDK package to your build

Add the installed SDK package to your firmware build:

- include `include/fota/*.h`
- link the provided `fota_sdk` library
- keep SDK implementation sources outside your product tree

Typical install tree:

```text
dist/
├── include/fota/
├── lib/
└── bin/   # optional reference utilities
```

---

## Step 2 — Implement the HAL (`fota_hal.h`)

Create a file `fota_hal_<platform>.c` and implement **all** functions declared in `include/fota_hal.h`:

```c
#include "fota_hal.h"
#include "your_flash_driver.h"

/* Partition base addresses — adapt to your flash layout */
#define SLOT_ACTIVE_BASE    0x08020000UL
#define SLOT_INACTIVE_BASE  0x08060000UL
#define SLOT_SIZE           0x00040000UL   /* 256 KB each */
#define SECTOR_SIZE         0x00001000UL   /* 4 KB erase sector */

static uint32_t _base(int slot) {
    return (slot == FOTA_SLOT_ACTIVE) ? SLOT_ACTIVE_BASE : SLOT_INACTIVE_BASE;
}

int hal_flash_read(int slot, uint32_t offset, uint8_t *buf, size_t len) {
    return your_flash_read(_base(slot) + offset, buf, len) == 0
           ? FOTA_HAL_OK : FOTA_HAL_ERR_IO;
}

int hal_flash_write(int slot, uint32_t offset, const uint8_t *buf, size_t len) {
    if (slot == FOTA_SLOT_ACTIVE) return FOTA_HAL_ERR_PROTECTED;
    return your_flash_write(_base(slot) + offset, buf, len) == 0
           ? FOTA_HAL_OK : FOTA_HAL_ERR_IO;
}

int hal_flash_erase(int slot, uint32_t offset, size_t len) {
    if (slot == FOTA_SLOT_ACTIVE) return FOTA_HAL_ERR_PROTECTED;
    return your_flash_erase(_base(slot) + offset, len) == 0
           ? FOTA_HAL_OK : FOTA_HAL_ERR_IO;
}

int hal_set_boot_active_slot(int slot) {
    /* Write to your bootloader's boot-flag register/storage */
    return your_bootloader_set_next_boot(slot) == 0
           ? FOTA_HAL_OK : FOTA_HAL_ERR_IO;
}

void hal_system_reboot(void) {
    NVIC_SystemReset();   /* Cortex-M */
}

uint32_t hal_flash_sector_size(void) { return SECTOR_SIZE; }
uint32_t hal_flash_partition_size(int slot) { (void)slot; return SLOT_SIZE; }
```

---

## Step 3 — Implement the platform seams

The FSM calls these external functions (declared in `fota_fsm.c`):

### `fota_http_get()`

```c
int fota_http_get(const char *url, const char *bearer_token,
                  uint32_t range_start,
                  uint8_t *out_buf, size_t buf_size)
{
    https_request_t req = {0};
    req.method = HTTP_GET;
    req.url    = url;
    snprintf(req.auth_header, sizeof(req.auth_header),
             "Authorization: Bearer %s", bearer_token);
    if (range_start > 0)
        snprintf(req.range_header, sizeof(req.range_header),
                 "Range: bytes=%u-", (unsigned)range_start);

    int n = https_send_request(&req, out_buf, buf_size);
    return n;   /* bytes written, or negative on error */
}
```

### `fota_http_post_json()`

```c
int fota_http_post_json(const char *url, const char *bearer_token,
                        const char *json_body,
                        char *resp_buf, size_t resp_buf_size)
{
    https_request_t req = {0};
    req.method       = HTTP_POST;
    req.url          = url;
    req.content_type = "application/json";
    req.body         = json_body;
    snprintf(req.auth_header, sizeof(req.auth_header),
             "Authorization: Bearer %s", bearer_token);

    int status = https_send_request(&req, (uint8_t *)resp_buf, resp_buf_size);
    return (status >= 200 && status < 300) ? 0 : -1;
}
```

### `fota_bspatch()` (optional for FULL-only)

Use this only when you enable DELTA updates. No separate partner-side runtime setup is needed.

```c
#include "bspatch.h"

int fota_bspatch(const uint8_t *delta, size_t delta_len) {
    /* bspatch reads old from SLOT_ACTIVE via hal_flash_read,
       writes new to SLOT_INACTIVE via hal_flash_write */
    return bspatch_apply(delta, delta_len,
                         hal_flash_read,  FOTA_SLOT_ACTIVE,
                         hal_flash_write, FOTA_SLOT_INACTIVE);
}
```

### `fota_ecdsa_verify()`

```c
#include "mbedtls/ecdsa.h"
#include "mbedtls/sha256.h"

/* Embed the server public key PEM — fetch once from GET /api/v1/devices/public-key */
static const char *SERVER_PUBLIC_KEY_PEM = "-----BEGIN PUBLIC KEY-----\n...";

int fota_ecdsa_verify(const uint8_t *data, size_t data_len,
                       const char *sig_b64)
{
    /* 1. SHA-256 hash of data */
    uint8_t hash[32];
    mbedtls_sha256(data, data_len, hash, 0);

    /* 2. base64url-decode sig_b64 → DER bytes */
    uint8_t sig_der[128];
    size_t sig_len;
    base64url_decode(sig_b64, sig_der, sizeof(sig_der), &sig_len);

    /* 3. Verify with mbedTLS */
    mbedtls_ecdsa_context ecdsa;
    mbedtls_ecdsa_init(&ecdsa);
    /* load SERVER_PUBLIC_KEY_PEM into ecdsa ... */
    int rc = mbedtls_ecdsa_read_signature(&ecdsa, hash, 32, sig_der, sig_len);
    mbedtls_ecdsa_free(&ecdsa);
    return (rc == 0) ? 1 : 0;
}
```

### `fota_sha256()`

```c
#include "mbedtls/sha256.h"
void fota_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    mbedtls_sha256(data, len, out, 0);
}
```

---

### JSON field extraction helpers

Implement the JSON helper functions declared in `fota_platform.h`. A minimal implementation can use a lightweight parser such as [cJSON](https://github.com/DaveGamble/cJSON) or [jsmn](https://github.com/zserge/jsmn):

```c
int fota_json_get_string(const char *json,
                         const char *key,
                         char *out,
                         size_t out_size)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *field = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsString(field) || field->valuestring == NULL) {
        cJSON_Delete(root);
        return -1;
    }

    strncpy(out, field->valuestring, out_size - 1);
    out[out_size - 1] = '\0';
    cJSON_Delete(root);
    return 0;
}
```

---

## Step 4 — Drive the FSM from your application task

```c
#include "fota_fsm.h"

void fota_task(void *arg) {
    fota_config_t cfg = {
        .server_base_url = "https://api.fota.example.com",
        .device_token    = nvs_get_device_token(),
        .current_version = APP_VERSION_STRING,
        .ram_free_kb     = heap_free_kb(),
        .storage_free_kb = flash_free_kb(),
        .on_state_change = my_state_logger,
        .on_progress     = my_progress_ui,
    };

    fota_ctx_t *ctx = fota_fsm_init(&cfg);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(6 * 60 * 60 * 1000));  /* check every 6 h */
        fota_fsm_trigger(ctx);

        while (fota_fsm_state(ctx) != FOTA_STATE_IDLE) {
            fota_fsm_run(ctx);
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (fota_fsm_last_error(ctx) == FOTA_ERR_NO_UPDATE) {
            log_info("Firmware up to date.");
        }
    }
}
```

---

## Security checklist (minimum)

- [ ] `ECDSA_PRIVATE_KEY_PEM` is set on the server.
- [ ] Device verifies ECDSA signature before switching boot slot.
- [ ] `hal_flash_write(FOTA_SLOT_ACTIVE, ...)` always returns `FOTA_HAL_ERR_PROTECTED`.
- [ ] `hal_set_boot_active_slot()` is called only after verification succeeds.

---

## Additional integration documents

- `docs/SDK_PARTNER_DISTRIBUTION.md` — partner package boundaries and delivery profile
- `docs/HAL_CONTRACT.md` — required HAL behavior contract
- `docs/CONFORMANCE_CHECKLIST.md` — validation checklist before production rollout

---

## File reference

| File | Purpose |
|:---|:---|
| `include/fota_hal.h` | HAL interface — implement this |
| `include/fota_fsm.h` | FSM public API |
| `libfota_sdk.*` | Prebuilt SDK library linked into your firmware build |
| `PORTING_GUIDE.md` | This document |

---

## Platform Ports

Reference platform implementations may be used during bring-up, but partner integration should link the prebuilt `fota_sdk` library and add only the platform-specific HAL/platform seam sources needed for the target.

---

### ESP32-C3 / ESP32-S3 (ESP-IDF)

Files: `src/fota_hal_esp32.c`, `src/fota_platform_esp32.c`

**Checklist before building:**

1. Partition table — add OTA partitions in `partitions.csv` or use the
   built-in "Two OTA definitions" template (`CONFIG_PARTITION_TABLE_TWO_OTA=y`).
   Its slots are 1 MB; an app with Wi-Fi + TLS usually needs a custom table
   like `examples/esp-idf/partitions.csv`.
2. Replace the `SERVER_PUBKEY_PEM` placeholder in `fota_platform_esp32.c`
   with your production ECDSA P-256 public key.
3. Register your CMake component:

```cmake
idf_component_register(
    SRCS
        "../src/fota_hal_esp32.c"
        "../src/fota_platform_esp32.c"
        "../src/fota_fsm.c"
        "../src/fota_common.c"
    INCLUDE_DIRS "../include"
    REQUIRES esp_http_client mbedtls nvs_flash esp_partition bootloader_support
)
```

4. Drive the FSM from your `app_main` task (FreeRTOS):

```c
#include "fota_fsm.h"
#include "nvs_flash.h"

void fota_task(void *arg)
{
    fota_config_t cfg = {
        .server_base_url = "https://api.tinyota.xyz",
        .device_token    = nvs_get_device_token(),   /* your NVS helper */
        .current_version = APP_VERSION_STRING,
        .ram_free_kb     = 0,   /* FULL-only port: no fota_bspatch() */
    };
    fota_ctx_t *ctx = fota_fsm_init(&cfg);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(6UL * 3600UL * 1000UL));
        fota_fsm_trigger(ctx);
        while (fota_fsm_state(ctx) != FOTA_STATE_IDLE) {
            fota_fsm_run(ctx);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

void app_main(void)
{
    nvs_flash_init();
    /* ... Wi-Fi init ... */
    xTaskCreate(fota_task, "fota", 8192, NULL, 5, NULL);
}
```

5. Enable recommended boot settings in `sdkconfig`:

```
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_BOOTLOADER_WDT_ENABLE=y
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y
```

6. Build and flash (example):

```bash
idf.py set-target esp32c3      # or esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

**Serial number source:** eFuse MAC address via `esp_efuse_mac_get_default()`.
No extra configuration needed.

**Bring-up validation (ESP32):**
- [ ] Device registers and receives a device token.
- [ ] FULL update path writes only to inactive OTA slot.
- [ ] Wrong checksum and wrong signature both fail without switching slots.
- [ ] Console Devices page shows the `success` / `failed` status reports.
- [ ] Power-cycle after `hal_set_boot_active_slot()` boots new firmware.
- [ ] Failed signature test does not switch active boot slot.

---

### Nordic nRF52840 (Zephyr + MCUboot)

Files: `src/fota_hal_nrf52.c`, `src/fota_platform_nrf52.c`

**Checklist before building:**

1. Enable MCUboot as the bootloader:

```
# prj.conf
CONFIG_BOOTLOADER_MCUBOOT=y
CONFIG_IMG_MANAGER=y
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_STREAM_FLASH=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_SOCKETS_POSIX_NAMES=y
CONFIG_HTTP_CLIENT=y
CONFIG_MBEDTLS=y
CONFIG_MBEDTLS_BUILTIN=y
```

2. Register a TLS CA certificate for the hosted API at tag `42`:

```c
/* call once at boot, before fota_fsm_init() */
tls_credential_add(42, TLS_CREDENTIAL_CA_CERTIFICATE,
                   ca_cert_pem, strlen(ca_cert_pem) + 1);
```

3. Replace `SERVER_PUBKEY_PEM` placeholder in `fota_platform_nrf52.c`.

4. Add sources to `CMakeLists.txt`:

```cmake
target_sources(app PRIVATE
    src/fota_hal_nrf52.c
    src/fota_platform_nrf52.c
    src/fota_fsm.c
    src/fota_common.c
)
target_include_directories(app PRIVATE include)
```

5. After successful boot from the new slot, confirm the upgrade to prevent
   MCUboot from reverting:

```c
/* Call this once from your application after verifying the new firmware works */
boot_write_img_confirmed();
```

6. Drive the FSM from a Zephyr thread:

```c
#include "fota_fsm.h"

K_THREAD_STACK_DEFINE(fota_stack, 4096);
static struct k_thread fota_thread_data;

static void fota_thread(void *a, void *b, void *c)
{
    fota_config_t cfg = {
        .server_base_url = "https://api.tinyota.xyz",
        .device_token    = settings_get_device_token(),
        .current_version = APP_VERSION_STRING,
        .ram_free_kb     = 0,   /* FULL-only port: no fota_bspatch() */
    };
    fota_ctx_t *ctx = fota_fsm_init(&cfg);
    while (1) {
        k_sleep(K_HOURS(6));
        fota_fsm_trigger(ctx);
        while (fota_fsm_state(ctx) != FOTA_STATE_IDLE) {
            fota_fsm_run(ctx);
            k_sleep(K_MSEC(20));
        }
    }
}

void main(void)
{
    /* ... network init (Wi-Fi companion or modem) ... */
    k_thread_create(&fota_thread_data, fota_stack,
                    K_THREAD_STACK_SIZEOF(fota_stack),
                    fota_thread, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
}
```

7. Example build command (nRF52840 DK):

```bash
west build -b nrf52840dk_nrf52840 app \
    -- -DCONFIG_BOOTLOADER_MCUBOOT=y
west flash
```

**Serial number source:** FICR DEVICEID registers (factory-programmed 64-bit UID).
No extra configuration needed.

**Network note:** nRF52840 has no direct IP stack.
Supported options out of the box:
- nRF7002 Wi-Fi companion module (nRF Connect SDK ≥ 2.5)
- UART AT modem via `CONFIG_MODEM_GSM_PPP` or similar

**Bring-up validation (nRF52840):**
- [ ] TLS CA certificate is loaded before first HTTP call.
- [ ] `boot_request_upgrade()` is triggered only after verification passes.
- [ ] `boot_write_img_confirmed()` is called after successful first boot.
- [ ] Reboot fallback works when new image is not confirmed.
- [ ] Both Wi-Fi/modem network paths can complete `/devices/check`.

