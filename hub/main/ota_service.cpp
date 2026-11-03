/*
 * AIO Feeder — hub OTA service implementation (PLAN.md §7.6, §12 M8a;
 * decision D37). See ota_service.h for the contract.
 *
 * Download flow (its own task, never blocks the loop):
 *   validate URL → esp_http_client GET (redirects on, cert bundle for
 *   https) → esp_ota_begin(next slot, size unknown) → stream 4 KB chunks
 *   → mbedtls sha256 + progress → esp_ota_end (image checksum) → optional
 *   pinned-sha compare → read the staged image's own version
 *   (esp_ota_get_partition_description) → esp_ota_set_boot_partition →
 *   NEED_REBOOT. The user reboots (pane/CLI/web) — the hub stays live
 *   until then; a power cut in this phase is harmless (the old slot is
 *   still the boot target until set_boot, and after it the bootloader's
 *   rollback covers a failed boot).
 *
 * Boot side: a PENDING_VERIFY running image confirms after
 * CONFIG_AIO_OTA_CONFIRM_MS of healthy loop ticks. CONFIG_AIO_OTA_SELFTEST
 * (QEMU only) walks stage/copy/confirm/bad-image/rollback across 5 boots
 * and prints the markers tools/qemu/ota_run.py asserts.
 */
#include "ota_service.h"

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

#include <mbedtls/sha256.h>

namespace aio {
namespace ota {

static IHubStore *s_store = nullptr;
static int64_t (*s_unix)(void) = nullptr;
static std::atomic<uint8_t> s_phase{OTA_IDLE};
static std::atomic<uint8_t> s_pct{0};
static std::atomic<uint8_t> s_err{OTA_ERR_NONE};
static char s_slot[10] = "factory";
static char s_ver[24] = "";
static char s_target[24] = "";
static uint8_t s_expect_sha[32];
static bool s_have_expect = false;
static uint32_t s_confirm_at = 0;

/* selftest NVS keys (framed blobs via the same store) */
constexpr char SELF_KEY[] = "otslf";
constexpr char BOOT_KEY[] = "otbc";

static void set_phase(uint8_t p) { s_phase.store(p, std::memory_order_release); }

void brief(OtaBrief *out) {
  if (!out) return;
  *out = OtaBrief{};
  out->phase = s_phase.load(std::memory_order_acquire);
  out->pct = s_pct.load(std::memory_order_acquire);
  out->err = s_err.load(std::memory_order_acquire);
  snprintf(out->slot, sizeof(out->slot), "%s", s_slot);
  snprintf(out->ver, sizeof(out->ver), "%s", s_ver);
  snprintf(out->target, sizeof(out->target), "%s", s_target);
}

bool busy(void) {
  const uint8_t p = s_phase.load(std::memory_order_acquire);
  return p == OTA_DOWNLOAD || p == OTA_VERIFY;
}

bool last_status(OtaStatus *out) {
  if (!s_store || !out) return false;
  return ota_status_load(*s_store, out);
}

bool url_save(const char *url) {
  if (!s_store || !ota_url_valid(url)) return false;
  return ota_url_save(*s_store, url);
}

void url_get(char *out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!s_store || !ota_url_load(*s_store, out, cap)) snprintf(out, cap, "%s", OTA_URL_DEF);
}

bool set_expected_sha(const char *hex64) {
  if (!hex64 || strlen(hex64) != 64) return false;
  static const char *HEX = "0123456789abcdefABCDEF";
  for (int i = 0; i < 32; i++) {
    const char *a = strchr(HEX, hex64[2 * i]);
    const char *b = strchr(HEX, hex64[2 * i + 1]);
    if (!a || !b || !*a || !*b) return false;
    s_expect_sha[i] = (uint8_t)((a - HEX) % 16 << 4 | (b - HEX) % 16);
  }
  s_have_expect = true;
  return true;
}

/* --- partition helpers ------------------------------------------------------------ */

/* Result of the last staged attempt, persisted for the dashboard/pane. */
static void save_status(uint8_t result, uint8_t err, const char *ver,
                        uint32_t unix_now) {
  if (!s_store) return;
  OtaStatus s;
  if (!ota_status_load(*s_store, &s)) {
    s = OtaStatus{};
    s.version = OTA_STATUS_VERSION;
  }
  s.last_result = result;
  s.last_err = err;
  snprintf(s.last_ver, sizeof(s.last_ver), "%s", ver ? ver : "");
  s.last_unix = unix_now;
  ota_status_save(*s_store, s);
}

#if defined(CONFIG_AIO_OTA_SELFTEST) && CONFIG_AIO_OTA_SELFTEST
/* selftest-only: label lookup + the partition→slot streamer (kept out of
 * the normal builds so -Wunused-function never bites under -Werror) */
static const esp_partition_t *slot_by_label(const char *label) {
  esp_partition_iterator_t it =
      esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label);
  if (!it) return nullptr;
  const esp_partition_t *p = esp_partition_get(it);
  esp_partition_iterator_release(it);
  return p;
}

