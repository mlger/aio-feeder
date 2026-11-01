/*
 * AIO Feeder — bench serial CLI (M1 deliverable + M2 link commands, PLAN.md §12).
 * Line-based, 115200 8N1. Feeds go through the same Actuator FSM the radio
 * uses, so bench behaviour equals field behaviour.
 *
 * M2 additions: pairing (serial-provisioned key, D29), channel control,
 * bench modes for the 1000-feed loopback, loss-test frames, schedule dump.
 */
#ifndef AIO_CLI_H
#define AIO_CLI_H

#include <Arduino.h>
#include "../../lib/proto/pairing.h"
#include "../../link_core/link.h"
#include "node_config.h"

/* RAM-only bench modes for the M2 loopback (never persisted):
 *   0 off    — field behaviour, all SR rules enforced
 *   1 radio  — FEED answered with RESULT over the air, relay never touched
 *   2 live   — real actuation, SR3/SR4/SR7/SR8 bypassed; SR2 + FAULT remain */
enum BenchMode : uint8_t { BENCH_OFF = 0, BENCH_RADIO = 1, BENCH_LIVE = 2 };

struct CliCtx {
  Hal *hal;
  aio::Actuator *act;
  aio::Limits *limits;
  aio::FeedJournal *journal;
  aio::TimeValidity *tv;
  NodeConfig *cfg;
  aio::LinkEngine *eng;
  aio::ILinkTransport *nowt;
  uint8_t *bench_mode;
  bool *time_ok;
  /* OTA pairing stash (§6.3): pending PAIR_ACCEPT from handle_pair_accept */
  pair_accept_payload_t *pair_acc;
  uint8_t *pair_hub_mac;
  bool *pair_acc_valid;
  uint16_t vin_mv;
  bool vin_present;
  void (*persist_fn)();
  void (*erase_fn)();
};

static const char *result_name(uint8_t r) {
  switch (r) {
    case FEED_OK: return "OK";
    case FEED_REJ_COOLDOWN: return "REJ_COOLDOWN";
    case FEED_REJ_LIMIT_HOUR: return "REJ_LIMIT_HOUR";
    case FEED_REJ_LIMIT_DAY: return "REJ_LIMIT_DAY";
    case FEED_REJ_TOO_LONG: return "REJ_TOO_LONG";
    case FEED_REJ_NO_TIME: return "REJ_NO_TIME";
    case FEED_REJ_FAULT: return "REJ_FAULT";
    case FEED_REJ_LOW_VOLT: return "REJ_LOW_VOLT";
    case FEED_REJ_LIMITS_UNCONFIRMED: return "REJ_LIMITS_UNCONFIRMED";
    case FEED_REJ_MANUAL_STOP: return "REJ_MANUAL_STOP";
    case FEED_ABORTED: return "ABORTED";
    case FEED_GATE_NOT_OPENED: return "GATE_NOT_OPENED";
    case FEED_GATE_STUCK_OPEN: return "GATE_STUCK_OPEN";
    default: return "?";
  }
}

static const char *mech_name(uint8_t m) {
  switch (m) {
    case MECH_SOLENOID_GATE: return "solenoid_gate";
    case MECH_MOTOR_ONEWAY: return "motor_oneway";
    case MECH_PUMP: return "pump";
    default: return "?";
  }
}

static const char *bench_name(uint8_t m) {
  switch (m) {
    case BENCH_OFF: return "off";
    case BENCH_RADIO: return "radio";
    case BENCH_LIVE: return "live";
    default: return "?";
  }
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* "AABBCCDDEEFF" or "AA:BB:CC:DD:EE:FF" → 6 bytes. */
static bool parse_mac(const char *s, uint8_t out[6]) {
  int nib = 0, byte = 0, val = 0;
  for (const char *p = s; *p && byte < 6; p++) {
    if (*p == ':' || *p == '-') continue;
    int v = hexval(*p);
    if (v < 0) return false;
    val = (val << 4) | v;
    if (++nib == 2) {
      out[byte++] = (uint8_t)val;
      nib = 0;
      val = 0;
    }
  }
  return byte == 6 && nib == 0;
}

static bool parse_key(const char *s, uint8_t out[32], uint8_t *len) {
  uint8_t buf[64];
  int nib = 0, byte = 0, val = 0;
  for (const char *p = s; *p; p++) {
    if (*p == ':' || *p == '-') continue;
    int v = hexval(*p);
    if (v < 0 || byte >= 64) return false;
    val = (val << 4) | v;
    if (++nib == 2) {
      buf[byte++] = (uint8_t)val;
      nib = 0;
      val = 0;
    }
  }
  if (nib != 0) return false;
  if (byte != 32 && byte != 16) return false; /* 128 or 256 bit keys */
  memset(out, 0, 32);
  memcpy(out, buf, byte);
  *len = (uint8_t)byte;
  return true;
}

static void print_mac(const uint8_t mac[6]) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                mac[5]);
}

