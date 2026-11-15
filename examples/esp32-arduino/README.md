# ESP32 + Arduino / PlatformIO

Signed OTA updates for ESP32, ESP32-C3 and ESP32-S3 using the Arduino
framework. The `TinyOTA` library (in `lib/TinyOTA`) streams the new image
into the next OTA slot with Arduino `Update`, checks the SHA-256 and the
ECDSA P-256 signature, and only then switches and reboots.

Status: compiled in CI; not yet tested on hardware.

## PlatformIO

```bash
cp include/tinyota_config.example.h include/tinyota_config.h   # fill it in
pio run -e esp32dev -t upload       # first flash over USB
pio device monitor
```

Other boards: `-e esp32-c3` or `-e esp32-s3`, or add your board to
`platformio.ini`. The partition table must have two app slots
(`min_spiffs.csv` or the default table both do).

## Arduino IDE

1. Zip the `lib/TinyOTA` folder and add it with *Sketch > Include Library >
   Add .ZIP Library*. Install **ArduinoJson** (v7) from the Library Manager.
2. Open *File > Examples > TinyOTA > Basic* and fill in the
   `tinyota_config.h` tab.
3. *Tools > Partition Scheme*: any scheme with OTA (for example
   "Minimal SPIFFS (1.9MB APP with OTA)").
4. Upload over USB once.

## Ship an update

1. Change `FW_VERSION` (for example `1.0.0` to `1.0.1`) and build.
2. Upload the binary in the console (*Firmwares*) with version `1.0.1`:
   - PlatformIO: `.pio/build/esp32dev/firmware.bin`
   - Arduino IDE: *Sketch > Export Compiled Binary*, then `<sketch>.ino.bin`
     (not `.merged.bin` or `.bootloader.bin`)
3. Create and start a deployment for the device model.
4. The device checks every hour (and right after boot), installs, reboots,
   and the console shows `success`.

## Prompt for your AI assistant

```text
Read AGENTS.md (or CLAUDE.md) in the TinyOTA SDK and examples/esp32-arduino/.
Add TinyOTA OTA updates to my existing PlatformIO (Arduino) project.

Board: <for example esp32-c3-devkitm-1>
My main file: <src/main.cpp>
Firmware version now: <1.0.0>

1. Copy lib/TinyOTA into my lib/ folder and add ArduinoJson@^7 to lib_deps.
2. Set a partition table with two OTA slots in platformio.ini.
3. Create include/tinyota_config.h from the example (leave placeholders for
   the token and public key, I will paste them).
4. Call ota.begin() after Wi-Fi connects and ota.checkForUpdate() once an hour
   without blocking my existing loop.
5. Do not change TinyOTA.cpp unless something does not compile.
Then list every file you changed and what I must fill in.
```

## Notes

- TLS: set `TINYOTA_ROOT_CA` to the root certificate of `api.tinyota.xyz`
  for production. With `NULL` the certificate is not checked; the firmware
  signature is still verified, so a forged image cannot be installed, but
  the device token could be exposed on a hostile network.
- The library asks for FULL images only (`ram_free = 0`).
