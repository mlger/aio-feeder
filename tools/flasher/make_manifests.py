#!/usr/bin/env python3
"""Assemble the GitHub Pages web flasher site (ESP Web Tools).

Reads per-env firmware build outputs, copies the static site from web/, writes
one ESP Web Tools manifest.json per target plus build-info.json.

Usage: make_manifests.py --build-dir BUILD_DIR --version V [--web DIR] [--out DIR]

BUILD_DIR layout (what the CI pages job assembles from artifacts):
    BUILD_DIR/hub_esp32s3/{bootloader,partitions,boot_app0,firmware}.bin
    BUILD_DIR/node_esp8266/firmware.bin
    BUILD_DIR/node_esp32c3/{bootloader,partitions,boot_app0,firmware}.bin

boot_app0.bin is optional (absent on non-Arduino builds). Flash offsets follow
esptool defaults per chip family; the ESP8266 firmware.bin embeds eboot and
flashes as one image at 0x0.
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

# env -> (site dir, chipFamily, [(bin name, flash offset)], required bin names)
#
# ESP32-S3/C3: bootloader + partitions + OTA data + app (esptool defaults).
# ESP8266: PIO's firmware.bin is ONE image that already embeds eboot — flash
# the whole thing at 0x0, no separate bootloader part.
TARGETS: dict[str, tuple[str, str, list[tuple[str, int]], tuple[str, ...]]] = {
    "hub_esp32s3": (
        "hub",
        "ESP32-S3",
        [
            ("bootloader.bin", 0x0),
            ("partitions.bin", 0x8000),
            ("boot_app0.bin", 0xE000),
            ("firmware.bin", 0x10000),
        ],
        ("firmware.bin", "bootloader.bin", "partitions.bin"),
    ),
    "node_esp32c3": (
        "node-esp32c3",
        "ESP32-C3",
        [
            ("bootloader.bin", 0x0),
            ("partitions.bin", 0x8000),
            ("boot_app0.bin", 0xE000),
            ("firmware.bin", 0x10000),
        ],
        ("firmware.bin", "bootloader.bin", "partitions.bin"),
    ),
    "node_esp8266": (
        "node-esp8266",
        "ESP8266",
        [
            ("firmware.bin", 0x0),
        ],
        ("firmware.bin",),
    ),
}
TITLES = {
    "hub_esp32s3": "AIO Feeder Hub (ESP32-S3)",
    "node_esp32c3": "AIO Feeder Node (ESP32-C3)",
    "node_esp8266": "AIO Feeder Node (ESP8266)",
}


def build_manifest(version: str, env: str) -> dict:
    site_dir, family, parts, _required = TARGETS[env]
    return {
        "name": TITLES[env],
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": family,
                # part paths resolve against the manifest URL, and every part
                # lands next to its manifest.json — bare names only.
                "parts": [{"path": name, "offset": offset} for name, offset in parts],
            }
        ],
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", type=Path, required=True)
    ap.add_argument("--version", required=True, help="firmware version string (e.g. git describe)")
    ap.add_argument("--web", type=Path, default=Path("web"), help="static site source dir")
    ap.add_argument("--out", type=Path, default=Path("site"), help="output dir for Pages")
    args = ap.parse_args()

    if not args.web.is_dir():
        print(f"error: web dir not found: {args.web}", file=sys.stderr)
        return 1
    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)

    # static site + nojekyll so esp-web-tools assets serve as-is
    shutil.copytree(args.web, args.out, dirs_exist_ok=True)
    (args.out / ".nojekyll").write_text("")

    for env, (site_dir, _family, parts, required) in TARGETS.items():
        env_dir = args.build_dir / env
        missing_required = [n for n in required if not (env_dir / n).is_file()]
        if missing_required:
            print(f"error: {env}: missing {', '.join(missing_required)} in {env_dir}",
                  file=sys.stderr)
            return 1
        out_dir = args.out / site_dir
        out_dir.mkdir()
        for name, _offset in parts:
            src = env_dir / name
            if src.is_file():  # boot_app0.bin may be absent on IDF builds
                shutil.copy2(src, out_dir / name)
        # keep the manifest consistent with what was actually written
        manifest = build_manifest(args.version, env)
        manifest["builds"][0]["parts"] = [
            {"path": n, "offset": o}
            for n, o in parts
            if (out_dir / n).is_file()
        ]
        (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(f"{env}: {out_dir / 'manifest.json'} ({len(manifest['builds'][0]['parts'])} parts)")

    (args.out / "build-info.json").write_text(json.dumps({"version": args.version}) + "\n")
    print(f"site ready: {args.out} (version {args.version})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