static void print_key(const uint8_t key[32], uint8_t len) {
  for (int i = 0; i < len; i++) Serial.printf("%02X", key[i]);
}

static void cli_help() {
  Serial.println(F("AIO feeder node CLI (M2 bench)"));
  Serial.println(F("  status                 full status"));
  Serial.println(F("  feed <amount>          manual feed (profile units)"));
  Serial.println(F("  feeds <amount>         scheduled feed (needs time + limits)"));
  Serial.println(F("  abort                  abort in-flight feed"));
  Serial.println(F("  stop on|off            manual-stop latch (SR9)"));
  Serial.println(F("  fault clear            clear latched fault"));
  Serial.println(F("  time                   show RTC + trust state"));
  Serial.println(F("  timeset <unix>         write RTC + count as hub sync"));
  Serial.println(F("  cal <ms_per_unit_x100> set calibration in profile"));
  Serial.println(F("  caps <hour> <day>      set SR3 caps"));
  Serial.println(F("  conf on|off            limits confirmed (SR8)"));
  Serial.println(F("  relay <ms>             direct relay test under hard timer"));
  Serial.println(F("  journal                dump feed journal"));
  Serial.println(F("  vin                    sample supply monitor"));
  Serial.println(F("  erase                  factory-reset config slots"));
  Serial.println(F("  wdt                    stall to test watchdog reset"));
  Serial.println(F("  reset                  soft restart"));
  Serial.println(F("M2 link:"));
  Serial.println(F("  pair <hub_mac>         pair with hub (generates + prints key)"));
  Serial.println(F("  pairok                 accept pending PAIR_ACCEPT (unwrap + store key)"));
  Serial.println(F("  pairstatus             show pending PAIR_ACCEPT + its code"));
  Serial.println(F("  key <hex>              set hub key manually (32/16 bytes)"));
  Serial.println(F("  id <n>                 set node id (1..255)"));
  Serial.println(F("  chan <1-13>            set ESP-NOW channel"));
  Serial.println(F("  link                   link status + counters"));
  Serial.println(F("  scan                   force channel scan (§6.2)"));
  Serial.println(F("  hb                     send heartbeat now"));
  Serial.println(F("  sendtest <n>           send n loss-test frames"));
  Serial.println(F("  sched                  show pushed schedule + skip/hold"));
  Serial.println(F("  bench off|radio|live   bench modes (RAM only, see docs/bench-m2.md)"));
  Serial.println(F("  unlink                 forget hub pairing"));
}

static void cli_status(const CliCtx &c) {
  Serial.printf("node_id=%u state=%s fault=%s(%s) stop=%d\n", c.cfg->node_id,
                c.act->idle() ? "idle" : "busy", c.act->in_fault() ? "yes" : "no",
                c.act->fault_why() ? c.act->fault_why() : "-", c.cfg->manual_stop);
  Serial.printf("profile=%s mech=%s unit=%u cal_x100=%u max_on=%u cooldown=%u\n",
                c.cfg->profile.name, mech_name(c.cfg->profile.mech),
                c.cfg->profile.unit, c.cfg->profile.cal_ms_per_unit_x100,
                profile_max_on_ms(&c.cfg->profile), c.cfg->profile.cooldown_ms);
  Serial.printf("caps hour=%u/%u day=%u/%u confirmed=%d\n", c.limits->counters().hour_units,
                c.cfg->profile.max_units_per_hour, c.limits->counters().day_units,
                c.cfg->profile.max_units_per_day, c.limits->limits_confirmed);
  Serial.printf("last: status=%s actual_on_ms=%u\n",
                result_name(c.act->last_status()), c.act->last_actual_on_ms());
  Serial.printf("link paired=%d chan=%u bench=%s\n", c.cfg->paired, c.cfg->chan,
                bench_name(*c.bench_mode));
}

