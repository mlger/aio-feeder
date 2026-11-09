#!/usr/bin/env python3
"""Add GitHub-release firmware versions to the Pages flasher site.

Run AFTER make_manifests.py has assembled the static root (web/, ui/
gallery, main-CI firmware at hub/ node-esp32c3/ node-esp8266/). For each
recent GitHub release that carries the firmware zips it:

  1. downloads aio-feeder-<tag>-{hub_esp32s3,node_esp32c3,node_esp8266}.zip
  2. extracts the binaries into <out>/fw/<tag>/<target>/, writes an ESP
     Web Tools manifest (version = tag) and the zip's ota.json
  3. moves the main-CI build from the site root into <out>/fw/main/
  4. promotes the NEWEST release into the site root — the hub's default
     OTA URL (ota_core.h OTA_URL_DEF = .../hub/firmware.bin) keeps working
     and always serves the latest release, and the picker's default is
     the root
  5. writes <out>/releases.json for the flasher page's version picker

With no usable releases the root build is left untouched and releases.json
degrades to a single "latest build" entry, so the page still works on a
fresh repo.

Usage:
  release_site.py --out site --repo OWNER/REPO [--token T] [--keep 8]
                  [--main-version V]
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import shutil
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from make_manifests import TARGETS  # noqa: E402  (site dirs + part layout)

UA = {"User-Agent": "aio-feeder-pages", "Accept": "application/vnd.github+json"}
API = "https://api.github.com"


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Manual redirect handling so the auth header never leaks to the
    signed-URL CDN host on release-asset downloads."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


_OPENER = urllib.request.build_opener(_NoRedirect)


def _get(url: str, token: str | None, timeout: int = 60) -> bytes:
    headers = dict(UA)
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(url, headers=headers)
    try:
        with _OPENER.open(req, timeout=timeout) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        loc = e.headers.get("Location") if e.headers else None
        if e.status in (301, 302, 303, 307, 308) and loc:
            # follow cross-host redirects WITHOUT the auth header
            with _OPENER.open(urllib.request.Request(loc, headers=dict(UA)),
                              timeout=timeout) as r:
                return r.read()
        raise


def fetch_releases(repo: str, token: str | None, keep: int) -> list[dict]:
    raw = json.loads(_get(f"{API}/repos/{repo}/releases?per_page=100", token))
    out = []
    for rel in raw:
        if rel.get("draft"):
            continue
        assets = {a["name"]: a["browser_download_url"]
                  for a in rel.get("assets", []) if a.get("state") == "uploaded"}
        tag = rel.get("tag_name", "")
        needed = [f"aio-feeder-{tag}-{env}.zip" for env in TARGETS]
        if not tag or not all(n in assets for n in needed):
            missing = [n for n in needed if n not in assets]
            print(f"release {tag or '(no tag)'}: skipped (missing {', '.join(missing)})")
            continue
        out.append({
            "tag": tag,
            "name": rel.get("name") or tag,
            "prerelease": bool(rel.get("prerelease")),
            "published_at": rel.get("published_at", ""),
            "assets": {n: assets[n] for n in needed},
        })
        if len(out) >= keep:
            break
    return out


def stage_release(out: Path, rel: dict, token: str | None) -> bool:
    """Download + extract one release into out/fw/<tag>/<target>/."""
    tag = rel["tag"]
    target_root = out / "fw" / tag
    staged_any = False
    for env, (site_dir, _family, parts, required) in TARGETS.items():
        zbytes = _get(rel["assets"][f"aio-feeder-{tag}-{env}.zip"], token)
        dest = target_root / site_dir
        dest.mkdir(parents=True, exist_ok=True)
        got: dict[str, bytes] = {}
        ota_json = None
        with zipfile.ZipFile(io.BytesIO(zbytes)) as zf:
            names = set(zf.namelist())
            for name, _off in parts:
                if name in names:
                    got[name] = zf.read(name)
            if "ota.json" in names:
                ota_json = zf.read("ota.json")
        missing = [n for n in required if n not in got]
        if missing:
            print(f"release {tag}/{env}: skipped (zip lacks {', '.join(missing)})")
            return False
        for name, blob in got.items():
            (dest / name).write_bytes(blob)
        # manifest: version = tag, parts = what the zip actually carried
        manifest = {
            "name": None, "version": tag, "new_install_prompt_erase": True,
            "builds": [{"parts": [{"path": n, "offset": o}
                                  for n, o in parts if n in got]}],
        }
        # name/chipFamily come from the shared table (no drift)
        from make_manifests import TITLES, TARGETS as _T
        manifest["name"] = TITLES[env]
        manifest["builds"][0]["chipFamily"] = _T[env][1]
        (dest / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        # ota.json: keep the zip's own descriptor unless it disagrees with
        # the firmware.bin we just extracted (then regenerate)
        if ota_json:
            try:
                d = json.loads(ota_json)
                fw = got.get("firmware.bin")
                ok = (d.get("version") == tag and fw is not None
                      and d.get("sha256") == hashlib.sha256(fw).hexdigest())
                if ok:
                    (dest / "ota.json").write_bytes(ota_json)
                    ota_json = None
            except ValueError:
                pass
        if ota_json is not None or "ota.json" not in names:
            fw = got.get("firmware.bin")
            if fw is not None:
                (dest / "ota.json").write_text(json.dumps({
                    "version": tag,
                    "file": "firmware.bin",
                    "sha256": hashlib.sha256(fw).hexdigest(),
                    "size": len(fw),
                }, indent=2) + "\n")
        staged_any = True
    print(f"release {tag}: staged {len(TARGETS)} targets -> fw/{tag}/")
    return staged_any


def move_root_build_to_fw_main(out: Path) -> bool:
    """Relocate the main-CI build (root hub/, node-*/) into fw/main/."""
    moved = False
    for env, (site_dir, _f, parts, _req) in TARGETS.items():
        src = out / site_dir
        if not (src / "firmware.bin").is_file():
            continue
        dest = out / "fw" / "main" / site_dir
        dest.mkdir(parents=True, exist_ok=True)
        for name, _off in parts:
            f = src / name
            if f.is_file():
                shutil.move(str(f), dest / name)
        for name in ("manifest.json", "ota.json"):
            f = src / name
            if f.is_file():
                shutil.move(str(f), dest / name)
        moved = True
    if moved:
        print("main CI build moved -> fw/main/")
    return moved


def promote(out: Path, tag: str) -> None:
    """Copy the newest release into the site root (hub/, node-*/)."""
    for env, (site_dir, _f, parts, _req) in TARGETS.items():
        src = out / "fw" / tag / site_dir
        dest = out / site_dir
        dest.mkdir(parents=True, exist_ok=True)
        for name, _off in parts:
            f = src / name
            if f.is_file():
                shutil.copy2(f, dest / name)
        for name in ("manifest.json", "ota.json"):
            f = src / name
            if f.is_file():
                shutil.copy2(f, dest / name)
    print(f"root promoted to release {tag}")


def write_releases_json(out: Path, rels: list[dict], main_version: str | None,
                        main_moved: bool) -> None:
    data = {
        "latest": rels[0]["tag"] if rels else None,
        "main": {
            "dir": "fw/main" if main_moved else "",
            "version": main_version,
        },
        "releases": [
            {
                "tag": r["tag"],
                "name": r["name"],
                "dir": "" if i == 0 else f"fw/{r['tag']}",
                "prerelease": r["prerelease"],
                "published_at": r["published_at"],
            }
            for i, r in enumerate(rels)
        ],
    }
    (out / "releases.json").write_text(json.dumps(data, indent=2) + "\n")
    print(f"releases.json: latest={data['latest']} releases={len(rels)} "
          f"main={data['main']['dir'] or '(root)'}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True, help="assembled site dir")
    ap.add_argument("--repo", required=True, help="OWNER/REPO")
    ap.add_argument("--token", default=None,
                    help="GitHub token (private repos / API rate limits)")
    ap.add_argument("--keep", type=int, default=8,
                    help="how many recent releases to publish (default 8)")
    ap.add_argument("--main-version", default=None,
                    help="version string of the root CI build (git describe)")
    args = ap.parse_args()

    if not args.out.is_dir():
        print(f"error: site dir not found: {args.out} (run make_manifests.py first)",
              file=sys.stderr)
        return 1

    try:
        rels = fetch_releases(args.repo, args.token, args.keep)
    except (urllib.error.URLError, OSError, ValueError) as e:
        # degrade gracefully: keep the root build, empty picker entries
        print(f"warning: releases fetch failed ({e}); publishing root build only",
              file=sys.stderr)
        rels = []

    staged: list[dict] = []
    for rel in rels:
        try:
            if stage_release(args.out, rel, args.token):
                staged.append(rel)
        except (urllib.error.URLError, OSError, zipfile.BadZipFile, KeyError) as e:
            print(f"release {rel['tag']}: stage failed ({e}); skipped", file=sys.stderr)

    main_moved = False
    if staged:
        main_moved = move_root_build_to_fw_main(args.out)
        promote(args.out, staged[0]["tag"])
    write_releases_json(args.out, staged, args.main_version, main_moved)
    return 0


if __name__ == "__main__":
    sys.exit(main())
