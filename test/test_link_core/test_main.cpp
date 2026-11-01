/* Host tests for the M2 link core (PLAN.md §6.2, §6.4–§6.6, §12 M2).
 * Runs in CI on every push; the same engine code runs on hub and both nodes. */
#include <stddef.h>
#include <unity.h>
#include <vector>

#include "crc16.h"
#include "link.h"
#include "messages.h"
#include "pairing.h"
#include "profile.h"
#include "sha256.h"

#include "feed_journal.h"
#include "node_limits.h"
#include "schedule.h"
#include "../../node/src/node_config.h"

using namespace aio;

void setUp(void) {}
void tearDown(void) {}

/* ---- crypto vectors ------------------------------------------------------ */

void test_sha256_abc(void) {
  uint8_t d[32];
  sha256("abc", 3, d);
  static const uint8_t want[] = {
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
      0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
      0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, d, 32);
}

void test_hmac_rfc4231_case1(void) {
  uint8_t key[20];
  memset(key, 0x0b, sizeof(key));
  uint8_t d[32];
  hmac_sha256(key, sizeof(key), (const uint8_t *)"Hi There", 8, d);
  static const uint8_t want[] = {0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c,
                                 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b, 0x88, 0x1d,
                                 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7, 0x26, 0xe9, 0x37,
                                 0x6c, 0x2e, 0x32, 0xcf, 0xf7};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, d, 32);
}

void test_hmac_rfc4231_case2(void) {
  uint8_t d[32];
  hmac_sha256((const uint8_t *)"Jefe", 4,
              (const uint8_t *)"what do ya want for nothing?", 28, d);
  static const uint8_t want[] = {0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a,
                                 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00,
                                 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83, 0x9d, 0xec, 0x58,
                                 0xb9, 0x64, 0xec, 0x38, 0x43};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, d, 32);
}

void test_frame_mac_tamper(void) {
  uint8_t key[32];
  memset(key, 7, sizeof(key));
  uint8_t buf[16];
  memcpy(buf, "12345678", 8);
  uint8_t mac[8];
  frame_compute_mac(key, buf, sizeof(buf), mac);
  uint8_t mac2[8];
  buf[3] ^= 0x10;
  frame_compute_mac(key, buf, sizeof(buf), mac2);
  TEST_ASSERT_FALSE(frame_mac_equal(mac, mac2));
  buf[3] ^= 0x10;
  frame_compute_mac(key, buf, sizeof(buf), mac2);
  TEST_ASSERT_TRUE(frame_mac_equal(mac, mac2));
}

void test_pairing_wrap_roundtrip(void) {
  uint8_t node_mac[6] = {1, 2, 3, 4, 5, 6};
  uint8_t node_nonce[8], hub_nonce[8];
  for (int i = 0; i < 8; i++) {
    node_nonce[i] = (uint8_t)(0x20 + i);
    hub_nonce[i] = (uint8_t)(0xA0 + i);
  }
  uint8_t digits[4];
  pairing_code(node_mac, node_nonce, hub_nonce, digits);
  for (int i = 0; i < 4; i++) TEST_ASSERT_TRUE(digits[i] >= '0' && digits[i] <= '9');

  uint8_t key[32], wrapped[32], unwrapped[32];
  for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
  pairing_wrap_key(key, digits, node_nonce, hub_nonce, wrapped);
  TEST_ASSERT_NOT_EQUAL(0, memcmp(key, wrapped, 32));
  pairing_unwrap_key(wrapped, digits, node_nonce, hub_nonce, unwrapped);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(key, unwrapped, 32);

  /* one wrong digit must not unwrap to the same key */
  uint8_t wrong[4];
  memcpy(wrong, digits, 4);
  wrong[0] = (uint8_t)('0' + (wrong[0] - '0' + 1) % 10);
  pairing_unwrap_key(wrapped, wrong, node_nonce, hub_nonce, unwrapped);
  TEST_ASSERT_NOT_EQUAL(0, memcmp(key, unwrapped, 32));
}

