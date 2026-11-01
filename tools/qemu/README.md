# tools/qemu — emulation (PLAN.md §7.7)

Espressif QEMU fork + `idf.py qemu` for the hub (ESP32-S3) and the C3 node.
Needs ESP-IDF >= 5.3.1. Install: `idf_tools.py install qemu-xtensa qemu-riscv32`.

- Hub UI headless/graphics: `idf.py qemu --graphics monitor` (RGB framebuffer).
- Networking runs over emulated OpenCores Ethernet (web dashboard, MQTT, NTP).
- Not emulated: Wi-Fi/ESP-NOW, BT, USB, I²C, touch, CH422G, RTC, SD — see §7.7.

CI: `ci.yml` job **qemu-hub** builds the hub with `SDKCONFIG_DEFAULTS=
"sdkconfig.defaults;sdkconfig.qemu.defaults"` (no octal PSRAM, radio off —
neither is emulated), merges a 16 MB flash image with esptool, boots it in
`qemu-system-xtensa -M esp32s3` from the pinned espressif/qemu release, greps
the serial log for the boot markers (`aio-feeder-hub`, `core ready`) and fails
on `Guru Meditation`. D6 is decided (pure ESP-IDF, v0.3).

M4a screenshot run (`CONFIG_AIO_UI_SHOT`): the QEMU build also enables the
LVGL dashboard, in-firmware sim nodes (`hub/main/sim_nodes.h`) and a pinned
wall clock; `tools/qemu/capture.py` boots the same flash image with a monitor
socket, waits for `AIO_SHOT <screen>` markers on serial and dumps the LVGL
framebuffer (ELF symbol `g_shot_fb`) via `pmemsave`, converting each frame to
`screens/<name>.png` (uploaded as the `hub-ui-screens` artifact). Captured
today: `dashboard`, `node_detail`, `sched_editor`, `history` (the firmware
rotates screens on a fixed 25 s schedule; more screens join the `--screens`
list as they land).
