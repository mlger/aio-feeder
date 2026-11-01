# Environment & tooling setup

Everything needed to build, test, flash and work on AIO Feeder. Status against
the M0 exit criteria (PLAN.md §12) is at the bottom.

## 1. Firmware toolchain (PlatformIO)

```bash
pip install platformio        # pio CLI; VS Code users: PlatformIO IDE extension
pio run -d hub -e hub_esp32s3
pio run -d node -e node_esp8266
pio run -d node -e node_esp32c3
pio test -d test              # host unit tests (Unity, native)
pio device monitor -d hub     # serial console, 115200
```

Environments live in `hub/platformio.ini` and `node/platformio.ini`. The hub
env is an Arduino stub; if D6 lands on pure ESP-IDF, the LVGL/esp_lcd work
moves into an ESP-IDF project and QEMU (§3 below) comes with it.

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
| `context7` | `https://mcp.context7.com/mcp` | Up-to-date docs for PlatformIO, ESP-IDF, Arduino cores, LVGL, etc. (https://github.com/upstash/context7) |
| `lvgl` | `https://lvgl.mcp.kapa.ai/` | Official LVGL docs MCP — mandated by PLAN.md §7.1 / D26 |

Claude Code picks up project-scope servers from the repo-root `.mcp.json`
automatically (approve them when prompted). The plan sketched
`.claude/.mcp.json`; the root `.mcp.json` is the path Claude Code actually
reads, so that is what ships. Other clients:

```bash
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
| PlatformIO envs (hub_esp32s3, node_esp8266, node_esp32c3) | ✅ (boot stubs) |
| Shared proto lib | ✅ `lib/proto/proto.h` |
| Host unit tests in CI | ✅ `test/` (Unity, native) |
| CI builds all targets | ✅ `.github/workflows/ci.yml` |
| Web flasher deployed to GitHub Pages | ✅ CI `pages` job (needs one-time Pages enable) |
| QEMU job boots hub + C3 images in CI | ⏳ deferred — needs ESP-IDF hub build (D6) |
| LVGL docs MCP config | ✅ `.mcp.json` |
