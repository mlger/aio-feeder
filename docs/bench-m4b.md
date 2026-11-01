# Bench M4b — hub pairing flow (PLAN.md §6.3, §8.5)

Hub side of OTA pairing, landed for M4b. The node side is still
serial-provisioned (D29): node firmware does not send `PAIR_REQ` yet — until
it does, this flow is exercised with the QEMU sim candidate and the host
tests, and on the bench via the serial CLI.

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

## Bench procedure (serial CLI)

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

`pair close` cancels; the window also self-expires after 60 s. Once node
firmware sends `PAIR_REQ` (button held at boot) and unwraps the key on its
side, this becomes the end-to-end OTA pairing path (next M4b step).
