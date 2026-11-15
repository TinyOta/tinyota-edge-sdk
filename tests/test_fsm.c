/* Host unit tests for fota_fsm.c + fota_common.c (run with ctest). */
#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>

#include "fake_platform.h"
#include "fota_fsm.h"
#include "fota_platform.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const uint8_t FW[] = "new firmware image v1.1.0";

static void sha_hex(const uint8_t *d, size_t n, char out[65])
{
    uint8_t dig[32];
    SHA256(d, n, dig);
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", dig[i]);
}

/* Trigger one update cycle and run the FSM until it settles. */
static fota_ctx_t *run_cycle(void)
{
    static fota_ctx_t *ctx;
    fota_config_t cfg = {
        .server_base_url = "https://api.example.test",
        .device_token    = "jwt-abc",
        .current_version = "1.0.0",
    };
    ctx = fota_fsm_init(&cfg);
    if (setjmp(g_fake.reboot_jmp) == 0) {
        fota_fsm_trigger(ctx);
        for (int i = 0; i < 20 && fota_fsm_state(ctx) != FOTA_STATE_IDLE; i++) {
            fota_fsm_run(ctx);
        }
    }
    return ctx;
}

static char check_resp[1024];

static void set_update(const char *checksum, const char *signature)
{
    snprintf(check_resp, sizeof(check_resp),
             "{\"update_available\":true,\"current_version\":\"1.0.0\","
             "\"target_version\":\"1.1.0\",\"payload_type\":\"FULL\","
             "\"download_url\":\"/api/v1/devices/download/firmware/3\","
             "\"checksum\":%s%s%s,\"signature\":%s%s%s}",
             checksum ? "\"" : "", checksum ? checksum : "null", checksum ? "\"" : "",
             signature ? "\"" : "", signature ? signature : "null", signature ? "\"" : "");
    g_fake.check_response = check_resp;
    g_fake.payload = FW;
    g_fake.payload_len = sizeof(FW);
    g_fake.valid_signature = "good-sig";
}

static void test_full_update_success(void)
{
    char hex[65];
    puts("full update with valid checksum and signature switches slot and reboots");
    fake_reset();
    sha_hex(FW, sizeof(FW), hex);
    set_update(hex, "good-sig");
    fota_ctx_t *ctx = run_cycle();
    CHECK(g_fake.rebooted);
    CHECK(g_fake.boot_slot == 1);
    CHECK(memcmp(g_fake.slots[1], FW, sizeof(FW)) == 0);
    CHECK(strstr(g_fake.last_status_body, "\"status\":\"success\"") != NULL);
    CHECK(strstr(g_fake.last_status_body, "\"version\":\"1.1.0\"") != NULL);
    fota_fsm_destroy(ctx);
}

static void test_hash_mismatch(void)
{
    puts("checksum mismatch is rejected before flash write and reported");
    fake_reset();
    set_update("00000000000000000000000000000000000000000000000000000000000000ff", "good-sig");
    fota_ctx_t *ctx = run_cycle();
    CHECK(!g_fake.rebooted);
    CHECK(g_fake.boot_slot == 0);
    CHECK(fota_fsm_last_error(ctx) == FOTA_ERR_HASH_MISMATCH);
    CHECK(g_fake.slots[1][0] == 0xFF);
    CHECK(strstr(g_fake.last_status_body, "\"status\":\"failed\"") != NULL);
    CHECK(strstr(g_fake.last_status_body, "\"error_code\":-4") != NULL);
    CHECK(strstr(g_fake.last_status_body, "\"fsm_state\":\"PATCHING\"") != NULL);
    fota_fsm_destroy(ctx);
}

static void test_missing_checksum(void)
{
    puts("missing checksum is rejected");
    fake_reset();
    set_update(NULL, "good-sig");
    fota_ctx_t *ctx = run_cycle();
    CHECK(!g_fake.rebooted);
    CHECK(fota_fsm_last_error(ctx) == FOTA_ERR_HASH_MISMATCH);
    fota_fsm_destroy(ctx);
}

static void test_missing_signature(void)
{
    char hex[65];
    puts("missing signature is rejected and the slot is erased");
    fake_reset();
    sha_hex(FW, sizeof(FW), hex);
    set_update(hex, NULL);
    fota_ctx_t *ctx = run_cycle();
    CHECK(!g_fake.rebooted);
    CHECK(g_fake.boot_slot == 0);
    CHECK(fota_fsm_last_error(ctx) == FOTA_ERR_SIG_INVALID);
    CHECK(g_fake.slots[1][0] == 0xFF);
    fota_fsm_destroy(ctx);
}

static void test_bad_signature(void)
{
    char hex[65];
    puts("invalid signature is rejected and the slot is erased");
    fake_reset();
    sha_hex(FW, sizeof(FW), hex);
    set_update(hex, "forged-sig");
    fota_ctx_t *ctx = run_cycle();
    CHECK(!g_fake.rebooted);
    CHECK(fota_fsm_last_error(ctx) == FOTA_ERR_SIG_INVALID);
    CHECK(g_fake.slots[1][0] == 0xFF);
    fota_fsm_destroy(ctx);
}

static void test_no_update(void)
{
    puts("no update available does not post a status report");
    fake_reset();
    fota_ctx_t *ctx = run_cycle();
    CHECK(fota_fsm_last_error(ctx) == FOTA_ERR_NO_UPDATE);
    CHECK(g_fake.status_posts == 0);
    fota_fsm_destroy(ctx);
}

static void test_register_and_json(void)
{
    char token[64];
    char s[16];
    int n = 0;
    puts("fota_register and JSON helpers from fota_common.c");
    fake_reset();
    CHECK(fota_register("https://api.example.test", "reg-tok", "SER1", token, sizeof(token)) == 0);
    CHECK(strcmp(token, "jwt-abc") == 0);
    CHECK(strcmp(g_fake.last_post_url, "https://api.example.test/api/v1/devices/register") == 0);
    CHECK(fota_json_get_string("{\"a\": \"x\\\"y\"}", "a", s, sizeof(s)) == 0 && strcmp(s, "x\"y") == 0);
    CHECK(fota_json_get_int("{\"n\":-12}", "n", &n) == 0 && n == -12);
    CHECK(fota_report_status("https://api.example.test", "jwt", "ok\"", "1.0", NULL, 0, "a\nb") == 0);
    CHECK(strstr(g_fake.last_status_body, "\"status\":\"ok\"") != NULL);
    CHECK(strstr(g_fake.last_status_body, "\"message\":\"ab\"") != NULL);

    char b64[16];
    CHECK(fota_b64url_to_b64("ab-_c", b64, sizeof(b64)) == 8 && strcmp(b64, "ab+/c===") == 0);
    CHECK(fota_b64url_to_b64("abcd", b64, sizeof(b64)) == 4 && strcmp(b64, "abcd") == 0);
    CHECK(fota_b64url_to_b64("abcdefghijklmnop", b64, sizeof(b64)) == -1);
}

int main(void)
{
    test_full_update_success();
    test_hash_mismatch();
    test_missing_checksum();
    test_missing_signature();
    test_bad_signature();
    test_no_update();
    test_register_and_json();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all FSM tests passed");
    return 0;
}
