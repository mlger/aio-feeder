/*
 * AIO Feeder — hub feed manager (PLAN.md §7.2, §6.6, §9.1, §9.5).
 *
 * The SINGLE module permitted to originate MSG_FEED. One in-flight feed per
 * node, at most `stagger_max` nodes energised hub-wide at once (§9.5, default
 * 1). State machine per in-flight feed:
 *
 *   CREATED → SENT → ACKED → RESULT_OK | RESULT_FAIL
 *                └──────────────► TIMEOUT_UNKNOWN   (§6.6: retries exhausted —
 *                                                    alert, never blind re-issue)
 *   FEED_ACK accepted=0 → RESULT_FAIL (terminal, reason = FEED_REJ_*)
 *
 * Write-ahead intent: the PendingIntent blob is persisted before the FEED
 * frame goes out and cleared on the terminal transition. After a reboot a
 * still-present intent means the hub died mid-feed → FEED_UNKNOWN event
 * (outcome unknowable; the node's journal dedup + §9.4 reconciliation covers
 * the rest). This is the "no blind re-issue" guarantee across reboots.
 *
 * Pure logic, host-tested. IHubLink decouples it from LinkEngine.
 */
#ifndef AIO_FEED_MANAGER_H
#define AIO_FEED_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../lib/proto/messages.h"
#include "../../lib/proto/proto.h"
#include "event_log.h"
#include "hub_registry.h"
#include "hub_store.h"

namespace aio {

enum class FeedKind : uint8_t { Scheduled = 0, CatchUp = 1, Manual = 2 };

struct FeedJob {
  uint32_t feed_id;
  uint8_t node;
  uint16_t amount;
  uint8_t unit;
  uint8_t kind; /* FeedKind */
};

struct PendingIntent {
  uint16_t ver;
  uint32_t feed_id;
  uint16_t amount;
  uint8_t node;
  uint8_t kind;
};
#define PENDING_VERSION 1

struct IHubLink {
  virtual ~IHubLink() = default;
  virtual bool send_feed(uint8_t node, const feed_payload_t &f, uint32_t now_ms) = 0;
};

class FeedManager {
 public:
  void begin(IHubLink *link, IHubStore *store, EventLog *log, uint8_t stagger_max) {
    link_ = link;
    store_ = store;
    log_ = log;
    stagger_ = stagger_max ? stagger_max : 1;
    inflight_ = 0;
    memset(slots_, 0, sizeof(slots_));
    q_head_ = q_tail_ = 0;
  }

  uint8_t stagger_max() const { return stagger_; }
  void set_stagger(uint8_t n) { stagger_ = n ? n : 1; }

  /* False if the node already has a queued or in-flight feed (§9.5 mutex).
   * The WAL intent is durable from this moment: a crash before the FEED
   * frame ever leaves also surfaces as FEED_UNKNOWN after reboot. */
  bool submit(const FeedJob &j) {
    if (j.node == 0 || node_busy(j.node)) return false;
    PendingIntent p{PENDING_VERSION, j.feed_id, j.amount, j.node, j.kind};
    store_put(*store_, pend_key_for(j.node), PENDING_VERSION, &p, sizeof(p));
    queue_[q_tail_ % HUB_MAX_NODES] = j;
    q_tail_++;
    return true;
  }

  /* Pops queued feeds while stagger capacity allows; persists intent, sends. */
  void tick(uint32_t now_ms, uint32_t unix_now) {
    while (inflight_ < stagger_ && q_head_ < q_tail_) {
      FeedJob j = queue_[q_head_ % HUB_MAX_NODES];
      q_head_++;
      start_job(j, now_ms, unix_now);
    }
  }

  void on_ack(uint8_t node, const feed_ack_payload_t &a, uint32_t unix_now) {
    Slot *s = find_inflight(node, a.feed_id);
    if (!s) return;
    if (a.accepted) {
      s->phase = FeedPhase::Acked;
    } else {
      finish(*s, EV_FEED_REJ, a.reason, unix_now, 0);
    }
  }

