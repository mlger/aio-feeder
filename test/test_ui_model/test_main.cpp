/* Host tests for the hub UI model (PLAN.md §7.2 ui_model, §8, §12 M4a).
 *
 * Fixed scene: Sunday 2026-11-01, 07:21 local (Asia/Manila, UTC+8). Three
 * paired nodes — #1 and #2 fresh (sim heartbeats), #3 silent (offline).
 * Schedules: #1 07:30 @40 g daily, #2 07:35 @25 g daily, #3 06:50 @30 g
 * daily (already past → tomorrow). Event log: #1 fed OK 40 min ago, #2 fed
 * OK 26 h ago, #3's morning feed was missed → alert. */
#include <string.h>

#include <unity.h>

#include "event_log.h"
#include "hub_registry.h"
#include "hub_time.h"
#include "ui_model.h"

using namespace aio;
using aio::ui::Dashboard;
using aio::ui::PeerFresh;

static int64_t g_unix; /* 2026-11-01 07:21 +08:00 */
static uint32_t g_now_ms = 60000;
static HubRegistry reg;
static EventLog log_;

void setUp(void) {
  g_unix = aio::days_from_civil(2026, 11, 1) * 86400 + (7 * 3600 + 21 * 60) - 8 * 3600;
  g_now_ms = 60000;
  reg.reset();
  reg.data().tz_rule = TZ_MANILA;
  log_ = EventLog{};
}

void tearDown(void) {}

static HubNode *add_node(uint8_t keyc, uint16_t min_of_day, uint16_t amount) {
  const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, (uint8_t)(0x10 + reg.count() + 1)};
  HubNode *n = reg.add(mac, 1);
  TEST_ASSERT_NOT_NULL(n);
  memset(n->key, keyc, 32);
  n->key_len = 32;
  n->sched.entry_count = 1;
  n->sched.entries[0].min_of_day = min_of_day;
  n->sched.entries[0].days_mask = 0x7F;
  n->sched.entries[0].amount = amount;
  return n;
}

/* fresh: #1 rx 5 s ago, #2 rx 2 min ago; #3 absent */
static const PeerFresh *fresh_table(uint8_t *count) {
  static PeerFresh f[2];
  f[0] = PeerFresh{1, g_now_ms - 5000, 12000, 0};
  f[1] = PeerFresh{2, g_now_ms - 120000, 11900, 0};
  *count = 2;
  return f;
}

static void seed_events() {
  /* ids assigned by add order: 1, 2, 3 */
  log_.push((uint32_t)(g_unix - 2400), 1, EV_FEED_OK, FEED_OK, 1001, 40, 0);
  log_.push((uint32_t)(g_unix - 26 * 3600), 2, EV_FEED_OK, FEED_OK, 1002, 25, 0);
  log_.push((uint32_t)(g_unix - 1800), 3, EV_SKIPPED_MISSED, 0, 0, 30, 1);
}

static void test_empty_registry_renders_safe_defaults(void) {
  const Dashboard d = aio::ui::build_dashboard(reg, log_, 0, g_now_ms, nullptr, 0);
  TEST_ASSERT_FALSE(d.time_ok);
  TEST_ASSERT_EQUAL_STRING("--:--", d.clock);
  TEST_ASSERT_EQUAL_UINT8(0, d.row_count);
  TEST_ASSERT_EQUAL_INT32(-1, d.next_in_min);
  TEST_ASSERT_FALSE(d.has_alert);
}

static void test_rows_states_and_freshness(void) {
  add_node(0xA5, 450, 40);
  add_node(0x5A, 455, 25);
  add_node(0x66, 410, 30);
  uint8_t fc;
  const PeerFresh *f = fresh_table(&fc);
  const Dashboard d = aio::ui::build_dashboard(reg, log_, g_unix, g_now_ms, f, fc);

  TEST_ASSERT_EQUAL_UINT8(3, d.row_count);
  TEST_ASSERT_EQUAL_UINT8(3, d.total);
  TEST_ASSERT_EQUAL_UINT8(1, d.online); /* #1 fresh, #2 stale, #3 silent */

  TEST_ASSERT_EQUAL_UINT8(1, d.rows[0].id);
  TEST_ASSERT_EQUAL_INT(aio::ui::NODE_ONLINE, d.rows[0].state);
  TEST_ASSERT_EQUAL_UINT16(12000, d.rows[0].vin_mv);

  TEST_ASSERT_EQUAL_UINT8(2, d.rows[1].id);
  TEST_ASSERT_EQUAL_INT(aio::ui::NODE_STALE, d.rows[1].state); /* 120 s > 90 s window */

  TEST_ASSERT_EQUAL_UINT8(3, d.rows[2].id);
  TEST_ASSERT_EQUAL_INT(aio::ui::NODE_OFFLINE, d.rows[2].state);
}

