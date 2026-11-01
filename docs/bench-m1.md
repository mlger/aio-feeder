# M1 bench checklist — node bring-up (PLAN.md §12 M1)

Code side of M1 is done: HAL v1 on both MCUs, hard relay-off timer, watchdog,
DS3231 driver, A/B config storage, feed journal, actuator FSM with all
PRE_CHECKs, and the serial CLI. The items below need real hardware and close
the milestone. Firmware flows through CI → web flasher like everything else.

## What you need

- ESP8266 (D1 mini / ESP-12F) **and** ESP32-C3 SuperMini bench nodes
- DS3231 module + cell, 5 V relay module, 12 V push-pull solenoid + flyback
  diode, 12 V supply + buck
- Pull resistor so the relay is OFF whenever the MCU doesn't drive it
  (PLAN.md §4.5 default-OFF warning), fuse + TVS on the 12 V input
- USB data cables, Chrome/Edge for the web flasher
- Scope or logic analyzer, multimeter

## Bring-up sequence

1. **Flash** the node image for your MCU from https://mlger.github.io/aio-feeder/
   (or `pio run -d node -e node_esp8266|node_esp32c3 && pio run -t upload`).
2. Serial CLI at 115200: `status` → expect `fault=no stop=0 limits_confirmed=0`
   (SR8: a fresh node refuses scheduled feeds until confirmed).
3. **SR1 (default OFF):** scope the relay module input across power-on, reset,
   `wdt` reset, and repeated re-plugs. Relay must never click. Verify with the
   pull resistor alone by holding the MCU in reset.
4. **SR2 (hard max):** `relay 2000` with the scope on the relay output —
   measure on-time ≈ 2000 ms. Then `wdt` mid-`relay` run: the timer ISR must
   cut the output even though the loop is dead. Confirm the CLI prints
   `hard timer fired` on the next boot's status (fault latched).
5. **Feed path:** `conf on`, `timeset <unix>` (sets DS3231, counts as sync),
   `feeds 4` → relay ON for ≈1000 ms (4 g × 250 ms/g default cal), CLI prints
   `RESULT id=… status=OK actual_on_ms=…`. Weigh the output, record in the
   calibration table below. Repeat 3×, median → `cal <ms_per_gram_x100>`.
6. **SR3/SR4:** `caps 10 20`, then feed 10× — 11th must print
   `REJ_LIMIT_HOUR`. Immediate re-feed → `REJ_COOLDOWN`. Power-cycle: caps
   still enforced (counters persisted, CRC'd A/B slots).
7. **SR6 (no double feed):** `feed 4`, cut DC power mid-feed, restore. `status`
   shows the boot line `in-progress feed(s) marked attempted`; re-sending the
   same `feed_id` answers from the journal and does **not** actuate.
8. **SR7 (time):** with no `timeset`, `feeds 4` → `REJ_NO_TIME` (manual `feed`
   still works). Remove the RTC cell, reboot → `time` shows `osf=1`,
   `healthy=0`, scheduled feeds refuse until `timeset` again.
9. **SR8:** `erase` → reboot → `feeds 4` → `REJ_LIMITS_UNCONFIRMED` until
   `conf on`.
10. **Supply monitor:** fit the divider, `vin` shows rail mV; drag the rail
    below ~10.8 V → `REJ_LOW_VOLT`. [VERIFY threshold against the real
    solenoid inrush sag]

## Calibration log (per food type)

| Date | Food | Test amount | Measured (g) | cal x100 (ms/g) | Notes |
|---|---|---|---|---|---|
|  |  |  |  |  |  |

## M1 exit items still open (per PLAN.md §12)

- ESP-NOW range / packet-loss / channel tests → **M2** (radio lands there);
  the M1 exit criterion "first range test on real boards" moves with it.
- Verified ESP-NOW encrypted-peer limits (both SDKs) → record in the decision
  log when M2 lands.
- RTC trust defaults (30 d / 24 h) — confirm against the soak results above.
