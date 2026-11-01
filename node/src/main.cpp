/*
 * AIO Feeder — node firmware, M2 link (PLAN.md §7.3, §12 M2).
 *
 * Cooperative loop: CLI → ESP-NOW drain → actuator tick → link tick
 * → command drain (HAL side effects) → persistence → periodic sampling.
 * One actuation at a time; the relay-off hardware timer is the SR2
 * guarantee, the loop is only the fast path.
 *
 * Radio: the FSM's ReportResult commands become FEED_RESULT frames over the
 * same seam the CLI prints on; FEED frames enter the same request_feed() the
 * CLI uses, so a radio feed and a bench feed are indistinguishable to the
 * safety core (SR2–SR9 all apply; bench modes are RAM-only, docs/bench-m2.md).
 */
#include <Arduino.h>

#include "../../lib/proto/pairing.h"
#include "../../lib/proto/proto.h"
#include "node_config.h"

#if defined(AIO_NODE_8266)
#include "hal_esp8266.h"
#include "now_esp8266.h"
#elif defined(AIO_NODE_C3)
#include "hal_esp32c3.h"
#include "now_esp32.h"
#else
#error "Define AIO_NODE_8266 or AIO_NODE_C3 (see platformio.ini)"
#endif

#include "../../link_core/link.h"
#include "../../node_core/actuator.h"
#include "../../node_core/feed_journal.h"
#include "../../node_core/node_limits.h"
#include "../../node_core/schedule.h"
#include "../../node_core/time_validity.h"
#include "cli.h"

/* Supply monitor: reject below this rail voltage (12 V nominal) [VERIFY]. */
constexpr uint16_t VIN_MIN_MV = 10800;
/* Readings below this are treated as "no divider fitted" → allowed, warned. */
constexpr uint16_t VIN_ABSENT_MV = 2000;

static Hal hal;
static NodeConfig cfg;
static uint8_t g_own_mac[6]; /* station MAC — §6.3 pairing code input */
static aio::Limits limits;
static aio::FeedJournal journal;
static aio::TimeValidity tv;
static aio::Actuator act;
static aio::LinkEngine eng;
static CliCtx cli_ctx;
static bool time_ok = false;
static uint16_t vin_mv = 0;
static bool vin_present = false;
static uint32_t last_persist_ms = 0;
static bool persist_pending = false;
static uint32_t last_rtc_poll = 0;
static uint32_t last_vin_poll = 0;
static uint32_t boot_ms = 0;
static uint8_t bench_mode = BENCH_OFF;
static uint32_t last_feed_id = 0;
static uint8_t region[kStorageRegion];

#if defined(AIO_NODE_8266)
static NowTransport8266 nowt;
#else
static NowTransportEsp32 nowt;
#endif

/* --- persistence ------------------------------------------------------------ */

/* RAM → blob mirror, then commit one slot (alternating). */
static void persist_now() {
  memcpy(cfg.journal, (const void *)journal.entries(), sizeof(cfg.journal));
  cfg.counters = limits.counters();
  cfg.sync_unix = tv.sync_unix();
  cfg.limits_confirmed = limits.limits_confirmed ? 1 : 0;
  cfg.tx_seq = eng.current_tx_seq();
  cfg.write_seq++;
  config_fill_slot(region, cfg);
  uint8_t *dst = region + (cfg.write_seq & 1 ? CONFIG_SLOT_SIZE : 0);
  uint8_t *src = region + (cfg.write_seq & 1 ? 0 : CONFIG_SLOT_SIZE);
  memcpy(dst, src, CONFIG_SLOT_SIZE);
  hal.storage_write(region, kStorageRegion);
  journal.dirty = false;
  last_persist_ms = millis();
  persist_pending = false;
}

/* Factory reset: both slots overwritten, nothing recoverable. */
static void erase_config() {
  cfg = NodeConfig{};
  cfg.magic = CONFIG_MAGIC;
  cfg.write_seq = 1;
  cfg.node_id = 1;
  cfg.booted = 1;
  cfg.profile = profile_default();
  cfg.feed_id_seq = 1000;
  config_fill_slot(region, cfg);
  memcpy(region + CONFIG_SLOT_SIZE, region, CONFIG_SLOT_SIZE);
  hal.storage_write(region, kStorageRegion);
  journal.dirty = false;
  Serial.println(F("config erased → defaults; rebooting"));
  delay(100);
  ESP.restart();
}