/* ---- loopback harness ----------------------------------------------------- */

static uint8_t SHARED_KEY[32];

struct MockTransport : public ILinkTransport {
  struct Sent {
    uint8_t mac[6];
    std::vector<uint8_t> data;
  };
  std::vector<Sent> sent;
  uint8_t chan = 1;
  bool fail = false;

  bool send(const uint8_t mac[6], const uint8_t *data, size_t len) override {
    if (fail) return false;
    Sent s;
    memcpy(s.mac, mac, 6);
    s.data.assign(data, data + len);
    sent.push_back(s);
    return true;
  }
  void set_channel(uint8_t c) override { chan = c; }
  uint8_t channel() const override { return chan; }
  void clear() { sent.clear(); }
};

/* Wire two engines together: frames each queued get delivered to the other.
 * Fixture shortcut: both sides register the same 6-byte address. */
static void pump(MockTransport &a, LinkEngine &ea, MockTransport &b, LinkEngine &eb,
                 uint32_t now_ms) {
  std::vector<MockTransport::Sent> av(a.sent.begin(), a.sent.end());
  std::vector<MockTransport::Sent> bv(b.sent.begin(), b.sent.end());
  a.clear();
  b.clear();
  for (auto &f : av) eb.on_rx(f.mac, f.data.data(), f.data.size(), -60, now_ms);
  for (auto &f : bv) ea.on_rx(f.mac, f.data.data(), f.data.size(), -60, now_ms);
}

static size_t build_frame(uint8_t *buf, const uint8_t key[32], uint8_t type, uint8_t node_id,
                          uint16_t seq, const void *pl, size_t plen) {
  buf[0] = PROTO_VER;
  buf[1] = type;
  buf[2] = node_id;
  buf[3] = 0;
  buf[4] = (uint8_t)(seq & 0xFF);
  buf[5] = (uint8_t)(seq >> 8);
  buf[6] = (uint8_t)plen;
  buf[7] = (uint8_t)(plen >> 8);
  if (plen) memcpy(buf + 8, pl, plen);
  frame_compute_mac(key, buf, 8 + plen, buf + 8 + plen);
  return 8 + plen + 8;
}

/* ---- FEED round trip (hub ⇄ node) ---------------------------------------- */

static bool g_feed_got;
static feed_payload_t g_feed_pl;
static bool g_ack_got;
static feed_ack_payload_t g_ack_pl;
static int g_result_count;
static feed_result_payload_t g_result_pl;
static int g_feed_unknown_count;

static void reset_cbs() {
  g_feed_got = false;
  g_ack_got = false;
  g_result_count = 0;
  g_feed_unknown_count = 0;
}

static LinkHandlers node_handlers() {
  LinkHandlers h{};
  h.on_feed = [](const uint8_t *, const feed_payload_t &f) {
    g_feed_got = true;
    g_feed_pl = f;
  };
  return h;
}

static LinkHandlers hub_handlers() {
  LinkHandlers h{};
  h.on_feed_ack = [](uint8_t, const feed_ack_payload_t &a) {
    g_ack_got = true;
    g_ack_pl = a;
  };
  h.on_feed_result = [](uint8_t, const feed_result_payload_t &r) {
    g_result_pl = r;
    g_result_count++;
  };
  h.on_feed_unknown = [](uint32_t, uint8_t) { g_feed_unknown_count++; };
  return h;
}

