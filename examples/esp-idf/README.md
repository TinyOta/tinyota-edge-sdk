# ESP32 + ESP-IDF

Uses the C SDK (`fota_fsm.c`, ESP32 HAL and platform port) straight from
this repository. Status: compiled in CI against ESP-IDF v5.3; not yet
tested on hardware.

## Build and flash

```bash
cp main/tinyota_config.example.h main/tinyota_config.h   # fill it in
idf.py set-target esp32c3        # or esp32, esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

`sdkconfig.defaults` selects `partitions.csv` (two 1.875 MB OTA slots),
size optimisation, app rollback and the TLS certificate bundle. The
built-in two-OTA table has 1 MB slots, which this example (TLS + Wi-Fi)
already exceeds.

## Ship an update

1. Change `FW_VERSION`, run `idf.py build`.
2. Upload `build/tinyota_idf_example.bin` in the console with the same version.
3. Start a deployment. The device checks every hour.

## How it works

- `app_main` marks the running image valid (rollback protection), starts
  Wi-Fi and a FOTA task.
- The task registers with `fota_register()` on every boot. The same serial
  always maps to the same device, so no token storage is needed.
- `fota_fsm_run()` downloads, checks SHA-256 and the signature, switches the
  OTA slot and reboots. Failures are reported to the console.
- RAM: the SDK buffers the whole image in heap. Use a board with PSRAM or
  keep the image well below the free heap.

## Prompt for your AI assistant

```text
Read AGENTS.md in the TinyOTA SDK and examples/esp-idf/.
Add TinyOTA updates to my ESP-IDF project <path>, target <esp32s3>.
Reuse the SDK sources the way examples/esp-idf/main/CMakeLists.txt does,
add the sdkconfig.defaults entries, and start the FOTA task from my app_main
after my Wi-Fi code. Keep ram_free_kb = 0. Do not edit files under src/ or
include/ of the SDK. List the files you changed and what I must fill in.
```
