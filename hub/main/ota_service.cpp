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

#include <bootloader_common.h>
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
 * (partition→slot copy); the HTTP path has its own reader.
 *
 * Source reads are plain esp_partition_read — sequential, no ordering
 * tricks. The QEMU story so far (all CI-pinned):
 *   - v0.11.0: a 6 MB runtime esp_partition_mmap SIGFPE'd the QEMU MMU
 *     model, and reads after a partition-wide erase looked poisoned;
 *   - v0.12.0: even a 16-byte esp_partition_mmap reset the guest
 *     instantly, and the ground-truth flash dump proved the merge right:
 *     factory@0x10000 holds `e9 06 02 4f`, yet esp_partition_read
 *     returned `00 00 e9 06` — the QIO fast-read model prepends two
 *     phantom bytes to every response, shifting the stream by two while
 *     the bootloader's standard reads validate and boot the same bytes.
 *   - v0.12.0 round 3 (DIO pinned, reads right): esp_ota_end still can't
 *     run in the guest. Its internal esp_image_verify maps the staged
 *     image through the runtime cache, and the model host-SIGSEGV'd QEMU
 *     (rc=-11) mid-walk on the ota_1 mapping (0xa60000) — the copy was
 *     already byte-identical on flash. Even in the surviving 0x62 range
 *     the first post-write window pass mis-reads: esp_ota_end refused a
 *     byte-identical copy (ESP_ERR_OTA_VALIDATE_FAILED) that a follow-up
 *     esp_image_verify (set_boot's) accepted moments later.
 *   - v0.12.0 round 4 (slots < 8 MB): esp_ota_end is gone, but
 *     esp_ota_set_boot_partition still validates through the same
 *     runtime-cache path and refused the byte-exact ota_0 copy twice
 *     ("Image hash failed - image is corrupt"), killing the walk at
 *     stage0. Below the 8 MB line the failure is a clean hash verdict,
 *     not a SIGSEGV — and no retry helps: the window stays wrong.
 * So NO cache-backed runtime verification may gate the selftest —
 * neither esp_ota_end (dropped) nor set_boot's internal one, and after
 * round 5 no cache-backed otadata read either (read_otadata() mmaps in
 * app context; the rollback boot died in it after four boots had read
 * the same sectors fine). The copy is gated by copy_is_exact() (SPI
 * reads), the boot slot by boot_select() below (IDF API first, then a
 * raw otadata write mirroring esp_rewrite_ota_data() — IDF v5.4
 * esp_ota_ops.c — through esp_partition read/erase/write), and every
 * state lookup/confirm by the raw SPI helpers. The bootloader owns the
 * real image validation with its own (proven) mapping path at the next
 * boot. The QEMU sdkconfig overlays pin DIO flash mode (reads correct)
 * and partitions_qemu_ota.csv keeps every executed slot below the 8 MB
 * line where the boot path is proven. The one-shot "copy diag" line
 * prints the first source bytes so a CI round proves the mode fix
 * without a second run. */
/* Selftest-only: compare `len` bytes of src vs dst through the (now
 * correct) read path; on the first divergence print it and stop. */
static bool copy_is_exact(const esp_partition_t *src, const esp_partition_t *dst,
                          uint32_t len) {
  static uint8_t a[64], b[64];
  for (uint32_t q = 0; q < len; q += sizeof(a)) {
    const uint32_t n = (len - q) < sizeof(a) ? (len - q) : sizeof(a);
    if (esp_partition_read(src, q, a, n) != ESP_OK ||
        esp_partition_read(dst, q, b, n) != ESP_OK)
      return false;
    if (memcmp(a, b, n) != 0) {
      for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i]) {
          printf("ota: copy diff at 0x%lx: src=%02x dst=%02x\n",
                 (unsigned long)(q + i), a[i], b[i]);
          break;
        }
      return false;
    }
  }
  return true;
}