/* Stream `src` into `dst` through esp_ota_ops, byte-exact to the scanned
 * image length. Returns the length, 0 on failure. Used by the selftest
 * (partition→slot copy); the HTTP path has its own reader. Every failure
 * path prints its esp_err so a CI serial tail pinpoints the step.
 *
 * The source is read through the flash cache (esp_partition_mmap) — the
 * same window the running code executes from, so QEMU serves it exactly;
 * the esp_flash driver read path (esp_partition_read) returned garbage
 * here (scan-bad at byte 0 of a verifiably valid image). Chunks stage
 * into RAM before esp_ota_write: no flash-mapped source may be live
 * across a flash operation. */
static uint32_t stream_copy(const esp_partition_t *src, const esp_partition_t *dst) {
  esp_ota_handle_t h = 0;
  esp_err_t e = esp_ota_begin(dst, 0, &h);
  if (e != ESP_OK) {
    printf("ota: copy begin err=0x%x (%s)\n", (int)e, esp_err_to_name(e));
    return 0;
  }
  printf("ota: copy src=%s @0x%lx -> dst=%s @0x%lx\n", src->label,
         (unsigned long)src->address, dst->label, (unsigned long)dst->address);

  esp_partition_mmap_handle_t mh = 0;
  const uint8_t *mm = nullptr;
  e = esp_partition_mmap(src, 0, src->size, ESP_PARTITION_MMAP_DATA,
                         (const void **)&mm, &mh);
  if (e != ESP_OK) {
    printf("ota: copy mmap err=0x%x (%s)\n", (int)e, esp_err_to_name(e));
    esp_ota_abort(h);
    return 0;
  }

  uint8_t *buf = static_cast<uint8_t *>(malloc(4096));
  if (!buf) {
    printf("ota: copy OOM\n");
    esp_partition_munmap(mh);
    esp_ota_abort(h);
    return 0;
  }
  OtaImgScan scan;
  uint32_t off = 0;
  bool done = false, ok = true;
  while (!done && ok) {
    const uint32_t chunk =
        (off + 4096 <= src->size) ? 4096 : (src->size - off);
    if (chunk == 0) {
      printf("ota: copy ran past partition end (image end not found)\n");
      ok = false;
      break;
    }
    memcpy(buf, mm + off, chunk);
    ota_img_scan(&scan, buf, chunk);
    if (scan.bad) {
      printf("ota: copy scan bad at off=%lu stage=%u hdr=%02x %02x %02x %02x\n",
             (unsigned long)off, (unsigned)scan.stage, scan.hdr[0],
             scan.hdr[1], scan.hdr[2], scan.hdr[3]);
      ok = false;
      break;
    }
    uint32_t take = chunk;
    if (scan.done) {
      if (scan.img_len <= off) {
        printf("ota: copy short scan img_len=%lu off=%lu\n",
               (unsigned long)scan.img_len, (unsigned long)off);
        ok = false;
        break;
      }
      /* the checksum+SHA tail can span past this chunk — only finish when
       * the whole image lies inside the bytes copied so far, else write
       * the full chunk and pick the tail up next round */
      if (scan.img_len <= off + chunk) {
        take = scan.img_len - off;
        done = true;
      }
    }
    e = esp_ota_write(h, buf, take);
    if (e != ESP_OK) {
      printf("ota: copy write err=0x%x at off=%lu take=%lu\n", (int)e,
             (unsigned long)off, (unsigned long)take);
      ok = false;
    }
    off += take;
  }
  free(buf);
  esp_partition_munmap(mh);
  if (!ok) {
    esp_ota_abort(h);
    return 0;
  }
  e = esp_ota_end(h);
  if (e != ESP_OK) {
    printf("ota: copy end err=0x%x (%s) img_len=%lu\n", (int)e,
           esp_err_to_name(e), (unsigned long)scan.img_len);
    esp_ota_abort(h);
    return 0;
  }
  return scan.img_len;
}
#endif /* CONFIG_AIO_OTA_SELFTEST */

