/* Host tests for the M3 hub core (PLAN.md §7.2, §7.4, §9.4, §9.9, §12 M3).
 * Exit criterion under test: schedules fire correctly across reboots.
 *
 * All time is computed in Asia/Manila (UTC+8, no DST) unless a test opts
 * into an EU/US rule for the DST cases. "Local seconds space" = unix shifted
 * by the offset, per hub_time.h. */
#include <map>
#include <string>
#include <vector>

#include <unity.h>

#include "event_log.h"
#include "feed_manager.h"
#include "hub_core.h"
#include "hub_registry.h"
#include "hub_store.h"
#include "hub_time.h"
#include "messages.h"
#include "profile.h"

using namespace aio;

void setUp(void) {}
void tearDown(void) {}

/* ---- fakes ------------------------------------------------------------------ */

struct RamStore : IHubStore {
  std::map<std::string, std::vector<uint8_t>> m;
  bool put(const char *key, const uint8_t *data, size_t len) override {
    m[key] = std::vector<uint8_t>(data, data + len);
    return true;
  }
  size_t get(const char *key, uint8_t *out, size_t max_len) override {
    auto it = m.find(key);
    if (it == m.end()) return 0;
    size_t n = it->second.size() < max_len ? it->second.size() : max_len;
    memcpy(out, it->second.data(), n);
    return it->second.size();
  }
  bool del(const char *key) override { return m.erase(key) > 0; }
  void corrupt(const char *key) {
    if (!m[key].empty()) m[key].back() ^= 0xFF;
  }
};

struct MockLink : IHubLink {
  std::vector<std::pair<uint8_t, feed_payload_t>> sent;
  bool fail = false;
  bool send_feed(uint8_t node, const feed_payload_t &f, uint32_t) override {
    if (fail) return false;
    sent.emplace_back(node, f);
    return true;
  }
  size_t count_for(uint8_t node) const {
    size_t n = 0;
    for (auto &s : sent)
      if (s.first == node) n++;
    return n;
  }
};

/* Hub under test: registry + store + link + log + core. */
struct Hub {
  RamStore store;
  MockLink link;
  EventLog log;
  HubRegistry reg;
  HubCore core;

  Hub() { reg.data().tz_rule = TZ_MANILA; }

  void begin(int64_t unix_now) { core.begin(&reg, &link, &store, &log, unix_now); }
  /* reboot on the same storage device (one NVS) */
  Hub *reborn(int64_t unix_now) {
    Hub *h = new Hub();
    h->store = store;
    TEST_ASSERT_TRUE(h->reg.load(store));
    h->begin(unix_now);
    return h;
  }
  void tick(uint32_t ms, int64_t unix) { core.tick(ms, unix); }
};

/* Asia/Manila helpers: local seconds → unix. */
static constexpr int MANILA_OFF_S = 8 * 3600;
static int64_t manila_unix(int64_t local_s) { return local_s - MANILA_OFF_S; }

/* 2025-01-02 is a Thursday. Day starts, local space. */
static const int64_t DAY0 = days_from_civil(2025, 1, 2);
static int64_t local_at(int64_t day, int min) { return day * 86400 + min * 60; }

/* Make a node (id 1) with one daily entry at min_of_day, amount 20. */
static HubNode *add_node(Hub &h, uint16_t min_of_day, uint8_t days = 0x7F) {
  static const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0x00, 0x00, 0x01};
  HubNode *n = h.reg.add(mac, 1);
  TEST_ASSERT_NOT_NULL(n);
  n->sched.entries[0] = {min_of_day, days, 20};
  n->sched.entry_count = 1;
  n->sched.enabled = 1;
  return n;
}

/* ---- hub_time ----------------------------------------------------------------- */

