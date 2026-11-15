/**
 * @file fota_hal_nrf52.c
 * @brief FOTA HAL implementation for Nordic nRF52840 (Zephyr RTOS)
 *
 * A/B slot model: managed by MCUboot.
 *   Slot 0 (primary / active)   — currently running image
 *   Slot 1 (secondary / update) — incoming firmware target
 *
 * Prerequisites:
 *   - Zephyr v3.4+ with MCUboot as the bootloader
 *   - CONFIG_FLASH=y
 *   - CONFIG_FLASH_MAP=y
 *   - CONFIG_IMG_MANAGER=y        (boot_write_img_confirmed etc.)
 *   - CONFIG_MCUBOOT_BOOTLOADER=y or CONFIG_BOOTLOADER_MCUBOOT=y
 *
 * Partition layout (boards/nrf52840dk_nrf52840.conf typical):
 *   FLASH_AREA_ID(image_0)  — primary   (slot 0)
 *   FLASH_AREA_ID(image_1)  — secondary (slot 1)
 *
 * Add to CMakeLists.txt:
 *   target_sources(app PRIVATE fota_hal_nrf52.c)
 *
 * Do NOT use fota_hal_posix.c alongside this file.
 */

#include "../include/fota_hal.h"

#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_ficr.h>   /* nRF hardware UID registers */

#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(fota_hal, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* Flash area IDs (MCUboot primary/secondary)                          */
/* ------------------------------------------------------------------ */
#define SLOT0_AREA_ID  FLASH_AREA_ID(image_0)
#define SLOT1_AREA_ID  FLASH_AREA_ID(image_1)

/* nRF52840 uses 4 KB erase sectors */
#define NRF52_SECTOR_SIZE  4096U

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static int get_area_id(int slot)
{
    if (slot == FOTA_SLOT_ACTIVE)   return SLOT0_AREA_ID;
    if (slot == FOTA_SLOT_INACTIVE) return SLOT1_AREA_ID;
    return -1;
}

/**
 * Open a flash area.  Caller must call flash_area_close() when done.
 */
static int open_area(int slot, const struct flash_area **fa)
{
    int id = get_area_id(slot);
    if (id < 0) return FOTA_HAL_ERR_PARAM;

    int rc = flash_area_open(id, fa);
    if (rc) {
        LOG_ERR("flash_area_open(slot=%d) failed: %d", slot, rc);
        return FOTA_HAL_ERR_IO;
    }
    return FOTA_HAL_OK;
}

/* ------------------------------------------------------------------ */
/* hal_flash_read                                                       */
/* ------------------------------------------------------------------ */
int hal_flash_read(int slot, uint32_t offset, uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return FOTA_HAL_ERR_PARAM;

    const struct flash_area *fa = NULL;
    int rc = open_area(slot, &fa);
    if (rc) return rc;

    rc = flash_area_read(fa, offset, buf, len);
    flash_area_close(fa);

    if (rc) {
        LOG_ERR("hal_flash_read: flash_area_read err=%d", rc);
        return FOTA_HAL_ERR_IO;
    }
    return FOTA_HAL_OK;
}

/* ------------------------------------------------------------------ */
/* hal_flash_write                                                      */
/* ------------------------------------------------------------------ */
int hal_flash_write(int slot, uint32_t offset, const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return FOTA_HAL_ERR_PARAM;
    if (slot == FOTA_SLOT_ACTIVE)  return FOTA_HAL_ERR_PROTECTED;

    const struct flash_area *fa = NULL;
    int rc = open_area(slot, &fa);
    if (rc) return rc;

    rc = flash_area_write(fa, offset, buf, len);
    flash_area_close(fa);

    if (rc) {
        LOG_ERR("hal_flash_write: flash_area_write err=%d", rc);
        return FOTA_HAL_ERR_IO;
    }
    return FOTA_HAL_OK;
}

/* ------------------------------------------------------------------ */
/* hal_flash_erase                                                      */
/* ------------------------------------------------------------------ */
int hal_flash_erase(int slot, uint32_t offset, size_t len)
{
    if (slot == FOTA_SLOT_ACTIVE) return FOTA_HAL_ERR_PROTECTED;

    const struct flash_area *fa = NULL;
    int rc = open_area(slot, &fa);
    if (rc) return rc;

    rc = flash_area_erase(fa, offset, len);
    flash_area_close(fa);

    if (rc) {
        LOG_ERR("hal_flash_erase: flash_area_erase err=%d", rc);
        return FOTA_HAL_ERR_IO;
    }
    return FOTA_HAL_OK;
}

/* ------------------------------------------------------------------ */
/* hal_set_boot_active_slot                                             */
/* ------------------------------------------------------------------ */
int hal_set_boot_active_slot(int slot)
{
    /*
     * MCUboot model:
     *   1. Mark the secondary (slot 1) image as a pending upgrade.
     *   2. MCUboot validates + swaps on next boot.
     *   3. Application calls boot_write_img_confirmed() after boot
     *      to confirm the upgrade is stable.
     *
     * This HAL only covers step 1 (request the swap).
     * The application layer is responsible for step 3.
     */
    if (slot != FOTA_SLOT_INACTIVE) {
        LOG_WRN("hal_set_boot_active_slot: only switching to slot 1 is supported");
        return FOTA_HAL_ERR_PARAM;
    }

    int rc = boot_request_upgrade(BOOT_UPGRADE_TEST);
    if (rc) {
        LOG_ERR("hal_set_boot_active_slot: boot_request_upgrade err=%d", rc);
        return FOTA_HAL_ERR_IO;
    }
    LOG_INF("MCUboot upgrade request set — will swap on next reboot");
    return FOTA_HAL_OK;
}

/* ------------------------------------------------------------------ */
/* hal_system_reboot                                                    */
/* ------------------------------------------------------------------ */
void hal_system_reboot(void)
{
    LOG_INF("Rebooting for firmware swap...");
    k_msleep(100); /* flush log buffer */
    sys_reboot(SYS_REBOOT_COLD);
    /* Does not return */
    while (1) { }
}

/* ------------------------------------------------------------------ */
/* hal_flash_sector_size                                                */
/* ------------------------------------------------------------------ */
uint32_t hal_flash_sector_size(void)
{
    return NRF52_SECTOR_SIZE;
}

/* ------------------------------------------------------------------ */
/* hal_flash_partition_size                                             */
/* ------------------------------------------------------------------ */
uint32_t hal_flash_partition_size(int slot)
{
    const struct flash_area *fa = NULL;
    if (open_area(slot, &fa) != FOTA_HAL_OK) return 0;

    uint32_t size = (uint32_t)fa->fa_size;
    flash_area_close(fa);
    return size;
}

/* ------------------------------------------------------------------ */
/* hal_get_serial                                                       */
/* ------------------------------------------------------------------ */
int hal_get_serial(char *out, size_t out_size)
{
    if (!out || out_size < 18) return FOTA_HAL_ERR_PARAM;

    /*
     * nRF52840 has a 64-bit DEVICEID in FICR registers (two 32-bit words).
     * This is factory-programmed and unique per chip.
     *
     * FICR.DEVICEID[0] and FICR.DEVICEID[1] total 8 bytes → 16 hex chars.
     */
    uint32_t id0 = nrf_ficr_deviceid_get(NRF_FICR, 0);
    uint32_t id1 = nrf_ficr_deviceid_get(NRF_FICR, 1);

    snprintf(out, out_size, "%08X%08X", (unsigned)id0, (unsigned)id1);
    return FOTA_HAL_OK;
}
