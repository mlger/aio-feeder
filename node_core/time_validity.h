/*
 * AIO Feeder — node time trust (SR7, D21; PLAN.md §7.4).
 *
 * Time is valid when:
 *   - healthy RTC and the last hub sync is no older than rtc_trust_days
 *     (default 30 d, confirmed in M1), or
 *   - without a healthy RTC, a TIME_SYNC arrived within the last 24 h.
 *
 * With an RTC the sync age is measured against the RTC's own clock
 * (sync_unix persisted), so trust survives a node reboot. Without one it
 * falls back to millis() and resets at boot.
 */
#ifndef AIO_TIME_VALIDITY_H
#define AIO_TIME_VALIDITY_H

#include <stdint.h>

namespace aio {

/* Defaults pending M1 confirmation (SR7, D21). */
constexpr int64_t kRtcTrustDays = 30;
constexpr uint32_t kNoRtcTrustMs = 24u * 3600u * 1000u;

class TimeValidity {
 public:
  void on_sync(int64_t unix_now, uint32_t now_ms) {
    sync_unix_ = unix_now;
    sync_ms_ = now_ms;
    has_sync_ms_ = true;
  }
  void on_rtc_status(bool healthy) { rtc_healthy_ = healthy; }
  bool rtc_healthy() const { return rtc_healthy_; }

  /* Boot restore: the persisted sync unix timestamp. */
  void restore_sync(int64_t persisted_sync_unix) { sync_unix_ = persisted_sync_unix; }
  int64_t sync_unix() const { return sync_unix_; }
  uint32_t sync_ms() const { return sync_ms_; }

  /* RTC path: measure sync age against the RTC clock. */
  bool valid_rtc(int64_t rtc_now_unix) const {
    if (!rtc_healthy_ || sync_unix_ <= 0 || rtc_now_unix <= 0) return false;
    int64_t age = rtc_now_unix - sync_unix_;
    return age >= 0 && age <= kRtcTrustDays * 86400;
  }

  /* No-RTC path: measure against millis(). */
  bool valid_ms(uint32_t now_ms) const {
    if (rtc_healthy_) return false;  // callers choose one path per RTC presence
    if (!has_sync_ms_) return false;
    return (now_ms - sync_ms_) <= kNoRtcTrustMs;
  }

 private:
  bool rtc_healthy_ = false;
  bool has_sync_ms_ = false;
  int64_t sync_unix_ = 0;
  uint32_t sync_ms_ = 0;
};

}  // namespace aio
#endif /* AIO_TIME_VALIDITY_H */
