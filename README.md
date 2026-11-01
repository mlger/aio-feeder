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

M3 — Hub core (PLAN.md §12) **on pure ESP-IDF** (decision D6 decided: hub =
ESP-IDF v5.4 + QEMU, nodes stay Arduino/PlatformIO). On top of M1 (bench node)
and M2 (ESP-NOW link), the hub runs the real core: schedule engine with
fire-once occurrences across reboots (§12 M3 exit criterion), §9.4 missed-feed
policy (skip / catch-up once within window, never stack), §9.9 skip/hold gates,
DST-correct local time (UTC/Manila/EU/US rules), feed_manager as the single
FEED originator (§7.2 FSM, §6.6 WAL intent → FEED_UNKNOWN on hub death
mid-feed, §9.5 stagger), CRC'd NVS persistence, PCF85063 RTC fallback + SNTP,
TIME_SYNC push on drift, and the bench CLI as an `esp_console` REPL.
Host-tested in CI (97 tests incl. a UDP e2e sim: hub core ↔ fake node over
real loopback sockets, retry/repeat policy exercised); QEMU boot smoke in CI.
Hardware verification: `docs/bench-m1.md` (node), `docs/bench-m2.md` (radio),
`docs/bench-m3.md` (scheduler + reboots), `docs/bench-sim.md` (fake node).
M4a hub UI screens (LVGL 9 on ESP-IDF, warm dark theme, offscreen RGB565
framebuffer, host-tested `ui_model` presenters §7.2/§8): **dashboard** (node
cards, clock, alert footer), **node detail** (state/vin/last-seen, next feed,
skip/hold gate banner, sorted schedule, recent feeds), **schedule editor**
(entry list + weekday chips + time/amount draft, pure edit model),
**history** (7-day bars, per-node totals, recent events) and **alerts**
(§8.7 active + acknowledged: offline/stale condition rows, failed/rejected/
unconfirmed/missed feed events, host-tested ack keys) are in, with
in-firmware sim nodes (two real LinkEngine instances over loopback + one
silent node) and a seeded week of history driving them under QEMU;
`tools/qemu/capture.py` dumps the framebuffer per `AIO_SHOT` marker in the
CI `qemu-hub` job (screenshots uploaded as artifacts). All five M4a screens
pass the QEMU screenshot run. Next: M4b pairing.

Releases: **v0.1.0**, **v0.2.0**, **v0.3.0**. Cut the next one the same way —
`git tag vX.Y.Z && git push origin vX.Y.Z` — and CI attaches the firmware zips
to a GitHub Release automatically.