void test_feed_roundtrip(void) {
  reset_cbs();
  MockTransport ta, tb;
  LinkEngine hub, node;
  hub.begin(LinkEngine::Role::Hub, &ta, hub_handlers(), 0);
  node.begin(LinkEngine::Role::Node, &tb, node_handlers(), 0);
  uint8_t mac[6] = {0xAA, 0, 0, 0, 0, 1};
  hub.add_peer(mac, 1, SHARED_KEY, 32);
  node.add_peer(mac, LINK_HUB_ID, SHARED_KEY, 32);

  feed_payload_t f{1234, 5, UNIT_GRAMS, 0};
  TEST_ASSERT_TRUE(hub.send_feed(1, f, 1000));
  TEST_ASSERT_TRUE(hub.feed_pending());
  pump(ta, hub, tb, node, 1010);
  TEST_ASSERT_TRUE(g_feed_got);
  TEST_ASSERT_EQUAL_UINT32(1234, g_feed_pl.feed_id);
  TEST_ASSERT_TRUE(hub.feed_pending()); /* stays pending until the ACK */

  node.send_feed_ack(1234, 1, FEED_OK, 1020);
  pump(ta, hub, tb, node, 1030);
  TEST_ASSERT_TRUE(g_ack_got);
  TEST_ASSERT_EQUAL_UINT32(1234, g_ack_pl.feed_id);
  TEST_ASSERT_TRUE(g_ack_pl.accepted);
  TEST_ASSERT_FALSE(hub.feed_pending()); /* ACK cancels the retry queue */

  node.send_feed_result(1234, FEED_OK, 1250, 0, 0xFF, 1700000000u, 1040);
  node.tick(1045); /* RESULT #1 */
  pump(ta, hub, tb, node, 1050);
  TEST_ASSERT_EQUAL_INT(1, g_result_count);
  TEST_ASSERT_EQUAL_UINT32(1234, g_result_pl.feed_id);

  /* repeats #2/#3 carry the same seq → seq-deduped by the hub (§6.6) */
  node.tick(1300);
  node.tick(1550);
  pump(ta, hub, tb, node, 1560);
  TEST_ASSERT_EQUAL_INT(1, g_result_count);
  TEST_ASSERT_EQUAL_UINT32(2, hub.stats().results_deduped);
}

void test_feed_retries_then_unknown(void) {
  reset_cbs();
  MockTransport ta;
  LinkEngine hub;
  hub.begin(LinkEngine::Role::Hub, &ta, hub_handlers(), 0);
  uint8_t mac[6] = {0xAA, 0, 0, 0, 0, 1};
  hub.add_peer(mac, 1, SHARED_KEY, 32);

  feed_payload_t f{77, 5, UNIT_GRAMS, 0};
  TEST_ASSERT_TRUE(hub.send_feed(1, f, 1000));
  hub.tick(1200); /* before the 300 ms retry: nothing */
  TEST_ASSERT_EQUAL_INT(0, g_feed_unknown_count);
  TEST_ASSERT_EQUAL_UINT32(1, ta.sent.size());

  hub.tick(1301); /* retry 1 (300 ms) */
  hub.tick(2301); /* retry 2 (+1000 ms) */
  hub.tick(4301); /* retry 3 (+3000 ms) */
  TEST_ASSERT_EQUAL_UINT32(4, ta.sent.size()); /* original + 3 resends */
  TEST_ASSERT_EQUAL_INT(0, g_feed_unknown_count);
  /* all four carry the same seq: the node sees retries as duplicates */
  TEST_ASSERT_EQUAL_UINT32(ta.sent[0].data[4], ta.sent[3].data[4]);
  TEST_ASSERT_EQUAL_UINT32(ta.sent[0].data[5], ta.sent[3].data[5]);

  hub.tick(7302); /* policy exhausted → UNKNOWN + alert, never re-issued */
  TEST_ASSERT_EQUAL_INT(1, g_feed_unknown_count);
  TEST_ASSERT_FALSE(hub.feed_pending());
  TEST_ASSERT_EQUAL_UINT32(4, ta.sent.size());
}