static void test_next_feed_today_tomorrow_and_global(void) {
  add_node(0xA5, 450, 40);  /* 07:30 → in 9 min */
  add_node(0x5A, 455, 25);  /* 07:35 → in 14 min */
  add_node(0x66, 410, 30);  /* 06:50 passed → tomorrow 06:50 → 1409 min */
  uint8_t fc;
  const PeerFresh *f = fresh_table(&fc);
  const Dashboard d = aio::ui::build_dashboard(reg, log_, g_unix, g_now_ms, f, fc);

  TEST_ASSERT_TRUE(d.rows[0].has_next);
  TEST_ASSERT_EQUAL_UINT16(450, d.rows[0].next_min_of_day);
  TEST_ASSERT_EQUAL_INT32(9, d.rows[0].next_in_min);
  TEST_ASSERT_EQUAL_UINT16(40, d.rows[0].next_amount);

  TEST_ASSERT_EQUAL_INT32(1409, d.rows[2].next_in_min);

  TEST_ASSERT_EQUAL_INT32(9, d.next_in_min);
  TEST_ASSERT_EQUAL_UINT16(40, d.next_amount);
}

static void test_next_feed_unsorted_today_entries(void) {
  /* two entries today, later one first in the array → earliest must win */
  HubNode *n = add_node(0xA5, 540, 10); /* 09:00 */
  n->sched.entry_count = 2;
  n->sched.entries[1].min_of_day = 460; /* 07:40 */
  n->sched.entries[1].days_mask = 0x7F;
  n->sched.entries[1].amount = 15;
  const Dashboard d = aio::ui::build_dashboard(reg, log_, g_unix, g_now_ms, nullptr, 0);
  TEST_ASSERT_EQUAL_INT32(19, d.next_in_min); /* 07:40 − 07:21 */
  TEST_ASSERT_EQUAL_UINT16(15, d.next_amount);
}

static void test_last_event_and_alert(void) {
  add_node(0xA5, 450, 40);
  add_node(0x5A, 455, 25);
  add_node(0x66, 410, 30);
  seed_events();
  uint8_t fc;
  const PeerFresh *f = fresh_table(&fc);
  const Dashboard d = aio::ui::build_dashboard(reg, log_, g_unix, g_now_ms, f, fc);

  TEST_ASSERT_TRUE(d.rows[0].has_last);
  TEST_ASSERT_EQUAL_UINT8(EV_FEED_OK, d.rows[0].last_kind);
  TEST_ASSERT_EQUAL_UINT16(40, d.rows[0].last_amount);
  TEST_ASSERT_EQUAL_UINT32(2400, d.rows[0].last_age_s);

  TEST_ASSERT_TRUE(d.has_alert);
  TEST_ASSERT_EQUAL_UINT8(3, d.alert_node);
  TEST_ASSERT_EQUAL_UINT8(EV_SKIPPED_MISSED, d.alert_kind);
}

static void test_clock_format_manila(void) {
  add_node(0xA5, 450, 40);
  const Dashboard d = aio::ui::build_dashboard(reg, log_, g_unix, g_now_ms, nullptr, 0);
  TEST_ASSERT_TRUE(d.time_ok);
  TEST_ASSERT_EQUAL_STRING("07:21 Sun 01 Nov", d.clock);
}

static void test_state_boundaries(void) {
  TEST_ASSERT_EQUAL_INT(aio::ui::NODE_OFFLINE, aio::ui::state_of(60000, 0));
  TEST_ASSERT_EQUAL_INT(aio::ui::NODE_ONLINE, aio::ui::state_of(91000, 1000));
  TEST_ASSERT_EQUAL_INT(aio::ui::NODE_ONLINE,
                        aio::ui::state_of(60000, 60000 - aio::ui::UI_FRESH_WINDOW_MS));
  TEST_ASSERT_EQUAL_INT(aio::ui::NODE_STALE,
                        aio::ui::state_of(60000, 60000 - aio::ui::UI_FRESH_WINDOW_MS - 1));
  TEST_ASSERT_EQUAL_INT(aio::ui::NODE_OFFLINE,
                        aio::ui::state_of(60000, 60000 - 3 * aio::ui::UI_FRESH_WINDOW_MS - 1));
}

/* --- node detail ------------------------------------------------------------------ */

static void test_detail_unknown_node(void) {
  add_node(0xA5, 450, 40);
  const aio::ui::NodeDetail d =
      aio::ui::build_node_detail(reg, log_, 99, g_unix, g_now_ms, nullptr, 0);
  TEST_ASSERT_FALSE(d.found);
}