static void persist_maybe() {
  if (!journal.dirty && !persist_pending) return;
  if (millis() - last_persist_ms < 30000) {
    persist_pending = true;
    return;
  }
  persist_now();
}

/* --- time validity refresh (SR7) --------------------------------------------- */

static void refresh_time(uint32_t now_ms) {
  if (last_rtc_poll != 0 && now_ms - last_rtc_poll < 60000) return;
  last_rtc_poll = now_ms;
  int64_t unix_now = 0;
  bool osf = false;
  if (hal.rtc_healthy(&unix_now, &osf)) {
    tv.on_rtc_status(true);
    limits.roll(unix_now);
    time_ok = tv.valid_rtc(unix_now);
  } else {
    tv.on_rtc_status(false);
    time_ok = tv.valid_ms(now_ms); /* no RTC: 24 h window since boot sync */
  }
}

/* --- link handlers ------------------------------------------------------------- */

static int64_t unix_now_guess() {
  bool osf;
  int64_t u = 0;
  if (hal.rtc_healthy(&u, &osf)) return u;
  return time_ok ? tv.sync_unix() + (int64_t)((millis() - tv.sync_ms()) / 1000) : 0;
}

static void hb_fill(heartbeat_payload_t *hb) {
  hb->uptime_ms = millis() - boot_ms;
  hb->rssi = 0; /* not exposed by these SDK callbacks */
  hb->vin_mv = vin_mv;
  hb->hopper_low = 0; /* no hopper sensor on the bench rig yet */
  hb->last_feed_id = last_feed_id;
  hb->day_units = (uint16_t)limits.counters().day_units;
  uint8_t flags = 0;
  if (cfg.manual_stop) flags |= HB_FLAG_MANUAL_STOP;
  if (!time_ok) flags |= HB_FLAG_TIME_INVALID;
  if (!limits.limits_confirmed) flags |= HB_FLAG_LIMITS_UNCONFIRMED;
  bool osf;
  int64_t u = 0;
  if (hal.rtc_healthy(&u, &osf)) {
    flags |= HB_FLAG_RTC_OK;
    if (osf) flags |= HB_FLAG_RTC_LOST;
  }
  hb->flags = flags;
  hb->rtc_drift_ms = cfg.rtc_drift_ms;
}

static void handle_feed(const uint8_t mac[6], const feed_payload_t &f) {
  uint32_t now = millis();

  /* Bench radio mode: pure protocol loopback, relay never energised. */
  if (bench_mode == BENCH_RADIO) {
    eng.send_feed_ack(f.feed_id, 1, FEED_OK, now);
    eng.send_feed_result(f.feed_id, FEED_OK, 0, 0, 0xFF, (uint32_t)unix_now_guess(), now);
    return;
  }

  /* SR6: duplicates answer from the journal, never actuate again. */
  const aio::JournalEntry *e = journal.find(f.feed_id);
  if (e) {
    if (e->in_progress) {
      eng.send_feed_ack(f.feed_id, 1, FEED_OK, now); /* retry lost its ACK; still running */
    } else {
      eng.send_feed_result(f.feed_id, e->status, e->actual_on_ms, 0, 0xFF,
                           (uint32_t)unix_now_guess(), now);
    }
    Serial.printf("RX feed id=%u -> journal dup (%s)\n", f.feed_id,
                  e->in_progress ? "in progress" : result_name(e->status));
    return;
  }

  aio::FeedRequest req{};
  req.feed_id = f.feed_id;
  req.amount = f.amount;
  /* Radio feeds are the "scheduled" class (SR7/SR8); bench live mode downgrades
   * to manual-class so the loopback can run without time/confirmation. */
  req.scheduled = bench_mode != BENCH_LIVE;
  req.time_ok = time_ok;
  req.manual_stop = cfg.manual_stop;
  req.vin_ok = !vin_present || vin_mv >= VIN_ABSENT_MV;
  bool acc;
  uint8_t res;
  act.request_feed(req, now, &acc, &res);
  eng.send_feed_ack(f.feed_id, acc ? 1 : 0, res, now);
  Serial.printf("RX feed id=%u amount=%u -> %s\n", f.feed_id, f.amount,
                acc ? "accepted" : result_name(res));
}

