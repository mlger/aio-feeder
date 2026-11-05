#!/usr/bin/env python3
"""
AIO Feeder — QEMU OTA selftest runner (PLAN.md §7.6, §12 M8a).

Boots the hub flash image (built with sdkconfig.qemu.ota.defaults, which
enables CONFIG_AIO_OTA_SELFTEST) and watches the serial log for the boot
walk's markers. Unlike capture.py this omits -no-reboot: the walk IS a
sequence of five guest resets in one QEMU process.

Markers, in order (ota_service.cpp / badapp):
  ota: selftest stage0 staged        copy of the running image → ota_0
  ota: running ota_0 ... PENDING_VERIFY
  ota: image confirmed valid         health window elapsed
  ota: selftest stage2 staged        badapp → ota_1
  badapp: running                    the non-confirming image
  ota: selftest rollback VERIFIED    bootloader invalidated ota_1

Exits 0 only if every marker appeared in order before the deadline.
"""

import argparse
import re
import subprocess
import sys
import time

MARKERS = [
    r"selftest stage0 staged",
    r"PENDING_VERIFY",
    r"image confirmed valid",
    r"selftest stage2 staged",
    r"badapp: running",
    r"selftest rollback VERIFIED",
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", required=True, help="qemu-system-xtensa binary")
    ap.add_argument("--flash", required=True, help="merged 16 MB flash image")
    ap.add_argument("--serial", default="ota_selftest.log", help="serial log path")
    ap.add_argument("--qemu-log", default="ota_qemu.log", help="QEMU own output")
    ap.add_argument("--timeout", type=float, default=240.0, help="overall deadline (s)")
    args = ap.parse_args()

    import os
    if os.path.exists(args.serial):
        os.remove(args.serial)

    # no -no-reboot: guest resets must continue inside this process
    qemu_cmd = [
        args.qemu,
        "-M", "esp32s3",
        "-drive", f"file={args.flash},if=mtd,format=raw",
        "-display", "none",
        "-serial", f"file:{args.serial}",
        "-monitor", "none",
    ]
    print("[ota-selftest] qemu:", " ".join(qemu_cmd))
    qemu_out = open(args.qemu_log, "w")
    qemu = subprocess.Popen(qemu_cmd, stdout=qemu_out, stderr=subprocess.STDOUT)

    deadline = time.time() + args.timeout
    pending = list(MARKERS)
    pos = 0
    # rolling window: each selftest marker prints exactly ONCE across the
    # five guest reboots, so a line split across two serial polls must not
    # be lost (per-chunk search used to fail the job intermittently)
    tail = ""
    ok = False
    try:
        while time.time() < deadline:
            exited = qemu.poll() is not None
            try:
                with open(args.serial, "rb") as f:
                    f.seek(pos)
                    chunk = f.read().decode("utf-8", errors="replace")
                    pos = f.tell()
            except FileNotFoundError:
                chunk = ""
            tail += chunk
            while pending and re.search(pending[0], tail):
                print(f"[ota-selftest] marker: {pending[0]}")
                pending.pop(0)
            if len(tail) > 512:
                tail = tail[-512:]
            if not pending:
                ok = True
                if exited:
                    print("[ota-selftest] walk complete — qemu exited after "
                          "the last marker (model artifact, walk stands)")
                break
            if exited:
                # drain once more before giving up: the guest may have
                # printed the final marker in the same instant the model
                # died, before any poll wakeup got to read it
                print(f"[ota-selftest] qemu exited early (rc={qemu.returncode})")
                break
            time.sleep(0.3)
    finally:
        if qemu.poll() is None:
            qemu.terminate()
            try:
                qemu.wait(timeout=5)
            except subprocess.TimeoutExpired:
                qemu.kill()
        qemu_out.close()

    if not ok:
        print(f"[ota-selftest] MISSING markers: {pending}")
        try:
            with open(args.serial, encoding="utf-8", errors="replace") as f:
                lines = f.readlines()
            print("[ota-selftest] --- serial tail ---")
            print("".join(lines[-60:]))
        except OSError:
            pass
        return 1
    print("[ota-selftest] complete: boot-slot switch, confirm, rollback all verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
