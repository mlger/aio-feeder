/*
 * AIO Feeder — hub UI model (PLAN.md §7.2 ui_model, §8 dashboard).
 *
 * Pure logic, host-tested: turns the hub state (registry, event log, link
 * peer freshness, wall clock) into the flat "dashboard" snapshot the LVGL
 * view renders. The view code (hub/main/ui) never touches the registry
 * directly — it only reads this snapshot — so the same presenter runs in
 * host tests, the QEMU screenshot build and on hardware.
 *
 * Freshness (§6.2): a node is ONLINE when a valid frame arrived within
 * FRESH_WINDOW_MS (3 missed heartbeats), STALE inside the grace window after
 * that, and OFFLINE when the hub has never heard from it this boot.
 */
#ifndef AIO_UI_MODEL_H
#define AIO_UI_MODEL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "event_log.h"
#include "hub_registry.h"
#include "hub_time.h"
#include "settings_store.h"

namespace aio {
namespace ui {

enum NodeState : uint8_t { NODE_ONLINE = 0, NODE_STALE = 1, NODE_OFFLINE = 2 };

/* 3 missed heartbeats (§6.2 HB_PERIOD_MS = 30 s). */
constexpr uint32_t UI_FRESH_WINDOW_MS = 90000;

/* What the main loop passes in per paired node (from LinkEngine peers). */
struct PeerFresh {
  uint8_t node_id;
  uint32_t last_rx_ms; /* 0 = nothing this boot */
  uint16_t vin_mv;
  uint8_t flags;
};

struct NodeRow {
  uint8_t id = 0;
  NodeState state = NODE_OFFLINE;
  uint16_t vin_mv = 0;
  uint8_t flags = 0;

  /* next scheduled occurrence, forward-looking (display only, §8) */
  bool has_next = false;
  uint16_t next_min_of_day = 0;
  int32_t next_in_min = 0; /* minutes from now (local), always > 0 */
  uint16_t next_amount = 0;

  /* newest terminal event for this node (OK/FAIL/REJ/UNKNOWN) */
  bool has_last = false;
  uint8_t last_kind = 0;
  uint8_t last_result = 0;
  uint16_t last_amount = 0;
  uint32_t last_age_s = 0;
};

struct Dashboard {
  bool time_ok = false;
  char clock[24] = "--:--"; /* "07:21 Sun 01 Nov" or "--:--" */
  uint8_t total = 0;
  uint8_t online = 0; /* NODE_ONLINE rows */

  int32_t next_in_min = -1; /* global next feed, -1 = none scheduled */
  uint16_t next_amount = 0;

  /* newest non-OK terminal/skip event (alert strip, §8) */
  bool has_alert = false;
  uint8_t alert_node = 0;
  uint8_t alert_kind = 0;

