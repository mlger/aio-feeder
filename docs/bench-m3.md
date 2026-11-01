# M3 bench verification — hub core

Hub core (scheduler, feed_manager, time, storage, event log) is host-tested in
CI (`test/test_hub_core`, 25 tests) including the §12 M3 exit criterion
("schedules fire correctly across reboots" — reboot no-double-fire, catch-up
once within window, never stack, WAL intent → FEED_UNKNOWN on hub death
mid-feed). What still needs the real hardware:

## Hub board (Waveshare ESP32-S3-Touch-LCD-4.3B)

- [ ] `sched add 480 0x7F 20` + `sched push` on a real bench node; watch the
      FEED/ACK/RESULT cycle fire at 08:00 local (tz rule 1, Asia/Manila).
- [ ] **Reboot persistence:** power-cycle the hub right after a fire
      (`feed` in flight) → boot prints the FEED_UNKNOWN reconciliation, and
      no second FEED goes to the node. Repeat with power-cut at boot before
      NTP sync (RTC path) — occurrence still not double-fired.
- [ ] **NTP path:** with `AIO_WIFI_SSID/PASS` set, `time` shows a valid unix
      within ~15 s of boot; nodes receive TIME_SYNC on heartbeat when their
      drift > 2 s (node CLI shows `drift` shrinking).
- [ ] **PCF85063 RTC fallback:** no Wi-Fi → boot sets system time from the
      RTC; pull the CR927 / drain it, set a wild RTC time → boot rejects the
      value (range check + oscillator-stop flag) and schedules stay parked
      (SR7) until time is valid.
- [ ] **Stagger (§9.5):** two bench nodes, `stagger 1` → `feed 1 5` then
      `feed 2 5` — second FEED leaves only after the first resolves;
      `stagger 2` → both leave immediately.
- [ ] **Registry survives reboot:** `peer add`, `sched add`, power-cycle →
      `peers` + `sched list` intact (NVS blob CRC path).
- [ ] **Catch-up on real wall clock:** set policy 1 (`catchup_window_min`
      30) on a node, power the hub down across a scheduled minute, boot
      within 30 min → exactly one CATCHUP feed; boot after 2 h →
      SKIP_MISSED event only.
- [ ] Record calibration context for the stock estimator (M4): commanded vs
      `FEED_RESULT` actual amounts for ≥ 10 feeds.

## Deferred (tracked)

- QEMU boot smoke test for the hub image — still pending decision D6
  (ESP-IDF vs Arduino for the LVGL hub).
- SD-card event history (§7.5) — M4; the RAM ring (128 events) is enough for
  M3 and feeding never depends on it.
