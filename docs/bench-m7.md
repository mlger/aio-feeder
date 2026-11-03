# Bench M7 — resilience bring-up (PLAN.md §12 M7, FMEA 1–8 / 13–17 / 22–23)

What CI proves and what only the bench can. CI runs the whole **pure** path
(`test_node_core` incl. the fallback scheduler, `test_hub_core` incl.
reconciliation + the unpair queue, `test_link_core` incl. the keyed UNPAIR
round-trip, `test_remote` incl. the v1→v2 settings migration) and boots the
hub in QEMU. The bench proves the parts that need radio, RTC and sensors.

## Fallback schedule on the node (§9.6, FMEA #4)

1. Pair a node, push a schedule (`sched 1 add 510 127 20` then `sched 1 push`),
   confirm `SCHEDULE_ACK ok=1` and the node prints `RX schedule v… push=…`.
2. Power the hub down (or walk the node out of range). The node prints
   `link lost → channel scan` and keeps scanning; feeding continues per the
   stored copy.
3. At the pushed minute the node prints
   `LOCAL sched v… entry0: amount=20 -> feed id=0x8000… accepted` and runs it
   under **all** limits (caps, cooldown, journal, `RESULT` reporting).
4. Edge cases on serial:
   - inside `skip_until` / `hold_until` → `occurrence inside skip_until` /
     `… hold_until`, no feed;
   - schedule older than 7 days → `fallback copy stale (>7 d) — not running`;
   - node rebooted with a flat RTC → `LOCAL sched` never fires (SR7: no valid
     time) until the next TIME_SYNC; the hub reconciles the missed
     occurrence on return (§9.4 default skip + alert, or catch-up-once).
5. Power the hub back on: the node's `FEED_RESULT` rows reconcile into the
   hub history as OK rows (source `scheduled`) and the stock estimate drops
   by what actually dispensed — verify `events` on the CLI and the dashboard
   history.
6. Catch-up policy (`missed_policy=1`): if the node already ran the missed
   occurrence from its fallback copy, the hub **stands down** (D37 — no
   second dose; the self-feed is visible as a local-range result). Known
   limit: a fallback feed the node ran *while the hub was down* reaches the
   hub only if the hub boots before the node's 3× result repeats expire —
   otherwise the row is skipped-as-missed and never double-counted.

## Pending unpair (FMEA #15)

1. Remove a **powered-down** node: `peer del 1` →
   `peer removed (unpair pending until it checks in)`; `remote` (web status)
   lists it under `unpair` and the alerts/history show `UNPAIR_PEND`.
2. Power the node on: it is still paired, so its heartbeats keep coming and
   every beat retries the keyed `UNPAIR`; the node then prints
   `RX unpair: pairing + fallback schedule cleared — beaconing` and starts
   HELLO/PAIR_REQ beacons again → the hub prints
   `unpair confirmed (node re-beaconed unpaired)` and the web `unpair` list
   empties.
   A hub reboot in between keeps working: the queue blob (`unp`) re-seeds
   the engine peer at boot (`pending unpairs: N`).

## Sensors + stop switch (FMEA #2 / #8 / #17)

Pin maps (both `[VERIFY]` vs the rig — see the HAL headers):

| input | ESP8266 | ESP32-C3 | wiring |
|---|---|---|---|
| stop switch (SR9) | D6/GPIO12 | GPIO2 | to GND, pullup, active-low, latching |
| hopper-low (§4.2) | D7/GPIO13 | GPIO4 | sensor open-drain to GND, pullup, active-low |
| gate reed (FMEA #2) | D0/GPIO16 | GPIO5 | reed to GND **+ external pullup** (8266 GPIO16 has none), LOW = closed |

- Stop switch: flip it → `stop switch -> manual_stop=1`, feeds get
  `REJECTED (manual stop)`; the heartbeat flag `HB_FLAG_MANUAL_STOP` shows on
  the hub. Rigs **without** the switch never see it (CLI `stop` still works).
- Hopper: set the profile flag (`PROFILE_FLAG_HOPPER_SENSOR` via a CONFIG
  push), pull the sensor low → heartbeat `hopper_low=1` → hub logs
  `HOPPER_LOW` once per low period and notifies (§9.11 switch `notify_sensor`).
- Gate reed: flag `PROFILE_FLAG_GATE_SENSOR` — every `RESULT` line now ends
  `gate=1` (closed again) / `gate=0` (still open → welded-contact suspect).

## RTC sanity (FMEA #22 / #23)

- Flat backup cell: open the RTC case / pull the battery → OSF set →
  `time_ok=0` (`TIME INVALID` heartbeat flag), no scheduled feeds anywhere
  until the next TIME_SYNC.
- Garbage reads: the node rejects I²C reads that jump backwards (>2 s) or
  forward (>60 s) vs the millis() projection —
  `WARN rtc read rejected: … (N total)`, the projection stands in.

## Notify trigger switches (§9.11, rmt blob v2)

Web dashboard → Remote settings now has master/offline/fault/missed/stock/
sensor switches. A v0.9 hub keeps its MQTT credentials across the upgrade
(v1 → v2 prefix migration, `test_v1_blob_migrates`). Turn the master off →
`aio/hub/notify` goes quiet except for administrative rows (unpair).
