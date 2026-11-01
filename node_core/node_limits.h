/*
 * AIO Feeder — node safety caps (SR3, SR4, SR8; PLAN.md §7.3 PRE_CHECK).
 *
 * Hour/day buckets roll on UTC hour/UTC day stamps (unix/3600, unix/86400).
 * Counters are persisted by the runtime so caps survive reboots (SR3);
 * a fresh node starts limits-unconfirmed and refuses scheduled feeds until
 * the hub pushes profile + counters or confirms zero (SR8).
 */
#ifndef AIO_LIMITS_H
#define AIO_LIMITS_H

#include <stdint.h>

namespace aio {

struct UsageCounters {
  uint32_t hour_units = 0;
  uint32_t day_units = 0;
  uint32_t hour_stamp = 0;  /* unix/3600 of the hour bucket */
  uint32_t day_stamp = 0;   /* unix/86400 of the day bucket */
};

class Limits {
 public:
  void configure(uint32_t max_units_per_hour, uint32_t max_units_per_day,
                 uint32_t cooldown_ms) {
    max_hour_ = max_units_per_hour;
    max_day_ = max_units_per_day;
    cooldown_ms_ = cooldown_ms;
  }

  /* Restore persisted counters and roll stale buckets forward. */
  void load(const UsageCounters &c, int64_t unix_now) {
    counters_ = c;
    roll(unix_now);
  }
  const UsageCounters &counters() const { return counters_; }

  void roll(int64_t unix_now) {
    if (unix_now <= 0) return;  // no valid time yet: keep buckets as-is
    uint32_t hour = (uint32_t)(unix_now / 3600);
    uint32_t day = (uint32_t)(unix_now / 86400);
    if (counters_.hour_stamp != hour) {
      counters_.hour_stamp = hour;
      counters_.hour_units = 0;
    }
    if (counters_.day_stamp != day) {
      counters_.day_stamp = day;
      counters_.day_units = 0;
    }
  }

  bool would_exceed_hour(uint32_t units) const {
    if (bench_bypass) return false;
    return counters_.hour_units + units > max_hour_;
  }
  bool would_exceed_day(uint32_t units) const {
    if (bench_bypass) return false;
    return counters_.day_units + units > max_day_;
  }
  void add(uint32_t units) {
    counters_.hour_units += units;
    counters_.day_units += units;
  }

  /* SR4: minimum cooldown between actuations. last_actuation_ms is millis(). */
  bool cooldown_ok(uint32_t now_ms) const {
    if (bench_bypass) return true;
    return last_actuation_ms_ == 0 || (now_ms - last_actuation_ms_) >= cooldown_ms_;
  }
  void mark_actuated(uint32_t now_ms) { last_actuation_ms_ = now_ms; }
  uint32_t cooldown_remaining(uint32_t now_ms) const {
    if (cooldown_ok(now_ms)) return 0;
    return cooldown_ms_ - (now_ms - last_actuation_ms_);
  }

  /* SR8: scheduled feeds refused until the hub confirms profile + counters. */
  bool limits_confirmed = false;

  /* Bench-only, RAM-only, never persisted: bypasses SR3/SR4 so the 1000-feed
   * loopback (§12 M2) can run against the bench rig. SR2 (hard max on-time)
   * and the FAULT latch are NOT bypassed — they live in the Actuator. */
  bool bench_bypass = false;
  void set_bench_bypass(bool on) { bench_bypass = on; }

 private:
  uint32_t max_hour_ = 0;
  uint32_t max_day_ = 0;
  uint32_t cooldown_ms_ = 0;
  uint32_t last_actuation_ms_ = 0;
  UsageCounters counters_{};
};

}  // namespace aio
#endif /* AIO_LIMITS_H */
