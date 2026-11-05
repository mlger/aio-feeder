/*
 * AIO Feeder — hub OTA core (PLAN.md §7.6, §12 M8a).
 *
 * Pure logic, host-tested. The firmware glue (hub/main/ota_service.cpp)
 * runs the actual esp_ota_ops + esp_http_client flow; this header owns the
 * parts worth testing on the host:
 *
 *   - URL validation (scheme + length + no whitespace/control bytes);
 *   - a tiny semver parse/compare for the "update available?" line —
 *     advisory only: the user points at an image, the hub never refuses
 *     a download just because versions look unordered;
 *   - the esp_image streaming length scanner — a copied image must be
 *     written at its exact length or esp_ota_end rejects the trailing
 *     0xFF padding (and a hash-appended image fails the SHA check);
 *   - the NVS status blob ("ota") + the brief view the Firmware pane and
 *     the web dashboard render.
 *
 * D37 (v0.11.0): the hub pulls a plain HTTP(S) image — LAN threat model,
 * esp_ota_end verifies the image checksum and the release ota.json pins
 * the SHA-256 so the download can be checked end-to-end. Client TLS certs
 * are a later hardening item (M8c); ESP-NOW nodes are untouched here.
 */
#ifndef AIO_OTA_CORE_H
#define AIO_OTA_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hub_store.h"

namespace aio {

constexpr size_t OTA_URL_LEN = 160; /* 159 + NUL */
/* The Pages site carries the same build the manifests describe; a hub with
 * no stored URL defaults here (web flasher → same firmware, one hop). */
constexpr char OTA_URL_DEF[] = "https://mlger.github.io/aio-feeder/hub/firmware.bin";
constexpr char OTA_MANIFEST_NAME[] = "ota.json";

/* --- URL validation ------------------------------------------------------------ */

static inline bool ota_url_valid(const char *s) {
  if (!s) return false;
  const size_t n = strlen(s);
  if (n == 0 || n >= OTA_URL_LEN) return false;
  const bool http = strncmp(s, "http://", 7) == 0;
  const bool https = strncmp(s, "https://", 8) == 0;
  if (!http && !https) return false;
  for (const char *p = s; *p; p++)
    if ((uint8_t)*p < 0x21 || (uint8_t)*p == 0x7F) return false;
  return true;
}

/* --- advisory semver ------------------------------------------------------------- */

struct OtaSemver {
  uint8_t maj = 0, min = 0, pat = 0;
};

/* "v0.11.0", "0.11.0", "v0.11.0-3-gabc" → {0,11,0}. Anything else false. */
static inline bool ota_semver(const char *s, OtaSemver *out) {
  if (!s || !out) return false;
  const char *p = (*s == 'v' || *s == 'V') ? s + 1 : s;
  uint32_t v[3] = {0, 0, 0};
  int idx = 0, digits = 0;
  for (; *p; p++) {
    if (*p >= '0' && *p <= '9') {
      v[idx] = v[idx] * 10 + (uint32_t)(*p - '0');
      if (v[idx] > 255) return false;
      digits++;
      continue;
    }
    if (*p == '.' && idx < 2 && digits) {
      idx++;
      digits = 0;
      continue;
    }
    break; /* "-3-gabc", end, or garbage → the numbers so far stand */
  }
  if (idx != 2 || !digits) return false;
  *out = OtaSemver{(uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2]};
  return true;
}

static inline int ota_semver_cmp(const OtaSemver &a, const OtaSemver &b) {
  if (a.maj != b.maj) return a.maj < b.maj ? -1 : 1;
  if (a.min != b.min) return a.min < b.min ? -1 : 1;
  if (a.pat != b.pat) return a.pat < b.pat ? -1 : 1;
  return 0;
}

/* Advisory "is the candidate newer?" — false unless BOTH parse. */
static inline bool ota_ver_newer(const char *cur, const char *cand) {
  OtaSemver a, b;
  if (!ota_semver(cur, &a) || !ota_semver(cand, &b)) return false;
  return ota_semver_cmp(b, a) > 0;
}

/* --- esp_image streaming length scanner -------------------------------------------
 *
 * Feeds chunks; once `done`, `img_len` is the exact image length: header +
 * segments + 16-byte-aligned checksum byte (+32 B SHA-256 when the header
 * says hash_appended). Matches esp_image_metadata_t.image_len for the
 * images the IDF build produces. Offset machine — never needs the payload
 * bytes themselves, so the device can scan while it streams the copy. */
struct OtaImgScan {
  static constexpr uint8_t HDR_LEN = 24;
  uint8_t hdr[HDR_LEN] = {};
  uint8_t hdr_have = 0;
  uint8_t shdr[8] = {};    /* partial segment header absorb */
  uint8_t shdr_have = 0;

