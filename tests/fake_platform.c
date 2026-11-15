/* In-memory fake HAL + platform seams for host unit tests. */
#include <string.h>
#include <openssl/sha.h>

#include "fake_platform.h"
#include "fota_hal.h"
#include "fota_platform.h"

fake_state_t g_fake;

void fake_reset(void)
{
    memset(&g_fake, 0, sizeof(g_fake));
    memset(g_fake.slots, 0xFF, sizeof(g_fake.slots));
}

/* ---------------- HAL ---------------- */

int hal_flash_read(int slot, uint32_t offset, uint8_t *buf, size_t len)
{
    if (slot < 0 || slot > 1 || !buf || offset + len > FAKE_SLOT_SIZE) return FOTA_HAL_ERR_PARAM;
    memcpy(buf, g_fake.slots[slot] + offset, len);
    return FOTA_HAL_OK;
}

int hal_flash_write(int slot, uint32_t offset, const uint8_t *buf, size_t len)
{
    if (slot != FOTA_SLOT_INACTIVE) return FOTA_HAL_ERR_PROTECTED;
    if (!buf || offset + len > FAKE_SLOT_SIZE) return FOTA_HAL_ERR_PARAM;
    memcpy(g_fake.slots[slot] + offset, buf, len);
    return FOTA_HAL_OK;
}

int hal_flash_erase(int slot, uint32_t offset, size_t len)
{
    if (slot != FOTA_SLOT_INACTIVE) return FOTA_HAL_ERR_PROTECTED;
    if (offset + len > FAKE_SLOT_SIZE) len = FAKE_SLOT_SIZE - offset;
    memset(g_fake.slots[slot] + offset, 0xFF, len);
    return FOTA_HAL_OK;
}

int hal_set_boot_active_slot(int slot) { g_fake.boot_slot = slot; return FOTA_HAL_OK; }

void hal_system_reboot(void)
{
    g_fake.rebooted = 1;
    longjmp(g_fake.reboot_jmp, 1);
}

uint32_t hal_flash_sector_size(void) { return 4096u; }
uint32_t hal_flash_partition_size(int slot) { (void)slot; return FAKE_SLOT_SIZE; }

int hal_get_serial(char *out, size_t out_size)
{
    strncpy(out, "TEST-0001", out_size - 1);
    out[out_size - 1] = '\0';
    return FOTA_HAL_OK;
}

/* ---------------- Platform seams ---------------- */

int fota_http_get(const char *url, const char *bearer_token, uint32_t range_start,
                  uint8_t *out_buf, size_t buf_size)
{
    (void)url; (void)bearer_token;
    if (range_start > g_fake.payload_len) return -1;
    size_t n = g_fake.payload_len - range_start;
    if (n > buf_size) n = buf_size;
    memcpy(out_buf, g_fake.payload + range_start, n);
    return (int)n;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

int fota_http_post_json(const char *url, const char *bearer_token, const char *json_body,
                        char *resp_buf, size_t resp_buf_size)
{
    (void)bearer_token;
    strncpy(g_fake.last_post_url, url, sizeof(g_fake.last_post_url) - 1);
    const char *resp = "{}";
    if (ends_with(url, "/api/v1/devices/check")) {
        resp = g_fake.check_response ? g_fake.check_response : "{\"update_available\":false}";
    } else if (ends_with(url, "/api/v1/devices/status")) {
        g_fake.status_posts++;
        strncpy(g_fake.last_status_body, json_body, sizeof(g_fake.last_status_body) - 1);
    } else if (ends_with(url, "/api/v1/devices/register")) {
        resp = "{\"unit_id\":7,\"model_key\":\"mk\",\"access_token\":\"jwt-abc\"}";
    } else {
        return -1;
    }
    strncpy(resp_buf, resp, resp_buf_size - 1);
    resp_buf[resp_buf_size - 1] = '\0';
    return 0;
}

void fota_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    SHA256(data, len, out);
}

int fota_ecdsa_verify(const uint8_t *data, size_t data_len, const char *sig_b64)
{
    (void)data; (void)data_len;
    return (g_fake.valid_signature && strcmp(sig_b64, g_fake.valid_signature) == 0) ? 1 : 0;
}
