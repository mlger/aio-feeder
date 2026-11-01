/* Host tests for the M4b pairing session (PLAN.md §6.3, §8.5, §9.7; D14/D29).
 *
 * Exit criteria under test: the hub runs the full pairing flow — discovery,
 * confirmation code, key wrap under the code — with deterministic entropy,
 * and every §9.7 capacity rule rejects what it must reject.
 */
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "hub_registry.h"
#include "messages.h"
#include "pairing.h"
#include "pairing_session.h"
#include "profile.h"

using namespace aio;

void setUp(void) {}
void tearDown(void) {}

/* ---- deterministic PRNG (xorshift32) ----------------------------------------- */

static uint32_t g_rs = 0x12345678;
static void test_rng(uint8_t *out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    g_rs ^= g_rs << 13;
    g_rs ^= g_rs >> 17;
    g_rs ^= g_rs << 5;
    out[i] = (uint8_t)(g_rs & 0xFF);
  }
}

static const uint8_t MAC_A[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x99};
static const uint8_t MAC_B[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x9A};
static const uint8_t NODE_NONCE_A[8] = {1, 2, 3, 4, 5, 6, 7, 8};
static const uint8_t NODE_NONCE_B[8] = {9, 8, 7, 6, 5, 4, 3, 2};

static pair_req_payload_t make_req(const uint8_t nonce[8], uint16_t fw) {
  pair_req_payload_t pr;
  memset(&pr, 0, sizeof(pr));
  pr.fw_ver = fw;
  pr.caps = 0x01;
  memcpy(pr.node_nonce, nonce, 8);
  return pr;
}

/* ---- code + wrap primitives (lib/proto/pairing.h round trip) ------------------ */

void test_pairing_code_is_ascii_digits(void) {
  uint8_t digits[4];
  pairing_code(MAC_A, NODE_NONCE_A, NODE_NONCE_B, digits);
  for (int i = 0; i < 4; i++) {
    TEST_ASSERT_TRUE(digits[i] >= '0' && digits[i] <= '9');
  }
  /* deterministic for the same inputs */
  uint8_t again[4];
  pairing_code(MAC_A, NODE_NONCE_A, NODE_NONCE_B, again);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(digits, again, 4);
}

void test_pairing_wrap_roundtrip_and_wrong_code(void) {
  uint8_t key[32], wrapped[32], back[32];
  for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 7 + 1);
  uint8_t digits[4];
  pairing_code(MAC_A, NODE_NONCE_A, NODE_NONCE_B, digits);

  pairing_wrap_key(key, digits, NODE_NONCE_A, NODE_NONCE_B, wrapped);
  TEST_ASSERT_NOT_EQUAL(0, memcmp(key, wrapped, 32)); /* actually wrapped */

  TEST_ASSERT_EQUAL(0, pairing_unwrap_key(wrapped, digits, NODE_NONCE_A,
                                          NODE_NONCE_B, back));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(key, back, 32);

  /* a wrong confirmed code must not reconstruct the key */
  uint8_t wrong[4] = {digits[0], digits[1], digits[2],
                      (uint8_t)(digits[3] == '9' ? '0' : digits[3] + 1)};
  pairing_unwrap_key(wrapped, wrong, NODE_NONCE_A, NODE_NONCE_B, back);
  TEST_ASSERT_NOT_EQUAL(0, memcmp(key, back, 32));
}

/* ---- session flow -------------------------------------------------------------- */

void test_no_entropy_no_candidate(void) {
  PairingSession s;
  HubRegistry reg;
  TEST_ASSERT_TRUE(s.open(1000));
  const pair_req_payload_t pr = make_req(NODE_NONCE_A, 0x0101);
  TEST_ASSERT_FALSE(s.on_pair_req(MAC_A, pr, -50, 1100, &reg));
  TEST_ASSERT_EQUAL_UINT8(0, s.cand_count());
}