void test_feed_dup_reanswered_not_actuated_twice(void) {
  /* Engine level: a retried FEED (same seq) reaches on_feed again; the node
   * runtime answers from the journal (SR6) — FeedJournal checks below. */
  static int feed_calls = 0;
  feed_calls = 0;
  LinkHandlers h{};
  h.on_feed = [](const uint8_t *, const feed_payload_t &) { feed_calls++; };
  MockTransport tb;
  LinkEngine node;
  node.begin(LinkEngine::Role::Node, &tb, h, 0);
  uint8_t hub_mac[6] = {0xBB, 0, 0, 0, 0, 1};
  node.add_peer(hub_mac, LINK_HUB_ID, SHARED_KEY, 32);

  uint8_t buf[FRAME_HEADER_SIZE + sizeof(feed_payload_t) + FRAME_MAC_SIZE];
  feed_payload_t f{42, 5, UNIT_GRAMS, 0};
  size_t n = build_frame(buf, SHARED_KEY, MSG_FEED, 1, 100, &f, sizeof(f));
  node.on_rx(hub_mac, buf, n, -60, 0);
  node.on_rx(hub_mac, buf, n, -60, 0); /* hub retry, same seq */
  TEST_ASSERT_EQUAL_INT(2, feed_calls); /* runtime must journal-dedup */
  /* FEED duplicates are dispatched to on_feed (re-answer, §6.6), so the
   * generic dup counter stays at 0 here — other types land there. */
  TEST_ASSERT_EQUAL_UINT32(0, node.stats().rx_dup);

  FeedJournal j;
  j.record_intent(42);
  uint8_t stored;
  TEST_ASSERT_TRUE(j.known(42, &stored));
  TEST_ASSERT_EQUAL_UINT8(FEED_ABORTED, stored); /* in-progress answers ABORTED */
  TEST_ASSERT_NOT_NULL(j.find(42));
  TEST_ASSERT_EQUAL_UINT8(1, j.find(42)->in_progress);
  j.record_result(42, FEED_OK, 100);
  TEST_ASSERT_TRUE(j.known(42, &stored));
  TEST_ASSERT_EQUAL_UINT8(FEED_OK, stored);
  TEST_ASSERT_EQUAL_UINT8(0, j.find(42)->in_progress);
}

void test_seq_window_and_replay(void) {
  MockTransport tb;
  LinkEngine node;
  node.begin(LinkEngine::Role::Node, &tb, LinkHandlers{}, 0);
  uint8_t hub_mac[6] = {0xBB, 0, 0, 0, 0, 2};
  node.add_peer(hub_mac, LINK_HUB_ID, SHARED_KEY, 32);

  uint8_t buf[64];
  time_sync_payload_t t{1700000000u, 0};
  /* seq 10 fresh, 10 dup, 12 fresh, 15 (gap over lost 13/14) fresh */
  size_t n = build_frame(buf, SHARED_KEY, MSG_TIME_SYNC, LINK_HUB_ID, 10, &t, sizeof(t));
  node.on_rx(hub_mac, buf, n, -60, 0);
  node.on_rx(hub_mac, buf, n, -60, 0);
  TEST_ASSERT_EQUAL_UINT32(1, node.stats().rx_dup);
  n = build_frame(buf, SHARED_KEY, MSG_TIME_SYNC, LINK_HUB_ID, 12, &t, sizeof(t));
  node.on_rx(hub_mac, buf, n, -60, 0);
  n = build_frame(buf, SHARED_KEY, MSG_TIME_SYNC, LINK_HUB_ID, 15, &t, sizeof(t));
  node.on_rx(hub_mac, buf, n, -60, 0);
  TEST_ASSERT_EQUAL_UINT32(3, node.stats().rx_ok);

  /* ancient replay: 40 behind → dropped as replay (§6.4) */
  n = build_frame(buf, SHARED_KEY, MSG_TIME_SYNC, LINK_HUB_ID, (uint16_t)(15 - 40), &t,
                  sizeof(t));
  node.on_rx(hub_mac, buf, n, -60, 0);
  TEST_ASSERT_EQUAL_UINT32(1, node.stats().rx_replay);
  TEST_ASSERT_EQUAL_UINT32(3, node.stats().rx_ok);

  /* wrong key → bad MAC, frame never dispatched */
  uint8_t other_key[32];
  memset(other_key, 9, sizeof(other_key));
  n = build_frame(buf, other_key, MSG_TIME_SYNC, LINK_HUB_ID, 30, &t, sizeof(t));
  node.on_rx(hub_mac, buf, n, -60, 0);
  TEST_ASSERT_EQUAL_UINT32(1, node.stats().rx_bad_mac);
  TEST_ASSERT_EQUAL_UINT32(3, node.stats().rx_ok);
}

