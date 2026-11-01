/*
 * AIO Feeder — hub calibration session (PLAN.md §8.4, §9.3; FR4, D27).
 *
 * Pure logic, host-tested. Times a test dispense on a node, takes the
 * weighed amount, derives the node's flow rate (ms per gram/ml), repeats
 * and keeps the median, then writes it into the profile (§9.3):
 *
 *   start(node, profile) → set_test_s() → the caller sends the commanded
 *   FEED (test_grams() under the current rate) and calls mark_sent() →
 *   FEED_RESULT routes through on_result() (OK holds actual_on_ms for the
 *   weigh step; any node rejection bounces back with a note) →
 *   set_weigh_grams_x100() validates the weighed amount (≥ 5 g, §9.3.3)
 *   and the derived rate (§9.3.4 sanity range; must fit the uint16
 *   cal_ms_per_unit_x100 field) → repeat for a median → apply() writes the
 *   median into the profile and re-CRCs; mark_saved() closes it out.
 *
 * The test run deliberately bypasses feed_manager (like the M2 feedloop):
 * the node still enforces every profile limit (§9.3.2 "under normal
 * limits"), and the wizard must not tangle with scheduled-feed state.
 *
 * All timing is against the caller's ms clock; tick() enforces the
 * FEED_RESULT deadline. Start over with close() + start().
 */
#ifndef AIO_CALIB_SESSION_H
#define AIO_CALIB_SESSION_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../lib/proto/messages.h"
#include "../../lib/proto/profile.h"
#include "hub_registry.h"
#include "ui_model.h"

namespace aio {

constexpr uint32_t CAL_DISPENSE_TIMEOUT_MS = 15000; /* FEED_RESULT deadline */
/* §9.3.4 sanity range, ms per unit × 100: 10 ms/unit (free-flowing) up to
 * the uint16 field cap 65535 (655.35 ms/unit). Outside that band the result
 * is a scale or food problem, not a calibration to store. */
constexpr uint32_t CAL_RATE_MIN_X100 = 1000;
constexpr uint32_t CAL_RATE_MAX_X100 = 65535;

class CalibSession {
 public:
  /* --- lifecycle ------------------------------------------------------------------- */

  /* Opens the wizard for node_id with a copy of its profile. Returns false
   * when a session is already open. */
  bool start(uint8_t node_id, const node_profile_t &p) {
    if (phase_ != PH_IDLE) return false;
    node_id_ = node_id;
    profile_ = p;
    old_rate_ = p.cal_ms_per_unit_x100;
    test_s_ = default_test_s(p);
    samples_n_ = 0;
    note_[0] = '\0';
    saved_id_ = 0;
    phase_ = PH_RUNTIME;
    return true;
  }

  void close() {
    phase_ = PH_IDLE;
    node_id_ = 0;
    samples_n_ = 0;
    note_[0] = '\0';
    saved_id_ = 0;
  }

  bool is_open() const { return phase_ != PH_IDLE; }
  uint8_t node_id() const { return node_id_; }

  /* --- test run length (§9.3.1: gate default 2 s, ≤ node max on-time) --------------- */

  /* The run can never outlast the node's per-actuation cap (SR2): the gate
   * cap is GATE_MAX_ON_MS (D27), other mechs use max_on_ms_single. */
  static uint32_t max_test_s(const node_profile_t &p) {
    const uint32_t cap = profile_max_on_ms(&p) / 1000u;
    return cap ? cap : 1;
  }

  static uint32_t default_test_s(const node_profile_t &p) {
    const uint32_t cap = max_test_s(p);
    const uint32_t want = (p.mech == MECH_SOLENOID_GATE) ? 2 : 5;
    return want < cap ? want : cap;
  }

  bool set_test_s(uint32_t s) {
    if (phase_ != PH_RUNTIME) return false;
    if (!s || s > max_test_s(profile_)) return false;
    test_s_ = s;
    return true;
  }

  uint32_t test_s() const { return test_s_; }