void test_wday_and_civil_roundtrip(void) {
  TEST_ASSERT_EQUAL_UINT8(4, wday_from_days(0));  /* 1970-01-01 Thursday */
  TEST_ASSERT_EQUAL_UINT8(4, wday_from_days(DAY0));
  int64_t y;
  unsigned m, d;
  civil_from_days(days_from_civil(2025, 12, 31), &y, &m, &d);
  TEST_ASSERT_EQUAL_INT64(2025, y);
  TEST_ASSERT_EQUAL_UINT32(12, m);
  TEST_ASSERT_EQUAL_UINT32(31, d);
}

void test_tz_manila_fixed_offset(void) {
  const TzRule *r = tz_rule(TZ_MANILA);
  TEST_ASSERT_EQUAL_INT32(480, r->std_min);
  TEST_ASSERT_EQUAL_INT32(480, tz_offset_min(TZ_MANILA, 0));
  TEST_ASSERT_EQUAL_INT32(480, tz_offset_min(TZ_MANILA, 1750000000));
  const LocalTime t = local_from_unix(manila_unix(local_at(DAY0, 8 * 60)), TZ_MANILA);
  TEST_ASSERT_EQUAL_UINT16(480, t.min_of_day);
  TEST_ASSERT_EQUAL_UINT8(4, t.wday); /* Thursday */
  TEST_ASSERT_EQUAL_INT64(DAY0 * 86400, t.day_start);
}

void test_tz_eu_dst_transitions(void) {
  /* 2025-03-30 (last Sun Mar) 01:00 UTC; 2025-10-26 01:00 UTC */
  const int64_t start = days_from_civil(2025, 3, 30) * 86400 + 3600;
  const int64_t end = days_from_civil(2025, 10, 26) * 86400 + 3600;
  TEST_ASSERT_EQUAL_INT32(60, tz_offset_min(TZ_EU_CENTRAL, start - 1));
  TEST_ASSERT_EQUAL_INT32(120, tz_offset_min(TZ_EU_CENTRAL, start));
  TEST_ASSERT_EQUAL_INT32(120, tz_offset_min(TZ_EU_CENTRAL, end - 1));
  TEST_ASSERT_EQUAL_INT32(60, tz_offset_min(TZ_EU_CENTRAL, end));
}

void test_tz_us_dst_transitions(void) {
  /* 2025-03-09 (2nd Sun Mar) 07:00 UTC; 2025-11-02 (1st Sun Nov) 06:00 UTC */
  const int64_t start = days_from_civil(2025, 3, 9) * 86400 + 7 * 3600;
  const int64_t end = days_from_civil(2025, 11, 2) * 86400 + 6 * 3600;
  TEST_ASSERT_EQUAL_INT32(-300, tz_offset_min(TZ_US_EASTERN, start - 1));
  TEST_ASSERT_EQUAL_INT32(-240, tz_offset_min(TZ_US_EASTERN, start));
  TEST_ASSERT_EQUAL_INT32(-240, tz_offset_min(TZ_US_EASTERN, end - 1));
  TEST_ASSERT_EQUAL_INT32(-300, tz_offset_min(TZ_US_EASTERN, end));
}

/* ---- store -------------------------------------------------------------------- */

