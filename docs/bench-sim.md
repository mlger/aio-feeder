# Bench: fake node + host e2e sim (v0.3, PLAN.md §13, D25)

## What exists

- `tools/sim/fake_node.cpp` — a host-runnable feeder node speaking the M2/M3
  proto over UDP: HEARTBEATs every 30 s, FEED → FEED_ACK → FEED_RESULT after a
  simulated dispense, SR6 journal (duplicates answered, never actuated twice),
  ABORT / TIME_SYNC / SCHEDULE_SET / CONFIG / STATUS_REQ handled.
- `tools/sim/link_udp.h` — `ILinkTransport` over POSIX UDP (one proto frame per
  datagram), with a `drop_next()` black-hole hook for loss tests.
- `test/test_e2e_sim/` — the full hub pipeline (HubRegistry + HubCore +
  LinkEngine hub role) against that fake node over real loopback sockets, with
  a virtual clock (1 sim-ms per step, unix clock ticking 1 s per 1000 ms).

## e2e coverage (all green locally and in CI)

| Test | Exercised |
|---|---|
| `scheduled_feed_full_pipeline` | schedule entry → occurrence fires at local minute → FEED → ACK → RESULT → one `FEED_OK` event, 20 g dispensed exactly once |
| `dropped_feed_retries_once_actuates_once` | first FEED datagram dropped → hub retry fires at +300 ms (§6.6) → single actuation (SR6), feed settles |
| `result_repeats_cover_lost_results` | RESULT repeats 1–2 dropped → repeat 3 lands → exactly one event (§6.6 repeat, §9.7 dedup) |
| `time_sync_push_on_invalid_node_clock` | heartbeat flagged `HB_FLAG_TIME_INVALID` → hub pushes TIME_SYNC (§7.4) → node clears the flag |

## Run it

```bash
pio test -d test                  # CI path (real Unity)
tools/test-local/run.sh           # g++ path when the PlatformIO CDN is blocked
```

Standalone fake node against a real hub (or future QEMU chardev):

```bash
g++ -std=c++17 -Ilib/proto -Ilink_core -Itools/sim tools/sim/fake_node.cpp -o fake_node
./fake_node --port 9001 --hub 127.0.0.1:9000 --id 1
# hub side: peer add 02:00:00:00:00:02 1 <key>   (key must match --key / sim default 32×0xA5)
```

## Not covered here (bench items, need hardware)

- ESP-NOW radio path itself (M2 bench, `docs/bench-m2.md`).
- Real actuator timing/limits (M1, `docs/bench-m1.md`).
- In-QEMU hub ↔ fake-node transport (chardev) — deferred to M4a, D25.
