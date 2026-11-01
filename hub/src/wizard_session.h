/*
 * AIO Feeder — first-run setup wizard session (PLAN.md §9.13, FR21; M4b).
 *
 * Pure logic, host-tested. Six steps, Back available at every one:
 *
 *   (1) time zone + NTP          (2) Wi-Fi name (+ connection test)
 *   (3) choose a paired node or pair a new one
 *   (4) hopper size + amount in it now  — SKIPPED when the node was just
 *       paired, because the pairing dialog already asked (§9.13)
 *   (5) first schedule (time, days preset, amount within the node's
 *       per-feed cap)                    (6) done + reminders
 *
 * Nothing is written to a node (or the registry) until the schedule step is
 * saved: every set_* only touches draft fields, and commit() applies the
 * whole wizard in one place. The caller then pushes SCHEDULE_SET + CONFIG
 * and persists.
 *
 * The LVGL screen renders ui::WizardView snapshots (never the session), the
 * same presenter pattern as pairing/calibration.
 */
#ifndef AIO_WIZARD_SESSION_H
#define AIO_WIZARD_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../lib/proto/profile.h"
#include "hub_registry.h"
#include "settings_store.h"
#include "ui_model.h"

namespace aio {

class WizardSession {
 public:
  void close() {
    reset();
  }
  bool is_open() const { return phase_ != ui::WIZ_IDLE; }

  bool open() {
    if (phase_ != ui::WIZ_IDLE) return false;
    reset();
    phase_ = ui::WIZ_TZ;
    return true;
  }

  ui::WizStep step() const { return phase_; }

  /* --- step 1: time zone + NTP -------------------------------------------------- */

  bool set_tz(uint8_t rule, bool ntp_on) {
    if (phase_ != ui::WIZ_TZ) return false;
    if (rule >= TZ_RULE_COUNT) {
      snprintf(note_, sizeof(note_), "unknown time zone");
      return false;
    }
    tz_rule_ = rule;
    ntp_on_ = ntp_on;
    return true;
  }

  /* --- step 2: Wi-Fi ------------------------------------------------------------ */

  /* ssid required (≤ 32 chars); pass may be empty (open network). */
  bool set_wifi(const char *ssid, const char *pass) {
    if (phase_ != ui::WIZ_WIFI) return false;
    if (!ssid || !ssid[0] || strlen(ssid) >= sizeof(wifi_ssid_)) {
      snprintf(note_, sizeof(note_), "network name is required");
      return false;
    }
    if (pass && strlen(pass) >= sizeof(wifi_pass_)) {
      snprintf(note_, sizeof(note_), "password too long (max 64)");
      return false;
    }
    snprintf(wifi_ssid_, sizeof(wifi_ssid_), "%s", ssid);
    snprintf(wifi_pass_, sizeof(wifi_pass_), "%s", pass ? pass : "");
    note_[0] = '\0';
    return true;
  }

  /* Connection test outcome (radio bring-up on hardware; the QEMU shot
   * build reports "no radio" and the wizard continues anyway). */
  void wifi_test_result(bool ok, const char *note) {
    wifi_state_ = ok ? 1 : 0;
    snprintf(wifi_note_, sizeof(wifi_note_), "%s",
             note ? note : (ok ? "connected" : "not connected"));
  }

  /* --- step 3: node ------------------------------------------------------------- */

  bool choose_existing(uint8_t node_id) {
    if (phase_ != ui::WIZ_NODE) return false;
    if (!node_id) return false;
    node_id_ = node_id;
    pair_new_ = false;
    just_paired_ = false;
    return true;
  }

  /* "Pair a new one" — the caller opens the pairing window; the wizard
   * resumes through on_paired() when the commit lands. */
  void choose_pair_new() {
    if (phase_ != ui::WIZ_NODE) return;
    pair_new_ = true;
    node_id_ = 0;
  }

  /* Pairing committed for this node → chosen + capacity step skipped. */
  bool on_paired(uint8_t node_id) {
    if (!node_id) return false;
    if (phase_ != ui::WIZ_NODE && phase_ != ui::WIZ_CAP) return false;
    node_id_ = node_id;
    pair_new_ = false;
    just_paired_ = true;
    if (phase_ == ui::WIZ_NODE) phase_ = ui::WIZ_CAP; /* marked skip; next() hops */
    return true;
  }

  /* --- step 4: hopper size + level (§9.7, same rules as pairing) ----------------- */

  bool set_capacity(uint32_t total, uint32_t now) {
    if (phase_ != ui::WIZ_CAP) return false;
    if (!ui::pair_capacity_valid(total, now)) {
      snprintf(note_, sizeof(note_), "size 1..100000, in-it 0..size");
      return false;
    }
    cap_total_ = total;
    cap_now_ = now;
    note_[0] = '\0';
    return true;
  }

  /* --- step 5: first schedule ----------------------------------------------------- */