void test_discover_and_code_stability(void) {
  PairingSession s;
  s.begin(test_rng);
  HubRegistry reg;
  TEST_ASSERT_TRUE(s.open(1000));

  TEST_ASSERT_TRUE(s.on_pair_req(MAC_A, make_req(NODE_NONCE_A, 0x0101), -50, 1100, &reg));
  TEST_ASSERT_EQUAL_UINT8(1, s.cand_count());

  ui::PairingView v1 = s.view(1100);
  TEST_ASSERT_EQUAL_UINT8(ui::PAIR_DISCOVER, v1.step);
  TEST_ASSERT_EQUAL_UINT8(1, v1.cand_count);
  TEST_ASSERT_TRUE(v1.window_left_s > 0 && v1.window_left_s <= 60);

  TEST_ASSERT_TRUE(s.select(0));
  ui::PairingView v2 = s.view(1100);
  TEST_ASSERT_EQUAL_UINT8(ui::PAIR_CODE, v2.step);
  for (int i = 0; i < 4; i++)
    TEST_ASSERT_TRUE(v2.code_digits[i] >= '0' && v2.code_digits[i] <= '9');

  /* repeat PAIR_REQ: refreshed RSSI, unchanged candidate count and code */
  TEST_ASSERT_FALSE(s.on_pair_req(MAC_A, make_req(NODE_NONCE_A, 0x0101), -72, 1200, &reg));
  TEST_ASSERT_EQUAL_UINT8(1, s.cand_count());
  ui::PairingView v3 = s.view(1200);
  TEST_ASSERT_EQUAL_INT8(-72, v3.cand[0].rssi);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(v2.code_digits, v3.code_digits, 4);
}

void test_candidates_get_distinct_codes(void) {
  PairingSession s;
  s.begin(test_rng);
  HubRegistry reg;
  TEST_ASSERT_TRUE(s.open(0));
  TEST_ASSERT_TRUE(s.on_pair_req(MAC_A, make_req(NODE_NONCE_A, 1), -50, 1, &reg));
  TEST_ASSERT_TRUE(s.on_pair_req(MAC_B, make_req(NODE_NONCE_B, 1), -60, 2, &reg));
  TEST_ASSERT_TRUE(s.select(0));
  ui::PairingView a = s.view(2);
  s.cancel_code();
  TEST_ASSERT_EQUAL_UINT8(ui::PAIR_DISCOVER, s.view(2).step);
  TEST_ASSERT_TRUE(s.select(1));
  ui::PairingView b = s.view(2);
  TEST_ASSERT_NOT_EQUAL(0, memcmp(a.code_digits, b.code_digits, 4));
}

void test_already_paired_mac_ignored(void) {
  PairingSession s;
  s.begin(test_rng);
  HubRegistry reg;
  TEST_ASSERT_NOT_NULL(reg.add(MAC_A, 1)); /* node already paired */
  TEST_ASSERT_TRUE(s.open(0));
  TEST_ASSERT_FALSE(s.on_pair_req(MAC_A, make_req(NODE_NONCE_A, 1), -50, 1, &reg));
  TEST_ASSERT_EQUAL_UINT8(0, s.cand_count());
}

void test_max_candidates_capped(void) {
  PairingSession s;
  s.begin(test_rng);
  HubRegistry reg;
  TEST_ASSERT_TRUE(s.open(0));
  for (int i = 0; i < 4; i++) {
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, (uint8_t)(0x40 + i)};
    uint8_t nonce[8];
    for (int j = 0; j < 8; j++) nonce[j] = (uint8_t)(i * 8 + j + 1);
    TEST_ASSERT_TRUE(s.on_pair_req(mac, make_req(nonce, 1), -50, 1, &reg));
  }
  uint8_t mac5[6] = {0x02, 0, 0, 0, 0, 0x50};
  TEST_ASSERT_FALSE(s.on_pair_req(mac5, make_req(NODE_NONCE_A, 1), -50, 1, &reg));
  TEST_ASSERT_EQUAL_UINT8(4, s.cand_count());
}

void test_window_expiry_closes_session(void) {
  PairingSession s;
  s.begin(test_rng);
  HubRegistry reg;
  TEST_ASSERT_TRUE(s.open(1000));
  TEST_ASSERT_TRUE(s.on_pair_req(MAC_A, make_req(NODE_NONCE_A, 1), -50, 1100, &reg));
  TEST_ASSERT_FALSE(s.tick(60000)); /* still inside */
  TEST_ASSERT_TRUE(s.tick(61000));  /* 61000 - 1000 >= 60000 → expired */
  TEST_ASSERT_FALSE(s.is_open());
  TEST_ASSERT_EQUAL_UINT8(ui::PAIR_IDLE, s.view(61000).step);
  TEST_ASSERT_FALSE(s.on_pair_req(MAC_B, make_req(NODE_NONCE_B, 1), -50, 62000, &reg));
  TEST_ASSERT_TRUE(s.open(62000)); /* can reopen */
  TEST_ASSERT_TRUE(s.on_pair_req(MAC_B, make_req(NODE_NONCE_B, 1), -50, 62100, &reg));
}

