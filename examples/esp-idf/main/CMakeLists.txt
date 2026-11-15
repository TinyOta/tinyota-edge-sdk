# Builds the TinyOTA Edge SDK straight from this repository.
set(SDK "${CMAKE_CURRENT_LIST_DIR}/../../..")

idf_component_register(
    SRCS
        "main.c"
        "${SDK}/src/fota_fsm.c"
        "${SDK}/src/fota_common.c"
        "${SDK}/src/fota_hal_esp32.c"
        "${SDK}/src/fota_platform_esp32.c"
    INCLUDE_DIRS
        "."                 # tinyota_config.h
        "${SDK}/include"
    REQUIRES
        esp_http_client esp-tls mbedtls nvs_flash esp_partition app_update
        efuse esp_wifi esp_netif esp_event
)
