# HAL Contract

This contract is the minimum implementation scope required from integrators.

## Required Functions (must implement)

From include/fota_hal.h:

- int hal_flash_read(int slot, uint32_t offset, uint8_t *buf, size_t len)
- int hal_flash_write(int slot, uint32_t offset, const uint8_t *buf, size_t len)
- int hal_flash_erase(int slot, uint32_t offset, size_t len)
- int hal_set_boot_active_slot(int slot)
- void hal_system_reboot(void)
- int hal_get_serial(char *out, size_t out_size)

## Optional/Informational Functions (recommended)

- uint32_t hal_flash_sector_size(void)
- uint32_t hal_flash_partition_size(int slot)

## Behavioral Requirements

1. Slot safety
- Writing or erasing active slot during update must be blocked.
- Return FOTA_HAL_ERR_PROTECTED when active slot write/erase is attempted.

2. Bounds and alignment
- Reject out-of-range operations with FOTA_HAL_ERR_PARAM.
- Reject unaligned erase requests with FOTA_HAL_ERR_ALIGN.

3. Deterministic error mapping
- Flash I/O failures must map to FOTA_HAL_ERR_IO.
- Timeouts must map to FOTA_HAL_ERR_TIMEOUT.

4. Boot switch safety
- hal_set_boot_active_slot() must only change boot target metadata.
- No destructive erase/write outside boot-control area.

5. Reboot behavior
- hal_system_reboot() must not return.

6. Serial identity
- hal_get_serial() must return a stable hardware-derived identifier.
- Do not use compile-time constants in production.

## Platform Seams Required with HAL

From include/fota_platform.h:

- fota_http_get
- fota_http_post_json
- fota_sha256
- fota_ecdsa_verify
- fota_json_get_string / fota_json_get_int / fota_json_get_bool
- fota_bspatch (required only for DELTA support)

## Validation Entry Criteria

Before release candidate sign-off:

- All required HAL APIs implemented.
- FULL update path passes end-to-end test.
- Signature verification path verified with real public key.
- Recovery path tested for network interruption and reboot during update.