  void on_result(uint8_t node, const feed_result_payload_t &r, uint32_t unix_now) {
    if (log_->has_result(r.feed_id)) return; /* §9.7 no double counting */
    Slot *s = find_inflight(node, r.feed_id);
    if (!s) {
      /* late/reconciled result for a feed this hub no longer tracks */
      log_->push(unix_now, node, EV_FEED_FAIL, r.status, r.feed_id, 0, r.pulses);
      return;
    }
    finish(*s, r.status == RESULT_OK ? EV_FEED_OK : EV_FEED_FAIL, r.status,
           unix_now, r.pulses);
  }

  void on_unknown(uint32_t feed_id, uint8_t node, uint32_t unix_now) {
    Slot *s = find_inflight(node, feed_id);
    if (!s) return;
    finish(*s, EV_FEED_UNKNOWN, RESULT_TIMEOUT_UNKNOWN, unix_now, 0);
  }

  /* Call once per known node at boot: a surviving intent = hub died mid-feed. */
  bool recover_pending(uint8_t node_id, uint32_t unix_now) {
    PendingIntent p;
    if (store_get(*store_, pend_key_for(node_id), PENDING_VERSION, &p, sizeof(p)) !=
        (int)sizeof(p))
      return false;
    store_->del(pend_key_for(node_id));
    log_->push(unix_now, node_id, EV_FEED_UNKNOWN, RESULT_TIMEOUT_UNKNOWN,
               p.feed_id, p.amount);
    return true;
  }

  static const char *pend_key_for(uint8_t id) {
    static char key[10];
    snprintf(key, sizeof(key), "pend:%u", (unsigned)id);
    return key;
  }

  int inflight_count() const { return inflight_; }
  bool node_busy(uint8_t node) const {
    for (int i = 0; i < HUB_MAX_NODES; i++)
      if (slots_[i].used && slots_[i].job.node == node) return true;
    for (uint32_t i = q_head_; i < q_tail_; i++)
      if (queue_[i % HUB_MAX_NODES].node == node) return true;
    return false;
  }

 private:
  enum class FeedPhase : uint8_t { Created, Sent, Acked };
  struct Slot {
    FeedJob job;
    FeedPhase phase;
    bool used;
  };

  Slot *find_inflight(uint8_t node, uint32_t feed_id) {
    for (int i = 0; i < HUB_MAX_NODES; i++)
      if (slots_[i].used && slots_[i].job.node == node &&
          slots_[i].job.feed_id == feed_id)
        return &slots_[i];
    return nullptr;
  }

  void start_job(const FeedJob &j, uint32_t now_ms, uint32_t unix_now) {
    for (int i = 0; i < HUB_MAX_NODES; i++) {
      if (slots_[i].used) continue;
      Slot &s = slots_[i];
      s.job = j;
      s.phase = FeedPhase::Created;
      s.used = true;
      inflight_++;
      feed_payload_t f{j.feed_id, j.amount, j.unit, 0};
      if (link_->send_feed(j.node, f, now_ms)) {
        s.phase = FeedPhase::Sent;
      } else {
        /* no peer / radio refused: deterministic fail, node never got it */
        finish(s, EV_FEED_FAIL, EVENT_RESULT_NO_LINK, unix_now, 0);
      }
      return;
    }
  }

  void finish(Slot &s, uint8_t kind, uint8_t result, uint32_t unix_now,
              int16_t aux) {
    log_->push(unix_now, s.job.node, kind, result, s.job.feed_id, s.job.amount,
               aux);
    store_->del(pend_key_for(s.job.node));
    memset(&s, 0, sizeof(s));
    inflight_--;
  }

  IHubLink *link_ = nullptr;
  IHubStore *store_ = nullptr;
  EventLog *log_ = nullptr;
  uint8_t stagger_ = 1;
  int inflight_ = 0;
  Slot slots_[HUB_MAX_NODES]{};
  FeedJob queue_[HUB_MAX_NODES];
  uint32_t q_head_ = 0, q_tail_ = 0;
};

}  // namespace aio

#endif /* AIO_FEED_MANAGER_H */
