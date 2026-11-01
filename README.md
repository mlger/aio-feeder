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
├─ hub/                 # hub firmware — env hub_esp32s3
├─ node/                # node firmware — envs node_esp8266, node_esp32c3
├─ node_core/           # portable node logic: actuator FSM, limits, journal (host-tested)
├─ test/                # host unit tests (Unity, native)
├─ tools/flasher/       # ESP Web Tools manifest builder used by CI
├─ tools/qemu/          # QEMU scripts (placeholder, pending decision D6)
├─ hardware/            # schematics, BOM, enclosure CAD
├─ web/                 # GitHub Pages site (web flasher, ESP Web Tools)
└─ docs/                # setup guide, M1/M2 bench logs, prototype, third-party notices
```

## Quick start

```bash
pip install platformio          # or: brew install platformio

pio run -d hub -e hub_esp32s3   # build hub firmware
pio run -d node -e node_esp32c3 # build C3 node (or -e node_esp8266)
pio test -d test                # host unit tests
```

Full environment setup (ESP-IDF, QEMU emulation, MCP servers, agent tooling):
[`docs/SETUP.md`](docs/SETUP.md).

## Automation

| Workflow | Trigger | What it does |
|---|---|---|
| [`ci.yml`](.github/workflows/ci.yml) | push to `main`, every PR, manual | Host unit tests + all three firmware builds (shared [`build.yml`](.github/workflows/build.yml)); on `main` it also deploys the web flasher to GitHub Pages |
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

M2 — Link (PLAN.md §12). On top of the M1 bench node (HAL v1 on both node
MCUs, hard relay-off timer, watchdog, DS3231, A/B config slots, feed journal,
actuator FSM, serial CLI — host-tested), the ESP-NOW link now exists on hub
and both node MCUs: the frame MAC (truncated HMAC-SHA256), per-peer seq with
replay window, FEED/ACK/RESULT with the §6.6 retry policy, heartbeat +
STATUS_REQ ack, channel scan (§6.2), schedule push with `skip_until` /
`hold_until`, and a bench hub CLI (`feed`, `feedloop`, `peer`, `chan`, …).
Host-tested in CI (39 tests incl. RFC 4231 vectors); all three firmware
targets build. Hardware verification: `docs/bench-m1.md` (node) and
`docs/bench-m2.md` (radio + 1000-feed loopback). Next: M3 hub core.

Releases: none tagged yet — cut the first one with `git tag v0.1.0 && git
push origin v0.1.0`.
