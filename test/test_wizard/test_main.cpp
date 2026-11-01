/* Host tests for the setup wizard session (PLAN.md §9.13, FR21; M4b).
 *
 * Scene: a hub with two paired nodes (a 1500 g gate "kitA" and a pump).
 * The wizard walks tz → wifi → node → (capacity) → schedule → commit and
 * must not touch the registry until the schedule step is saved (§9.13). */
#include <string.h>

#include <unity.h>

#include "wizard_session.h"

using namespace aio;

static HubRegistry reg;
static HubSettings set;
static WizardSession wiz;

void setUp(void) {
  reg = HubRegistry{};
  set = settings_defaults();
  wiz = WizardSession{};
  wiz.bind_registry(&reg);
  reg.data().tz_rule = TZ_UTC;
  const uint8_t mac1[6] = {0x02, 0, 0, 0, 0, 0x11};
  HubNode *n = reg.add(mac1, 1);
  TEST_ASSERT_NOT_NULL(n);
  snprintf(n->profile.name, sizeof(n->profile.name), "kitA");
  /* 250 ms per gram → 10000 ms gate cap → 40 g per-feed cap (SR2/D27) */
  n->profile.cal_ms_per_unit_x100 = 25000;
  n->profile.crc16 = profile_crc16(&n->profile);
  const uint8_t mac2[6] = {0x02, 0, 0, 0, 0, 0x12};
  HubNode *n2 = reg.add(mac2, 1);
  TEST_ASSERT_NOT_NULL(n2);
  snprintf(n2->profile.name, sizeof(n2->profile.name), "pump");
  n2->profile.mech = MECH_PUMP;
  n2->profile.unit = UNIT_ML;
  n2->profile.storage_capacity = 1000;
  n2->profile.crc16 = profile_crc16(&n2->profile);
}

void tearDown(void) {}

static void test_open_and_step_order(void) {
  TEST_ASSERT_TRUE(wiz.open());
  TEST_ASSERT_TRUE(wiz.is_open());
  TEST_ASSERT_EQUAL(ui::WIZ_TZ, wiz.step());
  TEST_ASSERT_FALSE(wiz.open()); /* already open */
  TEST_ASSERT_TRUE(wiz.next());  /* step 1 accepts defaults (§9.13) */
  TEST_ASSERT_EQUAL(ui::WIZ_WIFI, wiz.step());
}

static void test_tz_validation(void) {
  TEST_ASSERT_TRUE(wiz.open());
  TEST_ASSERT_TRUE(wiz.set_tz(TZ_MANILA, true));
  TEST_ASSERT_FALSE(wiz.set_tz(9, true));
  TEST_ASSERT_TRUE(wiz.next());
  TEST_ASSERT_EQUAL(ui::WIZ_WIFI, wiz.step());
  TEST_ASSERT_TRUE(wiz.back());
  TEST_ASSERT_EQUAL(ui::WIZ_TZ, wiz.step());
}

static void test_wifi_required(void) {
  TEST_ASSERT_TRUE(wiz.open());
  wiz.set_tz(TZ_MANILA, true);
  TEST_ASSERT_TRUE(wiz.next());
  TEST_ASSERT_FALSE(wiz.next()); /* no ssid yet */
  TEST_ASSERT_FALSE(wiz.set_wifi("", "x"));
  TEST_ASSERT_TRUE(wiz.set_wifi("home", nullptr)); /* open network: empty pass ok */
  TEST_ASSERT_TRUE(wiz.set_wifi("home-net", "pw"));
  TEST_ASSERT_TRUE(wiz.next());
  TEST_ASSERT_EQUAL(ui::WIZ_NODE, wiz.step());
  wiz.wifi_test_result(false, "no radio (QEMU)");
  const ui::WizardView v = wiz.view();
  TEST_ASSERT_EQUAL(0, v.wifi_state);
  TEST_ASSERT_EQUAL_STRING("no radio (QEMU)", v.wifi_note);
}

