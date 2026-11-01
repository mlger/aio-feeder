# Bench M4b — hub pairing flow (PLAN.md §6.3, §8.5)

Full OTA pairing, hub **and** node side. An unpaired node beacons
`HELLO` + `PAIR_REQ` every 5 s (stable persisted nonce, §6.3.2); the hub's
60 s window picks it up; after the user confirms the 4-digit code both ways,
the hub commits (key wrapped under the code, registry + peer + NVS) and the
node unwraps + stores the key on `pairok`.

## What the hub does now

`pair open` (or "Add node" once the settings UI lands) opens the **60 s
pairing window** (§6.3.1). Any unpaired node beaconing `PAIR_REQ` joins the
discovery list (max 4 candidates, already-paired MACs ignored). For every
candidate the hub derives a **4-digit confirmation code**
(`lib/proto/pairing.h`: HMAC over node MAC + node nonce + a fresh 8-byte hub
nonce) — both sides show the same digits; the user confirms they match
(§6.3.4, D14). Repeat `PAIR_REQ`s refresh RSSI only; the displayed code never
changes mid-confirm.

After the code is confirmed, the form step collects **name, mech template
(gate 1500 g / auger 5000 g / pump 1000 ml), full hopper size and the amount
in it now** (§8.5, §9.7 rules: total 1–100000, amount ≤ total, amber summary
at ≤ 20 %). `commit` then:

1. generates a random 256-bit per-node key (`esp_fill_random`),
2. wraps it under the confirmed code (XOR keystream keyed by the 4 digits +
   both nonces) — the key never crosses the air in the clear (§6.3.5),
