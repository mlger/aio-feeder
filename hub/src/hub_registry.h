/*
 * AIO Feeder — hub node registry + per-node schedule + occurrence state
 * (PLAN.md §7.2 node_registry / profile_store / schedule_engine storage).
 *
 * Pure logic, host-tested. One persisted blob ("reg") carries everything the
 * hub needs across a reboot: peers (mac/key/channel), profiles, schedules
 * (§9.6/§9.9) and each node's occurrence bookkeeping — last_done[i] is the
 * newest occurrence (local day + minute) already handled (fired, skipped or
 * caught up), so a reboot can never double-fire a schedule.
 *
 * Layout: all members ≤4-byte aligned so host and both MCUs serialize
 * identically (see hub_store.h).
 */
#ifndef AIO_HUB_REGISTRY_H
#define AIO_HUB_REGISTRY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../lib/proto/messages.h"
#include "../../lib/proto/profile.h"
#include "hub_store.h"
#include "hub_time.h"

namespace aio {

constexpr uint8_t HUB_MAX_NODES = 8;
constexpr uint8_t HUB_SCHED_MAX_ENTRIES = 16;
#define REG_VERSION 1

/* Per-node schedule (mirror of the SCHEDULE_SET we push, §9.6/§9.9). */
struct HubSched {
  uint16_t version;
  uint8_t entry_count;
  uint8_t enabled;
  uint32_t skip_until;  /* unix; occurrences before → SKIPPED_USER */
  uint32_t hold_until;  /* unix; occurrences before → SKIPPED_HOLD */
  uint32_t pushed_at;   /* unix of last push (freshness display) */
  sched_entry_t entries[HUB_SCHED_MAX_ENTRIES];
};

/* Occurrence bookkeeping (persisted inside the registry blob). */
struct OccRef {
  uint32_t day_start; /* local-space midnight (hub_time.h) */
  uint16_t min_of_day;
};
static inline bool occ_ref_le(const OccRef &a, const OccRef &b) {
  if (a.day_start != b.day_start) return a.day_start < b.day_start;
  return a.min_of_day <= b.min_of_day;
}

struct SchedRunState {
  OccRef last_done[HUB_SCHED_MAX_ENTRIES]; /* newest handled occurrence */
};

struct HubNode {
  uint8_t id;
  uint8_t mac[6];
  uint8_t key[32];
  uint8_t key_len;
  uint8_t channel;
  node_profile_t profile;
  HubSched sched;
  SchedRunState run;
};

struct RegistryData {
  uint16_t version;
  uint8_t count;
  uint8_t next_node_id;
  uint16_t sched_version;
  uint8_t tz_rule;
  uint8_t stagger_max;   /* §9.5: max solenoids energised at once, default 1 */
  uint32_t feed_id_seq;
  HubNode nodes[HUB_MAX_NODES];
};

class HubRegistry {
 public:
  void reset() {
    memset(&d_, 0, sizeof(d_));
    d_.version = REG_VERSION;
    d_.stagger_max = 1;
    d_.next_node_id = 1;
    d_.feed_id_seq = 1000;
    d_.sched_version = 1;
  }

  HubRegistry() { reset(); }

  bool load(IHubStore &s) {
    RegistryData d;
    if (store_get(s, "reg", REG_VERSION, &d, sizeof(d)) != (int)sizeof(d))
      return false;
    if (d.count > HUB_MAX_NODES) return false;
    d_ = d;
    return true;
  }

  bool save(IHubStore &s) const {
    return store_put(s, "reg", REG_VERSION, &d_, sizeof(d_));
  }

  HubNode *add(const uint8_t mac[6], uint8_t channel) {
    if (d_.count >= HUB_MAX_NODES) return nullptr;
    if (by_mac(mac)) return nullptr;
    HubNode *n = free_slot();
    if (!n) return nullptr;
    memset(n, 0, sizeof(*n));
    n->id = d_.next_node_id++;
    memcpy(n->mac, mac, 6);
    n->channel = channel;
    n->profile = profile_default();
    n->sched.enabled = 1;
    n->sched.version = d_.sched_version;
    d_.count++;
    return n;
  }

  bool remove(uint8_t node_id) {
    HubNode *n = by_id(node_id);
    if (!n) return false;
    memset(n, 0, sizeof(*n));
    d_.count--;
    return true;
  }

  HubNode *by_id(uint8_t id) {
    if (id == 0) return nullptr;
    for (int i = 0; i < HUB_MAX_NODES; i++)
      if (d_.nodes[i].id == id) return &d_.nodes[i];
    return nullptr;
  }
  const HubNode *by_id(uint8_t id) const {
    return const_cast<HubRegistry *>(this)->by_id(id);
  }

  HubNode *by_mac(const uint8_t mac[6]) {
    for (int i = 0; i < HUB_MAX_NODES; i++)
      if (d_.nodes[i].id && !memcmp(d_.nodes[i].mac, mac, 6))
        return &d_.nodes[i];
    return nullptr;
  }

  HubNode *node(int index) {
    return (index >= 0 && index < HUB_MAX_NODES) ? &d_.nodes[index] : nullptr;
  }
  const HubNode *node(int index) const {
    return (index >= 0 && index < HUB_MAX_NODES) ? &d_.nodes[index] : nullptr;
  }
  uint8_t count() const { return d_.count; }

  RegistryData &data() { return d_; }
  const RegistryData &data() const { return d_; }

 private:
  HubNode *free_slot() {
    for (int i = 0; i < HUB_MAX_NODES; i++)
      if (d_.nodes[i].id == 0) return &d_.nodes[i];
    return nullptr;
  }
  RegistryData d_;
};

}  // namespace aio

#endif /* AIO_HUB_REGISTRY_H */