static void test_node_step_existing(void) {
  TEST_ASSERT_TRUE(wiz.open());
  wiz.set_tz(TZ_MANILA, true);
  wiz.next();
  wiz.set_wifi("home", "pw");
  wiz.next();

  const ui::WizardView before = wiz.view();
  TEST_ASSERT_EQUAL_UINT8(2, before.node_count);
  TEST_ASSERT_EQUAL_STRING("kitA", before.node_names[0]);
  TEST_ASSERT_EQUAL_UINT8(1, before.node_ids[0]);

  TEST_ASSERT_FALSE(wiz.next()); /* nothing chosen */
  TEST_ASSERT_TRUE(wiz.choose_existing(2));
  TEST_ASSERT_TRUE(wiz.next());
  TEST_ASSERT_EQUAL(ui::WIZ_CAP, wiz.step()); /* not just paired → cap asked */
  TEST_ASSERT_TRUE(wiz.back());
  TEST_ASSERT_EQUAL(ui::WIZ_NODE, wiz.step());
  TEST_ASSERT_TRUE(wiz.choose_existing(1));
  TEST_ASSERT_TRUE(wiz.next());
  TEST_ASSERT_EQUAL(ui::WIZ_CAP, wiz.step());
}

static void test_pair_new_skips_capacity(void) {
  TEST_ASSERT_TRUE(wiz.open());
  wiz.set_tz(TZ_MANILA, true);
  wiz.next();
  wiz.set_wifi("home", "pw");
  wiz.next();
  wiz.choose_pair_new();
  TEST_ASSERT_FALSE(wiz.next()); /* must pair first */
  TEST_ASSERT_TRUE(wiz.on_paired(2)); /* pairing commit landed */
  TEST_ASSERT_TRUE(wiz.next());       /* §9.13: hopper step skipped */
  TEST_ASSERT_EQUAL(ui::WIZ_SCHED, wiz.step());
  TEST_ASSERT_TRUE(wiz.back());
  TEST_ASSERT_EQUAL(ui::WIZ_NODE, wiz.step()); /* back from sched skips cap */
}

static void test_capacity_rules(void) {
  TEST_ASSERT_TRUE(wiz.open());
  wiz.set_tz(TZ_MANILA, true);
  wiz.next();
  wiz.set_wifi("home", "pw");
  wiz.next();
  wiz.choose_existing(1);
  wiz.next();

  TEST_ASSERT_FALSE(wiz.set_capacity(0, 0));       /* below 1 */
  TEST_ASSERT_FALSE(wiz.set_capacity(100001, 0));  /* above 100000 */
  TEST_ASSERT_FALSE(wiz.set_capacity(1500, 2000)); /* in-it > total */
  TEST_ASSERT_TRUE(wiz.set_capacity(1500, 400));
  const ui::WizardView v = wiz.view();
  TEST_ASSERT_EQUAL_UINT32(1100, v.to_fill);
  TEST_ASSERT_EQUAL_UINT8(27, v.pct_full);
  TEST_ASSERT_FALSE(v.amber);
  TEST_ASSERT_TRUE(wiz.next());
  TEST_ASSERT_EQUAL(ui::WIZ_SCHED, wiz.step());
}

