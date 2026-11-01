/*
 * AIO Feeder — node firmware, M1 bench (PLAN.md §7.3, §12 M1).
 *
 * Cooperative loop: CLI → actuator tick → command drain (HAL side effects)
 * → persistence → periodic sampling. One actuation at a time; the relay-off
 * hardware timer is the SR2 guarantee, the loop is only the fast path.
 *
 * Radio/ESP-NOW arrives in M2; the FSM emits ReportResult commands that M2
 * will turn into FEED_RESULT frames over the same seam.
 */
#include <Arduino.h>

#include "../../lib/proto/proto.h"
#include "node_config.h"

#if defined(AIO_NODE_8266)
#include "hal_esp8266.h"
#elif defined(AIO_NODE_C3)
#include "hal_esp32c3.h"
#else
#error "Define AIO_NODE_8266 or AIO_NODE_C3 (see platformio.ini)"
#endif

#include "../../node_core/actuator.h"
#include "../../node_core/feed_journal.h"
#include "../../node_core/limits.h"
#include "../../node_core/time_validity.h"
#include "cli.h"

/* Supply monitor: reject below this rail voltage (12 V nominal) [VERIFY]. */
constexpr uint16_t VIN_MIN_MV = 10800;
/* Readings below this are treated as "no divider fitted" → allowed, warned. */
constexpr uint16_t VIN_ABSENT_MV = 2000;

static Hal hal;
static NodeConfig cfg;
static aio::Limits limits;
static aio::FeedJournal journal;
static aio::TimeValidity tv;
static aio::Actuator act;
static CliCtx cli_ctx;
static bool time_ok = false;
static uint16_t vin_mv = 0;
static bool vin_present = false;
static uint32_t last_persist_ms = 0;
static bool persist_pending = false;
static uint32_t last_rtc_poll = 0;
static uint32_t last_vin_poll = 0;
static uint8_t region[kStorageRegion];

/* --- persistence ------------------------------------------------------------ */

/* RAM → blob mirror, then commit one slot (alternating). */
static void persist_now() {
  memcpy(cfg.journal, (const void *)journal.entries(), sizeof(cfg.journal));
  cfg.counters = limits.counters();
  cfg.sync_unix = tv.sync_unix();
  cfg.limits_confirmed = limits.limits_confirmed;
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
        Serial.printf("RESULT id=%u status=%s actual_on_ms=%u pulses=%u\n", c.feed_id,
                      result_name(c.result), c.actual_on_ms, c.pulses);
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
  Serial.println(AIO_FIRMWARE_NAME " boot (M1 bench)");

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

  cli_ctx = CliCtx{};
  cli_ctx.hal = &hal;
  cli_ctx.act = &act;
  cli_ctx.limits = &limits;
  cli_ctx.journal = &journal;
  cli_ctx.tv = &tv;
  cli_ctx.cfg = &cfg;
  cli_ctx.time_ok = &time_ok;
  cli_ctx.persist_fn = persist_now;
  cli_ctx.erase_fn = erase_config;

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

  act.tick(now);
  apply_commands();

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

  persist_maybe();
}
