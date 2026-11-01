/*
 * AIO Feeder — PIN lock state (PLAN.md §8 UX rules, FR10; M4b).
 *
 * Pure logic, host-tested, RAM-only: the stored secret lives in the settings
 * blob (settings_store.h); this class tracks the session lock. Rules per §8:
 *
 *   - scope is explicit: "settings only" or "settings and manual feeds";
 *   - a correct PIN unlocks for 60 s (PIN_UNLOCK_MS), then re-locks;
 *   - schedules and automatic feeds are NEVER blocked — the scheduler never
 *     asks this class anything; only the manual-feed originators do.
 *
 * The firmware owns one instance; the QEMU shot build drives it for the
 * pin_entry screenshots.
 */
#ifndef AIO_PIN_LOCK_H
#define AIO_PIN_LOCK_H

#include <stdint.h>

#include "settings_store.h"

namespace aio {

class PinLock {
 public:
  /* Re-read the stored config (call after settings load / pin set / off). */
  void configure(const HubSettings &s) {
    enabled_ = s.pin_enabled != 0;
    scope_ = s.pin_scope < PIN_SCOPE_COUNT ? s.pin_scope : PIN_SCOPE_SETTINGS;
    hash_ = s.pin_hash;
    salt_ = s.pin_salt;
    if (!enabled_) locked_ = false; /* no PIN → nothing is ever gated */
  }

  bool enabled() const { return enabled_; }
  uint8_t scope() const { return scope_; }

  bool locked() const { return enabled_ && locked_; }

  /* Settings screen / pairing / factory reset / backup-restore are gated
   * whenever the lock is engaged (§8 note: pairing + factory reset live in
   * Settings, so "settings only" covers them). */
  bool blocks_settings() const { return locked(); }

  /* Manual feeds are gated only when the scope says so. */
  bool blocks_manual_feed() const {
    return locked() && scope_ == PIN_SCOPE_SETTINGS_FEEDS;
  }

  /* Correct digits → unlocked for 60 s from now_ms. Wrong digits never
   * extend anything. */
  bool unlock(const char *digits4, uint32_t now_ms) {
    if (!enabled_) return true; /* nothing to unlock */
    HubSettings s{};
    s.pin_enabled = 1;
    s.pin_salt = salt_;
    s.pin_hash = hash_;
    if (!pin_verify(s, digits4)) {
      fails_++;
      return false;
    }
    locked_ = false;
    unlock_ms_ = now_ms;
    fails_ = 0;
    return true;
  }

  /* Auto re-lock after the 60 s window (call from the periodic loop). */
  void tick(uint32_t now_ms) {
    if (locked_ || !enabled_) return;
    if ((uint32_t)(now_ms - unlock_ms_) >= PIN_UNLOCK_MS) locked_ = true;
  }

  void relock() { locked_ = true; }

  uint16_t unlock_left_s(uint32_t now_ms) const {
    if (locked_ || !enabled_) return 0;
    const uint32_t used = (uint32_t)(now_ms - unlock_ms_);
    if (used >= PIN_UNLOCK_MS) return 0;
    return (uint16_t)((PIN_UNLOCK_MS - used + 999) / 1000);
  }

  uint8_t fail_count() const { return fails_; }

 private:
  bool enabled_ = false;
  bool locked_ = true;
  uint8_t scope_ = PIN_SCOPE_SETTINGS;
  uint32_t hash_ = 0, salt_ = 0;
  uint32_t unlock_ms_ = 0;
  uint8_t fails_ = 0;
};

}  // namespace aio

#endif /* AIO_PIN_LOCK_H */
