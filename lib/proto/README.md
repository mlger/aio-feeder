# lib/proto — shared protocol definitions

One source of truth for the hub ↔ node wire format (PLAN.md §6.5, §6.6): the
frame struct, message types, result codes and heartbeat flag bits. Plain C so
it compiles on the hub (ESP32-S3), both node MCUs (ESP8266, ESP32-C3) **and**
the host test target.

Usage in the PlatformIO projects (`hub/`, `node/`, `test/`): the `platformio.ini`
adds `-I../lib/proto` to `build_flags`. Keep everything header-only here until
it earns a `.c` file (e.g. the HMAC wrapper), then add a `library.json`.

Tests: `test/test_proto/` runs on the `native` env in CI (`pio test -d test`).
