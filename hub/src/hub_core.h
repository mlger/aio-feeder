/*
 * AIO Feeder — hub core: schedule engine + feed orchestration
 * (PLAN.md §7.2 sched_task + feed_manager ownership, §9.4, §9.9, §12 M3).
 *
 * Exit criterion (§12 M3): schedules fire correctly across reboots.
 *
 * Reboot model:
 *   - Schedules live in the registry blob, per-node occurrence bookkeeping in
 *     the run blob (last handled occurrence per entry). Both survive reboot.
 *   - begin() runs the boot scan: occurrences since the last handled one are
 *     classified per §9.4 — one catch-up feed max per node (policy=1, inside
 *     catchup_window_min), everything else SKIPPED_MISSED (never stack).
 *     An occurrence inside the current minute still fires on time.
 *   - While running, 1 Hz tick(): an occurrence is due when it lies inside
 *     (last handled, now] — so a DST gap ("runs just after the gap", §7.4)
 *     and the repeated hour (fires once, first occurrence) fall out naturally.
 *   - Firing marks the occurrence handled BEFORE the FEED goes out (persisted),
 *     so a reboot can never double-fire; the feed manager's WAL intent turns
 *     a reboot mid-feed into FEED_UNKNOWN (§6.6).
 *
 * Pure logic, host-tested; the firmware binds IHubLink to LinkEngine.
 */
#ifndef AIO_HUB_CORE_H
#define AIO_HUB_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../lib/proto/messages.h"
#include "../../lib/proto/profile.h"
#include "../../lib/proto/proto.h"
#include "../../node_core/schedule.h"
#include "event_log.h"
#include "feed_manager.h"
#include "hub_registry.h"
#include "hub_store.h"
#include "hub_time.h"

namespace aio {

/* Scan this many local days back at boot for missed occurrences (§9.6
 * freshness window is 7 days). */
constexpr uint8_t MISSED_SCAN_DAYS = 7;

class HubCore {
 public:
  void begin(HubRegistry *reg, IHubLink *link, IHubStore *store, EventLog *log,
             int64_t unix_now) {
    reg_ = reg;
    log_ = log;
    store_ = store;
    fm_.begin(link, store, log, reg->data().stagger_max);
    /* a hub death mid-feed surfaces as FEED_UNKNOWN, never a re-issue */
    for (int i = 0; i < HUB_MAX_NODES; i++) {
      HubNode *n = reg->node(i);
      if (!n || !n->id) continue;
      fm_.recover_pending(n->id, unix_now > 1600000000 ? (uint32_t)unix_now : 0);
    }
    /* SR7: scheduled feeds need valid time — defer the boot scan until the
     * clock is trusted (RTC/NTP); tick() completes it then. */
    if (unix_now >= 1600000000) {
      boot_scan(unix_now);
      boot_scan_done_ = true;
      reg->save(*store);
    }
  }

  /* 1 Hz from sched_task; now_ms = millis(), unix_now = clock. */
  void tick(uint32_t now_ms, int64_t unix_now) {
    if (unix_now < 0 || unix_now < 1600000000) {
      fm_.tick(now_ms, 0); /* manual feeds still drain; scheduled need time */
      return;              /* time not valid (SR7) */
    }
    if (!boot_scan_done_) {
      boot_scan(unix_now);
      boot_scan_done_ = true;
      reg_->save(*store_);
    }
    if (last_eval_unix_ < 0 || unix_now != last_eval_unix_) {
      eval(unix_now); /* fire/queue first… */
      last_eval_unix_ = unix_now;
    }
    fm_.tick(now_ms, (uint32_t)unix_now); /* …then send, same tick */
  }

  /* Link callbacks (forwarded from LinkHandlers by the firmware). */
  void on_feed_ack(uint8_t node, const feed_ack_payload_t &a, uint32_t unix_now) {
    fm_.on_ack(node, a, unix_now);
  }
  void on_feed_result(uint8_t node, const feed_result_payload_t &r, uint32_t unix_now) {
    fm_.on_result(node, r, unix_now);
  }
  void on_feed_unknown(uint32_t feed_id, uint8_t node, uint32_t unix_now) {
    fm_.on_unknown(feed_id, node, unix_now);
  }

