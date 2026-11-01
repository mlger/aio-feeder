# tools/qemu — emulation (PLAN.md §7.7)

Espressif QEMU fork + `idf.py qemu` for the hub (ESP32-S3) and the C3 node.
Needs ESP-IDF >= 5.3.1. Install: `idf_tools.py install qemu-xtensa qemu-riscv32`.

- Hub UI headless/graphics: `idf.py qemu --graphics monitor` (RGB framebuffer).
- Networking runs over emulated OpenCores Ethernet (web dashboard, MQTT, NTP).
- Not emulated: Wi-Fi/ESP-NOW, BT, USB, I²C, touch, CH422G, RTC, SD — see §7.7.

CI: `ci.yml` job **qemu-hub** builds the hub with `SDKCONFIG_DEFAULTS=
"sdkconfig.defaults;sdkconfig.qemu.defaults"` (no octal PSRAM, radio off —
neither is emulated), merges a 16 MB flash image with esptool, and boots it in
`qemu-system-xtensa -M esp32s3` from the pinned espressif/qemu release,
grepping the serial log for the boot markers (`aio-feeder-hub`, `core ready`)
and failing on `Guru Meditation`. D6 is decided (pure ESP-IDF, v0.3).