static void test_detail_schedule_sorted_and_next(void) {
  HubNode *n = add_node(0xA5, 540, 10); /* 09:00 added first */
  n->sched.entry_count = 3;
  n->sched.entries[1].min_of_day = 420; /* 07:00 */
  n->sched.entries[1].days_mask = 0x7F;
  n->sched.entries[1].amount = 8;
  n->sched.entries[2].min_of_day = 780; /* 13:00 */
  n->sched.entries[2].days_mask = 0x7F;
  n->sched.entries[2].amount = 12;

  uint8_t fc;
  const PeerFresh *f = fresh_table(&fc);
  const aio::ui::NodeDetail d =
      aio::ui::build_node_detail(reg, log_, 1, g_unix, g_now_ms, f, fc);

  TEST_ASSERT_TRUE(d.found);
  TEST_ASSERT_TRUE(d.sched_enabled);
  TEST_ASSERT_EQUAL_UINT8(3, d.entry_count);
  TEST_ASSERT_EQUAL_UINT16(420, d.entries[0].min_of_day);
  TEST_ASSERT_EQUAL_UINT16(540, d.entries[1].min_of_day);
  TEST_ASSERT_EQUAL_UINT16(780, d.entries[2].min_of_day);

  TEST_ASSERT_TRUE(d.has_next);
  TEST_ASSERT_EQUAL_UINT16(540, d.next_min_of_day); /* 09:00 is next after 07:21 */
  TEST_ASSERT_EQUAL_INT32(99, d.next_in_min);
  TEST_ASSERT_EQUAL_UINT16(10, d.next_amount);
  TEST_ASSERT_FALSE(d.has_skip);
  TEST_ASSERT_FALSE(d.has_hold);
}

static void test_detail_gates_skip_and_hold(void) {
  HubNode *n = add_node(0xA5, 450, 40);
  n->sched.skip_until = (uint32_t)(g_unix + 45 * 60);
  n->sched.hold_until = (uint32_t)(g_unix + 90 * 60);
  n->sched.pushed_at = (uint32_t)(g_unix - 3 * 3600);

  const aio::ui::NodeDetail d =
      aio::ui::build_node_detail(reg, log_, 1, g_unix, g_now_ms, nullptr, 0);
  TEST_ASSERT_TRUE(d.has_skip);
  TEST_ASSERT_TRUE(d.has_hold);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)g_unix + 90 * 60, d.gate_until);
  TEST_ASSERT_EQUAL_INT32(90, d.gate_left_min); /* ceil to the minute */
  TEST_ASSERT_EQUAL_STRING("pushed 3 h ago", d.pushed);
}

static void test_detail_expired_gate_not_shown(void) {
  HubNode *n = add_node(0xA5, 450, 40);
  n->sched.hold_until = (uint32_t)(g_unix - 60);
  const aio::ui::NodeDetail d =
      aio::ui::build_node_detail(reg, log_, 1, g_unix, g_now_ms, nullptr, 0);
  TEST_ASSERT_FALSE(d.has_hold);
  TEST_ASSERT_EQUAL_INT32(0, d.gate_left_min);
}

static void test_detail_events_newest_first_capped(void) {
  add_node(0xA5, 450, 40);
  /* 6 terminal events for node 1; push oldest first so ring order == time order */
  for (int i = 6; i >= 1; i--)
    log_.push((uint32_t)(g_unix - i * 600), 1, EV_FEED_OK, FEED_OK, 2000 + i, 10, 0);
  log_.push((uint32_t)(g_unix - 30), 2, EV_FEED_OK, FEED_OK, 2006, 5, 0); /* other node */

  const aio::ui::NodeDetail d =
      aio::ui::build_node_detail(reg, log_, 1, g_unix, g_now_ms, nullptr, 0);
  TEST_ASSERT_EQUAL_UINT8(aio::ui::UI_DETAIL_MAX_EVENTS, d.event_count);
  /* newest first: node-1 events at ages 600, 1200, ... (node-2 event excluded) */
  TEST_ASSERT_EQUAL_UINT32(600, d.events[0].age_s);
  TEST_ASSERT_EQUAL_UINT16(10, d.events[0].amount);
  for (int i = 0; i < d.event_count; i++)
    TEST_ASSERT_EQUAL_UINT32(600 * (i + 1), d.events[i].age_s);
}

static void test_detail_state_freshness_and_offline(void) {
  add_node(0xA5, 450, 40);
  add_node(0x66, 410, 30);
  add_node(0x77, 400, 20); /* id 3 exists but is absent from the fresh table */
  uint8_t fc;
  const PeerFresh *f = fresh_table(&fc);
  const aio::ui::NodeDetail d1 =
      aio::ui::build_node_detail(reg, log_, 1, g_unix, g_now_ms, f, fc);
  TEST_ASSERT_EQUAL_INT(aio::ui::NODE_ONLINE, d1.state);
  TEST_ASSERT_EQUAL_UINT32(5, d1.last_rx_age_s); /* seconds, not ms */
  TEST_ASSERT_TRUE(d1.heard);

  const aio::ui::NodeDetail d3 =
      aio::ui::build_node_detail(reg, log_, 3, g_unix, g_now_ms, f, fc);
  TEST_ASSERT_EQUAL_INT(aio::ui::NODE_OFFLINE, d3.state);
  TEST_ASSERT_FALSE(d3.heard);
  TEST_ASSERT_TRUE(d3.time_ok); /* wall clock still valid for an offline node */
  TEST_ASSERT_EQUAL_STRING("07:21", d3.clock);
}

