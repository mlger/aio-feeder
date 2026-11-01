/*
 * AIO Feeder — fallback schedule helpers (PLAN.md §9.6, §9.9, SR10).
 * Pure logic, host-tested. Storage lives in the node config blob; execution
 * (which needs valid time per SR7) is wired by the node runtime.
 */
#ifndef AIO_SCHEDULE_H
#define AIO_SCHEDULE_H

#include <stdint.h>
#include "../lib/proto/messages.h"

namespace aio {

/* days_mask bit order = struct tm tm_wday: bit0 Sunday … bit6 Saturday. */
constexpr uint8_t SCHED_ALL_DAYS = 0x7F;

static inline bool sched_day_due(uint8_t days_mask, uint8_t wday) {
  if (days_mask == 0) return false;
  return (days_mask >> (wday % 7)) & 1;
}

/* Is this entry due at min_of_day on weekday wday? Exact-minute match. */
static inline bool sched_entry_due(const sched_entry_t &e, uint16_t min_of_day,
                                   uint8_t wday) {
  return e.min_of_day == min_of_day && sched_day_due(e.days_mask, wday);
}

/* §9.9: skip_until (SKIPPED_USER) and hold_until (SKIPPED_HOLD) block
 * scheduled feeds until the unix second passes. Manual feeds are never
 * blocked — the caller decides. */
enum class SchedGate { Run, SkippedUser, SkippedHold };

static inline SchedGate schedule_gate(uint32_t unix_now, uint32_t skip_until,
                                      uint32_t hold_until) {
  if (hold_until && unix_now < hold_until) return SchedGate::SkippedHold;
  if (skip_until && unix_now < skip_until) return SchedGate::SkippedUser;
  return SchedGate::Run;
}

/* §9.6 freshness: a fallback schedule is valid for 7 days after push. */
constexpr uint32_t SCHEDULE_FRESHNESS_S = 7u * 24u * 3600u;

}  // namespace aio

#endif /* AIO_SCHEDULE_H */