/* --- boot select -----------------------------------------------------------------
 *
 * esp_ota_set_boot_partition() re-validates the target with
 * esp_image_verify(ESP_IMAGE_VERIFY) before it writes otadata, and the
 * app-context verifier maps the whole image through the runtime cache —
 * the one flash path the QEMU model gets wrong (round 4: two "Image hash
 * failed" verdicts on ota_0, a copy the SPI read path had just proven
 * byte-identical — see the QEMU story above). The bootloader re-validates
 * the slot with its own (proven) mapping at the next boot anyway, so the
 * IDF helper is tried once and, when it refuses, otadata is written
 * directly: the same entry, seq and state arithmetic as
 * esp_rewrite_ota_data() in IDF v5.4 esp_ota_ops.c, but through
 * esp_partition_read/erase/write — the path QEMU models faithfully.
 * Entry format (esp_flash_partitions.h, 32 B): { uint32 ota_seq;
 * uint8 seq_label[20]; uint32 ota_state; uint32 crc } with the CRC over
 * the 4-byte ota_seq only — computed by the real
 * bootloader_common_ota_select_crc(), already linked for the app.
 *   - active entry = the valid one with the max seq (tie: sector 0);
 *   - new seq = (slot+1) % count + i*count, smallest >= active seq;
 *   - written to the inactive sector (sector 0 when both are invalid);
 *   - state = ESP_OTA_IMG_NEW: the bootloader rewrites it PENDING_VERIFY
 *     at the next boot (bootloader_utility.c), exactly the state the
 *     walk's markers assert. */
static bool otadata_raw_select(const esp_partition_t *dst) {
  const esp_partition_t *otadata = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otadata) return false;
  const int slot = (int)dst->subtype - (int)ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
  const int count = esp_ota_get_app_partition_count();
  if (count == 0 || slot < 0 || slot >= count) return false;

  esp_ota_select_entry_t sel[2];
  if (esp_partition_read(otadata, 0, &sel[0], sizeof(sel[0])) != ESP_OK ||
      esp_partition_read(otadata, otadata->erase_size, &sel[1],
                         sizeof(sel[1])) != ESP_OK)
    return false;
  const bool valid[2] = {bootloader_common_ota_select_valid(&sel[0]),
                         bootloader_common_ota_select_valid(&sel[1])};
  const int active =
      (valid[0] && valid[1]) ? (sel[0].ota_seq >= sel[1].ota_seq ? 0 : 1)
                             : (valid[0] ? 0 : (valid[1] ? 1 : -1));

  uint32_t seq;
  int sec;
  if (active >= 0) {
    const uint32_t base = (uint32_t)(slot + 1) % (uint32_t)count;
    uint32_t i = 0;
    while (base + i * (uint32_t)count < sel[active].ota_seq) i++;
    seq = base + i * (uint32_t)count;
    sec = (~active) & 1; /* the IDF helper always rewrites the inactive one */
  } else {
    seq = (uint32_t)(slot + 1); /* unformatted otadata: first seq */
    sec = 0;
  }
  sel[sec].ota_seq = seq;
  sel[sec].ota_state = ESP_OTA_IMG_NEW;
  sel[sec].crc = bootloader_common_ota_select_crc(&sel[sec]);

  const size_t off = (size_t)sec * otadata->erase_size;
  if (esp_partition_erase_range(otadata, off, otadata->erase_size) != ESP_OK)
    return false;
  if (esp_partition_write(otadata, off, &sel[sec], sizeof(sel[sec])) != ESP_OK)
    return false;

  /* read-back gate — only the SPI path is trusted in the guest */
  esp_ota_select_entry_t back;
  if (esp_partition_read(otadata, off, &back, sizeof(back)) != ESP_OK ||
      memcmp(&back, &sel[sec], sizeof(back)) != 0)
    return false;
  printf("ota: otadata[%d] := seq=%lu state=NEW (%s) — raw select\n", sec,
         (unsigned long)seq, dst->label);
  return true;
}

/* Try the IDF API first (it owns the otadata scheme and works whenever
 * the model's cache window happens to read the fresh copy right — round
 * 3 proved it can); its refusal writes nothing, so falling back to the
 * raw select is always safe. Either way the bootloader validates the
 * slot image for real at the next boot. */
static bool boot_select(const esp_partition_t *dst) {
  if (esp_ota_set_boot_partition(dst) == ESP_OK) return true;
  printf("ota: set_boot refused — runtime-cache verify mis-read (QEMU model); raw otadata select\n");
  return otadata_raw_select(dst);
}

/* Selftest-only state lookup through the SPI path: esp_ota_get_state_partition
 * reads otadata through esp_partition_mmap — the runtime-cache path the QEMU
 * model dies on (round 5: the rollback boot SIGSEGV'd QEMU right there, after
 * four boots had read the same two sectors without complaint — read_otadata()
 * mmaps in app context too). Mirrors the IDF lookup exactly: the entry whose
 * (seq-1) % count maps to the partition's slot and whose CRC checks out wins;
 * no match → false (the caller treats it like ESP_ERR_NOT_FOUND). */
