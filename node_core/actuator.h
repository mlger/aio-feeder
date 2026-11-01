/*
 * AIO Feeder — actuator state machine (PLAN.md §7.3, SR2–SR8).
 *
 * Pure logic: no MCU headers. Each call returns the commands the runtime must
 * apply in order (persist journal → relay → hard timer → results), so the
 * write-before-actuate ordering (SR6) is explicit and host-testable.
 *
 *   IDLE ──request──► PRE_CHECK ──ok──► ON(pulse k) ──on_ms──► OFF_GAP ──┐
 *     ▲                        │fail          ▲ more pulses ◄──────────┘
 *     └────── COOLDOWN ◄───────┴── RESULT ◄──┘done
 *   FAULT (latched) ◄── watchdog/limit/sensor/storage violation
 *
 * The hard timer (SR2) is armed with on_ms + margin at every RelayOn; the
 * loop turning the relay off is the fast path, the timer is the guarantee.
 */
#ifndef AIO_ACTUATOR_H
#define AIO_ACTUATOR_H

#include <stdint.h>
#include <string.h>
#include "node_limits.h"
#include "feed_journal.h"
#include "proto.h"
#include "profile.h"
#include "time_validity.h"

namespace aio {

/* Hard-timer margin over the computed on-time (loop is the fast path). */
constexpr uint32_t kHardTimerMarginMs = 500;

struct ActuatorCmd {
  enum Kind : uint8_t {
    PersistIntent,   /* write feed_id IN_PROGRESS before the relay (SR6) */
    RelayOn,
    RelayOff,
    ArmHardTimer,    /* ms = duration; forces relay OFF when it fires */
    CancelHardTimer,
    PersistResult,   /* journal record_result */
    ReportResult,    /* hub/CLI: feed_id, result, actual_on_ms, pulses */
  } kind;
  uint32_t feed_id = 0;
  uint32_t ms = 0;
  uint8_t result = 0;        /* feed_result_t */
  uint16_t actual_on_ms = 0;
  uint16_t pulses = 0;
};

struct FeedRequest {
  uint32_t feed_id = 0;
  uint32_t amount = 0;       /* in profile units (g/ml/pulses) */
  bool scheduled = false;    /* false = manual (time not required, SR7) */
  bool time_ok = false;      /* runtime: TimeValidity decision */
  bool vin_ok = true;        /* runtime: supply monitor decision */
  bool manual_stop = false;  /* SR9 latch */
};

class Actuator {
 public:
  void configure(const node_profile_t &p) { profile_ = p; }
  const node_profile_t &profile() const { return profile_; }
  void attach(Limits *limits, FeedJournal *journal) {
    limits_ = limits;
    journal_ = journal;
  }

  /* --- state queries -------------------------------------------------- */
  bool in_fault() const { return fault_; }
  void set_fault(const char *why) { fault_ = true; fault_why_ = why; }
  void clear_fault() { fault_ = false; fault_why_ = nullptr; }
  const char *fault_why() const { return fault_why_; }
  bool idle() const { return st_ == St::Idle; }
  uint8_t last_status() const { return last_status_; }
  uint16_t last_actual_on_ms() const { return last_actual_on_ms_; }

  /* Hard-timer backstop fired: the loop failed to stop the relay in time. */
  void on_hard_timer_fired(uint32_t now_ms) {
    if (st_ == St::On || st_ == St::Gap) {
      if (st_ == St::On) {
        uint32_t ran = elapsed(now_ms);
        on_accum_ms_ += ran > current_on_ms() ? current_on_ms() : ran;
      }
      relay_off_cmds();
      set_fault("hard_timer");
      finish(now_ms, FEED_REJ_FAULT);
    }
  }

