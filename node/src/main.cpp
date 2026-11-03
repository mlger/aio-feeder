/*
 * AIO Feeder — node firmware, M7 resilience (PLAN.md §7.3, §9.6, §12 M7).
 *
 * Cooperative loop: CLI → ESP-NOW drain → actuator tick → link tick
 * → command drain (HAL side effects) → persistence → periodic sampling
 * → fallback-schedule poll (§9.6, once per local minute).
 * One actuation at a time; the relay-off hardware timer is the SR2
 * guarantee, the loop is only the fast path.
 *
 * Radio: the FSM's ReportResult commands become FEED_RESULT frames over the
 * same seam the CLI prints on; FEED frames enter the same request_feed() the
 * CLI uses, so a radio feed, a bench feed and a fallback-schedule feed are
 * indistinguishable to the safety core (SR2–SR9 all apply; bench modes are
 * RAM-only, docs/bench-m2.md).
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
#include "../../node_core/local_sched.h"
#include "../../node_core/node_limits.h"
#include "../../node_core/schedule.h"
#include "../../node_core/time_validity.h"
#include "cli.h"
#include "ota_run.h"

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

/* M7: shared clock estimate (refresh_time maintains it; loop reads it). */
static int64_t g_unix_now = 0;
/* M7 (FMEA #23): RTC sanity — reject reads that jump backwards or far from
 * millis() tracking; the projection stands in until the next good read. */
static int64_t rtc_ref_unix = 0;
static uint32_t rtc_ref_ms = 0;
static uint16_t rtc_rejects = 0;
/* M7 (§9.6): fallback-schedule minute tick. */
static int64_t last_local_min = -1;
static aio::LocalSchedState last_sched_state = aio::LocalSchedState::Idle;
/* M7 (SR9/FMEA #17): the physical stop switch owns the latch once it has
 * been seen active — bench rigs without the switch keep CLI control. */
static bool stop_switch_seen = false;
/* M7 (FMEA #8): hopper-low edge, serial warn once per low period. */
static bool hopper_warned = false;

/* M8b build identity (platformio.ini): what the hub sees in HELLO and what
 * proves a new image actually booted (D40 lock semantics). */
#ifndef AIO_NODE_FW_VER
#define AIO_NODE_FW_VER 0
#endif
static void fw_ver_str(uint32_t v, char *out, size_t cap) {
  if (!v) snprintf(out, cap, "?");
  else snprintf(out, cap, "%u.%u.%u", (unsigned)((v >> 16) & 0xFF),
                (unsigned)((v >> 8) & 0xFF), (unsigned)(v & 0xFF));
}

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
    /* FMEA #23: a garbage I²C read must never move the clock. Reject reads
     * that jump backwards (>2 s) or forward (>60 s) from the millis()
     * projection; keep the projection for this poll. */
    int64_t use = unix_now;
    if (rtc_ref_unix != 0) {
      const int64_t expect =
          rtc_ref_unix + (int64_t)((now_ms - rtc_ref_ms) / 1000);
      if (unix_now < expect - 2 || unix_now > expect + 60) {
        rtc_rejects++;
        Serial.printf("WARN rtc read rejected: %lld vs expected %lld (%u total)\n",
                      (long long)unix_now, (long long)expect, rtc_rejects);
        use = expect;
      }
    }
    rtc_ref_unix = use;
    rtc_ref_ms = now_ms;
    tv.on_rtc_status(true);
    limits.roll(use);
    time_ok = tv.valid_rtc(use);
    g_unix_now = use;
  } else {
    tv.on_rtc_status(false);
    time_ok = tv.valid_ms(now_ms); /* no RTC: 24 h window since boot sync */
    g_unix_now = time_ok ? tv.sync_unix() + (int64_t)((now_ms - tv.sync_ms()) / 1000) : 0;
  }
}

/* --- link handlers ------------------------------------------------------------- */

static int64_t unix_now_guess() {
  bool osf;
  int64_t u = 0;
  if (hal.rtc_healthy(&u, &osf)) return u;
  return time_ok ? tv.sync_unix() + (int64_t)((millis() - tv.sync_ms()) / 1000) : 0;
}

static void handle_feed(const uint8_t mac[6], const feed_payload_t &f);
static bool ota_actuation_allowed();