static void test_detail_time_invalid_keeps_defaults(void) {
  add_node(0xA5, 450, 40);
  const aio::ui::NodeDetail d =
      aio::ui::build_node_detail(reg, log_, 1, 0 /* unix far too small */, g_now_ms, nullptr, 0);
  TEST_ASSERT_TRUE(d.found);
  TEST_ASSERT_FALSE(d.time_ok);
  TEST_ASSERT_EQUAL_STRING("--:--", d.clock);
  TEST_ASSERT_FALSE(d.has_next);
  TEST_ASSERT_EQUAL_STRING("never pushed", d.pushed);
}

/* --- schedule editor ---------------------------------------------------------------- */

static void test_editor_base_snapshot_sorted_and_tz(void) {
  HubNode *n = add_node(0xA5, 540, 10);
  n->sched.entry_count = 2;
  n->sched.entries[1].min_of_day = 420;
  n->sched.entries[1].days_mask = 0x7F;
  n->sched.entries[1].amount = 8;

  const aio::ui::SchedEditor e =
      aio::ui::build_sched_editor(reg, 1, g_unix);
  TEST_ASSERT_TRUE(e.found);
  TEST_ASSERT_TRUE(e.enabled);
  TEST_ASSERT_EQUAL_UINT8(2, e.entry_count);
  TEST_ASSERT_EQUAL_UINT16(420, e.entries[0].min_of_day);
  TEST_ASSERT_EQUAL_UINT16(540, e.entries[1].min_of_day);
  TEST_ASSERT_EQUAL_INT8(0, e.selected);
  TEST_ASSERT_EQUAL_UINT16(420, e.d_min_of_day); /* draft loaded from row 0 */
  TEST_ASSERT_EQUAL_STRING("Asia/Manila", e.tz);
}

static void test_editor_gates_and_unknown_node(void) {
  HubNode *n = add_node(0xA5, 450, 40);
  n->sched.hold_until = (uint32_t)(g_unix + 3600);
  const aio::ui::SchedEditor e = aio::ui::build_sched_editor(reg, 1, g_unix);
  TEST_ASSERT_TRUE(e.has_hold);
  TEST_ASSERT_FALSE(e.has_skip);
  TEST_ASSERT_EQUAL_INT32(60, e.gate_left_min);

  const aio::ui::SchedEditor missing = aio::ui::build_sched_editor(reg, 42, g_unix);
  TEST_ASSERT_FALSE(missing.found);
}

static void test_editor_draft_time_amount_clamps(void) {
  aio::ui::SchedEditor e = aio::ui::build_sched_editor(reg, 1, g_unix);
  aio::ui::editor_set_time(e, 2000);
  TEST_ASSERT_EQUAL_UINT16(1439, e.d_min_of_day);
  aio::ui::editor_set_time(e, 61);
  TEST_ASSERT_EQUAL_UINT16(61, e.d_min_of_day);
  aio::ui::editor_set_amount(e, 0);
  TEST_ASSERT_EQUAL_UINT16(1, e.d_amount);
  aio::ui::editor_set_amount(e, 5000);
  TEST_ASSERT_EQUAL_UINT16(999, e.d_amount);
}

static void test_editor_toggle_day_never_empty(void) {
  aio::ui::SchedEditor e = aio::ui::build_sched_editor(reg, 1, g_unix);
  e.d_days = 0x02; /* only Monday */
  aio::ui::editor_toggle_day(e, 1); /* last day: refuse to clear */
  TEST_ASSERT_EQUAL_UINT8(0x02, e.d_days);
  aio::ui::editor_toggle_day(e, 3); /* add Wed */
  TEST_ASSERT_EQUAL_UINT8(0x0A, e.d_days);
  aio::ui::editor_toggle_day(e, 3); /* remove Wed again */
  TEST_ASSERT_EQUAL_UINT8(0x02, e.d_days);
  aio::ui::editor_toggle_day(e, 9); /* invalid day: ignored */
  TEST_ASSERT_EQUAL_UINT8(0x02, e.d_days);
}