static void report_result(const aio::ActuatorCmd &c) {
  last_feed_id = c.feed_id;
  Serial.printf("RESULT id=%u status=%s actual_on_ms=%u pulses=%u\n", c.feed_id,
                result_name(c.result), c.actual_on_ms, c.pulses);
  eng.send_feed_result(c.feed_id, c.result, c.actual_on_ms, c.pulses, 0xFF,
                       (uint32_t)unix_now_guess(), millis());
}

static void handle_time_sync(const time_sync_payload_t &t) {
  bool osf;
  int64_t rtc_before = 0;
  hal.rtc_healthy(&rtc_before, &osf);
  if (hal.rtc_write_unix((int64_t)t.unix_time)) {
    cfg.rtc_drift_ms =
        rtc_before ? (int32_t)((rtc_before - (int64_t)t.unix_time) * 1000) : 0;
    tv.on_sync((int64_t)t.unix_time, millis());
    Serial.printf("RX time_sync unix=%u drift_ms=%d\n", (unsigned)t.unix_time,
                  cfg.rtc_drift_ms);
    persist_now();
  } else {
    Serial.println(F("RX time_sync: rtc write FAILED"));
  }
}

static void handle_schedule_set(const schedule_hdr_t &hdr, const sched_entry_t *entries) {
  if (hdr.entry_count > SCHEDULE_STORE_MAX) {
    Serial.println(F("RX schedule: too many entries, refused"));
    eng.send_schedule_ack(hdr.version, 0, millis());
    return;
  }
  cfg.sched_version = hdr.version;
  cfg.sched_count = hdr.entry_count;
  cfg.skip_until = hdr.skip_until;
  cfg.hold_until = hdr.hold_until;
  memset(cfg.sched_entries, 0, sizeof(cfg.sched_entries));
  memcpy(cfg.sched_entries, entries, (size_t)hdr.entry_count * sizeof(sched_entry_t));
  int64_t u = unix_now_guess();
  cfg.sched_pushed_unix = u > 0 ? (uint32_t)u : 0;
  Serial.printf("RX schedule v%u entries=%u skip=%u hold=%u\n", hdr.version, hdr.entry_count,
                hdr.skip_until, hdr.hold_until);
  persist_now();
  eng.send_schedule_ack(hdr.version, 1, millis());
}

static void handle_config(const node_profile_t &p, uint8_t limits_confirmed) {
  if (!profile_valid(&p)) {
    Serial.println(F("RX config: bad profile CRC, refused"));
    eng.send_config_ack(p.crc16, 0, millis());
    return;
  }
  cfg.profile = p;
  act.configure(p);
  limits.configure(p.max_units_per_hour, p.max_units_per_day, p.cooldown_ms);
  limits.limits_confirmed = limits_confirmed != 0;
  cfg.limits_confirmed = limits.limits_confirmed ? 1 : 0;
  Serial.printf("RX config: profile applied (confirmed=%d)\n", limits.limits_confirmed);
  persist_now();
  eng.send_config_ack(p.crc16, 1, millis());
}

/* --- OTA pairing, node side (§6.3, D29) ---------------------------------------- */

/* Newest PAIR_ACCEPT stashed for the user to confirm ('pairok' on serial).
 * The hub committed its side already; if the user declines, the hub shows
 * the node offline and it can be removed there (§6.3.5 residual). */
static pair_accept_payload_t g_pair_acc;
static uint8_t g_pair_hub_mac[6];
static bool g_pair_acc_valid = false;