void test_select_needs_discover_phase(void) {
  PairingSession s;
  s.begin(test_rng);
  HubRegistry reg;
  TEST_ASSERT_TRUE(s.open(0));
  TEST_ASSERT_FALSE(s.select(0)); /* empty list */
  TEST_ASSERT_TRUE(s.on_pair_req(MAC_A, make_req(NODE_NONCE_A, 1), -50, 1, &reg));
  TEST_ASSERT_TRUE(s.select(0));
  TEST_ASSERT_FALSE(s.select(0)); /* no re-select from CODE phase */
}

/* ---- commit --------------------------------------------------------------------- */

void test_commit_happy_path_full_flow(void) {
  PairingSession s;
  s.begin(test_rng);
  s.set_channel(6);
  HubRegistry reg;

  TEST_ASSERT_TRUE(s.open(0));
  TEST_ASSERT_TRUE(s.on_pair_req(MAC_A, make_req(NODE_NONCE_A, 0x0201), -55, 10, &reg));
  TEST_ASSERT_TRUE(s.select(0));
  TEST_ASSERT_TRUE(s.confirm_code());

  /* capture the confirmed code from the view before commit */
  ui::PairingView v = s.view(20);
  TEST_ASSERT_EQUAL_UINT8(ui::PAIR_FORM, v.step);
  TEST_ASSERT_EQUAL_UINT32(1500, v.cap_total); /* gate template default */
  TEST_ASSERT_EQUAL_UINT8(0, v.pct_full);

  s.set_form("hopper A", 1, 5000, 1200); /* auger, 5 kg, 1.2 kg inside */
  pair_accept_payload_t acc;
  memset(&acc, 0, sizeof(acc));
  uint8_t node_id = 0;
  TEST_ASSERT_EQUAL_UINT8(PairingSession::PAIR_COMMIT_OK, s.commit(reg, &acc, &node_id));
  TEST_ASSERT_TRUE(node_id >= 1);

  /* registry state */
  HubNode *n = reg.by_mac(MAC_A);
  TEST_ASSERT_NOT_NULL(n);
  TEST_ASSERT_EQUAL_UINT8(node_id, n->id);
  TEST_ASSERT_EQUAL_UINT8(32, n->key_len);
  TEST_ASSERT_EQUAL_UINT8(6, n->channel);
  TEST_ASSERT_EQUAL_UINT8(0, strcmp(n->profile.name, "hopper A"));
  TEST_ASSERT_EQUAL_UINT8(MECH_MOTOR_ONEWAY, n->profile.mech);
  TEST_ASSERT_EQUAL_UINT8(UNIT_GRAMS, n->profile.unit);
  TEST_ASSERT_EQUAL_UINT32(5000, n->profile.storage_capacity);
  TEST_ASSERT_EQUAL_UINT8(1, profile_valid(&n->profile));

  /* accept payload: ids, channel, code, and the key survives the wrap */
  TEST_ASSERT_EQUAL_UINT8(node_id, acc.node_id);
  TEST_ASSERT_EQUAL_UINT8(6, acc.channel);
  for (int i = 0; i < 4; i++)
    TEST_ASSERT_TRUE(acc.code_digits[i] >= '0' && acc.code_digits[i] <= '9');
  TEST_ASSERT_EQUAL_UINT8_ARRAY(v.code_digits, acc.code_digits, 4);
  uint8_t unwrapped[32];
  pairing_unwrap_key(acc.wrapped_key, acc.code_digits, NODE_NONCE_A, acc.hub_nonce,
                     unwrapped);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(n->key, unwrapped, 32);

  /* session lands in DONE and reports the new id */
  TEST_ASSERT_EQUAL_UINT8(ui::PAIR_DONE, s.view(30).step);
  TEST_ASSERT_EQUAL_UINT8(node_id, s.view(30).new_node_id);
  s.close();
  TEST_ASSERT_FALSE(s.is_open());
}