  /* Manual feed (§9.10 double-feed confirm is a UI concern, M4). */
  bool request_manual(uint8_t node, uint16_t amount, uint32_t now_ms,
                      int64_t unix_now) {
    HubNode *n = reg_->by_id(node);
    if (!n) return false;
    FeedJob j{reg_->data().feed_id_seq++, node, amount, n->profile.unit,
              (uint8_t)FeedKind::Manual};
    if (!fm_.submit(j)) {
      reg_->data().feed_id_seq--; /* id not consumed */
      return false;
    }
    reg_->save(*store_);
    fm_.tick(now_ms, (uint32_t)unix_now);
    return true;
  }

  FeedManager &fm() { return fm_; }
  EventLog &log() { return *log_; }
  uint8_t tz() const { return reg_->data().tz_rule; }

  /* Test/device visibility: newest handled occurrence for an entry. */
  const OccRef &last_done(uint8_t node_id, uint8_t entry) const {
    const HubNode *n = reg_->by_id(node_id);
    static OccRef none{0, 0};
    if (!n || entry >= HUB_SCHED_MAX_ENTRIES) return none;
    return n->run.last_done[entry];
  }

 private:
  /* --- boot: classify occurrences since last handled (§9.4, never stack) --- */
  void boot_scan(int64_t unix_now) {
    const LocalTime now = local_from_unix(unix_now, tz());
    for (int i = 0; i < HUB_MAX_NODES; i++) {
      HubNode *n = reg_->node(i);
      if (!n || !n->id || !n->sched.enabled) continue;
      node_boot_scan(*n, now, unix_now);
    }
  }

  void node_boot_scan(HubNode &n, const LocalTime &now, int64_t unix_now) {
    const node_profile_t &p = n.profile;
    const int64_t catchup_win_s =
        (int64_t)(p.catchup_window_min ? p.catchup_window_min : 30) * 60;
    bool catchup_used = false;
    for (uint8_t e = 0; e < n.sched.entry_count && e < HUB_SCHED_MAX_ENTRIES; e++) {
      const sched_entry_t &en = n.sched.entries[e];
      if (en.min_of_day >= 1440) continue;
      const OccRef orig_last = n.run.last_done[e];
      OccRef latest{0, 0};
      int64_t latest_local = 0;
      for (uint8_t back = 0; back <= MISSED_SCAN_DAYS; back++) {
        const int64_t day = now.day_start - (int64_t)back * 86400;
        const uint8_t wday = wday_from_days(day / 86400);
        if (!sched_day_due(en.days_mask, wday)) continue;
        OccRef occ{(uint32_t)day, en.min_of_day};
        if (occ_ref_le(occ, n.run.last_done[e])) continue; /* already handled */
        /* strictly in the future (beyond the current minute)? */
        if (!occ_ref_le(occ, OccRef{(uint32_t)now.day_start, now.min_of_day}))
          continue;
        latest = occ;
        latest_local = day + (int64_t)en.min_of_day * 60;
        break;
      }
      if (!latest.day_start) continue;

      /* older unhandled occurrences of this entry (multi-day outage) */
      uint16_t older = 0;
      for (uint8_t back = 0; back <= MISSED_SCAN_DAYS; back++) {
        const int64_t day = now.day_start - (int64_t)back * 86400;
        const uint8_t wday = wday_from_days(day / 86400);
        if (!sched_day_due(en.days_mask, wday)) continue;
        OccRef occ{(uint32_t)day, en.min_of_day};
        if (occ_ref_le(occ, orig_last)) continue;
        if (occ.day_start == latest.day_start) continue; /* handled below */
        if (!occ_ref_le(occ, OccRef{(uint32_t)now.day_start, now.min_of_day}))
          continue;
        older++;
      }

      const int64_t age_s = now.local_s - latest_local;
      const SchedGate gate =
          schedule_gate(occ_unix_est(latest.day_start, latest.min_of_day, tz(),
                                     unix_now),
                        n.sched.skip_until, n.sched.hold_until);
      const bool within_window = age_s >= 0 && age_s <= catchup_win_s;

      if (gate == SchedGate::SkippedUser) {
        skipped(n, e, latest, EV_SKIPPED_USER, 1, unix_now);
      } else if (gate == SchedGate::SkippedHold) {
        skipped(n, e, latest, EV_SKIPPED_HOLD, 1, unix_now);
      } else if (age_s < 60) {
        if (older) report_older(n, e, older, unix_now);
        fire(n, e, latest, (uint8_t)FeedKind::Scheduled, unix_now);
      } else if (p.missed_policy == 1 && !catchup_used && within_window) {
        /* §9.4 catch up once — one feed per node, never stacked */
        catchup_used = true;
        if (older) report_older(n, e, older, unix_now);
        fire(n, e, latest, (uint8_t)FeedKind::CatchUp, unix_now);
        log_->push((uint32_t)unix_now, n.id, EV_CATCHUP, 0, 0, en.amount,
                   (int16_t)(age_s / 60), EV_SRC_CATCHUP);
      } else {
        /* default policy skip (and catch-up used or outside window): one
         * event covers the latest + all older occurrences */
        n.run.last_done[e] = latest;
        reg_->save(*store_);
        log_->push((uint32_t)unix_now, n.id, EV_SKIPPED_MISSED, 0, 0,
                   en.amount, (int16_t)(older + 1));
      }
    }
  }

