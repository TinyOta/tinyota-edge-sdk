/**
 * @file fota_hal.h
 * @brief FOTA Hardware Abstraction Layer (HAL) — C99 Interface
 *
 * Manufacturers MUST implement every function in this header to port the
 * FOTA SDK to their platform.  The SDK core calls these functions; it never
 * accesses flash memory or hardware directly.
 *
 * Conformance:
 *   - C99 (no C++ features).
 *   - Thread-safety: the SDK calls HAL functions from a single task/thread
 *     unless otherwise noted.  Implementations need not be re-entrant unless
 *     the RTOS scheduler can preempt between HAL calls.
 *
 * Return values:
 *   - FOTA_HAL_OK  (0): success.
 *   - Negative values: error codes defined below.
 *
 * Partition identifiers (slot):
 *   - FOTA_SLOT_ACTIVE   (0): currently running firmware partition.
 *   - FOTA_SLOT_INACTIVE (1): the other A/B partition (update target).
 */

#ifndef FOTA_HAL_H
#define FOTA_HAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Return codes                                                         */
/* ------------------------------------------------------------------ */
#define FOTA_HAL_OK             0
#define FOTA_HAL_ERR_IO        -1   /**< Flash read/write/erase failure. */
#define FOTA_HAL_ERR_PARAM     -2   /**< Invalid parameter (NULL pointer, bad offset/len). */
#define FOTA_HAL_ERR_ALIGN     -3   /**< Offset or length not aligned to flash sector. */
#define FOTA_HAL_ERR_PROTECTED -4   /**< Flash region is write-protected. */
#define FOTA_HAL_ERR_TIMEOUT   -5   /**< Operation timed out. */

/* ------------------------------------------------------------------ */
/* Partition (slot) identifiers                                         */
/* ------------------------------------------------------------------ */
#define FOTA_SLOT_ACTIVE    0  /**< Currently booted firmware slot. */
#define FOTA_SLOT_INACTIVE  1  /**< Target slot for the incoming update. */

/* ------------------------------------------------------------------ */
/* Flash I/O                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Read bytes from a flash partition.
 *
 * @param slot    FOTA_SLOT_ACTIVE or FOTA_SLOT_INACTIVE.
 * @param offset  Byte offset within the partition (0 = start of partition).
 * @param buf     Destination buffer (must be at least @p len bytes).
 * @param len     Number of bytes to read.
 * @return FOTA_HAL_OK on success, negative error code on failure.
 */
int hal_flash_read(int slot, uint32_t offset, uint8_t *buf, size_t len);

/**
 * @brief Write bytes to a flash partition.
 *
 * The target sector(s) MUST have been erased via hal_flash_erase() first.
 *
 * @param slot    Must be FOTA_SLOT_INACTIVE (writing to the active slot is
 *                not permitted during an update).
 * @param offset  Byte offset within the partition.
 * @param buf     Source buffer.
 * @param len     Number of bytes to write.
 * @return FOTA_HAL_OK on success, negative error code on failure.
 */
int hal_flash_write(int slot, uint32_t offset, const uint8_t *buf, size_t len);

/**
 * @brief Erase flash sectors in a partition.
 *
 * Both @p offset and @p len must be aligned to the platform's erase-sector
 * size.  The SDK always erases before writing.
 *
 * @param slot    Must be FOTA_SLOT_INACTIVE.
 * @param offset  Byte offset of the first sector to erase.
 * @param len     Total number of bytes to erase (sector-aligned).
 * @return FOTA_HAL_OK on success, negative error code on failure.
 */
int hal_flash_erase(int slot, uint32_t offset, size_t len);

/* ------------------------------------------------------------------ */
/* Boot control                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Set the boot-priority slot.
 *
 * Called ONLY after the inactive slot has been written AND the ECDSA
 * signature verified.  The system will boot from @p slot on the next
 * reset.
 *
 * @param slot  FOTA_SLOT_ACTIVE or FOTA_SLOT_INACTIVE.
 * @return FOTA_HAL_OK on success, negative error code on failure.
 */
int hal_set_boot_active_slot(int slot);

/**
 * @brief Trigger a system reboot.
 *
 * This function must not return.  The implementation should perform any
 * required hardware finalisation before issuing the reset instruction.
 */
void hal_system_reboot(void);

/* ------------------------------------------------------------------ */
/* Optional / informational                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Return the flash erase-sector size in bytes.
 *
 * The SDK uses this to align erase operations.  Return 0 if the platform
 * erases in variable-sized chunks (the SDK will use a conservative 4 KB).
 *
 * @return Erase sector size in bytes (e.g. 4096, 65536), or 0.
 */
uint32_t hal_flash_sector_size(void);

/**
 * @brief Return the total size of one firmware partition in bytes.
 *
 * Used by the SDK to validate that an incoming firmware image fits.
 *
 * @param slot  FOTA_SLOT_ACTIVE or FOTA_SLOT_INACTIVE.
 * @return Partition size in bytes, or 0 if unknown.
 */
uint32_t hal_flash_partition_size(int slot);

/**
 * @brief Read the hardware serial number of this device.
 *
 * The serial number uniquely identifies the physical unit and is used
 * during device registration with the FOTA server.  The implementation
 * must read from a hardware source — do NOT use a compile-time constant
 * or an environment variable in production.
 *
 * Platform examples:
 *   - STM32:      Read 96-bit UID from 0x1FF0F420, format as hex string.
 *   - Linux:      Read /proc/cpuinfo "Serial", /etc/machine-id, or
 *                 a NIC MAC address via ioctl(SIOCGIFHWADDR).
 *   - macOS:      Use IOKit IOPlatformSerialNumber.
 *   - FreeRTOS:   Read MCU UID register via platform HAL.
 *
 * @param out       Buffer to receive the null-terminated serial string.
 * @param out_size  Size of @p out in bytes (including null terminator).
 *                  A minimum of 64 bytes is recommended.
 * @return FOTA_HAL_OK on success, negative error code on failure.
 */
int hal_get_serial(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* FOTA_HAL_H */