  /* --- feed request ---------------------------------------------------- */
  /* Returns false and sets *result when rejected (no commands emitted). */
  bool request_feed(const FeedRequest &req, uint32_t now_ms, bool *accepted,
                    uint8_t *result) {
    *accepted = false;
    *result = FEED_OK;

    /* SR6: duplicates answer from the journal, never actuate again. */
    uint8_t stored;
    if (journal_ && journal_->known(req.feed_id, &stored)) {
      *result = stored;
      return true;
    }
    if (fault_) { *result = FEED_REJ_FAULT; return false; }            /* SR latched first */
    if (st_ != St::Idle) { *result = FEED_REJ_COOLDOWN; return false; }
    if (req.manual_stop) { *result = FEED_REJ_MANUAL_STOP; return false; }
    if (limits_ && !limits_->cooldown_ok(now_ms)) {
      *result = FEED_REJ_COOLDOWN;
      return false;
    }
    if (!supply_ok(req)) { *result = FEED_REJ_LOW_VOLT; return false; }
    if (req.scheduled && !req.time_ok) { *result = FEED_REJ_NO_TIME; return false; }   /* SR7 */
    if (req.scheduled && limits_ && !limits_->limits_confirmed) {
      *result = FEED_REJ_LIMITS_UNCONFIRMED;                                           /* SR8 */
      return false;
    }

    /* Actuation plan + hard on-time cap (SR2). */
    uint16_t max_on = profile_max_on_ms(&profile_);
    uint32_t on_budget = 0;
    if (profile_.unit == UNIT_PULSES) {
      if (profile_.pulse_on_ms == 0) { *result = FEED_REJ_TOO_LONG; return false; }
      uint32_t max_pulses = profile_.max_on_ms_single / profile_.pulse_on_ms;
      if (req.amount == 0 || req.amount > max_pulses ||
          (uint32_t)req.amount * profile_.pulse_on_ms > max_on) {
        *result = FEED_REJ_TOO_LONG;
        return false;
      }
      pulses_total_ = (uint16_t)req.amount;
      on_budget = (uint32_t)pulses_total_ * profile_.pulse_on_ms;
    } else {
      on_budget = (uint32_t)req.amount * profile_.cal_ms_per_unit_x100 / 100;
      if (on_budget == 0 || on_budget > max_on) { *result = FEED_REJ_TOO_LONG; return false; }
      pulses_total_ = 0;
    }

    /* SR3: hourly/daily caps on commanded units. */
    if (limits_) {
      if (limits_->would_exceed_hour(req.amount)) { *result = FEED_REJ_LIMIT_HOUR; return false; }
      if (limits_->would_exceed_day(req.amount)) { *result = FEED_REJ_LIMIT_DAY; return false; }
    }

    /* Accept: persist intent first (SR6); the relay comes on in on_intent_saved. */
    feed_id_ = req.feed_id;
    amount_ = req.amount;
    on_budget_ = on_budget;
    pulses_done_ = 0;
    aborted_ = false;
    st_ = St::WaitIntent;
    *accepted = true;
    emit(ActuatorCmd::PersistIntent, feed_id_);
    return true;
  }

  /* Runtime confirms the journal write landed → energise. */
  bool on_intent_saved(bool persisted_ok, uint32_t now_ms) {
    if (st_ != St::WaitIntent) return false;
    if (!persisted_ok) {  /* SR6 cannot be honored → refuse and latch FAULT */
      set_fault("journal_write");
      st_ = St::Idle;
      emit(ActuatorCmd::ReportResult, feed_id_).result = FEED_REJ_FAULT;
      return false;
    }
    st_ = St::On;
    t_state_ = now_ms;
    on_accum_ms_ = 0;
    if (limits_) limits_->mark_actuated(now_ms);  /* SR4 window starts now */
    emit(ActuatorCmd::RelayOn);
    emit(ActuatorCmd::ArmHardTimer).ms = current_on_ms() + kHardTimerMarginMs;
    return true;
  }

  /* --- cooperative tick ------------------------------------------------- */
  void tick(uint32_t now_ms) {
    switch (st_) {
      case St::Idle:
      case St::WaitIntent:
        break;
      case St::On:
        if (elapsed(now_ms) >= current_on_ms()) {
          on_accum_ms_ += current_on_ms();
          pulses_done_++;
          relay_off_cmds();
          t_state_ = now_ms;
          if (pulses_done_ < pulses_total_) {
            st_ = St::Gap;
          } else {
            finish(now_ms, FEED_OK);
          }
        }
        break;
      case St::Gap:
        if (elapsed(now_ms) >= profile_.pulse_off_ms) {
          /* next pulse must still fit the hard budget (SR2) */
          if (on_accum_ms_ + profile_.pulse_on_ms <= profile_.max_on_ms_single) {
            st_ = St::On;
            t_state_ = now_ms;
            emit(ActuatorCmd::RelayOn);
            emit(ActuatorCmd::ArmHardTimer).ms = current_on_ms() + kHardTimerMarginMs;
          } else {
            finish(now_ms, FEED_OK);
          }
        }
        break;
      case St::Cooldown:
        if (elapsed(now_ms) >= profile_.cooldown_ms) st_ = St::Idle;
        break;
    }
  }

