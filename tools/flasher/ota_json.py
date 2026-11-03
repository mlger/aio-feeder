#!/usr/bin/env python3
"""Write an ota.json descriptor beside a firmware image (M8a, D37).

The hub (and later the nodes) fetch this descriptor, then the .bin, and
pin the download to the digest so a truncated/proxied fetch fails before
it flashes. Release zips carry one per target; the Pages site gets the
same file from make_manifests.py.

Usage: ota_json.py --dir DIR --version V [--file firmware.bin]
"""
import argparse
import hashlib
import json
import pathlib
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dir", type=pathlib.Path, required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--file", default="firmware.bin")
    args = ap.parse_args()

    fw = args.dir / args.file
    if not fw.is_file():
        print(f"error: {fw} not found", file=sys.stderr)
        return 1
    (args.dir / "ota.json").write_text(
        json.dumps(
            {
                "version": args.version,
                "file": args.file,
                "sha256": hashlib.sha256(fw.read_bytes()).hexdigest(),
                "size": fw.stat().st_size,
            },
            indent=2,
        )
        + "\n"
    )
    print(f"ota.json: {args.dir} ({args.file})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