void test_hub_answers_hello(void) {
  static hello_payload_t got;
  static int hello_count = 0;
  hello_count = 0;
  LinkHandlers h{};
  h.on_hello = [](const uint8_t *, const hello_payload_t &pl, int8_t) {
    got = pl;
    hello_count++;
  };
  MockTransport ta;
  LinkEngine hub;
  hub.begin(LinkEngine::Role::Hub, &ta, h, 0);
  hub.set_channel(7);

  uint8_t buf[64];
  hello_payload_t hl{AIO_FW_VER, 0, 0};
  uint8_t src[6] = {0xCC, 0, 0, 0, 0, 9};
  size_t n = build_frame(buf, AIO_ZERO_KEY, MSG_HELLO, 0, 5, &hl, sizeof(hl));
  hub.on_rx(src, buf, n, -50, 0);
  TEST_ASSERT_EQUAL_INT(1, hello_count);
  TEST_ASSERT_EQUAL_UINT16(AIO_FW_VER, got.fw_ver);
  TEST_ASSERT_EQUAL_UINT32(1, ta.sent.size());
  TEST_ASSERT_EQUAL_UINT8(MSG_HELLO_ACK, ta.sent[0].data[1]);
  hello_ack_payload_t ack;
  memcpy(&ack, ta.sent[0].data.data() + FRAME_HEADER_SIZE, sizeof(ack));
  TEST_ASSERT_EQUAL_UINT8(7, ack.channel); /* hub advertises its channel */
}

void test_channel_scan_hops_and_locks(void) {
  static uint8_t locked_ch = 0;
  static int lost_count = 0;
  locked_ch = 0;
  lost_count = 0;
  LinkHandlers h{};
  h.on_channel_locked = [](uint8_t ch) { locked_ch = ch; };
  h.on_link_lost = []() { lost_count++; };
  MockTransport tb;
  LinkEngine node;
  node.begin(LinkEngine::Role::Node, &tb, h, 0);
  uint8_t hub_mac[6] = {0xBB, 0, 0, 0, 0, 3};
  node.add_peer(hub_mac, LINK_HUB_ID, SHARED_KEY, 32);

  node.tick(0);
  TEST_ASSERT_FALSE(node.scanning()); /* 10 s boot grace, hub "recent" */
  node.tick(71000);                   /* past the 60 s silence window */
  TEST_ASSERT_TRUE(node.scanning());
  TEST_ASSERT_EQUAL_INT(1, lost_count);

  node.tick(71100); /* first hop: channel 1 */
  TEST_ASSERT_EQUAL_UINT8(1, tb.chan);
  TEST_ASSERT_EQUAL_UINT8(MSG_HELLO, tb.sent[0].data[1]);
  node.tick(71350); /* +250 ms → channel 2 */
  TEST_ASSERT_EQUAL_UINT8(2, tb.chan);
  node.tick(71600); /* → channel 3 */
  TEST_ASSERT_EQUAL_UINT8(3, tb.chan);

  /* a HELLO_ACK from the hub locks the channel it names (§6.2) */
  uint8_t buf[64];
  hello_ack_payload_t ack{1, 1, 6};
  size_t n = build_frame(buf, AIO_ZERO_KEY, MSG_HELLO_ACK, LINK_HUB_ID, 1, &ack, sizeof(ack));
  node.on_rx(hub_mac, buf, n, -60, 71700);
  TEST_ASSERT_FALSE(node.scanning());
  TEST_ASSERT_EQUAL_UINT8(6, locked_ch);
  TEST_ASSERT_EQUAL_UINT8(6, node.channel());
  TEST_ASSERT_TRUE(node.linked(71710));
}