  /* --- running: fire occurrences inside (last_done, now] ------------------- */
  void eval(int64_t unix_now) {
    const LocalTime now = local_from_unix(unix_now, tz());
    for (int i = 0; i < HUB_MAX_NODES; i++) {
      HubNode *n = reg_->node(i);
      if (!n || !n->id || !n->sched.enabled) continue;
      for (uint8_t e = 0; e < n->sched.entry_count && e < HUB_SCHED_MAX_ENTRIES;
           e++) {
        const sched_entry_t &en = n->sched.entries[e];
        if (en.min_of_day >= 1440) continue;
        if (!sched_day_due(en.days_mask, now.wday)) continue;
        OccRef occ{(uint32_t)now.day_start, en.min_of_day};
        if (occ_ref_le(occ, n->run.last_done[e])) continue; /* handled */
        /* occurrence must already have arrived in local time */
        if (now.local_s < now.day_start + (int64_t)en.min_of_day * 60) continue;
        const SchedGate gate = schedule_gate(
            occ_unix_est(occ.day_start, occ.min_of_day, tz(), unix_now),
            n->sched.skip_until, n->sched.hold_until);
        if (gate == SchedGate::SkippedUser)
          skipped(*n, e, occ, EV_SKIPPED_USER, 1, unix_now);
        else if (gate == SchedGate::SkippedHold)
          skipped(*n, e, occ, EV_SKIPPED_HOLD, 1, unix_now);
        else
          fire(*n, e, occ, (uint8_t)FeedKind::Scheduled, unix_now);
      }
    }
  }

  void report_older(HubNode &n, uint8_t e, uint16_t count, int64_t unix_now) {
    log_->push((uint32_t)unix_now, n.id, EV_SKIPPED_MISSED, 0, 0,
               n.sched.entries[e].amount, (int16_t)count);
  }

  /* Mark handled and hand to the feed manager (occurrence marked before send,
   * persisted — a reboot can never double-fire). */
  void fire(HubNode &n, uint8_t e, const OccRef &occ, uint8_t kind,
            int64_t unix_now) {
    if (fm_.node_busy(n.id)) {
      /* node mutex busy: leave occurrence unhandled; it will classify as
       * missed/catch-up on the next boot — never queue two feeds per node */
      return;
    }
    n.run.last_done[e] = occ;
    FeedJob j{reg_->data().feed_id_seq++, n.id, n.sched.entries[e].amount,
              n.profile.unit, kind};
    fm_.submit(j);
    reg_->save(*store_); /* last_done + feed_id_seq durable BEFORE send */
  }

  void skipped(HubNode &n, uint8_t e, const OccRef &occ, uint8_t kind,
               int16_t aux, int64_t unix_now) {
    n.run.last_done[e] = occ;
    reg_->save(*store_);
    log_->push((uint32_t)unix_now, n.id, kind, 0, 0,
               n.sched.entries[e].amount, aux);
  }

  HubRegistry *reg_ = nullptr;
  IHubStore *store_ = nullptr;
  EventLog *log_ = nullptr;
  FeedManager fm_;
  int64_t last_eval_unix_ = -1;
  bool boot_scan_done_ = false;
};

}  // namespace aio

#endif /* AIO_HUB_CORE_H */
