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
      if (e->kind == EV_FEED_FAIL || e->kind == EV_FEED_REJ ||
          e->kind == EV_FEED_UNKNOWN || e->kind == EV_SKIPPED_MISSED) {
        d.has_alert = true;
        d.alert_node = e->node;
        d.alert_kind = e->kind;
      }
    }
  }
  return d;
}

}  // namespace ui
}  // namespace aio

#endif /* AIO_UI_MODEL_H */
