/* Host tests for the node safety core (PLAN.md §7.3, §3.3 SR2–SR8, §11.1).
 * These run on CI for every push; the same code runs on both node MCUs. */
#include <stddef.h>
#include <unity.h>
#include "crc16.h"
#include "profile.h"
#include "config_slots.h"
#include "node_limits.h"
#include "feed_journal.h"
#include "time_validity.h"
#include "actuator.h"

using namespace aio;

void setUp(void) {}
void tearDown(void) {}

/* ---- lib/proto ---------------------------------------------------------- */

void test_crc16_known_vector(void) {
  TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16_ccitt(0xFFFF, (const uint8_t *)"123456789", 9));
}

void test_profile_default_is_valid(void) {
  node_profile_t p = profile_default();
  TEST_ASSERT_TRUE(profile_valid(&p));
  p.max_on_ms_single = 12345;
  TEST_ASSERT_FALSE(profile_valid(&p));  /* CRC must break on edit */
}

void test_profile_gate_cap(void) {
  node_profile_t p = profile_default();
  p.max_on_ms_single = 60000;  /* absurd */
  TEST_ASSERT_EQUAL_UINT16(GATE_MAX_ON_MS, profile_max_on_ms(&p));  /* gate mode capped */
  p.mech = MECH_PUMP;
  TEST_ASSERT_EQUAL_UINT16(60000, profile_max_on_ms(&p));           /* pump not capped */
}

/* ---- config slots -------------------------------------------------------- */

void test_slot_select_picks_intact_newer(void) {
  uint8_t a[10] = {0}, b[10] = {0};
  a[0] = 1;  b[0] = 2;  /* seq in first 4 bytes */
  slot_fill_crc(a, 8);
  slot_fill_crc(b, 8);
  TEST_ASSERT_EQUAL_PTR(b, slot_select(a, b, 8));
  b[3] ^= 0xFF;  /* corrupt newer */
  TEST_ASSERT_EQUAL_PTR(a, slot_select(a, b, 8));
  a[2] ^= 0xFF;  /* corrupt both */
  TEST_ASSERT_NULL(slot_select(a, b, 8));
}

/* ---- limits (SR3/SR4/SR8) ------------------------------------------------ */

void test_limits_hour_day_rollover(void) {
  Limits l;
  l.configure(10, 20, 1000);
  int64_t h1 = (int64_t)100 * 3600 + 5;  /* hour 100 */
  l.load({}, h1);
  l.add(10);
  TEST_ASSERT_TRUE(l.would_exceed_hour(1));   /* hour full */
  TEST_ASSERT_FALSE(l.would_exceed_day(1));   /* day still has room */
  l.roll(h1 + 3600);                          /* next hour, same day */
  TEST_ASSERT_FALSE(l.would_exceed_hour(1));
  TEST_ASSERT_TRUE(l.would_exceed_day(11));   /* day cap unchanged: 10 used */
}

void test_limits_cooldown(void) {
  Limits l;
  l.configure(100, 100, 3000);
  TEST_ASSERT_TRUE(l.cooldown_ok(0));
  l.mark_actuated(5000);
  TEST_ASSERT_FALSE(l.cooldown_ok(7999));
  TEST_ASSERT_TRUE(l.cooldown_ok(8000));
}

/* ---- journal (SR6) -------------------------------------------------------- */

void test_journal_write_before_actuate_and_dedup(void) {
  FeedJournal j;
  uint8_t st;
  TEST_ASSERT_FALSE(j.known(42, &st));
  j.record_intent(42);
  TEST_ASSERT_TRUE(j.dirty);
  TEST_ASSERT_TRUE(j.known(42, &st));
  TEST_ASSERT_EQUAL_UINT8(FEED_ABORTED, st);  /* intent without result reads as attempted */
  j.record_result(42, FEED_OK, 250);
  TEST_ASSERT_TRUE(j.known(42, &st));
  TEST_ASSERT_EQUAL_UINT8(FEED_OK, st);
}

void test_journal_boot_in_progress_resolves_to_attempted(void) {
  FeedJournal j;
  j.record_intent(7);
  TEST_ASSERT_EQUAL_UINT32(1, j.resolve_in_progress());  /* boot: never repeat (SR6) */
  uint8_t st;
  TEST_ASSERT_TRUE(j.known(7, &st));
  TEST_ASSERT_EQUAL_UINT8(FEED_ABORTED, st);
  TEST_ASSERT_EQUAL_UINT32(0, j.resolve_in_progress());
}

