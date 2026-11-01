/*
 * AIO Feeder — persisted node configuration (PLAN.md §7.5).
 * One blob, two A/B slots with CRC (config_slots.h); write_seq first so
 * slot_select() can pick the newer intact copy.
 */
#ifndef AIO_NODE_CONFIG_H
#define AIO_NODE_CONFIG_H

#include "../../lib/proto/profile.h"
#include "../../node_core/config_slots.h"
#include "../../node_core/feed_journal.h"
#include "../../node_core/node_limits.h"
#include "../../node_core/time_validity.h"

#define CONFIG_SLOT_SIZE 512
#define CONFIG_MAGIC 0xA10FEED1u

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
