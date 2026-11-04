#!/usr/bin/env python3
"""
AIO Feeder — QEMU UI screenshot runner (PLAN.md §7.7, §12 M4a).

Boots the hub flash image in the Espressif QEMU fork (esp32s3 machine),
watches the serial log for the firmware's "AIO_SHOT <name>" markers, and
dumps the LVGL framebuffer (ELF symbol g_shot_fb) via the QEMU human
monitor `pmemsave` command. Each dump is composed into the 800×480 panel
image the hardware shows (display_hw.cpp flush_cb: the 352×240 LVGL
render 2× nearest-neighbor expanded into the centered 704×480 window,
48 px black side bars) and written as PNG.

No third-party Python deps (hand-rolled PNG writer on zlib).

Usage (paths relative to the repo root, as in ci.yml):

  python3 tools/qemu/capture.py \
      --qemu /tmp/qemu/qemu/bin/qemu-system-xtensa \
      --flash hub/build/flash.img \
      --elf hub/build/aio-feeder-hub.elf \
      --screens dashboard --out screens --timeout 75

Outputs:
  screens/<name>.png   one PNG per captured screen
  serial.log           full guest serial output
Exits non-zero if any requested screen is missing at the end.
"""

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import time
import zlib

MARKER = re.compile(rb"AIO_SHOT\s+([A-Za-z0-9_\-]+)")
FB_SYMBOL = "g_shot_fb"
FB_W, FB_H = 352, 240  # LVGL logical render — must match hub/main/ui/ui.cpp
FB_BYTES = FB_W * FB_H * 2

# The 800×480 ST7262 panel image (display_hw.cpp board constants): the
# logical frame ×2 nearest-neighbor expanded into a 704×480 window centered
# in the panel — 48 px black bars left and right, full-height. Captures are
# composed to this size so the PNGs show what the device actually displays,
# not the shrunken logical canvas.
PANEL_W, PANEL_H = 800, 480
SCALE = 2
WIN_W = FB_W * SCALE          # 704
WIN_H = FB_H * SCALE          # 480 (no top/bottom bars)
WIN_X = (PANEL_W - WIN_W) // 2  # 48 px bars


def fb_addr_from_elf(elf: str, symbol: str, nm: str) -> int:
    out = subprocess.check_output([nm, elf], text=True, errors="replace")
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == symbol:
            return int(parts[0], 16)
    raise SystemExit(f"error: symbol {symbol!r} not found in {elf} (nm={nm})")


def write_png(path: str, w: int, h: int, rgb: bytes) -> None:
    stride = w * 3
    raw = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride] for y in range(h))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (len(data).to_bytes(4, "big") + tag + data +
                (zlib.crc32(tag + data) & 0xFFFFFFFF).to_bytes(4, "big"))

    ihdr = (w).to_bytes(4, "big") + (h).to_bytes(4, "big") + bytes([8, 2, 0, 0, 0])
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def rgb565le_to_rgb888(data: bytes, w: int, h: int) -> bytes:
    out = bytearray(w * h * 3)
    for i in range(w * h):
        px = data[2 * i] | (data[2 * i + 1] << 8)
        r = ((px >> 11) & 0x1F) << 3
        g = ((px >> 5) & 0x3F) << 2
        b = (px & 0x1F) << 3
        out[3 * i] = r | (r >> 5)     # replicate MSBs → full-range 8-bit
        out[3 * i + 1] = g | (g >> 6)
        out[3 * i + 2] = b | (b >> 5)
    return bytes(out)


def compose_panel(fb: bytes) -> bytes:
    """Replicate display_hw.cpp flush_cb on the host: the 352×240 LVGL
    frame ×2 nearest-neighbor expanded into the 704×480 window, centered
    in the 800×480 panel. Bars stay black from the zeroed buffer exactly
    like the panel framebuffer's init memset."""
    logical = rgb565le_to_rgb888(fb, FB_W, FB_H)
    panel = bytearray(PANEL_W * PANEL_H * 3)  # zeroed → black bars
    win_row = bytearray(WIN_W * 3)            # one expanded logical row
    row = FB_W * 3
    for ly in range(FB_H):
        src = logical[ly * row:(ly + 1) * row]
        for lx in range(FB_W):
            s3 = lx * 3
            d6 = lx * 6
            px = src[s3:s3 + 3]
            win_row[d6:d6 + 3] = px
            win_row[d6 + 3:d6 + 6] = px
        for dy in (0, 1):  # each logical row lands on two panel rows
            dst = (ly * SCALE + dy) * PANEL_W * 3 + WIN_X * 3
            panel[dst:dst + WIN_W * 3] = win_row
    return bytes(panel)