void test_journal_ring_wraps(void) {
  FeedJournal j;
  for (uint32_t id = 1; id <= AIO_JOURNAL_SIZE + 4; id++) j.record_result(id, FEED_OK, 1);
  uint8_t st;
  TEST_ASSERT_FALSE(j.known(1, &st));                            /* oldest evicted */
  TEST_ASSERT_TRUE(j.known(AIO_JOURNAL_SIZE + 4, &st));          /* newest kept */
}

/* ---- time validity (SR7/D21) ---------------------------------------------- */

void test_time_valid_rtc_path(void) {
  TimeValidity t;
  t.on_rtc_status(true);
  int64_t day0 = (int64_t)1700000000;
  t.on_sync(day0, 100);
  TEST_ASSERT_TRUE(t.valid_rtc(day0 + 29 * 86400));   /* 29 days: trusted */
  TEST_ASSERT_FALSE(t.valid_rtc(day0 + 31 * 86400));  /* 31 days: expired */
  TEST_ASSERT_FALSE(t.valid_rtc(day0 - 10));          /* RTC behind sync: reject */
  t.on_rtc_status(false);                             /* oscillator stop */
  TEST_ASSERT_FALSE(t.valid_rtc(day0 + 5));
}

void test_time_valid_no_rtc_24h(void) {
  TimeValidity t;
  t.on_rtc_status(false);
  TEST_ASSERT_FALSE(t.valid_ms(1000));     /* never synced */
  t.on_sync(1700000000, 1000);
  TEST_ASSERT_TRUE(t.valid_ms(1000 + 23u * 3600u * 1000u));
  TEST_ASSERT_FALSE(t.valid_ms(1000 + 25u * 3600u * 1000u));
}

/* ---- actuator FSM (§7.3) --------------------------------------------------- */

struct Run {
  Actuator a;
  Limits l;
  FeedJournal j;
  ActuatorCmd cmd[8];
  uint32_t n = 0;
  const ActuatorCmd *find(ActuatorCmd::Kind k, uint32_t skip = 0) const {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < n; i++)
      if (cmd[i].kind == k && seen++ == skip) return &cmd[i];
    return nullptr;
  }
};

static void feed_once(Run &r, uint32_t amount, uint32_t now_ms, bool scheduled = false,
                      bool time_ok = true) {
  node_profile_t p = profile_default();  /* gate: 250 ms/g, max 10 s */
  r.l.configure(p.max_units_per_hour, p.max_units_per_day, p.cooldown_ms);
  r.l.limits_confirmed = true;
  r.a.configure(p);
  r.a.attach(&r.l, &r.j);
  FeedRequest req;
  req.feed_id = 1000;
  req.amount = amount;
  req.scheduled = scheduled;
  req.time_ok = time_ok;
  bool acc;
  uint8_t res;
  TEST_ASSERT_TRUE(r.a.request_feed(req, now_ms, &acc, &res));
  TEST_ASSERT_TRUE(acc);
  r.n += r.a.drain(r.cmd + r.n, 8 - r.n);
  TEST_ASSERT_TRUE(r.a.on_intent_saved(true, now_ms));
  r.n += r.a.drain(r.cmd + r.n, 8 - r.n);
}

void test_actuator_happy_gate_timed(void) {
  Run r; feed_once(r, 4, 1000);  /* 4 g × 250 ms = 1000 ms */
  const ActuatorCmd *intent = r.find(ActuatorCmd::PersistIntent);
  const ActuatorCmd *on = r.find(ActuatorCmd::RelayOn);
  const ActuatorCmd *arm = r.find(ActuatorCmd::ArmHardTimer);
  TEST_ASSERT_NOT_NULL(intent);
  TEST_ASSERT_NOT_NULL(on);
  TEST_ASSERT_NOT_NULL(arm);
  TEST_ASSERT_EQUAL_UINT32(1500, arm->ms);  /* 1000 + 500 margin (SR2) */
  /* order: intent before relay (SR6) */
  uint32_t i_intent = 0, i_on = 0;
  for (uint32_t i = 0; i < r.n; i++) {
    if (r.cmd[i].kind == ActuatorCmd::PersistIntent) i_intent = i;
    if (r.cmd[i].kind == ActuatorCmd::RelayOn) i_on = i;
  }
  TEST_ASSERT_LESS_THAN_UINT32(i_on, i_intent);
  /* finish at on_ms */
  r.a.tick(2000);  /* 1000 ms elapsed */
  r.n += r.a.drain(r.cmd + r.n, 8 - r.n);
  TEST_ASSERT_NOT_NULL(r.find(ActuatorCmd::RelayOff));
  TEST_ASSERT_NOT_NULL(r.find(ActuatorCmd::CancelHardTimer));
  const ActuatorCmd *rep = r.find(ActuatorCmd::ReportResult);
  TEST_ASSERT_NOT_NULL(rep);
  TEST_ASSERT_EQUAL_UINT8(FEED_OK, rep->result);
  TEST_ASSERT_EQUAL_UINT16(1000, rep->actual_on_ms);
  TEST_ASSERT_FALSE(r.a.idle());                  /* cooling down */
  r.a.tick(1000 + 4000);                          /* cooldown 3 s over */
  TEST_ASSERT_TRUE(r.a.idle());
}