static void cli_journal(const NodeConfig &cfg) {
  for (uint32_t i = 0; i < AIO_JOURNAL_SIZE; i++) {
    const aio::JournalEntry &e = cfg.journal[i];
    if (e.feed_id == 0) continue;
    Serial.printf("  id=%u status=%s on_ms=%u %s\n", e.feed_id, result_name(e.status),
                  e.actual_on_ms, e.in_progress ? "IN_PROGRESS" : "");
  }
}

static void cli_link(const CliCtx &c) {
  const aio::LinkStats &s = c.eng->stats();
  Serial.printf("paired=%d chan=%u linked=%d scanning=%d peers=%u\n", c.cfg->paired,
                c.eng->channel(), c.eng->linked(millis()), c.eng->scanning(),
                c.eng->peer_count());
  if (c.cfg->paired) {
    Serial.print("hub_mac=");
    print_mac(c.cfg->hub_mac);
    Serial.println();
  }
  Serial.printf("tx=%u tx_fail=%u rx_ok=%u rx_dup=%u rx_replay=%u bad_mac=%u no_peer=%u\n",
                s.tx, s.tx_fail, s.rx_ok, s.rx_dup, s.rx_replay, s.rx_bad_mac, s.rx_no_peer);
  Serial.printf("feed_retries=%u feed_unknown=%u results_deduped=%u test_rx=%u\n",
                s.feed_retries, s.feed_unknown, s.results_deduped, s.test_rx);
}

static void cli_sched(const NodeConfig &cfg) {
  Serial.printf("version=%u count=%u pushed=%u freshness=%u\n", cfg.sched_version,
                cfg.sched_count, cfg.sched_pushed_unix,
                cfg.sched_pushed_unix
                    ? (uint32_t)(cfg.sched_pushed_unix + aio::SCHEDULE_FRESHNESS_S)
                    : 0);
  Serial.printf("skip_until=%u hold_until=%u\n", cfg.skip_until, cfg.hold_until);
  for (uint8_t i = 0; i < cfg.sched_count && i < SCHEDULE_STORE_MAX; i++) {
    const sched_entry_t &e = cfg.sched_entries[i];
    Serial.printf("  [%u] %02u:%02u days=0x%02X amount=%u\n", i, e.min_of_day / 60,
                  e.min_of_day % 60, e.days_mask, e.amount);
  }
}

/* Apply (or undo) bench-mode bypasses. Cooldown=0 copy: SR4 off in bench. */
static void bench_apply(CliCtx &c) {
  if (*c.bench_mode == BENCH_LIVE) {
    node_profile_t p = c.cfg->profile;
    p.cooldown_ms = 0;
    c.act->configure(p);
    c.limits->set_bench_bypass(true);
  } else {
    c.act->configure(c.cfg->profile);
    c.limits->set_bench_bypass(false);
  }
}