static void test_schedule_rules_and_commit(void) {
  TEST_ASSERT_TRUE(wiz.open());
  wiz.set_tz(TZ_EU_CENTRAL, false);
  wiz.next();
  wiz.set_wifi("home", "pw");
  wiz.next();
  wiz.choose_existing(1);
  wiz.next();
  wiz.set_capacity(2000, 500);
  wiz.next();

  const uint32_t cap = wiz.feed_cap_units();
  TEST_ASSERT_EQUAL_UINT32(40, cap); /* 10000 ms / 250 ms per g */
  TEST_ASSERT_EQUAL_STRING("g", wiz.unit_label());

  TEST_ASSERT_FALSE(wiz.set_schedule(1500, 0x7F, 40)); /* min ≥ 1440 */
  TEST_ASSERT_FALSE(wiz.set_schedule(480, 0x00, 40));  /* no days */
  TEST_ASSERT_FALSE(wiz.set_schedule(480, 0xFF, 40));  /* 8th bit set */
  TEST_ASSERT_FALSE(wiz.set_schedule(480, 0x7F, 0));   /* amount 0 */
  TEST_ASSERT_FALSE(wiz.set_schedule(480, 0x7F, 41));  /* above per-feed cap */

  /* nothing may be written before commit (§9.13) */
  TEST_ASSERT_EQUAL_UINT8(TZ_UTC, reg.data().tz_rule);
  TEST_ASSERT_EQUAL_UINT16(0, reg.by_id(1)->sched.entry_count);

  TEST_ASSERT_TRUE(wiz.set_schedule(480, 0x7F, 40));
  uint8_t node_out = 0;
  TEST_ASSERT_EQUAL(WizardSession::WZ_COMMIT_OK, wiz.commit(reg, set, &node_out));
  TEST_ASSERT_EQUAL_UINT8(1, node_out);
  TEST_ASSERT_EQUAL(ui::WIZ_DONE, wiz.step());

  /* committed: tz, capacity, schedule, versions */
  TEST_ASSERT_EQUAL_UINT8(TZ_EU_CENTRAL, reg.data().tz_rule);
  TEST_ASSERT_EQUAL_UINT8(0, set.ntp_on);
  TEST_ASSERT_EQUAL_STRING("home", set.wifi_ssid);
  TEST_ASSERT_EQUAL_STRING("pw", set.wifi_pass);
  const HubNode *n = reg.by_id(1);
  TEST_ASSERT_EQUAL_UINT32(2000, n->profile.storage_capacity);
  TEST_ASSERT_EQUAL_UINT8(1, n->sched.enabled);
  TEST_ASSERT_EQUAL_UINT8(1, n->sched.entry_count);
  TEST_ASSERT_EQUAL_UINT16(480, n->sched.entries[0].min_of_day);
  TEST_ASSERT_EQUAL_UINT16(40, n->sched.entries[0].amount);
  TEST_ASSERT_EQUAL_UINT32(0, n->sched.pushed_at); /* caller must re-push */
  TEST_ASSERT_EQUAL_UINT16(2, reg.data().sched_version); /* bumped */
  TEST_ASSERT_EQUAL_UINT16(2, n->sched.version);
}

static void test_commit_no_node(void) {
  TEST_ASSERT_TRUE(wiz.open());
  wiz.set_tz(TZ_MANILA, true);
  wiz.next();
  wiz.set_wifi("home", "pw");
  wiz.next();
  wiz.choose_existing(1);
  TEST_ASSERT_TRUE(wiz.next());
  wiz.set_capacity(1500, 400);
  TEST_ASSERT_TRUE(wiz.next());
  TEST_ASSERT_TRUE(wiz.set_schedule(480, 0x7F, 40));
  reg.remove(1); /* node vanished mid-wizard */
  TEST_ASSERT_EQUAL(WizardSession::WZ_COMMIT_NO_NODE, wiz.commit(reg, set, nullptr));
}

static void test_pump_cap_uses_ml(void) {
  TEST_ASSERT_TRUE(wiz.open());
  wiz.set_tz(TZ_MANILA, true);
  wiz.next();
  wiz.set_wifi("home", "pw");
  wiz.next();
  wiz.choose_existing(2);
  TEST_ASSERT_EQUAL_STRING("ml", wiz.unit_label());
}

static void test_view_node_selection(void) {
  TEST_ASSERT_TRUE(wiz.open());
  wiz.set_tz(TZ_MANILA, true);
  wiz.next();
  wiz.set_wifi("home", "pw");
  wiz.next();
  wiz.choose_pair_new();
  const ui::WizardView v = wiz.view();
  TEST_ASSERT_EQUAL(-2, v.node_sel);
  TEST_ASSERT_TRUE(v.pair_new);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_open_and_step_order);
  RUN_TEST(test_tz_validation);
  RUN_TEST(test_wifi_required);
  RUN_TEST(test_node_step_existing);
  RUN_TEST(test_pair_new_skips_capacity);
  RUN_TEST(test_capacity_rules);
  RUN_TEST(test_schedule_rules_and_commit);
  RUN_TEST(test_commit_no_node);
  RUN_TEST(test_pump_cap_uses_ml);
  RUN_TEST(test_view_node_selection);
  return UNITY_END();
}