void test_actuator_reject_reasons(void) {
  bool acc;
  uint8_t res;
  node_profile_t p = profile_default();
  Limits l;
  l.configure(p.max_units_per_hour, p.max_units_per_day, p.cooldown_ms);
  FeedJournal j;
  Actuator a;
  a.configure(p);
  a.attach(&l, &j);
  FeedRequest req;
  req.feed_id = 1;

  req.amount = 11;                       /* 11 g; hour cap 200 */
  l.add(195);                            /* 195 + 11 > 200 */
  TEST_ASSERT_FALSE(a.request_feed(req, 0, &acc, &res));
  TEST_ASSERT_EQUAL_UINT8(FEED_REJ_LIMIT_HOUR, res);

  l.load({}, 0);                         /* counters back to zero */
  req.amount = 100;                      /* 25 s > 10 s gate cap */
  TEST_ASSERT_FALSE(a.request_feed(req, 0, &acc, &res));
  TEST_ASSERT_EQUAL_UINT8(FEED_REJ_TOO_LONG, res);

  req.amount = 4;
  req.scheduled = true;
  req.time_ok = false;                   /* SR7 */
  TEST_ASSERT_FALSE(a.request_feed(req, 0, &acc, &res));
  TEST_ASSERT_EQUAL_UINT8(FEED_REJ_NO_TIME, res);

  req.time_ok = true;
  l.limits_confirmed = false;            /* SR8 */
  TEST_ASSERT_FALSE(a.request_feed(req, 0, &acc, &res));
  TEST_ASSERT_EQUAL_UINT8(FEED_REJ_LIMITS_UNCONFIRMED, res);

  l.limits_confirmed = true;
  req.manual_stop = true;                /* SR9 */
  TEST_ASSERT_FALSE(a.request_feed(req, 0, &acc, &res));
  TEST_ASSERT_EQUAL_UINT8(FEED_REJ_MANUAL_STOP, res);

  req.manual_stop = false;
  req.vin_ok = false;                    /* supply monitor */
  TEST_ASSERT_FALSE(a.request_feed(req, 0, &acc, &res));
  TEST_ASSERT_EQUAL_UINT8(FEED_REJ_LOW_VOLT, res);
}

void test_actuator_duplicate_feed_never_reactuates(void) {
  Run r; feed_once(r, 4, 1000);
  r.a.tick(1000 + 1000);      /* finish */
  r.a.drain(r.cmd, 8);
  r.a.tick(1000 + 1000 + 3000);  /* cooldown over */
  r.a.drain(r.cmd, 8);

  bool acc;
  uint8_t res;
  FeedRequest dup;
  dup.feed_id = 1000;  /* same id */
  dup.amount = 4;
  TEST_ASSERT_TRUE(r.a.request_feed(dup, 5000, &acc, &res));
  TEST_ASSERT_FALSE(acc);                /* answered from journal */
  TEST_ASSERT_EQUAL_UINT8(FEED_OK, res);
  TEST_ASSERT_NULL(r.find(ActuatorCmd::RelayOn));  /* no new actuation (SR6) */
}

void test_actuator_abort_counts_partial(void) {
  Run r; feed_once(r, 10, 0);   /* 10 g = 2500 ms */
  TEST_ASSERT_TRUE(r.a.abort(1000));  /* after 1000 ms on */
  r.n += r.a.drain(r.cmd + r.n, 8 - r.n);
  const ActuatorCmd *rep = r.find(ActuatorCmd::ReportResult);
  TEST_ASSERT_NOT_NULL(rep);
  TEST_ASSERT_EQUAL_UINT8(FEED_ABORTED, rep->result);
  TEST_ASSERT_EQUAL_UINT16(1000, rep->actual_on_ms);  /* partial */
}

