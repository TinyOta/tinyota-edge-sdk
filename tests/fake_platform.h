/* In-memory fake platform for host unit tests. Not for production use. */
#ifndef FAKE_PLATFORM_H
#define FAKE_PLATFORM_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#define FAKE_SLOT_SIZE (64u * 1024u)

typedef struct {
    /* Inputs set by a test */
    const char    *check_response;   /* body returned for POST /devices/check */
    const uint8_t *payload;          /* body returned for GET download_url   */
    size_t         payload_len;
    const char    *valid_signature;  /* fota_ecdsa_verify() accepts only this */

    /* Observations */
    uint8_t  slots[2][FAKE_SLOT_SIZE];
    int      boot_slot;
    int      rebooted;
    int      status_posts;
    char     last_status_body[512];
    char     last_post_url[512];
    jmp_buf  reboot_jmp;
} fake_state_t;

extern fake_state_t g_fake;

void fake_reset(void);

#endif
