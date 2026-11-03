# bench-m8 — node OTA on hardware (M8b, v0.12.0)

The CI side proves the wire and the pure logic: `test_link_core` walks
ENTER_OTA hub→node with the blob **ciphertext checked for plaintext
credentials**, the unwrap round-trip, the role guards and the ACK path;
`test_proto` pins the appended message types / flag bits; `test_ota`
covers the nodefw meta store and the 234-byte payload math. What CI
**cannot** prove is Wi-Fi + HTTP + the Update partition on real silicon —
that is this bench run, plus the FMEA #11 lock dance across reboots.

M8a's hub download path lives in `bench-m8a.md`; this document covers the
node side (§7.6, D39/D40).

Prereqs: hub v0.12.0+ on the bench Wi-Fi, node paired and on v0.12.0
(`AIO_NODE_FW_VER=0x000C00`), node .bin from a release zip, serial
consoles on both boards.

## 0. One-time layout migration

- **C3**: pre-M8b images used the Arduino default partition table — flash
  v0.12.0 once over USB (`pio run -d node -e node_esp32c3 -t upload`).
  From then on `partitions_c3_ota.csv` (dual 1.5 MB slots + otadata) is
  in place and further updates go over the air.
- **8266**: layout change only (`eagle.flash.4m1m.ld`); EEPROM keeps its
  flash-end address, config survives, OTA scratch grows to ~2.6 MB.
  Re-flash over USB once as well.
- Node serial banner must read `ready: fw=0.12.0 …`; the hub dashboard
  node card then shows `fw 0.12.0 · esp32c3` (learned from the heartbeat
  tail within 30 s).

## 1. Upload + push, happy path (C3)

1. Dashboard → *Firmware update* → **Node firmware**: pick
   `aio-feeder-nodec3.bin` from the release zip, target `esp32c3`,
   version `0.12.0` → *Upload to hub*. Status line shows
   `stored: esp32c3 v0.12.0 · N KB · sha …`.
2. Node card → *Update FW* → confirm. Expected on the node serial:

   ```console
   RX enter_ota -> accepted (0)
   OTA: joining <hub ssid>
   OTA: wifi up, ip 192.168.1.x
   OTA: flashed 412345 bytes
   OTA: rebooting into the new image
   ```

   and on the hub: `ENTER_OTA_ACK node=1 ok=1 reason=0`, then
   `ENTER_OTA sent node=1 url=http://…/fw/node`.
3. After the reboot: `ready: fw=0.12.0 … ota_lock=0` and
   `OTA: update 0.12.0 -> 0.12.0 confirmed…` **only** if the version
   actually changed — same-version re-push keeps the node locked by
   design (see §3). For this test push the *other* target build's
   version or a bumped `AIO_NODE_FW_VER`.
4. C3-specific: the new slot starts `PENDING_VERIFY`;
   `ota_mark_valid()` confirms at setup end — a build that hangs before
   that rolls back to the old slot (testable only with a doctored
   image; see §4).

## 2. Same run on the ESP8266

Upload `aio-feeder-node8266.bin` (target `esp8266`). The push to an
8266 node must fail with `409 image is for a different MCU` while a C3
node is selected with a C3 image stored — the MCU match comes from the
heartbeat tail. Then store the 8266 image and repeat §1; the eboot
scratch flow reboots into the new image exactly once.

## 3. FMEA #11 — the lock dance

1. **Actuation gate**: while a node is locked (after a refused/failed
   update or a same-version push), `feed 5` on serial answers
   `feed -> rejected: REJ_OTA_LOCK`, a radio FEED is ACKed
   `accepted=0 reason=FEED_REJ_OTA_LOCK`, the fallback schedule is
   silent, and the dashboard node card shows **ota lock**.
2. **Lock survives power cuts**: pull node power mid-download →
   reconnect → boots with
   `OTA: LOCK ACTIVE (from 0.12.0, running 0.12.0, stage 1)` — no feed
   until resolved.
3. **Sha mismatch refused**: upload a truncated/random .bin as the
   image, push → the node downloads, prints
   `OTA: sha256 MISMATCH — image refused`, re-arms ESP-NOW, **stays
   locked**, hub sees the node online again with the old version.
4. **Recovery**: push the correct image (node re-enters OTA even while
   locked) → new version boots → lock clears itself. Serial-only
   escape: `otalock off` → `ota lock cleared (actuation re-enabled)`;
   `otalock` alone prints `lock/stage/from_ver`.

## 4. C3 rollback (doctored image, optional)

Flash an image whose `setup()` hangs before `ota_mark_valid()` (bench
build with `while(1);` after `hal.init()`), push it OTA. Expected: the
bootloader boots it twice, gets no confirm, and **rolls back** to the
previous slot; the node then runs the OLD version with the lock still
set (old image + stage DONE + same `ota_from_ver` → no clear), which is
exactly the D40 "old image back from rollback" case.

## 5. Hub-served image integrity

`curl -o pulled.bin http://<hub-ip>/fw/node` → `sha256sum pulled.bin`
matches the `sha` shown by the dashboard and the release `ota.json`.
HTTP auth: `/fw/node` is open like `/` (the sha pin is the integrity
root); `/api/nodefw*` require the PIN when `web_auth` is on.

## Recorded results

| # | Step | Result |
|---|------|--------|
| 0 | One-time layout migration (C3 + 8266) | pending bench hardware |
| 1 | C3 upload + push + confirm | pending |
| 2 | 8266 push + MCU mismatch 409 | pending |
| 3 | Lock dance (gate, power-cut, sha-mismatch, recovery) | pending |
| 4 | C3 rollback with doctored image | pending |
| 5 | `/fw/node` sha + auth | pending |