class Monitor:
    """QEMU human monitor over a unix socket."""

    def __init__(self, path: str):
        self.path = path
        self.sock = None

    def connect(self, timeout_s: float = 30.0) -> None:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.settimeout(2.0)
                self.sock.connect(self.path)
                self._drain(0.5)  # banner + first prompt
                return
            except OSError:
                if self.sock:
                    self.sock.close()
                self.sock = None
                time.sleep(0.2)
        raise SystemExit("error: could not connect to the QEMU monitor socket")

    def _drain(self, wait_s: float) -> bytes:
        buf = b""
        deadline = time.time() + wait_s
        while time.time() < deadline:
            try:
                piece = self.sock.recv(4096)
                if piece:
                    buf += piece
                    deadline = time.time() + wait_s  # keep reading while active
            except socket.timeout:
                break
            except OSError:
                break
        return buf

    def command(self, cmd: str) -> bytes:
        self.sock.sendall((cmd + "\n").encode())
        return self._drain(1.0)

    def pmemsave(self, addr: int, size: int, path: str) -> None:
        self.command(f"pmemsave {addr} {size} {path}")
        deadline = time.time() + 10.0
        while time.time() < deadline:
            if os.path.exists(path) and os.path.getsize(path) == size:
                return
            time.sleep(0.05)
        raise SystemExit(f"error: pmemsave did not produce {path} ({size} bytes)")

    def quit(self) -> None:
        try:
            self.command("quit")
        except OSError:
            pass


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", required=True, help="qemu-system-xtensa binary")
    ap.add_argument("--flash", required=True, help="merged flash image")
    ap.add_argument("--elf", required=True, help="app ELF (for the fb symbol)")
    ap.add_argument("--screens", default="dashboard",
                    help="comma-separated screen names to capture")
    ap.add_argument("--out", default="screens", help="output dir for PNGs")
    ap.add_argument("--serial", default="serial.log", help="serial log path")
    ap.add_argument("--qemu-log", default="qemu.log", help="QEMU stdout/stderr path")
    ap.add_argument("--monitor", default="/tmp/aio-qemu-mon.sock", help="monitor socket")
    ap.add_argument("--nm", default="xtensa-esp-elf-nm", help="nm binary")
    ap.add_argument("--timeout", type=float, default=75.0, help="overall deadline (s)")
    args = ap.parse_args()

    screens = [s.strip() for s in args.screens.split(",") if s.strip()]
    os.makedirs(args.out, exist_ok=True)

    if shutil.which(args.nm):
        nm = args.nm
    else:  # fall back to plain nm (host builds / non-IDF environments)
        nm = "nm"
    addr = fb_addr_from_elf(args.elf, FB_SYMBOL, nm)
    print(f"[capture] {FB_SYMBOL} @ 0x{addr:08X} ({nm})")

    serial_path = args.serial
    if os.path.exists(serial_path):
        os.remove(serial_path)

    qemu_cmd = [
        args.qemu,
        "-M", "esp32s3",
        "-drive", f"file={args.flash},if=mtd,format=raw",
        "-display", "none",
        "-serial", f"file:{serial_path}",
        "-monitor", f"unix:{args.monitor},server,nowait",
        "-no-reboot",
    ]
    if os.path.exists(args.monitor):
        os.remove(args.monitor)
    print("[capture] qemu:", " ".join(qemu_cmd))
    # keep QEMU's own output — an early exit with no guest serial is
    # undiagnosable otherwise (v0.6: QEMU died twice right after the app
    # banner with nothing on serial)
    qemu_out = open(args.qemu_log, "w")
    qemu = subprocess.Popen(qemu_cmd, stdout=qemu_out, stderr=subprocess.STDOUT)

    mon = Monitor(args.monitor)
    deadline = time.time() + args.timeout
    pending = set(screens)
    seen_markers = set()
    captured = []

    try:
        # wait for the socket to appear, then attach
        sock_deadline = time.time() + 20.0
        while time.time() < sock_deadline and not os.path.exists(args.monitor):
            if qemu.poll() is not None:
                raise SystemExit(f"error: qemu exited early (rc={qemu.returncode})")
            time.sleep(0.1)
        mon.connect()

        pos = 0
        while pending and time.time() < deadline:
            if qemu.poll() is not None:
                print("[capture] qemu exited early")
                break
            time.sleep(0.2)
            try:
                with open(serial_path, "rb") as f:
                    f.seek(pos)
                    chunk = f.read()
                    pos = f.tell()
            except FileNotFoundError:
                continue
            for m in MARKER.finditer(chunk):
                name = m.group(1).decode()
                if name in pending:
                    # frame settled: firmware prints the marker ≥400 ms after
                    # the last flush and nothing invalidates afterwards
                    time.sleep(0.8)
                    raw = os.path.join(args.out, f"{name}.raw")
                    mon.pmemsave(addr, FB_BYTES, raw)
                    png = os.path.join(args.out, f"{name}.png")
                    with open(raw, "rb") as f:
                        data = f.read()
                    write_png(png, PANEL_W, PANEL_H, compose_panel(data))
                    os.remove(raw)
                    print(f"[capture] saved {png} "
                          f"({PANEL_W}x{PANEL_H} panel, {len(data)} bytes raw fb)")
                    pending.discard(name)
                    captured.append(name)
                seen_markers.add(name)
    finally:
        if mon.sock:
            mon.quit()
        if qemu.poll() is None:
            qemu.terminate()
            try:
                qemu.wait(timeout=5)
            except subprocess.TimeoutExpired:
                qemu.kill()

    missing = [s for s in screens if s not in captured]
    if missing:
        print(f"[capture] MISSING screens: {', '.join(missing)} "
              f"(markers seen: {sorted(seen_markers) or 'none'})")
        try:
            with open(args.qemu_log, encoding="utf-8", errors="replace") as f:
                tail = f.read()[-2000:]
            if tail.strip():
                print("[capture] --- qemu.log tail ---")
                print(tail)
        except OSError:
            pass
        # no markers at all usually means the UI task died at boot — show
        # the head of the serial log (the panic lines live there, the tail
        # only shows the engine drivers still ticking)
        try:
            with open(serial_path, encoding="utf-8", errors="replace") as f:
                head = "".join(f.readlines()[:80])
            if head.strip():
                print("[capture] --- serial.log head (boot) ---")
                print(head)
        except OSError:
            pass
        return 1
    print(f"[capture] done: {', '.join(captured)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
