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
