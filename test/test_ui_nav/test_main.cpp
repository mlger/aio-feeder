/* Host tests for the hub UI navigation model (PLAN.md §8, §12 M5).
 *
 * Covers: tap routing (dashboard card → detail, per-node id riding along),
 * the ≤4-deep back stack, home reset, the §8 PIN gate on the settings flow
 * (locked → PIN screen, pin_result lands on settings), lock engaging while
 * the user sits on a protected screen, and the Actions one-shot flags.
 */
#include <string.h>

#include <unity.h>

#include "ui_nav.h"

using namespace aio;
using aio::nav::Actions;
using aio::nav::Nav;
using aio::nav::Screen;

static Nav g;

void setUp(void) { g.reset(); }
void tearDown(void) {}

/* --- routing ----------------------------------------------------------- */

static void test_dashboard_opens_detail_with_node(void) {
  Screen s = g.open(nav::SC_DETAIL, 3);
  TEST_ASSERT_EQUAL(nav::SC_DETAIL, s);
  TEST_ASSERT_EQUAL(3, g.node);
  TEST_ASSERT_TRUE(g.can_back());
}

static void test_back_returns_to_dash(void) {
  g.open(nav::SC_DETAIL, 2);
  TEST_ASSERT_EQUAL(nav::SC_DASH, g.back()); /* back() returns the landing screen */
  TEST_ASSERT_FALSE(g.can_back());
  TEST_ASSERT_EQUAL(nav::SC_DASH, g.cur);
}

static void test_back_at_dash_stays(void) {
  TEST_ASSERT_EQUAL(nav::SC_DASH, g.back());
  TEST_ASSERT_EQUAL(nav::SC_DASH, g.cur);
}

static void test_deep_walk_pops_in_order(void) {
  g.open(nav::SC_DETAIL, 1);
  g.open(nav::SC_EDIT, 1);
  TEST_ASSERT_EQUAL(nav::SC_EDIT, g.cur);
  TEST_ASSERT_EQUAL(nav::SC_DETAIL, g.back());
  TEST_ASSERT_EQUAL(1, g.node); /* node id sticks across the stack */
  TEST_ASSERT_EQUAL(nav::SC_DASH, g.back());
}

static void test_home_clears_stack(void) {
  g.open(nav::SC_DETAIL, 1);
  g.open(nav::SC_HIST, 0);
  TEST_ASSERT_EQUAL(nav::SC_DASH, g.home());
  TEST_ASSERT_FALSE(g.can_back());
}

static void test_back_stack_overflow_keeps_recent(void) {
  /* dash → 5 opens: stack caps at 4, the dash entry is dropped */
  g.open(nav::SC_DETAIL, 1);
  g.open(nav::SC_HIST, 0);
  g.open(nav::SC_DETAIL, 2);
  g.open(nav::SC_EDIT, 2);
  g.open(nav::SC_ALERTS, 0);
  TEST_ASSERT_EQUAL(nav::SC_DETAIL, g.back());
  TEST_ASSERT_EQUAL(nav::SC_HIST, g.back());
  TEST_ASSERT_EQUAL(nav::SC_DETAIL, g.back());
  /* the dash entry dropped and EDIT never made it in — the 4th back lands
   * on the oldest surviving entry (dash) */
  TEST_ASSERT_EQUAL(nav::SC_DASH, g.back());
}

static void test_open_same_screen_no_stack_growth(void) {
  g.open(nav::SC_DETAIL, 1);
  g.open(nav::SC_DETAIL, 1);
  TEST_ASSERT_EQUAL(1, g.back_n);
}

static void test_open_rejects_invalid_screen(void) {
  Screen bogus = (Screen)(nav::SC_COUNT + 3);
  TEST_ASSERT_EQUAL(nav::SC_DASH, g.open(bogus, 0));
  TEST_ASSERT_FALSE(g.can_back());
}

/* --- §8 PIN gate -------------------------------------------------------- */

static void test_settings_gate_routes_to_pin_when_locked(void) {
  g.set_locked(true);
  TEST_ASSERT_EQUAL(nav::SC_PIN, g.open(nav::SC_SET, 0));
  TEST_ASSERT_EQUAL(nav::SC_PIN, g.cur);
}

static void test_pin_ok_lands_on_settings(void) {
  g.set_locked(true);
  g.open(nav::SC_SET, 0);
  TEST_ASSERT_EQUAL(nav::SC_SET, g.pin_result(true));
  TEST_ASSERT_FALSE(g.locked);
}

static void test_pin_wrong_stays_on_pin(void) {
  g.set_locked(true);
  g.open(nav::SC_SET, 0);
  TEST_ASSERT_EQUAL(nav::SC_PIN, g.pin_result(false));
  TEST_ASSERT_TRUE(g.locked);
}

static void test_unlock_without_pending_lands_on_settings(void) {
  /* degenerate pending state: pin_result falls back to settings */
  g.set_locked(true);
  g.open(nav::SC_SET, 0);
  g.pending = nav::SC_DASH;
  TEST_ASSERT_EQUAL(nav::SC_SET, g.pin_result(true));
}