void test_actuator_hard_timer_backstop_latches_fault(void) {
  Run r; feed_once(r, 4, 0);
  r.a.on_hard_timer_fired(60000);  /* timer fired late: loop stalled */
  r.n += r.a.drain(r.cmd + r.n, 8 - r.n);
  TEST_ASSERT_NOT_NULL(r.find(ActuatorCmd::RelayOff));
  TEST_ASSERT_TRUE(r.a.in_fault());
  const ActuatorCmd *rep = r.find(ActuatorCmd::ReportResult);
  TEST_ASSERT_EQUAL_UINT8(FEED_REJ_FAULT, rep->result);
  /* faulted node refuses everything */
  bool acc;
  uint8_t res;
  FeedRequest req;
  req.feed_id = 2;
  req.amount = 4;
  TEST_ASSERT_FALSE(r.a.request_feed(req, 70000, &acc, &res));
  TEST_ASSERT_EQUAL_UINT8(FEED_REJ_FAULT, res);
}

void test_actuator_pulses_mode(void) {
  node_profile_t p = profile_default();
  p.unit = UNIT_PULSES;
  p.mech = MECH_MOTOR_ONEWAY;
  p.pulse_on_ms = 50;
  p.pulse_off_ms = 100;
  p.max_on_ms_single = 1000;   /* max 20 pulses */
  p.cal_ms_per_unit_x100 = 0;
  p.cooldown_ms = 100;
  p.max_units_per_hour = 100;
  p.max_units_per_day = 100;
  p.crc16 = profile_crc16(&p);
  Limits l;
  l.configure(100, 100, 100);
  l.limits_confirmed = true;
  FeedJournal j;
  Actuator a;
  a.configure(p);
  a.attach(&l, &j);
  FeedRequest req;
  req.feed_id = 5;
  req.amount = 3;
  bool acc;
  uint8_t res;
  TEST_ASSERT_TRUE(a.request_feed(req, 0, &acc, &res));
  { ActuatorCmd tmp[8]; a.drain(tmp, 8); }
  TEST_ASSERT_TRUE(a.on_intent_saved(true, 0));
  { ActuatorCmd tmp[8]; a.drain(tmp, 8); }
  uint32_t ms = 0, offs = 0, ons = 0;
  ActuatorCmd cmd[8];
  uint32_t n;
  /* Uniform sweep: count every command wherever it appears. */
  for (int t = 0; t < 40 && !a.idle(); t++) {
    ms += 25;
    a.tick(ms);
    n = a.drain(cmd, 8);
    for (uint32_t i = 0; i < n; i++) {
      if (cmd[i].kind == ActuatorCmd::RelayOn) ons++;
      if (cmd[i].kind == ActuatorCmd::RelayOff) offs++;
      if (cmd[i].kind == ActuatorCmd::ReportResult) {
        TEST_ASSERT_EQUAL_UINT8(FEED_OK, cmd[i].result);
        TEST_ASSERT_EQUAL_UINT16(3, cmd[i].pulses);
        TEST_ASSERT_EQUAL_UINT16(150, cmd[i].actual_on_ms);  /* 3 × 50 ms */
      }
    }
  }
  /* initial RelayOn (from on_intent_saved) + 2 gap→on transitions = 3 */
  TEST_ASSERT_EQUAL_UINT32(3, offs);
  TEST_ASSERT_EQUAL_UINT32(3, ons + 1);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_crc16_known_vector);
  RUN_TEST(test_profile_default_is_valid);
  RUN_TEST(test_profile_gate_cap);
  RUN_TEST(test_slot_select_picks_intact_newer);
  RUN_TEST(test_limits_hour_day_rollover);
  RUN_TEST(test_limits_cooldown);
  RUN_TEST(test_journal_write_before_actuate_and_dedup);
  RUN_TEST(test_journal_boot_in_progress_resolves_to_attempted);
  RUN_TEST(test_journal_ring_wraps);
  RUN_TEST(test_time_valid_rtc_path);
  RUN_TEST(test_time_valid_no_rtc_24h);
  RUN_TEST(test_actuator_happy_gate_timed);
  RUN_TEST(test_actuator_reject_reasons);
  RUN_TEST(test_actuator_duplicate_feed_never_reactuates);
  RUN_TEST(test_actuator_abort_counts_partial);
  RUN_TEST(test_actuator_hard_timer_backstop_latches_fault);
  RUN_TEST(test_actuator_pulses_mode);
  return UNITY_END();
}
