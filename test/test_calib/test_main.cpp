/* Host tests for the M4b calibration wizard (PLAN.md §8.4, §9.3; FR4, D27).
 *
 * Exit criteria under test: the wizard walks select → test run → weigh →
 * median → save with every §9.3 guard enforced — test length capped by the
 * node's max on-time (SR2), weighed amounts under 5 g refused, derived
 * rates inside the profile-format sanity range, and the stored value is the
 * median of the runs, CRC'd into the profile.
 */
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "calib_session.h"
#include "messages.h"
#include "profile.h"

using namespace aio;

void setUp(void) {}
void tearDown(void) {}

static node_profile_t gate_profile(uint16_t rate_x100) {
  node_profile_t p = profile_default(); /* gate, grams, 250 ms/g */
  p.cal_ms_per_unit_x100 = rate_x100;
  p.crc16 = profile_crc16(&p);
  return p;
}

static node_profile_t motor_profile(uint16_t rate_x100) {
  node_profile_t p = gate_profile(rate_x100);
  p.mech = MECH_MOTOR_ONEWAY;
  p.crc16 = profile_crc16(&p);
  return p;
}

/* ---- defaults (§9.3.1) --------------------------------------------------------- */

void test_start_defaults_gate_two_seconds(void) {
  CalibSession c;
  const node_profile_t p = gate_profile(25000);
  TEST_ASSERT_TRUE(c.start(1, p));
  TEST_ASSERT_TRUE(c.is_open());
  TEST_ASSERT_EQUAL_UINT32(2, CalibSession::default_test_s(p));
  TEST_ASSERT_EQUAL_UINT32(2, c.test_s());
  /* 2 s at 250 ms/g → 8 g commanded */
  TEST_ASSERT_EQUAL_UINT32(8, c.test_grams());
}

void test_start_defaults_other_mech_five_seconds(void) {
  const node_profile_t p = motor_profile(25000);
  TEST_ASSERT_EQUAL_UINT32(5, CalibSession::default_test_s(p));
}

void test_max_test_s_follows_node_cap(void) {
  node_profile_t p = gate_profile(25000);
  TEST_ASSERT_EQUAL_UINT32(10, CalibSession::max_test_s(p)); /* D27 gate cap */
  p.max_on_ms_single = 3000; /* motor-style short cap */
  TEST_ASSERT_EQUAL_UINT32(3, CalibSession::max_test_s(p));
  p.max_on_ms_single = 0; /* degenerate profile still allows 1 s */
  TEST_ASSERT_EQUAL_UINT32(1, CalibSession::max_test_s(p));
}

/* ---- test length selection ------------------------------------------------------ */

void test_set_test_s_bounds(void) {
  CalibSession c;
  const node_profile_t p = gate_profile(25000);
  TEST_ASSERT_TRUE(c.start(1, p));
  TEST_ASSERT_FALSE(c.set_test_s(0));
  TEST_ASSERT_FALSE(c.set_test_s(11)); /* above the 10 s gate cap */
  TEST_ASSERT_TRUE(c.set_test_s(10));
  TEST_ASSERT_EQUAL_UINT32(10, c.test_s());
  /* 10 s at 250 ms/g → 40 g commanded */
  TEST_ASSERT_EQUAL_UINT32(40, c.test_grams());
}

void test_set_test_s_only_in_runtime(void) {
  CalibSession c;
  const node_profile_t p = gate_profile(25000);
  TEST_ASSERT_TRUE(c.start(1, p));
  TEST_ASSERT_TRUE(c.mark_sent(77, 1000)); /* → DISPENSE */
  TEST_ASSERT_FALSE(c.set_test_s(5));
}

void test_test_grams_min_one(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(1, gate_profile(65535))); /* slowest representable */
  TEST_ASSERT_TRUE(c.set_test_s(1));
  /* 1000 ms × 100 / 65535 = 1.52 → ceil 2 */
  TEST_ASSERT_EQUAL_UINT32(2, c.test_grams());
}

/* ---- dispense phase ------------------------------------------------------------- */

