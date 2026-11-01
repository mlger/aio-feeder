/*
 * AIO Feeder — hub storage abstraction + CRC'd blob framing (PLAN.md §7.5).
 *
 * Pure logic, host-tested. The device binds this to NVS (Preferences) in the
 * hub firmware; host tests use a RAM map that doubles as the reboot
 * simulator. Blobs carry {magic, ver, len, crc16} so corruption, truncation
 * and version skew are rejected instead of loaded.
 *
 * Layout note: payloads are PODs with ≤4-byte-aligned members only (the
 * occurrence key uses day:u32 + min:u16, not u64), so GCC produces identical
 * bytes on x86-64 host, Xtensa S3 and RISC-V C3 — memcpy blobs are safe.
 */
#ifndef AIO_HUB_STORE_H
#define AIO_HUB_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../lib/proto/crc16.h"

namespace aio {

#define STORE_MAGIC 0xA10F

class IHubStore {
 public:
  virtual ~IHubStore() = default;
  virtual bool put(const char *key, const uint8_t *data, size_t len) = 0;
  /* Returns payload length, 0 if missing. */
  virtual size_t get(const char *key, uint8_t *out, size_t max_len) = 0;
  virtual bool del(const char *key) = 0;
};

struct StoreHdr {
  uint16_t magic;
  uint16_t ver;
  uint16_t len;
  uint16_t crc; /* over len payload bytes */
};

static inline bool store_put(IHubStore &s, const char *key, uint16_t ver,
                             const void *data, uint16_t len) {
  if (len == 0 || len > 4096) return false;
  StoreHdr h{STORE_MAGIC, ver, len,
             crc16_ccitt(0xFFFF, (const uint8_t *)data, len)};
  /* single key holding header ++ payload keeps device writes atomic */
  uint8_t blob[sizeof(StoreHdr) + 4096];
  memcpy(blob, &h, sizeof(h));
  memcpy(blob + sizeof(h), data, len);
  return s.put(key, blob, sizeof(h) + len);
}

/* Returns payload length, or -1 on missing/corrupt/version-skew. */
static inline int store_get(IHubStore &s, const char *key, uint16_t ver,
                            void *out, uint16_t max_len) {
  uint8_t blob[sizeof(StoreHdr) + 4096];
  size_t n = s.get(key, blob, sizeof(blob));
  if (n < sizeof(StoreHdr)) return -1;
  StoreHdr h;
  memcpy(&h, blob, sizeof(h));
  if (h.magic != STORE_MAGIC || h.ver != ver) return -1;
  if (h.len == 0 || h.len > max_len || n < sizeof(StoreHdr) + h.len) return -1;
  if (h.crc != crc16_ccitt(0xFFFF, blob + sizeof(StoreHdr), h.len)) return -1;
  memcpy(out, blob + sizeof(StoreHdr), h.len);
  return (int)h.len;
}

}  // namespace aio

#endif /* AIO_HUB_STORE_H */
