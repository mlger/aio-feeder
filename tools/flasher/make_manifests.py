#!/usr/bin/env python3
"""Assemble the GitHub Pages web flasher site (ESP Web Tools).

Reads per-env firmware build outputs, copies the static site from web/, writes
one ESP Web Tools manifest.json per target plus build-info.json. When given
--screens (the hub-ui-screens artifact from the QEMU run), the LVGL captures
are published as a ui/ gallery page next to the flasher.

Usage: make_manifests.py --build-dir BUILD_DIR --version V [--web DIR] [--out DIR] [--screens DIR]

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
import hashlib
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


def ui_gallery_html(version: str, shots: list[Path]) -> str:
    """Standalone gallery page for the QEMU-captured LVGL hub UI — same
    warm-paper theme as web/index.html, dark figure wells so the 800×480
    panel screenshots blend into their frames."""
    figures = []
    for shot in shots:
        name = shot.stem
        label = name.replace("_", " ")
        figures.append(
            f'<figure><a href="{name}.png" target="_blank" rel="noopener">'
            f'<img src="{name}.png" alt="hub {label} screen" loading="lazy" '
            f'width="800" height="480"></a>'
            f'<figcaption><span>{label}</span>'
            f'<span class="mono">{name}.png</span></figcaption></figure>'
        )
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AIO Feeder — Hub UI Gallery</title>
<meta name="description" content="The AIO Feeder hub LVGL interface, captured by CI from the emulated hub and composed into the 800×480 panel image the hardware displays.">
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns=%27http://www.w3.org/2000/svg%27 viewBox=%270 0 96 96%27%3E%3Crect width=%2796%27 height=%2796%27 rx=%2718%27 fill=%27%2326251e%27/%3E%3Cpath d=%27M22 22h52L56 48v14l-16 8V48z%27 fill=%27none%27 stroke=%27%23f54e00%27 stroke-width=%276%27 stroke-linejoin=%27round%27/%3E%3C/svg%3E">
<style>
:root{{--bg:#f2f1ed;--surface:#f7f7f4;--surface-4:#e6e5e0;--fg:#26251e;--muted:#5f5d52;
--border:oklab(0.263 -0.0023 0.0125 / .12);--accent:#f54e00;--accent-ink:#b93a00;
--well:#15161a;--code-fg:#f2f1ed;
--font-display:"Helvetica Neue",system-ui,-apple-system,"Segoe UI",sans-serif;
--font-body:"Iowan Old Style","Palatino Linotype","URW Palladio L",ui-serif,Georgia,Cambria,serif;
--font-ui:system-ui,-apple-system,"Segoe UI","Helvetica Neue",sans-serif;
--font-mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
--wrap:1180px}}
*{{box-sizing:border-box}}
body{{margin:0;background:var(--bg);color:var(--fg);font:400 17px/1.55 var(--font-body)}}
h1{{margin:0;font:400 clamp(34px,4.6vw,56px)/1.08 var(--font-display);letter-spacing:-.03em}}
p{{margin:0}}
a{{color:inherit}}
.mono{{font-family:var(--font-mono);font-size:.86em}}
:focus-visible{{outline:2px solid var(--accent);outline-offset:3px;border-radius:4px}}
.wrap{{max-width:var(--wrap);margin:0 auto;padding:0 28px}}
.eyebrow{{font:500 12px/1 var(--font-mono);letter-spacing:.06em;text-transform:uppercase;color:var(--muted)}}
.nav{{position:sticky;top:0;z-index:20;background:color-mix(in srgb,var(--bg) 88%,transparent);backdrop-filter:blur(10px);border-bottom:1px solid var(--border)}}
.nav .wrap{{display:flex;align-items:center;gap:16px;height:60px}}
.nav a.brand{{font:400 18px/1 var(--font-display);text-decoration:none}}
.nav .back{{margin-left:auto;font:500 14px/1 var(--font-ui);color:var(--accent-ink);text-decoration:none;padding:10px 14px;border-radius:8px}}
.nav .back:hover{{background:var(--surface-4)}}
header.page{{padding:56px 0 40px}}
h1{{margin:16px 0 18px}}
.lede{{font-size:19px;color:var(--muted);max-width:62ch;text-wrap:pretty}}
.stamp{{display:inline-flex;align-items:center;gap:8px;margin-top:20px;padding:8px 12px;border-radius:999px;background:var(--surface);border:1px solid var(--border);font:500 13px/1 var(--font-ui)}}
.grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(min(360px,100%),1fr));gap:28px;padding:8px 0 72px}}
figure{{margin:0;background:var(--well);border-radius:14px;overflow:hidden;box-shadow:0 18px 44px rgba(0,0,0,.16)}}
figure img{{display:block;width:100%;height:auto;aspect-ratio:5/3}}
figcaption{{display:flex;align-items:baseline;justify-content:space-between;gap:12px;padding:12px 16px;background:var(--surface);border-top:1px solid var(--border)}}
figcaption span:first-child{{font:500 15px/1.2 var(--font-ui);text-transform:capitalize}}
figcaption .mono{{color:var(--muted)}}
footer{{border-top:1px solid var(--border);padding:28px 0;color:var(--muted);font:400 14px/1.5 var(--font-ui)}}
footer a{{color:var(--accent-ink)}}
</style>
</head>
<body>
<div class="nav"><div class="wrap">
<a class="brand" href="../">AIO Feeder</a>
<a class="back" href="../">← Web flasher</a>
</div></div>

<main>
<header class="page"><div class="wrap">
<p class="eyebrow">QEMU · LVGL · CI captures</p>
<h1>Hub UI gallery</h1>
<p class="lede">The hub's LVGL dashboard, captured by CI from the emulated hub (Espressif QEMU fork). The firmware renders at 352×240 logical; each frame is composed into the 800×480 ST7262 panel image — 2× nearest-neighbor into the centered 704×480 window, black side bars — exactly what the hardware displays.</p>
<p class="stamp"><span class="mono">build {version}</span><span>·</span><span>{len(shots)} screens</span></p>
</div></header>

<div class="wrap grid">
{chr(10).join(figures)}
</div>
</main>

<footer><div class="wrap">
Captured headlessly from <span class="mono">aio-feeder-hub</span> in QEMU — see <a href="https://github.com/mlger/aio-feeder/tree/main/tools/qemu" target="_blank" rel="noopener">tools/qemu</a>. <a href="../">Back to the web flasher</a>.
</div></footer>
</body>
</html>
"""


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
    ap.add_argument("--screens", type=Path, default=None,
                    help="dir of QEMU-captured hub UI PNGs (hub-ui-screens "
                         "artifact); published as the ui/ gallery")
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

    # M8b: publish the QEMU-captured LVGL UI as the ui/ gallery page
    if args.screens:
        if not args.screens.is_dir():
            print(f"error: screens dir not found: {args.screens}", file=sys.stderr)
            return 1
        shots = sorted(args.screens.glob("*.png"))
        if not shots:
            print(f"error: no .png screenshots in {args.screens}", file=sys.stderr)
            return 1
        ui_dir = args.out / "ui"
        ui_dir.mkdir()
        for shot in shots:
            shutil.copy2(shot, ui_dir / shot.name)
        (ui_dir / "index.html").write_text(ui_gallery_html(args.version, shots))
        print(f"ui gallery: {ui_dir / 'index.html'} ({len(shots)} screens)")

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
        # M8a: OTA descriptor next to the image — the hub (and later the
        # nodes) fetch this, then the .bin, and pin the download to the
        # digest so a truncated/proxied fetch fails before it flashes.
        firmware = env_dir / "firmware.bin"
        if firmware.is_file():
            digest = hashlib.sha256(firmware.read_bytes()).hexdigest()
            (out_dir / "ota.json").write_text(
                json.dumps(
                    {
                        "version": args.version,
                        "file": "firmware.bin",
                        "sha256": digest,
                        "size": firmware.stat().st_size,
                    },
                    indent=2,
                )
                + "\n"
            )
        print(f"{env}: {out_dir / 'manifest.json'} ({len(manifest['builds'][0]['parts'])} parts)")

    (args.out / "build-info.json").write_text(json.dumps({"version": args.version}) + "\n")
    print(f"site ready: {args.out} (version {args.version})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