static void test_editor_apply_updates_selected_and_resorts(void) {
  add_node(0xA5, 540, 10); /* 09:00 */
  add_node(0x5A, 555, 5);  /* id 2 for the fill test below */
  aio::ui::SchedEditor e = aio::ui::build_sched_editor(reg, 1, g_unix);
  TEST_ASSERT_EQUAL_UINT8(1, e.entry_count);
  TEST_ASSERT_EQUAL_INT8(0, e.selected);

  /* edit row 0 from 09:00 to 06:30 → sorts before, selection follows */
  aio::ui::editor_set_time(e, 390);
  TEST_ASSERT_TRUE(aio::ui::editor_apply_draft(e));
  TEST_ASSERT_EQUAL_UINT16(390, e.entries[0].min_of_day);
  TEST_ASSERT_EQUAL_UINT16(10, e.entries[0].amount);
  TEST_ASSERT_EQUAL_INT8(0, e.selected);

  /* no selection → draft becomes a new row */
  e.selected = -1;
  aio::ui::editor_set_time(e, 750); /* 12:30 */
  TEST_ASSERT_TRUE(aio::ui::editor_apply_draft(e));
  TEST_ASSERT_EQUAL_UINT8(2, e.entry_count);
  TEST_ASSERT_EQUAL_UINT16(390, e.entries[0].min_of_day);
  TEST_ASSERT_EQUAL_UINT16(750, e.entries[1].min_of_day);
  TEST_ASSERT_EQUAL_INT8(1, e.selected); /* new row selected */
}

static void test_editor_add_remove_and_full_list(void) {
  add_node(0xA5, 450, 40);
  aio::ui::SchedEditor e = aio::ui::build_sched_editor(reg, 1, g_unix);
  /* fill to UI_DETAIL_MAX_ENTRIES (selection forced to -1 → apply adds) */
  for (int i = 0; i < aio::ui::UI_DETAIL_MAX_ENTRIES - 1; i++) {
    e.selected = -1;
    aio::ui::editor_set_time(e, (uint16_t)(500 + i * 10));
    TEST_ASSERT_TRUE(aio::ui::editor_apply_draft(e));
  }
  TEST_ASSERT_EQUAL_UINT8(aio::ui::UI_DETAIL_MAX_ENTRIES, e.entry_count);
  /* full and nothing selected → apply fails */
  e.selected = -1;
  TEST_ASSERT_FALSE(aio::ui::editor_apply_draft(e));

  /* remove the selected row, then remove down to empty */
  aio::ui::editor_select(e, 0);
  TEST_ASSERT_TRUE(aio::ui::editor_remove_selected(e));
  TEST_ASSERT_EQUAL_UINT8(aio::ui::UI_DETAIL_MAX_ENTRIES - 1, e.entry_count);
  e.selected = -1;
  TEST_ASSERT_FALSE(aio::ui::editor_remove_selected(e));
  aio::ui::editor_select(e, 0);
  while (e.entry_count) TEST_ASSERT_TRUE(aio::ui::editor_remove_selected(e));
  TEST_ASSERT_EQUAL_UINT8(0, e.entry_count);
  TEST_ASSERT_EQUAL_INT8(-1, e.selected);
}

/* --- history ------------------------------------------------------------------------ */

static void test_history_empty_and_invalid_time(void) {
  const aio::ui::HistoryView h0 = aio::ui::build_history(reg, log_, 0);
  TEST_ASSERT_FALSE(h0.time_ok);
  TEST_ASSERT_EQUAL_STRING("--:--", h0.clock);
  TEST_ASSERT_EQUAL_UINT32(0, h0.total_feeds);
  TEST_ASSERT_EQUAL_UINT8(0, h0.node_count);

  add_node(0xA5, 450, 40);
  const aio::ui::HistoryView h = aio::ui::build_history(reg, log_, g_unix);
  TEST_ASSERT_TRUE(h.time_ok);
  TEST_ASSERT_EQUAL_STRING("07:21 Sun 01 Nov", h.clock);
  TEST_ASSERT_EQUAL_STRING("Su", h.day_labels[0]);
  TEST_ASSERT_EQUAL_STRING("Sa", h.day_labels[1]); /* Sunday back one day */
  TEST_ASSERT_EQUAL_UINT8(1, h.node_count);        /* registry-seeded, 0 g */
  TEST_ASSERT_EQUAL_UINT32(0, h.nodes[0].grams);
}