/* --- download task ------------------------------------------------------------------ */

struct DownloadCtx {
  char url[OTA_URL_LEN];
};

static void download_task(void *arg) {
  DownloadCtx *ctx = static_cast<DownloadCtx *>(arg);

  const esp_partition_t *update =
      esp_ota_get_next_update_partition(esp_ota_get_running_partition());
  if (!update) {
    s_err.store(OTA_ERR_BEGIN, std::memory_order_release);
    set_phase(OTA_FAIL);
    goto done;
  }

  {
    esp_http_client_config_t cfg = {};
    cfg.url = ctx->url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    /* redirects follow automatically in IDF 5.4 (disable_auto_redirect
     * defaults to false); cap the wait, not the hops */
    cfg.timeout_ms = 15000;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c || esp_http_client_open(c, 0) != ESP_OK ||
        esp_http_client_fetch_headers(c) < 0 ||
        esp_http_client_get_status_code(c) != 200) {
      if (c) esp_http_client_cleanup(c);
      s_err.store(OTA_ERR_CONNECT, std::memory_order_release);
      set_phase(OTA_FAIL);
      goto done;
    }

    esp_ota_handle_t h = 0;
    if (esp_ota_begin(update, 0, &h) != ESP_OK) {
      esp_http_client_cleanup(c);
      s_err.store(OTA_ERR_BEGIN, std::memory_order_release);
      set_phase(OTA_FAIL);
      goto done;
    }

    {
      uint8_t *buf = static_cast<uint8_t *>(malloc(4096));
      mbedtls_sha256_context sha;
      mbedtls_sha256_init(&sha);
      mbedtls_sha256_starts(&sha, 0);
      bool ok = buf != nullptr;
      int64_t total = 0;
      const int64_t clen = esp_http_client_get_content_length(c);
      while (ok) {
        const int n = esp_http_client_read(c, reinterpret_cast<char *>(buf), 4096);
        if (n < 0) {
          ok = false;
          s_err.store(OTA_ERR_WRITE, std::memory_order_release);
          break;
        }
        if (n == 0) {
          /* EOF — but a truncated body (server died mid-send) also reads 0 */
          if (!esp_http_client_is_complete_data_received(c)) {
            ok = false;
            s_err.store(OTA_ERR_CONNECT, std::memory_order_release);
          }
          break;
        }
        mbedtls_sha256_update(&sha, buf, (size_t)n);
        if (esp_ota_write(h, buf, (size_t)n) != ESP_OK) {
          ok = false;
          s_err.store(OTA_ERR_WRITE, std::memory_order_release);
          break;
        }
        total += n;
        if (clen > 0)
          s_pct.store((uint8_t)(total * 100 / clen), std::memory_order_relaxed);
      }
      /* pct is advisory (content length may be unknown); set 100 on EOF */
      if (ok) s_pct.store(100, std::memory_order_release);
      set_phase(OTA_VERIFY);

      uint8_t digest[32];
      mbedtls_sha256_finish(&sha, digest);
      mbedtls_sha256_free(&sha);
      free(buf);

      if (ok && s_have_expect &&
          memcmp(digest, s_expect_sha, sizeof(digest)) != 0) {
        ok = false;
        s_err.store(OTA_ERR_SHA, std::memory_order_release);
      }
      if (ok && esp_ota_end(h) != ESP_OK) {
        ok = false;
        s_err.store(OTA_ERR_END, std::memory_order_release);
      }
      esp_app_desc_t desc = {};
      if (ok && esp_ota_get_partition_description(update, &desc) == ESP_OK) {
        /* %.23s: desc.version is 32 B, s_target 24 — a hard cap keeps
         * -Wformat-truncation quiet; semver fits easily */
        snprintf(s_target, sizeof(s_target), "%.23s", desc.version);
      }
      if (ok && esp_ota_set_boot_partition(update) != ESP_OK) {
        ok = false;
        s_err.store(OTA_ERR_SETBOOT, std::memory_order_release);
      }
      esp_http_client_cleanup(c);
      if (ok) {
        printf("ota: staged %s (%lld KB) — reboot to apply\n", s_target,
               (long long)(total / 1024));
        save_status(OTA_RES_NONE, OTA_ERR_NONE, s_target,
                    s_unix ? (uint32_t)s_unix() : 0);
        set_phase(OTA_NEED_REBOOT);
      } else {
        printf("ota: download failed (%s)\n", ota_err_name(s_err.load()));
        save_status(OTA_RES_ERR, s_err.load(), "",
                    s_unix ? (uint32_t)s_unix() : 0);
        set_phase(OTA_FAIL);
      }
    }
  }