/* Config-mutating commands persist immediately via ctx.persist_fn(). */
static void cli_poll(CliCtx &c) {
  static char line[96];
  static uint8_t pos = 0;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch != '\n') {
      if (pos < sizeof(line) - 1) line[pos++] = ch;
      continue;
    }
    line[pos] = 0;
    pos = 0;
    if (!line[0]) continue;

    char *cmd = strtok(line, " ");
    char *a1 = strtok(nullptr, " ");
    char *a2 = strtok(nullptr, " ");
    char *a3 = strtok(nullptr, " ");
    uint32_t now = millis();

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
      cli_help();
    } else if (!strcmp(cmd, "status")) {
      cli_status(c);
    } else if (!strcmp(cmd, "feed") || !strcmp(cmd, "feeds")) {
      if (!a1) { Serial.println(F("usage: feed <amount>")); continue; }
      aio::FeedRequest req{};
      req.feed_id = ++c.cfg->feed_id_seq;
      req.amount = (uint32_t)atol(a1);
      req.scheduled = !strcmp(cmd, "feeds") && *c.bench_mode == BENCH_OFF;
      req.time_ok = *c.time_ok;
      req.manual_stop = c.cfg->manual_stop;
      req.vin_ok = !c.vin_present || c.vin_mv >= 2000; /* no reading → allow, warn */
      bool acc;
      uint8_t res;
      if (c.act->request_feed(req, now, &acc, &res)) {
        Serial.printf("feed %u id=%u -> %s\n", req.amount, req.feed_id,
                      acc ? "accepted" : result_name(res));
      } else {
        Serial.printf("feed %u -> rejected: %s\n", req.amount, result_name(res));
      }
      c.persist_fn(); /* feed_id_seq advanced */
    } else if (!strcmp(cmd, "abort")) {
      Serial.println(c.act->abort(now) ? F("aborted") : F("nothing in flight"));
    } else if (!strcmp(cmd, "stop")) {
      c.cfg->manual_stop = (a1 && !strcmp(a1, "on")) ? 1 : 0;
      Serial.printf("manual_stop=%d\n", c.cfg->manual_stop);
      c.persist_fn(); /* SR9 latch survives resets */
    } else if (!strcmp(cmd, "fault") && a1 && !strcmp(a1, "clear")) {
      c.act->clear_fault();
      Serial.println(F("fault cleared"));
    } else if (!strcmp(cmd, "time")) {
      bool osf;
      int64_t u = 0;
      bool ok = c.hal->rtc_healthy(&u, &osf);
      Serial.printf("rtc healthy=%d osf=%d unix=%lld time_ok=%d drift_ms=%d\n", ok, osf,
                    (long long)u, *c.time_ok, c.cfg->rtc_drift_ms);
    } else if (!strcmp(cmd, "timeset")) {
      if (!a1) { Serial.println(F("usage: timeset <unix>")); continue; }
      int64_t u = (int64_t)atoll(a1);
      if (c.hal->rtc_write_unix(u)) {
        c.tv->on_sync(u, millis());
        Serial.println(F("rtc written (counts as hub sync)"));
        c.persist_fn(); /* sync_unix persisted for the SR7 RTC path */
      } else {
        Serial.println(F("rtc write FAILED"));
      }
    } else if (!strcmp(cmd, "cal")) {
      if (!a1) { Serial.println(F("usage: cal <ms_per_unit_x100>")); continue; }
      c.cfg->profile.cal_ms_per_unit_x100 = (uint16_t)atol(a1);
      c.cfg->profile.crc16 = profile_crc16(&c.cfg->profile);
      bench_apply(c); /* re-apply (or clear) bench profile copy */
      Serial.println(F("calibration updated"));
      c.persist_fn();
    } else if (!strcmp(cmd, "caps")) {
      if (!a1 || !a2) { Serial.println(F("usage: caps <hour> <day>")); continue; }
      c.cfg->profile.max_units_per_hour = (uint32_t)atol(a1);
      c.cfg->profile.max_units_per_day = (uint32_t)atol(a2);
      c.cfg->profile.crc16 = profile_crc16(&c.cfg->profile);
      c.limits->configure(c.cfg->profile.max_units_per_hour,
                          c.cfg->profile.max_units_per_day, c.cfg->profile.cooldown_ms);
      Serial.println(F("caps updated"));
      c.persist_fn();
    } else if (!strcmp(cmd, "conf")) {
      c.limits->limits_confirmed = (a1 && !strcmp(a1, "on"));
      c.cfg->limits_confirmed = c.limits->limits_confirmed ? 1 : 0;
      Serial.printf("limits_confirmed=%d\n", c.limits->limits_confirmed);
      c.persist_fn(); /* SR8 flag survives resets */
    } else if (!strcmp(cmd, "relay")) {
      uint32_t ms = a1 ? (uint32_t)atol(a1) : 500;
      if (ms > profile_max_on_ms(&c.cfg->profile)) {
        Serial.println(F("refused: over max on-time (SR2)"));
        continue;
      }
      Serial.printf("relay %u ms (hard timer armed)\n", ms);
      c.hal->arm_relay_off(ms + 500);
      c.hal->relay(true);
      uint32_t t0 = millis();
      while (millis() - t0 < ms) { c.hal->feed_wdt(); delay(1); }
      c.hal->relay(false);
      c.hal->cancel_relay_off();
      Serial.println(c.hal->relay_off_fired() ? F("hard timer fired (loop too slow!)")
                                              : F("relay off (loop path)"));
    } else if (!strcmp(cmd, "journal")) {
      cli_journal(*c.cfg);
    } else if (!strcmp(cmd, "vin")) {
      Serial.printf("vin=%u mV present=%d\n", c.vin_mv, c.vin_present);
    } else if (!strcmp(cmd, "erase")) {
      c.erase_fn();
    } else if (!strcmp(cmd, "wdt")) {
      Serial.println(F("stalling — expect watchdog reset"));
      delay(100);
      c.hal->stall_for_wdt_test();
    } else if (!strcmp(cmd, "reset")) {
      Serial.println(F("rebooting"));
      delay(50);
      ESP.restart();
    /* ---- M2 link commands ---- */
    } else if (!strcmp(cmd, "pair")) {
      uint8_t mac[6];
      if (!a1 || !parse_mac(a1, mac)) { Serial.println(F("usage: pair <hub_mac>")); continue; }
      memcpy(c.cfg->hub_mac, mac, 6);
      for (int i = 0; i < 8; i++) { /* 256-bit key from the HW RNG */
        uint32_t r = c.hal->random32();
        memcpy(c.cfg->hub_key + 4 * i, &r, 4);
      }
      c.cfg->hub_key_len = 32;
      c.cfg->paired = 1;
      c.eng->clear_peers();
      c.eng->add_peer(c.cfg->hub_mac, aio::LINK_HUB_ID, c.cfg->hub_key, c.cfg->hub_key_len);
      c.eng->set_self(c.cfg->node_id, 1);
      Serial.print(F("paired with "));
      print_mac(c.cfg->hub_mac);
      Serial.println();
      Serial.print(F("key="));
      print_key(c.cfg->hub_key, c.cfg->hub_key_len);
      Serial.println();
      Serial.println(F("-> enter the same key on the hub: peer add <node_mac> <id> <key>"));
      c.persist_fn();
    } else if (!strcmp(cmd, "pairok")) {
      /* §6.3.5: unwrap the wrapped key under the confirmed code and store.
       * The code digits were already verified against our nonce in the RX
       * handler; pairok is the human confirmation that the hub screen shows
       * the same digits. */
      if (!*c.pair_acc_valid) {
        Serial.println(F("no pending PAIR_ACCEPT (open the hub pairing window first)"));
        continue;
      }
      uint8_t key[32];
      pairing_unwrap_key(c.pair_acc->wrapped_key, c.pair_acc->code_digits,
                         c.cfg->pair_nonce, c.pair_acc->hub_nonce, key);
      memcpy(c.cfg->hub_key, key, 32);
      c.cfg->hub_key_len = 32;
      memcpy(c.cfg->hub_mac, c.pair_hub_mac, 6);
      if (c.pair_acc->node_id) c.cfg->node_id = c.pair_acc->node_id;
      if (c.pair_acc->channel >= 1 && c.pair_acc->channel <= 13)
        c.cfg->chan = c.pair_acc->channel;
      c.cfg->paired = 1;
      c.eng->clear_peers();
      c.eng->add_peer(c.cfg->hub_mac, aio::LINK_HUB_ID, c.cfg->hub_key,
                      c.cfg->hub_key_len);
      c.eng->set_self(c.cfg->node_id, 1);
      if (c.nowt) c.nowt->set_channel(c.cfg->chan);
      c.eng->set_channel(c.cfg->chan);
      *c.pair_acc_valid = false;
      c.persist_fn();
      Serial.printf("paired: id=%u chan=%u key stored (wrapped, never on air in the clear)\n",
                    c.cfg->node_id, c.cfg->chan);
    } else if (!strcmp(cmd, "pairstatus")) {
      if (!*c.pair_acc_valid) {
        Serial.println(F("no pending PAIR_ACCEPT"));
      } else {
        Serial.printf("pending from %02X:%02X:%02X:%02X:%02X:%02X code=%c%c%c%c node_id=%u"
                      " chan=%u — 'pairok' to accept\n",
                      c.pair_hub_mac[0], c.pair_hub_mac[1], c.pair_hub_mac[2],
                      c.pair_hub_mac[3], c.pair_hub_mac[4], c.pair_hub_mac[5],
                      c.pair_acc->code_digits[0], c.pair_acc->code_digits[1],
                      c.pair_acc->code_digits[2], c.pair_acc->code_digits[3],
                      c.pair_acc->node_id, c.pair_acc->channel);
      }
    } else if (!strcmp(cmd, "key")) {
      if (!a1) { Serial.println(F("usage: key <hex32|hex16>")); continue; }
      if (!parse_key(a1, c.cfg->hub_key, &c.cfg->hub_key_len)) {
        Serial.println(F("bad key (need 32 or 16 bytes hex)"));
        continue;
      }
      if (c.cfg->paired && c.eng->hub_peer())
        c.eng->set_key(c.eng->hub_peer(), c.cfg->hub_key, c.cfg->hub_key_len);
      Serial.println(F("key set"));
      c.persist_fn();
    } else if (!strcmp(cmd, "id")) {
      if (!a1) { Serial.println(F("usage: id <1-255>")); continue; }
      uint32_t n = atol(a1);
      if (n == 0 || n > 255) { Serial.println(F("bad id")); continue; }
      c.cfg->node_id = (uint8_t)n;
      c.eng->set_self(c.cfg->node_id, c.cfg->paired);
      Serial.printf("node_id=%u\n", c.cfg->node_id);
      c.persist_fn();
    } else if (!strcmp(cmd, "chan")) {
      if (!a1) { Serial.println(F("usage: chan <1-13>")); continue; }
      uint32_t ch = atol(a1);
      if (ch < 1 || ch > 13) { Serial.println(F("bad channel")); continue; }
      c.cfg->chan = (uint8_t)ch;
      if (c.nowt) c.nowt->set_channel(c.cfg->chan);
      c.eng->set_channel(c.cfg->chan);
      Serial.printf("chan=%u\n", c.cfg->chan);
      c.persist_fn();
    } else if (!strcmp(cmd, "link")) {
      cli_link(c);
    } else if (!strcmp(cmd, "scan")) {
      c.eng->force_scan(now);
      Serial.println(c.eng->scanning() ? F("scanning 1..13") : F("scan refused (need peer)"));
    } else if (!strcmp(cmd, "hb")) {
      heartbeat_payload_t hb{};
      if (c.eng->send_heartbeat(hb, now)) /* empty payload: bench probe */
        Serial.println(F("hb sent (probe, empty payload)"));
      else
        Serial.println(F("hb failed (no paired hub)"));
    } else if (!strcmp(cmd, "sendtest")) {
      uint32_t n = a1 ? (uint32_t)atol(a1) : 100;
      uint32_t sent = 0;
      for (uint32_t i = 0; i < n; i++) {
        c.hal->feed_wdt();
        if (c.eng->send_test(i, millis())) sent++;
        if ((i & 0x0F) == 0x0F) delay(4); /* be gentle with the SDK send queue */
      }
      Serial.printf("sendtest: %u/%u sent\n", sent, n);
    } else if (!strcmp(cmd, "sched")) {
      cli_sched(*c.cfg);
    } else if (!strcmp(cmd, "bench")) {
      if (a1 && !strcmp(a1, "off")) *c.bench_mode = BENCH_OFF;
      else if (a1 && !strcmp(a1, "radio")) *c.bench_mode = BENCH_RADIO;
      else if (a1 && !strcmp(a1, "live")) *c.bench_mode = BENCH_LIVE;
      else { Serial.println(F("usage: bench off|radio|live")); continue; }
      bench_apply(c);
      Serial.printf("bench=%s (RAM only, cleared on reset)\n", bench_name(*c.bench_mode));
    } else if (!strcmp(cmd, "unlink")) {
      c.cfg->paired = 0;
      memset(c.cfg->hub_mac, 0, 6);
      memset(c.cfg->hub_key, 0, sizeof(c.cfg->hub_key));
      c.cfg->hub_key_len = 0;
      c.eng->clear_peers();
      c.eng->set_self(c.cfg->node_id, 0);
      Serial.println(F("unpaired (beaconing HELLO)"));
      c.persist_fn();
    } else {
      Serial.print(F("unknown: "));
      Serial.println(cmd);
      cli_help();
    }
  }
}

#endif /* AIO_CLI_H */
