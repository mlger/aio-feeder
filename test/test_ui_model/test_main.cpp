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
  TEST_ASSERT_EQUAL_UINT32(5000, d1.last_rx_age_s);
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
  return UNITY_END();
}
