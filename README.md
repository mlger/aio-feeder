# AIO Feeder

[![CI](https://github.com/mlger/aio-feeder/actions/workflows/ci.yml/badge.svg)](https://github.com/mlger/aio-feeder/actions/workflows/ci.yml)
[![Release](https://github.com/mlger/aio-feeder/actions/workflows/release.yml/badge.svg)](https://github.com/mlger/aio-feeder/actions/workflows/release.yml)
[![Web flasher](https://img.shields.io/badge/web--flasher-mlger.github.io%2Faio--feeder-2dd4bf)](https://mlger.github.io/aio-feeder/)

All-in-One (AIO) Feeder — a multi-node automated feeding system. One
touchscreen hub (Waveshare ESP32-S3-Touch-LCD-4.3B) owns schedules,
calibration, logging and alerts; cheap identical feeder nodes (ESP8266 or
ESP32-C3 SuperMini + DS3231 + relay + 12 V solenoid) dispense on command and
survive hub loss with their own fallback schedule.

- **Spec:** [`PLAN.md`](PLAN.md) — the engineering plan (architecture, safety
  requirements, protocol, milestones). Start there.
- **Hub display prototype:** [`docs/prototype/index.html`](docs/prototype/index.html)
  — open it in a browser to click through the UI design.
- **Web flasher:** https://mlger.github.io/aio-feeder/ — flash the hub and
  node firmware from Chrome/Edge over USB (ESP Web Tools). Deployed by CI.

## Repository layout (PLAN.md §13)

```
aio-feeder/
├─ PLAN.md              # engineering plan (v0.7)
├─ AGENTS.md            # ponytail rules + project notes for AI agents
├─ .mcp.json            # MCP servers: context7, LVGL docs, 4 Espressif servers
├─ .claude/skills/      # ponytail skills (vendored)
├─ .github/workflows/   # CI, releases (semver tags), PR automation
├─ lib/proto/           # shared frame/profile/status definitions (C, host-testable)
├─ link_core/           # portable ESP-NOW link: HMAC frames, seq/replay, retries (host-tested)
├─ hub/                 # hub firmware — ESP-IDF v5.4 project (CMakeLists.txt, main/, src/)
├─ node/                # node firmware — envs node_esp8266, node_esp32c3
├─ node_core/           # portable node logic: actuator FSM, limits, journal (host-tested)
├─ test/                # host tests (Unity, native) incl. UDP e2e sim
├─ tools/flasher/       # ESP Web Tools manifest builder used by CI
├─ tools/sim/           # fake feeder node (host, UDP) + UDP link transport
├─ tools/test-local/    # g++ host-test runner for environments without PlatformIO
├─ tools/qemu/          # QEMU boot-smoke docs (job lives in ci.yml)
├─ hardware/            # schematics, BOM, enclosure CAD
├─ web/                 # GitHub Pages site (web flasher, ESP Web Tools)
└─ docs/                # setup guide, M1/M2 bench logs, prototype, third-party notices
```

## Quick start

```bash
# Hub — ESP-IDF v5.4 (decision D6: pure ESP-IDF)
git clone https://github.com/mlger/aio-feeder && cd aio-feeder
idf.py -C hub set-target esp32s3 && idf.py -C hub build
idf.py -C hub -p /dev/ttyUSB0 flash monitor    # bench CLI runs in the monitor

# Nodes — PlatformIO/Arduino
pip install platformio          # or: brew install platformio
pio run -d node -e node_esp32c3 # build C3 node (or -e node_esp8266)

# Host tests (Unity, native; runs the UDP e2e sim too)
pio test -d test
# or, where the PlatformIO CDN is unreachable:
tools/test-local/run.sh

# Fake feeder node for bench testing (speaks the M2/M3 proto over UDP)
g++ -std=c++17 -Ilib/proto -Ilink_core -Itools/sim tools/sim/fake_node.cpp -o fake_node
./fake_node --hub 127.0.0.1:9000   # then `peer add 02:00:00:00:00:02 1 <key>` on the hub
```

Full environment setup (ESP-IDF, QEMU emulation, MCP servers, agent tooling):
[`docs/SETUP.md`](docs/SETUP.md).

## Automation

| Workflow | Trigger | What it does |
|---|---|---|
| [`ci.yml`](.github/workflows/ci.yml) | push to `main`, every PR, manual | Host unit tests + UDP e2e sim; hub build with ESP-IDF v5.4 and both node builds (shared [`build.yml`](.github/workflows/build.yml)); **QEMU boot smoke** (hub image boots in `qemu-system-xtensa`, markers + no panic); on `main` also deploys the web flasher to GitHub Pages |
| [`release.yml`](.github/workflows/release.yml) | push of a `v*.*.*` tag | Runs the same tests/build, then attaches one firmware zip per target to a GitHub Release with auto-generated notes |
| [`pr.yml`](.github/workflows/pr.yml) | PR opened/edited | Conventional-commit title check; auto-labels the PR by changed paths (`.github/labeler.yml`) |

Pull requests use the [`.github/PULL_REQUEST_TEMPLATE.md`](.github/PULL_REQUEST_TEMPLATE.md)
checklist and get the CI suite automatically.

### Semantic versioning

Commits follow [Conventional Commits](https://www.conventionalcommits.org/):
`fix:` implies a **patch** bump, `feat:` a **minor** bump, and `feat!:` (or a
`BREAKING CHANGE:` footer) a **major** bump. Releases are cut by tagging — CI
does the rest:

```bash
git tag v0.1.0              # pick the bump the commits since the last tag imply
git push origin v0.1.0
```

The release workflow then:

1. runs the host tests and builds hub + both node firmwares,
2. zips the flashable binaries as `aio-feeder-<version>-<target>.zip`
   (identical content to what the [web flasher](https://mlger.github.io/aio-feeder/)
   serves),
3. creates a GitHub Release named after the tag, with notes generated from the
   commits/PRs since the previous tag.

The web flasher page shows the exact firmware version (`git describe`) it was
built from, so a tagged build displays its own release number.

## Working with AI agents

- `AGENTS.md` pins every agent to the ponytail ruleset (smallest safe change)
  plus project notes; the six ponytail skills are vendored under
  `.claude/skills/` for Claude Code. Prefer the plugin install if you use
  Claude Code: `/plugin marketplace add DietrichGebert/ponytail` then
  `/plugin install ponytail@ponytail`.
- `.mcp.json` gives agents **context7** (up-to-date library docs), the
  **LVGL docs** server (PLAN.md §7.1, D26) and four **Espressif** servers
  (documentation search, component registry, troubleshooting workflows,
  esp-pilot). Per-server notes and `claude mcp add` commands:
  [`docs/SETUP.md`](docs/SETUP.md).

## Status

**M5a — display + touch backend (v0.7.0)** on top of M4a/M4b: the Waveshare
ESP32-S3-Touch-LCD-4.3B hub now drives its ST7262 RGB panel (800×480, plain
RGB), CH422G expander and GT911 touch (D34) — LVGL at the logical 352×240,
×2-expanded in flush. All M4a/M4b screens carry tap targets (dashboard
cards, detail strips, settings rows + action strip, PIN pad, pairing /
wizard / calibration strips, back/home overlay) wired through the
host-tested nav model (back stack, §8 PIN gate) to engine-serviced request
flags; CI screenshots a tap-driven path end-to-end (`inject_tap` → real
indev → hit-test → request → engine). Display failure runs headless —
feeding never depends on the UI (§3.3). Remaining for M5 (M5b): numeric /
text entry on the touchscreen, mixed-type node polish (PLAN §12).

M4b — hub setup workflow (v0.6.0): pairing with confirmation code +
wrapped key (D29, §6.3), calibration wizard, settings (§8.8), PIN lock
(§8, D18/D33), backup/export/restore (D31, D32), setup wizard (§9.13) —
LVGL scenes + host-tested sessions + QEMU screenshots for every screen.
M4a — hub operate UI (v0.4.0): dashboard, node detail, schedule editor,
history (7-day chart), alerts.

M3 — hub core on pure ESP-IDF (D6): schedule engine with fire-once
occurrences across reboots, §9.4 missed-feed policy, §9.9 skip/hold gates,
DST-correct local time, feed_manager as the single FEED originator (§6.6
WAL), CRC'd NVS persistence, PCF85063 RTC fallback + SNTP, bench CLI as an
`esp_console` REPL. M1/M2: bench node + ESP-NOW link (1000-feed loopback).

Host-tested in CI (182 tests incl. the UDP e2e sim); QEMU boot smoke +
screenshot run in CI. Hardware verification: `docs/bench-m1.md` (node),
`docs/bench-m2.md` (radio), `docs/bench-m3.md` (scheduler + reboots),
`docs/bench-m4b.md` (pairing), `docs/bench-m5.md` (display/touch/SD).

