/*
 * AIO Feeder — node-firmware store metadata (M8b, PLAN §7.6, FMEA #11).
 *
 * The hub keeps ONE uploaded node image ("nfw" flash partition, raw bytes)
 * plus this small "nfwmeta" blob: what the image is (target/version/sha256)
 * and what each node last reported (MCU + release version from the HELLO
 * extension, last ENTER_OTA ack). Kept out of the registry (D36 pattern) so
 * registry layout changes never orphan pairing data.
 *
 * Pure + host-testable: no MCU headers. Partition I/O and HTTP live in
 * remote_net.cpp; main.cpp feeds HELLO updates through nodefw_hello().
 */
#ifndef AIO_NODEFW_H
#define AIO_NODEFW_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hub_registry.h"
#include "hub_store.h"

namespace aio {

/* enum, not #define: call sites use aio::NFW_TARGET_* — macros ignore
 * namespaces and expand to `aio::1` (caught by CI, M8b build). */
enum : uint8_t {
  NFW_TARGET_NONE = 0,
  NFW_TARGET_8266 = 1,
  NFW_TARGET_C3 = 2,
};

/* Image metadata — mirrors the first bytes the upload handler computes. */
struct NodeFwImage {
  uint32_t magic; /* NFW_IMAGE_MAGIC when a real image is stored */
  uint32_t size;  /* image bytes in the partition */
  uint8_t target; /* NFW_TARGET_* */
  char ver[16];   /* release string, e.g. "0.12.0" */
  uint8_t sha[32];
};

struct NodeFwNode {
  uint8_t node_id;
  uint8_t mcu;         /* MCU_* (HELLO extension, 0 = unknown) */
  uint32_t rel_ver;    /* AIO_NODE_FW_VER of the node, 0 = unknown */
  uint8_t ack_ok;      /* last ENTER_OTA_ACK, 0xFF = none yet */
  uint8_t ack_reason;
  uint32_t push_unix;  /* last ENTER_OTA sent, 0 = never */
};

struct NodeFwData {
  uint16_t version;
  NodeFwImage img;
  NodeFwNode node[HUB_MAX_NODES];
};

#define NFW_META_VERSION 1
constexpr uint32_t NFW_IMAGE_MAGIC = 0xA10F8100u; /* same reason: aio::-qualified use */
#define NFW_MAX_IMAGE (1024u * 1024u) /* "nfw" partition size (partitions.csv) */

class NodeFw {
 public:
  void reset() {
    memset(&d_, 0, sizeof(d_));
    d_.version = NFW_META_VERSION;
  }
  NodeFw() { reset(); }

  bool load(IHubStore &s) {
    NodeFwData d;
    if (store_get(s, "nfwmeta", NFW_META_VERSION, &d, sizeof(d)) != (int)sizeof(d))
      return false;
    d_ = d;
    return true;
  }
  bool save(IHubStore &s) const { return store_put(s, "nfwmeta", NFW_META_VERSION, &d_, sizeof(d_)); }

  const NodeFwImage &img() const { return d_.img; }
  void img_set(const NodeFwImage &i) { d_.img = i; }
  void img_clear() { memset(&d_.img, 0, sizeof(d_.img)); }
  bool img_valid() const { return d_.img.magic == NFW_IMAGE_MAGIC && d_.img.size > 0; }

  /* HELLO extension (M8b): remember what the node runs. */
  void hello(uint8_t node_id, uint32_t rel_ver, uint8_t mcu) {
    NodeFwNode *n = slot(node_id, true);
    if (!n) return;
    n->rel_ver = rel_ver;
    n->mcu = mcu;
  }
  /* ENTER_OTA_ACK from the node. */
  void ack(uint8_t node_id, uint8_t ok, uint8_t reason) {
    NodeFwNode *n = slot(node_id, true);
    if (!n) return;
    n->ack_ok = ok;
    n->ack_reason = reason;
  }
  void pushed(uint8_t node_id, uint32_t unix) {
    NodeFwNode *n = slot(node_id, true);
    if (!n) return;
    n->push_unix = unix;
    n->ack_ok = 0xFF;
    n->ack_reason = 0;
  }
  void node_removed(uint8_t node_id) {
    for (uint8_t i = 0; i < HUB_MAX_NODES; i++)
      if (d_.node[i].node_id == node_id) memset(&d_.node[i], 0, sizeof(d_.node[i]));
  }
  const NodeFwNode *node(uint8_t node_id) const {
    for (uint8_t i = 0; i < HUB_MAX_NODES; i++)
      if (d_.node[i].node_id == node_id) return &d_.node[i];
    return nullptr;
  }

 private:
  NodeFwNode *slot(uint8_t node_id, bool create) {
    for (uint8_t i = 0; i < HUB_MAX_NODES; i++)
      if (d_.node[i].node_id == node_id) return &d_.node[i];
    if (!create) return nullptr;
    for (uint8_t i = 0; i < HUB_MAX_NODES; i++) {
      if (!d_.node[i].node_id) {
        memset(&d_.node[i], 0, sizeof(d_.node[i]));
        d_.node[i].node_id = node_id;
        return &d_.node[i];
      }
    }
    return nullptr;
  }

  NodeFwData d_;
};

/* --- shared helpers (host tests cover these) -------------------------------- */

static inline const char *nodefw_target_name(uint8_t t) {
  switch (t) {
    case NFW_TARGET_8266: return "esp8266";
    case NFW_TARGET_C3: return "esp32c3";
    default: return "none";
  }
}

/* 0x000C00 → "0.12.0"; 0 → "?" (never flashed a versioned build). */
static inline void nodefw_ver_str(uint32_t v, char *out, size_t cap) {
  if (!v) { snprintf(out, cap, "?"); return; }
  snprintf(out, cap, "%u.%u.%u", (unsigned)((v >> 16) & 0xFF),
           (unsigned)((v >> 8) & 0xFF), (unsigned)(v & 0xFF));
}

/* Upload validation: a plausible node image for a known target. */
static inline bool nodefw_image_plausible(uint32_t size, uint8_t target) {
  if (target != NFW_TARGET_8266 && target != NFW_TARGET_C3) return false;
  return size >= 1024 && size <= NFW_MAX_IMAGE;
}

/* sha256 as lowercase hex (64 chars + NUL). */
static inline void nodefw_sha_hex(const uint8_t sha[32], char *out, size_t cap) {
  static const char *hex = "0123456789abcdef";
  size_t o = 0;
  for (int i = 0; i < 32 && o + 2 < cap; i++) {
    out[o++] = hex[sha[i] >> 4];
    out[o++] = hex[sha[i] & 0xF];
  }
  out[o] = 0;
}

}  // namespace aio
#endif /* AIO_NODEFW_H */
