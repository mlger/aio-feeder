/*
 * AIO Feeder — hub pairing session (PLAN.md §6.3, §8.5; D14, D29).
 *
 * Pure logic, host-tested. Owns the 60 s "Add node" pairing window:
 *
 *   open() → PAIR_REQ arrives → candidate + per-candidate hub nonce + the
 *   4-digit confirmation code (lib/proto/pairing.h, HMAC over both nonces)
 *   → user taps the candidate and confirms the code matches what the node
 *   shows → form (name, mech template, hopper size + amount now, §9.7) →
 *   commit() generates the per-node key, wraps it under the confirmed code
 *   (so the key never crosses the air in the clear, D14) and adds the node
 *   to the registry. The caller sends the returned PAIR_ACCEPT with the
 *   all-zero discovery key (§6.4).
 *
 * Repeat PAIR_REQs refresh RSSI/age but never change the displayed code —
 * the digits must stay stable for the whole confirm step. The window is a
 * fixed 60 s from open(); expiry closes the session and drops everything.
 *
 * Entropy is injectable (begin(fill)): the device passes esp_fill_random;
 * host tests pass a deterministic PRNG. Without a fill function no
 * candidate is ever created — pairing silently refuses to run.
 */
#ifndef AIO_PAIRING_SESSION_H
#define AIO_PAIRING_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../lib/proto/messages.h"
#include "../../lib/proto/pairing.h"
#include "../../lib/proto/profile.h"
#include "hub_registry.h"
#include "ui_model.h"

namespace aio {

constexpr uint32_t PAIR_WINDOW_MS = 60000; /* §6.3.1: "60 s pairing window" */

class PairingSession {
 public:
  using RngFill = void (*)(uint8_t *out, size_t n);

  /* rng = cryptographically secure fill (esp_fill_random on device). */
  void begin(RngFill rng) { rng_ = rng; }

  /* Radio channel PAIR_ACCEPT advertises to the node. */
  void set_channel(uint8_t ch) { channel_ = ch; }
  uint8_t channel() const { return channel_; }

  /* --- lifecycle ------------------------------------------------------------------ */

  /* "Add node" (§6.3.1). Returns false when a window is already open. */
  bool open(uint32_t now_ms) {
    if (phase_ != PH_IDLE) return false;
    cand_count_ = 0;
    sel_ = -1;
    new_node_id_ = 0;
    phase_ = PH_DISCOVER;
    open_ms_ = now_ms;
    return true;
  }

  /* Cancel / window expiry / dismiss after DONE. */
  void close() {
    phase_ = PH_IDLE;
    cand_count_ = 0;
    sel_ = -1;
    new_node_id_ = 0;
  }

  bool is_open() const { return phase_ != PH_IDLE; }

  /* Periodic expiry check; returns true exactly once when the window ends. */
  bool tick(uint32_t now_ms) {
    if (phase_ == PH_IDLE) return false;
    if ((uint32_t)(now_ms - open_ms_) < PAIR_WINDOW_MS) return false;
    close();
    return true;
  }

  uint16_t window_left_s(uint32_t now_ms) const {
    if (phase_ == PH_IDLE) return 0;
    const uint32_t used = (uint32_t)(now_ms - open_ms_);
    if (used >= PAIR_WINDOW_MS) return 0;
    return (uint16_t)((PAIR_WINDOW_MS - used + 999) / 1000);
  }

  /* --- link callback ---------------------------------------------------------------- */

