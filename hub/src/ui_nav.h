/*
 * AIO Feeder — hub UI navigation model (PLAN.md §8, §12 M5: touch wiring).
 *
 * Pure logic, host-tested: which screen a tap opens, the (short) back
 * stack, and the PIN gate on the settings flow (§8 lock). The firmware's
 * ui.cpp renders whatever nav says; the engine services the request flags
 * (Actions) from its own loop, so taps never touch the registry from the
 * LVGL task.
 *
 * Screen graph (M5 touch backend):
 *
 *   DASH ──card tap──► DETAIL ──rows──► EDIT / HIST / ALERTS
 *     └── gear ──► (locked? PIN : SET)          │ back stack (≤4)
 *   PIN ──ok──► SET ──rows──► pane actions (pairing, wizard, backup, …)
 */
#ifndef AIO_UI_NAV_H
#define AIO_UI_NAV_H

#include <stdint.h>

namespace aio {
namespace nav {

enum Screen : uint8_t {
  SC_DASH = 0,
  SC_DETAIL,
  SC_EDIT,
  SC_HIST,
  SC_ALERTS,
  SC_PAIR,
  SC_CAL,
  SC_SET,
  SC_PIN,
  SC_WIZ,
  SC_COUNT
};

/* Back-stack depth: dash → detail → editor is the deepest walk; 4 covers
 * it with slack. Overflow drops the oldest entry (home always wins). */
constexpr uint8_t BACK_MAX = 4;

struct Nav {
  Screen cur = SC_DASH;
  uint8_t node = 0;      /* node id shown by DETAIL/EDIT */
  bool locked = false;   /* mirror of PinLock::blocks_settings() */
  Screen pending = SC_DASH; /* where a successful unlock lands */

  Screen back_stack[BACK_MAX] = {};
  uint8_t back_n = 0;

  void reset() {
    cur = SC_DASH;
    node = 0;
    locked = false;
    pending = SC_DASH;
    back_n = 0;
  }

  void set_locked(bool l) {
    locked = l;
    if (l && (cur == SC_SET || cur == SC_PIN)) {
      /* lock engaged under the user: bounce out to the dashboard (§8) */
      back_n = 0;
      cur = SC_DASH;
    }
  }

  /* Open from a tap. Returns the screen to load. node_id rides along for
   * the per-node screens. */
  Screen open(Screen s, uint8_t node_id) {
    if (s >= SC_COUNT) return cur;
    if (s == SC_DETAIL || s == SC_EDIT) node = node_id;
    if (s == SC_SET && locked) {
      pending = SC_SET;
      return push(SC_PIN);
    }
    return push(s);
  }

  /* PIN result: ok → land on the pending target (settings); !ok → stay. */
  Screen pin_result(bool ok) {
    if (!ok) return SC_PIN;
    locked = false;
    return push(pending == SC_DASH ? SC_SET : pending);
  }

  Screen back() {
    if (back_n == 0) return cur; /* nothing to pop (dash or stale) */
    cur = back_stack[--back_n];
    return cur;
  }

  Screen home() {
    back_n = 0;
    cur = SC_DASH;
    return cur;
  }

  bool can_back() const { return back_n > 0; }

 private:
  Screen push(Screen s) {
    if (s != cur && back_n < BACK_MAX) back_stack[back_n++] = cur;
    cur = s;
    return s;
  }
};

/* --- tap → engine requests -------------------------------------------------
 * The LVGL task sets flags; the engine loop polls ui::take_actions() once
 * per tick and services them (sessions, snapshots, CLI-equivalent paths).
 * One-shot semantics: take clears. */
struct Actions {
  /* detail/editor/history/alerts refresh for the tapped screens */
  bool detail = false;
  uint8_t detail_node = 0;
  bool editor = false;
  uint8_t editor_node = 0;
  bool history = false;
  bool alerts = false;

  /* settings panes / sessions */
  bool pair_open = false;
  bool pair_cancel = false;
  int8_t pair_select = -1;   /* candidate index */
  bool pair_confirm = false; /* code matches the node */
  bool pair_commit = false;
  bool pair_close = false;

  bool wiz_open = false;
  int8_t wiz_tz = -1;        /* row index in the TZ list */
  int8_t wiz_node = -1;      /* existing node id chosen (0 = new node) */
  bool wiz_next = false;
  bool wiz_back = false;
  bool wiz_commit = false;
  bool wiz_close = false;

  bool cal_open = false;
  int8_t cal_test_s = -1;    /* test length pick (s) */
  bool cal_run = false;
  bool cal_next = false;     /* accept the weigh step (defaults) */
  bool cal_save = false;
  bool cal_close = false;

  /* settings panes */
  bool settings = false;     /* entering SET: engine republishes a fresh view */
  int8_t set_section = -1;   /* SetSection index tapped */
  bool backup_export = false;
  bool backup_restore = false;
  bool restart = false;
  bool pin_submit = false;   /* PIN pad OK: digits via ui::pin_entered() */
  int8_t detail_feed = -1;   /* feed preview button on DETAIL (M5b) */

  void clear() { *this = Actions{}; }
  bool any() const {
    return detail || editor || history || alerts || pair_open || pair_cancel ||
           pair_select >= 0 || pair_confirm || pair_commit || pair_close ||
           wiz_open || wiz_tz >= 0 || wiz_node >= 0 || wiz_next || wiz_back ||
           wiz_commit || wiz_close || cal_open || cal_test_s >= 0 || cal_run ||
           cal_next || cal_save || cal_close || settings || set_section >= 0 ||
           backup_export || backup_restore || restart || pin_submit ||
           detail_feed >= 0;
  }
};

}  // namespace nav
}  // namespace aio

#endif /* AIO_UI_NAV_H */
