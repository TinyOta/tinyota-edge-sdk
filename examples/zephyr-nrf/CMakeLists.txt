# TinyOTA example for Zephyr / nRF Connect SDK with MCUboot
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tinyota_zephyr_example)

set(SDK ${CMAKE_CURRENT_LIST_DIR}/../..)

target_sources(app PRIVATE
    src/main.c
    ${SDK}/src/fota_fsm.c
    ${SDK}/src/fota_common.c
    ${SDK}/src/fota_hal_nrf52.c
    ${SDK}/src/fota_platform_nrf52.c
)
target_include_directories(app PRIVATE
    src                 # tinyota_config.h
    ${SDK}/include
)