static void test_lock_engaged_on_settings_bounces_to_dash(void) {
  g.open(nav::SC_SET, 0); /* unlocked session */
  g.set_locked(true);     /* 60 s expiry while the user is there */
  TEST_ASSERT_EQUAL(nav::SC_DASH, g.cur);
  TEST_ASSERT_FALSE(g.can_back());
}

static void test_lock_engaged_on_pin_bounces_to_dash(void) {
  g.set_locked(true);
  g.open(nav::SC_SET, 0);
  g.set_locked(true);
  TEST_ASSERT_EQUAL(nav::SC_DASH, g.cur);
}

/* --- Actions one-shot flags --------------------------------------------- */

static void test_actions_any_and_clear(void) {
  Actions a;
  TEST_ASSERT_FALSE(a.any());
  a.detail = true;
  a.detail_node = 2;
  a.set_section = 5;
  TEST_ASSERT_TRUE(a.any());
  a.clear();
  TEST_ASSERT_FALSE(a.any());
  TEST_ASSERT_EQUAL(-1, a.set_section); /* back to the idle sentinel */
}

static void test_actions_negative_sentinels_report_idle(void) {
  Actions a;
  a.pair_select = -1;
  a.wiz_tz = -1;
  TEST_ASSERT_FALSE(a.any());
  a.pair_select = 1;
  TEST_ASSERT_TRUE(a.any());
}

/* --- M5b actions --------------------------------------------------------- */

static void test_m5b_actions_default_idle(void) {
  Actions a;
  /* entry pad, pairing form, wizard extras, editor drafts, detail ops */
  TEST_ASSERT_EQUAL(-1, a.detail_feed);
  TEST_ASSERT_EQUAL(-2, a.ed_row);
  TEST_ASSERT_EQUAL(255, a.ed_day);
  TEST_ASSERT_EQUAL(entry::ENT_NONE, a.entry_tgt);
  TEST_ASSERT_FALSE(a.any());
  /* each group flips any() on its own */
  a.entry_commit = true;
  TEST_ASSERT_TRUE(a.any());
  a.clear();
  a.pair_mech = true;
  TEST_ASSERT_TRUE(a.any());
  a.clear();
  a.wiz_node = 3;
  TEST_ASSERT_TRUE(a.any());
  a.clear();
  a.wiz_pair_new = true;
  TEST_ASSERT_TRUE(a.any());
  a.clear();
  a.wiz_ntp = true;
  TEST_ASSERT_TRUE(a.any());
  a.clear();
  a.wiz_days = true;
  TEST_ASSERT_TRUE(a.any());
  a.clear();
  a.ed_row = 0;
  TEST_ASSERT_TRUE(a.any());
  a.clear();
  a.ed_day = 4;
  TEST_ASSERT_TRUE(a.any());
  a.clear();
  a.ed_apply = true;
  TEST_ASSERT_TRUE(a.any());
  a.clear();
  a.ed_del = true;
  TEST_ASSERT_TRUE(a.any());
  a.clear();
  a.ed_toggle = true;
  TEST_ASSERT_TRUE(a.any());
  a.clear();
  a.detail_feed = 2;
  TEST_ASSERT_TRUE(a.any());
  a.clear();
  a.detail_skip = true;
  TEST_ASSERT_TRUE(a.any());
  a.clear();
  a.detail_hold = true;
  TEST_ASSERT_TRUE(a.any());
}

static void test_m5b_entry_commit_payload_survives_copy(void) {
  Actions a;
  a.entry_commit = true;
  a.entry_tgt = entry::ENT_CAL_WEIGH;
  a.entry_u32 = 825;
  strcpy(a.entry_text, "kit");
  Actions b = a;
  TEST_ASSERT_EQUAL(entry::ENT_CAL_WEIGH, b.entry_tgt);
  TEST_ASSERT_EQUAL_UINT32(825, b.entry_u32);
  TEST_ASSERT_EQUAL_STRING("kit", b.entry_text);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_dashboard_opens_detail_with_node);
  RUN_TEST(test_back_returns_to_dash);
  RUN_TEST(test_back_at_dash_stays);
  RUN_TEST(test_deep_walk_pops_in_order);
  RUN_TEST(test_home_clears_stack);
  RUN_TEST(test_back_stack_overflow_keeps_recent);
  RUN_TEST(test_open_same_screen_no_stack_growth);
  RUN_TEST(test_open_rejects_invalid_screen);
  RUN_TEST(test_settings_gate_routes_to_pin_when_locked);
  RUN_TEST(test_pin_ok_lands_on_settings);
  RUN_TEST(test_pin_wrong_stays_on_pin);
  RUN_TEST(test_unlock_without_pending_lands_on_settings);
  RUN_TEST(test_lock_engaged_on_settings_bounces_to_dash);
  RUN_TEST(test_lock_engaged_on_pin_bounces_to_dash);
  RUN_TEST(test_actions_any_and_clear);
  RUN_TEST(test_actions_negative_sentinels_report_idle);
  RUN_TEST(test_m5b_actions_default_idle);
  RUN_TEST(test_m5b_entry_commit_payload_survives_copy);
  return UNITY_END();
}
