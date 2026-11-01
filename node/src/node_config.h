/*
 * AIO Feeder — persisted node configuration (PLAN.md §7.5).
 * One blob, two A/B slots with CRC (config_slots.h); write_seq first so
 * slot_select() can pick the newer intact copy.
 *
 * M2 adds the link identity (hub MAC + key + channel + persisted tx_seq, so
 * replay windows survive reboots) and the pushed fallback schedule with its
 * skip/hold windows (§9.6, §9.9). Layout change → CONFIG_MAGIC bumped: bench
 * nodes re-default on first boot after upgrade (journal/counters reset).
 */
#ifndef AIO_NODE_CONFIG_H
#define AIO_NODE_CONFIG_H

#include "../../lib/proto/messages.h"
#include "../../lib/proto/profile.h"
#include "../../node_core/config_slots.h"
#include "../../node_core/feed_journal.h"
#include "../../node_core/node_limits.h"
#include "../../node_core/schedule.h"
#include "../../node_core/time_validity.h"

#define CONFIG_SLOT_SIZE 512
#define CONFIG_MAGIC 0xA10FEED2u

/* Node-side store cap: a 7-day schedule needs ≤ 32 entries; frames may carry
 * more (SCHEDULE_MAX_ENTRIES) but a node refuses the overflow. */
#define SCHEDULE_STORE_MAX 32

struct NodeConfig {
  uint32_t write_seq;          /* slot_select() recency (first 4 bytes) */
  uint32_t magic;
  uint8_t node_id;
  uint8_t manual_stop;         /* SR9 latch */
  uint8_t limits_confirmed;    /* SR8 flag */
  uint8_t booted;              /* 0xFF on first ever boot */
  uint32_t feed_id_seq;        /* monotonic generator for CLI/bench feeds */
  int64_t sync_unix;           /* last hub time sync (SR7 RTC path) */
  node_profile_t profile;
  aio::UsageCounters counters;
  aio::JournalEntry journal[AIO_JOURNAL_SIZE];
  uint32_t journal_head;

  /* --- M2 link identity (§6.2–§6.4) --- */
  uint8_t paired;              /* 0 = unpaired (beacons HELLO), 1 = paired */
  uint8_t chan;                /* locked ESP-NOW channel (§6.2 scan) */
  uint8_t hub_mac[6];
  uint8_t hub_key[32];         /* per-node HMAC key (§6.4, D13) */
  uint8_t hub_key_len;
  uint8_t reserved0;
  uint16_t tx_seq;             /* persisted; resumed +32 on boot */
  int32_t rtc_drift_ms;        /* measured at last TIME_SYNC (§6.5 HB) */

  /* --- M2 fallback schedule (§9.6) --- */
  uint16_t sched_version;
  uint8_t sched_count;
  uint8_t sched_pad;
  uint32_t skip_until;         /* SKIPPED_USER window end (§9.9) */
  uint32_t hold_until;         /* SKIPPED_HOLD window end (§9.9) */
  uint32_t sched_pushed_unix;  /* freshness window anchor (§9.6) */
  sched_entry_t sched_entries[SCHEDULE_STORE_MAX];
};

/* Raw slot = payload + crc16 (LE). Payload is the struct, no packing games. */
constexpr size_t kConfigPayload = sizeof(NodeConfig);
static_assert(kConfigPayload + 2 <= CONFIG_SLOT_SIZE, "config blob exceeds slot");

/* Layout inside the EEPROM/NVS region: [slot A][slot B]. */
constexpr size_t kStorageRegion = CONFIG_SLOT_SIZE * 2;

static inline void config_fill_slot(uint8_t *slot, const NodeConfig &cfg) {
  memset(slot, 0xFF, CONFIG_SLOT_SIZE);
  memcpy(slot, &cfg, sizeof(cfg));
  aio::slot_fill_crc(slot, kConfigPayload);
}

static inline bool config_load_slot(const uint8_t *region, NodeConfig *out) {
  const uint8_t *slot = aio::slot_select(region, region + CONFIG_SLOT_SIZE, kConfigPayload);
  if (!slot) return false;
  memcpy(out, slot, sizeof(*out));
  return out->magic == CONFIG_MAGIC;
}

#endif /* AIO_NODE_CONFIG_H */