void test_mark_sent_and_result_routing(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(1, gate_profile(25000)));
  TEST_ASSERT_TRUE(c.mark_sent(77, 1000)); /* RUNTIME → DISPENSE */
  TEST_ASSERT_FALSE(c.mark_sent(78, 1100)); /* wrong phase now */
  TEST_ASSERT_EQUAL_UINT32(77, c.pending_feed_id());
  /* wrong feed id ignored */
  TEST_ASSERT_FALSE(c.on_result(76, FEED_OK, 2000));
  TEST_ASSERT_EQUAL_UINT32(77, c.pending_feed_id());
  /* the real one moves to WEIGH and holds the on-time */
  TEST_ASSERT_TRUE(c.on_result(77, FEED_OK, 2000));
  TEST_ASSERT_EQUAL_UINT32(0, c.pending_feed_id());
  TEST_ASSERT_FALSE(c.on_result(77, FEED_OK, 2000)); /* consumed */
}

void test_result_rejection_bounces_with_note(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(2, gate_profile(25000)));
  TEST_ASSERT_TRUE(c.mark_sent(9, 1000));
  TEST_ASSERT_FALSE(c.on_result(9, FEED_REJ_LIMIT_DAY, 0));
  const ui::CalibView v = c.view(2000);
  TEST_ASSERT_EQUAL(ui::CAL_RUNTIME, v.step);
  TEST_ASSERT_TRUE(v.note[0] != '\0');
}

void test_dispense_timeout(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(1, gate_profile(25000)));
  TEST_ASSERT_TRUE(c.mark_sent(5, 1000));
  TEST_ASSERT_FALSE(c.tick(1000 + 14000));
  TEST_ASSERT_TRUE(c.tick(1000 + CAL_DISPENSE_TIMEOUT_MS));
  TEST_ASSERT_EQUAL(ui::CAL_RUNTIME, c.view(20000).step);
  TEST_ASSERT_TRUE(c.view(20000).note[0] != '\0');
}

void test_dispense_countdown_in_view(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(1, gate_profile(25000)));
  TEST_ASSERT_TRUE(c.mark_sent(5, 1000));
  const ui::CalibView v = c.view(1000 + 4200);
  TEST_ASSERT_EQUAL(ui::CAL_DISPENSE, v.step);
  TEST_ASSERT_EQUAL_UINT32(11, v.left_s); /* ceil((15000-4200)/1000) */
}

/* ---- weigh guards (§9.3.3, §9.3.4) ---------------------------------------------- */

static void run_one(CalibSession &c, uint32_t feed_id, uint16_t on_ms) {
  TEST_ASSERT_TRUE(c.mark_sent(feed_id, 1000));
  TEST_ASSERT_TRUE(c.on_result(feed_id, FEED_OK, on_ms));
}

void test_weigh_below_five_grams_refused(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(1, gate_profile(25000)));
  run_one(c, 1, 2000);
  TEST_ASSERT_FALSE(c.set_weigh_grams_x100(499));
  TEST_ASSERT_TRUE(c.view(0).note[0] != '\0');
  TEST_ASSERT_EQUAL(ui::CAL_WEIGH, c.view(0).step); /* still waiting */
  TEST_ASSERT_EQUAL_UINT8(0, c.sample_count());
}

void test_weigh_valid_run_becomes_sample(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(1, gate_profile(25000)));
  run_one(c, 1, 2000);
  TEST_ASSERT_TRUE(c.set_weigh_grams_x100(800)); /* 8 g */
  /* 2000 ms × 10000 / 800 = 25000 */
  TEST_ASSERT_EQUAL_UINT8(1, c.sample_count());
  TEST_ASSERT_EQUAL_UINT16(25000, c.median_rate_x100());
  TEST_ASSERT_EQUAL(ui::CAL_RUNTIME, c.view(0).step);
  TEST_ASSERT_EQUAL_UINT8(0, c.view(0).note[0]);
}

