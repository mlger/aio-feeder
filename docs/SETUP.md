# Environment & tooling setup

Everything needed to build, test, flash and work on AIO Feeder. Status against
the M0 exit criteria (PLAN.md §12) is at the bottom.

## 1. Firmware toolchains

```bash
# Hub — ESP-IDF v5.4 (decision D6: pure ESP-IDF project in hub/)
# install esp-idf per https://docs.espressif.com/projects/esp-idf (or use the
# espressif/idf:v5.4 container, which is what CI does)
. $IDF_PATH/export.sh
idf.py -C hub set-target esp32s3
idf.py -C hub build
idf.py -C hub -p /dev/ttyUSB0 flash monitor   # bench CLI is an esp_console REPL
idf.py -C hub qemu monitor                    # boot the hub in QEMU (needs idf.py qemu)

# Nodes — PlatformIO/Arduino
pip install platformio        # pio CLI; VS Code users: PlatformIO IDE extension
pio run -d node -e node_esp8266
pio run -d node -e node_esp32c3
pio test -d test              # host unit tests (Unity, native, incl. UDP e2e sim)

# Host tests where the PlatformIO CDN is unreachable:
tools/test-local/run.sh       # same sources, g++ + vendored Unity shim
```

Node environments live in `node/platformio.ini`; the hub is an ESP-IDF
CMake project (`hub/CMakeLists.txt`, sources in `hub/main/`, portable core
in `hub/src/`). Wi-Fi credentials for NTP go through `idf.py menuconfig` →
"AIO Feeder hub" (they are build-time Kconfig, never committed).

## 2. AI agent tooling

### Ponytail (lazy senior dev mode)

Vendored in this repo so every agent gets it with zero setup:

- `AGENTS.md` — the ruleset + project notes (Cursor, Codex, Kimi, Qoder and
  most agents read this automatically).
- `.claude/skills/ponytail*/` — the six skills for Claude Code projects.
- Claude Code users can instead install the plugin (tracks upstream updates):
  `/plugin marketplace add DietrichGebert/ponytail` → `/plugin install ponytail@ponytail`.
- Source: https://github.com/DietrichGebert/ponytail (MIT, see
  `docs/THIRD_PARTY_PONYTAIL.md`).

### MCP servers (`.mcp.json`, project scope)

| Server | URL | Purpose |
|---|---|---|
| `espressif-documentation` | `https://mcp.espressif.com/docs` | Semantic search over Espressif docs — ESP-IDF, Arduino-ESP32, ESP-AT (`search_espressif_sources`). OAuth on first connect; 40 req/h rate limit |
| `espressif-components` | `https://components.espressif.com/mcp` | ESP Component Registry — search managed components, usage docs (`idf.py add-dependency` targets) |
| `espressif-engineering` | `https://ts-mcp.espressif.com/mcp` | Espressif troubleshooting workflows — environment setup, Wi-Fi debugging, schematic/PCB review. OAuth on first connect |
| `esp-pilot` | `https://mcp.esp-pilot.espressif.com/mcp` | Espressif ESP-Pilot — agentic embedded copilot for ESP-IDF/Arduino-ESP32 projects (project scaffolding, coding and debugging assistance) |
| `context7` | `https://mcp.context7.com/mcp` | Up-to-date docs for PlatformIO, LVGL, Arduino cores, etc. (https://github.com/upstash/context7) |
| `lvgl` | `https://lvgl.mcp.kapa.ai/` | Official LVGL docs MCP — mandated by PLAN.md §7.1 / D26 |

Claude Code picks up project-scope servers from the repo-root `.mcp.json`
automatically (approve them when prompted; OAuth servers open a browser flow
on first use). The plan sketched `.claude/.mcp.json`; the root `.mcp.json` is
the path Claude Code actually reads, so that is what ships. Other clients:

```bash
claude mcp add --transport http espressif-documentation https://mcp.espressif.com/docs
claude mcp add --transport http espressif-components https://components.espressif.com/mcp
claude mcp add --transport http espressif-engineering https://ts-mcp.espressif.com/mcp
claude mcp add --transport http esp-pilot https://mcp.esp-pilot.espressif.com/mcp
claude mcp add --transport http context7 https://mcp.context7.com/mcp
claude mcp add --transport http lvgl https://lvgl.mcp.kapa.ai/
```

Context7 works keyless (rate-limited); set `CONTEXT7_API_KEY` in your
environment for higher limits — never commit it.

## 3. Emulation (QEMU) — optional, hub UI work without hardware

Needs ESP-IDF ≥ 5.3.1 (S3 support for `idf.py qemu`):

```bash
idf_tools.py install qemu-xtensa qemu-riscv32
idf.py qemu --graphics monitor    # hub UI in the emulated RGB framebuffer
```

Not emulated: Wi-Fi/ESP-NOW, BT, USB, I²C, touch, CH422G, RTC, SD (§7.7).
CI QEMU boot smoke tests (M0 criterion) are deferred until the hub build
targets ESP-IDF; see `tools/qemu/README.md`.

## 4. Web flasher + GitHub Pages

The flasher is a static site (`web/`) using **ESP Web Tools** (Web Serial —
Chrome/Edge/desktop only). CI builds the firmware, drops the `.bin`s into
per-target folders and writes an ESP Web Tools `manifest.json` for each
(`tools/flasher/make_manifests.py`).

One-time enable (repo owner):

- **UI:** Settings → Pages → Build and deployment → Source: **GitHub Actions**.
- **API:** `gh api -X POST repos/mlger/aio-feeder/pages -f build_type=workflow`

Notes:
- The repo is private; GitHub Pages for private repos requires a paid plan
  (Pro/Team). If Pages is unavailable, make the repo public or upgrade.
- Firmware artifacts for offline flashing also come out of every CI run
  (Actions → CI → `firmware-*` artifacts).

## 5. M0 checklist (PLAN.md §12)

| Criterion | Status |
|---|---|
| Repo + plan + prototype checked in | ✅ |
| PlatformIO envs (node_esp8266, node_esp32c3) + ESP-IDF hub project | ✅ (hub = ESP-IDF v5.4 since D6 was decided; node = M2 link firmware) |
| Shared proto lib | ✅ `lib/proto/proto.h` + messages/sha256/pairing |
| Host unit tests in CI | ✅ `test/` (Unity, native): proto + node_core + link_core + hub_core + UDP e2e sim (68 tests) |
| CI builds all targets | ✅ `.github/workflows/ci.yml` (hub via espressif/idf:v5.4 container) |
| Web flasher deployed to GitHub Pages | ✅ live: https://mlger.github.io/aio-feeder/ |
| QEMU job boots hub image in CI | ✅ `ci.yml` `qemu-hub` — merges flash image, boots `qemu-system-xtensa -M esp32s3`, checks boot markers + no panic (D6 decided) |
| LVGL docs MCP config | ✅ `.mcp.json` (LVGL + context7 + 4 Espressif servers incl. esp-pilot) |
| M1 bench node firmware (hard timer, serial CLI, HAL v1) | ✅ code; hardware verification per `docs/bench-m1.md` |
| M2 link (ESP-NOW, HMAC frames, retries, channel scan, skip/hold) | ✅ code, all 3 targets build; hardware verification per `docs/bench-m2.md` |