static bool slot_state_raw(const esp_partition_t *p, esp_ota_img_states_t *out) {
  const esp_partition_t *otadata = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otadata || !p) return false;
  const int slot = (int)p->subtype - (int)ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
  const int count = esp_ota_get_app_partition_count();
  if (count == 0 || slot < 0 || slot >= count) return false;
  for (int i = 0; i < 2; i++) {
    esp_ota_select_entry_t e;
    if (esp_partition_read(otadata, (size_t)i * otadata->erase_size, &e,
                           sizeof(e)) != ESP_OK)
      return false;
    if (e.ota_seq != UINT32_MAX &&
        e.crc == bootloader_common_ota_select_crc(&e) &&
        (uint32_t)((e.ota_seq - 1) % (uint32_t)count) == (uint32_t)slot) {
      *out = (esp_ota_img_states_t)e.ota_state;
      return true;
    }
  }
  return false;
}

/* Selftest-only confirm: esp_ota_mark_app_valid_cancel_rollback() reads
 * otadata through esp_partition_mmap as well. Mirror its
 * esp_ota_current_ota_is_workable(true) core over the SPI path: the active
 * entry for the running slot is marked VALID and rewritten in place (same
 * sector, same seq — the sector-swap scheme is untouched). */
static bool ota_confirm_raw(void) {
  const esp_partition_t *otadata = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  const esp_partition_t *run = esp_ota_get_running_partition();
  if (!otadata || !run ||
      (int)run->subtype < (int)ESP_PARTITION_SUBTYPE_APP_OTA_MIN)
    return false;
  const int slot = (int)run->subtype - (int)ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
  const int count = esp_ota_get_app_partition_count();
  if (count == 0 || slot < 0 || slot >= count) return false;

  esp_ota_select_entry_t sel[2];
  if (esp_partition_read(otadata, 0, &sel[0], sizeof(sel[0])) != ESP_OK ||
      esp_partition_read(otadata, otadata->erase_size, &sel[1],
                         sizeof(sel[1])) != ESP_OK)
    return false;
  const bool valid[2] = {bootloader_common_ota_select_valid(&sel[0]),
                         bootloader_common_ota_select_valid(&sel[1])};
  const int active =
      (valid[0] && valid[1]) ? (sel[0].ota_seq >= sel[1].ota_seq ? 0 : 1)
                             : (valid[0] ? 0 : (valid[1] ? 1 : -1));
  if (active < 0 ||
      (int)((sel[active].ota_seq - 1) % (uint32_t)count) != slot)
    return false;
  if (sel[active].ota_state == ESP_OTA_IMG_VALID) return true;
  sel[active].ota_state = ESP_OTA_IMG_VALID;
  const size_t off = (size_t)active * otadata->erase_size;
  if (esp_partition_erase_range(otadata, off, otadata->erase_size) != ESP_OK)
    return false;
  if (esp_partition_write(otadata, off, &sel[active],
                          sizeof(sel[active])) != ESP_OK)
    return false;
  printf("ota: otadata[%d] := seq=%lu state=VALID — raw confirm\n", active,
         (unsigned long)sel[active].ota_seq);
  return true;
}