static void test_history_day_buckets_and_totals(void) {
  add_node(0xA5, 450, 40);
  add_node(0x5A, 455, 25);
  /* today (07:21 local): one OK feed 40 g; yesterday: 25 g; 8 days ago: outside */
  log_.push((uint32_t)(g_unix - 2400), 1, EV_FEED_OK, FEED_OK, 3001, 40, 0);
  log_.push((uint32_t)(g_unix - 86400 - 3600), 2, EV_FEED_OK, FEED_OK, 3002, 25, 0);
  log_.push((uint32_t)(g_unix - 8 * 86400), 1, EV_FEED_OK, FEED_OK, 3003, 90, 0);

  const aio::ui::HistoryView h = aio::ui::build_history(reg, log_, g_unix);
  TEST_ASSERT_EQUAL_UINT16(1, h.days[0].feeds);
  TEST_ASSERT_EQUAL_UINT32(40, h.days[0].grams);
  TEST_ASSERT_EQUAL_UINT16(1, h.days[1].feeds);
  TEST_ASSERT_EQUAL_UINT32(25, h.days[1].grams);
  TEST_ASSERT_EQUAL_UINT16(0, h.days[2].feeds);
  TEST_ASSERT_EQUAL_UINT32(0, h.days[6].grams); /* 8-day-old event excluded */

  TEST_ASSERT_EQUAL_UINT32(2, h.total_feeds);
  TEST_ASSERT_EQUAL_UINT32(65, h.total_grams);
}

static void test_history_node_totals_sorted_desc(void) {
  add_node(0xA5, 450, 40); /* id 1 */
  add_node(0x5A, 455, 25); /* id 2 */
  add_node(0x66, 410, 30); /* id 3, silent */
  /* node 2: 25 + 40 = 65 g; node 1: 40 g; node 3: none. Push oldest first so
   * ring order == time order. */
  log_.push((uint32_t)(g_unix - 3600), 2, EV_FEED_OK, FEED_OK, 3002, 40, 0);
  log_.push((uint32_t)(g_unix - 2400), 2, EV_FEED_OK, FEED_OK, 3001, 25, 0);
  log_.push((uint32_t)(g_unix - 1800), 1, EV_FEED_OK, FEED_OK, 3003, 40, 0);
  /* a FAIL counts in the event list but not in grams */
  log_.push((uint32_t)(g_unix - 60), 1, EV_FEED_FAIL, FEED_ABORTED, 3004, 0, 0);

  const aio::ui::HistoryView h = aio::ui::build_history(reg, log_, g_unix);
  TEST_ASSERT_EQUAL_UINT8(3, h.node_count);
  TEST_ASSERT_EQUAL_UINT8(2, h.nodes[0].id); /* 65 g first */
  TEST_ASSERT_EQUAL_UINT32(65, h.nodes[0].grams);
  TEST_ASSERT_EQUAL_UINT16(2, h.nodes[0].feeds);
  TEST_ASSERT_EQUAL_UINT8(1, h.nodes[1].id); /* 40 g */
  TEST_ASSERT_EQUAL_UINT32(40, h.nodes[1].grams);
  TEST_ASSERT_EQUAL_UINT8(3, h.nodes[2].id); /* silent node still listed */
  TEST_ASSERT_EQUAL_UINT32(0, h.nodes[2].grams);

  TEST_ASSERT_EQUAL_UINT8(4, h.event_count); /* newest first incl. the FAIL */
  TEST_ASSERT_EQUAL_UINT8(EV_FEED_FAIL, h.events[0].kind);
  TEST_ASSERT_EQUAL_UINT32(60, h.events[0].age_s);
  TEST_ASSERT_EQUAL_UINT32(1800, h.events[1].age_s);
  TEST_ASSERT_EQUAL_UINT32(2400, h.events[2].age_s);
}

static void test_history_event_list_capped(void) {
  add_node(0xA5, 450, 40);
  /* push oldest first so ring order == time order; 10 events, list keeps 8 */
  for (int i = 10; i >= 1; i--)
    log_.push((uint32_t)(g_unix - i * 60), 1, EV_FEED_OK, FEED_OK, 4000 + i, 5, 0);
  const aio::ui::HistoryView h = aio::ui::build_history(reg, log_, g_unix);
  TEST_ASSERT_EQUAL_UINT8(aio::ui::UI_HISTORY_ROWS, h.event_count);
  TEST_ASSERT_EQUAL_UINT32(60, h.events[0].age_s); /* newest first */
  TEST_ASSERT_EQUAL_UINT32(60 * 8, h.events[7].age_s);
  TEST_ASSERT_EQUAL_UINT32(10, h.total_feeds); /* chart counts all */
}

/* --- alerts (§8.7) -------------------------------------------------------------------- */

static void test_alerts_time_invalid_empty(void) {
  add_node(0xA5, 450, 40);
  log_.push((uint32_t)(g_unix - 60), 1, EV_FEED_FAIL, FEED_ABORTED, 5001, 40, 0);
  aio::ui::AlertsAck ack;
  const aio::ui::AlertsView v =
      aio::ui::build_alerts(reg, log_, 0, g_now_ms, nullptr, 0, ack);
  TEST_ASSERT_FALSE(v.time_ok);
  TEST_ASSERT_EQUAL_STRING("--:--", v.clock);
  TEST_ASSERT_EQUAL_UINT8(0, v.active_count);
  TEST_ASSERT_EQUAL_UINT8(0, v.acked_count);
  TEST_ASSERT_EQUAL_UINT8(0, v.total);
}