  uint8_t stage = 0; /* 0 header · 1 seg header · 2 seg data · 3 tail */
  uint8_t segs_left = 0;
  uint32_t seg_left = 0;
  uint32_t off = 0; /* image bytes consumed so far (headers + data) */
  bool hash_appended = false;
  bool done = false;
  bool bad = false;
  uint32_t img_len = 0;
};

static inline void ota_img_scan(OtaImgScan *s, const uint8_t *chunk, size_t n) {
  const uint8_t *p = chunk;
  while (n) {
    if (s->bad || s->done) return;
    switch (s->stage) {
      case 0: { /* image header: magic 0xE9, segment count, hash flag */
        while (s->hdr_have < OtaImgScan::HDR_LEN && n) {
          s->hdr[s->hdr_have++] = *p++;
          n--;
          s->off++;
        }
        if (s->hdr_have < OtaImgScan::HDR_LEN) return;
        if (s->hdr[0] != 0xE9) {
          s->bad = true;
          return;
        }
        s->segs_left = s->hdr[1];
        if (s->segs_left == 0 || s->segs_left > 16) {
          s->bad = true;
          return;
        }
        s->hash_appended = s->hdr[23] != 0;
        s->stage = 1;
        break;
      }
      case 1: { /* 8-byte segment header: load addr (4) + data len (4) */
        while (s->shdr_have < 8 && n) {
          s->shdr[s->shdr_have++] = *p++;
          n--;
          s->off++;
        }
        if (s->shdr_have < 8) return;
        s->shdr_have = 0;
        s->seg_left = (uint32_t)s->shdr[4] | ((uint32_t)s->shdr[5] << 8) |
                      ((uint32_t)s->shdr[6] << 16) |
                      ((uint32_t)s->shdr[7] << 24);
        s->stage = 2;
        break;
      }
      case 2: { /* segment data — skip over it */
        if (n < s->seg_left) {
          s->seg_left -= (uint32_t)n;
          s->off += (uint32_t)n;
          n = 0;
          return;
        }
        const uint32_t take = s->seg_left;
        p += take;
        n -= take;
        s->off += take;
        s->seg_left = 0;
        if (--s->segs_left == 0)
          s->stage = 3;
        else
          s->stage = 1;
        break;
      }
      default: { /* tail: checksum byte right after the data, 0xFF pad to the
                   * next 16-byte block, then the appended SHA256 —
                   * esp_image_format.c process_checksum reads the block
                   * [unpadded, ALIGN16(unpadded + 1)) and the hash right
                   * after it. The old pad-then-checksum model computed a
                   * 7-15 byte short length: copy_is_exact passed (prefix)
                   * but the bootloader hash-failed the truncated slot —
                   * only when the data end sat at off%16 >= 2, which is why
                   * the selftest was green on some binaries and broken on
                   * others. */
        const uint32_t blk = ((s->off + 1 + 15) & ~15u);
        s->img_len = blk + (s->hash_appended ? 32 : 0);
        s->done = true;
        return;
      }
    }
  }
}

/* --- status blob ("ota") + stored URL ("ourl") ------------------------------------ */

#define OTA_STATUS_VERSION 1
constexpr char OTA_STORE_KEY[] = "ota";
constexpr char OTA_URL_KEY[] = "ourl";

enum OtaResult : uint8_t {
  OTA_RES_NONE = 0,
  OTA_RES_OK,     /* image confirmed valid after reboot */
  OTA_RES_ERR,
};

enum OtaErr : uint8_t {
  OTA_ERR_NONE = 0,
  OTA_ERR_URL,      /* rejected before any connection */
  OTA_ERR_CONNECT,  /* http client open/headers failed */
  OTA_ERR_BEGIN,    /* esp_ota_begin failed (slot, erase) */
  OTA_ERR_WRITE,
  OTA_ERR_END,      /* esp_ota_end: checksum/structure */
  OTA_ERR_SHA,      /* expected digest mismatch */
  OTA_ERR_SETBOOT,
  OTA_ERR_COUNT
};

static inline const char *ota_err_name(uint8_t e) {
  static const char *N[OTA_ERR_COUNT] = {"none", "url", "connect", "begin",
                                         "write", "end", "sha", "setboot"};
  return e < OTA_ERR_COUNT ? N[e] : "?";
}

struct OtaStatus {
  uint16_t version;    /* OTA_STATUS_VERSION */
  uint8_t last_result; /* OtaResult */
  uint8_t last_err;    /* OtaErr */
  char last_ver[24];   /* version that was staged (or "") */
  uint32_t last_unix;  /* wall clock at the terminal event */
};

static inline bool ota_status_load(IHubStore &st, OtaStatus *out) {
  OtaStatus s;
  if (store_get(st, OTA_STORE_KEY, OTA_STATUS_VERSION, &s, sizeof(s)) !=
      (int)sizeof(s))
    return false;
  *out = s;
  return true;
}

static inline bool ota_status_save(IHubStore &st, const OtaStatus &s) {
  return store_put(st, OTA_STORE_KEY, OTA_STATUS_VERSION, &s, sizeof(s));
}

static inline bool ota_url_load(IHubStore &st, char *out, size_t cap) {
  if (cap < OTA_URL_LEN) return false;
  out[0] = '\0';
  /* stored as a versioned blob; len = strlen + 1 (buffer carries the
   * 6-byte StoreHdr framing too — store_get caps the whole blob) */
  uint8_t blob[sizeof(StoreHdr) + OTA_URL_LEN];
  int n = store_get(st, OTA_URL_KEY, 1, blob, sizeof(blob));
  if (n <= 0) return false;
  memcpy(out, blob, (size_t)n);
  out[OTA_URL_LEN - 1] = '\0';
  return ota_url_valid(out);
}

static inline bool ota_url_save(IHubStore &st, const char *url) {
  if (!ota_url_valid(url)) return false;
  return store_put(st, OTA_URL_KEY, 1, url, (uint16_t)(strlen(url) + 1));
}

/* --- brief view (Firmware pane + web dashboard) ----------------------------------- */

enum OtaPhase : uint8_t {
  OTA_IDLE = 0,
  OTA_DOWNLOAD,
  OTA_VERIFY, /* bytes done; esp_ota_end/sha/setboot running */
  OTA_NEED_REBOOT,
  OTA_PENDING, /* running the new slot, health window ticking */
  OTA_VALID,   /* new image confirmed */
  OTA_FAIL,
  OTA_PHASE_COUNT
};

static inline const char *ota_phase_name(uint8_t p) {
  static const char *N[OTA_PHASE_COUNT] = {
      "idle", "downloading", "verifying", "need reboot", "pending verify",
      "valid", "failed"};
  return p < OTA_PHASE_COUNT ? N[p] : "?";
}

struct OtaBrief {
  uint8_t phase = OTA_IDLE; /* OtaPhase */
  uint8_t pct = 0;
  uint8_t err = OTA_ERR_NONE;
  char slot[10] = "factory"; /* running slot name */
  char ver[24] = "";         /* running version */
  char target[24] = "";      /* staged version, once known */
};

}  // namespace aio

#endif /* AIO_OTA_CORE_H */
