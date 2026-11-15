/**
 * @file fota_hal_posix.c
 * @brief POSIX HAL — file-backed flash simulation for development and testing.
 *
 * Each A/B firmware slot is represented by a regular file:
 *
 *   Slot 0 (active):    /tmp/fota_slot_0.bin
 *   Slot 1 (inactive):  /tmp/fota_slot_1.bin
 *
 * The current boot-target slot number is stored as a single ASCII digit in:
 *
 *   /tmp/fota_boot_slot
 *
 * This implementation is intentionally simple.  Its purpose is to allow
 * the FOTA FSM and platform code to be exercised on a Linux/macOS
 * developer workstation without any physical flash hardware.
 *
 * Do NOT use this HAL in production firmware — replace it with a
 * platform-specific implementation that drives your real flash driver.
 *
 * Build:
 *   gcc -std=c99 -c fota_hal_posix.c -o fota_hal_posix.o
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* SIOCGIFHWADDR is Linux glibc only — not available on musl (Alpine) */
#if defined(__linux__) && defined(__GLIBC__)
#  include <linux/if.h>
#  include <linux/sockios.h>
#  define FOTA_HAVE_SIOCGIFHWADDR 1
#endif

#include "../include/fota_hal.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

/** Erase-sector size simulated by this HAL (matches a typical NOR flash). */
#define POSIX_HAL_SECTOR_SIZE       4096U

/** Simulated partition size per slot (4 MB). */
#define POSIX_HAL_PARTITION_SIZE    (4U * 1024U * 1024U)

/** Erased-byte value as per NOR flash convention. */
#define POSIX_HAL_ERASED_BYTE       0xFFU

static const char *slot_paths[2] = {
    "/tmp/fota_slot_0.bin",
    "/tmp/fota_slot_1.bin"
};

static const char *boot_slot_path = "/tmp/fota_boot_slot";

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/**
 * Open (and optionally create) the slot file.
 * @p mode must be one of "rb", "r+b", or "wb".
 * When @p create_if_missing is non-zero and the file does not exist,
 * a new file pre-filled with 0xFF is created.
 */
static FILE *open_slot_file(int slot, const char *mode, int create_if_missing)
{
    if (slot < 0 || slot > 1) {
        fprintf(stderr, "[hal] invalid slot %d\n", slot);
        return NULL;
    }

    const char *path = slot_paths[slot];
    FILE *fp = fopen(path, mode);

    if (!fp && create_if_missing) {
        /* Create the file, fill with 0xFF to simulate erased flash */
        fp = fopen(path, "wb");
        if (!fp) {
            perror("[hal] fopen create");
            return NULL;
        }

        uint8_t sector[POSIX_HAL_SECTOR_SIZE];
        memset(sector, POSIX_HAL_ERASED_BYTE, sizeof(sector));
        size_t sectors = POSIX_HAL_PARTITION_SIZE / POSIX_HAL_SECTOR_SIZE;
        for (size_t i = 0; i < sectors; i++) {
            if (fwrite(sector, 1, sizeof(sector), fp) != sizeof(sector)) {
                perror("[hal] fwrite init");
                fclose(fp);
                return NULL;
            }
        }
        fclose(fp);

        /* Re-open with requested mode (must be "r+b" for read-write) */
        fp = fopen(path, mode);
        if (!fp) {
            perror("[hal] fopen after create");
        }
    }

    return fp;
}