void test_heartbeat_period(void) {
  static int hb_filled = 0;
  hb_filled = 0;
  LinkHandlers h{};
  h.hb_fill = [](heartbeat_payload_t *hb) {
    hb->uptime_ms = 4242;
    hb->day_units = 17;
    hb_filled++;
  };
  MockTransport tb;
  LinkEngine node;
  node.begin(LinkEngine::Role::Node, &tb, h, 0);
  uint8_t hub_mac[6] = {0xBB, 0, 0, 0, 0, 4};
  node.add_peer(hub_mac, LINK_HUB_ID, SHARED_KEY, 32);

  node.tick(2000); /* first HB 2 s after boot */
  TEST_ASSERT_EQUAL_INT(1, hb_filled);
  TEST_ASSERT_EQUAL_UINT32(1, tb.sent.size());
  TEST_ASSERT_EQUAL_UINT8(MSG_HEARTBEAT, tb.sent[0].data[1]);
  heartbeat_payload_t hb;
  memcpy(&hb, tb.sent[0].data.data() + FRAME_HEADER_SIZE, sizeof(hb));
  TEST_ASSERT_EQUAL_UINT32(4242, hb.uptime_ms);

  /* keep the link "fresh" so the 60 s silence scan does not kick in */
  uint8_t buf[FRAME_HEADER_SIZE + FRAME_MAC_SIZE];
  size_t n = build_frame(buf, SHARED_KEY, MSG_STATUS_REQ, LINK_HUB_ID, 90, nullptr, 0);
  node.on_rx(hub_mac, buf, n, -60, 20000);

  node.tick(30000); /* 28 s since the last HB: period is 30 s */
  TEST_ASSERT_EQUAL_INT(1, hb_filled);
  node.tick(32001);
  TEST_ASSERT_EQUAL_INT(2, hb_filled);
}

void test_result_repeat_three_times(void) {
  MockTransport tb;
  LinkEngine node;
  node.begin(LinkEngine::Role::Node, &tb, LinkHandlers{}, 0);
  uint8_t hub_mac[6] = {0xBB, 0, 0, 0, 0, 5};
  node.add_peer(hub_mac, LINK_HUB_ID, SHARED_KEY, 32);

  node.tick(500); /* consumes the post-boot HB so it cannot pollute the count */
  tb.clear();

  node.send_feed_result(9, FEED_OK, 250, 0, 0xFF, 0, 1000);
  node.tick(1000);
  node.tick(1150); /* too early */
  TEST_ASSERT_EQUAL_UINT32(1, tb.sent.size());
  node.tick(1250);
  node.tick(1500);
  node.tick(1750); /* nothing: only 3 total */
  TEST_ASSERT_EQUAL_UINT32(3, tb.sent.size());
  /* all three are the same retransmitted frame (same seq) */
  TEST_ASSERT_EQUAL_UINT32(tb.sent[0].data[4], tb.sent[2].data[4]);
  TEST_ASSERT_EQUAL_UINT32(tb.sent[0].data[5], tb.sent[2].data[5]);
}

void test_schedule_helpers(void) {
  sched_entry_t e{8 * 60, 0x7F, 20}; /* 08:00 all days */
  TEST_ASSERT_TRUE(sched_entry_due(e, 8 * 60, 3));
  TEST_ASSERT_FALSE(sched_entry_due(e, 8 * 60 + 1, 3));
  sched_entry_t weekdays{8 * 60, 0x7E, 20}; /* bits 1..6 = Mon..Sat */
  TEST_ASSERT_FALSE(sched_day_due(weekdays.days_mask, 0)); /* Sunday */
  TEST_ASSERT_TRUE(sched_day_due(weekdays.days_mask, 1));  /* Monday */
  TEST_ASSERT_TRUE(sched_entry_due(weekdays, 480, 5));
  TEST_ASSERT_FALSE(sched_entry_due(weekdays, 480, 0));
  TEST_ASSERT_TRUE(sched_entry_due(weekdays, 480, 6)); /* Saturday */

  TEST_ASSERT_EQUAL_INT((int)SchedGate::Run, (int)schedule_gate(1000, 0, 0));
  TEST_ASSERT_EQUAL_INT((int)SchedGate::SkippedUser, (int)schedule_gate(999, 1000, 0));
  TEST_ASSERT_EQUAL_INT((int)SchedGate::Run, (int)schedule_gate(1000, 1000, 0));
  TEST_ASSERT_EQUAL_INT((int)SchedGate::SkippedHold, (int)schedule_gate(999, 1000, 2000));
  TEST_ASSERT_EQUAL_INT((int)SchedGate::SkippedHold, (int)schedule_gate(999, 0, 2000));
}