  /* PAIR_REQ (hub role). Returns true when a NEW candidate joined the
   * discovery list. Already-paired MACs (per reg) are ignored; repeats
   * refresh RSSI in any active phase but never change a candidate's code.
   * No new candidates join once the user is past the discovery step. */
  bool on_pair_req(const uint8_t mac[6], const pair_req_payload_t &pr, int8_t rssi,
                   uint32_t now_ms, HubRegistry *reg) {
    if (phase_ == PH_IDLE || phase_ == PH_DONE) return false;
    if (!rng_) return false; /* no entropy — refuse to pair */
    if (reg && reg->by_mac(mac)) return false;
    for (uint8_t i = 0; i < cand_count_; i++) {
      if (!memcmp(cand_[i].mac, mac, 6)) {
        cand_[i].rssi = rssi; /* refresh, keep nonces + code stable */
        return false;
      }
    }
    if (phase_ != PH_DISCOVER) return false;
    if (cand_count_ >= ui::PAIR_MAX_CANDIDATES) return false;
    PairCand &c = cand_[cand_count_];
    memcpy(c.mac, mac, 6);
    memcpy(c.node_nonce, pr.node_nonce, 8);
    rng_(c.hub_nonce, 8);
    pairing_code(mac, c.node_nonce, c.hub_nonce, c.code);
    c.fw_ver = pr.fw_ver;
    c.caps = pr.caps;
    c.rssi = rssi;
    cand_count_++;
    return true;
  }

  /* --- UI actions ------------------------------------------------------------------- */

  uint8_t cand_count() const { return cand_count_; }
  const uint8_t *cand_mac(uint8_t i) const {
    return i < cand_count_ ? cand_[i].mac : nullptr;
  }

  /* User tapped a candidate → code step. */
  bool select(uint8_t idx) {
    if (phase_ != PH_DISCOVER || idx >= cand_count_) return false;
    sel_ = (int8_t)idx;
    phase_ = PH_CODE;
    return true;
  }

  /* User confirmed the node's digits match the hub's → form step, prefilled
   * with the gate template (the UI switches templates when the mech chip
   * changes, via set_form). */
  bool confirm_code() {
    if (phase_ != PH_CODE || sel_ < 0) return false;
    phase_ = PH_FORM;
    form_name_[0] = '\0';
    form_mech_ = 0;
    form_cap_total_ = ui::pair_template_capacity(0);
    form_cap_now_ = 0;
    return true;
  }

  void cancel_code() {
    if (phase_ == PH_CODE) {
      phase_ = PH_DISCOVER;
      sel_ = -1;
    }
  }

  /* UI edits the form before commit (name "" = default "feeder<id>"). */
  void set_form(const char *name, uint8_t mech_sel, uint32_t cap_total,
                uint32_t cap_now) {
    if (mech_sel > 2) mech_sel = 0;
    if (name) {
      strncpy(form_name_, name, sizeof(form_name_) - 1);
      form_name_[sizeof(form_name_) - 1] = '\0';
    } else {
      form_name_[0] = '\0';
    }
    form_mech_ = mech_sel;
    form_cap_total_ = cap_total;
    form_cap_now_ = cap_now;
  }

  /* --- commit (§6.3.5) ---------------------------------------------------------------- */

  enum CommitResult : uint8_t {
    PAIR_COMMIT_OK = 0,
    PAIR_COMMIT_NO_SESSION,     /* not in the form step */
    PAIR_COMMIT_BAD_CAPACITY,   /* §9.7 validation failed */
    PAIR_COMMIT_NO_ENTROPY,
    PAIR_COMMIT_ALREADY_PAIRED, /* mac registered meanwhile */
    PAIR_COMMIT_REG_FULL,       /* 8 nodes max */
  };