/* ------------------------------------------------------------------ */
/* hal_flash_read                                                       */
/* ------------------------------------------------------------------ */
int hal_flash_read(int slot, uint32_t offset, uint8_t *buf, size_t len)
{
    if (!buf || len == 0) {
        return FOTA_HAL_ERR_PARAM;
    }
    if (offset + len > POSIX_HAL_PARTITION_SIZE) {
        fprintf(stderr, "[hal_flash_read] read beyond partition end "
                "(offset=%u len=%zu)\n", (unsigned)offset, len);
        return FOTA_HAL_ERR_PARAM;
    }

    FILE *fp = open_slot_file(slot, "rb", 1);
    if (!fp) {
        return FOTA_HAL_ERR_IO;
    }

    if (fseek(fp, (long)offset, SEEK_SET) != 0) {
        perror("[hal_flash_read] fseek");
        fclose(fp);
        return FOTA_HAL_ERR_IO;
    }

    size_t n = fread(buf, 1, len, fp);
    fclose(fp);

    if (n != len) {
        fprintf(stderr, "[hal_flash_read] short read: wanted %zu got %zu\n",
                len, n);
        return FOTA_HAL_ERR_IO;
    }
    return FOTA_HAL_OK;
}

/* ------------------------------------------------------------------ */
/* hal_flash_write                                                      */
/* ------------------------------------------------------------------ */
int hal_flash_write(int slot, uint32_t offset, const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) {
        return FOTA_HAL_ERR_PARAM;
    }
    if (offset + len > POSIX_HAL_PARTITION_SIZE) {
        fprintf(stderr, "[hal_flash_write] write beyond partition end "
                "(offset=%u len=%zu)\n", (unsigned)offset, len);
        return FOTA_HAL_ERR_PARAM;
    }

    /* Open for read+write; create (pre-filled with 0xFF) if missing */
    FILE *fp = open_slot_file(slot, "r+b", 1);
    if (!fp) {
        return FOTA_HAL_ERR_IO;
    }

    if (fseek(fp, (long)offset, SEEK_SET) != 0) {
        perror("[hal_flash_write] fseek");
        fclose(fp);
        return FOTA_HAL_ERR_IO;
    }

    size_t n = fwrite(buf, 1, len, fp);

    /* If writing from the start, truncate the file to exactly offset+len bytes.
     * This ensures hal_flash_partition_size() returns the actual firmware size,
     * not the pre-erased partition size. */
    if (offset == 0) {
        int fd = fileno(fp);
        if (fd >= 0) ftruncate(fd, (off_t)len);
    }

    fclose(fp);

    if (n != len) {
        fprintf(stderr, "[hal_flash_write] short write: wanted %zu wrote %zu\n",
                len, n);
        return FOTA_HAL_ERR_IO;
    }
    return FOTA_HAL_OK;
}

/* ------------------------------------------------------------------ */
/* hal_flash_erase                                                      */
/* ------------------------------------------------------------------ */
int hal_flash_erase(int slot, uint32_t offset, size_t len)
{
    if (len == 0) {
        return FOTA_HAL_OK; /* nothing to do */
    }

    /* Enforce sector alignment */
    if (offset % POSIX_HAL_SECTOR_SIZE != 0 ||
        len    % POSIX_HAL_SECTOR_SIZE != 0) {
        fprintf(stderr,
                "[hal_flash_erase] offset/len not sector-aligned "
                "(offset=%u len=%zu sector=%u)\n",
                (unsigned)offset, len, POSIX_HAL_SECTOR_SIZE);
        return FOTA_HAL_ERR_ALIGN;
    }
    if (offset + len > POSIX_HAL_PARTITION_SIZE) {
        fprintf(stderr, "[hal_flash_erase] erase beyond partition end\n");
        return FOTA_HAL_ERR_PARAM;
    }

    /* Open for read+write; create (pre-filled with 0xFF) if missing */
    FILE *fp = open_slot_file(slot, "r+b", 1);
    if (!fp) {
        return FOTA_HAL_ERR_IO;
    }

    if (fseek(fp, (long)offset, SEEK_SET) != 0) {
        perror("[hal_flash_erase] fseek");
        fclose(fp);
        return FOTA_HAL_ERR_IO;
    }

    /* Write 0xFF over the target range one sector at a time */
    uint8_t sector[POSIX_HAL_SECTOR_SIZE];
    memset(sector, POSIX_HAL_ERASED_BYTE, sizeof(sector));

    size_t remaining = len;
    while (remaining > 0) {
        size_t chunk = (remaining < POSIX_HAL_SECTOR_SIZE)
                       ? remaining : POSIX_HAL_SECTOR_SIZE;
        if (fwrite(sector, 1, chunk, fp) != chunk) {
            perror("[hal_flash_erase] fwrite");
            fclose(fp);
            return FOTA_HAL_ERR_IO;
        }
        remaining -= chunk;
    }

    fclose(fp);
    return FOTA_HAL_OK;
}

