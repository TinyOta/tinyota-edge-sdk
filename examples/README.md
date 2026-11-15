# TinyOTA examples

Pick the folder that matches your toolchain. Each one is a complete project
with a `tinyota_config.example.h` (or `.env.example`) to fill in, build
steps, the file to upload to the console, and a prompt for your AI coding
assistant.

| Folder | Environment | Status |
|---|---|---|
| [`esp32-arduino/`](esp32-arduino/) | ESP32 / C3 / S3 with Arduino IDE or PlatformIO | Compiled in CI, not yet tested on hardware |
| [`esp-idf/`](esp-idf/) | ESP32 / C3 / S3 with ESP-IDF v5 | Compiled in CI, not yet tested on hardware |
| [`zephyr-nrf/`](zephyr-nrf/) | nRF52/53/91 with Zephyr or nRF Connect SDK + MCUboot | Not compiled or tested yet |
| [`linux/`](linux/) | Raspberry Pi and other Linux devices (Python agent + systemd) | Agent covered by unit tests |

STM32, RP2040/Pico W and other MCUs: follow `PORTING_GUIDE.md` and start
from the port closest to your chip.

## Before you start (all environments)

1. In the console, create a **device model** and copy its **registration token**.
2. Copy the signing public key from
   `https://api.tinyota.xyz/api/v1/devices/public-key` (`public_key_pem`).
3. Put both into the example's config file. Never commit that file.
4. Use the same version string in the firmware (`FW_VERSION`) and in the
   console upload form. `1.0.0` and `v1.0.0` are different versions.

## Using an AI assistant

The SDK root has `AGENTS.md`, `CLAUDE.md`, `.github/copilot-instructions.md`
and `.cursor/rules/tinyota.mdc`. They are the same text (generated from
`docs/AI_CONTEXT.md`), so Claude Code, Codex, Copilot and Cursor all pick
it up automatically when you open the SDK folder. Each example README has a
prompt to paste.
