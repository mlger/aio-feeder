/*
 * AIO Feeder — hub pending-unpair queue (PLAN.md FMEA #15, §12 M7).
 *
 * Removing a node that is currently offline must not leave it paired: it
 * would keep running its fallback schedule and obeying commands forever.
 * The hub therefore parks the node's mac + per-peer key + tx seq in this
 * queue (own CRC'd store blob "unp" — a v0.9→v0.10 upgrade must not wipe
 * the registry, D36 pattern) and sends a keyed UNPAIR (MSG_UNPAIR) every
 * time the node shows up again (heartbeat, HELLO or PAIR_REQ). When the
 * node announces itself unpaired (HELLO state=0 / PAIR_REQ — it re-beacons
 * after clearing), the entry is dropped.
 *
 * Pure logic, host-tested. The engine sends by explicit mac + key
 * (LinkEngine::send_unpair) — no registry peer entry needed.
 */
#ifndef AIO_UNPAIR_QUEUE_H
#define AIO_UNPAIR_QUEUE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../lib/proto/proto.h"
#include "hub_store.h"

namespace aio {

#define UNPAIR_VERSION 1
constexpr uint8_t UNPAIR_MAX = 8; /* = HUB_MAX_NODES (kept independent on purpose) */

struct PendingUnpair {
  uint8_t id;            /* the node id it had in the registry (display only) */
  uint8_t mac[6];
  uint8_t key[32];
  uint8_t key_len;
  uint8_t channel;
  uint16_t tx_seq;       /* next seq to send with (persisted — no replays) */
  uint32_t since_unix;   /* when the removal was parked (0 = unknown) */
  uint16_t sends;        /* UNPAIR frames sent so far (visibility) */
};

class UnpairQueue {
 public:
  bool load(IHubStore &s) {
    return store_get(s, "unp", UNPAIR_VERSION, &d_, sizeof(d_)) == (int)sizeof(d_);
  }
  bool save(IHubStore &s) const {
    return store_put(s, "unp", UNPAIR_VERSION, &d_, sizeof(d_));
  }

  bool add(uint8_t id, const uint8_t mac[6], const uint8_t *key, uint8_t key_len,
           uint8_t channel, uint16_t tx_seq, uint32_t unix_now) {
    if (find(mac)) return true; /* already parked */
    if (d_.count >= UNPAIR_MAX) return false;
    PendingUnpair &p = d_.items[d_.count++];
    memset(&p, 0, sizeof(p));
    p.id = id;
    memcpy(p.mac, mac, 6);
    if (key && key_len) {
      memcpy(p.key, key, key_len > 32 ? 32 : key_len);
      p.key_len = key_len > 32 ? 32 : key_len;
    }
    p.channel = channel;
    p.tx_seq = tx_seq;
    p.since_unix = unix_now > 1600000000 ? (uint32_t)unix_now : 0;
    return true;
  }

  PendingUnpair *find(const uint8_t mac[6]) {
    for (uint8_t i = 0; i < d_.count; i++)
      if (!memcmp(d_.items[i].mac, mac, 6)) return &d_.items[i];
    return nullptr;
  }
  const PendingUnpair *find(const uint8_t mac[6]) const {
    return const_cast<UnpairQueue *>(this)->find(mac);
  }

  /* The node re-beaconed unpaired (HELLO state=0 / PAIR_REQ): done. */
  bool drop(const uint8_t mac[6]) {
    for (uint8_t i = 0; i < d_.count; i++) {
      if (!memcmp(d_.items[i].mac, mac, 6)) {
        for (uint8_t j = i; j + 1 < d_.count; j++) d_.items[j] = d_.items[j + 1];
        d_.count--;
        return true;
      }
    }
    return false;
  }

  uint8_t count() const { return d_.count; }
  const PendingUnpair *at(uint8_t i) const {
    return i < d_.count ? &d_.items[i] : nullptr;
  }

 private:
  struct Blob {
    uint16_t version;
    uint8_t count;
    uint8_t pad;
    PendingUnpair items[UNPAIR_MAX];
  };
  Blob d_{};
};

}  // namespace aio
#endif /* AIO_UNPAIR_QUEUE_H */