3. adds the node to the registry (id, channel, key, CRC'd profile),
4. returns the `PAIR_ACCEPT` payload; the hub sends it as a **zero-key
   discovery frame** (§6.4), adds the link peer and persists the registry
   blob to NVS.

The session stays in DONE until closed, so the UI can show "Feeder #N
added". Window expiry (60 s) or cancel drops everything un-committed.

Residual risk, documented per §6.3.5: the wrap entropy is the 4-digit code
(10^4). It defeats passive listeners, not offline brute force — the node-side
safety caps (SR2–SR4) remain the real protection.

## Host tests

`test/test_pairing` (18 cases, in the CI `host-tests` job): flow order,
code stability under repeats, distinct codes per candidate, window expiry,
select/commit phase guards, wrap round-trip + wrong-code rejection,
§9.7 capacity boundaries, registry-full and already-paired races, default
name, pump-ml template, capacity summary math.

## QEMU screenshots

The shot build adds a fourth sim engine — an unpaired candidate beaconing
genuine `PAIR_REQ`s (`hub/main/sim_nodes.h`). Inside the pairing rotation
slot the flow auto-plays: discover → code → form → done, and the LVGL
pairing screen (`hub/main/ui/ui.cpp`) renders each step. Captured in the CI
`qemu-hub` job as `pairing.png`, `pairing_code.png`, `pairing_form.png`,
`pairing_done.png` (artifact `hub-ui-screens`).

## Bench procedure (node side)

Power the node unpaired — it prints its MAC and beacons:

```
ESP-NOW up: unpaired, beaconing HELLO+PAIR_REQ every 5 s (mac 02:xx:…)
```

Then drive the hub as above. When the hub commits, the node prints:

```
PAIR code 1234 from 02:00:00:00:00:01 — matches the hub screen? 'pairok' to accept, ignore to decline
```

Compare the digits with the hub screen (`pair status`), then:

```
pairok            → paired: id=4 chan=1 key stored (wrapped, never on air in the clear)
link              → hub peer up, heartbeats flowing
```

A mismatched/stale accept is ignored (the node re-derives the code from its
own nonce + the accept's hub nonce). `pair <hub_mac>` (serial key
provisioning) still works as the fallback path. The config blob grew by the
8-byte `pair_nonce`; existing saved configs fall back to defaults on first
boot after this update (bench-only impact, pre-deployment).

## Bench procedure (hub, serial CLI)

```
aio-feeder> pair open
aio-feeder> pair status          # step=discover cands=1 ... (node beacons PAIR_REQ)
aio-feeder> pair select 0
aio-feeder> pair status          # code=1234 — compare with the node's display
aio-feeder> pair confirm
aio-feeder> pair form kitA 0 1500 400     # name, mech 0=gate/1=auger/2=pump, full, in-it
committed — PAIR_ACCEPT sent
aio-feeder> peers                # new peer listed, heartbeats flowing
```

`pair close` cancels; the window also self-expires after 60 s. With the
node beacons + `pairok` above this is the full end-to-end OTA pairing path
(§6.3) — the key crosses the air only XOR-masked under the confirmed code.

## Calibration wizard (§8.4, §9.3, D27)

`hub/src/calib_session.h` — pure logic, host-tested (`test/test_calib`,
22 cases). It calibrates a node **by open time** (g/s or ml/s, D27): the
hub sends the commanded test `FEED` (bypasses feed_manager like the M2
feedloop — the node still enforces every profile limit, §9.3.2), the node
answers `FEED_RESULT` with `actual_on_ms`, the operator weighs what came
out and enters it. Guards per §9.3:

- test run is **2 s by default for a gate** (5 s for auger/pump), never
  above the node's max on-time (SR2; gate capped at 10 s, D27);
- weighed amounts **under 5 g (or ml) are refused** (§9.3.3) — run longer;
- the derived ms-per-unit rate is sanity-ranged to what
  `cal_ms_per_unit_x100` (uint16) can store — 10 ms/unit … 655.35 ms/unit
  (§9.3.4); outside that band it's a scale or food problem;
- repeat runs become samples (up to 5); **the median is stored** (§9.3.5),
  written into the profile with a fresh CRC, persisted to NVS and pushed to
  the node as CONFIG.

## Calibration wizard — bench procedure (hub, serial CLI)

```
aio-feeder> cal start 1        # wizard open (2 s test, gate default)
aio-feeder> cal secs 3         # optional: longer run (≤ max on-time)
aio-feeder> cal run            # test FEED sent — bowl under the gate
aio-feeder> cal weigh 8.2      # enter what the scale said (min 5 g)
aio-feeder> cal run            # repeat; the median is what gets stored
aio-feeder> cal weigh 8.0
aio-feeder> cal run
aio-feeder> cal weigh 8.1
aio-feeder> cal save           # median → profile (CRC'd), NVS, CONFIG push
aio-feeder> cal status         # step=done node=1 test=2s runs=3 median=...
```

The LVGL calibration screen drives the same session from the touchscreen
(§8.4: choose run length → dispense → weigh → review median → save) and the
QEMU shot build auto-plays the flow in rotation slot 6 of 7 — sim node 1
answers the test feeds, the driver weighs 8.0 / 7.5 / 8.2 g, and the runner
captures `calib.png` (setup + runs list), `calib_weigh.png` (weigh prompt)
and `calib_done.png` (median saved + pushed) into the `hub-ui-screens`
artifact. The brief DISPENSE state is machine-paced and shares the `calib`
marker; the serial log shows every `FEED`/`FEED_RESULT` of the flow.

Residual notes: the dispensed test food is consumed (day/hour caps count
it, §9.3); the profile has no calibration-timestamp field yet — the §9.3.6
"calibration age" notice lands with the export/backup work.


## Settings screen, PIN lock (§8.8, §8 UX, FR10)

The settings screen follows the §8.8 jump bar — Restart, Display, Time,
Network, Remote, Security, Storage, Backup, Wizard, Danger — as a section
list plus the active pane. It renders `ui::SettingsView` snapshots built by
`build_settings()` from the registry + the settings blob + injected
environment (SD status, backup file count), so the same presenter runs in
host tests and under QEMU.

The settings blob (`hub/src/settings_store.h`, own CRC'd NVS key "set"
beside the registry's "reg") carries brightness (10-100 %), screen-off
timeout (0/30/60/120/300 s), history retention (7/30/90 d, §7.5), NTP
on/off, and the Wi-Fi credentials (password never leaves the device — it is
also excluded from backups, D31). The tz RULE stays in the registry: one
source of truth the scheduler and TIME_SYNC already read.

**PIN** (FR10, D33): 4 digits, stored as a salted hash in the blob
(`pin_set` resalts on every change — equal codes hash differently across
hubs). The RAM `PinLock` implements the §8 rules: a correct code unlocks
for 60 s, then auto re-locks; scope is "settings only" or "settings and
manual feeds"; **schedules and automatic feeds are never blocked** (the
scheduler never consults the lock). When the scope covers feeds, `feed`
asks for the code:

```
aio-feeder> pin set 0000 1234        # current, then new
aio-feeder> pin scope 1              # settings and manual feeds
aio-feeder> feed 1 40                # PIN required: feed <id> <amount> <pin>
aio-feeder> feed 1 40 1234           # unlocked for 60 s
aio-feeder> pin off 1234
```

The touchscreen flow is the PIN pad screen (view-side pad state,
`pin_press`/`pin_note`); the M5 touch backend wires the callbacks. The
serial console stays an owner path (no prompt) — the lock protects the
exposed surface (D33).

Host tests: `test/test_settings` (10) — blob round-trip + CRC rejection,
validators, salted hashes, 60 s window, scope gating, retention prune.

## Backup, restore and CSV export (§9.12, FR19, D18/D31/D32)

`backup now` serializes nodes (mac/key/profile CRC'd/schedules) + the
settings patch into `aio-backup-<YYYYMMDD-HHMM>.json` on the card; `backup
csv` writes `aio-history-<...>.csv` with the §9.12 columns
`time,node,amount,unit,result,source` and RFC-4180 quoting (names with
commas/quotes round-trip; test-covered). `backup restore <name>` parses
strictly (unknown keys refused) and replaces nodes + schedules + settings —
history, the current PIN and the current Wi-Fi password survive; sched_version
bumps and `pushed_at` clears so every node is re-pushed (CONFIG +
SCHEDULE_SET) right away; `feed_id_seq` never regresses (§6.6).

Exclusions (D18/D31): no PIN material, no MQTT password, no Wi-Fi password.
Per-node link keys ARE included — the primary restore case is a replaced
hub, and re-pairing every feeder would defeat it.

```
aio-feeder> backup now              # backup: aio-backup-20261101-2140.json (1523 B)
aio-feeder> backup ls
aio-feeder> backup csv
aio-feeder> backup restore aio-backup-20261101-2140.json
```

The card is SDMMC 1-bit on GPIO 11/12/13 (Kconfig `AIO_SD_*`), mounted
best-effort at boot — feeding never depends on it (§7.5); the Storage pane
shows the outcome. In QEMU (no SD emulated, §7.7) the same `IBackupStore`
interface is backed by a heap-allocated RAM "virtual SD" (D32), so the
Backup pane screenshots list a file actually written by the boot sequence.

Host tests: `test/test_backup` (8) — round-trip, exclusions, restore
semantics, corrupt/truncated/unknown-key rejection, buffer overflow,
CSV quoting.

## Setup wizard (§9.13, FR21)

`hub/src/wizard_session.h` — pure, host-tested (`test/test_wizard`, 10).
Six steps with Back at every one: (1) tz + NTP, (2) Wi-Fi + connection
test, (3) choose a paired feeder or pair a new one (the wizard resumes via
`on_paired()` when the pairing commit lands), (4) hopper size + amount in
it now — §9.7 rules, **skipped when the node was just paired**, (5) first
schedule (time, days preset, amount within the node's per-feed cap = max
on-time ÷ calibrated rate, `profile_feed_cap_units`), (6) done with
calibrate/Refilled reminders. Nothing touches the registry until the
schedule step is saved — `commit()` is the only write, then the caller
persists and pushes SCHEDULE_SET + CONFIG.

```
aio-feeder> wiz start
aio-feeder> wiz tz 1 && wiz next            # zone, step 2
aio-feeder> wiz wifi home-net secret && wiz next
aio-feeder> wiz node 1 && wiz next          # or: wiz node new + pair open
aio-feeder> wiz cap 1500 400 && wiz next
aio-feeder> wiz sched 480 127 40
aio-feeder> wiz save                        # commit + SCHEDULE_SET + CONFIG
```

The LVGL wizard screen renders `ui::WizardView` snapshots; the QEMU shot
driver auto-plays the flow in rotation slot 9 (tz → node → cap → sched →
done) and restores the node's seeded schedule afterwards so the other slots
stay deterministic.

## QEMU screenshots (M4b remainder)

The rotation is 25 s × 10 slots; the new drivers pace their phases across
the marker beats (pairing-style). Captured in the `qemu-hub` job as
`settings`, `settings_security`, `settings_backup`, `pin_entry`,
`pin_typing`, `pin_wrong`, `pin_ok`, `wizard`, `wizard_node`, `wizard_cap`,
`wizard_sched`, `wizard_done` (artifact `hub-ui-screens`).

Residual notes: the Wi-Fi connection test in the wizard is honest-by-state —
on hardware it reports the bring-up result, under QEMU/bench CLI it records
"no radio" and the flow continues (the step never blocks setup). The PIN
hash is a salted 32-bit mix, not crypto — the threat model is the
touchscreen (§8), the node-side caps remain the real fence (§3.3).
