/*
 * AIO Feeder — hub event log (PLAN.md §7.2 event_log, §9.2 result codes).
 *
 * RAM ring, host-tested. The append-only SD/history log is M4 (§7.5: feeding
 * never depends on it); this ring is what the reboot-dedup and "what
 * happened" queries use until then.
 *
 * Result dedup (§9.7 no double counting): has_result(feed_id) answers whether
 * a terminal event for a feed already exists, so a repeated FEED_RESULT
 * (node repeats 3×) or a result replayed after a reconnect cannot be counted
 * twice.
 */
#ifndef AIO_EVENT_LOG_H
#define AIO_EVENT_LOG_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace aio {

enum : uint8_t {
  EV_FEED_OK = 0,
  EV_FEED_FAIL,        /* node ran it, reported failure */
  EV_FEED_REJ,         /* node refused (FEED_ACK accepted=0, reason in result) */
  EV_FEED_UNKNOWN,     /* §6.6: retries exhausted or reboot mid-feed — never re-issued */
  EV_SKIPPED_USER,     /* §9.9 skip_until */
  EV_SKIPPED_HOLD,     /* §9.9 hold mode */
  EV_SKIPPED_MISSED,   /* §9.4 missed, outside catch-up (aux = occurrence count) */
  EV_CATCHUP,          /* §9.4 catch-up-once feed issued */
  EV_KIND_COUNT
};

/* result: FEED_OK / FEED_REJ_* / FEED_ABORTED / 0xFF = hub-side "no link"
 * (send failed — node deterministically never got the command). */
struct HubEvent {
  uint32_t unix;
  uint32_t feed_id;
  uint16_t amount;
  int16_t aux;     /* pulses, occurrence count, … */
  uint8_t node;
  uint8_t kind;
  uint8_t result;
  uint8_t src;     /* EV_SRC_* (RAM ring only — never persisted) */
};

constexpr uint8_t EVENT_RESULT_NO_LINK = 0xFF;

/* What asked for the feed (§9.12 CSV "source" column). Values mirror
 * FeedKind (Scheduled 0 / CatchUp 1 / Manual 2) so feed_manager can pass
 * s.job.kind straight through; skip/missed rows are hub-side scheduled
 * decisions and keep EV_SRC_SCHED. */
constexpr uint8_t EV_SRC_SCHED = 0;
constexpr uint8_t EV_SRC_CATCHUP = 1;
constexpr uint8_t EV_SRC_MANUAL = 2;

static inline const char *ev_src_name(uint8_t src) {
  switch (src) {
    case EV_SRC_CATCHUP: return "catchup";
    case EV_SRC_MANUAL: return "manual";
    default: return "scheduled";
  }
}

class EventLog {
 public:
  static constexpr uint16_t CAP = 128;

  /* Optional observer (M6 remote glue): fires once per push with the event,
   * before it becomes visible via at(). Runs in the caller's context — the
   * push sites hold the engine state mutex; keep the callback non-blocking. */
  void (*on_push)(const HubEvent &e) = nullptr;

  void push(uint32_t unix, uint8_t node, uint8_t kind, uint8_t result,
            uint32_t feed_id, uint16_t amount, int16_t aux = 0,
            uint8_t src = EV_SRC_SCHED) {
    HubEvent &e = ring_[head_];
    e.unix = unix;
    e.node = node;
    e.kind = kind;
    e.result = result;
    e.feed_id = feed_id;
    e.amount = amount;
    e.aux = aux;
    e.src = src;
    head_ = (uint16_t)((head_ + 1) % CAP);
    if (count_ < CAP) count_++;
    if (on_push) on_push(e);
  }

  /* §7.5 retention: drop entries older than `days` (settings screen,
   * 7/30/90). Feeding never depends on this; the RAM ring self-evicts at
   * CAP anyway. Returns the number of dropped entries. */
  uint16_t prune_older_than(uint32_t unix_now, uint8_t days) {
    if (!days || unix_now < 86400u * days) return 0;
    const uint32_t cutoff = unix_now - 86400u * days;
    const uint16_t start = (uint16_t)((head_ + CAP - count_) % CAP);
    uint16_t w = 0; /* logical write index — compaction keeps ring order */
    for (uint16_t r = 0; r < count_; r++) {
      const HubEvent e = ring_[(start + r) % CAP]; /* copy first: may alias */
      if (e.unix < cutoff) continue;
      if (w != r) ring_[(start + w) % CAP] = e;
      w++;
    }
    const uint16_t dropped = (uint16_t)(count_ - w);
    count_ = w;
    head_ = (uint16_t)((start + w) % CAP);
    return dropped;
  }

  /* 0 = oldest … count-1 = newest. */
  const HubEvent *at(uint16_t i) const {
    if (i >= count_) return nullptr;
    uint16_t start = (uint16_t)((head_ + CAP - count_) % CAP);
    return &ring_[(start + i) % CAP];
  }

  uint16_t count() const { return count_; }

  bool has_result(uint32_t feed_id) const {
    for (uint16_t i = 0; i < count_; i++) {
      const HubEvent *e = at(i);
      if (e->feed_id == feed_id &&
          (e->kind == EV_FEED_OK || e->kind == EV_FEED_FAIL ||
           e->kind == EV_FEED_REJ || e->kind == EV_FEED_UNKNOWN))
        return true;
    }
    return false;
  }

  uint16_t count_of(uint8_t kind) const {
    uint16_t n = 0;
    for (uint16_t i = 0; i < count_; i++)
      if (at(i)->kind == kind) n++;
    return n;
  }

 private:
  uint16_t start_index() const { return (uint16_t)((head_ + CAP - count_) % CAP); }

  HubEvent ring_[CAP]{};
  uint16_t head_ = 0;
  uint16_t count_ = 0;
};

}  // namespace aio

#endif /* AIO_EVENT_LOG_H */