static uint32_t stream_copy(const esp_partition_t *src, const esp_partition_t *dst) {
  esp_ota_handle_t h = 0;
  esp_err_t e = esp_ota_begin(dst, OTA_WITH_SEQUENTIAL_WRITES, &h);
  if (e != ESP_OK) {
    printf("ota: copy begin err=0x%x (%s)\n", (int)e, esp_err_to_name(e));
    return 0;
  }
  printf("ota: copy src=%s @0x%lx -> dst=%s @0x%lx (sequential)\n", src->label,
         (unsigned long)src->address, dst->label, (unsigned long)dst->address);

  /* one-shot diagnostic: the first 8 source bytes as the SPI path sees
   * them — `e9 06 02 4f ...` when the flash mode model is right */
  {
    uint8_t d[8] = {};
    esp_partition_read(src, 0, d, sizeof(d));
    printf("ota: copy diag first=%02x %02x %02x %02x %02x %02x %02x %02x\n",
           d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
  }

  static uint8_t buf[4096]; /* selftest-only, one caller at boot */
  OtaImgScan scan;
  uint32_t off = 0; /* image bytes handed to esp_ota_write so far */
  bool done = false;

  while (!done) {
    uint32_t want = sizeof(buf);
    if (off + want > src->size) want = src->size - off;
    e = esp_partition_read(src, off, buf, want);
    if (e != ESP_OK) {
      printf("ota: copy read err=0x%x (%s) at off=%lu\n", (int)e,
             esp_err_to_name(e), (unsigned long)off);
      break;
    }
    ota_img_scan(&scan, buf, want);
    if (scan.bad) {
      printf("ota: copy scan bad at off=%lu stage=%u hdr=%02x %02x %02x %02x\n",
             (unsigned long)off, (unsigned)scan.stage, scan.hdr[0],
             scan.hdr[1], scan.hdr[2], scan.hdr[3]);
      break;
    }
    uint32_t take = want;
    if (scan.done) {
      if (scan.img_len <= off) {
        printf("ota: copy short scan img_len=%lu off=%lu\n",
               (unsigned long)scan.img_len, (unsigned long)off);
        break;
      }
      /* the checksum+SHA tail can span past this chunk — only finish when
       * the whole image lies inside the bytes copied so far, else write
       * the full chunk and pick the tail up next round */
      if (scan.img_len <= off + want) {
        take = scan.img_len - off;
        done = true;
      }
    }
    e = esp_ota_write(h, buf, take);
    if (e != ESP_OK) {
      printf("ota: copy write err=0x%x (%s) at off=%lu take=%lu\n", (int)e,
             esp_err_to_name(e), (unsigned long)off, (unsigned long)take);
      break;
    }
    off += take;
  }
  if (!done) {
    esp_ota_abort(h);
    return 0;
  }
  /* Round 3+4 (see the QEMU story above): esp_ota_end never runs in the
   * guest — its verifier mmaps the staged image and the model kills the
   * host or mis-reads the fresh bytes. copy_is_exact() is the acceptance
   * gate instead; esp_ota_abort() releases the handle without touching
   * the written bytes, and boot_select() hands the slot to the
   * bootloader, which validates it for real at the next boot (the same
   * chain the round-2 fallback proved in stage0). */
  const bool exact = copy_is_exact(src, dst, scan.img_len);
  esp_ota_abort(h);
  if (!exact) {
    printf("ota: copy mismatch vs source (%lu B)\n",
           (unsigned long)scan.img_len);
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
#if defined(CONFIG_AIO_OTA_SELFTEST) && CONFIG_AIO_OTA_SELFTEST
  /* the selftest build confirms through the SPI path — the IDF helper
   * mmaps otadata (round 5) */
  const bool confirmed = ota_confirm_raw();
#else
  const bool confirmed = esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
#endif
  if (confirmed) {
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
      /* fixed selftest table: the first walk copies into ota_0, resolved
       * by label — esp_ota_get_next_update_partition() would read otadata
       * through esp_partition_mmap, the cache path (round 5) */
      const esp_partition_t *dst = slot_by_label("ota_0");
      printf("ota: selftest stage0 — copying the running image into %s\n",
             dst ? dst->label : "?");
      const uint32_t len = dst ? stream_copy(run, dst) : 0;
      printf("ota: selftest stage0 %s (%lu B)\n", len ? "staged" : "FAILED",
             (unsigned long)len);
      if (!len) return; /* stay: CI will see a missing marker */
      /* esp_ota_begin only erases — otadata needs an explicit boot select
       * or the next boot lands back in factory and the walk stalls */
      if (!boot_select(dst)) {
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
      if (!boot_select(dst)) {
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
      /* The walk VERIFIES rollback when the hub is back on the previous
       * valid slot AND the unconfirmed slot is dead. IDF 5.4's bootloader
       * downgrades a slot that rebooted without confirming to
       * ESP_OTA_IMG_ABORTED — unconditionally (no reset-reason check;
       * ESP_OTA_IMG_INVALID is only ever written by the explicit app-side
       * rollback API) — and ota_select_invalid() excludes ABORTED from
       * boot candidates, which is what performs the fallback. Accept both
       * states here; the running-partition check is the hard evidence.
       * State comes through the SPI path (round 5: the cache-backed
       * esp_ota_get_state_partition killed the model on this exact boot). */
      const bool fell_back = strcmp(run->label, "ota_0") == 0;
      const bool dead_slot =
          ota1 && slot_state_raw(ota1, &st) &&
          (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED);
      printf("ota: selftest rollback %s (running %s, ota_1 state %d)\n",
             (fell_back && dead_slot) ? "VERIFIED" : "not-invalid",
             run->label, (int)st);
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
#if defined(CONFIG_AIO_OTA_SELFTEST) && CONFIG_AIO_OTA_SELFTEST
  /* SPI-path state lookup — esp_ota_get_state_partition() mmaps (round 5) */
  const bool have_state = slot_state_raw(run, &st);
#else
  const bool have_state = esp_ota_get_state_partition(run, &st) == ESP_OK;
#endif
  if (have_state && st == ESP_OTA_IMG_PENDING_VERIFY) {
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