void test_commit_default_name(void) {
  PairingSession s;
  s.begin(test_rng);
  HubRegistry reg;
  TEST_ASSERT_TRUE(s.open(0));
  TEST_ASSERT_TRUE(s.on_pair_req(MAC_A, make_req(NODE_NONCE_A, 1), -50, 1, &reg));
  TEST_ASSERT_TRUE(s.select(0));
  TEST_ASSERT_TRUE(s.confirm_code());
  s.set_form("", 0, 1500, 0); /* no name → "feeder<id>" */
  pair_accept_payload_t acc;
  uint8_t node_id = 0;
  TEST_ASSERT_EQUAL_UINT8(PairingSession::PAIR_COMMIT_OK, s.commit(reg, &acc, &node_id));
  HubNode *n = reg.by_id(node_id);
  TEST_ASSERT_NOT_NULL(n);
  char want[16];
  snprintf(want, sizeof(want), "feeder%u", (unsigned)node_id);
  TEST_ASSERT_EQUAL_UINT8(0, strcmp(n->profile.name, want));
}

void test_commit_pump_template_ml(void) {
  PairingSession s;
  s.begin(test_rng);
  HubRegistry reg;
  TEST_ASSERT_TRUE(s.open(0));
  TEST_ASSERT_TRUE(s.on_pair_req(MAC_A, make_req(NODE_NONCE_A, 1), -50, 1, &reg));
  TEST_ASSERT_TRUE(s.select(0));
  TEST_ASSERT_TRUE(s.confirm_code());
  s.set_form("water", 2, 1000, 300);
  TEST_ASSERT_EQUAL_UINT8(PairingSession::PAIR_COMMIT_OK, s.commit(reg, nullptr, nullptr));
  HubNode *n = reg.by_mac(MAC_A);
  TEST_ASSERT_NOT_NULL(n);
  TEST_ASSERT_EQUAL_UINT8(MECH_PUMP, n->profile.mech);
  TEST_ASSERT_EQUAL_UINT8(UNIT_ML, n->profile.unit);
  TEST_ASSERT_EQUAL_UINT32(1000, n->profile.storage_capacity);
}

void test_commit_capacity_validation(void) {
  struct Case {
    uint32_t total, now;
    PairingSession::CommitResult want;
  };
  const Case cases[] = {
      {0, 0, PairingSession::PAIR_COMMIT_BAD_CAPACITY},
      {ui::PAIR_CAP_MAX + 1, 0, PairingSession::PAIR_COMMIT_BAD_CAPACITY},
      {100, 101, PairingSession::PAIR_COMMIT_BAD_CAPACITY},
      {1, 0, PairingSession::PAIR_COMMIT_OK},
      {100000, 0, PairingSession::PAIR_COMMIT_OK},
      {100000, 100000, PairingSession::PAIR_COMMIT_OK},
  };
  for (const Case &c : cases) {
    PairingSession s;
    s.begin(test_rng);
    HubRegistry reg;
    TEST_ASSERT_TRUE(s.open(0));
    TEST_ASSERT_TRUE(s.on_pair_req(MAC_A, make_req(NODE_NONCE_A, 1), -50, 1, &reg));
    TEST_ASSERT_TRUE(s.select(0));
    TEST_ASSERT_TRUE(s.confirm_code());
    s.set_form("t", 0, c.total, c.now);
    const PairingSession::CommitResult got = s.commit(reg, nullptr, nullptr);
    char msg[64];
    snprintf(msg, sizeof(msg), "total=%lu now=%lu", (unsigned long)c.total,
             (unsigned long)c.now);
    UNITY_TEST_ASSERT_EQUAL_UINT8((uint8_t)c.want, (uint8_t)got, __LINE__, msg);
  }
}

void test_commit_without_form_step_rejected(void) {
  PairingSession s;
  s.begin(test_rng);
  HubRegistry reg;
  TEST_ASSERT_TRUE(s.open(0));
  TEST_ASSERT_TRUE(s.on_pair_req(MAC_A, make_req(NODE_NONCE_A, 1), -50, 1, &reg));
  TEST_ASSERT_EQUAL_UINT8(PairingSession::PAIR_COMMIT_NO_SESSION,
                          s.commit(reg, nullptr, nullptr));
  TEST_ASSERT_TRUE(s.select(0));
  TEST_ASSERT_EQUAL_UINT8(PairingSession::PAIR_COMMIT_NO_SESSION,
                          s.commit(reg, nullptr, nullptr));
}

