# AIO Feeder

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
├─ .mcp.json            # MCP servers: context7 + LVGL docs
├─ .claude/skills/      # ponytail skills (vendored)
├─ docs/                # setup guide, prototype, third-party notices
├─ lib/proto/           # shared frame/profile/status definitions (C, host-testable)
├─ hub/                 # hub firmware — env hub_esp32s3
├─ node/                # node firmware — envs node_esp8266, node_esp32c3
│  ├─ hal_esp8266/  hal_esp32c3/
├─ node_core/           # portable node logic: actuator FSM, limits, journal (host-tested)
├─ test/                # host unit tests (Unity, native)
├─ tools/               # flasher manifest builder, QEMU scripts, simulators
├─ hardware/            # schematics, BOM, enclosure CAD
└─ web/                 # GitHub Pages site (web flasher, ESP Web Tools)
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

## Deployment (GitHub Pages)

`.github/workflows/ci.yml` on `main`:

1. runs the host unit tests,
2. builds all three firmware targets,
3. assembles the web flasher site with the built binaries
   (`tools/flasher/make_manifests.py` writes one ESP Web Tools manifest per
   target) and deploys it to GitHub Pages.

Enable once: repo **Settings → Pages → Source: GitHub Actions** (CI fails with
a clear error until this is on), or `gh api -X POST repos/mlger/aio-feeder/pages -f build_type=workflow`.

## Working with AI agents

- `AGENTS.md` pins every agent to the ponytail ruleset (smallest safe change)
  plus project notes; the six ponytail skills are vendored under
  `.claude/skills/` for Claude Code. Prefer the plugin install if you use
  Claude Code: `/plugin marketplace add DietrichGebert/ponytail` then
  `/plugin install ponytail@ponytail`.
- `.mcp.json` gives agents **context7** (up-to-date library docs) and the
  **LVGL docs** server the plan mandates (PLAN.md §7.1, D26).

## Status

M1 — Bench node (PLAN.md §12). HAL v1 on both node MCUs, hard relay-off
timer, watchdog, DS3231, A/B config slots, feed journal, actuator FSM and the
serial CLI are in (host-tested); hub firmware is still a boot stub. Bench
checklist: `docs/bench-m1.md`. Next: M2 link (ESP-NOW).