  bool set_schedule(uint16_t min_of_day, uint8_t days, uint16_t amount) {
    if (phase_ != ui::WIZ_SCHED) return false;
    if (min_of_day >= 1440) {
      snprintf(note_, sizeof(note_), "time 00:00..23:59");
      return false;
    }
    if (!days || (days & 0x7F) != days) {
      snprintf(note_, sizeof(note_), "pick at least one day");
      return false;
    }
    const uint32_t cap = feed_cap_units();
    if (!amount || (cap && amount > cap)) {
      snprintf(note_, sizeof(note_), "amount 1..%lu %s",
               (unsigned long)(cap ? cap : 999), unit_label());
      return false;
    }
    s_min_ = min_of_day;
    s_days_ = days;
    s_amount_ = amount;
    note_[0] = '\0';
    return true;
  }

  uint32_t feed_cap_units() const {
    const HubNode *n = reg_ ? reg_->by_id(node_id_) : nullptr;
    return n ? profile_feed_cap_units(&n->profile) : 0;
  }

  const char *unit_label() const {
    const HubNode *n = reg_ ? reg_->by_id(node_id_) : nullptr;
    return (n && n->profile.unit == UNIT_ML) ? "ml" : "g";
  }

  void bind_registry(HubRegistry *reg) { reg_ = reg; }

  /* --- navigation ------------------------------------------------------------------ */

  /* Advance one step. WZ_CAP is skipped for a just-paired node (§9.13). */
  bool next() {
    switch (phase_) {
      case ui::WIZ_TZ:
        phase_ = ui::WIZ_WIFI;
        return true;
      case ui::WIZ_WIFI:
        if (!wifi_ssid_[0]) {
          snprintf(note_, sizeof(note_), "set the network first");
          return false;
        }
        phase_ = ui::WIZ_NODE;
        return true;
      case ui::WIZ_NODE:
        if (pair_new_ && !node_id_) {
          snprintf(note_, sizeof(note_),
                   "pair the feeder; the wizard resumes");
          return false;
        }
        if (!node_id_) {
          snprintf(note_, sizeof(note_), "choose a feeder first");
          return false;
        }
        phase_ = just_paired_ ? ui::WIZ_SCHED : ui::WIZ_CAP;
        return true;
      case ui::WIZ_CAP:
        if (!ui::pair_capacity_valid(cap_total_, cap_now_)) {
          snprintf(note_, sizeof(note_), "size 1..100000, in-it 0..size");
          return false;
        }
        phase_ = ui::WIZ_SCHED;
        return true;
      case ui::WIZ_SCHED:
        /* validation happens in set_schedule; the SAVE action commits */
        if (!s_days_ || !s_amount_ || s_min_ >= 1440) {
          snprintf(note_, sizeof(note_), "set time, days and amount");
          return false;
        }
        return true; /* caller now calls commit() */
      default:
        return false;
    }
  }

  bool back() {
    switch (phase_) {
      case ui::WIZ_WIFI: phase_ = ui::WIZ_TZ; return true;
      case ui::WIZ_NODE: phase_ = ui::WIZ_WIFI; return true;
      case ui::WIZ_CAP: phase_ = ui::WIZ_NODE; return true;
      case ui::WIZ_SCHED: phase_ = just_paired_ ? ui::WIZ_NODE : ui::WIZ_CAP; return true;
      case ui::WIZ_DONE: phase_ = ui::WIZ_SCHED; return true;
      default: return false;
    }
  }

  /* --- commit (the ONLY write; §9.13 "nothing until the schedule step") -------------- */

  enum CommitResult : uint8_t {
    WZ_COMMIT_OK = 0,
    WZ_COMMIT_NO_NODE,
    WZ_COMMIT_BAD_STATE,
  };

  /* Applies tz + Wi-Fi/NTP to the registry/settings, writes capacity +
   * first schedule into the chosen node's profile/schedule. The caller
   * persists both blobs and pushes SCHEDULE_SET + CONFIG. */
  CommitResult commit(HubRegistry &reg, HubSettings &set, uint8_t *node_out) {
    if (phase_ != ui::WIZ_SCHED) return WZ_COMMIT_BAD_STATE;
    HubNode *n = reg.by_id(node_id_);
    if (!n) return WZ_COMMIT_NO_NODE;

    reg.data().tz_rule = tz_rule_;
    set.ntp_on = ntp_on_ ? 1 : 0;
    snprintf(set.wifi_ssid, sizeof(set.wifi_ssid), "%s", wifi_ssid_);
    snprintf(set.wifi_pass, sizeof(set.wifi_pass), "%s", wifi_pass_);

    n->profile.storage_capacity = cap_total_;
    n->profile.crc16 = profile_crc16(&n->profile);

    /* first schedule: replace the (probably empty) list with one entry */
    n->sched.enabled = 1;
    n->sched.entry_count = 1;
    n->sched.entries[0] = {(uint16_t)s_min_, s_days_, s_amount_};
    n->sched.skip_until = 0;
    n->sched.hold_until = 0;
    n->sched.version = ++reg.data().sched_version;
    n->sched.pushed_at = 0; /* caller pushes SCHEDULE_SET now */

    done_node_ = n->id;
    phase_ = ui::WIZ_DONE;
    if (node_out) *node_out = n->id;
    return WZ_COMMIT_OK;
  }