/* ------------------------------------------------------------------ */
/* hal_set_boot_active_slot                                             */
/* ------------------------------------------------------------------ */
int hal_set_boot_active_slot(int slot)
{
    if (slot != FOTA_SLOT_ACTIVE && slot != FOTA_SLOT_INACTIVE) {
        return FOTA_HAL_ERR_PARAM;
    }

    FILE *fp = fopen(boot_slot_path, "wb");
    if (!fp) {
        perror("[hal_set_boot_active_slot] fopen");
        return FOTA_HAL_ERR_IO;
    }

    /* Write the slot number as a single ASCII digit, e.g. "0" or "1" */
    char c = (char)('0' + slot);
    if (fwrite(&c, 1, 1, fp) != 1) {
        perror("[hal_set_boot_active_slot] fwrite");
        fclose(fp);
        return FOTA_HAL_ERR_IO;
    }

    fclose(fp);
    fprintf(stdout, "[hal_set_boot_active_slot] boot slot set to %d (%s)\n",
            slot, boot_slot_path);
    return FOTA_HAL_OK;
}

/* ------------------------------------------------------------------ */
/* hal_system_reboot                                                    */
/* ------------------------------------------------------------------ */
void hal_system_reboot(void)
{
    fprintf(stdout,
            "[hal_system_reboot] REBOOT — simulated by process exit.\n");
    fflush(stdout);
    exit(0);
}

/* ------------------------------------------------------------------ */
/* hal_flash_sector_size                                                */
/* ------------------------------------------------------------------ */
uint32_t hal_flash_sector_size(void)
{
    return POSIX_HAL_SECTOR_SIZE;
}

/* ------------------------------------------------------------------ */
/* hal_flash_partition_size                                             */
/* ------------------------------------------------------------------ */
uint32_t hal_flash_partition_size(int slot)
{
    /* For the active slot, return the actual file size so verification and
     * delta generation logic can work with the current firmware length.
     * For the inactive slot, always report full slot capacity; otherwise a
     * stale small test image can shrink the download buffer and trigger
     * CURLE_WRITE_ERROR when a larger payload arrives. */
    if (slot < 0 || slot > 1) return POSIX_HAL_PARTITION_SIZE;
    if (slot == FOTA_SLOT_INACTIVE) return POSIX_HAL_PARTITION_SIZE;

    FILE *fp = fopen(slot_paths[slot], "rb");
    if (!fp) return POSIX_HAL_PARTITION_SIZE;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fclose(fp);
    return (sz > 0) ? (uint32_t)sz : POSIX_HAL_PARTITION_SIZE;
}

/* ------------------------------------------------------------------ */
/* hal_get_serial                                                       */
/* ------------------------------------------------------------------ */
/**
 * POSIX reference implementation.  Priority order:
 *
 *  1. Linux: /proc/cpuinfo "Serial" line (Raspberry Pi and similar SBCs
 *     expose the SoC serial here).
 *  2. Linux: /etc/machine-id (stable UUID-like identifier assigned at
 *     first boot; available on all modern systemd-based distros).
 *  3. Any POSIX: First non-loopback network interface MAC address
 *     formatted as "MAC-AABBCCDDEEFF" (reliable on most systems that
 *     have at least one NIC).
 *  4. Fallback: hostname string prefixed with "HOST-".
 *
 * On real embedded hardware replace this entire function with code that
 * reads the MCU's unique ID registers (e.g. STM32: 0x1FF0F420 / 96 bits).
 */