  NodeRow rows[HUB_MAX_NODES];
  uint8_t row_count = 0;
};

static inline NodeState state_of(uint32_t now_ms, uint32_t last_rx_ms) {
  if (last_rx_ms == 0) return NODE_OFFLINE;
  const uint32_t age = (uint32_t)(now_ms - last_rx_ms);
  if (age <= UI_FRESH_WINDOW_MS) return NODE_ONLINE;
  if (age <= 3 * UI_FRESH_WINDOW_MS) return NODE_STALE;
  return NODE_OFFLINE; /* ~4.5 min silent: treat as gone */
}

/* Kinds the dashboard alert strip and the alerts screen (§8.7) both surface. */
static inline bool is_alert_kind(uint8_t kind) {
  return kind == EV_FEED_FAIL || kind == EV_FEED_REJ ||
         kind == EV_FEED_UNKNOWN || kind == EV_SKIPPED_MISSED;
}

/* Next future occurrence of a node's schedule, searched across the next
 * 7 days of the days_mask (display approximation of §9.6: exact for the
 * current day, day-granular beyond it). */
static inline bool next_occurrence(const HubNode &n, uint16_t now_min,
                                   uint8_t today_wday, uint16_t *out_min,
                                   int32_t *out_in_min, uint16_t *out_amount) {
  bool found = false;
  int32_t best = 0;
  uint16_t best_min = 0, best_amt = 0;
  for (int32_t d = 0; d < 7 && !found; d++) {
    const uint8_t wd = (uint8_t)((today_wday + d) % 7);
    for (uint8_t i = 0; i < n.sched.entry_count && !found; i++) {
      const sched_entry_t &e = n.sched.entries[i];
      if (!(e.days_mask & (1u << wd))) continue;
      const int32_t ahead = d * 1440 + ((int32_t)e.min_of_day - (int32_t)now_min);
      if (ahead <= 0) continue; /* today but already past */
      best = ahead;
      best_min = e.min_of_day;
      best_amt = e.amount;
      found = true;
    }
  }
  /* entries within one day are not sorted; prefer the earliest minute among
   * today's remaining entries before accepting a later day */
  if (found && best < 1440) {
    for (uint8_t i = 0; i < n.sched.entry_count; i++) {
      const sched_entry_t &e = n.sched.entries[i];
      if (!(e.days_mask & (1u << today_wday))) continue;
      const int32_t ahead = (int32_t)e.min_of_day - (int32_t)now_min;
      if (ahead <= 0) continue;
      if (ahead < best) {
        best = ahead;
        best_min = e.min_of_day;
        best_amt = e.amount;
      }
    }
  }
  if (!found) return false;
  *out_min = best_min;
  *out_in_min = best;
  *out_amount = best_amt;
  return true;
}

static inline Dashboard build_dashboard(const HubRegistry &reg, const EventLog &log,
                                        int64_t unix_now, uint32_t now_ms,
                                        const PeerFresh *fresh, uint8_t fresh_count) {
  Dashboard d;
  d.total = reg.count();

  static const char *WD[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *MO[13] = {"",   "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (unix_now >= 1600000000) {
    const LocalTime t = local_from_unix(unix_now, reg.data().tz_rule);
    int64_t y;
    unsigned m, day;
    civil_from_days(t.day_start / 86400, &y, &m, &day);
    snprintf(d.clock, sizeof(d.clock), "%02u:%02u %s %02u %s",
             (unsigned)(t.min_of_day / 60), (unsigned)(t.min_of_day % 60), WD[t.wday % 7],
             day, MO[m]);
    d.time_ok = true;

    for (int i = 0; i < HUB_MAX_NODES; i++) {
      const HubNode *n = reg.node(i);
      if (!n || !n->id) continue;
      NodeRow &r = d.rows[d.row_count++];
      r.id = n->id;
      for (int f = 0; f < fresh_count; f++) {
        if (fresh[f].node_id == n->id) {
          r.state = state_of(now_ms, fresh[f].last_rx_ms);
          r.vin_mv = fresh[f].vin_mv;
          r.flags = fresh[f].flags;
          if (r.state == NODE_ONLINE && d.online < 255) d.online++;
        }
      }
      if (n->sched.enabled && next_occurrence(*n, t.min_of_day, t.wday, &r.next_min_of_day,
                                              &r.next_in_min, &r.next_amount)) {
        r.has_next = true;
        if (d.next_in_min < 0 || r.next_in_min < d.next_in_min) {
          d.next_in_min = r.next_in_min;
          d.next_amount = r.next_amount;
        }
      }
      /* newest terminal event (scan newest → oldest) */
      for (uint16_t k = log.count(); k > 0 && !r.has_last; k--) {
        const HubEvent *e = log.at((uint16_t)(k - 1));
        if (e->node != n->id) continue;
        if (e->kind == EV_FEED_OK || e->kind == EV_FEED_FAIL || e->kind == EV_FEED_REJ ||
            e->kind == EV_FEED_UNKNOWN) {
          r.has_last = true;
          r.last_kind = e->kind;
          r.last_result = e->result;
          r.last_amount = e->amount;
          r.last_age_s =
              (unix_now > (int64_t)e->unix) ? (uint32_t)(unix_now - (int64_t)e->unix) : 0;
        }
      }
    }

    /* newest alert-worthy event, any node (newest → oldest, first hit wins) */
    for (uint16_t k = log.count(); k > 0 && !d.has_alert; k--) {
      const HubEvent *e = log.at((uint16_t)(k - 1));
      if (is_alert_kind(e->kind)) {
        d.has_alert = true;
        d.alert_node = e->node;
        d.alert_kind = e->kind;
      }
    }
  }
  return d;
}

/* --- node detail (§8.2) --------------------------------------------------------- */

constexpr uint8_t UI_DETAIL_MAX_EVENTS = 5;   /* newest terminal events shown */
constexpr uint8_t UI_DETAIL_MAX_ENTRIES = 6;  /* schedule rows shown */

struct DetailEvent {
  uint8_t kind = 0;
  uint8_t result = 0;
  uint16_t amount = 0;
  uint32_t unix = 0;
  uint32_t age_s = 0;
};

struct DetailSchedEntry {
  uint16_t min_of_day = 0;
  uint16_t amount = 0;
  uint8_t days_mask = 0;
};

struct NodeDetail {
  bool found = false; /* false → "unknown node" */
  NodeState state = NODE_OFFLINE;
  uint16_t vin_mv = 0;
  uint8_t flags = 0;
  bool heard = false;   /* any valid frame this boot */
  uint32_t last_rx_age_s = 0;

  bool sched_enabled = false;
  uint8_t entry_count = 0; /* min(UI_DETAIL_MAX_ENTRIES, n) */
  DetailSchedEntry entries[UI_DETAIL_MAX_ENTRIES]; /* sorted by min_of_day */

  bool has_skip = false; /* skip_until > unix_now */
  bool has_hold = false; /* hold_until > unix_now */
  uint32_t gate_until = 0;   /* later of the two (unix) */
  int32_t gate_left_min = 0; /* minutes left on the active gate */

  bool has_next = false;
  uint16_t next_min_of_day = 0;
  int32_t next_in_min = 0;
  uint16_t next_amount = 0;

  uint8_t event_count = 0; /* newest first */
  DetailEvent events[UI_DETAIL_MAX_EVENTS];

  bool time_ok = false;
  char clock[24] = "--:--";
  char pushed[24] = "never pushed"; /* schedule push freshness */
};

/* Node detail snapshot: schedule (sorted), active gates, next occurrence and
 * the newest terminal events for one node. Pure; host-tested. */
static inline NodeDetail build_node_detail(const HubRegistry &reg, const EventLog &log,
                                           uint8_t node_id, int64_t unix_now,
                                           uint32_t now_ms, const PeerFresh *fresh,
                                           uint8_t fresh_count) {
  NodeDetail d;
  const HubNode *n = reg.by_id(node_id);
  if (!n) return d;
  d.found = true;

  for (int f = 0; f < fresh_count; f++) {
    if (fresh[f].node_id == node_id) {
      d.state = state_of(now_ms, fresh[f].last_rx_ms);
      d.vin_mv = fresh[f].vin_mv;
      d.flags = fresh[f].flags;
      d.heard = fresh[f].last_rx_ms != 0;
      if (d.heard)
        d.last_rx_age_s = (now_ms - fresh[f].last_rx_ms) / 1000;
    }
  }

  d.sched_enabled = n->sched.enabled != 0;
  /* entries sorted by min_of_day (insertion sort, capped display list) */
  for (uint8_t i = 0; i < n->sched.entry_count; i++) {
    const sched_entry_t &e = n->sched.entries[i];
    uint8_t pos = d.entry_count;
    if (pos >= UI_DETAIL_MAX_ENTRIES) {
      /* full: keep it only if it sorts before the newest kept row */
      if (e.min_of_day >= d.entries[pos - 1].min_of_day) continue;
      pos = (uint8_t)(pos - 1);
    } else {
      d.entry_count++;
    }
    while (pos > 0 && d.entries[pos - 1].min_of_day > e.min_of_day) {
      d.entries[pos] = d.entries[pos - 1];
      pos--;
    }
    d.entries[pos].min_of_day = e.min_of_day;
    d.entries[pos].amount = e.amount;
    d.entries[pos].days_mask = e.days_mask;
  }

  if (unix_now >= 1600000000) {
    const LocalTime t = local_from_unix(unix_now, reg.data().tz_rule);
    d.time_ok = true;
    snprintf(d.clock, sizeof(d.clock), "%02u:%02u",
             (unsigned)(t.min_of_day / 60), (unsigned)(t.min_of_day % 60));

    if (n->sched.skip_until > (uint32_t)unix_now) {
      d.has_skip = true;
      d.gate_until = n->sched.skip_until;
    }
    if (n->sched.hold_until > (uint32_t)unix_now) {
      d.has_hold = true;
      if (n->sched.hold_until > d.gate_until) d.gate_until = n->sched.hold_until;
    }
    if (d.gate_until)
      d.gate_left_min = (int32_t)((d.gate_until - (uint32_t)unix_now + 59) / 60);

    if (n->sched.enabled &&
        next_occurrence(*n, t.min_of_day, t.wday, &d.next_min_of_day, &d.next_in_min,
                        &d.next_amount))
      d.has_next = true;

    if (n->sched.pushed_at) {
      const uint32_t age = (uint32_t)(unix_now - (int64_t)n->sched.pushed_at);
      if (age < 90)
        snprintf(d.pushed, sizeof(d.pushed), "pushed just now");
      else if (age < 86400)
        snprintf(d.pushed, sizeof(d.pushed), "pushed %lu h ago",
                 (unsigned long)(age / 3600));
      else
        snprintf(d.pushed, sizeof(d.pushed), "pushed %lu d ago",
                 (unsigned long)(age / 86400));
    }
  }

  for (uint16_t k = log.count(); k > 0 && d.event_count < UI_DETAIL_MAX_EVENTS; k--) {
    const HubEvent *e = log.at((uint16_t)(k - 1));
    if (e->node != node_id) continue;
    if (e->kind != EV_FEED_OK && e->kind != EV_FEED_FAIL && e->kind != EV_FEED_REJ &&
        e->kind != EV_FEED_UNKNOWN)
      continue;
    DetailEvent &de = d.events[d.event_count++];
    de.kind = e->kind;
    de.result = e->result;
    de.amount = e->amount;
    de.unix = e->unix;
    de.age_s = (unix_now > (int64_t)e->unix) ? (uint32_t)(unix_now - (int64_t)e->unix) : 0;
  }
  return d;
}

/* --- schedule editor (§8.3) ------------------------------------------------------ */

struct SchedEditor {
  bool found = false;
  uint8_t node_id = 0;
  bool enabled = false;

  uint8_t entry_count = 0;                         /* sorted display copy */
  DetailSchedEntry entries[UI_DETAIL_MAX_ENTRIES]; /* capped like the detail view */

  int8_t selected = -1;        /* selected row, -1 = none (draft for a new row) */
  uint16_t d_min_of_day = 480; /* draft fields — edited by touch, rendered live */
  uint16_t d_amount = 20;
  uint8_t d_days = 0x7F;

  bool has_skip = false; /* node-level gates (header shows Undo skip / Resume) */
  bool has_hold = false;
  int32_t gate_left_min = 0;

  char tz[16] = "UTC"; /* daylight-saving rule name (§8.3) */
  char clock[8] = "--:--"; /* HH:MM at snapshot time */
};

/* Editor base snapshot. Pure; host-tested. Draft ops below mutate the struct
 * in place; commit/push to the node is an engine command (M4b), not part of
 * the model. */
static inline SchedEditor build_sched_editor(const HubRegistry &reg, uint8_t node_id,
                                             int64_t unix_now) {
  SchedEditor e;
  const HubNode *n = reg.by_id(node_id);
  if (!n) return e;
  e.found = true;
  e.node_id = node_id;
  e.enabled = n->sched.enabled != 0;

  for (uint8_t i = 0; i < n->sched.entry_count; i++) {
    const sched_entry_t &s = n->sched.entries[i];
    uint8_t pos = e.entry_count;
    if (pos >= UI_DETAIL_MAX_ENTRIES) {
      if (s.min_of_day >= e.entries[pos - 1].min_of_day) continue;
      pos = (uint8_t)(pos - 1);
    } else {
      e.entry_count++;
    }
    while (pos > 0 && e.entries[pos - 1].min_of_day > s.min_of_day) {
      e.entries[pos] = e.entries[pos - 1];
      pos--;
    }
    e.entries[pos].min_of_day = s.min_of_day;
    e.entries[pos].amount = s.amount;
    e.entries[pos].days_mask = s.days_mask;
  }

  if (unix_now >= 1600000000) {
    if (n->sched.skip_until > (uint32_t)unix_now) e.has_skip = true;
    if (n->sched.hold_until > (uint32_t)unix_now) e.has_hold = true;
    const uint32_t gate = (n->sched.hold_until > n->sched.skip_until) ? n->sched.hold_until
                                                                      : n->sched.skip_until;
    if (gate > (uint32_t)unix_now)
      e.gate_left_min = (int32_t)((gate - (uint32_t)unix_now + 59) / 60);
  }

  switch (reg.data().tz_rule) {
    case TZ_MANILA: snprintf(e.tz, sizeof(e.tz), "Asia/Manila"); break;
    case TZ_EU_CENTRAL: snprintf(e.tz, sizeof(e.tz), "EU Central"); break;
    case TZ_US_EASTERN: snprintf(e.tz, sizeof(e.tz), "US Eastern"); break;
    default: snprintf(e.tz, sizeof(e.tz), "UTC");
  }
  const LocalTime tnow = local_from_unix(unix_now, reg.data().tz_rule);
  snprintf(e.clock, sizeof(e.clock), "%02u:%02u", (unsigned)(tnow.min_of_day / 60),
           (unsigned)(tnow.min_of_day % 60));

  if (e.entry_count) {
    e.selected = 0;
    e.d_min_of_day = e.entries[0].min_of_day;
    e.d_amount = e.entries[0].amount;
    e.d_days = e.entries[0].days_mask;
  }
  return e;
}

/* Select a row and load it into the draft fields. */
static inline void editor_select(SchedEditor &e, int8_t idx) {
  if (idx < 0 || idx >= (int8_t)e.entry_count) {
    e.selected = -1;
    return;
  }
  e.selected = idx;
  e.d_min_of_day = e.entries[idx].min_of_day;
  e.d_amount = e.entries[idx].amount;
  e.d_days = e.entries[idx].days_mask;
}

/* Draft time clamp 00:00–23:59. */
static inline void editor_set_time(SchedEditor &e, uint16_t min_of_day) {
  if (min_of_day > 1439) min_of_day = 1439;
  e.d_min_of_day = min_of_day;
}

/* Draft amount clamp 1–999 (g or ml). */
static inline void editor_set_amount(SchedEditor &e, uint16_t amount) {
  if (amount < 1) amount = 1;
  if (amount > 999) amount = 999;
  e.d_amount = amount;
}

/* Toggle one weekday bit; the mask never drops to zero (§8.3 weekday toggles). */
static inline void editor_toggle_day(SchedEditor &e, uint8_t wday) {
  if (wday > 6) return;
  const uint8_t bit = (uint8_t)(1u << wday);
  if (e.d_days & bit) {
    if (e.d_days != bit) e.d_days &= (uint8_t)~bit; /* keep at least one day */
  } else {
    e.d_days |= bit;
  }
}

/* Write the draft into the selected row (or insert a new row when none is
 * selected). In-memory only: the caller re-sorts on the next snapshot build.
 * Returns false when the list is full and nothing is selected. */
static inline bool editor_apply_draft(SchedEditor &e) {
  if (e.selected >= 0) {
    DetailSchedEntry &row = e.entries[e.selected];
    row.min_of_day = e.d_min_of_day;
    row.amount = e.d_amount;
    row.days_mask = e.d_days;
    /* keep the display copy sorted */
    for (int8_t i = e.selected; i > 0 &&
                               e.entries[i - 1].min_of_day > e.entries[i].min_of_day;
         i--) {
      const DetailSchedEntry t = e.entries[i];
      e.entries[i] = e.entries[i - 1];
      e.entries[i - 1] = t;
    }
    for (int8_t i = e.selected;
         i < (int8_t)(e.entry_count - 1) && e.entries[i].min_of_day > e.entries[i + 1].min_of_day;
         i++) {
      const DetailSchedEntry t = e.entries[i];
      e.entries[i] = e.entries[i + 1];
      e.entries[i + 1] = t;
    }
    return true;
  }
  if (e.entry_count >= UI_DETAIL_MAX_ENTRIES) return false;
  e.entries[e.entry_count].min_of_day = e.d_min_of_day;
  e.entries[e.entry_count].amount = e.d_amount;
  e.entries[e.entry_count].days_mask = e.d_days;
  e.entry_count++;
  e.selected = (int8_t)(e.entry_count - 1);
  /* insertion-sort the new row into place */
  for (int8_t i = e.selected; i > 0 && e.entries[i - 1].min_of_day > e.entries[i].min_of_day;
       i--) {
    const DetailSchedEntry t = e.entries[i];
    e.entries[i] = e.entries[i - 1];
    e.entries[i - 1] = t;
    e.selected = i;
  }
  return true;
}

/* Remove the selected row; the draft keeps the removed row's values. */
static inline bool editor_remove_selected(SchedEditor &e) {
  if (e.selected < 0 || e.selected >= (int8_t)e.entry_count) return false;
  for (int8_t i = e.selected; i < (int8_t)(e.entry_count - 1); i++)
    e.entries[i] = e.entries[i + 1];
  e.entry_count--;
  if (e.entry_count == 0)
    e.selected = -1;
  else if (e.selected >= (int8_t)e.entry_count)
    e.selected = (int8_t)(e.entry_count - 1);
  return true;
}

/* --- history (§8.6) --------------------------------------------------------------- */

constexpr uint8_t UI_HISTORY_DAYS = 7;
constexpr uint8_t UI_HISTORY_ROWS = 8;

struct HistoryDay {
  uint16_t feeds = 0;
  uint32_t grams = 0;
};

struct HistoryNodeRow {
  uint8_t id = 0;
  uint16_t feeds = 0;
  uint32_t grams = 0;
};

struct HistoryEventRow {
  uint8_t node = 0;
  uint8_t kind = 0;
  uint8_t result = 0;
  uint16_t amount = 0;
  uint32_t age_s = 0;
};

struct HistoryView {
  bool time_ok = false;
  char clock[24] = "--:--";
  HistoryDay days[UI_HISTORY_DAYS]; /* [0] = today … [6] = 6 days ago */
  char day_labels[UI_HISTORY_DAYS][4]; /* "Su", "Mo", … (chart x-axis) */
  uint8_t node_count = 0;
  HistoryNodeRow nodes[HUB_MAX_NODES]; /* OK totals, grams desc */
  uint8_t event_count = 0;
  HistoryEventRow events[UI_HISTORY_ROWS]; /* newest first, any node */
  uint32_t total_feeds = 0; /* 7-day OK totals */
  uint32_t total_grams = 0;
};

/* History snapshot: 7-day OK buckets in local time, per-node OK totals and
 * the newest terminal events. Pure; host-tested. The RAM ring covers at most
 * CAP events — the SD-backed deep history (§7.5) replaces the source later
 * without changing this model. */
static inline HistoryView build_history(const HubRegistry &reg, const EventLog &log,
                                        int64_t unix_now) {
  HistoryView h;
  if (unix_now < 1600000000) return h;
  const LocalTime now_t = local_from_unix(unix_now, reg.data().tz_rule);
  h.time_ok = true;
  {
    static const char *WD[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *MO[13] = {"",   "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int64_t y;
    unsigned m, day;
    civil_from_days(now_t.day_start / 86400, &y, &m, &day);
    snprintf(h.clock, sizeof(h.clock), "%02u:%02u %s %02u %s",
             (unsigned)(now_t.min_of_day / 60), (unsigned)(now_t.min_of_day % 60),
             WD[now_t.wday % 7], day, MO[m]);
  }
  static const char *D2[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
  for (int i = 0; i < UI_HISTORY_DAYS; i++)
    snprintf(h.day_labels[i], sizeof(h.day_labels[i]), "%s",
             D2[(now_t.wday - i + 14) % 7]);

  /* accumulators seeded from the registry so silent nodes still show (0 g) */
  uint16_t node_feeds[HUB_MAX_NODES] = {0};
  uint32_t node_grams[HUB_MAX_NODES] = {0};
  uint8_t node_ids[HUB_MAX_NODES] = {0};
  uint8_t node_n = 0;
  for (int i = 0; i < HUB_MAX_NODES; i++) {
    const HubNode *n = reg.node(i);
    if (!n || !n->id) continue;
    node_ids[node_n] = n->id;
    node_n++;
  }

  for (uint16_t k = log.count(); k > 0; k--) {
    const HubEvent *e = log.at((uint16_t)(k - 1));
    /* index of the node's accumulator (registry order) */
    uint8_t acc = HUB_MAX_NODES;
    for (uint8_t i = 0; i < node_n; i++) {
      if (node_ids[i] == e->node) {
        acc = i;
        break;
      }
    }
    if (acc == HUB_MAX_NODES) continue; /* event for an unpaired id */

    if (e->kind == EV_FEED_OK) {
      const LocalTime et = local_from_unix((int64_t)e->unix, reg.data().tz_rule);
      const int64_t day_diff = (et.day_start - now_t.day_start) / 86400;
      if (day_diff <= 0 && -day_diff < UI_HISTORY_DAYS) {
        const uint8_t idx = (uint8_t)(-day_diff); /* [0]=today … [6]=6 d ago */
        h.days[idx].feeds++;
        h.days[idx].grams += e->amount;
        h.total_feeds++;
        h.total_grams += e->amount;
      }
      node_feeds[acc]++;
      node_grams[acc] += e->amount;
    }

    /* newest-first list (ring order), any terminal kind */
    if (e->kind == EV_FEED_OK || e->kind == EV_FEED_FAIL || e->kind == EV_FEED_REJ ||
        e->kind == EV_FEED_UNKNOWN) {
      if (h.event_count < UI_HISTORY_ROWS) {
        HistoryEventRow &r = h.events[h.event_count++];
        r.node = e->node;
        r.kind = e->kind;
        r.result = e->result;
        r.amount = e->amount;
        r.age_s = (unix_now > (int64_t)e->unix) ? (uint32_t)(unix_now - (int64_t)e->unix) : 0;
      }
    }
  }

  for (uint8_t i = 0; i < node_n; i++) {
    /* include paired nodes even with zero OK feeds (shows silent nodes) */
    h.nodes[h.node_count].id = node_ids[i];
    h.nodes[h.node_count].feeds = node_feeds[i];
    h.nodes[h.node_count].grams = node_grams[i];
    h.node_count++;
  }
  /* grams desc, stable for equal totals (registry order) */
  for (uint8_t i = 1; i < h.node_count; i++) {
    const HistoryNodeRow key = h.nodes[i];
    int8_t j = (int8_t)(i - 1);
    while (j >= 0 && h.nodes[j].grams < key.grams) {
      h.nodes[j + 1] = h.nodes[j];
      j--;
    }
    h.nodes[j + 1] = key;
  }
  return h;
}

/* --- alerts (§8.7) ------------------------------------------------------------------ */

constexpr uint8_t UI_ALERTS_MAX_ACTIVE = 6;
constexpr uint8_t UI_ALERTS_MAX_ACKED = 3;
constexpr uint8_t UI_ALERTS_ACK_CAP = 16; /* ack key ring size */

/* One row on the alerts screen. Two sources:
 *  - condition rows (cond=true): a node is STALE/OFFLINE right now; the row
 *    clears itself when the node is heard again, so it cannot be acked;
 *  - event rows (cond=false): newest alert-worthy events from the log;
 *    acknowledging moves them to the acked list. */
struct AlertRow {
  uint8_t node = 0;
  bool cond = false;
  NodeState state = NODE_OFFLINE; /* cond rows */
  bool never_heard = false;       /* cond rows: no valid frame this boot */
  uint8_t kind = 0;               /* event rows */
  uint8_t result = 0;
  uint16_t amount = 0;
  uint32_t age_s = 0; /* cond: seconds silent; event: event age */
};

struct AlertsView {
  bool time_ok = false;
  char clock[24] = "--:--";
  uint8_t total = 0;
  uint8_t online = 0;
  uint8_t active_count = 0;
  AlertRow active[UI_ALERTS_MAX_ACTIVE]; /* conditions first, then newest events */
  uint8_t acked_count = 0;
  AlertRow acked[UI_ALERTS_MAX_ACKED]; /* newest acked first */
};

/* Acknowledged-alert keys: a small RAM ring of (unix, node, kind) triples.
 * The engine owns one instance; build_alerts only reads it. RAM-only until
 * the SD-backed history lands (§7.5) — ack state resets on reboot, matching
 * the event ring's lifetime. */
class AlertsAck {
 public:
  void ack(uint32_t unix, uint8_t node, uint8_t kind) {
    if (is_acked(unix, node, kind)) return;
    AlertKey &k = keys_[head_];
    k.unix = unix;
    k.node = node;
    k.kind = kind;
    head_ = (uint8_t)((head_ + 1) % UI_ALERTS_ACK_CAP);
    if (count_ < UI_ALERTS_ACK_CAP) count_++;
  }

  bool is_acked(uint32_t unix, uint8_t node, uint8_t kind) const {
    for (uint8_t i = 0; i < count_; i++) {
      const AlertKey &k = keys_[i];
      if (k.unix == unix && k.node == node && k.kind == kind) return true;
    }
    return false;
  }

  uint8_t count() const { return count_; }

 private:
  struct AlertKey {
    uint32_t unix = 0;
    uint8_t node = 0;
    uint8_t kind = 0;
  };
  AlertKey keys_[UI_ALERTS_ACK_CAP];
  uint8_t head_ = 0;
  uint8_t count_ = 0;
};

/* Alerts snapshot (§8.7 "active and acknowledged"): condition rows for nodes
 * that are not ONLINE, then alert-worthy events newest-first, with acked
 * events moved to the acked list. Pure; host-tested. */
static inline AlertsView build_alerts(const HubRegistry &reg, const EventLog &log,
                                      int64_t unix_now, uint32_t now_ms,
                                      const PeerFresh *fresh, uint8_t fresh_count,
                                      const AlertsAck &ack) {
  AlertsView v;
  if (unix_now < 1600000000) return v;
  v.time_ok = true;
  {
    static const char *WD[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *MO[13] = {"",   "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const LocalTime t = local_from_unix(unix_now, reg.data().tz_rule);
    int64_t y;
    unsigned m, day;
    civil_from_days(t.day_start / 86400, &y, &m, &day);
    snprintf(v.clock, sizeof(v.clock), "%02u:%02u %s %02u %s",
             (unsigned)(t.min_of_day / 60), (unsigned)(t.min_of_day % 60), WD[t.wday % 7],
             day, MO[m]);
  }

  /* condition rows + online tally (registry order, same as the dashboard) */
  for (int i = 0; i < HUB_MAX_NODES; i++) {
    const HubNode *n = reg.node(i);
    if (!n || !n->id) continue;
    v.total++;
    NodeState st = NODE_OFFLINE;
    bool heard = false;
    uint32_t last_rx = 0;
    for (int f = 0; f < fresh_count; f++) {
      if (fresh[f].node_id == n->id) {
        st = state_of(now_ms, fresh[f].last_rx_ms);
        heard = fresh[f].last_rx_ms != 0;
        last_rx = fresh[f].last_rx_ms;
      }
    }
    if (st == NODE_ONLINE) v.online++;
    if (st != NODE_ONLINE && v.active_count < UI_ALERTS_MAX_ACTIVE) {
      AlertRow &r = v.active[v.active_count++];
      r.node = n->id;
      r.cond = true;
      r.state = st;
      r.never_heard = !heard;
      r.age_s = heard ? (now_ms - last_rx) / 1000 : 0;
    }
  }

  /* event rows, newest → oldest */
  for (uint16_t k = log.count(); k > 0; k--) {
    const HubEvent *e = log.at((uint16_t)(k - 1));
    if (!is_alert_kind(e->kind)) continue;
    const bool was_acked = ack.is_acked(e->unix, e->node, e->kind);
    if (was_acked && v.acked_count >= UI_ALERTS_MAX_ACKED) continue;
    if (!was_acked && v.active_count >= UI_ALERTS_MAX_ACTIVE) continue;
    AlertRow &r = was_acked ? v.acked[v.acked_count++] : v.active[v.active_count++];
    r.node = e->node;
    r.cond = false;
    r.kind = e->kind;
    r.result = e->result;
    r.amount = e->amount;
    r.age_s = (unix_now > (int64_t)e->unix) ? (uint32_t)(unix_now - (int64_t)e->unix) : 0;
  }
  return v;
}

/* --- pairing (§6.3, §8.5, D14/D29) --------------------------------------------------- */

constexpr uint8_t PAIR_MAX_CANDIDATES = 4; /* discovery list size */
constexpr uint32_t PAIR_CAP_MIN = 1;       /* §9.7: capacity range */
constexpr uint32_t PAIR_CAP_MAX = 100000;

/* The three steps of the pairing screen, one PairingView snapshot per step.
 * PAIR_IDLE = no pairing window open (dashboard shows nothing). */
enum PairStep : uint8_t {
  PAIR_IDLE = 0,   /* window closed */
  PAIR_DISCOVER,   /* window open — discovery list */
  PAIR_CODE,       /* 4-digit code shown, user compares with the node */
  PAIR_FORM,       /* name, mech template, hopper size + amount now */
  PAIR_DONE,       /* committed, PAIR_ACCEPT sent */
};

struct PairCandidateRow {
  uint8_t mac[6] = {0};
  uint16_t fw_ver = 0;
  int8_t rssi = 0;
  bool selected = false;
};

struct PairingView {
  PairStep step = PAIR_IDLE;
  uint16_t window_left_s = 0; /* seconds left of the 60 s window */

  uint8_t cand_count = 0;
  PairCandidateRow cand[PAIR_MAX_CANDIDATES];

  uint8_t code_digits[4] = {'-', '-', '-', '-'}; /* ASCII, PAIR_CODE step */

  /* form state (PAIR_FORM); cap_* carry the template defaults until edited */
  char name[13] = "";  /* 12 chars + NUL; "" = default "feeder<id>" */
  uint8_t mech_sel = 0;      /* 0 gate · 1 auger · 2 pump */
  uint32_t cap_total = 1500; /* full hopper/reservoir (g or ml) */
  uint32_t cap_now = 0;      /* amount in it now */

  /* live capacity summary (§8 UX rules) */
  uint8_t pct_full = 0;
  uint32_t to_fill = 0;
  bool amber = false; /* ≤ 20 % full */
  char summary[48] = "";

  uint8_t new_node_id = 0; /* PAIR_DONE: id assigned by the registry */
};

/* Mech template defaults (§8.5): 1500 g gate / 5000 g auger / 1000 ml pump. */
static inline uint32_t pair_template_capacity(uint8_t mech_sel) {
  switch (mech_sel) {
    case 1: return 5000; /* auger */
    case 2: return 1000; /* pump */
    default: return 1500; /* gate */
  }
}
static inline const char *pair_template_unit(uint8_t mech_sel) {
  return mech_sel == 2 ? "ml" : "g";
}

/* §9.7: reject total out of [1, 100000] and amount above total. */
static inline bool pair_capacity_valid(uint32_t cap_total, uint32_t cap_now) {
  return cap_total >= PAIR_CAP_MIN && cap_total <= PAIR_CAP_MAX &&
         cap_now <= cap_total;
}

/* Fill the live summary fields from cap_total/cap_now (call after editing). */
static inline void pair_capacity_summary(PairingView *v) {
  if (!v) return;
  v->pct_full = v->cap_total
                    ? (uint8_t)((v->cap_now * 100 + v->cap_total / 2) / v->cap_total)
                    : 0;
  if (v->pct_full > 100) v->pct_full = 100;
  v->to_fill = v->cap_now <= v->cap_total ? v->cap_total - v->cap_now : 0;
  v->amber = v->pct_full <= 20;
  snprintf(v->summary, sizeof(v->summary), "About %u %% full - %lu %s to fill",
           (unsigned)v->pct_full, (unsigned long)v->to_fill,
           pair_template_unit(v->mech_sel));
}

/* --- calibration wizard (§8.4, §9.3; FR4, D27) --------------------------------------- */

constexpr uint8_t CAL_MAX_SAMPLES = 5; /* §9.3.5: repeat, use the median */
/* §9.3.3: anything under 5 g (or ml) is too small to weigh reliably. */
constexpr uint32_t CAL_MIN_WEIGH_X100 = 500;

/* The steps of the calibration screen, one CalibView snapshot per step.
 * CAL_IDLE = no wizard session open. */
enum CalStep : uint8_t {
  CAL_IDLE = 0,
  CAL_RUNTIME,  /* pick the test length; shows commanded grams + samples */
  CAL_DISPENSE, /* test FEED in flight, waiting for FEED_RESULT */
  CAL_WEIGH,    /* enter the weighed amount for the run that just landed */
  CAL_DONE,     /* median saved into the profile and pushed */
};

struct CalSampleRow {
  uint32_t on_ms;      /* actual_on_ms the node reported for the run */
  uint32_t grams_x100; /* weighed amount × 100 */
  uint16_t rate_x100;  /* ms per unit × 100 derived from this run */
};

struct CalibView {
  CalStep step = CAL_IDLE;
  uint8_t node_id = 0;
  char name[13] = "";          /* profile name, NUL-terminated */
  uint8_t mech_sel = 0;        /* mech_t order: 0 gate · 1 auger · 2 pump */
  uint16_t old_rate_x100 = 0;  /* profile rate when the wizard opened */
  uint32_t test_s = 2;         /* chosen test run length */
  uint32_t max_test_s = 10;    /* node max on-time (SR2/D27) */
  uint32_t test_grams = 0;     /* commanded amount for test_s at old_rate */
  uint32_t left_s = 0;         /* dispense timeout countdown */
  uint32_t weigh_on_ms = 0;    /* on-time held for the weigh step */
  uint8_t sample_count = 0;
  CalSampleRow samples[CAL_MAX_SAMPLES];
  uint16_t median_x100 = 0;    /* median of the samples so far (0 = none) */
  uint8_t saved_node_id = 0;   /* DONE step */
  char note[48] = "";          /* last rejection / timeout, amber in the UI */
};

/* Rate for display as g/s ×10 (§8.4: "results show as g/s").
 * rate_x100 = ms per unit × 100 → g/s ×10 = 1,000,000 / rate_x100. */
static inline uint32_t cal_g_per_s_x10(uint16_t rate_x100) {
  return rate_x100 ? 1000000u / rate_x100 : 0;
}

/* Unit label for a mech selection (matches the pairing form: pump = ml). */
static inline const char *cal_unit_label(uint8_t mech_sel) {
  return mech_sel == 2 ? "ml" : "g";
}

/* --- setup wizard view (§8.9, §9.13; M4b) -------------------------------------------- */

/* Wizard steps; WIZ_CAP is skipped for a just-paired node (§9.13) — the
 * session jumps NODE → SCHED and view().step_no collapses accordingly. */
enum WizStep : uint8_t {
  WIZ_IDLE = 0,
  WIZ_TZ,
  WIZ_WIFI,
  WIZ_NODE,
  WIZ_CAP,
  WIZ_SCHED,
  WIZ_DONE,
};

struct WizardView {
  WizStep step = WIZ_IDLE;
  uint8_t step_no = 0; /* 1..6 for the header counter (0 = idle) */
  bool skip_cap = false;
  bool pair_new = false;

  uint8_t tz_rule = TZ_MANILA;
  bool ntp_on = true;

  char wifi_ssid[33] = "";
  int8_t wifi_state = -1; /* -1 untested, 0 failed/skipped, 1 ok */
  char wifi_note[32] = "";

  uint8_t node_count = 0;
  uint8_t node_ids[HUB_MAX_NODES] = {0};
  char node_names[HUB_MAX_NODES][13] = {};
  int8_t node_sel = -1; /* -1 none, -2 = pair new, else node id */

  uint32_t cap_total = 1500;
  uint32_t cap_now = 0;
  uint8_t pct_full = 0;
  uint32_t to_fill = 0;
  bool amber = false;
  char summary[48] = "";

  uint16_t s_min = 480;
  uint8_t s_days = 0x7F;
  uint16_t s_amount = 0;
  uint32_t feed_cap = 0; /* node per-feed cap (SR2 / rate) */
  bool unit_ml = false;  /* pump profiles dispense ml */

  char done_node_name[13] = "";
  char note[48] = "";
};

/* --- settings view (§8.8; M4b) -------------------------------------------------------- */

constexpr uint8_t UI_SET_SECTION_COUNT = 10;
/* §8.8 jump bar: Restart, Display, Time, Network, Remote, Security, Storage,
 * Backup, Wizard, Danger (Backup and Wizard are the §8.8 sub-entries). */
enum SetSection : uint8_t {
  SET_SEC_RESTART = 0,
  SET_SEC_DISPLAY,
  SET_SEC_TIME,
  SET_SEC_NETWORK,
  SET_SEC_REMOTE,
  SET_SEC_SECURITY,
  SET_SEC_STORAGE,
  SET_SEC_BACKUP,
  SET_SEC_WIZARD,
  SET_SEC_DANGER,
};

struct SettingsView {
  bool time_ok = false;
  char clock[24] = "--:--";
  uint8_t section = 0; /* SetSection pane shown on the right */

  /* display */
  uint8_t brightness = SET_BRIGHTNESS_DEF;
  uint16_t screen_off_s = SET_SCREEN_OFF_DEF;

  /* time */
  uint8_t tz_rule = TZ_UTC;
  bool ntp_on = true;
  char time_src[16] = "none";

  /* network */
  bool wifi_set = false;
  char wifi_ssid[33] = "";
  uint8_t chan = 1;

  /* security */
  bool pin_enabled = false;
  uint8_t pin_scope = PIN_SCOPE_SETTINGS;
  bool locked_now = false;
  uint16_t unlock_left_s = 0;

  /* storage / backup */
  uint8_t retention_days = SET_RETENTION_DEF;
  char sd_status[24] = "no card";
  uint8_t backup_count = 0;
  char backup_last[32] = ""; /* newest aio-backup-*.json (BACKUP_NAME_LEN) */

  /* danger */
  uint8_t node_count = 0;
};

/* Settings snapshot: pure data pass-through of registry + settings + the
 * environment the firmware injects (SD status, backup file count). */
static inline SettingsView build_settings(const HubRegistry &reg,
                                          const HubSettings &set, int64_t unix_now,
                                          const char *sd_status, uint8_t backup_count,
                                          const char *backup_last, bool locked_now,
                                          uint16_t unlock_left_s) {
  SettingsView v;
  v.section = 0;
  v.brightness = set.brightness;
  v.screen_off_s = set.screen_off_s;
  v.tz_rule = reg.data().tz_rule;
  v.ntp_on = set.ntp_on != 0;
  if (unix_now >= 1600000000) {
    v.time_ok = true;
    static const char *WD[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *MO[13] = {"",   "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const LocalTime t = local_from_unix(unix_now, reg.data().tz_rule);
    int64_t y;
    unsigned m, day;
    civil_from_days(t.day_start / 86400, &y, &m, &day);
    snprintf(v.clock, sizeof(v.clock), "%02u:%02u %s %02u %s",
             (unsigned)(t.min_of_day / 60), (unsigned)(t.min_of_day % 60),
             WD[t.wday % 7], day, MO[m]);
    snprintf(v.time_src, sizeof(v.time_src), "ntp-or-rtc");
  } else {
    snprintf(v.time_src, sizeof(v.time_src), "none");
  }
  v.wifi_set = set.wifi_ssid[0] != '\0';
  snprintf(v.wifi_ssid, sizeof(v.wifi_ssid), "%s", set.wifi_ssid);
  v.chan = 1; /* ESP-NOW channel: LinkEngine advertises it; settings pane is
               * informational here (the `chan` CLI owns changes) */
  v.pin_enabled = set.pin_enabled != 0;
  v.pin_scope = set.pin_scope;
  v.locked_now = locked_now;
  v.unlock_left_s = unlock_left_s;
  v.retention_days = set.history_days;
  snprintf(v.sd_status, sizeof(v.sd_status), "%s", sd_status ? sd_status : "no card");
  v.backup_count = backup_count;
  snprintf(v.backup_last, sizeof(v.backup_last), "%s", backup_last ? backup_last : "");
  v.node_count = reg.count();
  return v;
}

/* Day-mask → preset name for the wizard's schedule step (same vocabulary as
 * days_text, kept label-only for the model). */
static inline const char *wiz_days_preset(uint8_t mask) {
  if (mask == 0x7F) return "every day";
  if (mask == 0x1F) return "weekdays";
  if (mask == 0x60) return "weekends";
  return "custom";
}


}  // namespace ui
}  // namespace aio

#endif /* AIO_UI_MODEL_H */