  /* --- view snapshot ------------------------------------------------------------------ */

  ui::WizardView view() const {
    ui::WizardView v;
    v.step = phase_;
    v.step_no = 0;
    switch (phase_) {
      case ui::WIZ_TZ: v.step_no = 1; break;
      case ui::WIZ_WIFI: v.step_no = 2; break;
      case ui::WIZ_NODE: v.step_no = 3; break;
      case ui::WIZ_CAP: v.step_no = just_paired_ ? 0 : 4; break;
      case ui::WIZ_SCHED: v.step_no = 5; break;
      case ui::WIZ_DONE: v.step_no = 6; break;
      default: break;
    }
    v.skip_cap = just_paired_;
    v.pair_new = pair_new_;
    v.tz_rule = tz_rule_;
    v.ntp_on = ntp_on_;
    snprintf(v.wifi_ssid, sizeof(v.wifi_ssid), "%s", wifi_ssid_);
    v.wifi_state = wifi_state_;
    snprintf(v.wifi_note, sizeof(v.wifi_note), "%s", wifi_note_);

    if (reg_) {
      uint8_t k = 0;
      for (int i = 0; i < HUB_MAX_NODES && k < HUB_MAX_NODES; i++) {
        const HubNode *n = reg_->node(i);
        if (!n || !n->id) continue;
        v.node_ids[k] = n->id;
        snprintf(v.node_names[k], sizeof(v.node_names[k]), "%.12s",
                 n->profile.name[0] ? n->profile.name : "?");
        k++;
      }
      v.node_count = k;
    }
    v.node_sel = node_id_ ? (int8_t)node_id_ : (pair_new_ ? (int8_t)-2 : (int8_t)-1);

    v.cap_total = cap_total_;
    v.cap_now = cap_now_;
    v.pct_full = cap_total_ ? (uint8_t)((cap_now_ * 100 + cap_total_ / 2) / cap_total_) : 0;
    if (v.pct_full > 100) v.pct_full = 100;
    v.to_fill = cap_now_ <= cap_total_ ? cap_total_ - cap_now_ : 0;
    v.amber = v.pct_full <= 20;
    snprintf(v.summary, sizeof(v.summary), "About %u %% full - %lu %s to fill",
             (unsigned)v.pct_full, (unsigned long)v.to_fill, unit_label());

    v.s_min = s_min_;
    v.s_days = s_days_;
    v.s_amount = s_amount_;
    v.feed_cap = feed_cap_units();
    v.unit_ml = strcmp(unit_label(), "ml") == 0;
    snprintf(v.done_node_name, sizeof(v.done_node_name), "%.12s", done_name());
    snprintf(v.note, sizeof(v.note), "%s", note_);
    return v;
  }

 private:
  const char *done_name() const {
    const HubNode *n = reg_ ? reg_->by_id(done_node_) : nullptr;
    return n && n->profile.name[0] ? n->profile.name : "";
  }

  /* drafts + progress back to factory state; the registry binding survives */
  void reset() {
    phase_ = ui::WIZ_IDLE;
    tz_rule_ = TZ_MANILA;
    ntp_on_ = true;
    wifi_ssid_[0] = '\0';
    wifi_pass_[0] = '\0';
    wifi_state_ = -1;
    wifi_note_[0] = '\0';
    node_id_ = 0;
    pair_new_ = false;
    just_paired_ = false;
    cap_total_ = 1500;
    cap_now_ = 0;
    s_min_ = 480;
    s_days_ = 0x7F;
    s_amount_ = 0;
    done_node_ = 0;
    note_[0] = '\0';
  }

  HubRegistry *reg_ = nullptr;
  ui::WizStep phase_ = ui::WIZ_IDLE;

  uint8_t tz_rule_ = TZ_MANILA;
  bool ntp_on_ = true;
  char wifi_ssid_[33] = {0};
  char wifi_pass_[65] = {0};
  int8_t wifi_state_ = -1; /* -1 untested, 0 failed/skipped, 1 ok */
  char wifi_note_[32] = {0};

  uint8_t node_id_ = 0;
  bool pair_new_ = false;
  bool just_paired_ = false;

  uint32_t cap_total_ = 1500;
  uint32_t cap_now_ = 0;

  uint16_t s_min_ = 480;
  uint8_t s_days_ = 0x7F;
  uint16_t s_amount_ = 0;

  uint8_t done_node_ = 0;
  char note_[48] = {0};
};

}  // namespace aio

#endif /* AIO_WIZARD_SESSION_H */