static void test_alerts_condition_rows_offline_and_stale(void) {
  add_node(0xA5, 450, 40); /* id 1 */
  add_node(0x5A, 455, 25); /* id 2 */
  add_node(0x66, 410, 30); /* id 3, silent */
  uint8_t fc;
  const PeerFresh *f = fresh_table(&fc); /* #1 fresh, #2 stale (120 s), #3 none */
  aio::ui::AlertsAck ack;
  const aio::ui::AlertsView v =
      aio::ui::build_alerts(reg, log_, g_unix, g_now_ms, f, fc, ack);

  TEST_ASSERT_TRUE(v.time_ok);
  TEST_ASSERT_EQUAL_UINT8(3, v.total);
  TEST_ASSERT_EQUAL_UINT8(1, v.online);
  TEST_ASSERT_EQUAL_UINT8(2, v.active_count); /* two condition rows */
  TEST_ASSERT_EQUAL_UINT8(0, v.acked_count);

  TEST_ASSERT_EQUAL_UINT8(2, v.active[0].node); /* registry order */
  TEST_ASSERT_TRUE(v.active[0].cond);
  TEST_ASSERT_EQUAL_INT(aio::ui::NODE_STALE, v.active[0].state);
  TEST_ASSERT_FALSE(v.active[0].never_heard);
  TEST_ASSERT_EQUAL_UINT32(120, v.active[0].age_s); /* seconds silent */

  TEST_ASSERT_EQUAL_UINT8(3, v.active[1].node);
  TEST_ASSERT_TRUE(v.active[1].cond);
  TEST_ASSERT_EQUAL_INT(aio::ui::NODE_OFFLINE, v.active[1].state);
  TEST_ASSERT_TRUE(v.active[1].never_heard);
}

static void test_alerts_event_rows_newest_first_kinds(void) {
  add_node(0xA5, 450, 40); /* id 1, fresh (no condition row) */
  PeerFresh f[1];
  f[0] = PeerFresh{1, g_now_ms - 5000, 12000, 0};
  /* push oldest first; OK and CATCHUP are never alert rows */
  log_.push((uint32_t)(g_unix - 7200), 1, EV_FEED_OK, FEED_OK, 6001, 40, 0);
  log_.push((uint32_t)(g_unix - 3600), 1, EV_FEED_UNKNOWN, EVENT_RESULT_NO_LINK, 6002, 25, 0);
  log_.push((uint32_t)(g_unix - 600), 1, EV_FEED_REJ, FEED_REJ_FAULT, 6003, 40, 0);
  log_.push((uint32_t)(g_unix - 60), 1, EV_FEED_FAIL, FEED_ABORTED, 6004, 40, 0);
  log_.push((uint32_t)(g_unix - 30), 1, EV_CATCHUP, FEED_OK, 6005, 40, 0);

  aio::ui::AlertsAck ack;
  const aio::ui::AlertsView v =
      aio::ui::build_alerts(reg, log_, g_unix, g_now_ms, f, 1, ack);

  TEST_ASSERT_EQUAL_UINT8(3, v.active_count); /* FAIL, REJ, UNKNOWN */
  TEST_ASSERT_FALSE(v.active[0].cond);
  TEST_ASSERT_EQUAL_UINT8(EV_FEED_FAIL, v.active[0].kind);
  TEST_ASSERT_EQUAL_UINT8(FEED_ABORTED, v.active[0].result);
  TEST_ASSERT_EQUAL_UINT16(40, v.active[0].amount);
  TEST_ASSERT_EQUAL_UINT32(60, v.active[0].age_s);
  TEST_ASSERT_EQUAL_UINT8(EV_FEED_REJ, v.active[1].kind);
  TEST_ASSERT_EQUAL_UINT32(600, v.active[1].age_s);
  TEST_ASSERT_EQUAL_UINT8(EV_FEED_UNKNOWN, v.active[2].kind);
  TEST_ASSERT_EQUAL_UINT8(0, v.acked_count); /* nothing acked yet */
}

