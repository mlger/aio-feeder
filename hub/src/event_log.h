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
};

constexpr uint8_t EVENT_RESULT_NO_LINK = 0xFF;

class EventLog {
 public:
  static constexpr uint16_t CAP = 128;

  void push(uint32_t unix, uint8_t node, uint8_t kind, uint8_t result,
            uint32_t feed_id, uint16_t amount, int16_t aux = 0) {
    HubEvent &e = ring_[head_];
    e.unix = unix;
    e.node = node;
    e.kind = kind;
    e.result = result;
    e.feed_id = feed_id;
    e.amount = amount;
    e.aux = aux;
    head_ = (uint16_t)((head_ + 1) % CAP);
    if (count_ < CAP) count_++;
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
  HubEvent ring_[CAP]{};
  uint16_t head_ = 0;
  uint16_t count_ = 0;
};

}  // namespace aio

#endif /* AIO_EVENT_LOG_H */
