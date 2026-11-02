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

Entry pads (M5b): every form field on the session screens opens the shared
modal pad (numeric keys, or the on-screen keyboard for names/SSIDs):

1. Pairing FORM step: tap the Name row (keyboard), the Type row (cycles
   gate → auger → pump; caps reset to the template), Full / In it (numeric
   pad). Commit applies the form exactly like `pair form ...` on the CLI.
2. Wizard: Wi-Fi row 1 edits the SSID (keyboard), Feeder rows pick a node
   or "Pair a new feeder" (Next hands over to the pairing window and the
   wizard resumes when it lands), Hopper rows 1–2 open the numeric pads,
   Schedule rows: Time (HHMM pad), Days (cycles every day → weekdays →
   weekends), Amount (numeric pad).
3. Calibration: WEIGH step → Weigh zone opens the grams pad (one decimal
   is enough, e.g. `8.25`); Save applies the median and pushes CONFIG —
   same session the `cal ...` CLI drives.
4. Schedule editor: tap a row to select, tap below the rows (or New) to
   draft one; tap time / amount in the edit panel for the pads; the S M T
   W T F S chips toggle days; Save writes the registry and pushes
   SCHEDULE_SET; Delete removes the selected row; Pause flips the
   hub-side enabled gate (not part of SCHEDULE_SET).
5. Detail footer: Sched | Hist | Alert navigate; Feed now uses the next
   scheduled amount (or 20 g/ml), PIN-gated per §8 scope; Skip sets
   skip_until to the next occurrence; Hold sets hold_until = now + 1 h —
   both push SCHEDULE_SET so the node enforces the gate locally.

Watch the LVGL pool line at boot: the modal pad adds one blocker, one
panel and two btnmatrix widgets built once — `free=` should still be
comfortably positive (log it in the notes if it drops below ~10 %).

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
when scoped). Since M5b the touchscreen also covers the full forms the CLI
kept: pair form name/mech/capacity, wizard Wi-Fi/hopper/schedule, the cal
weigh step and the schedule editor rows. The CLI stays the bench fallback
and the source of truth for scripted flows.