void test_weigh_rate_too_fast_refused(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(1, gate_profile(25000)));
  run_one(c, 1, 10); /* 10 ms for 100 g */
  TEST_ASSERT_FALSE(c.set_weigh_grams_x100(10000));
  TEST_ASSERT_EQUAL_UINT8(0, c.sample_count());
}

void test_weigh_rate_too_slow_refused(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(1, gate_profile(25000)));
  run_one(c, 1, 10000); /* 10 s but weighed 5 g → 2000 ms/g, over the cap */
  TEST_ASSERT_FALSE(c.set_weigh_grams_x100(500));
  TEST_ASSERT_EQUAL_UINT8(0, c.sample_count());
  /* the same on-time with a believable weight is fine: 10 s / 25 g */
  TEST_ASSERT_TRUE(c.set_weigh_grams_x100(2500));
  TEST_ASSERT_EQUAL_UINT16(40000, c.median_rate_x100());
}

void test_weigh_only_in_weigh_phase(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(1, gate_profile(25000)));
  TEST_ASSERT_FALSE(c.set_weigh_grams_x100(800)); /* RUNTIME — no run yet */
}

/* ---- median (§9.3.5) ------------------------------------------------------------ */

void test_median_odd_even(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(1, gate_profile(25000)));
  run_one(c, 1, 2000);
  TEST_ASSERT_TRUE(c.set_weigh_grams_x100(800));  /* 25000 */
  run_one(c, 2, 2000);
  TEST_ASSERT_TRUE(c.set_weigh_grams_x100(1000)); /* 20000 */
  run_one(c, 3, 2000);
  TEST_ASSERT_TRUE(c.set_weigh_grams_x100(667));  /* 2000×10000/667 ≈ 29985 */
  TEST_ASSERT_EQUAL_UINT8(3, c.sample_count());
  TEST_ASSERT_EQUAL_UINT16(25000, c.median_rate_x100()); /* middle of 3 */
  run_one(c, 4, 2000);
  TEST_ASSERT_TRUE(c.set_weigh_grams_x100(2000)); /* 10000 */
  TEST_ASSERT_EQUAL_UINT8(4, c.sample_count());
  TEST_ASSERT_EQUAL_UINT16(20000, c.median_rate_x100()); /* lower middle of 4 */
}

void test_sample_bank_caps_at_five(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(1, gate_profile(25000)));
  for (int i = 0; i < 5; i++) {
    run_one(c, (uint32_t)(i + 1), 2000);
    TEST_ASSERT_TRUE(c.set_weigh_grams_x100(800));
  }
  TEST_ASSERT_EQUAL_UINT8(5, c.sample_count());
  run_one(c, 9, 2000);
  TEST_ASSERT_FALSE(c.set_weigh_grams_x100(800)); /* bank full */
  TEST_ASSERT_EQUAL_UINT8(5, c.sample_count());
}

/* ---- save (§9.3.5) --------------------------------------------------------------- */

void test_apply_writes_median_and_recrc(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(3, gate_profile(25000)));
  run_one(c, 1, 2000);
  TEST_ASSERT_TRUE(c.set_weigh_grams_x100(800));  /* 25000 */
  run_one(c, 2, 2000);
  TEST_ASSERT_TRUE(c.set_weigh_grams_x100(1000)); /* 20000 */

  node_profile_t p = gate_profile(25000);
  TEST_ASSERT_TRUE(c.apply(&p));
  TEST_ASSERT_EQUAL_UINT16(20000, p.cal_ms_per_unit_x100);
  TEST_ASSERT_TRUE(profile_valid(&p) != 0);

  /* without samples there is nothing to save */
  CalibSession empty;
  TEST_ASSERT_TRUE(empty.start(1, gate_profile(25000)));
  TEST_ASSERT_FALSE(empty.apply(&p));
}

