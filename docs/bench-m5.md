# Bench M5 — hub display, touch and SD bring-up (PLAN.md §12 M5a, D34/D35)

What CI proves and what only the bench can. CI (QEMU + host tests) covers
the whole **software** path: the LVGL scenes, the nav model, the tap→engine
request flow (the tap slot drives it through a real indev via `inject_tap`),
and every session. What it cannot cover is the silicon: the RGB panel
peripheral, the CH422G / GT911 I²C dances, the SD slot and the backlight.

## Display + touch (D34)

Boot the hub with a serial console open. Expected boot markers:

```
aio-display: panel 800x480 + GT911 up (LVGL 352x240 logical, x2)
ui: lv pool total=... free=...
ui: LVGL 9.x.x ready (352x240 logical on hardware)
```

Failure modes (all non-fatal by design, §3.3):

- `CH422G not responding (SDA=8 SCL=9)` — expander missing / wiring: the hub
  continues headless (console + radio still work). Check the touch FPC
  is seated (the expander hangs off the same bus).
- `rgb panel init failed` / black screen — timings or data-pin map; the
  constants live in `hub/main/display_hw.cpp` under `namespace board`
  (verified against the Waveshare demo's `esp_panel_board_custom_conf.h`).
- Panel shows a **shifted or doubled image** — PCLK edge or porch mismatch;
  try `pclk_active_neg = 0` first, then the 1024×600 timing table in the
  same demo header.
- Touch mirrored / offset — GT911 axes: fix in `touch_cb` scale math
  (`x_max`/`y_max` mirroring flags in `esp_lcd_touch_config_t`), not in the
  screens.

Touch walk (each tap target, M4a/M4b screens):

1. Dashboard: tap a card → node detail; ⚙ zone (top-right) → settings.
2. Detail: bottom strip → Schedule | History | Alerts; back/home zones at
   the bottom-right corner of every screen.
3. Settings: section list rows switch panes; bottom strip Restart | Add
   node | Export | Restore | Wizard (PIN-gated per §8 when enabled).
4. PIN pad: the "[ 1 ] …" lane; OK submits through PinLock (60 s window).
5. Pairing / wizard / calibration strips per the hint lanes; the flow ends
   on the DONE screens as in `docs/bench-m4b.md`.

Known M5a gaps (M5b): no on-screen text/numeric entry yet — pairing form
keeps template defaults (gate 1500 g), wizard capacity keeps defaults, the
calibration weigh step stays on the bench CLI, and the schedule editor is
view-only. The wizard TZ rows and node picker: TZ works by tap, the node
picker does not (CLI `wiz ...`).

## SD slot (D35)

The FPC is SPI-labeled (GPIO11 MOSI / 12 SCK / 13 MISO / EXIO4 CS). D35
corrected the v0.6 Kconfig swap — card CLK is GPIO12, CMD GPIO11, DAT0
GPIO13 — and `display_hw.cpp` holds EXIO4 high so the card enumerates in
native SD mode, not SPI. Verify before trusting backups to it:

```
sd                       # CLI: mount state + card note
backup                   # writes aio-backup-N.json, prints size
backup restore <name>    # round-trip: registry + schedules back
csv                      # CSV export of the feed log
```

If mount fails: try an SDMMC 4-bit sanity build (slot ignores DAT1-3 in
1-bit), check EXIO4 is high (`aio-display` log), and re-check the card
(standard-speed cards first — DDR/HSX cards can reject 1-bit @16 MHz
neighbours on this bus).

## Backlight

CH422G EXIO2 (DISP), active high, on at the end of `display_hw::init()`.
Dim / screen-off scheduling is a settings pane (§8.8) — the pane is
informational until M5b wires brightness; the bench CLI keeps `set ...`.

## Serial vs touchscreen scope (D33)

The touchscreen gates what the PIN gates (settings, pairing, manual feeds
when scoped). The console remains the owner path: no PIN prompt, and it
keeps the full forms (pair form name/capacity, cal weigh, sched entries,
set wifi) until M5b lands the on-screen entry.