void test_commit_registry_full(void) {
  PairingSession s;
  s.begin(test_rng);
  HubRegistry reg;
  for (int i = 0; i < HUB_MAX_NODES; i++) {
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, (uint8_t)(0x10 + i)};
    TEST_ASSERT_NOT_NULL(reg.add(mac, 1));
  }
  TEST_ASSERT_TRUE(s.open(0));
  TEST_ASSERT_TRUE(s.on_pair_req(MAC_A, make_req(NODE_NONCE_A, 1), -50, 1, &reg));
  TEST_ASSERT_TRUE(s.select(0));
  TEST_ASSERT_TRUE(s.confirm_code());
  s.set_form("x", 0, 1500, 0);
  TEST_ASSERT_EQUAL_UINT8(PairingSession::PAIR_COMMIT_REG_FULL,
                          s.commit(reg, nullptr, nullptr));
}

void test_commit_races_already_paired(void) {
  PairingSession s;
  s.begin(test_rng);
  HubRegistry reg;
  TEST_ASSERT_TRUE(s.open(0));
  TEST_ASSERT_TRUE(s.on_pair_req(MAC_A, make_req(NODE_NONCE_A, 1), -50, 1, &reg));
  TEST_ASSERT_TRUE(s.select(0));
  TEST_ASSERT_TRUE(s.confirm_code());
  s.set_form("x", 0, 1500, 0);
  TEST_ASSERT_NOT_NULL(reg.add(MAC_A, 1)); /* paired meanwhile (second hub?) */
  TEST_ASSERT_EQUAL_UINT8(PairingSession::PAIR_COMMIT_ALREADY_PAIRED,
                          s.commit(reg, nullptr, nullptr));
}

/* ---- capacity summary (§8 UX) ---------------------------------------------------- */

void test_capacity_summary_math(void) {
  ui::PairingView v;
  v.mech_sel = 0;
  v.cap_total = 3000;
  v.cap_now = 200;
  ui::pair_capacity_summary(&v);
  TEST_ASSERT_EQUAL_UINT8(7, v.pct_full);
  TEST_ASSERT_EQUAL_UINT32(2800, v.to_fill);
  TEST_ASSERT_TRUE(v.amber); /* ≤ 20 % */
  TEST_ASSERT_EQUAL_UINT8(0, strcmp(v.summary, "About 7 % full - 2800 g to fill"));

  v.cap_total = 5000;
  v.cap_now = 4900;
  ui::pair_capacity_summary(&v);
  TEST_ASSERT_EQUAL_UINT8(98, v.pct_full);
  TEST_ASSERT_EQUAL_UINT32(100, v.to_fill);
  TEST_ASSERT_FALSE(v.amber);

  /* pump unit is ml (§8.5 template) */
  v.mech_sel = 2;
  v.cap_total = 1000;
  v.cap_now = 250;
  ui::pair_capacity_summary(&v);
  TEST_ASSERT_EQUAL_UINT8(0, strcmp(v.summary, "About 25 % full - 750 ml to fill"));
}

void test_capacity_valid_boundaries(void) {
  TEST_ASSERT_FALSE(ui::pair_capacity_valid(0, 0));
  TEST_ASSERT_FALSE(ui::pair_capacity_valid(ui::PAIR_CAP_MAX + 1, 0));
  TEST_ASSERT_FALSE(ui::pair_capacity_valid(100, 101));
  TEST_ASSERT_TRUE(ui::pair_capacity_valid(1, 0));
  TEST_ASSERT_TRUE(ui::pair_capacity_valid(100000, 100000));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_pairing_code_is_ascii_digits);
  RUN_TEST(test_pairing_wrap_roundtrip_and_wrong_code);
  RUN_TEST(test_no_entropy_no_candidate);
  RUN_TEST(test_discover_and_code_stability);
  RUN_TEST(test_candidates_get_distinct_codes);
  RUN_TEST(test_already_paired_mac_ignored);
  RUN_TEST(test_max_candidates_capped);
  RUN_TEST(test_window_expiry_closes_session);
  RUN_TEST(test_select_needs_discover_phase);
  RUN_TEST(test_commit_happy_path_full_flow);
  RUN_TEST(test_commit_default_name);
  RUN_TEST(test_commit_pump_template_ml);
  RUN_TEST(test_commit_capacity_validation);
  RUN_TEST(test_commit_without_form_step_rejected);
  RUN_TEST(test_commit_registry_full);
  RUN_TEST(test_commit_races_already_paired);
  RUN_TEST(test_capacity_summary_math);
  RUN_TEST(test_capacity_valid_boundaries);
  return UNITY_END();
}
