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
`screens/<name>.png` (uploaded as the `hub-ui-screens` artifact). Captured:
`dashboard`, `node_detail`, `sched_editor`, `history`, `alerts` — the five
M4a screens — plus the M4b pairing steps `pairing`, `pairing_code`,
`pairing_form`, `pairing_done` and the calibration wizard steps `calib`,
`calib_weigh`, `calib_done` (the firmware rotates screens on a fixed 25 s
× 7-slot schedule; the pairing and calibration flows auto-play inside their
slots, driven by the sim candidate's `PAIR_REQ` beacons and sim-node FEED
results — see `docs/bench-m4b.md`).

RAM budget: the esp32s3 QEMU machine has no PSRAM, so the 176 KB LVGL
framebuffer (384×240×2 — 400-wide overflowed `dram0_0_seg` by ~14 KB when
the fifth screen landed) plus sim rings and the app's statics must all fit
internal DRAM. Keep new statics in the shot build small; the ui/sim code
documents the cuts (`UI_W 384`, `SimRing::CAP = 4`, 2-slot ESP-NOW ring).
