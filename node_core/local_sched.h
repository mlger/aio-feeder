/*
 * AIO Feeder — node-side fallback schedule execution (PLAN.md §9.6, §12 M7).
 *
 * The hub pushes the schedule (SCHEDULE_SET, persisted in the node config,
 * with skip_until/hold_until §9.9). When the hub is unreachable the node
 * still feeds: this module decides whether an entry is due right now.
 *
 * Rules (§9.6, SR7, SR10):
 *   - only while paired (unpair also clears the stored schedule);
 *   - only with valid time (SR7) — the caller passes its TimeValidity result;
 *   - only within the freshness window: schedule pushed ≤ 7 days ago;
 *   - skip_until / hold_until gate runs exactly like the hub (SKIPPED_USER /
 *     SKIPPED_HOLD); manual feeds are never affected;
 *   - exact-minute match, no node-side catch-up: an occurrence missed while
 *     the node was down is reconciled by the hub on return (§9.4/§9.6).
 *
 * Dedup: a run is keyed by a feed_id derived from (local day, minute, entry)
 * with the top bit set — disjoint from the sequential hub/node id ranges, so
 * the feed journal (SR6) also dedups fallback runs across reboots and can
 * never collide with a hub-issued feed.
 *
 * Pure logic, host-tested (test_node_core). The node runtime polls this once
 * per local minute and hands the result to the same request_feed() path a
 * radio feed takes — caps, cooldown, journal and results are identical.
 */
#ifndef AIO_LOCAL_SCHED_H
#define AIO_LOCAL_SCHED_H

#include <stdint.h>

#include "../hub/src/hub_time.h"
#include "../lib/proto/messages.h"
#include "schedule.h"

namespace aio {

/* Node-originated feed ids: top bit set. Hub ids start at 1000 and grow
 * sequentially — the ranges stay disjoint for any realistic lifetime. */
constexpr uint32_t LOCAL_FEED_ID_MARK = 0x80000000u;

/* dayno: 14 bits (≈ 44.9 y wrap — the journal ring forgets far sooner),
 * min_of_day: 11 bits, entry: 6 bits. */
static inline uint32_t local_sched_feed_id(int64_t day_start, uint16_t min_of_day,
                                           uint8_t entry) {
  const uint32_t dayno = (uint32_t)((day_start / 86400) & 0x3FFF);
  return LOCAL_FEED_ID_MARK | (dayno << 17) | ((uint32_t)(min_of_day & 0x7FF) << 6) |
         (entry & 0x3F);
}

struct LocalDue {
  uint8_t entry;      /* index into the stored entries */
  uint16_t amount;    /* in the profile's unit */
  uint32_t feed_id;   /* journal-stable dedup id (LOCAL_FEED_ID_MARK) */
};

enum class LocalSchedState {
  Idle,        /* nothing due this minute */
  Run,         /* *out is due — request the feed */
  SkippedUser, /* due but inside skip_until (§9.9) */
  SkippedHold, /* due but inside hold_until (§9.9) */
  NotFresh,    /* schedule copy older than the §9.6 window */
  NotTrusted,  /* unpaired or time not valid (SR7) */
};

/* One call per local minute (caller tracks minute changes). Returns at most
 * one due entry — one actuation at a time is the node mutex anyway. */
static inline LocalSchedState local_sched_poll(const sched_entry_t *entries,
                                               uint8_t count, uint8_t tz_rule_id,
                                               uint32_t pushed_unix,
                                               uint32_t skip_until,
                                               uint32_t hold_until,
                                               bool paired, bool time_ok,
                                               int64_t unix_now, LocalDue *out) {
  if (!paired || !time_ok || !entries || count == 0) return LocalSchedState::NotTrusted;
  if (pushed_unix == 0 || unix_now < (int64_t)pushed_unix ||
      unix_now - (int64_t)pushed_unix > (int64_t)SCHEDULE_FRESHNESS_S)
    return LocalSchedState::NotFresh;

  const LocalTime now = local_from_unix(unix_now, tz_rule_id);
  for (uint8_t e = 0; e < count; e++) {
    const sched_entry_t &en = entries[e];
    if (en.min_of_day >= 1440) continue;
    if (!sched_entry_due(en, now.min_of_day, now.wday)) continue;
    /* Occurrence keys are local-day based; the gate uses the estimated real
     * unix (±1 h around DST jumps — same approximation the hub uses). */
    const SchedGate gate = schedule_gate(
        occ_unix_est(now.day_start, en.min_of_day, tz_rule_id, unix_now), skip_until,
        hold_until);
    if (gate == SchedGate::SkippedUser) return LocalSchedState::SkippedUser;
    if (gate == SchedGate::SkippedHold) return LocalSchedState::SkippedHold;
    out->entry = e;
    out->amount = en.amount;
    out->feed_id = local_sched_feed_id(now.day_start, en.min_of_day, e);
    return LocalSchedState::Run;
  }
  return LocalSchedState::Idle;
}

}  // namespace aio
#endif /* AIO_LOCAL_SCHED_H */
