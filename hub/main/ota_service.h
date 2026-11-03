/*
 * AIO Feeder — hub OTA service (PLAN.md §7.6, §12 M8a).
 *
 * ESP-IDF glue over esp_ota_ops + esp_http_client: the download path for
 * real updates, the boot-side PENDING_VERIFY health confirm, and the CI
 * QEMU selftest boot walk (CONFIG_AIO_OTA_SELFTEST + tools/qemu/badapp).
 * Pure pieces live in hub/src/ota_core.h (host-tested).
 *
 * Threading: ota_tick() runs on the main loop task under g_state_mu;
 * phase/pct are atomics so the httpd thread can read a brief without
 * extra locks. The download task only touches flash + its own atomics —
 * the loop keeps feeding (§3.3: an update must never wedge the hub).
 */
#ifndef AIO_OTA_SERVICE_H
#define AIO_OTA_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "../src/hub_store.h"
#include "../src/ota_core.h"

namespace aio {
namespace ota {

/* One-time: bind the store + time source, read the running slot/version,
 * run the boot-side checks (selftest stage, PENDING_VERIFY detection). */
void begin(IHubStore *store, int64_t (*unix_fn)(void));

/* Main-loop tick (under g_state_mu): fires the health confirm when the
 * window elapses while PENDING_VERIFY. */
void tick(uint32_t now_ms);

/* Start an update from `url` (validated; esp_http_client follows
 * redirects). false = rejected (busy, bad URL, no update slot). */
bool start(const char *url);

/* Optional SHA-256 pin (64 hex chars) checked after the download. */
bool set_expected_sha(const char *hex64);

bool busy(void);
void brief(OtaBrief *out);

/* Persisted URL (web/CLI; the pane shows it, the web sets it). */
bool url_save(const char *url);
void url_get(char *out, size_t cap);

/* Reboot into the staged slot (phase NEED_REBOOT), or force the factory
 * image on the next boot (escape hatch — works from any phase). */
bool reboot_staged(void);
bool boot_factory(void);

/* Stats + last terminal status for the pane/CLI. */
bool last_status(OtaStatus *out);

}  // namespace ota
}  // namespace aio

#endif /* AIO_OTA_SERVICE_H */