  /* Commanded amount for the current test length under the profile's
   * *current* rate: test_ms × 100 / cal_ms_per_unit_x100, ceil, min 1.
   * Unit follows the profile (g, or ml for pumps). */
  uint32_t test_grams() const {
    if (!profile_.cal_ms_per_unit_x100) return 1;
    const uint32_t g = (test_s_ * 1000u * 100u + profile_.cal_ms_per_unit_x100 - 1u) /
                       profile_.cal_ms_per_unit_x100;
    return g ? g : 1;
  }

  /* --- FEED in flight ---------------------------------------------------------------- */

  /* The caller sent the commanded FEED; the wizard watches for its result. */
  bool mark_sent(uint32_t feed_id, uint32_t now_ms) {
    if (phase_ != PH_RUNTIME) return false;
    feed_id_ = feed_id;
    sent_ms_ = now_ms;
    phase_ = PH_DISPENSE;
    return true;
  }

  /* Feed id the wizard is waiting on (0 = none) — lets the result handler
   * filter cheaply before calling on_result. */
  uint32_t pending_feed_id() const {
    return phase_ == PH_DISPENSE ? feed_id_ : 0;
  }

  /* FEED_RESULT routing (call for every result; the session filters by
   * feed_id). OK → the weigh step holds actual_on_ms. Anything else (node
   * caps, abort, …) bounces back to the runtime step with a note. */
  bool on_result(uint32_t feed_id, uint8_t status, uint16_t actual_on_ms) {
    if (phase_ != PH_DISPENSE || feed_id != feed_id_) return false;
    if (status != FEED_OK) {
      snprintf(note_, sizeof(note_), "node rejected the run (code %u)",
               (unsigned)status);
      phase_ = PH_RUNTIME;
      return false;
    }
    if (!actual_on_ms) {
      snprintf(note_, sizeof(note_), "node reported zero on-time");
      phase_ = PH_RUNTIME;
      return false;
    }
    weigh_on_ms_ = actual_on_ms;
    phase_ = PH_WEIGH;
    return true;
  }

  /* FEED_RESULT deadline (§9.3.2); call from the periodic loop. */
  bool tick(uint32_t now_ms) {
    if (phase_ != PH_DISPENSE) return false;
    if ((uint32_t)(now_ms - sent_ms_) < CAL_DISPENSE_TIMEOUT_MS) return false;
    snprintf(note_, sizeof(note_), "no result from the node (timeout)");
    phase_ = PH_RUNTIME;
    return true;
  }

  /* --- weigh (§9.3.3 / §9.3.4) --------------------------------------------------------- */

  /* grams_x100: weighed amount × 100 (8.25 g → 825; ml likewise). A good
   * run becomes a sample and the wizard returns to the runtime step, ready
   * for the next run (§9.3.5: repeat, use the median). */
  bool set_weigh_grams_x100(uint32_t grams_x100) {
    if (phase_ != PH_WEIGH) return false;
    if (grams_x100 < ui::CAL_MIN_WEIGH_X100) {
      snprintf(note_, sizeof(note_), "%lu.%02lu %s is too small (min 5)",
               (unsigned long)(grams_x100 / 100), (unsigned long)(grams_x100 % 100),
               ui::cal_unit_label(profile_.mech));
      return false;
    }
    const uint32_t rate =
        ((uint32_t)weigh_on_ms_ * 10000u + grams_x100 / 2u) / grams_x100;
    if (rate < CAL_RATE_MIN_X100) {
      snprintf(note_, sizeof(note_), "flow too fast for the profile format");
      return false;
    }
    if (rate > CAL_RATE_MAX_X100) {
      snprintf(note_, sizeof(note_), "flow too slow / scale off (max 655 ms per %s)",
               ui::cal_unit_label(profile_.mech));
      return false;
    }
    if (samples_n_ >= ui::CAL_MAX_SAMPLES) return false;
    samples_[samples_n_].on_ms = weigh_on_ms_;
    samples_[samples_n_].grams_x100 = grams_x100;
    samples_[samples_n_].rate_x100 = (uint16_t)rate;
    samples_n_++;
    note_[0] = '\0';
    phase_ = PH_RUNTIME;
    return true;
  }

