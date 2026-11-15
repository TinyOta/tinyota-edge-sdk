# nRF + Zephyr / nRF Connect SDK (MCUboot)

Uses the C SDK with the nRF HAL (`flash_area` + MCUboot) and the Zephyr
HTTP/TLS port. Not compiled or tested yet; treat it as a starting point.

The board needs an IP network: nRF91 (LTE-M/NB-IoT), nRF7002 (Wi-Fi),
Ethernet, or Thread with a border router. Add your network bring-up where
`main.c` says so.

## Build and flash

```bash
cp src/tinyota_config.example.h src/tinyota_config.h   # fill it in
west build -b <board> --sysbuild .      # sysbuild adds MCUboot
west flash
```

## Ship an update

1. Change `FW_VERSION` and rebuild.
2. Upload `build/<app>/zephyr/zephyr.signed.bin` (MCUboot-signed) in the console.
3. Start a deployment. After download and signature check the SDK requests
   an MCUboot upgrade and reboots; the new image confirms itself on boot.

## Prompt for your AI assistant

```text
Read AGENTS.md in the TinyOTA SDK and examples/zephyr-nrf/.
Add TinyOTA updates to my nRF Connect SDK app <path> for board <nrf9160dk>.
Merge prj.conf, add the SDK sources like the example CMakeLists.txt, register
the CA at TLS tag 42, and run the FOTA loop in its own thread after my
network is up. Call boot_write_img_confirmed() after my self-test.
List the files you changed, Kconfig options added, and what I must fill in.
```