int hal_get_serial(char *out, size_t out_size)
{
    if (!out || out_size < 2) return FOTA_HAL_ERR_PARAM;

#if defined(__linux__)
    /* ---- 1. /proc/cpuinfo Serial ---- */
    {
        FILE *fp = fopen("/proc/cpuinfo", "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                if (strncmp(line, "Serial", 6) == 0) {
                    /* Line format: "Serial          : 0000000012345678" */
                    char *colon = strchr(line, ':');
                    if (colon) {
                        colon++;
                        while (*colon == ' ' || *colon == '\t') colon++;
                        /* Strip trailing newline */
                        size_t len = strlen(colon);
                        while (len > 0 && (colon[len-1] == '\n' ||
                                           colon[len-1] == '\r' ||
                                           colon[len-1] == ' ')) {
                            colon[--len] = '\0';
                        }
                        if (len > 0 && strcmp(colon, "0000000000000000") != 0) {
                            snprintf(out, out_size, "%s", colon);
                            fclose(fp);
                            return FOTA_HAL_OK;
                        }
                    }
                }
            }
            fclose(fp);
        }
    }

    /* ---- 2. /etc/machine-id ---- */
    {
        FILE *fp = fopen("/etc/machine-id", "r");
        if (fp) {
            char id[64] = {0};
            if (fgets(id, sizeof(id), fp)) {
                fclose(fp);
                /* Strip newline */
                size_t len = strlen(id);
                while (len > 0 && (id[len-1] == '\n' || id[len-1] == '\r')) {
                    id[--len] = '\0';
                }
                if (len >= 8) {
                    snprintf(out, out_size, "%s", id);
                    return FOTA_HAL_OK;
                }
            } else {
                fclose(fp);
            }
        }
    }
#endif /* __linux__ */

    /* ---- 3. First non-loopback NIC MAC address (glibc/Linux only) ---- */
#if defined(FOTA_HAVE_SIOCGIFHWADDR)
    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct ifreq ifr;
            struct ifconf ifc;
            char buf[1024];
            ifc.ifc_len = sizeof(buf);
            ifc.ifc_buf = buf;

            if (ioctl(sock, SIOCGIFCONF, &ifc) == 0) {
                struct ifreq *it  = ifc.ifc_req;
                struct ifreq *end = it + (ifc.ifc_len / sizeof(struct ifreq));

                for (; it != end; it++) {
                    if (strncmp(it->ifr_name, "lo", 2) == 0) continue;

                    strncpy(ifr.ifr_name, it->ifr_name, IFNAMSIZ - 1);
                    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

                    if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
                        unsigned char *mac =
                            (unsigned char *)ifr.ifr_hwaddr.sa_data;
                        if (mac[0] || mac[1] || mac[2] ||
                            mac[3] || mac[4] || mac[5]) {
                            snprintf(out, out_size,
                                     "MAC-%02X%02X%02X%02X%02X%02X",
                                     mac[0], mac[1], mac[2],
                                     mac[3], mac[4], mac[5]);
                            close(sock);
                            return FOTA_HAL_OK;
                        }
                    }
                }
            }
            close(sock);
        }
    }
#endif /* FOTA_HAVE_SIOCGIFHWADDR */

    /* ---- 4. Hostname fallback ---- */
    {
        char hostname[64] = {0};
        if (gethostname(hostname, sizeof(hostname) - 1) == 0 &&
            hostname[0] != '\0') {
            snprintf(out, out_size, "HOST-%s", hostname);
            fprintf(stderr,
                    "[hal_get_serial] WARNING: using hostname as serial — "
                    "not unique across devices!\n");
            return FOTA_HAL_OK;
        }
    }

    /* Nothing worked */
    fprintf(stderr, "[hal_get_serial] ERROR: could not determine serial\n");
    return FOTA_HAL_ERR_IO;
}