  /* Generates the per-node key, wraps it under the confirmed code, adds the
   * node to the registry and returns the PAIR_ACCEPT payload for the caller
   * to send (zero-key discovery frame, §6.4). On OK the session moves to
   * PH_DONE and stays there until close() so the UI can show the result. */
  CommitResult commit(HubRegistry &reg, pair_accept_payload_t *out,
                      uint8_t *out_node_id) {
    if (phase_ != PH_FORM || sel_ < 0) return PAIR_COMMIT_NO_SESSION;
    if (!rng_) return PAIR_COMMIT_NO_ENTROPY;
    if (!ui::pair_capacity_valid(form_cap_total_, form_cap_now_))
      return PAIR_COMMIT_BAD_CAPACITY;
    PairCand &c = cand_[sel_];
    if (reg.by_mac(c.mac)) return PAIR_COMMIT_ALREADY_PAIRED;
    HubNode *n = reg.add(c.mac, channel_);
    if (!n) return PAIR_COMMIT_REG_FULL;

    uint8_t key[32], wrapped[32];
    rng_(key, sizeof(key));
    pairing_wrap_key(key, c.code, c.node_nonce, c.hub_nonce, wrapped);
    memcpy(n->key, key, sizeof(key));
    n->key_len = 32;

    /* profile: template by mech + name + capacity (§8.5), then re-CRC */
    n->profile.mech = (uint8_t)mech_of(form_mech_);
    n->profile.unit = (form_mech_ == 2) ? UNIT_ML : UNIT_GRAMS;
    if (form_name_[0]) {
      strncpy(n->profile.name, form_name_, sizeof(n->profile.name) - 1);
      n->profile.name[sizeof(n->profile.name) - 1] = '\0';
    } else {
      snprintf(n->profile.name, sizeof(n->profile.name), "feeder%u",
               (unsigned)n->id);
    }
    n->profile.storage_capacity = form_cap_total_;
    n->profile.crc16 = profile_crc16(&n->profile);

    if (out) {
      out->node_id = n->id;
      out->channel = channel_;
      memcpy(out->code_digits, c.code, 4);
      memcpy(out->hub_nonce, c.hub_nonce, 8);
      memcpy(out->wrapped_key, wrapped, 32);
    }
    if (out_node_id) *out_node_id = n->id;
    new_node_id_ = n->id;
    phase_ = PH_DONE;
    return PAIR_COMMIT_OK;
  }

  uint8_t new_node_id() const { return new_node_id_; }

  /* --- view snapshot (the LVGL screen renders this; never the session) --------------- */

  ui::PairingView view(uint32_t now_ms) const {
    ui::PairingView v;
    switch (phase_) {
      case PH_DISCOVER: v.step = ui::PAIR_DISCOVER; break;
      case PH_CODE: v.step = ui::PAIR_CODE; break;
      case PH_FORM: v.step = ui::PAIR_FORM; break;
      case PH_DONE: v.step = ui::PAIR_DONE; break;
      default: v.step = ui::PAIR_IDLE; break;
    }
    v.window_left_s = window_left_s(now_ms);
    v.cand_count = cand_count_;
    for (uint8_t i = 0; i < cand_count_; i++) {
      ui::PairCandidateRow &r = v.cand[i];
      memcpy(r.mac, cand_[i].mac, 6);
      r.fw_ver = cand_[i].fw_ver;
      r.rssi = cand_[i].rssi;
      r.selected = ((int8_t)i == sel_);
    }
    if (sel_ >= 0) memcpy(v.code_digits, cand_[sel_].code, 4);
    strncpy(v.name, form_name_, sizeof(v.name) - 1);
    v.name[sizeof(v.name) - 1] = '\0';
    v.mech_sel = form_mech_;
    v.cap_total = form_cap_total_;
    v.cap_now = form_cap_now_;
    ui::pair_capacity_summary(&v);
    v.new_node_id = new_node_id_;
    return v;
  }

 private:
  enum PairPhase : uint8_t { PH_IDLE = 0, PH_DISCOVER, PH_CODE, PH_FORM, PH_DONE };

  struct PairCand {
    uint8_t mac[6];
    uint8_t node_nonce[8];
    uint8_t hub_nonce[8];
    uint8_t code[4]; /* ASCII digits */
    uint16_t fw_ver;
    uint8_t caps;
    int8_t rssi;
  };

  static uint8_t mech_of(uint8_t sel) {
    /* form index order matches mech_t: 0 gate, 1 auger(one-way), 2 pump */
    return sel;
  }

  RngFill rng_ = nullptr;
  uint8_t channel_ = 1;
  PairPhase phase_ = PH_IDLE;
  uint32_t open_ms_ = 0;
  PairCand cand_[ui::PAIR_MAX_CANDIDATES]{};
  uint8_t cand_count_ = 0;
  int8_t sel_ = -1;

  char form_name_[13] = "";
  uint8_t form_mech_ = 0;
  uint32_t form_cap_total_ = 1500;
  uint32_t form_cap_now_ = 0;

  uint8_t new_node_id_ = 0;
};

}  // namespace aio

#endif /* AIO_PAIRING_SESSION_H */