static void handle_pair_accept(const uint8_t mac[6], const pair_accept_payload_t &pa,
                               int8_t rssi) {
  (void)rssi;
  if (cfg.paired) return; /* already paired — ignore stray accepts */
  /* Derive the same 4 digits the hub showed; a mismatch means the request
   * this accept answers used a different nonce (stale accept) — ignore. */
  uint8_t expect[4];
  pairing_code(g_own_mac, cfg.pair_nonce, pa.hub_nonce, expect);
  if (memcmp(expect, pa.code_digits, 4) != 0) {
    Serial.println(F("RX pair: code mismatch (stale accept, ignored)"));
    return;
  }
  g_pair_acc = pa;
  memcpy(g_pair_hub_mac, mac, 6);
  g_pair_acc_valid = true;
  Serial.printf("PAIR code %c%c%c%c from %02X:%02X:%02X:%02X:%02X:%02X — matches the"
                " hub screen? 'pairok' to accept, ignore to decline\n",
                pa.code_digits[0], pa.code_digits[1], pa.code_digits[2],
                pa.code_digits[3], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static aio::LinkHandlers make_handlers() {
  aio::LinkHandlers h{};
  h.on_feed = handle_feed;
  h.on_time_sync = handle_time_sync;
  h.on_schedule_set = handle_schedule_set;
  h.on_config = handle_config;
  h.on_pair_accept = handle_pair_accept;
  h.on_status_req = []() {
    /* HB ack from the hub: reply at once, flagged so the hub does not
     * re-ack it (keeps the freshness window alive without a ping-pong). */
    heartbeat_payload_t hb{};
    hb_fill(&hb);
    eng.send_heartbeat(hb, millis(), 1);
  };
  h.on_abort = [](const abort_payload_t &a) {
    Serial.printf("RX abort feed_id=%u\n", a.feed_id);
    if (act.abort(millis())) Serial.println(F("aborted (radio)"));
  };
  h.hb_fill = hb_fill;
  h.on_channel_locked = [](uint8_t ch) {
    if (cfg.chan != ch) {
      cfg.chan = ch;
      Serial.printf("channel locked: %u (persisted)\n", ch);
      persist_now();
    }
  };
  h.on_link_up = []() { Serial.println(F("link up")); };
  h.on_link_lost = []() { Serial.println(F("link lost → channel scan")); };
  return h;
}

/* --- command application ------------------------------------------------------ */

static aio::ActuatorCmd cmds[8];

static void apply_commands() {
  uint32_t n = act.drain(cmds, 8);
  bool saw_intent = false;
  bool intent_ok = true;
  for (uint32_t i = 0; i < n; i++) {
    const aio::ActuatorCmd &c = cmds[i];
    switch (c.kind) {
      case aio::ActuatorCmd::PersistIntent:
        /* SR6: the journal write MUST land before the relay. */
        saw_intent = true;
        intent_ok = false;
        if (journal.dirty || persist_pending) {
          persist_now();
          intent_ok = true;
        }
        break;
      case aio::ActuatorCmd::RelayOn:
        hal.relay(true);
        hal.led(true);
        break;
      case aio::ActuatorCmd::RelayOff:
        hal.relay(false);
        hal.led(false);
        break;
      case aio::ActuatorCmd::ArmHardTimer:
        hal.arm_relay_off(c.ms);
        break;
      case aio::ActuatorCmd::CancelHardTimer:
        hal.cancel_relay_off();
        break;
      case aio::ActuatorCmd::PersistResult:
        persist_now();
        break;
      case aio::ActuatorCmd::ReportResult:
        report_result(c);
        break;
    }
  }
  if (saw_intent) act.on_intent_saved(intent_ok, millis());
}

/* --- setup / loop --------------------------------------------------------------- */

void setup() {
  hal.init(); /* SR1: relay pin driven OFF before anything else */
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println(AIO_FIRMWARE_NAME " boot (M2 link)");

  if (hal.storage_read(region, kStorageRegion) && config_load_slot(region, &cfg)) {
    Serial.printf("config loaded (seq=%u)\n", cfg.write_seq);
  } else {
    cfg = NodeConfig{};
    cfg.magic = CONFIG_MAGIC;
    cfg.write_seq = 1;
    cfg.node_id = 1;
    cfg.booted = 1;
    cfg.profile = profile_default();
    cfg.feed_id_seq = 1000;
    Serial.println("first boot: default config written");
    persist_now();
  }
  if (!profile_valid(&cfg.profile)) {
    cfg.profile = profile_default();
    Serial.println("profile CRC bad → defaults restored");
    persist_now();
  }
  if (cfg.sched_count > SCHEDULE_STORE_MAX) cfg.sched_count = 0;

  /* SR6: anything still IN_PROGRESS was attempted before the reset */
  memcpy((void *)journal.entries(), cfg.journal, sizeof(cfg.journal));
  uint32_t attempted = journal.resolve_in_progress();
  if (attempted) {
    Serial.printf("boot: %u in-progress feed(s) marked attempted (never repeated)\n",
                  attempted);
    persist_now();
  }

  limits.configure(cfg.profile.max_units_per_hour, cfg.profile.max_units_per_day,
                   cfg.profile.cooldown_ms);
  limits.limits_confirmed = cfg.limits_confirmed != 0; /* SR8 */
  limits.load(cfg.counters, 0);
  tv.restore_sync(cfg.sync_unix);

  act.configure(cfg.profile);
  act.attach(&limits, &journal);

  /* ESP-NOW + link engine */
  eng.begin(aio::LinkEngine::Role::Node, &nowt, make_handlers(), millis());
  eng.set_self(cfg.node_id, cfg.paired ? 1 : 0);
  if (cfg.paired && cfg.hub_key_len) {
    eng.add_peer(cfg.hub_mac, aio::LINK_HUB_ID, cfg.hub_key, cfg.hub_key_len);
    eng.restore_tx_seq((uint16_t)(cfg.tx_seq + 32)); /* replay-window skip */
    if (!nowt.begin(cfg.chan ? cfg.chan : 1)) Serial.println(F("ESP-NOW init FAILED"));
    else Serial.printf("ESP-NOW up: chan=%u hub=%02X:%02X:%02X:%02X:%02X:%02X\n",
                       cfg.chan ? cfg.chan : 1, cfg.hub_mac[0], cfg.hub_mac[1],
                       cfg.hub_mac[2], cfg.hub_mac[3], cfg.hub_mac[4], cfg.hub_mac[5]);
  } else {
    now_sta_mac(g_own_mac);
    /* §6.3: one stable nonce per pairing attempt — generate once, persist,
     * repeat inside every PAIR_REQ beacon (the hub's code derives from it). */
    static const uint8_t kZero[8] = {0};
    if (!memcmp(cfg.pair_nonce, kZero, 8)) {
      for (int i = 0; i < 2; i++) {
        const uint32_t r = hal.random32();
        memcpy(cfg.pair_nonce + 4 * i, &r, 4);
      }
      persist_now();
    }
    eng.set_pair_nonce(cfg.pair_nonce);
    if (!nowt.begin(cfg.chan ? cfg.chan : 1)) Serial.println(F("ESP-NOW init FAILED"));
    else
      Serial.printf("ESP-NOW up: unpaired, beaconing HELLO+PAIR_REQ every 5 s"
                    " (mac %02X:%02X:%02X:%02X:%02X:%02X)\n",
                    g_own_mac[0], g_own_mac[1], g_own_mac[2], g_own_mac[3], g_own_mac[4],
                    g_own_mac[5]);
  }

  cli_ctx = CliCtx{};
  cli_ctx.hal = &hal;
  cli_ctx.act = &act;
  cli_ctx.limits = &limits;
  cli_ctx.journal = &journal;
  cli_ctx.tv = &tv;
  cli_ctx.cfg = &cfg;
  cli_ctx.eng = &eng;
  cli_ctx.nowt = &nowt;
  cli_ctx.bench_mode = &bench_mode;
  cli_ctx.time_ok = &time_ok;
  cli_ctx.pair_acc = &g_pair_acc;
  cli_ctx.pair_hub_mac = g_pair_hub_mac;
  cli_ctx.pair_acc_valid = &g_pair_acc_valid;
  cli_ctx.persist_fn = persist_now;
  cli_ctx.erase_fn = erase_config;

  boot_ms = millis();
  refresh_time(0);
  Serial.printf("ready: time_ok=%d limits_confirmed=%d fault=%d\n", time_ok,
                limits.limits_confirmed, act.in_fault());
  cli_help();
}

void loop() {
  uint32_t now = millis();
  hal.feed_wdt();

  if (hal.relay_off_fired()) {
    Serial.println("HARD TIMER fired: loop stalled, relay forced OFF, fault latched");
    act.on_hard_timer_fired(now);
  }

  cli_poll(cli_ctx); /* config mutations persist via persist_fn/erase_fn */

  nowt.pump(eng, now); /* ESP-NOW rx → engine (journal dedups feeds, SR6) */

  act.tick(now);
  apply_commands();

  eng.tick(now); /* retries, RESULT repeats, heartbeat, channel scan */

  refresh_time(now);

  if (now - last_vin_poll >= 5000 || last_vin_poll == 0) {
    last_vin_poll = now;
    vin_mv = hal.vin_mv();
    vin_present = vin_mv >= VIN_ABSENT_MV;
    if ((cfg.profile.flags & PROFILE_FLAG_SUPPLY_MONITOR) && vin_present &&
        vin_mv < VIN_MIN_MV) {
      Serial.printf("WARN supply low: %u mV\n", vin_mv);
    }
  }
  cli_ctx.vin_mv = vin_mv;
  cli_ctx.vin_present = vin_present;
  *cli_ctx.time_ok = time_ok;

  persist_maybe(); /* also lands cfg.tx_seq for the replay window */
}
