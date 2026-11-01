# tools/qemu — emulation (PLAN.md §7.7)

Espressif QEMU fork + `idf.py qemu` for the hub (ESP32-S3) and the C3 node.
Needs ESP-IDF >= 5.3.1. Install: `idf_tools.py install qemu-xtensa qemu-riscv32`.

- Hub UI headless/graphics: `idf.py qemu --graphics monitor` (RGB framebuffer).
- Networking runs over emulated OpenCores Ethernet (web dashboard, MQTT, NTP).
- Not emulated: Wi-Fi/ESP-NOW, BT, USB, I²C, touch, CH422G, RTC, SD — see §7.7.

CI QEMU boot smoke tests land when the hub build moves to ESP-IDF (D6);
tracked in docs/SETUP.md M0 checklist.
