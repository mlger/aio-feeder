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
`pairing_form`, `pairing_done`, the calibration wizard steps `calib`,
`calib_weigh`, `calib_done`, and the M4b settings trio `settings`,
`settings_security`, `settings_backup`, the PIN pad states `pin_entry`,
`pin_typing`, `pin_wrong`, `pin_ok` and the setup wizard steps `wizard`,
`wizard_node`, `wizard_cap`, `wizard_sched`, `wizard_done` (the firmware
rotates screens on a fixed 25 s × 10-slot schedule; the pairing, calibration,
PIN and wizard flows auto-play inside their slots — see `docs/bench-m4b.md`).

RAM budget: the esp32s3 QEMU machine has no PSRAM, so the 176 KB LVGL
framebuffer (384×240×2 — 400-wide overflowed `dram0_0_seg` by ~14 KB when
the fifth screen landed) plus sim rings and the app's statics must all fit
internal DRAM. Keep new statics in the shot build small; the ui/sim code
documents the cuts (`UI_W 384`, `SimRing::CAP = 3`, 2-slot ESP-NOW ring).
Static DRAM ran out again twice with the seventh screen: the feedloop
dedup table (4 KB, REPL-only) is compiled out of the shot build and the
LVGL pool is raised 64 → 72 KB (`CONFIG_LV_MEM_SIZE_KILOBYTES` in
`sdkconfig.qemu.defaults`; note `LV_MEM_POOL_EXPAND_SIZE` is a no-op in
9.2.2). The ui task prints `lv pool total/free` at boot — watch it before
adding screens.

v0.6 took the build to 10 slots and overflowed `dram0_0_seg` by 464 B;
the PIN pad became a single `lv_btnmatrix` (12 separate buttons cost
~1.5 KB of pool for widgets the shot never touches). Shrinking the pool
to 70 KB got the link through but left only 2.2 KB free at boot and the
first dashboard render died in `lv_memset(NULL)` (StoreProhibited — the
same failure mode the 64 KB pool hit on the seventh screen). The framebuffer then
dropped 384 → 352 px (2 × 170 px cards, ~15 KB of .bss back). Two more
experiments settled the pool question: the component IGNORES pool-size
requests (`lv pool total` printed 63196 B for 70/84 KiB — treat it as a
fixed 64 KiB) and does not expose `CONFIG_LV_USE_STDLIB_MALLOC` (unknown
symbol; forcing it panicked in lv_label_set_text). The ten-screen build
fits by demand control instead: the PIN pad is a styled multi-line LABEL
(a lv_btnmatrix drew 12 rounded cells whose mask allocations exhausted
the pool mid-draw — buttons progressively vanished frame by frame).
`tools/qemu/capture.py` carries the same FB_W/FB_H constants.

M8a additions (v0.11):

- The tap walk gained a stage: dash → detail → home → settings → BACKUP →
  SECURITY → **FIRMWARE** (`tap_firmware.png`), exercising the settings
  action-strip remap on the new pane.
- **OTA selftest** (`ci.yml` job `ota-selftest`): a second hub build with
  `sdkconfig.qemu.ota.defaults` (AIO_NO_RADIO, no UI, 5 s confirm window,
  `CONFIG_AIO_OTA_SELFTEST`) boots beside `tools/qemu/badapp` (a
  deliberately non-confirming image merged into the `badapp` data
  partition at 0xFA0000). The walk is five real guest reboots in one QEMU
  process — `tools/qemu/ota_run.py` omits capture.py's `-no-reboot` for
  exactly that reason — and asserts the marker order: stage0 staged
  (running image copied into ota_0 through the full esp_ota_ops path) →
  PENDING_VERIFY → health confirm → stage2 staged (badapp → ota_1) →
  badapp runs and never confirms → bootloader invalidates ota_1 and falls
  back (`rollback VERIFIED`). A boot counter in NVS aborts the walk if it
  ever loops past 14 boots.