static void test_alerts_ack_moves_row_and_dedups(void) {
  add_node(0xA5, 450, 40);
  PeerFresh f[1];
  f[0] = PeerFresh{1, g_now_ms - 5000, 12000, 0};
  const uint32_t t_old = (uint32_t)(g_unix - 3600);
  const uint32_t t_new = (uint32_t)(g_unix - 60);
  log_.push(t_old, 1, EV_FEED_UNKNOWN, EVENT_RESULT_NO_LINK, 7001, 25, 0);
  log_.push(t_new, 1, EV_FEED_FAIL, FEED_ABORTED, 7002, 40, 0);

  aio::ui::AlertsAck ack;
  ack.ack(t_old, 1, EV_FEED_UNKNOWN);
  ack.ack(t_old, 1, EV_FEED_UNKNOWN); /* duplicate is a no-op */
  TEST_ASSERT_EQUAL_UINT8(1, ack.count());

  const aio::ui::AlertsView v =
      aio::ui::build_alerts(reg, log_, g_unix, g_now_ms, f, 1, ack);
  TEST_ASSERT_EQUAL_UINT8(1, v.active_count); /* only the FAIL stays active */
  TEST_ASSERT_EQUAL_UINT8(EV_FEED_FAIL, v.active[0].kind);
  TEST_ASSERT_EQUAL_UINT8(1, v.acked_count);
  TEST_ASSERT_EQUAL_UINT8(EV_FEED_UNKNOWN, v.acked[0].kind);
  TEST_ASSERT_EQUAL_UINT32(3600, v.acked[0].age_s);
  TEST_ASSERT_FALSE(v.acked[0].cond);

  /* ack key mismatch (different kind) does not hide the FAIL */
  aio::ui::AlertsAck ack2;
  ack2.ack(t_new, 1, EV_FEED_REJ);
  const aio::ui::AlertsView v2 =
      aio::ui::build_alerts(reg, log_, g_unix, g_now_ms, f, 1, ack2);
  TEST_ASSERT_EQUAL_UINT8(2, v2.active_count);
  TEST_ASSERT_EQUAL_UINT8(0, v2.acked_count);
}

static void test_alerts_caps_newest_kept(void) {
  add_node(0xA5, 450, 40);
  PeerFresh f[1];
  f[0] = PeerFresh{1, g_now_ms - 5000, 12000, 0};
  /* 8 FAILs, oldest first; then ack the 4 oldest → 3 fit the acked list */
  for (int i = 8; i >= 1; i--)
    log_.push((uint32_t)(g_unix - i * 60), 1, EV_FEED_FAIL, FEED_ABORTED,
              8000 + i, 40, 0);
  aio::ui::AlertsAck ack;
  for (int i = 5; i <= 8; i++)
    ack.ack((uint32_t)(g_unix - i * 60), 1, EV_FEED_FAIL);

  const aio::ui::AlertsView v =
      aio::ui::build_alerts(reg, log_, g_unix, g_now_ms, f, 1, ack);
  TEST_ASSERT_EQUAL_UINT8(aio::ui::UI_ALERTS_MAX_ACKED, v.acked_count);
  TEST_ASSERT_EQUAL_UINT32(300, v.acked[0].age_s); /* newest acked first (i=5) */
  TEST_ASSERT_EQUAL_UINT8(4, v.active_count);      /* unacked i=1..4 */
  TEST_ASSERT_EQUAL_UINT32(60, v.active[0].age_s); /* newest first */
  TEST_ASSERT_EQUAL_UINT32(240, v.active[3].age_s);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_registry_renders_safe_defaults);
  RUN_TEST(test_rows_states_and_freshness);
  RUN_TEST(test_next_feed_today_tomorrow_and_global);
  RUN_TEST(test_next_feed_unsorted_today_entries);
  RUN_TEST(test_last_event_and_alert);
  RUN_TEST(test_clock_format_manila);
  RUN_TEST(test_state_boundaries);
  RUN_TEST(test_detail_unknown_node);
  RUN_TEST(test_detail_schedule_sorted_and_next);
  RUN_TEST(test_detail_gates_skip_and_hold);
  RUN_TEST(test_detail_expired_gate_not_shown);
  RUN_TEST(test_detail_events_newest_first_capped);
  RUN_TEST(test_detail_state_freshness_and_offline);
  RUN_TEST(test_detail_time_invalid_keeps_defaults);
  RUN_TEST(test_editor_base_snapshot_sorted_and_tz);
  RUN_TEST(test_editor_gates_and_unknown_node);
  RUN_TEST(test_editor_draft_time_amount_clamps);
  RUN_TEST(test_editor_toggle_day_never_empty);
  RUN_TEST(test_editor_apply_updates_selected_and_resorts);
  RUN_TEST(test_editor_add_remove_and_full_list);
  RUN_TEST(test_history_empty_and_invalid_time);
  RUN_TEST(test_history_day_buckets_and_totals);
  RUN_TEST(test_history_node_totals_sorted_desc);
  RUN_TEST(test_history_event_list_capped);
  RUN_TEST(test_alerts_time_invalid_empty);
  RUN_TEST(test_alerts_condition_rows_offline_and_stale);
  RUN_TEST(test_alerts_event_rows_newest_first_kinds);
  RUN_TEST(test_alerts_ack_moves_row_and_dedups);
  RUN_TEST(test_alerts_caps_newest_kept);
  return UNITY_END();
}