  uint8_t sample_count() const { return samples_n_; }

  /* §9.3.5: median of the samples (odd → middle, even → lower middle). */
  uint16_t median_rate_x100() const {
    if (!samples_n_) return 0;
    uint16_t v[ui::CAL_MAX_SAMPLES];
    for (uint8_t i = 0; i < samples_n_; i++) v[i] = samples_[i].rate_x100;
    for (uint8_t i = 1; i < samples_n_; i++) { /* insertion sort, n ≤ 5 */
      const uint16_t k = v[i];
      int8_t j = (int8_t)(i - 1);
      for (; j >= 0 && v[j] > k; j--) v[j + 1] = v[j];
      v[j + 1] = k;
    }
    return v[(samples_n_ - 1) / 2];
  }

  /* --- save (§9.3.5) ------------------------------------------------------------------- */

  /* Writes the median into the profile and re-CRCs it. The caller then
   * persists the registry and pushes CONFIG to the node. */
  bool apply(node_profile_t *p) const {
    if (!p || !samples_n_) return false;
    p->cal_ms_per_unit_x100 = median_rate_x100();
    p->crc16 = profile_crc16(p);
    return true;
  }

  void mark_saved() {
    if (phase_ == PH_RUNTIME || phase_ == PH_WEIGH) {
      saved_id_ = node_id_;
      phase_ = PH_DONE;
    }
  }

  /* --- view snapshot (the LVGL screen renders this; never the session) ---------------- */

  ui::CalibView view(uint32_t now_ms) const {
    ui::CalibView v;
    switch (phase_) {
      case PH_RUNTIME: v.step = ui::CAL_RUNTIME; break;
      case PH_DISPENSE: v.step = ui::CAL_DISPENSE; break;
      case PH_WEIGH: v.step = ui::CAL_WEIGH; break;
      case PH_DONE: v.step = ui::CAL_DONE; break;
      default: v.step = ui::CAL_IDLE; break;
    }
    v.node_id = node_id_;
    strncpy(v.name, profile_.name, sizeof(v.name) - 1);
    v.name[sizeof(v.name) - 1] = '\0';
    v.mech_sel = profile_.mech <= 2 ? profile_.mech : 0;
    v.old_rate_x100 = old_rate_;
    v.test_s = test_s_;
    v.max_test_s = max_test_s(profile_);
    v.test_grams = test_grams();
    if (phase_ == PH_DISPENSE) {
      const uint32_t used = (uint32_t)(now_ms - sent_ms_);
      v.left_s = used < CAL_DISPENSE_TIMEOUT_MS
                     ? (CAL_DISPENSE_TIMEOUT_MS - used + 999) / 1000
                     : 0;
    }
    v.weigh_on_ms = phase_ == PH_WEIGH ? weigh_on_ms_ : 0;
    v.sample_count = samples_n_;
    for (uint8_t i = 0; i < samples_n_; i++) v.samples[i] = samples_[i];
    v.median_x100 = median_rate_x100();
    v.saved_node_id = saved_id_;
    strncpy(v.note, note_, sizeof(v.note) - 1);
    v.note[sizeof(v.note) - 1] = '\0';
    return v;
  }

 private:
  enum CalPhase : uint8_t { PH_IDLE = 0, PH_RUNTIME, PH_DISPENSE, PH_WEIGH, PH_DONE };

  node_profile_t profile_{};
  uint8_t node_id_ = 0;
  uint16_t old_rate_ = 0;
  CalPhase phase_ = PH_IDLE;
  uint32_t test_s_ = 2;
  uint32_t feed_id_ = 0;
  uint32_t sent_ms_ = 0;
  uint32_t weigh_on_ms_ = 0;
  ui::CalSampleRow samples_[ui::CAL_MAX_SAMPLES]{};
  uint8_t samples_n_ = 0;
  char note_[48] = "";
  uint8_t saved_id_ = 0;
};

}  // namespace aio

#endif /* AIO_CALIB_SESSION_H */
