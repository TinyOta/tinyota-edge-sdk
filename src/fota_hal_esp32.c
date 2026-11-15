/**
 * @file fota_hal_esp32.c
 * @brief FOTA HAL implementation for ESP32-C3 / ESP32-S3 (ESP-IDF)
 *
 * Tested with: ESP-IDF v5.x
 * Supported chips: ESP32-C3, ESP32-S3 (and ESP32 series with OTA support)
 *
 * Prerequisites in sdkconfig / menuconfig:
 *   - CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y  (recommended)
 *   - Partition table must have ota_0 and ota_1 entries.
 *     Use partitions.csv or the built-in "Two OTA definitions" template.
 *
 * Include in your CMakeLists.txt:
 *   idf_component_register(SRCS "fota_hal_esp32.c" ...)
 *
 * Do NOT use fota_hal_posix.c alongside this file.
 */

#include "../include/fota_hal.h"

/* ESP-IDF headers — available after `idf.py` environment is sourced */
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_efuse.h"
#if defined(__has_include)
#  if __has_include("esp_mac.h")
#    include "esp_mac.h"      /* esp_efuse_mac_get_default() on ESP-IDF v5 */
#  endif
#endif
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "fota_hal";

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/**
 * Return the esp_partition_t pointer for a FOTA slot index.
 *
 * slot 0 (FOTA_SLOT_ACTIVE)   → currently running OTA partition
 * slot 1 (FOTA_SLOT_INACTIVE) → next OTA partition to write into
 */
static const esp_partition_t *get_partition(int slot)
{
    if (slot == FOTA_SLOT_ACTIVE) {
        return esp_ota_get_running_partition();
    } else if (slot == FOTA_SLOT_INACTIVE) {
        return esp_ota_get_next_update_partition(NULL);
    }
    ESP_LOGE(TAG, "get_partition: invalid slot %d", slot);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* hal_flash_read                                                       */
/* ------------------------------------------------------------------ */
int hal_flash_read(int slot, uint32_t offset, uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return FOTA_HAL_ERR_PARAM;

    const esp_partition_t *part = get_partition(slot);
    if (!part) return FOTA_HAL_ERR_PARAM;

    esp_err_t err = esp_partition_read(part, offset, buf, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hal_flash_read: esp_partition_read err=0x%x", err);
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
    if (slot == FOTA_SLOT_ACTIVE) return FOTA_HAL_ERR_PROTECTED;

    const esp_partition_t *part = get_partition(slot);
    if (!part) return FOTA_HAL_ERR_PARAM;

    /* ESP-IDF requires 4-byte alignment for writes; the SDK guarantees this */
    esp_err_t err = esp_partition_write(part, offset, buf, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hal_flash_write: esp_partition_write err=0x%x", err);
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

    const esp_partition_t *part = get_partition(slot);
    if (!part) return FOTA_HAL_ERR_PARAM;

    /* esp_partition_erase_range requires sector-aligned offset and len */
    esp_err_t err = esp_partition_erase_range(part, offset, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hal_flash_erase: esp_partition_erase_range err=0x%x", err);
        return FOTA_HAL_ERR_IO;
    }
    return FOTA_HAL_OK;
}

/* ------------------------------------------------------------------ */
/* hal_set_boot_active_slot                                             */
/* ------------------------------------------------------------------ */
int hal_set_boot_active_slot(int slot)
{
    /* Only switching to the inactive (newly written) slot makes sense */
    if (slot != FOTA_SLOT_INACTIVE) {
        ESP_LOGW(TAG, "hal_set_boot_active_slot: slot %d is already active or invalid", slot);
        return FOTA_HAL_ERR_PARAM;
    }

    const esp_partition_t *part = get_partition(FOTA_SLOT_INACTIVE);
    if (!part) return FOTA_HAL_ERR_PARAM;

    esp_err_t err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hal_set_boot_active_slot: esp_ota_set_boot_partition err=0x%x", err);
        return FOTA_HAL_ERR_IO;
    }
    ESP_LOGI(TAG, "Boot partition set to: %s", part->label);
    return FOTA_HAL_OK;
}

/* ------------------------------------------------------------------ */
/* hal_system_reboot                                                    */
/* ------------------------------------------------------------------ */
void hal_system_reboot(void)
{
    ESP_LOGI(TAG, "Initiating system restart for firmware switch...");
    esp_restart();
    /* esp_restart() does not return */
    while (1) { }
}

/* ------------------------------------------------------------------ */
/* hal_flash_sector_size                                                */
/* ------------------------------------------------------------------ */
uint32_t hal_flash_sector_size(void)
{
    /* SPI NOR flash on ESP32 series uses 4 KB erase sectors */
    return 4096U;
}

/* ------------------------------------------------------------------ */
/* hal_flash_partition_size                                             */
/* ------------------------------------------------------------------ */
uint32_t hal_flash_partition_size(int slot)
{
    const esp_partition_t *part = get_partition(slot);
    if (!part) return 0;
    return (uint32_t)part->size;
}

/* ------------------------------------------------------------------ */
/* hal_get_serial                                                       */
/* ------------------------------------------------------------------ */
int hal_get_serial(char *out, size_t out_size)
{
    if (!out || out_size < 18) return FOTA_HAL_ERR_PARAM;

    /*
     * ESP32 chips expose a unique 48-bit MAC address burned into eFuse.
     * We format it as a 12-character hex string (no colons) to keep it
     * compact and URL-safe for use as a device identifier.
     *
     * Alternative: use esp_efuse_mac_get_custom() for a custom eFuse field.
     */
    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hal_get_serial: esp_efuse_mac_get_default err=0x%x", err);
        return FOTA_HAL_ERR_IO;
    }

    snprintf(out, out_size, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return FOTA_HAL_OK;
}
