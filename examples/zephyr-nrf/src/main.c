/*
 * TinyOTA example for Zephyr / nRF Connect SDK + MCUboot.
 *
 * Boot -> confirm this image with MCUboot -> wait for the network ->
 * register (same serial returns the same device) -> check every hour.
 * On success the SDK marks slot 1 for upgrade and reboots; MCUboot swaps.
 *
 * Not yet tested on hardware.
 */
#include <string.h>

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/tls_credentials.h>

#include "fota_fsm.h"
#include "fota_hal.h"
#include "fota_platform.h"
#include "tinyota_config.h"   /* copy tinyota_config.example.h and fill it in */

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define FOTA_TLS_TAG 42       /* must match fota_platform_nrf52.c */

static const char root_ca[] = TINYOTA_ROOT_CA_PEM;
static char token[768];

static bool register_device(void)
{
    char serial[32];
    if (hal_get_serial(serial, sizeof(serial)) != FOTA_HAL_OK) return false;
    if (fota_register(TINYOTA_API, TINYOTA_REG_TOKEN, serial, token, sizeof(token)) != 0) {
        LOG_ERR("register failed: check TINYOTA_REG_TOKEN, network and CA");
        return false;
    }
    LOG_INF("registered as %s", serial);
    return true;
}

int main(void)
{
    LOG_INF("firmware %s", FW_VERSION);

    /* Tell MCUboot this image works, otherwise it reverts on the next
     * reset. In a real product run your self-test first. */
    if (!boot_is_img_confirmed()) {
        boot_write_img_confirmed();
    }

    tls_credential_add(FOTA_TLS_TAG, TLS_CREDENTIAL_CA_CERTIFICATE,
                       root_ca, sizeof(root_ca));

    /* Bring up your network here (LTE attach, Wi-Fi connect, ...). */

    while (!register_device()) {
        k_sleep(K_SECONDS(60));
    }

    fota_config_t cfg = {
        .server_base_url = TINYOTA_API,
        .device_token    = token,
        .current_version = FW_VERSION,
        .ram_free_kb     = 0,          /* nRF port is FULL-only */
    };
    fota_ctx_t *ctx = fota_fsm_init(&cfg);

    for (;;) {
        fota_fsm_trigger(ctx);
        while (fota_fsm_state(ctx) != FOTA_STATE_IDLE) {
            fota_fsm_run(ctx);         /* reboots inside on success */
            k_sleep(K_MSEC(20));
        }
        fota_error_t err = fota_fsm_last_error(ctx);
        if (err == FOTA_ERR_NETWORK) {
            register_device();         /* refreshes an expired token */
        } else if (err != FOTA_ERR_NO_UPDATE) {
            LOG_WRN("update failed: %d", (int)err);
        }
        k_sleep(K_HOURS(1));
    }
    return 0;
}
