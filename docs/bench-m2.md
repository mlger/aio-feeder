# M2 bench checklist — ESP-NOW link (PLAN.md §12 M2)

Code side of M2 is done: link engine (host-tested, 39 CI tests incl. RFC 4231
HMAC vectors) on hub and both node MCUs, frame MAC (truncated HMAC-SHA256,
D13), seq + replay window, FEED/ACK/RESULT with the §6.6 retry policy,
heartbeat + STATUS_REQ ack, channel scan (§6.2), SCHEDULE_SET with
`skip_until`/`hold_until`, CONFIG push, and the bench CLIs on both sides.
The items below need real boards and close the milestone.

## What you need

- ESP32-S3 hub board **and** one node (ESP8266 D1 mini and/or C3 SuperMini)
- Two USB data cables, two serial monitors at 115200 (or one at a time)
- Chrome/Edge + https://mlger.github.io/aio-feeder/ for flashing
- For the live-feed part of the loopback: relay + solenoid rig from bench-m1

## Pairing (serial-provisioned, D29 — OTA pairing UI is M3)

1. Flash the **hub** (`hub_esp32s3`) and a **node** (`node_esp8266` or
   `node_esp32c3`) from the web flasher.
2. Hub serial: `info` → note the hub MAC and channel (default 1).
3. Node serial: `pair <hub_mac>` → prints `key=<64 hex>`. The key lives in
   the node's config slots; it is shown over serial only, once.
4. Node serial: `link` shows its own state; get the node MAC from
   `pair` output context or the hub's HELLO line (unpaired nodes beacon
   HELLO every 5 s — the hub prints them with their MAC).
5. Hub serial: `peer add <node_mac> <node_id> <key>` with any id 1..255.
6. Within ~2 s the hub prints `HB node=…` (heartbeat ack loop keeps the
   node's freshness window fed) and the node's `link` shows `linked=1`.

## M2 exit items (per PLAN.md §12)

1. **1000-feed loopback, no duplicates** (the exit criterion):
   - Node: `bench radio` (RAM-only protocol loopback — FEED is answered with
     RESULT over the air, the relay is never energised).
   - Hub: `feedloop <node_id> 1000` → wait for
     `feedloop done: sent=1000 ok=1000 dup_errors=0`.
   - Repeat with `bench live` for ~20 feeds with the solenoid rig attached to
     confirm the real actuation path over the radio (SR2 still caps it).
   - Power-cycle the node mid-`feedloop`: the journal must answer the retried
     FEED from flash (SR6) and the run must still end `dup_errors=0`.
2. **Range / packet-loss test (§11.2):** node `sendtest 500`, hub `stats` →
   `test_rx` counts received. Repeat at 1 m, 5 m, 20 m, through one wall.
   Record loss rates in the table below. Expect ~0 % indoors at 5 m.
3. **Channel test (§6.2):** hub `chan 6`, node stays on 1 → within 60 s the
   node must print `link lost → channel scan`, then `channel locked: 6`.
   `link` on the node shows `chan=6` (persisted — survives a reboot).
4. **Encrypted-peer limits [VERIFY, both SDKs]:** pair N nodes (start with
   the max you can assemble); record the largest N where every node keeps
   `linked=1` for 10 min. We use unencrypted + HMAC (D13), so this records
   the ceiling for a possible native-encryption switch, not our operating
   point. Write the result into the decision log (D13 note).
5. **RTC trust defaults (D15/D21):** leave a node linked for 24 h (or trust
   the soak from bench-m1); confirm no spurious `time_invalid` flapping in
   `HB flags=0x02`. If flapping, lower `rtc_trust_days` and note it.

## Loss / range log

| Distance / obstacle | sent | received (test_rx) | loss % | notes |
|---|---|---|---|---|

## Peer-limit log (D13 ceiling)

| Boards (SDK) | paired N | all linked 10 min? | notes |
|---|---|---|---|

## Notes / gotchas

- Node `bench off|radio|live` is RAM-only and clears on reset — never
  persisted, SR2 (hard max on-time) and the FAULT latch stay active in every
  mode. Field behaviour = `bench off`.
- The hub's peer table is RAM for the bench (M3 adds storage): after a hub
  reboot, re-run `peer add`.
- `key <hex>` on the node (and the third `peer add` argument on the hub)
  accepts 16- or 32-byte hex keys; `pair` always generates 32 bytes.
- Node CLI `sched` shows the last pushed schedule incl. skip/hold windows —
  use hub `sched <id> <skip_unix> <hold_unix>` to push one and compare.