static void hb_fill(heartbeat_payload_t *hb) {
  hb->uptime_ms = millis() - boot_ms;
  hb->rssi = 0; /* not exposed by these SDK callbacks */
  hb->vin_mv = vin_mv;
  hb->hopper_low =
      (cfg.profile.flags & PROFILE_FLAG_HOPPER_SENSOR) ? (hal.hopper_low() ? 1 : 0) : 0;
  hb->last_feed_id = last_feed_id;
  hb->day_units = (uint16_t)limits.counters().day_units;
  uint8_t flags = 0;
  if (cfg.manual_stop) flags |= HB_FLAG_MANUAL_STOP;
  if (!time_ok) flags |= HB_FLAG_TIME_INVALID;
  if (!limits.limits_confirmed) flags |= HB_FLAG_LIMITS_UNCONFIRMED;
  if (cfg.ota_lock) flags |= HB_FLAG_OTA_LOCK;
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

  /* M8b FMEA #11: an ota-locked node answers, never actuates. */
  if (!ota_actuation_allowed()) {
    eng.send_feed_ack(f.feed_id, 0, FEED_REJ_OTA_LOCK, now);
    Serial.printf("RX feed id=%u -> REJ_OTA_LOCK\n", f.feed_id);
    return;
  }

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
  /* FMEA #2 detection: with a reed sensor fitted, report whether the gate
   * actually closed again — a gate still open after the feed suggests a
   * welded contact. 0xFF = no sensor (wire default, §6.5). */
  uint8_t gate_ok = 0xFF;
  if ((cfg.profile.flags & PROFILE_FLAG_GATE_SENSOR) &&
      cfg.profile.mech == MECH_SOLENOID_GATE)
    gate_ok = hal.gate_closed() ? 1 : 0;
  Serial.printf("RESULT id=%u status=%s actual_on_ms=%u pulses=%u gate=%u\n", c.feed_id,
                result_name(c.result), c.actual_on_ms, c.pulses, gate_ok);
  eng.send_feed_result(c.feed_id, c.result, c.actual_on_ms, c.pulses, gate_ok,
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
    cfg.tz_rule = t.tz_rule; /* §9.6: the fallback schedule runs in local time */
    rtc_ref_unix = 0;        /* RTC was rewritten: re-anchor the sanity window */
    g_unix_now = (int64_t)t.unix_time;
    Serial.printf("RX time_sync unix=%u tz=%u drift_ms=%d\n", (unsigned)t.unix_time,
                  t.tz_rule, cfg.rtc_drift_ms);
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

static void handle_unpair() {
  /* FMEA #15: the hub removed this node — stop obeying, re-beacon (§6.3). */
  if (bench_mode != BENCH_OFF) {
    Serial.println(F("RX unpair (bench: ignored)"));
    return;
  }
  if (!cfg.paired) return;
  cfg.paired = 0;
  memset(cfg.hub_mac, 0, 6);
  memset(cfg.hub_key, 0, sizeof(cfg.hub_key));
  cfg.hub_key_len = 0;
  cfg.sched_count = 0; /* the fallback copy dies with the pairing (§9.6) */
  cfg.sched_version = 0;
  cfg.sched_pushed_unix = 0;
  cfg.skip_until = 0;
  cfg.hold_until = 0;
  cfg.limits_confirmed = 0; /* hub re-pushes CONFIG on the next pairing */
  last_local_min = -1;
  last_sched_state = aio::LocalSchedState::NotTrusted;
  eng.clear_peers();
  eng.set_self(cfg.node_id, 0);
  Serial.println(F("RX unpair: pairing + fallback schedule cleared — beaconing"));
  persist_now();
}

/* --- M8b node OTA (§7.6, FMEA #11) ------------------------------------------------ */

/* SR2 parity: while the lock is set nothing may actuate — radio feeds,
 * fallback schedule and CLI all go through this gate. */
static bool ota_actuation_allowed() { return !cfg.ota_lock; }

static void handle_enter_ota(const enter_ota_payload_t &o) {
  uint8_t ok = 0, reason = OTA_ACK_REJ_BAD;
  char ssid[OTA_SSID_MAX + 1], pass[OTA_PASS_MAX + 1], url[OTA_URL_MAX + 1];

  do {
    if (bench_mode != BENCH_OFF) { reason = OTA_ACK_REJ_BENCH; break; }
    if (act.in_fault() || !act.idle()) { reason = OTA_ACK_REJ_BUSY; break; }
    if (!(o.flags & OTA_FLAG_WRAPPED) ||
        o.ssid_len > OTA_SSID_MAX || o.pass_len > OTA_PASS_MAX ||
        !o.url_len || o.url_len > OTA_URL_MAX ||
        (size_t)o.ssid_len + o.pass_len + o.url_len > sizeof(o.blob))
      break; /* bad payload — answered REJ_BAD */
    /* Unwrap with OUR hub key: a foreign hub cannot produce a valid MAC
     * (replay window + HMAC), so the unwrap key is trusted by construction. */
    uint8_t blob[sizeof(ssid) + sizeof(pass) + sizeof(url)];
    memcpy(blob, o.blob, o.ssid_len + o.pass_len + o.url_len);
    ota_wrap(cfg.hub_key, o.ks_seed, blob, o.ssid_len + o.pass_len + o.url_len);
    memcpy(ssid, blob, o.ssid_len); ssid[o.ssid_len] = 0;
    memcpy(pass, blob + o.ssid_len, o.pass_len); pass[o.pass_len] = 0;
    memcpy(url, blob + o.ssid_len + o.pass_len, o.url_len); url[o.url_len] = 0;

    /* FMEA #11: persist lock + identity BEFORE the radio leaves ESP-NOW —
     * a crash after this point still boots actuation-disabled. */
    cfg.ota_lock = 1;
    cfg.ota_from_ver = AIO_NODE_FW_VER;
    cfg.ota_stage = OTA_STAGE_ARMED;
    persist_now();
    hal.relay(false);
    hal.cancel_relay_off();
    ok = 1;
    reason = OTA_ACK_OK;
  } while (0);

  Serial.printf("RX enter_ota -> %s (%u)\n", ok ? "accepted" : "refused", reason);
  eng.send_ota_ack(ok, reason, millis());
  if (!ok) return;

  /* Leave ESP-NOW, pull, verify, flash. This blocks the loop for minutes —
  * the WDT is fed inside ota_run; nothing can actuate (lock + idle FSM). */
  nowt.end();
  const OtaRunResult r = ota_run(hal, ssid, pass, url,
                                 (o.flags & OTA_FLAG_SHA) ? o.sha256 : nullptr);
  if (r.ok) {
    cfg.ota_stage = OTA_STAGE_DONE;
    persist_now();
    Serial.println("OTA: rebooting into the new image");
    delay(200);
    hal.reboot();
  }
  /* Failure: keep the lock (D40), re-arm the link — the hub sees the node
   * back with the OLD fw version and can retry; serial 'otalock off' escapes. */
  Serial.println("OTA: failed — staying locked, re-arming ESP-NOW");
  nowt.begin(cfg.chan ? cfg.chan : 1);
}

/* Boot-time lock resolution (D40): the lock survives the OTA reboot; only a
 * DIFFERENT image with stage DONE may clear it. */
static void ota_boot_resolve() {
  if (!cfg.ota_lock) return;
  char from[16], now_[16];
  fw_ver_str(cfg.ota_from_ver, from, sizeof(from));
  fw_ver_str(AIO_NODE_FW_VER, now_, sizeof(now_));
  if (cfg.ota_stage == OTA_STAGE_DONE && cfg.ota_from_ver != AIO_NODE_FW_VER) {
    cfg.ota_lock = 0;
    cfg.ota_stage = OTA_STAGE_NONE;
    persist_now();
    Serial.printf("OTA: update %s -> %s confirmed, actuation lock cleared\n", from, now_);
  } else {
    Serial.printf("OTA: LOCK ACTIVE (from %s, running %s, stage %u) — actuation"
                  " disabled; 'otalock off' on serial to clear\n", from, now_,
                  cfg.ota_stage);
  }
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
  h.on_unpair = handle_unpair;
  h.on_enter_ota = handle_enter_ota;
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

/* --- fallback schedule (§9.6, M7) ---------------------------------------------- */

/* Once per local minute: run a due fallback occurrence through the same
 * request_feed() path a radio feed takes — caps, cooldown, journal and
 * FEED_RESULT reporting are identical (the hub reconciles on return). */
static void local_sched_poll_tick() {
  if (bench_mode != BENCH_OFF) return; /* bench modes are RAM-only, no real feeds */
  if (!ota_actuation_allowed()) return; /* M8b FMEA #11: locked during/after OTA */
  if (g_unix_now < 1600000000) {
    last_local_min = -1;
    return;
  }
  const int64_t minute = g_unix_now / 60;
  if (minute == last_local_min) return;
  last_local_min = minute;

  aio::LocalDue due{};
  const aio::LocalSchedState st =
      aio::local_sched_poll(cfg.sched_entries, cfg.sched_count, cfg.tz_rule,
                       cfg.sched_pushed_unix, cfg.skip_until, cfg.hold_until,
                       cfg.paired != 0, time_ok, g_unix_now, &due);
  if (st != aio::LocalSchedState::Run && st != last_sched_state) {
    switch (st) {
      case aio::LocalSchedState::NotFresh:
        Serial.println(F("sched: fallback copy stale (>7 d) — not running"));
        break;
      case aio::LocalSchedState::SkippedUser:
        Serial.println(F("sched: occurrence inside skip_until"));
        break;
      case aio::LocalSchedState::SkippedHold:
        Serial.println(F("sched: occurrence inside hold_until"));
        break;
      default:
        break; /* NotTrusted: silent (unpaired / no valid time) */
    }
  }
  last_sched_state = st;
  if (st != aio::LocalSchedState::Run) return;

  aio::FeedRequest req{};
  req.feed_id = due.feed_id; /* journal-stable occurrence id (SR6) */
  req.amount = due.amount;
  req.scheduled = true;
  req.time_ok = time_ok;
  req.manual_stop = cfg.manual_stop;
  req.vin_ok = !vin_present || vin_mv >= VIN_ABSENT_MV;
  bool acc = false;
  uint8_t res = 0;
  act.request_feed(req, millis(), &acc, &res);
  if (acc) {
    Serial.printf("LOCAL sched v%u entry%u: amount=%u -> feed id=%u accepted\n",
                  cfg.sched_version, due.entry, due.amount, due.feed_id);
  } else if (res == FEED_OK) {
    Serial.printf("LOCAL sched id=%u: already ran (journal dedup)\n", due.feed_id);
  } else {
    Serial.printf("LOCAL sched entry%u rejected: %s\n", due.entry, result_name(res));
  }
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

  /* M8b (D40): an OTA lock set before the last reboot gates everything below
   * until a different image confirms itself — or serial unlocks it. */
  ota_boot_resolve();

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
  hal.ota_mark_valid(); /* C3: setup completed — cancel bootloader rollback */
  char fw[16];
  fw_ver_str(AIO_NODE_FW_VER, fw, sizeof(fw));
  Serial.printf("ready: fw=%s time_ok=%d limits_confirmed=%d fault=%d ota_lock=%d\n",
                fw, time_ok, limits.limits_confirmed, act.in_fault(), cfg.ota_lock);
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
  local_sched_poll_tick(); /* §9.6: may submit a feed — drained below */
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

  /* SR9 (FMEA #17): physical stop switch — once seen fitted, it owns the
   * manual_stop latch (bench rigs without the switch keep CLI control). */
  if (hal.stop_active()) stop_switch_seen = true;
  if (stop_switch_seen) {
    const uint8_t want = hal.stop_active() ? 1 : 0;
    if (want != cfg.manual_stop) {
      cfg.manual_stop = want;
      Serial.printf("stop switch -> manual_stop=%d\n", want);
      persist_now();
    }
  }

  /* FMEA #8: hopper-low edge — the heartbeat carries the level; warn once. */
  if ((cfg.profile.flags & PROFILE_FLAG_HOPPER_SENSOR) && hal.hopper_low()) {
    if (!hopper_warned) {
      hopper_warned = true;
      Serial.println(F("WARN hopper low"));
    }
  } else {
    hopper_warned = false;
  }

  persist_maybe(); /* also lands cfg.tx_seq for the replay window */
}
