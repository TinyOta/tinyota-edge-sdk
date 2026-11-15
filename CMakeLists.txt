cmake_minimum_required(VERSION 3.16)
project(fota-device-agent-cpp VERSION 2.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FOTA_BUILD_AGENT "Build sample validation program" ON)
option(FOTA_BUILD_TESTS "Build host unit tests for the FSM (needs OpenSSL)" OFF)

# ---------------------------------------------------------------------------
# Platform selection
#
# Set FOTA_PLATFORM on the CMake command line or in your toolchain file:
#   -DFOTA_PLATFORM=posix      (default — Linux/macOS development)
#   -DFOTA_PLATFORM=esp32      (ESP32-C3 / ESP32-S3 via ESP-IDF)
#   -DFOTA_PLATFORM=nrf52      (nRF52840 via Zephyr + MCUboot)
#
# ESP32 note:  Build through `idf.py build`, not CMake directly.
#              Add fota_hal_esp32.c and fota_platform_esp32.c to your
#              IDF component SRCS list.
#
# nRF52840 note: Build through `west build`, not CMake directly.
#                Add fota_hal_nrf52.c and fota_platform_nrf52.c to your
#                Zephyr CMakeLists.txt target_sources().
# ---------------------------------------------------------------------------
set(FOTA_PLATFORM "posix" CACHE STRING
    "Target platform: posix | esp32 | nrf52")

if(FOTA_PLATFORM STREQUAL "posix")
    find_package(CURL REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(BZip2 REQUIRED)
    set(FOTA_HAL_SRC  src/fota_hal_posix.c)
    set(FOTA_PLAT_SRC src/fota_platform_posix.c)
    set(FOTA_PLAT_LIBS CURL::libcurl OpenSSL::SSL OpenSSL::Crypto BZip2::BZip2)
elseif(FOTA_PLATFORM STREQUAL "esp32")
    # ESP-IDF manages its own CMake targets; these lines document the files
    # to register — actual build is via idf_component_register().
    set(FOTA_HAL_SRC  src/fota_hal_esp32.c)
    set(FOTA_PLAT_SRC src/fota_platform_esp32.c)
    set(FOTA_PLAT_LIBS "")   # ESP-IDF links mbedTLS / esp_http_client automatically
elseif(FOTA_PLATFORM STREQUAL "nrf52")
    # Zephyr builds via `west build`; these document the source files.
    set(FOTA_HAL_SRC  src/fota_hal_nrf52.c)
    set(FOTA_PLAT_SRC src/fota_platform_nrf52.c)
    set(FOTA_PLAT_LIBS "")   # Zephyr links mbedTLS / net stack automatically
else()
    message(FATAL_ERROR "Unknown FOTA_PLATFORM '${FOTA_PLATFORM}'. "
                        "Valid values: posix, esp32, nrf52")
endif()

# ---------------------------------------------------------------------------
# fota_sdk — C99 core library
# ---------------------------------------------------------------------------
add_library(fota_sdk STATIC
    src/fota_fsm.c
    src/fota_common.c
    ${FOTA_HAL_SRC}
    ${FOTA_PLAT_SRC}
    src/bspatch.c
)

target_include_directories(fota_sdk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(FOTA_PLAT_LIBS)
    target_link_libraries(fota_sdk PUBLIC ${FOTA_PLAT_LIBS})
endif()

target_compile_options(fota_sdk PRIVATE -Wall -Wextra)
if(FOTA_PLATFORM STREQUAL "posix")
    target_compile_definitions(fota_sdk PRIVATE FOTA_ENABLE_STDIO)
endif()
message(STATUS "FOTA SDK — platform: ${FOTA_PLATFORM}")


# ---------------------------------------------------------------------------
# fota_sdk_sample — Sample validation program (uses fota_sdk)
# ---------------------------------------------------------------------------
if(FOTA_BUILD_AGENT)
    add_executable(fota_sdk_sample
        src/fota_main.c
    )

    target_link_libraries(fota_sdk_sample PRIVATE fota_sdk)

    target_include_directories(fota_sdk_sample PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
endif()

# ---------------------------------------------------------------------------
# Host unit tests — FSM + fota_common.c against an in-memory fake platform
# ---------------------------------------------------------------------------
if(FOTA_BUILD_TESTS)
    find_package(OpenSSL REQUIRED)
    enable_testing()
    add_executable(test_fsm
        tests/test_fsm.c
        tests/fake_platform.c
        src/fota_fsm.c
        src/fota_common.c
    )
    target_include_directories(test_fsm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_fsm PRIVATE OpenSSL::Crypto)
    target_compile_options(test_fsm PRIVATE -Wall -Wextra)
    add_test(NAME fsm COMMAND test_fsm)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS fota_sdk
    EXPORT fota_sdkTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(FOTA_BUILD_AGENT)
    install(TARGETS fota_sdk_sample DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(FILES
    include/fota_hal.h
    include/fota_fsm.h
    include/fota_platform.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fota
)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/fota_sdkConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/fota_sdkConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fota_sdk
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/fota_sdkConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/fota_sdkConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/fota_sdkConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fota_sdk
)

install(EXPORT fota_sdkTargets
    FILE fota_sdkTargets.cmake
    NAMESPACE fota_sdk::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fota_sdk
)