done:
  delete ctx;
  vTaskDelete(nullptr);
}

bool start(const char *url) {
  if (busy()) return false;
  if (!url || !ota_url_valid(url)) {
    s_err.store(OTA_ERR_URL, std::memory_order_release);
    return false;
  }
  DownloadCtx *ctx = new DownloadCtx{};
  snprintf(ctx->url, sizeof(ctx->url), "%s", url);
  s_err.store(OTA_ERR_NONE, std::memory_order_release);
  s_target[0] = '\0';
  s_have_expect = false;
  s_pct.store(0, std::memory_order_release);
  set_phase(OTA_DOWNLOAD);
  if (xTaskCreate(download_task, "ota-dl", 12288, ctx, 4, nullptr) != pdPASS) {
    delete ctx;
    s_err.store(OTA_ERR_BEGIN, std::memory_order_release);
    set_phase(OTA_FAIL);
    return false;
  }
  return true;
}

/* --- boot side ---------------------------------------------------------------------- */

void tick(uint32_t now_ms) {
  static bool armed = false;
  if (s_phase.load(std::memory_order_acquire) != OTA_PENDING) return;
  if (!armed) {
    /* arm on the first healthy loop tick — the window measures liveness,
     * not boot time */
    armed = true;
    s_confirm_at = now_ms + CONFIG_AIO_OTA_CONFIRM_MS;
    return;
  }
  if ((int32_t)(now_ms - s_confirm_at) < 0) return;
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    printf("ota: image confirmed valid (%s)\n", s_ver);
    save_status(OTA_RES_OK, OTA_ERR_NONE, s_ver,
                s_unix ? (uint32_t)s_unix() : 0);
    set_phase(OTA_VALID);
#if defined(CONFIG_AIO_OTA_SELFTEST) && CONFIG_AIO_OTA_SELFTEST
    uint8_t stage = 0;
    if (store_get(*s_store, SELF_KEY, 1, &stage, 1) == 1 && stage == 1) {
      printf("ota: selftest confirmed — rebooting into stage 2\n");
      stage = 2;
      store_put(*s_store, SELF_KEY, 1, &stage, 1);
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_restart();
    }
#endif
  }
}