  /* SR9/§9.7: abort an in-flight feed; the partial amount counts. */
  bool abort(uint32_t now_ms) {
    if (st_ != St::On && st_ != St::Gap && st_ != St::WaitIntent) return false;
    if (st_ == St::On) {
      uint32_t ran = elapsed(now_ms);
      on_accum_ms_ += ran > current_on_ms() ? current_on_ms() : ran;
    }
    relay_off_cmds();
    aborted_ = true;
    finish(now_ms, FEED_ABORTED);
    return true;
  }

  /* Command drain: runtime calls after each FSM entry point. */
  uint32_t drain(ActuatorCmd *out, uint32_t cap) {
    uint32_t n = cmd_n_ < cap ? cmd_n_ : cap;
    memcpy(out, cmds_, n * sizeof(ActuatorCmd));
    cmd_n_ = 0;
    return n;
  }

 private:
  enum class St : uint8_t { Idle, WaitIntent, On, Gap, Cooldown };

  uint32_t elapsed(uint32_t now_ms) const { return now_ms - t_state_; }
  uint32_t current_on_ms() const {
    return profile_.unit == UNIT_PULSES ? profile_.pulse_on_ms : on_budget_;
  }

  bool supply_ok(const FeedRequest &req) const {
    /* Enforced only when the profile monitors the supply (§7.3 #4). */
    return (profile_.flags & PROFILE_FLAG_SUPPLY_MONITOR) ? req.vin_ok : true;
  }

  void relay_off_cmds() {
    emit(ActuatorCmd::RelayOff);
    emit(ActuatorCmd::CancelHardTimer);
  }

  ActuatorCmd &emit(ActuatorCmd::Kind k, uint32_t feed_id = 0) {
    ActuatorCmd c{};
    c.kind = k;
    c.feed_id = feed_id ? feed_id : feed_id_;
    cmds_[cmd_n_++] = c;
    return cmds_[cmd_n_ - 1];
  }

  /* Counters (§9.7): OK → full amount; ABORTED/mid-feed FAULT → partial. */
  uint32_t dispensed_units(uint8_t status) const {
    if (status == FEED_OK) return amount_;
    if (profile_.unit == UNIT_PULSES) return (uint32_t)pulses_done_;
    return (uint32_t)last_actual_on_ms_ * 100 / profile_.cal_ms_per_unit_x100;
  }

  void finish(uint32_t now_ms, uint8_t result) {
    last_status_ = result;
    last_actual_on_ms_ = (uint16_t)on_accum_ms_;

    if (journal_) journal_->record_result(feed_id_, result, last_actual_on_ms_);
    if (limits_) limits_->add(dispensed_units(result));

    ActuatorCmd &pr = emit(ActuatorCmd::PersistResult, feed_id_);
    pr.result = result;
    pr.actual_on_ms = last_actual_on_ms_;
    ActuatorCmd &rr = emit(ActuatorCmd::ReportResult, feed_id_);
    rr.result = result;
    rr.actual_on_ms = last_actual_on_ms_;
    rr.pulses = pulses_done_;

    st_ = St::Cooldown;
    t_state_ = now_ms;
  }

  node_profile_t profile_{};
  Limits *limits_ = nullptr;
  FeedJournal *journal_ = nullptr;
  St st_ = St::Idle;
  bool fault_ = false;
  bool aborted_ = false;
  const char *fault_why_ = nullptr;
  uint32_t feed_id_ = 0;
  uint32_t amount_ = 0;
  uint32_t on_budget_ = 0;
  uint32_t on_accum_ms_ = 0;
  uint32_t t_state_ = 0;
  uint16_t pulses_total_ = 0;
  uint16_t pulses_done_ = 0;
  uint8_t last_status_ = FEED_OK;
  uint16_t last_actual_on_ms_ = 0;
  ActuatorCmd cmds_[8]{};
  uint32_t cmd_n_ = 0;
};

}  // namespace aio
#endif /* AIO_ACTUATOR_H */
