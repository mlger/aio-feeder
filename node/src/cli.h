/*
 * AIO Feeder — bench serial CLI (M1 deliverable: "serial CLI", PLAN.md §12).
 * Line-based, 115200 8N1. Feeds go through the same Actuator FSM the radio
 * will use in M2, so bench behaviour equals field behaviour.
 */
#ifndef AIO_CLI_H
#define AIO_CLI_H

#include <Arduino.h>
#include "node_config.h"

struct CliCtx {
  Hal *hal;
  aio::Actuator *act;
  aio::Limits *limits;
  aio::FeedJournal *journal;
  aio::TimeValidity *tv;
  NodeConfig *cfg;
  bool *time_ok;
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

static void cli_help() {
  Serial.println(F("AIO feeder node CLI (M1 bench)"));
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
}

static void cli_journal(const NodeConfig &cfg) {
  for (uint32_t i = 0; i < AIO_JOURNAL_SIZE; i++) {
    const aio::JournalEntry &e = cfg.journal[i];
    if (e.feed_id == 0) continue;
    Serial.printf("  id=%u status=%s on_ms=%u %s\n", e.feed_id, result_name(e.status),
                  e.actual_on_ms, e.in_progress ? "IN_PROGRESS" : "");
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
      req.scheduled = !strcmp(cmd, "feeds");
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
      Serial.printf("rtc healthy=%d osf=%d unix=%lld time_ok=%d\n", ok, osf,
                    (long long)u, *c.time_ok);
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
      c.act->configure(c.cfg->profile);
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
    } else {
      Serial.print(F("unknown: "));
      Serial.println(cmd);
      cli_help();
    }
  }
}

#endif /* AIO_CLI_H */