void test_mark_saved_and_done_view(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(4, gate_profile(25000)));
  run_one(c, 1, 2000);
  TEST_ASSERT_TRUE(c.set_weigh_grams_x100(800));  /* 25000 */
  run_one(c, 2, 2000);
  TEST_ASSERT_TRUE(c.set_weigh_grams_x100(1000)); /* 20000 */
  node_profile_t p = gate_profile(25000);
  TEST_ASSERT_TRUE(c.apply(&p));
  c.mark_saved();
  const ui::CalibView v = c.view(0);
  TEST_ASSERT_EQUAL(ui::CAL_DONE, v.step);
  TEST_ASSERT_EQUAL_UINT8(4, v.saved_node_id);
  TEST_ASSERT_EQUAL_UINT16(25000, v.old_rate_x100);
  TEST_ASSERT_EQUAL_UINT16(20000, v.median_x100);
}

/* ---- lifecycle edges ------------------------------------------------------------- */

void test_close_resets_and_restart_rebases(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(1, gate_profile(25000)));
  run_one(c, 1, 2000);
  TEST_ASSERT_TRUE(c.set_weigh_grams_x100(800));
  c.close();
  TEST_ASSERT_FALSE(c.is_open());
  TEST_ASSERT_EQUAL_UINT8(0, c.node_id());

  /* reopening with a *changed* profile rebases the shown "old rate" */
  TEST_ASSERT_TRUE(c.start(1, gate_profile(20000)));
  TEST_ASSERT_EQUAL_UINT8(0, c.sample_count());
  const ui::CalibView v = c.view(0);
  TEST_ASSERT_EQUAL_UINT16(20000, v.old_rate_x100);
  TEST_ASSERT_EQUAL_UINT8(1, v.node_id);
}

void test_start_refuses_while_open(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(1, gate_profile(25000)));
  TEST_ASSERT_FALSE(c.start(2, gate_profile(25000)));
  TEST_ASSERT_EQUAL_UINT8(1, c.node_id()); /* unchanged */
}

void test_view_fields_runtime(void) {
  CalibSession c;
  TEST_ASSERT_TRUE(c.start(2, gate_profile(25000)));
  const ui::CalibView v = c.view(0);
  TEST_ASSERT_EQUAL(ui::CAL_RUNTIME, v.step);
  TEST_ASSERT_EQUAL_UINT8(2, v.node_id);
  TEST_ASSERT_EQUAL_UINT32(2, v.test_s);
  TEST_ASSERT_EQUAL_UINT32(10, v.max_test_s);
  TEST_ASSERT_EQUAL_UINT32(8, v.test_grams);
  TEST_ASSERT_EQUAL_UINT16(25000, v.old_rate_x100);
  TEST_ASSERT_EQUAL_STRING("node", v.name); /* profile_default name */
}

int run_all_tests(void) {
  UNITY_BEGIN();
  RUN_TEST(test_start_defaults_gate_two_seconds);
  RUN_TEST(test_start_defaults_other_mech_five_seconds);
  RUN_TEST(test_max_test_s_follows_node_cap);
  RUN_TEST(test_set_test_s_bounds);
  RUN_TEST(test_set_test_s_only_in_runtime);
  RUN_TEST(test_test_grams_min_one);
  RUN_TEST(test_mark_sent_and_result_routing);
  RUN_TEST(test_result_rejection_bounces_with_note);
  RUN_TEST(test_dispense_timeout);
  RUN_TEST(test_dispense_countdown_in_view);
  RUN_TEST(test_weigh_below_five_grams_refused);
  RUN_TEST(test_weigh_valid_run_becomes_sample);
  RUN_TEST(test_weigh_rate_too_fast_refused);
  RUN_TEST(test_weigh_rate_too_slow_refused);
  RUN_TEST(test_weigh_only_in_weigh_phase);
  RUN_TEST(test_median_odd_even);
  RUN_TEST(test_sample_bank_caps_at_five);
  RUN_TEST(test_apply_writes_median_and_recrc);
  RUN_TEST(test_mark_saved_and_done_view);
  RUN_TEST(test_close_resets_and_restart_rebases);
  RUN_TEST(test_start_refuses_while_open);
  RUN_TEST(test_view_fields_runtime);
  return UNITY_END();
}

int main(void) { return run_all_tests(); }