void test_store_blob_roundtrip_and_reject(void) {
  RamStore s;
  uint32_t v = 0xDEADBEEF;
  TEST_ASSERT_TRUE(store_put(s, "k", 7, &v, sizeof(v)));
  uint32_t out = 0;
  TEST_ASSERT_EQUAL_INT((int)sizeof(v), store_get(s, "k", 7, &out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT32(0xDEADBEEF, out);
  TEST_ASSERT_EQUAL_INT(-1, store_get(s, "k", 8, &out, sizeof(out))); /* ver skew */
  s.corrupt("k");
  TEST_ASSERT_EQUAL_INT(-1, store_get(s, "k", 7, &out, sizeof(out))); /* crc */
  TEST_ASSERT_EQUAL_INT(-1, store_get(s, "nope", 7, &out, sizeof(out)));
}

/* ---- registry ------------------------------------------------------------------ */

void test_registry_roundtrip_across_store(void) {
  RamStore s;
  static const uint8_t mac[6] = {0xAA, 0x01, 0x02, 0x03, 0x04, 0x05};
  HubRegistry a;
  HubNode *n = a.add(mac, 6);
  TEST_ASSERT_NOT_NULL(n);
  n->key[0] = 0x42;
  n->key_len = 16;
  n->profile.max_units_per_day = 321;
  n->profile.crc16 = profile_crc16(&n->profile); /* keep the profile intact */
  n->sched.entries[0] = {480, 0x7F, 25};
  n->sched.entry_count = 1;
  n->run.last_done[0].day_start = 123;
  TEST_ASSERT_TRUE(a.save(s));

  HubRegistry b;
  TEST_ASSERT_TRUE(b.load(s));
  HubNode *m = b.by_id(n->id);
  TEST_ASSERT_NOT_NULL(m);
  TEST_ASSERT_EQUAL_UINT8(6, m->channel);
  TEST_ASSERT_EQUAL_UINT8(0x42, m->key[0]);
  TEST_ASSERT_EQUAL_UINT16(321, m->profile.max_units_per_day);
  TEST_ASSERT_EQUAL_UINT32(123, m->run.last_done[0].day_start);
  TEST_ASSERT_EQUAL_UINT16(480, m->sched.entries[0].min_of_day);
  TEST_ASSERT_TRUE(profile_valid(&m->profile));
  s.corrupt("reg");
  HubRegistry c;
  TEST_ASSERT_FALSE(c.load(s)); /* CRC reject → fresh, not garbage */
}

/* ---- event log ------------------------------------------------------------------ */

void test_event_log_ring_wraps_and_dedups(void) {
  EventLog l;
  for (uint32_t i = 0; i < 200; i++)
    l.push(i, 1, EV_FEED_OK, 0, i, 10);
  TEST_ASSERT_EQUAL_UINT16(EventLog::CAP, l.count());
  TEST_ASSERT_EQUAL_UINT32(72, l.at(0)->unix); /* oldest kept = 200 - 128 */
  TEST_ASSERT_EQUAL_UINT32(199, l.at(l.count() - 1)->unix);
  TEST_ASSERT_TRUE(l.has_result(199));
  TEST_ASSERT_FALSE(l.has_result(10)); /* evicted */
  l.push(0, 1, EV_CATCHUP, 0, 777, 10);
  TEST_ASSERT_FALSE(l.has_result(777)); /* CATCHUP is not a terminal result */
}

/* ---- feed manager FSM ------------------------------------------------------------ */

static feed_ack_payload_t mk_ack(uint32_t id, uint8_t accepted, uint8_t reason) {
  feed_ack_payload_t a;
  memset(&a, 0, sizeof(a));
  a.feed_id = id;
  a.accepted = accepted;
  a.reason = reason;
  return a;
}
static feed_result_payload_t mk_res(uint32_t id, uint8_t status) {
  feed_result_payload_t r;
  memset(&r, 0, sizeof(r));
  r.feed_id = id;
  r.status = status;
  r.actual_on_ms = 100;
  r.pulses = 1;
  r.gate_ok = 0xFF;
  return r;
}

void test_fm_full_fsm_ack_then_result(void) {
  RamStore s;
  MockLink l;
  EventLog log;
  FeedManager fm;
  fm.begin(&l, &s, &log, 1);
  TEST_ASSERT_TRUE(fm.submit({1000, 1, 20, UNIT_GRAMS, (uint8_t)FeedKind::Scheduled}));
  fm.tick(0, 1000);
  TEST_ASSERT_EQUAL_INT(1, fm.inflight_count());
  TEST_ASSERT_EQUAL_size_t(1, l.sent.size());
  fm.on_ack(1, mk_ack(1000, 1, 0), 1001);
  fm.on_result(1, mk_res(1000, RESULT_OK), 1002);
  TEST_ASSERT_EQUAL_INT(0, fm.inflight_count());
  TEST_ASSERT_TRUE(log.has_result(1000));
  TEST_ASSERT_EQUAL_UINT16(1, log.count_of(EV_FEED_OK));
  fm.on_result(1, mk_res(1000, RESULT_OK), 1003); /* repeated result */
  TEST_ASSERT_EQUAL_UINT16(1, log.count_of(EV_FEED_OK)); /* no double count */
}

void test_fm_reject_is_terminal(void) {
  RamStore s;
  MockLink l;
  EventLog log;
  FeedManager fm;
  fm.begin(&l, &s, &log, 1);
  fm.submit({1001, 1, 20, UNIT_GRAMS, (uint8_t)FeedKind::Manual});
  fm.tick(0, 1000);
  fm.on_ack(1, mk_ack(1001, 0, FEED_REJ_COOLDOWN), 1001);
  TEST_ASSERT_EQUAL_INT(0, fm.inflight_count());
  TEST_ASSERT_EQUAL_UINT16(1, log.count_of(EV_FEED_REJ));
}

void test_fm_unknown_when_retries_exhausted(void) {
  RamStore s;
  MockLink l;
  EventLog log;
  FeedManager fm;
  fm.begin(&l, &s, &log, 1);
  fm.submit({1002, 1, 20, UNIT_GRAMS, (uint8_t)FeedKind::Scheduled});
  fm.tick(0, 1000);
  fm.on_unknown(1002, 1, 5000);
  TEST_ASSERT_EQUAL_INT(0, fm.inflight_count());
  TEST_ASSERT_EQUAL_UINT16(1, log.count_of(EV_FEED_UNKNOWN));
}

void test_fm_stagger_and_node_mutex(void) {
  RamStore s;
  MockLink l;
  EventLog log;
  FeedManager fm;
  fm.begin(&l, &s, &log, 1);
  TEST_ASSERT_TRUE(fm.submit({1, 1, 10, UNIT_GRAMS, 0}));
  TEST_ASSERT_FALSE(fm.submit({2, 1, 10, UNIT_GRAMS, 0})); /* same node busy */
  TEST_ASSERT_TRUE(fm.submit({3, 2, 10, UNIT_GRAMS, 0}));  /* other node queues */
  fm.tick(0, 1000);
  TEST_ASSERT_EQUAL_size_t(1, l.sent.size()); /* stagger 1: node 1 only */
  fm.on_result(1, mk_res(1, RESULT_OK), 1001);
  fm.tick(1002, 1002);
  TEST_ASSERT_EQUAL_size_t(2, l.sent.size());
  TEST_ASSERT_EQUAL_UINT8(2, l.sent[1].first);

  fm.set_stagger(2);
  fm.submit({4, 3, 10, UNIT_GRAMS, 0});
  fm.tick(1003, 1003);
  TEST_ASSERT_EQUAL_size_t(3, l.sent.size()); /* capacity 2 → sent at once */
}

void test_fm_send_fail_is_no_link_event(void) {
  RamStore s;
  MockLink l;
  EventLog log;
  FeedManager fm;
  fm.begin(&l, &s, &log, 1);
  l.fail = true;
  fm.submit({9, 1, 10, UNIT_GRAMS, 0});
  fm.tick(0, 1000);
  TEST_ASSERT_EQUAL_INT(0, fm.inflight_count());
  TEST_ASSERT_EQUAL_UINT16(1, log.count_of(EV_FEED_FAIL));
  TEST_ASSERT_EQUAL_UINT8(EVENT_RESULT_NO_LINK, log.at(0)->result);
}

/* ---- schedule engine across reboots (§12 M3 exit criterion) ---------------------- */

void test_first_boot_fires_on_time_then_no_refire_same_minute(void) {
  Hub h;
  add_node(h, 8 * 60); /* 08:00 daily, 20 g */
  h.reg.save(h.store);
  const int64_t fire_unix = manila_unix(local_at(DAY0, 8 * 60)) + 10; /* 08:00:10 */
  h.begin(fire_unix);
  h.tick(0, fire_unix);
  TEST_ASSERT_EQUAL_size_t(1, h.link.sent.size());
  TEST_ASSERT_EQUAL_UINT16(20, h.link.sent[0].second.amount);
  h.tick(60000, fire_unix + 50); /* same minute */
  TEST_ASSERT_EQUAL_size_t(1, h.link.sent.size());
  TEST_ASSERT_EQUAL_UINT32(DAY0 * 86400, h.core.last_done(1, 0).day_start);
  TEST_ASSERT_EQUAL_UINT16(480, h.core.last_done(1, 0).min_of_day);
}

void test_reboot_same_day_does_not_refire(void) {
  Hub h;
  add_node(h, 480);
  h.reg.save(h.store);
  h.begin(manila_unix(local_at(DAY0, 480)) + 10);
  h.tick(0, manila_unix(local_at(DAY0, 480)) + 10);
  TEST_ASSERT_EQUAL_size_t(1, h.link.sent.size());

  Hub *h2 = h.reborn(manila_unix(local_at(DAY0, 481))); /* 08:01 */
  h2->tick(0, manila_unix(local_at(DAY0, 481)));
  TEST_ASSERT_EQUAL_size_t(0, h2->link.sent.size());
  /* next day 08:00 fires again */
  h2->tick(1000, manila_unix(local_at(DAY0 + 1, 480)));
  TEST_ASSERT_EQUAL_size_t(1, h2->link.sent.size());
  delete h2;
}

void test_missed_default_policy_skips_after_reboot(void) {
  Hub h;
  add_node(h, 480); /* policy 0 (default) */
  h.reg.save(h.store);
  /* first boot yesterday evening: occurrence bookkeeping starts fresh */
  h.begin(manila_unix(local_at(DAY0 - 1, 20 * 60)));
  h.tick(0, manila_unix(local_at(DAY0 - 1, 20 * 60)));

  Hub *h2 = h.reborn(manila_unix(local_at(DAY0, 10 * 60))); /* 10:00, missed 08:00 */
  h2->tick(0, manila_unix(local_at(DAY0, 10 * 60)));
  TEST_ASSERT_EQUAL_size_t(0, h2->link.sent.size());       /* never stacked */
  TEST_ASSERT_EQUAL_UINT16(1, h2->log.count_of(EV_SKIPPED_MISSED));
  TEST_ASSERT_EQUAL_UINT32(DAY0 * 86400, h2->core.last_done(1, 0).day_start);
  delete h2;
}

void test_catchup_once_within_window(void) {
  Hub h;
  add_node(h, 480);
  h.reg.by_id(1)->profile.missed_policy = 1;
  h.reg.by_id(1)->profile.catchup_window_min = 30;
  h.reg.save(h.store);
  h.begin(manila_unix(local_at(DAY0 - 1, 20 * 60)));
  h.tick(0, manila_unix(local_at(DAY0 - 1, 20 * 60)));

  Hub *h2 = h.reborn(manila_unix(local_at(DAY0, 8 * 60 + 10))); /* 08:10, age 10 min */
  h2->tick(0, manila_unix(local_at(DAY0, 8 * 60 + 10)));
  TEST_ASSERT_EQUAL_size_t(1, h2->link.sent.size()); /* exactly one catch-up */
  TEST_ASSERT_EQUAL_UINT16(1, h2->log.count_of(EV_CATCHUP));
  /* second reboot 10 min later: occurrence already handled → nothing */
  Hub *h3 = h2->reborn(manila_unix(local_at(DAY0, 8 * 60 + 20)));
  h3->tick(0, manila_unix(local_at(DAY0, 8 * 60 + 20)));
  TEST_ASSERT_EQUAL_size_t(0, h3->link.sent.size());
  delete h3;
  delete h2;
}

void test_catchup_window_expired_stays_skipped(void) {
  Hub h;
  add_node(h, 480);
  h.reg.by_id(1)->profile.missed_policy = 1;
  h.reg.save(h.store);
  h.begin(manila_unix(local_at(DAY0 - 1, 20 * 60)));
  h.tick(0, manila_unix(local_at(DAY0 - 1, 20 * 60)));
  Hub *h2 = h.reborn(manila_unix(local_at(DAY0, 10 * 60))); /* age 2 h > 30 min */
  h2->tick(0, manila_unix(local_at(DAY0, 10 * 60)));
  TEST_ASSERT_EQUAL_size_t(0, h2->link.sent.size());
  TEST_ASSERT_EQUAL_UINT16(1, h2->log.count_of(EV_SKIPPED_MISSED));
  delete h2;
}

void test_multi_day_outage_never_stacks(void) {
  Hub h;
  add_node(h, 480);
  h.reg.by_id(1)->profile.missed_policy = 1;
  h.reg.save(h.store);
  h.begin(manila_unix(local_at(DAY0, 480))); /* fires day 0 */
  h.tick(0, manila_unix(local_at(DAY0, 480)));
  TEST_ASSERT_EQUAL_size_t(1, h.link.sent.size());

  Hub *h2 = h.reborn(manila_unix(local_at(DAY0 + 3, 8 * 60 + 5))); /* 3 days later */
  h2->tick(0, manila_unix(local_at(DAY0 + 3, 8 * 60 + 5)));
  TEST_ASSERT_EQUAL_size_t(1, h2->link.sent.size()); /* one catch-up, not three */
  TEST_ASSERT_EQUAL_UINT16(1, h2->log.count_of(EV_CATCHUP));
  /* days 1-2 reported as ONE event, aux = occurrence count */
  TEST_ASSERT_EQUAL_UINT16(1, h2->log.count_of(EV_SKIPPED_MISSED));
  for (uint16_t i = 0; i < h2->log.count(); i++) {
    const HubEvent *e = h2->log.at(i);
    if (e->kind == EV_SKIPPED_MISSED) TEST_ASSERT_EQUAL_INT16(2, e->aux);
  }
  delete h2;
}

void test_skip_and_hold_gates(void) {
  Hub h;
  add_node(h, 480);
  const int64_t boot = manila_unix(local_at(DAY0, 479));
  h.begin(boot);
  /* hold until tomorrow → today's occurrence SKIPPED_HOLD */
  h.reg.by_id(1)->sched.hold_until = (uint32_t)(boot + 86400);
  h.tick(0, boot + 60); /* 08:00 */
  TEST_ASSERT_EQUAL_size_t(0, h.link.sent.size());
  TEST_ASSERT_EQUAL_UINT16(1, h.log.count_of(EV_SKIPPED_HOLD));

  /* skip_until in the past → runs normally */
  Hub h2;
  add_node(h2, 480);
  const int64_t boot2 = manila_unix(local_at(DAY0, 479));
  h2.begin(boot2);
  h2.reg.by_id(1)->sched.skip_until = (uint32_t)(boot2 - 3600);
  h2.tick(0, boot2 + 60);
  TEST_ASSERT_EQUAL_size_t(1, h2.link.sent.size());
}

void test_reboot_mid_feed_becomes_unknown_never_refires(void) {
  Hub h;
  add_node(h, 480);
  h.reg.save(h.store);
  const int64_t fire = manila_unix(local_at(DAY0, 480));
  h.begin(fire);
  h.tick(0, fire); /* FEED sent, hub "crashes" before ack/result */
  TEST_ASSERT_EQUAL_size_t(1, h.link.sent.size());
  const uint32_t dead_id = h.link.sent[0].second.feed_id;

  Hub *h2 = h.reborn(fire + 120); /* two minutes later */
  TEST_ASSERT_EQUAL_UINT16(1, h2->log.count_of(EV_FEED_UNKNOWN));
  h2->tick(0, fire + 120);
  TEST_ASSERT_EQUAL_size_t(0, h2->link.sent.size()); /* §6.6: no blind re-issue */
  /* the node's (repeated) result arrives after reconnect: the UNKNOWN from
   * the reboot is already the terminal record (§9.7 dedup) — no new event */
  h2->core.on_feed_result(1, mk_res(dead_id, RESULT_OK), (uint32_t)(fire + 130));
  TEST_ASSERT_TRUE(h2->log.has_result(dead_id));
  h2->core.on_feed_result(1, mk_res(dead_id, RESULT_OK), (uint32_t)(fire + 131));
  TEST_ASSERT_EQUAL_UINT16(1, h2->log.count_of(EV_FEED_UNKNOWN));
  TEST_ASSERT_EQUAL_UINT16(0, h2->log.count_of(EV_FEED_FAIL)); /* counted once */
  delete h2;
}

void test_boot_scan_deferred_until_clock_valid(void) {
  Hub h;
  add_node(h, 480);
  h.reg.save(h.store);
  h.begin(0); /* RTC dead, no NTP yet → SR7: no scheduled feeds */
  h.tick(0, 0);
  TEST_ASSERT_EQUAL_size_t(0, h.link.sent.size());
  /* clock becomes valid right at 08:00 → boot scan fires the occurrence */
  const int64_t t = manila_unix(local_at(DAY0, 480));
  h.tick(1000, t);
  TEST_ASSERT_EQUAL_size_t(1, h.link.sent.size());
}

void test_dst_spring_gap_fires_just_after(void) {
  Hub h;
  add_node(h, 150); /* 02:30 — inside the skipped hour */
  h.reg.data().tz_rule = TZ_EU_CENTRAL;
  /* 2025-03-30 is the last Sunday of March (skips 02:00→03:00 local) */
  const int64_t day = days_from_civil(2025, 3, 30);
  /* tick 1: local 01:59 CET → nothing due yet */
  const int64_t t1 = local_at(day, 119) - 60 * 60; /* CET offset */
  h.begin(t1);
  h.tick(0, t1);
  TEST_ASSERT_EQUAL_size_t(0, h.link.sent.size());
  /* tick 2: local 03:00 CEST → 02:30 occurrence fires ("just after the gap") */
  const int64_t t2 = local_at(day, 180) - 120 * 60; /* CEST offset */
  h.tick(1000, t2);
  TEST_ASSERT_EQUAL_size_t(1, h.link.sent.size());
}

void test_dst_fall_repeat_hour_fires_once(void) {
  Hub h;
  add_node(h, 150); /* 02:30 — inside the repeated hour */
  h.reg.data().tz_rule = TZ_EU_CENTRAL;
  const int64_t day = days_from_civil(2025, 10, 26);
  /* first 02:30 local (still CEST): 00:30 UTC */
  const int64_t t1 = local_at(day, 150) - 120 * 60;
  h.begin(t1);
  h.tick(0, t1);
  TEST_ASSERT_EQUAL_size_t(1, h.link.sent.size());
  /* second 02:30 local (CET now): 01:30 UTC, one hour later */
  const int64_t t2 = t1 + 3600;
  h.tick(1000, t2);
  TEST_ASSERT_EQUAL_size_t(1, h.link.sent.size()); /* first occurrence only */
}

void test_day_mask_and_two_entries_per_day(void) {
  Hub h;
  static const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0x00, 0x00, 0x02};
  HubNode *n = h.reg.add(mac, 1);
  n->sched.entries[0] = {480, 0x10, 20};     /* bit4 = Thursday only */
  n->sched.entries[1] = {20 * 60, 0x7F, 5};  /* 20:00 daily */
  n->sched.entry_count = 2;
  h.reg.save(h.store);
  /* 2025-01-02 IS a Thursday: 08:00 fires; 2025-01-03 (Friday): it does not */
  TEST_ASSERT_EQUAL_UINT8(4,
                          local_from_unix(manila_unix(local_at(DAY0, 0)), TZ_MANILA).wday);
  h.begin(manila_unix(local_at(DAY0, 480)));
  h.tick(0, manila_unix(local_at(DAY0, 480))); /* Thursday 08:00 fires */
  TEST_ASSERT_EQUAL_size_t(1, h.link.sent.size()); /* Thursday 08:00 ✓ */
  h.core.on_feed_result(1, mk_res(h.link.sent[0].second.feed_id, RESULT_OK), 1);
  h.tick(1, manila_unix(local_at(DAY0, 20 * 60)));
  TEST_ASSERT_EQUAL_size_t(2, h.link.sent.size()); /* 20:00 daily ✓ */
  h.core.on_feed_result(1, mk_res(h.link.sent[1].second.feed_id, RESULT_OK), 2);
  h.tick(2, manila_unix(local_at(DAY0 + 1, 480))); /* Friday 08:00 ✗ */
  TEST_ASSERT_EQUAL_size_t(2, h.link.sent.size());
  h.tick(3, manila_unix(local_at(DAY0 + 1, 20 * 60)));
  TEST_ASSERT_EQUAL_size_t(3, h.link.sent.size()); /* Friday 20:00 ✓ */
}

void test_manual_feed_via_core(void) {
  Hub h;
  add_node(h, 480);
  h.begin(manila_unix(local_at(DAY0, 480)));
  h.tick(0, manila_unix(local_at(DAY0, 480))); /* scheduled feed fires + sends */
  TEST_ASSERT_EQUAL_size_t(1, h.link.sent.size());
  h.core.on_feed_result(1, mk_res(h.link.sent[0].second.feed_id, RESULT_OK),
                        (uint32_t)manila_unix(local_at(DAY0, 480)) + 5);
  /* node free again → manual feed goes through */
  TEST_ASSERT_TRUE(h.core.request_manual(1, 7, 500, manila_unix(local_at(DAY0, 480)) + 6));
  TEST_ASSERT_EQUAL_size_t(2, h.link.sent.size());
  TEST_ASSERT_EQUAL_UINT16(7, h.link.sent[1].second.amount);
  TEST_ASSERT_EQUAL_UINT32(1001, h.link.sent[1].second.feed_id); /* reg seq */
  TEST_ASSERT_FALSE(h.core.request_manual(1, 7, 501, 0)); /* node busy */
  TEST_ASSERT_FALSE(h.core.request_manual(9, 7, 502, 0)); /* no such node */
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_wday_and_civil_roundtrip);
  RUN_TEST(test_tz_manila_fixed_offset);
  RUN_TEST(test_tz_eu_dst_transitions);
  RUN_TEST(test_tz_us_dst_transitions);
  RUN_TEST(test_store_blob_roundtrip_and_reject);
  RUN_TEST(test_registry_roundtrip_across_store);
  RUN_TEST(test_event_log_ring_wraps_and_dedups);
  RUN_TEST(test_fm_full_fsm_ack_then_result);
  RUN_TEST(test_fm_reject_is_terminal);
  RUN_TEST(test_fm_unknown_when_retries_exhausted);
  RUN_TEST(test_fm_stagger_and_node_mutex);
  RUN_TEST(test_fm_send_fail_is_no_link_event);
  RUN_TEST(test_first_boot_fires_on_time_then_no_refire_same_minute);
  RUN_TEST(test_reboot_same_day_does_not_refire);
  RUN_TEST(test_missed_default_policy_skips_after_reboot);
  RUN_TEST(test_catchup_once_within_window);
  RUN_TEST(test_catchup_window_expired_stays_skipped);
  RUN_TEST(test_multi_day_outage_never_stacks);
  RUN_TEST(test_skip_and_hold_gates);
  RUN_TEST(test_reboot_mid_feed_becomes_unknown_never_refires);
  RUN_TEST(test_boot_scan_deferred_until_clock_valid);
  RUN_TEST(test_dst_spring_gap_fires_just_after);
  RUN_TEST(test_dst_fall_repeat_hour_fires_once);
  RUN_TEST(test_day_mask_and_two_entries_per_day);
  RUN_TEST(test_manual_feed_via_core);
  return UNITY_END();
}
