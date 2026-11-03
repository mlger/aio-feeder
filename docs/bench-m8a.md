# bench-m8a — hub OTA on hardware (M8a, v0.11.0)

The CI/QEMU side (`ota-selftest` job) proves the boot-slot machinery:
stage → PENDING_VERIFY → health confirm → non-confirming image →
bootloader rollback. What QEMU **cannot** prove is the download path
(esp_http_client over real Wi-Fi, https against the cert bundle, content
lengths, redirects) — that is this bench run.

Prereqs: hub on the bench Wi-Fi (wizard or `set wifi`), a machine on the
same LAN serving an image, serial console up.

## 1. Plain-HTTP pull, stage, apply, confirm

```console
# serve the hub image from a laptop:
python3 -m http.server 8000 -d hub/build   # firmware.bin from any CI artifact

aio-feeder> ota url http://192.168.1.50:8000/firmware.bin
url saved
aio-feeder> ota start
ota: pulling http://192.168.1.50:8000/firmware.bin
download started
aio-feeder> ota
running factory v0.11.0
state: downloading 47%
...
staged target: v0.11.0-N-gSHA
state: need reboot
aio-feeder> ota apply
```

Expected: reboot into `ota_0`, banner prints the same version, and within
5 min (default `AIO_OTA_CONFIRM_MS`) the serial log shows
`ota: image confirmed valid (…)`; `ota` then reports `state: valid`,
`running ota_0`. Feeding/schedules keep firing throughout the download —
confirm at least one scheduled feed lands while `downloading`.

## 2. Reboot persistence + status blob

After the confirm: `reboot` (CLI) → still boots `ota_0` (VALID, not
pending). `ota` shows the last terminal event (`last: ok … version`).

## 3. sha-mismatch refusal (optional but recommended)

Serve a corrupted image (flip a byte in a copy), set its URL, start:
`ota` must end at `state: failed (sha)` when a pinned digest is supplied.
v0.11 ships the digest in `ota.json` beside the Pages/release images; the
bench check is to point the hub at a *mismatched* pair and observe the
refusal before any flash write (nothing is written — the download runs
into the slot but `esp_ota_set_boot_partition` never happens; the hub
keeps running from the old slot with `need reboot` absent).

## 4. Power-cut during download

Pull the plug mid-download (state `downloading`), restore. Expected:
old image boots (otadata untouched — the staged slot is only selected by
`esp_ota_set_boot_partition`), `ota` reports `failed (write)` or a fresh
`idle`, feeding resumes normally, no rollback marker.

## 5. Factory escape hatch

From the OTA slot: `ota factory` → reboot lands on `factory`, registry /
settings / history intact (NVS is shared, only the app slot changes).
`tap_settings` → Firmware pane on the touchscreen shows
`Running: factory <ver>`.

## 6. HTTPS against the Pages site

```console
aio-feeder> ota url https://mlger.github.io/aio-feeder/hub/firmware.bin
aio-feeder> ota start
```

Expected: cert-bundle validation passes (no `esp-tls` errors), download
completes, `staged target:` matches the site's `ota.json` version.

## Recorded results

| # | Step | Result |
|---|------|--------|
| 1 | HTTP pull + stage + apply + confirm | pending bench hardware |
| 2 | Reboot persistence | pending |
| 3 | sha mismatch | pending |
| 4 | Power-cut mid-download | pending |
| 5 | Factory escape | pending |
| 6 | HTTPS Pages pull | pending |