void test_schedule_set_fits_frame_and_store(void) {
  TEST_ASSERT_TRUE(SCHEDULE_MAX_ENTRIES >= 32);
  TEST_ASSERT_TRUE(SCHEDULE_STORE_MAX <= SCHEDULE_MAX_ENTRIES);
  MockTransport ta;
  LinkEngine hub;
  hub.begin(LinkEngine::Role::Hub, &ta, LinkHandlers{}, 0);
  uint8_t mac[6] = {0xAA, 0, 0, 0, 0, 9};
  hub.add_peer(mac, 1, SHARED_KEY, 32);
  sched_entry_t entries[SCHEDULE_MAX_ENTRIES];
  TEST_ASSERT_FALSE(hub.send_schedule_set(1, 1, entries, SCHEDULE_MAX_ENTRIES + 1, 0, 0, 0));
  TEST_ASSERT_TRUE(hub.send_schedule_set(1, 1, entries, SCHEDULE_STORE_MAX, 0, 0, 0));
  TEST_ASSERT_EQUAL_UINT32(1, ta.sent.size());
  TEST_ASSERT_TRUE(ta.sent[0].data.size() <= 250);
}

void test_limits_bench_bypass(void) {
  Limits l;
  l.configure(10, 20, 3000);
  TEST_ASSERT_TRUE(l.cooldown_ok(0)); /* never actuated → ok (SR4) */
  l.mark_actuated(1000);
  TEST_ASSERT_FALSE(l.cooldown_ok(1500)); /* 500 ms < 3000 ms cooldown */
  TEST_ASSERT_TRUE(l.would_exceed_hour(100));
  l.set_bench_bypass(true);
  TEST_ASSERT_TRUE(l.cooldown_ok(1500));       /* SR4 bypassed on the bench only */
  TEST_ASSERT_FALSE(l.would_exceed_hour(100)); /* SR3 bypassed */
  l.add(100);
  TEST_ASSERT_EQUAL_UINT32(100, l.counters().hour_units);
  l.set_bench_bypass(false);
  TEST_ASSERT_TRUE(l.would_exceed_hour(1)); /* counters still accumulated */
}

void test_config_blob_fits_slot(void) {
  TEST_ASSERT_TRUE(kConfigPayload + 2 <= CONFIG_SLOT_SIZE);
  TEST_ASSERT_TRUE(sizeof(NodeConfig) >= 400); /* sanity: new fields landed */
}

int main(void) {
  UNITY_BEGIN();
  for (int i = 0; i < 32; i++) SHARED_KEY[i] = (uint8_t)(0x10 + i);

  RUN_TEST(test_sha256_abc);
  RUN_TEST(test_hmac_rfc4231_case1);
  RUN_TEST(test_hmac_rfc4231_case2);
  RUN_TEST(test_frame_mac_tamper);
  RUN_TEST(test_pairing_wrap_roundtrip);
  RUN_TEST(test_feed_roundtrip);
  RUN_TEST(test_feed_retries_then_unknown);
  RUN_TEST(test_feed_dup_reanswered_not_actuated_twice);
  RUN_TEST(test_seq_window_and_replay);
  RUN_TEST(test_hub_answers_hello);
  RUN_TEST(test_channel_scan_hops_and_locks);
  RUN_TEST(test_heartbeat_period);
  RUN_TEST(test_result_repeat_three_times);
  RUN_TEST(test_schedule_helpers);
  RUN_TEST(test_schedule_set_fits_frame_and_store);
  RUN_TEST(test_limits_bench_bypass);
  RUN_TEST(test_config_blob_fits_slot);
  return UNITY_END();
}
