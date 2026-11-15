/*
 * TinyOTA example for ESP-IDF v5.x (ESP32, ESP32-C3, ESP32-S3).
 *
 * Boot -> Wi-Fi -> mark this image valid -> register (same serial returns
 * the same device, so registering on every boot is safe) -> check for an
 * update every hour with the TinyOTA Edge SDK FSM.
 *
 * Not yet tested on hardware.
 */
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "fota_fsm.h"
#include "fota_hal.h"
#include "fota_platform.h"
#include "tinyota_config.h"   /* copy tinyota_config.example.h and fill it in */

static const char *TAG = "app";

#define CHECK_PERIOD_MS (60UL * 60UL * 1000UL)   /* 1 hour */
#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi;
static char s_token[768];

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(s_wifi, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    s_wifi = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, WIFI_PASSWORD, sizeof(cfg.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static bool register_device(void)
{
    char serial[32];
    if (hal_get_serial(serial, sizeof(serial)) != FOTA_HAL_OK) return false;
    if (fota_register(TINYOTA_API, TINYOTA_REG_TOKEN, serial, s_token, sizeof(s_token)) != 0) {
        ESP_LOGE(TAG, "register failed: check TINYOTA_REG_TOKEN and the device limit");
        return false;
    }
    ESP_LOGI(TAG, "registered as %s", serial);
    return true;
}

static void on_state(fota_ctx_t *ctx, fota_state_t from, fota_state_t to, void *user)
{
    (void)ctx; (void)user;
    ESP_LOGI(TAG, "FOTA state %d -> %d", (int)from, (int)to);
}

static void fota_task(void *arg)
{
    (void)arg;
    xEventGroupWaitBits(s_wifi, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    while (!register_device()) {
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    }

    fota_config_t cfg = {
        .server_base_url = TINYOTA_API,
        .device_token    = s_token,
        .current_version = FW_VERSION,
        .ram_free_kb     = 0,          /* ESP32 port is FULL-only */
        .on_state_change = on_state,
    };
    fota_ctx_t *ctx = fota_fsm_init(&cfg);

    for (;;) {
        xEventGroupWaitBits(s_wifi, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        fota_fsm_trigger(ctx);
        while (fota_fsm_state(ctx) != FOTA_STATE_IDLE) {
            fota_fsm_run(ctx);           /* reboots inside on success */
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        fota_error_t err = fota_fsm_last_error(ctx);
        if (err == FOTA_ERR_NETWORK) {
            register_device();           /* refreshes an expired token */
        } else if (err != FOTA_ERR_NO_UPDATE) {
            ESP_LOGW(TAG, "update failed: %d", (int)err);
        }
        vTaskDelay(pdMS_TO_TICKS(CHECK_PERIOD_MS));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "firmware %s", FW_VERSION);

    /* With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the bootloader goes back to
     * the previous image unless the new one confirms itself. Run your own
     * self-test before this line in a real product. */
    esp_ota_mark_app_valid_cancel_rollback();

    wifi_start();
    xTaskCreate(fota_task, "fota", 8192, NULL, 5, NULL);
}