#if defined(CONFIG_AIO_OTA_SELFTEST) && CONFIG_AIO_OTA_SELFTEST
static void selftest_boot(const esp_partition_t *run) {
  uint8_t stage = 0, boots = 0;
  const int rn = store_get(*s_store, SELF_KEY, 1, &stage, 1);
  if (rn != 1) stage = 0;
  if (store_get(*s_store, BOOT_KEY, 1, &boots, 1) != 1) boots = 0;
  boots++;
  store_put(*s_store, BOOT_KEY, 1, &boots, 1);
  if (boots > 14) {
    printf("ota: selftest STUCK after %u boots — aborting\n", (unsigned)boots);
    stage = 0;
    store_put(*s_store, SELF_KEY, 1, &stage, 1);
    return;
  }

  switch (stage) {
    case 0: {
      const esp_partition_t *dst = esp_ota_get_next_update_partition(run);
      printf("ota: selftest stage0 — copying the running image into %s\n",
             dst ? dst->label : "?");
      const uint32_t len = dst ? stream_copy(run, dst) : 0;
      printf("ota: selftest stage0 %s (%lu B)\n", len ? "staged" : "FAILED",
             (unsigned long)len);
      if (!len) return; /* stay: CI will see a missing marker */
      /* esp_ota_begin only erases — otadata needs an explicit boot select
       * or the next boot lands back in factory and the walk stalls */
      if (esp_ota_set_boot_partition(dst) != ESP_OK) {
        printf("ota: selftest stage0 set_boot FAILED\n");
        stage = 0;
        store_put(*s_store, SELF_KEY, 1, &stage, 1);
        return;
      }
      stage = 1;
      store_put(*s_store, SELF_KEY, 1, &stage, 1);
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_restart();
      return;
    }
    case 1:
      /* running the staged copy — tick() confirms and advances to 2 */
      printf("ota: selftest stage1 — running %s, awaiting health confirm\n",
             run->label);
      return;
    case 2: {
      const esp_partition_t *bad = slot_by_label("badapp");
      const esp_partition_t *dst = slot_by_label("ota_1");
      printf("ota: selftest stage2 — staging %s into ota_1\n",
             bad ? "badapp" : "(missing!)");
      const uint32_t len = bad && dst ? stream_copy(bad, dst) : 0;
      printf("ota: selftest stage2 %s (%lu B)\n", len ? "staged" : "FAILED",
             (unsigned long)len);
      if (!len) return;
      stage = 3;
      store_put(*s_store, SELF_KEY, 1, &stage, 1);
      if (esp_ota_set_boot_partition(dst) != ESP_OK) {
        printf("ota: selftest set_boot FAILED\n");
        stage = 0;
        store_put(*s_store, SELF_KEY, 1, &stage, 1);
        return;
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_restart();
      return;
    }
    case 3: {
      const esp_partition_t *ota1 = slot_by_label("ota_1");
      esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
      const bool invalid =
          ota1 && esp_ota_get_state_partition(ota1, &st) == ESP_OK &&
          st == ESP_OTA_IMG_INVALID;
      printf("ota: selftest rollback %s (running %s, ota_1 state %d)\n",
             invalid ? "VERIFIED" : "not-invalid", run->label, (int)st);
      stage = 0;
      store_put(*s_store, SELF_KEY, 1, &stage, 1);
      return;
    }
    default:
      stage = 0;
      store_put(*s_store, SELF_KEY, 1, &stage, 1);
      return;
  }
}
#endif /* CONFIG_AIO_OTA_SELFTEST */

void begin(IHubStore *store, int64_t (*unix_fn)(void)) {
  s_store = store;
  s_unix = unix_fn;
  const esp_partition_t *run = esp_ota_get_running_partition();
  /* %.9s: partition labels are 16 B max, slots here are factory/ota_0/ota_1 */
  snprintf(s_slot, sizeof(s_slot), "%.9s", run->label);
  /* %.23s: app version field is 32 B, s_ver 24 */
  snprintf(s_ver, sizeof(s_ver), "%.23s", esp_app_get_description()->version);

#if defined(CONFIG_AIO_OTA_SELFTEST) && CONFIG_AIO_OTA_SELFTEST
  selftest_boot(run);
#endif

  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  if (esp_ota_get_state_partition(run, &st) == ESP_OK &&
      st == ESP_OTA_IMG_PENDING_VERIFY) {
    s_confirm_at = 0; /* set by the first tick */
    set_phase(OTA_PENDING);
    printf("ota: running %s %s — PENDING_VERIFY (confirm window %d s)\n",
           s_slot, s_ver, (int)(CONFIG_AIO_OTA_CONFIRM_MS / 1000));
  }
}

bool reboot_staged(void) {
  const uint8_t p = s_phase.load(std::memory_order_acquire);
  if (p != OTA_NEED_REBOOT && p != OTA_PENDING) return false;
  vTaskDelay(pdMS_TO_TICKS(150)); /* let the pane log flush */
  esp_restart();
  return true; /* not reached */
}

bool boot_factory(void) {
  const esp_partition_t *factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (!factory) return false;
  printf("ota: escape hatch — next boot = factory\n");
  if (esp_ota_set_boot_partition(factory) != ESP_OK) return false;
  vTaskDelay(pdMS_TO_TICKS(150));
  esp_restart();
  return true; /* not reached */
}

}  // namespace ota
}  // namespace aio
