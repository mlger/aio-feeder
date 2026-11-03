/*
 * AIO Feeder — hub firmware, M3 bench host on ESP-IDF (PLAN.md §12 M3,
 * decision D6: pure ESP-IDF + QEMU).
 *
 * Same M3 core as the M0–M2 Arduino bench build, now wired through IDF
 * primitives:
 *
 *   - schedule engine (hub_core.h): 1 Hz eval, fire-once occurrences, §9.4
 *     catch-up-once / skip on reboot, §9.9 skip/hold gates;
 *   - feed_manager: the only FEED originator (§7.2 FSM, §9.5 stagger);
 *   - persistence (nvs_store.h → hub_store.h on NVS): registry, keys,
 *     profiles, schedules, occurrence state — schedules survive reboot;
 *   - time: SNTP over Wi-Fi (CONFIG_AIO_WIFI_*) when configured, onboard
 *     PCF85063 RTC fallback (§7.4); TIME_SYNC pushed on drift > 2 s;
 *   - radio: native ESP-NOW (now_espnow.h), channel scan on the nodes.
 *
 * Bench CLI = an esp_console REPL on UART0 (menuconfig "AIO Feeder hub"
 * for the Wi-Fi credentials). M2/M3 commands, identical semantics:
 *
 *   info | chan <1-13> | peer add <mac> <id> <key> | peer del <id> | peers
 *   feed <id> <amount> | feedloop <id> <n>  (M2 loopback, bypasses feed_manager)
 *   sched <id> list|add <min> <days> <amt>|del <i>|clear|push
 *   time | tz <0-3> | stagger <n> | events [n] | sync <id> <unix>
 *   config <id> | statusreq <id> | abort <id> <feed_id> | stats | help
 *
 * Concurrency: the REPL task and the 1 ms loop task share the engine state;
 * every command body and every loop iteration run under g_state_mu.
 *
 * QEMU boot markers: "%s <version> (ESP-IDF)" then "core ready" — both
 * print before the radio starts (the Wi-Fi peripheral is not emulated).
 *
 * QEMU screenshot build (CONFIG_AIO_UI_SHOT, M4a): radio off + in-firmware
 * sim nodes (sim_nodes.h) + LVGL dashboard (ui/) rendering into a static
 * framebuffer; tools/qemu/capture.py dumps it per "AIO_SHOT" marker.
 */
#include <esp_app_desc.h>
#include <esp_console.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <time.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>

#include "backup.h"
#include "backup_store.h"
#include "hub_core.h"
#include "hub_registry.h"
#include "hub_store.h"
#include "hub_time.h"
#include "calib_session.h"
#include "pin_lock.h"
#include "remote.h"
#include "remote_net.h"
#include "settings_store.h"
#include "unpair_queue.h"
#include "wizard_session.h"
#include "now_espnow.h"
#include "nvs_store.h"
#include "pairing_session.h"
#include "pcf85063.h"
#include "pcf85063_idf.h"
#include "ui/ui.h"

#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT
#include "sim_nodes.h"  /* single-include header (defines the sim objects) */
#endif

#include "../../lib/proto/messages.h"
#include "../../lib/proto/profile.h"
#include "../../lib/proto/proto.h"
#include "../../link_core/link.h"

#ifndef AIO_FIRMWARE_NAME
#define AIO_FIRMWARE_NAME "aio-feeder-hub"
#endif

using namespace aio;    /* the bench firmware body speaks the core vocabulary */
using namespace aio::ui; /* SET_SEC_*, WIZ_*, views — §8/§9 names at hand */

constexpr uint8_t HUB_CHAN_DEFAULT = 1;
constexpr int64_t TIME_VALID_MIN = 1600000000; /* 2020-09 (SR7 range check) */

/* --- singletons ---------------------------------------------------------------- */

static NowTransportIdf nowt;
static aio::LinkEngine eng;
static aio::NvsHubStore nvs;
static aio::HubRegistry reg;
static aio::EventLog evlog;
static aio::ui::AlertsAck g_alerts_ack; /* §8.7 ack keys, RAM-only (resets on reboot) */
static aio::HubCore core;
static aio::PairingSession g_pair; /* §6.3 pairing window (M4b) */
static aio::CalibSession g_calib;  /* §9.3 calibration wizard (M4b) */
static aio::HubSettings g_set;     /* §8.8 settings blob (M4b) */
static aio::RemoteSettings g_rset; /* §6.7 remote settings blob (M6) */
static aio::UnpairQueue g_unpair;  /* FMEA #15: keyed UNPAIR pending per node */
static bool g_hopper_alerted[aio::HUB_MAX_NODES] = {}; /* FMEA #8 edge latch */
static aio::PinLock g_pin;         /* §8 PIN lock (M4b, RAM) */
static aio::WizardSession g_wiz;   /* §9.13 setup wizard (M4b) */
static aio::IBackupStore *g_sd = nullptr; /* SD card / virtual SD (M4b) */
static char g_sd_note[24] = "no card";
static char g_backup_last[32] = "";
static uint8_t g_backup_count = 0;
static uint8_t s_set_section = 0; /* active settings pane (driver + taps) */
static aio::Pcf85063 rtc;
static aio::PcfI2cIdf rtc_bus;
static bool rtc_ok = false;
static SemaphoreHandle_t g_state_mu = nullptr;

struct StateLock {
  StateLock() { xSemaphoreTake(g_state_mu, portMAX_DELAY); }
  ~StateLock() { xSemaphoreGive(g_state_mu); }
};

static uint32_t ms_now() { return (uint32_t)(esp_timer_get_time() / 1000); }

static int64_t hub_unix_now() { return (int64_t)time(nullptr); }

static bool time_valid() { return hub_unix_now() >= TIME_VALID_MIN; }

static const char *time_source_name() {
  if (!time_valid()) return "none";
  return "ntp-or-rtc"; /* UI (M4) distinguishes via staleness; CLI prints raw */
}

struct LinkAdapter : aio::IHubLink {
  bool send_feed(uint8_t node, const feed_payload_t &f, uint32_t now_ms) override {
    return eng.send_feed(node, f, now_ms);
  }
};
static LinkAdapter link_adapter;

/* feedloop state (M2 1000-feed loopback, §12 M2 exit criterion) — REPL-only,
 * compiled out of the QEMU shot build (~4.2 KB of static DRAM goes to the
 * LVGL pool there instead; the REPL is skipped so nothing can reach it) */
#if !(defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT)
static uint32_t loop_target = 0;
static uint32_t loop_sent = 0;
static uint32_t loop_ok = 0;
static uint32_t loop_dup_errors = 0;
static uint32_t loop_last_feed_id = 0;
static uint32_t loop_next_ms = 0;
static uint8_t loop_node = 0;
static uint32_t seen_results[1024];
#endif
static uint32_t last_full_hb_push_ms = 0;

/* --- time ---------------------------------------------------------------------- */

static void rtc_to_system() {
  aio::PcfTime t;
  bool os_flag = false;
  if (!rtc_ok || !rtc.read(t, os_flag) || os_flag) {
    printf("RTC %s — not trusted (os=%d)\n", rtc_ok ? "out of range" : "absent",
           (int)os_flag);
    return;
  }
  const int64_t days = aio::days_from_civil(t.year, t.mon, t.mday);
  const int64_t unix = days * 86400 + (int64_t)t.hour * 3600 + (int64_t)t.min * 60 + t.sec;
  if (unix < TIME_VALID_MIN) {
    printf("RTC value out of range — ignored\n");
    return;
  }
  const struct timeval tv = {(time_t)unix, 0};
  settimeofday(&tv, nullptr);
  printf("RTC → system time: %lld (os_flag=%d)\n", (long long)unix, (int)os_flag);
}

static void time_begin() {
  rtc_ok = rtc.begin(rtc_bus);
  rtc_to_system();
  /* SNTP is enabled by the radio bring-up when CONFIG_AIO_WIFI_SSID is set */
}

/* --- unpair queue (M7, FMEA #15) -------------------------------------------------- */

/* Retry the keyed UNPAIR for one queued node. Keeps/rebuilds the engine peer
 * (the node is still paired — heartbeats are keyed) so retries survive hub
 * reboots; the seq lives in the queue entry, persisted on every send. */
static void unpair_send_one(const uint8_t mac[6], uint32_t now) {
  aio::PendingUnpair *q = g_unpair.find(mac);
  if (!q) return;
  if (!eng.peer_by_mac(mac)) eng.add_peer(mac, q->id, q->key, q->key_len);
  if (eng.send_unpair(mac, q->key, q->key_len, q->id, q->tx_seq, now)) {
    q->tx_seq++;
    q->sends++;
    g_unpair.save(nvs);
  }
}

/* Park a node for unpair + best-effort send + history row. The registry node
 * is still present here; the caller removes it and KEEPS the engine peer
 * while the entry is queued (heartbeats drive the retry). */
static bool unpair_park(aio::HubNode *n) {
  aio::LinkPeer *p = eng.peer_by_mac(n->mac);
  if (!g_unpair.add(n->id, n->mac, n->key, n->key_len, n->channel,
                    p ? p->tx_seq : 0, hub_unix_now()))
    return false;
  unpair_send_one(n->mac, ms_now());
  evlog.push((uint32_t)hub_unix_now(), n->id, aio::EV_UNPAIR_PENDING, 0, 0, 0, 0);
  printf("unpair pending node=%u (UNPAIR retried on check-in)\n", n->id);
  return true;
}

/* --- engine callbacks ----------------------------------------------------------- */

static void cb_hello(const uint8_t mac[6], const hello_payload_t &pl, int8_t rssi) {
  printf("HELLO from %02X:%02X:%02X:%02X:%02X:%02X fw=%u state=%u rssi=%d%s\n", mac[0],
         mac[1], mac[2], mac[3], mac[4], mac[5], pl.fw_ver, pl.state, rssi,
         pl.state == 0 ? " (unpaired: peer add this mac)" : "");
  /* FMEA #15: an unpaired beacon from a queued node = the UNPAIR landed. */
  if (pl.state == 0 && g_unpair.drop(mac)) {
    g_unpair.save(nvs);
    eng.remove_peer(mac);
    printf("unpair confirmed (node re-beaconed unpaired)\n");
  } else if (pl.state == 1 && g_unpair.find(mac)) {
    unpair_send_one(mac, ms_now()); /* still paired → retry now */
  }
}

static void cb_feed_ack(uint8_t node_id, const feed_ack_payload_t &a) {
  printf("FEED_ACK node=%u id=%u accepted=%u reason=%u\n", node_id, (unsigned)a.feed_id,
         a.accepted, a.reason);
  core.on_feed_ack(node_id, a, (uint32_t)hub_unix_now());
}

static void cb_feed_result(uint8_t node_id, const feed_result_payload_t &r) {
#if !(defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT)
  /* duplicate detection for the 1000-feed loopback (§12 M2 exit criterion) */
  if (r.feed_id == loop_last_feed_id && loop_target) return; /* engine dedups; belt+braces */
  if (loop_target && r.feed_id > loop_last_feed_id) {
    if (seen_results[r.feed_id % 1024] == r.feed_id) loop_dup_errors++;
    seen_results[r.feed_id % 1024] = r.feed_id;
    loop_last_feed_id = r.feed_id;
    loop_ok++;
  }
#endif
  printf("FEED_RESULT node=%u id=%u status=%u on_ms=%u pulses=%u t=%u\n", node_id,
         (unsigned)r.feed_id, r.status, r.actual_on_ms, r.pulses, (unsigned)r.t_unix);
  if (r.feed_id & 0x80000000u) core.note_local_result(node_id); /* D37 guard */
  g_calib.on_result(r.feed_id, r.status, r.actual_on_ms); /* §9.3 wizard routing */
  core.on_feed_result(node_id, r, (uint32_t)hub_unix_now());
}

static void cb_heartbeat(uint8_t node_id, const heartbeat_payload_t &hb, int8_t rssi) {
  printf("HB node=%u up=%us vin=%u day=%u flags=0x%02X drift=%d rssi=%d\n", node_id,
         (unsigned)(hb.uptime_ms / 1000), hb.vin_mv, hb.day_units, hb.flags,
         (int)hb.rtc_drift_ms, rssi);
  /* Ack non-reply heartbeats with STATUS_REQ so the node's freshness window
   * (§6.2 scan trigger) stays fed on an otherwise quiet link (D30). */
  aio::LinkPeer *p = eng.peer_by_node(node_id);
  if (p && p->last_flags == 0) eng.send_status_req(node_id, ms_now());
  /* §7.4: TIME_SYNC at least per heartbeat when the node's time looks off. */
  const uint32_t now = ms_now();
  const bool stale_push = (uint32_t)(now - last_full_hb_push_ms) > 10u * 60u * 1000u;
  if (time_valid() && p &&
      ((hb.flags & HB_FLAG_TIME_INVALID) || abs(hb.rtc_drift_ms) > 2000 || stale_push)) {
    const time_sync_payload_t t{(uint32_t)hub_unix_now(), reg.data().tz_rule};
    eng.send_time_sync(node_id, t, now);
  }
  /* FMEA #15: the node is still paired — retry the UNPAIR on every beat. */
  if (p && g_unpair.find(p->mac)) unpair_send_one(p->mac, now);
  /* FMEA #8: hopper-low edge → alert row (notify rides the event observer). */
  if (node_id < aio::HUB_MAX_NODES) {
    if (hb.hopper_low && !g_hopper_alerted[node_id]) {
      g_hopper_alerted[node_id] = true;
      evlog.push((uint32_t)hub_unix_now(), node_id, aio::EV_HOPPER_LOW, 0, 0, 0, 0);
    } else if (!hb.hopper_low && g_hopper_alerted[node_id]) {
      g_hopper_alerted[node_id] = false;
    }
  }
}

static void cb_pair_req(const uint8_t mac[6], const pair_req_payload_t &pr, int8_t rssi) {
  /* A PAIR_REQ means the node is unpaired — a queued entry is resolved
   * (either the UNPAIR landed or the node factory-reset; either way done). */
  if (g_unpair.drop(mac)) {
    g_unpair.save(nvs);
    eng.remove_peer(mac);
    printf("PAIR_REQ from a queued node — unpair resolved\n");
  }
  const bool added = g_pair.on_pair_req(mac, pr, rssi, ms_now(), &reg);
  if (added) /* repeats refresh RSSI only — printing them floods the UART */
    printf("PAIR_REQ from %02X:%02X:%02X:%02X:%02X:%02X fw=%u rssi=%d (candidate)\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], pr.fw_ver, rssi);
}

/* Send the committed PAIR_ACCEPT (zero-key discovery frame, §6.4) and put
 * the new peer on the link. Used by the shot driver and, later, the real
 * settings UI + a serial command. */
static bool pairing_send_accept(const uint8_t mac[6], const pair_accept_payload_t &acc,
                                uint32_t now) {
  if (!eng.send_pair_accept(mac, acc, now)) return false;
  const aio::HubNode *n = reg.by_id(acc.node_id);
  if (n) eng.add_peer(n->mac, n->id, n->key, n->key_len);
  reg.save(nvs);
  return true;
}

static void cb_feed_unknown(uint32_t feed_id, uint8_t node_id) {
  printf("FEED UNKNOWN id=%u node=%u (alert: never re-issued, §6.6)\n", (unsigned)feed_id,
         node_id);
  core.on_feed_unknown(feed_id, node_id, (uint32_t)hub_unix_now());
}

static void cb_schedule_ack(uint8_t node_id, const ack_status_payload_t &a) {
  printf("SCHEDULE_ACK node=%u v=%u ok=%u\n", node_id, a.version, a.ok);
}
static void cb_config_ack(uint8_t node_id, const ack_status_payload_t &a) {
  printf("CONFIG_ACK node=%u crc=0x%04X ok=%u\n", node_id, a.version, a.ok);
}

static aio::LinkHandlers make_handlers() {
  aio::LinkHandlers h{};
  h.on_hello = cb_hello;
  h.on_feed_ack = cb_feed_ack;
  h.on_feed_result = cb_feed_result;
  h.on_heartbeat = cb_heartbeat;
  h.on_pair_req = cb_pair_req;
  h.on_feed_unknown = cb_feed_unknown;
  h.on_schedule_ack = cb_schedule_ack;
  h.on_config_ack = cb_config_ack;
  return h;
}

/* --- settings + backup (M4b) ------------------------------------------------------ */

static void settings_persist() {
  if (!settings_save(*static_cast<IHubStore *>(&nvs), g_set))
    printf("settings save FAILED\n");
  g_pin.configure(g_set);
}

static void sd_refresh_note() {
  if (!g_sd) return;
  snprintf(g_sd_note, sizeof(g_sd_note), "%s",
           g_sd->present() ? backup_store_kind() : "no card");
}

static void backup_list_note() {
  char names[4][BACKUP_NAME_LEN];
  const int n = g_sd ? g_sd->list(names, 4) : 0;
  g_backup_count = (uint8_t)(n < 0 ? 0 : n);
  g_backup_last[0] = '\0';
  if (n > 0) snprintf(g_backup_last, sizeof(g_backup_last), "%s", names[0]);
}

/* Runs a backup / CSV export with a transient heap buffer (the JSON for a
 * full registry does not fit the loop task's stack; statics are the QEMU
 * build's binding budget). Callers hold the state lock. */
static void backup_op(bool csv) {
  if (!g_sd) {
    printf("no backup store\n");
    return;
  }
  char *buf = static_cast<char *>(malloc(BACKUP_JSON_MAX));
  char name[BACKUP_NAME_LEN];
  size_t len = 0;
  bool ok;
  if (!buf) {
    printf("out of memory for the backup buffer\n");
    return;
  }
  if (csv)
    ok = csv_export_now(*g_sd, reg, evlog, hub_unix_now(), buf, BACKUP_JSON_MAX,
                        name, &len);
  else
    ok = backup_write_now(*g_sd, reg, g_set, hub_unix_now(), buf, BACKUP_JSON_MAX,
                          name, &len);
  if (ok)
    printf("%s %s (%zu B)\n", csv ? "CSV:" : "backup:", name, len);
  else
    printf("%s failed (card %s)\n", csv ? "csv" : "backup",
           g_sd->present() ? "write error" : "missing");
  free(buf);
  if (ok) backup_list_note();
}

static void backup_restore_op(const char *name) {
  if (!g_sd) return;
  char *buf = static_cast<char *>(malloc(BACKUP_JSON_MAX));
  if (!buf) {
    printf("out of memory for the restore buffer\n");
    return;
  }
  HubSettings set = g_set;
  const aio::RestoreResult r =
      backup_restore_now(*g_sd, name, reg, set, buf, BACKUP_JSON_MAX);
  free(buf);
  if (r != aio::RESTORE_OK) {
    printf("restore failed (%u) - nothing changed\n", (unsigned)r);
    return;
  }
  g_set = set; /* PIN + Wi-Fi password kept by the applier (§9.12) */
  settings_persist();
  reg.save(nvs);
  /* rebuild the link peer table from the restored registry */
  while (eng.peer_count()) {
    aio::LinkPeer *p = eng.peer_by_index(0);
    if (!p || !eng.remove_peer(p->mac)) break;
  }
  for (int i = 0; i < aio::HUB_MAX_NODES; i++) {
    const aio::HubNode *n = reg.node(i);
    if (!n || !n->id) continue;
    eng.add_peer(n->mac, n->id, n->key, n->key_len);
    eng.send_config(n->id, n->profile, 1, ms_now());
    eng.send_schedule_set(n->id, n->sched.version, n->sched.entries,
                          n->sched.entry_count, n->sched.skip_until,
                          n->sched.hold_until, ms_now());
  }
  printf("restored %s: nodes=%u tz=%u (CONFIG + SCHEDULE_SET re-pushed)\n", name,
         (unsigned)reg.count(), (unsigned)reg.data().tz_rule);
}

static void factory_reset() { /* caller holds the state lock */
  g_pair.close();
  g_calib.close();
  g_wiz.close();
  for (uint8_t id = 1; id <= aio::HUB_MAX_NODES; id++)
    nvs.del(aio::FeedManager::pend_key_for(id));
  reg.reset();
  reg.save(nvs);
  g_set = settings_defaults();
  settings_persist(); /* PIN gone with the blob (factory reset, §8.8) */
  g_rset = aio::remote_defaults();
  nvs.del(aio::RMT_STORE_KEY);
  printf("factory reset done - nodes=%u, settings back to defaults\n",
         (unsigned)reg.count());
}

/* --- parse helpers (unchanged from the bench CLI) -------------------------------- */

static void print_mac(const uint8_t mac[6]) {
  printf("%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool parse_mac(const char *s, uint8_t out[6]) {
  int nib = 0, byte = 0, val = 0;
  for (const char *p = s; *p && byte < 6; p++) {
    if (*p == ':' || *p == '-') continue;
    const int v = hexval(*p);
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
    const int v = hexval(*p);
    if (v < 0 || byte >= 64) return false;
    val = (val << 4) | v;
    if (++nib == 2) {
      buf[byte++] = (uint8_t)val;
      nib = 0;
      val = 0;
    }
  }
  if (nib != 0 || (byte != 32 && byte != 16)) return false;
  memset(out, 0, 32);
  memcpy(out, buf, byte);
  *len = (uint8_t)byte;
  return true;
}

static uint32_t parse_num(const char *s) {
  if (!s) return 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (uint32_t)strtoul(s + 2, nullptr, 16);
  return (uint32_t)strtoul(s, nullptr, 10);
}

/* --- CLI body (esp_console handlers; state under lock) --------------------------- */

static const char *ev_kind_name(uint8_t k) {
  static const char *names[aio::EV_KIND_COUNT] = {"FEED_OK", "FEED_FAIL", "FEED_REJ",
                                                  "FEED_UNKNOWN", "SKIP_USER",
                                                  "SKIP_HOLD", "SKIP_MISSED",
                                                  "CATCHUP", "HOPPER_LOW",
                                                  "UNPAIR_PEND"};
  return k < aio::EV_KIND_COUNT ? names[k] : "?";
}

static void hub_help() {
  printf("AIO feeder hub CLI (M3 bench core on ESP-IDF)\n");
  printf("  info                   mac, channel, counters\n");
  printf("  chan <1-13>            set ESP-NOW channel\n");
  printf("  peer add <mac> <id> <keyhex>   register + persist\n");
  printf("  peer del <id>\n");
  printf("  peers                  list peers + last heartbeat\n");
  printf("  feed <id> <amount>     manual feed (feed_manager FSM)\n");
  printf("  feedloop <id> <n>      M2 loopback, bypasses feed_manager\n");
  printf("  sched <id> list | add <min> <days> <amt> | del <i> | clear | push\n");
  printf("  time | tz <0-3>        clock state / timezone rule\n");
  printf("  stagger <n>            max simultaneous node feeds (§9.5)\n");
  printf("  events [n]             recent event log\n");
  printf("  sync <id> <unix>       TIME_SYNC\n");
  printf("  config <id>            push default profile\n");
  printf("  statusreq <id>         ask for a heartbeat now\n");
  printf("  abort <id> <feed_id>   ABORT\n");
  printf("  stats                  link counters\n");
  printf("  pair open|close|select|confirm|form|status   pairing (§6.3)\n");
  printf("  cal start <id> | secs <s> | run | weigh <g> | save | close\n");
  printf("  set show | brightness <n> | screenoff <s> | retention <d>\n");
  printf("      ntp <0|1> | wifi <ssid> [pass] | factoryreset RESET\n");
  printf("  pin status | set <cur> <new> | off <cur> | scope <0|1>\n");
  printf("  backup now | ls | restore <name> | csv\n");
  printf("  wiz start | tz <0-3> | ntp <0|1> | wifi <ssid> [pass]\n");
  printf("      node <id|new> | cap <total> <now> | sched <min> <days> <amt>\n");
  printf("      next | back | save\n");
}

static void sched_push(uint8_t node_id) {
  aio::HubNode *n = reg.by_id(node_id);
  if (!n) { printf("no such node\n"); return; }
  printf("%s\n", eng.send_schedule_set(node_id, ++reg.data().sched_version,
                                       n->sched.entries, n->sched.entry_count,
                                       n->sched.skip_until, n->sched.hold_until, ms_now())
                     ? "SCHEDULE_SET sent"
                     : "send failed");
}

/* --- M5b: touchscreen schedule editor ---------------------------------- *
 * The engine owns the editor draft: the 400 ms snapshot publishes would
 * otherwise clobber view-side edits (§8.3 drafts are session-like state).
 * Taps mutate the draft through the pure ui_model editors; Save/Delete
 * sync it back into the registry and push SCHEDULE_SET. */
static aio::ui::SchedEditor s_ed_draft;

static void ed_publish() { aio::ui::publish_sched_editor(s_ed_draft); }

static void ed_rebuild(uint8_t node_id) {
  s_ed_draft = aio::ui::build_sched_editor(reg, node_id, hub_unix_now());
  ed_publish();
}

/* Copy the draft rows into the node's schedule (version + save + push). */
static void ed_sync_registry(uint8_t node_id) {
  aio::HubNode *n = reg.by_id(node_id);
  if (!n || !s_ed_draft.found || s_ed_draft.node_id != node_id) return;
  uint8_t k = s_ed_draft.entry_count;
  if (k > aio::HUB_SCHED_MAX_ENTRIES) k = aio::HUB_SCHED_MAX_ENTRIES;
  for (uint8_t i = 0; i < k; i++) {
    n->sched.entries[i].min_of_day = s_ed_draft.entries[i].min_of_day;
    n->sched.entries[i].days_mask = s_ed_draft.entries[i].days_mask;
    n->sched.entries[i].amount = s_ed_draft.entries[i].amount;
  }
  n->sched.entry_count = k;
  n->sched.version = ++reg.data().sched_version;
  n->sched.pushed_at = (uint32_t)hub_unix_now();
  reg.save(nvs);
  sched_push(node_id);
}

static int cmd_info(int argc, char **argv) {
  (void)argc; (void)argv;
  StateLock lock;
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  printf("hub mac=");
  print_mac(mac);
  printf(" proto=v%d chan=%u peers=%u stagger=%u\n", PROTO_VER, eng.channel(),
         eng.peer_count(), reg.data().stagger_max);
  printf("inflight=%u sched_entries_total=%u\n", core.fm().inflight_count(), reg.count());
  return 0;
}

static int cmd_chan(int argc, char **argv) {
  if (argc != 2) { printf("usage: chan <1-13>\n"); return 1; }
  const uint32_t ch = parse_num(argv[1]);
  if (ch < 1 || ch > 13) { printf("bad channel\n"); return 1; }
  StateLock lock;
  eng.set_channel((uint8_t)ch);
  nowt.set_channel((uint8_t)ch);
  printf("chan=%u (HELLO_ACK advertises this)\n", (unsigned)ch);
  return 0;
}

static int cmd_peer(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "add")) {
    if (argc != 5) { printf("usage: peer add <mac> <id> <keyhex>\n"); return 1; }
    uint8_t mac[6], key[32], klen = 0;
    const uint32_t id = parse_num(argv[3]);
    if (!parse_mac(argv[2], mac) || !parse_key(argv[4], key, &klen) || id == 0 || id > 255) {
      printf("usage: peer add <mac> <id> <keyhex>\n");
      return 1;
    }
    StateLock lock;
    if (reg.by_id((uint8_t)id) || reg.by_mac(mac)) { printf("node already registered\n"); return 1; }
    aio::HubNode *n = reg.add(mac, eng.channel());
    if (!n) { printf("registry full\n"); return 1; }
    n->id = (uint8_t)id;
    if (reg.data().next_node_id <= n->id) reg.data().next_node_id = (uint8_t)(n->id + 1);
    memcpy(n->key, key, 32);
    n->key_len = klen;
    reg.save(nvs);
    printf("%s\n", eng.add_peer(mac, (uint8_t)id, key, klen) ? "peer added + persisted"
                                                              : "link peer table full");
    return 0;
  }
  if (argc >= 2 && !strcmp(argv[1], "del")) {
    if (argc != 3) { printf("usage: peer del <id>\n"); return 1; }
    const uint8_t id = (uint8_t)parse_num(argv[2]);
    StateLock lock;
    aio::HubNode *n = reg.by_id(id);
    if (!n) { printf("no such node\n"); return 1; }
    /* FMEA #15: park the keyed UNPAIR first; while queued, the engine peer
     * stays so heartbeats drive the retry until the node confirms. */
    const bool parked = unpair_park(n);
    if (!parked) eng.remove_peer(n->mac);
    reg.remove(id);
    nvs.del(aio::FeedManager::pend_key_for(id));
    reg.save(nvs);
    printf("%s\n", parked ? "peer removed (unpair pending until it checks in)"
                          : "peer removed (unpair queue full — node stays paired!)");
    return 0;
  }
  printf("usage: peer add <mac> <id> <keyhex> | peer del <id>\n");
  return 1;
}

static int cmd_peers(int argc, char **argv) {
  (void)argc; (void)argv;
  StateLock lock;
  const uint32_t now = ms_now();
  for (uint8_t i = 0; i < eng.peer_count(); i++) {
    aio::LinkPeer *p = eng.peer_by_index(i);
    printf("  id=%u mac=", p->node_id);
    print_mac(p->mac);
    printf(" key_len=%u", p->key_len);
    if (p->last_hb_ms)
      printf(" last_hb=%us ago vin=%u day=%u flags=0x%02X",
             (unsigned)((now - p->last_hb_ms) / 1000), p->last_hb.vin_mv,
             p->last_hb.day_units, p->last_hb.flags);
    printf("\n");
  }
  if (!eng.peer_count()) printf("  (none)\n");
  return 0;
}

static int cmd_feed(int argc, char **argv) {
  if (argc < 3 || argc > 4) {
    printf("usage: feed <node_id> <amount> [pin]\n");
    return 1;
  }
  StateLock lock;
  const uint32_t now = ms_now();
  /* §8: when the PIN scope covers manual feeds, the correct 4-digit code
   * (or an unexpired 60 s unlock) is required. Schedules never ask. */
  if (g_pin.blocks_manual_feed()) {
    if (argc != 4) {
      printf("PIN required: feed <id> <amount> <pin>\n");
      return 1;
    }
    if (!g_pin.unlock(argv[3], now)) {
      printf("wrong PIN\n");
      return 1;
    }
  }
  const int64_t unix_now = hub_unix_now();
  if (!time_valid()) printf("note: clock invalid — manual feed still allowed (SR7)\n");
  const bool ok = core.request_manual((uint8_t)parse_num(argv[1]), (uint16_t)parse_num(argv[2]),
                                      now, unix_now > 0 ? unix_now : 0);
  printf("%s\n", ok ? "feed submitted (feed_manager)" : "feed refused (no node / busy)");
  return 0;
}

static int cmd_feedloop(int argc, char **argv) {
#if !(defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT)
  if (argc != 3) { printf("usage: feedloop <node_id> <count>\n"); return 1; }
  StateLock lock;
  loop_node = (uint8_t)parse_num(argv[1]);
  loop_target = parse_num(argv[2]);
  loop_sent = 0;
  loop_ok = 0;
  loop_dup_errors = 0;
  loop_last_feed_id = 0;
  loop_next_ms = ms_now();
  memset(seen_results, 0, sizeof(seen_results));
  printf("feedloop start: node=%u n=%u (bench radio mode on the node!)\n", loop_node,
         (unsigned)loop_target);
#else
  (void)argc; (void)argv;
#endif
  return 0;
}

static int cmd_sched(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: sched <id> list|add <min> <days> <amt>|del <i>|clear|push\n");
    return 1;
  }
  StateLock lock;
  aio::HubNode *n = reg.by_id((uint8_t)parse_num(argv[1]));
  if (!n) { printf("no such node\n"); return 1; }
  const char *sub = argc >= 3 ? argv[2] : "list";
  if (!strcmp(sub, "list")) {
    printf("node %u sched: enabled=%u entries=%u skip=%lu hold=%lu\n", n->id,
           n->sched.enabled, n->sched.entry_count, (unsigned long)n->sched.skip_until,
           (unsigned long)n->sched.hold_until);
    for (uint8_t i = 0; i < n->sched.entry_count && i < aio::HUB_SCHED_MAX_ENTRIES; i++) {
      const sched_entry_t &e = n->sched.entries[i];
      printf("  [%u] %02u:%02u days=0x%02X amount=%u\n", i, e.min_of_day / 60,
             e.min_of_day % 60, e.days_mask, e.amount);
    }
    return 0;
  }
  if (!strcmp(sub, "add")) {
    if (argc != 6) { printf("usage: sched <id> add <min_of_day> <days> <amount>\n"); return 1; }
    if (n->sched.entry_count >= aio::HUB_SCHED_MAX_ENTRIES) { printf("sched full\n"); return 1; }
    const uint32_t min = parse_num(argv[3]), days = parse_num(argv[4]), amt = parse_num(argv[5]);
    if (min >= 1440 || !days || (days & 0x7F) != days || !amt) {
      printf("bad values (min<1440, days 0x01-0x7F, amt>0)\n");
      return 1;
    }
    n->sched.entries[n->sched.entry_count++] = {(uint16_t)min, (uint8_t)days, (uint16_t)amt};
    n->sched.version = ++reg.data().sched_version;
    n->sched.pushed_at = (uint32_t)hub_unix_now();
    reg.save(nvs);
    printf("entry added (use 'sched push' to send to the node)\n");
    return 0;
  }
  if (!strcmp(sub, "del")) {
    if (argc != 4) { printf("usage: sched <id> del <index>\n"); return 1; }
    const uint8_t i = (uint8_t)parse_num(argv[3]);
    if (i >= n->sched.entry_count) { printf("bad index\n"); return 1; }
    memmove(&n->sched.entries[i], &n->sched.entries[i + 1],
            (n->sched.entry_count - i - 1) * sizeof(sched_entry_t));
    n->sched.entry_count--;
    n->sched.version = ++reg.data().sched_version;
    reg.save(nvs);
    printf("entry removed\n");
    return 0;
  }
  if (!strcmp(sub, "clear")) {
    n->sched.entry_count = 0;
    n->sched.version = ++reg.data().sched_version;
    memset(n->run.last_done, 0, sizeof(n->run.last_done));
    reg.save(nvs);
    printf("schedule cleared\n");
    return 0;
  }
  if (!strcmp(sub, "push")) {
    sched_push((uint8_t)parse_num(argv[1]));
    return 0;
  }
  printf("unknown sched subcommand\n");
  return 1;
}

static int cmd_time(int argc, char **argv) {
  (void)argc; (void)argv;
  StateLock lock;
  const int64_t unix_now = hub_unix_now();
  printf("unix=%lld src=%s tz=%u valid=%d\n", (long long)unix_now, time_source_name(),
         reg.data().tz_rule, (int)time_valid());
  if (time_valid()) {
    const aio::LocalTime t = aio::local_from_unix(unix_now, reg.data().tz_rule);
    printf("local min_of_day=%u wday=%u day_start=%lld\n", t.min_of_day, t.wday,
           (long long)t.day_start);
  }
  return 0;
}

static int cmd_tz(int argc, char **argv) {
  if (argc != 2) { printf("usage: tz <0=UTC 1=Manila 2=CET 3=US-East>\n"); return 1; }
  const uint32_t r = parse_num(argv[1]);
  if (r >= aio::TZ_RULE_COUNT) { printf("bad rule\n"); return 1; }
  StateLock lock;
  reg.data().tz_rule = (uint8_t)r;
  reg.save(nvs);
  printf("tz rule=%u\n", (unsigned)r);
  return 0;
}

static int cmd_stagger(int argc, char **argv) {
  if (argc != 2) { printf("usage: stagger <1-8>\n"); return 1; }
  const uint32_t n = parse_num(argv[1]);
  if (!n || n > aio::HUB_MAX_NODES) { printf("bad n\n"); return 1; }
  StateLock lock;
  reg.data().stagger_max = (uint8_t)n;
  core.fm().set_stagger((uint8_t)n);
  reg.save(nvs);
  printf("stagger_max=%u\n", (unsigned)n);
  return 0;
}

static int cmd_events(int argc, char **argv) {
  StateLock lock;
  uint8_t show = argc >= 2 ? (uint8_t)parse_num(argv[1]) : 10;
  if (show > evlog.count()) show = evlog.count();
  printf("event log: %u entries (showing %u newest)\n", evlog.count(), show);
  for (uint16_t i = 0; i < show; i++) {
    const aio::HubEvent *e = evlog.at(evlog.count() - 1 - i);
    printf("  %lu node=%u %s result=%u id=%lu amt=%u aux=%d\n", (unsigned long)e->unix,
           e->node, ev_kind_name(e->kind), e->result, (unsigned long)e->feed_id, e->amount,
           e->aux);
  }
  return 0;
}

static int cmd_sync(int argc, char **argv) {
  if (argc != 3) { printf("usage: sync <node_id> <unix>\n"); return 1; }
  StateLock lock;
  const time_sync_payload_t t{parse_num(argv[2]), reg.data().tz_rule};
  printf("%s\n", eng.send_time_sync((uint8_t)parse_num(argv[1]), t, ms_now())
                     ? "TIME_SYNC sent"
                     : "send failed");
  return 0;
}

static int cmd_config(int argc, char **argv) {
  if (argc != 2) { printf("usage: config <node_id>\n"); return 1; }
  StateLock lock;
  aio::HubNode *n = reg.by_id((uint8_t)parse_num(argv[1]));
  const node_profile_t p = n ? n->profile : profile_default();
  printf("%s\n", eng.send_config((uint8_t)parse_num(argv[1]), p, 1, ms_now()) ? "CONFIG sent"
                                                                              : "send failed");
  return 0;
}

static int cmd_statusreq(int argc, char **argv) {
  if (argc != 2) { printf("usage: statusreq <node_id>\n"); return 1; }
  StateLock lock;
  printf("%s\n", eng.send_status_req((uint8_t)parse_num(argv[1]), ms_now())
                     ? "STATUS_REQ sent"
                     : "send failed");
  return 0;
}

static int cmd_abort(int argc, char **argv) {
  if (argc != 3) { printf("usage: abort <node_id> <feed_id>\n"); return 1; }
  StateLock lock;
  printf("%s\n", eng.send_abort((uint8_t)parse_num(argv[1]), parse_num(argv[2]), ms_now())
                     ? "ABORT sent"
                     : "send failed");
  return 0;
}

static int cmd_stats(int argc, char **argv) {
  (void)argc; (void)argv;
  StateLock lock;
  const aio::LinkStats &s = eng.stats();
  printf("tx=%u tx_fail=%u rx_ok=%u rx_dup=%u rx_replay=%u\n", (unsigned)s.tx,
         (unsigned)s.tx_fail, (unsigned)s.rx_ok, (unsigned)s.rx_dup, (unsigned)s.rx_replay);
  printf("bad_len=%u bad_ver=%u bad_mac=%u no_peer=%u test_rx=%u\n", (unsigned)s.rx_bad_len,
         (unsigned)s.rx_bad_ver, (unsigned)s.rx_bad_mac, (unsigned)s.rx_no_peer,
         (unsigned)s.test_rx);
  printf("feed_retries=%u feed_unknown=%u results_deduped=%u inflight=%u\n",
         (unsigned)s.feed_retries, (unsigned)s.feed_unknown, (unsigned)s.results_deduped,
         (unsigned)core.fm().inflight_count());
  printf("events: ok=%u rej=%u unknown=%u missed=%u\n", evlog.count_of(aio::EV_FEED_OK),
         evlog.count_of(aio::EV_FEED_REJ), evlog.count_of(aio::EV_FEED_UNKNOWN),
         evlog.count_of(aio::EV_SKIPPED_MISSED));
  return 0;
}

static int cmd_help(int argc, char **argv) {
  (void)argc; (void)argv;
  StateLock lock;
  hub_help();
  return 0;
}

/* pairing window from the bench console (real UI is the touchscreen, M4b):
 *   pair open              → 60 s discovery window
 *   pair close             → cancel
 *   pair select <idx>      → show the candidate's confirmation code
 *   pair confirm           → code matched on the node → form step
 *   pair form <name> <mech> <total> <now>  → commit + send PAIR_ACCEPT
 *   pair status            → current step / candidates
 */
static int cmd_pair(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: pair open|close|select|confirm|form|status ...\n");
    return 1;
  }
  StateLock lock;
  const uint32_t now = ms_now();
  const aio::ui::PairingView v = g_pair.view(now);
  if (!strcmp(argv[1], "open")) {
    printf("%s\n", g_pair.open(now) ? "pairing window open (60 s)" : "already open");
  } else if (!strcmp(argv[1], "close")) {
    g_pair.close();
    printf("closed\n");
  } else if (!strcmp(argv[1], "select") && argc == 3) {
    printf("%s\n", g_pair.select((uint8_t)parse_num(argv[2])) ? "code shown — compare with the node"
                                                               : "select failed");
  } else if (!strcmp(argv[1], "confirm")) {
    printf("%s\n", g_pair.confirm_code() ? "confirmed — set the form (pair form ...)"
                                          : "confirm failed");
  } else if (!strcmp(argv[1], "form") && argc == 6) {
    g_pair.set_form(argv[2], (uint8_t)parse_num(argv[3]), (uint32_t)parse_num(argv[4]),
                    (uint32_t)parse_num(argv[5]));
    pair_accept_payload_t acc{};
    uint8_t nid = 0;
    const aio::PairingSession::CommitResult r = g_pair.commit(reg, &acc, &nid);
    if (r != aio::PairingSession::PAIR_COMMIT_OK) {
      printf("commit failed (%u)\n", r);
      return 1;
    }
    const aio::HubNode *nn = reg.by_id(nid);
    printf("%s\n", nn && pairing_send_accept(nn->mac, acc, now)
                        ? "committed — PAIR_ACCEPT sent"
                        : "committed — accept send failed");
  } else if (!strcmp(argv[1], "status")) {
    static const char *STEP[] = {"idle", "discover", "code", "form", "done"};
    printf("step=%s cands=%u left=%us", STEP[v.step], v.cand_count, v.window_left_s);
    for (int i = 0; i < v.cand_count; i++) {
      printf("\n  [%u] %02X:%02X:%02X:%02X:%02X:%02X fw=%u rssi=%d", i, v.cand[i].mac[0],
             v.cand[i].mac[1], v.cand[i].mac[2], v.cand[i].mac[3], v.cand[i].mac[4],
             v.cand[i].mac[5], v.cand[i].fw_ver, v.cand[i].rssi);
    }
    if (v.step == aio::ui::PAIR_CODE || v.step == aio::ui::PAIR_FORM)
      printf("\n  code=%c%c%c%c", v.code_digits[0], v.code_digits[1], v.code_digits[2],
             v.code_digits[3]);
    if (v.step == aio::ui::PAIR_DONE) printf("\n  node_id=%u", v.new_node_id);
    printf("\n");
  } else {
    printf("usage: pair open|close|select|confirm|form|status ...\n");
    return 1;
  }
  return 0;
}

/* calibration wizard from the bench console (§8.4/§9.3; the touchscreen
 * wizard drives the same session):
 *   cal start <id>      open the wizard for a node
 *   cal secs <s>        test run length (≤ node max on-time, SR2/D27)
 *   cal run             send the commanded test FEED (bypasses feed_manager,
 *                       like feedloop — the node enforces its own caps)
 *   cal weigh <grams>   enter the weighed amount (decimal ok, min 5 g, §9.3.3)
 *   cal save            write the median into the profile + push CONFIG
 *   cal close | cal status
 */
static bool calib_run_send(uint32_t now) {
  aio::HubNode *n = reg.by_id(g_calib.node_id());
  if (!n || !g_calib.is_open()) return false;
  const feed_payload_t f{(uint32_t)(++reg.data().feed_id_seq),
                         (uint16_t)g_calib.test_grams(), n->profile.unit, 0};
  if (!eng.send_feed(g_calib.node_id(), f, now)) return false;
  g_calib.mark_sent(f.feed_id, now);
  return true;
}

static int cmd_cal(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: cal start <id>|secs <s>|run|weigh <g>|save|close|status\n");
    return 1;
  }
  StateLock lock;
  const uint32_t now = ms_now();
  if (!strcmp(argv[1], "start") && argc == 3) {
    aio::HubNode *n = reg.by_id((uint8_t)parse_num(argv[2]));
    if (!n) {
      printf("no such node\n");
      return 1;
    }
    printf("%s\n", g_calib.start(n->id, n->profile)
                        ? "calibration open — cal secs/run/weigh"
                        : "wizard already open");
  } else if (!strcmp(argv[1], "secs") && argc == 3) {
    printf("%s\n", g_calib.set_test_s(parse_num(argv[2])) ? "test length set"
                                                           : "secs rejected (≤ max on-time)");
  } else if (!strcmp(argv[1], "run") && argc == 2) {
    printf("%s\n", calib_run_send(now) ? "test FEED sent — watch the node"
                                        : "run failed");
  } else if (!strcmp(argv[1], "weigh") && argc == 3) {
    const double g = atof(argv[2]);
    if (g <= 0) {
      printf("usage: cal weigh <grams>\n");
      return 1;
    }
    printf("%s\n", g_calib.set_weigh_grams_x100((uint32_t)(g * 100.0 + 0.5))
                        ? "sample taken — cal run for the next one"
                        : "weigh rejected");
  } else if (!strcmp(argv[1], "save") && argc == 2) {
    aio::HubNode *n = reg.by_id(g_calib.node_id());
    if (!n || !g_calib.apply(&n->profile)) {
      printf("save failed (no samples?)\n");
      return 1;
    }
    reg.save(nvs);
    printf("saved %lu x100 ms per %s — CONFIG %s\n",
           (unsigned long)n->profile.cal_ms_per_unit_x100,
           n->profile.unit == UNIT_ML ? "ml" : "g",
           eng.send_config(n->id, n->profile, 1, now) ? "sent" : "send failed");
    g_calib.mark_saved();
  } else if (!strcmp(argv[1], "close") && argc == 2) {
    g_calib.close();
    printf("closed\n");
  } else if (!strcmp(argv[1], "status") && argc == 2) {
    const aio::ui::CalibView v = g_calib.view(now);
    static const char *STEP[] = {"idle", "runtime", "dispense", "weigh", "done"};
    printf("step=%s node=%u test=%lus runs=%u median=%lu\n", STEP[v.step], v.node_id,
           (unsigned long)v.test_s, v.sample_count, (unsigned long)v.median_x100);
    if (v.note[0]) printf("  note: %s\n", v.note);
  } else {
    printf("usage: cal start <id>|secs <s>|run|weigh <g>|save|close|status\n");
    return 1;
  }
  return 0;
}

/* settings (§8.8) from the bench console — the touchscreen panes edit the
 * same blob:
 *   set show | brightness <10-100> | screenoff <0|30|60|120|300>
 *   set retention <7|30|90> | ntp <0|1> | wifi <ssid> [pass]
 *   set factoryreset RESET   (typed word, §8.8 Danger zone)
 */
static int cmd_set(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: set show|brightness|screenoff|retention|ntp|wifi|mqtt|web|factoryreset ...\n");
    return 1;
  }
  StateLock lock;
  if (!strcmp(argv[1], "show")) {
    printf("brightness=%u screenoff=%us retention=%ud ntp=%u pin=%u scope=%u\n",
           (unsigned)g_set.brightness, (unsigned)g_set.screen_off_s,
           (unsigned)g_set.history_days, (unsigned)g_set.ntp_on,
           (unsigned)g_set.pin_enabled, (unsigned)g_set.pin_scope);
    printf("wifi ssid='%s' (pass kept on device) sd=%s backups=%u\n",
           g_set.wifi_ssid, g_sd_note, (unsigned)g_backup_count);
    printf("web=%u :%u auth=%u mqtt=%s disc=%u\n",
           (unsigned)g_rset.web_on, (unsigned)g_rset.web_port,
           (unsigned)g_rset.web_auth, g_rset.mqtt_on ? g_rset.mqtt_url : "off",
           (unsigned)g_rset.mqtt_disc);
    return 0;
  }
  if (!strcmp(argv[1], "brightness") && argc == 3) {
    g_set.brightness = set_clamp_brightness((int)parse_num(argv[2]));
    settings_persist();
    printf("brightness=%u\n", (unsigned)g_set.brightness);
    return 0;
  }
  if (!strcmp(argv[1], "screenoff") && argc == 3) {
    const uint32_t v = parse_num(argv[2]);
    if (!set_screen_off_valid((uint16_t)v)) {
      printf("bad timeout (0, 30, 60, 120, 300)\n");
      return 1;
    }
    g_set.screen_off_s = (uint16_t)v;
    settings_persist();
    printf("screenoff=%us\n", (unsigned)v);
    return 0;
  }
  if (!strcmp(argv[1], "retention") && argc == 3) {
    const uint32_t v = parse_num(argv[2]);
    if (!set_retention_valid((uint8_t)v)) {
      printf("bad retention (7, 30, 90)\n");
      return 1;
    }
    g_set.history_days = (uint8_t)v;
    settings_persist();
    printf("retention=%ud (pruned on the next sweep)\n", (unsigned)v);
    return 0;
  }
  if (!strcmp(argv[1], "ntp") && argc == 3) {
    g_set.ntp_on = parse_num(argv[2]) ? 1 : 0;
    settings_persist();
    printf("ntp=%u\n", (unsigned)g_set.ntp_on);
    return 0;
  }
  if (!strcmp(argv[1], "wifi") && argc >= 3) {
    if (strlen(argv[2]) >= sizeof(g_set.wifi_ssid)) {
      printf("ssid too long\n");
      return 1;
    }
    snprintf(g_set.wifi_ssid, sizeof(g_set.wifi_ssid), "%s", argv[2]);
    snprintf(g_set.wifi_pass, sizeof(g_set.wifi_pass), "%s",
             argc >= 4 ? argv[3] : "");
    settings_persist();
    printf("wifi ssid='%s' pass %s (takes effect on reboot)\n", g_set.wifi_ssid,
          g_set.wifi_pass[0] ? "set" : "cleared/open");
    return 0;
  }
  if (!strcmp(argv[1], "web") && argc >= 3) {
    /* set web <0|1> [port] [auth] — dashboard switch, §6.7 */
    const uint32_t port = argc >= 4 ? parse_num(argv[3]) : g_rset.web_port;
    const uint32_t auth = argc >= 5 ? parse_num(argv[4]) : g_rset.web_auth;
    if (!aio::remote_set_web(&g_rset, (uint8_t)parse_num(argv[2]),
                             (uint16_t)port, (uint8_t)auth)) {
      printf("bad web args (port 1-65535)\n");
      return 1;
    }
    aio::remote_save(nvs, g_rset);
    remote_net_request_apply(); /* restart from the loop task, no locks held */
    printf("web=%u port=%u auth=%u\n", (unsigned)g_rset.web_on,
           (unsigned)g_rset.web_port, (unsigned)g_rset.web_auth);
    return 0;
  }
  if (!strcmp(argv[1], "mqtt") && argc >= 3) {
    /* set mqtt <url> [user] [pass] — "" clears the broker, §6.7 */
    if (!aio::remote_set_mqtt(&g_rset, argv[2], argc >= 4 ? argv[3] : "",
                              argc >= 5 ? argv[4] : "")) {
      printf("url too long (max %u)\n", (unsigned)(sizeof(g_rset.mqtt_url) - 1));
      return 1;
    }
    g_rset.mqtt_on = argv[2][0] ? 1 : 0;
    aio::remote_save(nvs, g_rset);
    remote_net_request_apply(); /* restart from the loop task, no locks held */
    printf("mqtt %s user='%s' pass %s (applied now)\n",
           g_rset.mqtt_on ? g_rset.mqtt_url : "off", g_rset.mqtt_user,
           g_rset.mqtt_pass[0] ? "set" : "none");
    return 0;
  }
  if (!strcmp(argv[1], "factoryreset")) {
    if (argc != 3 || strcmp(argv[2], "RESET") != 0) {
      printf("type it: set factoryreset RESET\n");
      return 1;
    }
    factory_reset();
    return 0;
  }
  printf("usage: set show|brightness|screenoff|retention|ntp|wifi|mqtt|web|factoryreset ...\n");
  return 1;
}

/* remote fields for the Settings pane (SET_SEC_REMOTE body, ui.cpp) — build
 * the snapshot, then stamp the live remote state on top */
static void set_remote_view(aio::ui::SettingsView &sv) {
  sv.web_on = g_rset.web_on != 0;
  sv.web_port = g_rset.web_port;
  sv.mqtt_on = g_rset.mqtt_on != 0;
  snprintf(sv.mqtt_state, sizeof(sv.mqtt_state), "%s", remote_net_mqtt_state());
  snprintf(sv.remote_ip, sizeof(sv.remote_ip), "%s", remote_net_ip());
}

static int cmd_remote(int argc, char **argv) {
  (void)argv;
  StateLock lock;
  if (argc > 1) {
    printf("usage: remote  (status only — configure with 'set web' / 'set mqtt')\n");
    return 1;
  }
  printf("web %s :%u auth=%s ip=%s\n", remote_net_web_up() ? "up" : "off",
         (unsigned)g_rset.web_port, g_rset.web_auth ? "on" : "off",
         remote_net_ip()[0] ? remote_net_ip() : "-");
  printf("mqtt %s url='%s' user='%s' disc=%u\n", remote_net_mqtt_state(),
         g_rset.mqtt_url, g_rset.mqtt_user, (unsigned)g_rset.mqtt_disc);
  printf("topics: aio/hub/{status,notify} aio/node/<id>/{state,cmd/feed,event/feed_result,config}\n");
  return 0;
}

/* PIN (§8, FR10): stored as a salted hash in the settings blob; the RAM
 * PinLock tracks the 60 s unlock window. */
static int cmd_pin(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: pin status | set <cur> <new> | off <cur> | scope <0|1>\n");
    return 1;
  }
  StateLock lock;
  if (!strcmp(argv[1], "status")) {
    printf("pin=%s scope=%s fails=%u\n", g_set.pin_enabled ? "on" : "off",
           g_set.pin_scope == PIN_SCOPE_SETTINGS_FEEDS ? "settings+feeds"
                                                       : "settings",
           (unsigned)g_pin.fail_count());
    return 0;
  }
  if (!strcmp(argv[1], "set") && argc == 4) {
    if (g_set.pin_enabled && !pin_verify(g_set, argv[2])) {
      printf("current PIN wrong\n");
      return 1;
    }
    if (!pin_valid_digits(argv[3])) {
      printf("new PIN must be exactly 4 digits\n");
      return 1;
    }
    pin_set(&g_set, argv[3], [](uint8_t *o, size_t n) { esp_fill_random(o, n); });
    settings_persist();
    printf("PIN set (salted hash stored; backups never carry it, D18)\n");
    return 0;
  }
  if (!strcmp(argv[1], "off") && argc == 3) {
    if (!g_set.pin_enabled) {
      printf("PIN already off\n");
      return 0;
    }
    if (!pin_verify(g_set, argv[2])) {
      printf("current PIN wrong\n");
      return 1;
    }
    g_set.pin_enabled = 0;
    settings_persist();
    printf("PIN off\n");
    return 0;
  }
  if (!strcmp(argv[1], "scope") && argc == 3) {
    const uint32_t s = parse_num(argv[2]);
    if (s >= PIN_SCOPE_COUNT) {
      printf("scope 0 = settings only, 1 = settings and manual feeds\n");
      return 1;
    }
    g_set.pin_scope = (uint8_t)s;
    settings_persist();
    printf("scope=%u\n", (unsigned)s);
    return 0;
  }
  printf("usage: pin status | set <cur> <new> | off <cur> | scope <0|1>\n");
  return 1;
}

/* backup / restore / CSV export (§9.12) against the SD card. */
static int cmd_backup(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: backup now | ls | restore <name> | csv\n");
    return 1;
  }
  StateLock lock;
  if (!strcmp(argv[1], "now") && argc == 2) {
    backup_op(false);
    return 0;
  }
  if (!strcmp(argv[1], "csv") && argc == 2) {
    backup_op(true);
    return 0;
  }
  if (!strcmp(argv[1], "ls")) {
    char names[BACKUP_SLOTS_MAX][BACKUP_NAME_LEN];
    const int n = g_sd ? g_sd->list(names, BACKUP_SLOTS_MAX) : 0;
    printf("%s: %d file(s)\n", g_sd_note, n);
    for (int i = 0; i < n; i++) printf("  %s\n", names[i]);
    return 0;
  }
  if (!strcmp(argv[1], "restore") && argc == 3) {
    backup_restore_op(argv[2]);
    return 0;
  }
  printf("usage: backup now | ls | restore <name> | csv\n");
  return 1;
}

/* setup wizard (§9.13) from the bench console — the touchscreen wizard
 * drives the same session; nothing touches the registry until 'wiz save'. */
static int cmd_wiz(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: wiz start|tz|ntp|wifi|node|cap|sched|next|back|save|close|status\n");
    return 1;
  }
  StateLock lock;
  const uint32_t now = ms_now();
  if (!strcmp(argv[1], "start") && argc == 2) {
    printf("%s\n", g_wiz.open() ? "wizard open - step 1: time zone"
                                 : "wizard already open");
    return 0;
  }
  if (!strcmp(argv[1], "close")) {
    g_wiz.close();
    printf("closed\n");
    return 0;
  }
  if (g_wiz.step() == ui::WIZ_IDLE) {
    printf("wiz start first\n");
    return 1;
  }
  if (!strcmp(argv[1], "tz") && argc == 3) {
    printf("%s\n", g_wiz.set_tz((uint8_t)parse_num(argv[2]), g_set.ntp_on)
                        ? "zone set"
                        : "bad rule (0-3)");
  } else if (!strcmp(argv[1], "ntp") && argc == 3) {
    printf("%s\n", g_wiz.set_tz(g_wiz.view().tz_rule, parse_num(argv[2]) != 0)
                        ? "ntp set"
                        : "failed");
  } else if (!strcmp(argv[1], "wifi") && argc >= 3) {
    printf("%s\n", g_wiz.set_wifi(argv[2], argc >= 4 ? argv[3] : "")
                        ? "wifi set"
                        : "ssid required");
    /* the real connection test runs at bring-up on hardware; the bench CLI
     * records the attempt so the step can be passed honestly either way */
    g_wiz.wifi_test_result(false, "checked at boot (bench CLI)");
  } else if (!strcmp(argv[1], "node") && argc == 3) {
    if (!strcmp(argv[2], "new")) {
      g_wiz.choose_pair_new();
      printf("pair a feeder now ('pair open'); the wizard resumes on commit\n");
    } else {
      printf("%s\n", g_wiz.choose_existing((uint8_t)parse_num(argv[2]))
                          ? "node chosen"
                          : "no such node");
    }
  } else if (!strcmp(argv[1], "cap") && argc == 4) {
    printf("%s\n", g_wiz.set_capacity(parse_num(argv[2]), parse_num(argv[3]))
                        ? "capacity set"
                        : "size 1..100000, in-it 0..size");
  } else if (!strcmp(argv[1], "sched") && argc == 5) {
    printf("%s\n", g_wiz.set_schedule((uint16_t)parse_num(argv[2]),
                                       (uint8_t)parse_num(argv[3]),
                                       (uint16_t)parse_num(argv[4]))
                        ? "schedule set"
                        : "bad time/days/amount (see note)");
  } else if (!strcmp(argv[1], "next") && argc == 2) {
    printf("%s\n", g_wiz.next() ? "next step" : "blocked (see note)");
  } else if (!strcmp(argv[1], "back") && argc == 2) {
    printf("%s\n", g_wiz.back() ? "back a step" : "cannot go back");
  } else if (!strcmp(argv[1], "save") && argc == 2) {
    uint8_t nid = 0;
    const WizardSession::CommitResult r = g_wiz.commit(reg, g_set, &nid);
    if (r != WizardSession::WZ_COMMIT_OK) {
      printf("save failed (%u)\n", (unsigned)r);
      return 1;
    }
    settings_persist();
    reg.save(nvs);
    aio::HubNode *n = reg.by_id(nid);
    if (n) {
      eng.send_schedule_set(nid, n->sched.version, n->sched.entries,
                            n->sched.entry_count, n->sched.skip_until,
                            n->sched.hold_until, now);
      eng.send_config(nid, n->profile, 1, now);
      n->sched.pushed_at = (uint32_t)hub_unix_now();
      reg.save(nvs);
    }
    printf("wizard done: node %u - SCHEDULE_SET + CONFIG sent\n", (unsigned)nid);
  } else if (!strcmp(argv[1], "status") && argc == 2) {
    static const char *STEP[] = {"idle", "tz", "wifi", "node", "cap", "sched",
                                 "done"};
    const ui::WizardView v = g_wiz.view();
    printf("step=%s (%u of 6)%s\n", STEP[v.step], (unsigned)v.step_no,
           v.skip_cap ? " [hopper asked at pairing]" : "");
    if (v.note[0]) printf("  note: %s\n", v.note);
  } else {
    printf("usage: wiz start|tz|ntp|wifi|node|cap|sched|next|back|save|close|status\n");
    return 1;
  }
  return 0;
}

static void add_cmd(const char *name, const char *help, const char *hint,
                    esp_console_cmd_func_t fn) {
  esp_console_cmd_t c{};
  c.command = name;
  c.help = help;
  c.hint = hint;
  c.func = fn;
  esp_console_cmd_register(&c);
}

static void register_commands() {
  add_cmd("help", "print command list", nullptr, cmd_help);
  add_cmd("info", "hub mac, channel, counters", nullptr, cmd_info);
  add_cmd("chan", "set ESP-NOW channel", "<1-13>", cmd_chan);
  add_cmd("peer", "register/remove a node", "add <mac> <id> <keyhex> | del <id>", cmd_peer);
  add_cmd("peers", "list peers + last heartbeat", nullptr, cmd_peers);
  add_cmd("feed", "manual feed via feed_manager", "<node_id> <amount>", cmd_feed);
  add_cmd("feedloop", "M2 1000-feed loopback (bypasses feed_manager)", "<node_id> <count>",
          cmd_feedloop);
  add_cmd("sched", "schedule editor", "<id> list|add <min> <days> <amt>|del <i>|clear|push",
          cmd_sched);
  add_cmd("time", "clock state", nullptr, cmd_time);
  add_cmd("tz", "timezone rule", "<0=UTC 1=Manila 2=CET 3=US-East>", cmd_tz);
  add_cmd("stagger", "max simultaneous node feeds", "<1-8>", cmd_stagger);
  add_cmd("events", "recent event log", "[n]", cmd_events);
  add_cmd("sync", "send TIME_SYNC", "<node_id> <unix>", cmd_sync);
  add_cmd("config", "push default profile", "<node_id>", cmd_config);
  add_cmd("statusreq", "ask for a heartbeat now", "<node_id>", cmd_statusreq);
  add_cmd("abort", "send ABORT", "<node_id> <feed_id>", cmd_abort);
  add_cmd("stats", "link counters", nullptr, cmd_stats);
  add_cmd("pair", "pairing window (§6.3)", "open|close|select|confirm|form|status",
          cmd_pair);
  add_cmd("cal", "calibration wizard (§9.3)",
          "start <id>|secs <s>|run|weigh <g>|save|close", cmd_cal);
  add_cmd("set", "hub settings (§8.8)",
          "show|brightness|screenoff|retention|ntp|wifi|factoryreset", cmd_set);
  add_cmd("pin", "PIN lock (§8)", "status|set <cur> <new>|off <cur>|scope <0|1>",
          cmd_pin);
  add_cmd("backup", "backup/restore/CSV (§9.12)", "now|ls|restore <name>|csv",
          cmd_backup);
  add_cmd("wiz", "setup wizard (§9.13)",
          "start|tz|ntp|wifi|node|cap|sched|next|back|save", cmd_wiz);
  add_cmd("remote", "web + MQTT status (§6.7)", "", cmd_remote);
}

/* --- main loop task (1 ms; shares state with the REPL under g_state_mu) ---------- */

static void aio_loop_task(void *) {
  for (;;) {
    {
      StateLock lock;
      const uint32_t now = ms_now();
      nowt.pump(eng, now);
#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT
      aio::sim::pump_to_hub(eng, now); /* node → hub sim frames (QEMU shot build) */
#endif
      eng.tick(now);
      core.tick(now, hub_unix_now());
      remote_net_poll(now, hub_unix_now()); /* deferred applies + 30 s cadence */

#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT
      aio::sim::tick_nodes(now); /* node engines + hub → node sim frames */

      /* pairing flow driver (M4b shot): the session advances on the same
       * 25 s grid the UI rotation uses; slot 5 of 7 is the pairing screen.
       * Keep 25000 and the slot index in sync with ui.cpp SHOT_WINDOW_MS
       * and the SH_PAIR rotation entry. Stage bounds (8/14.5/21 s) put one
       * marker inside each phase with ≥1.5 s margin before the next flip
       * (the runner dumps the fb ~0.8 s after a marker, and marker prints
       * can land late when the ui loop stalls):
       * discover [0,8) code [8,14.5) form [14.5,21) done [21,25)
       * markers at +2 +7 +12 +17 +22 → discover, dup, code, form, done. */
      static uint32_t pair_cycle = UINT32_MAX;
      static uint8_t pair_stage = 0;
      const uint32_t cyc = now / 25000;
      if (cyc % 11 == 5) {
        if (pair_cycle != cyc) {
          pair_cycle = cyc;
          pair_stage = 0;
          /* drop the node a previous rotation committed, then reopen */
          const aio::HubNode *pn = reg.by_mac(aio::sim::SIM_PAIR_MAC);
          if (pn) {
            eng.remove_peer(pn->mac);
            reg.remove(pn->id);
          }
          g_pair.close();
          g_pair.open(now);
        }
        const uint32_t m = now % 25000;
        if (pair_stage == 0 && m >= 8000) {
          if (!g_pair.select(0)) printf("pairing shot: select failed\n");
          pair_stage = 1;
        }
        if (pair_stage == 1 && m >= 14500) {
          if (!g_pair.confirm_code()) printf("pairing shot: confirm failed\n");
          pair_stage = 2;
        }
        if (pair_stage == 2 && m >= 21000) {
          g_pair.set_form("kitA", 0, 1500, 400);
          pair_accept_payload_t acc{};
          uint8_t nid = 0;
          const aio::PairingSession::CommitResult r = g_pair.commit(reg, &acc, &nid);
          if (r == aio::PairingSession::PAIR_COMMIT_OK) {
            pairing_send_accept(aio::sim::SIM_PAIR_MAC, acc, now);
            printf("pairing: committed node %u (PAIR_ACCEPT sent, key wrapped)\n", nid);
          } else {
            printf("pairing shot: commit failed (%d)\n", (int)r);
          }
          pair_stage = 3;
        }
      }

      /* calibration wizard driver (M4b shot): slot 6 of 7, paced so each
       * captured phase holds across a marker beat (markers at +2 +7 +12
       * +17 +22 into the 25 s slot):
       *   start@0   RUNTIME holds [0,8)          → +2 "calib"
       *   run@8     result lands ~8.1 → WEIGH holds [8.1,13.5)
       *   weigh@13.5 run2 → WEIGH holds [14.1,18) → +17 "calib_weigh"
       *   weigh@18  run3 → WEIGH holds [18.6,20)
       *   weigh@20  3 samples → RUNTIME
       *   save@21   DONE holds [21,25)           → +22 "calib_done"
       * (DISPENSE is machine-paced (~50 ms with the sim node) and shares
       * the "calib" marker name; its state is visible in the serial log.) */
      static uint32_t cal_cycle = UINT32_MAX;
      static uint8_t cal_stage = 0;
      if (cyc % 11 == 6) {
        if (cal_cycle != cyc) {
          cal_cycle = cyc;
          cal_stage = 0;
          const aio::HubNode *cn = reg.by_id(1);
          if (cn) {
            g_calib.close();
            g_calib.start(1, cn->profile);
          }
        }
        const uint32_t m = now % 25000;
        if (cal_stage == 0 && m >= 8000) {
          if (!calib_run_send(now)) printf("calib shot: run1 failed\n");
          cal_stage = 1;
        }
        if (cal_stage == 1 && m >= 13500) {
          if (!g_calib.set_weigh_grams_x100(800)) printf("calib shot: weigh1 failed\n");
          if (!calib_run_send(now)) printf("calib shot: run2 failed\n");
          cal_stage = 2;
        }
        if (cal_stage == 2 && m >= 18000) {
          if (!g_calib.set_weigh_grams_x100(750)) printf("calib shot: weigh2 failed\n");
          if (!calib_run_send(now)) printf("calib shot: run3 failed\n");
          cal_stage = 3;
        }
        if (cal_stage == 3 && m >= 20000) {
          if (!g_calib.set_weigh_grams_x100(820)) printf("calib shot: weigh3 failed\n");
          cal_stage = 4;
        }
        if (cal_stage == 4 && m >= 21000) {
          aio::HubNode *cn2 = reg.by_id(1);
          if (cn2 && g_calib.apply(&cn2->profile)) {
            reg.save(nvs);
            eng.send_config(1, cn2->profile, 1, now);
            g_calib.mark_saved();
            printf("calib: node 1 rate now %lu x100 ms/unit (median of 3)\n",
                   (unsigned long)cn2->profile.cal_ms_per_unit_x100);
          } else {
            printf("calib shot: save failed\n");
          }
          cal_stage = 5;
        }
      }

      /* settings screen driver (M4b shot): slot 7 of 10. Stage bounds keep
       * each captured pane across a marker beat (markers at +2 +7 +12 +17
       * +22 into the 25 s slot): RESTART [0,8) → "settings", SECURITY
       * [8,14.5) → "settings_security" (+12), BACKUP [14.5,21) →
       * "settings_backup" (+17), DANGER [21,25). */
      static uint32_t set_cycle = UINT32_MAX;
      static uint8_t set_stage = 0;
      if (cyc % 11 == 7) {
        if (set_cycle != cyc) {
          set_cycle = cyc;
          set_stage = 0;
          g_pin.relock(); /* Security pane shows the locked state */
        }
        const uint32_t m = now % 25000;
        uint8_t sec = SET_SEC_RESTART;
        if (m >= 21000)
          sec = SET_SEC_DANGER;
        else if (m >= 14500)
          sec = SET_SEC_BACKUP;
        else if (m >= 8000)
          sec = SET_SEC_SECURITY;
        if (sec != set_stage) {
          set_stage = sec;
          s_set_section = sec;
        }
      }

      /* PIN pad driver (M4b shot): slot 8 of 10. Idle [0,8) → "pin_entry",
       * two digits [8,14.5) → "pin_typing", four digits + wrong note
       * [14.5,21) → "pin_wrong", correct code [21,25) → "pin_ok". */
      static uint32_t pin_cycle = UINT32_MAX;
      static uint8_t pin_stage = 0;
      if (cyc % 11 == 8) {
        if (pin_cycle != cyc) {
          pin_cycle = cyc;
          pin_stage = 0;
          aio::ui::pin_clear();
          aio::ui::pin_note("");
        }
        const uint32_t m = now % 25000;
        if (pin_stage == 0 && m >= 8000) {
          aio::ui::pin_press('1');
          aio::ui::pin_press('2');
          pin_stage = 1;
        }
        if (pin_stage == 1 && m >= 14500) {
          aio::ui::pin_press('9');
          aio::ui::pin_press('9'); /* 1299 != 1234 → wrong */
          aio::ui::pin_note("wrong PIN - try again");
          pin_stage = 2;
        }
        if (pin_stage == 2 && m >= 21000) {
          aio::ui::pin_clear();
          aio::ui::pin_press('1');
          aio::ui::pin_press('2');
          aio::ui::pin_press('3');
          aio::ui::pin_press('4');
          aio::ui::pin_note("unlocked for 60 s");
          pin_stage = 3;
        }
      }

      /* setup wizard driver (M4b shot): slot 9 of 10, five phases on 5 s
       * beats — tz [0,5) "wizard", node [5,10) "wizard_node", cap [10,15)
       * "wizard_cap", sched [15,20) "wizard_sched", done [20,25)
       * "wizard_done". Commit lands once per cycle; the node's previous
       * schedule is restored at the next cycle start so the seeded scene
       * stays deterministic for the other slots. */
      static uint32_t wiz_cycle = UINT32_MAX;
      static uint8_t wiz_stage = 0;
      static HubSched wiz_saved_sched;
      if (cyc % 11 == 9) {
        if (wiz_cycle != cyc) {
          wiz_cycle = cyc;
          if (wiz_stage == 5) { /* restore what last cycle's commit replaced */
            HubNode *rn = reg.by_id(1);
            if (rn) {
              rn->sched = wiz_saved_sched;
              rn->sched.version = ++reg.data().sched_version;
            }
          }
          wiz_stage = 0;
          HubNode *wn = reg.by_id(1);
          if (wn) wiz_saved_sched = wn->sched;
          g_wiz.close();
          g_wiz.open();
          g_wiz.set_tz(TZ_MANILA, true);
        }
        const uint32_t m = now % 25000;
        if (wiz_stage == 0 && m >= 5000) {
          g_wiz.next(); /* TZ → Wi-Fi (set_tz already ran at cycle start) */
          g_wiz.set_wifi("home-net", "pw");
          g_wiz.wifi_test_result(false, "no radio (QEMU)");
          g_wiz.next(); /* Wi-Fi → node */
          g_wiz.choose_existing(1);
          wiz_stage = 1;
        }
        if (wiz_stage == 1 && m >= 10000) {
          g_wiz.next(); /* → cap (existing node: hopper is asked) */
          g_wiz.set_capacity(1500, 400);
          wiz_stage = 2;
        }
        if (wiz_stage == 2 && m >= 15000) {
          g_wiz.next(); /* → sched */
          g_wiz.set_schedule(480, 0x7F, 40);
          wiz_stage = 3;
        }
        if (wiz_stage == 3 && m >= 20000) {
          uint8_t nid = 0;
          if (g_wiz.commit(reg, g_set, &nid) == WizardSession::WZ_COMMIT_OK) {
            printf("wizard shot: node %u committed (SCHEDULE_SET skipped in the shot build)\n",
                   (unsigned)nid);
          }
          wiz_stage = 5;
        }
      }
      /* tap-path slot (M5, no staged flow of its own): open the §8 gate so
       * the walk's gear tap reaches settings — the seeded PIN would
       * otherwise route it to the PIN pad and the lock mirror would bounce
       * the user back to the dashboard (observed in the first CI run). */
      if (cyc % 11 == 10) {
        static uint32_t tap_cycle = UINT32_MAX;
        if (tap_cycle != cyc) {
          tap_cycle = cyc;
          if (g_pin.enabled()) g_pin.unlock("1234", now);
          aio::ui::nav_sync_lock(false);
          s_set_section = 0; /* the walk's settings beats start at RESTART */
        }
      }
#endif /* shot-only sim tick + rotation drivers */

      /* dashboard + detail snapshots → UI task every 400 ms (both builds:
       * the hardware UI and the touch actions need fresh snapshots too) */
      static uint32_t ui_ms = 0;
      if (now - ui_ms >= 400) {
        ui_ms = now;
        aio::ui::PeerFresh fr[aio::HUB_MAX_NODES];
        uint8_t fn = 0;
        for (int i = 0; i < aio::HUB_MAX_NODES && fn < aio::HUB_MAX_NODES; i++) {
          const aio::HubNode *n = reg.node(i);
          if (!n || !n->id) continue;
          const aio::LinkPeer *p = eng.peer_by_node(n->id);
          fr[fn++] = aio::ui::PeerFresh{n->id, p ? p->last_rx_ms : 0,
                                        p ? p->last_hb.vin_mv : (uint16_t)0,
                                        p ? p->last_hb.flags : (uint8_t)0};
        }
        aio::ui::publish(aio::ui::build_dashboard(reg, evlog, hub_unix_now(), now, fr, fn));
        const uint8_t dn = aio::ui::detail_node();
        aio::ui::publish_detail(
            aio::ui::build_node_detail(reg, evlog, dn, hub_unix_now(), now, fr, fn));
        /* M5b: the engine-owned draft survives the periodic publish; it is
         * only rebuilt when it goes stale (boot, node switch, wipe) */
        if (!s_ed_draft.found || s_ed_draft.node_id != dn)
          s_ed_draft = aio::ui::build_sched_editor(reg, dn, hub_unix_now());
        aio::ui::publish_sched_editor(s_ed_draft);
        aio::ui::publish_history(aio::ui::build_history(reg, evlog, hub_unix_now()));
        aio::ui::publish_alerts(
            aio::ui::build_alerts(reg, evlog, hub_unix_now(), now, fr, fn, g_alerts_ack));
        aio::ui::publish_pairing(g_pair.view(now));
        aio::ui::publish_calib(g_calib.view(now));
        /* settings + wizard snapshots (M4b screens; section via the driver) */
        aio::ui::SettingsView sv =
            aio::ui::build_settings(reg, g_set, hub_unix_now(), g_sd_note,
                                    g_backup_count, g_backup_last,
                                    g_pin.blocks_settings(),
                                    g_pin.unlock_left_s(now));
        sv.section = s_set_section;
        set_remote_view(sv);
        aio::ui::publish_settings(sv);
        aio::ui::publish_wizard(g_wiz.view());
        /* §8: mirror the lock state so the nav model bounces on expiry */
        aio::ui::nav_sync_lock(g_pin.blocks_settings());
      }

      /* M5 touch: service tap requests (both builds — the shot build's tap
       * slot drives this same path through inject_tap). Requests come from
       * the LVGL task as one-shot flags; everything below runs on the
       * engine task with the state lock held, like the bench CLI. */
      aio::nav::Actions acts;
      aio::ui::take_actions(acts);
      if (acts.any()) {
        aio::ui::PeerFresh fr[aio::HUB_MAX_NODES];
        uint8_t fn = 0;
        for (int i = 0; i < aio::HUB_MAX_NODES && fn < aio::HUB_MAX_NODES; i++) {
          const aio::HubNode *n = reg.node(i);
          if (!n || !n->id) continue;
          const aio::LinkPeer *p = eng.peer_by_node(n->id);
          fr[fn++] = aio::ui::PeerFresh{n->id, p ? p->last_rx_ms : 0,
                                        p ? p->last_hb.vin_mv : (uint16_t)0,
                                        p ? p->last_hb.flags : (uint8_t)0};
        }
        const uint32_t un = hub_unix_now();
        if (acts.detail) {
          aio::ui::set_detail_node(acts.detail_node);
          aio::ui::publish_detail(aio::ui::build_node_detail(
              reg, evlog, acts.detail_node, un, now, fr, fn));
        }
        if (acts.editor) {
          aio::ui::set_detail_node(acts.editor_node);
          ed_rebuild(acts.editor_node);
        }
        if (acts.history)
          aio::ui::publish_history(aio::ui::build_history(reg, evlog, un));
        if (acts.alerts)
          aio::ui::publish_alerts(
              aio::ui::build_alerts(reg, evlog, un, now, fr, fn, g_alerts_ack));
        if (acts.settings || acts.set_section >= 0) {
          if (acts.set_section >= 0) s_set_section = (uint8_t)acts.set_section;
          aio::ui::SettingsView sv = aio::ui::build_settings(
              reg, g_set, un, g_sd_note, g_backup_count, g_backup_last,
              g_pin.blocks_settings(), g_pin.unlock_left_s(now));
          sv.section = s_set_section;
          set_remote_view(sv);
          aio::ui::publish_settings(sv);
        }
        /* PIN pad OK → PinLock verify (§8); the verdict routes the nav */
        if (acts.pin_submit) {
          const char *digits = aio::ui::pin_entered();
          const bool ok = strlen(digits) == 4 && g_pin.unlock(digits, now);
          aio::ui::pin_note(ok ? "unlocked for 60 s" : "wrong PIN - try again");
          aio::ui::pin_gate_result(ok);
          aio::ui::nav_sync_lock(g_pin.blocks_settings());
        }
        /* pairing (§6.3): taps drive the same session the bench CLI does */
        if (acts.pair_cancel || acts.pair_close) g_pair.close();
        if (acts.pair_open) g_pair.open(now);
        if (acts.pair_select >= 0) g_pair.select((uint8_t)acts.pair_select);
        if (acts.pair_confirm) g_pair.confirm_code();
        if (acts.pair_commit) {
          pair_accept_payload_t acc{};
          uint8_t nid = 0;
          if (g_pair.commit(reg, &acc, &nid) ==
              aio::PairingSession::PAIR_COMMIT_OK) {
            const aio::HubNode *nn = reg.by_id(nid);
            if (nn) pairing_send_accept(nn->mac, acc, now);
            reg.save(nvs);
            /* wizard "pair a new feeder" flow: the wizard resumes with the
             * capacity step skipped (§9.13) */
            if (g_wiz.is_open() && g_wiz.step() == aio::ui::WIZ_NODE)
              g_wiz.on_paired(nid);
          }
        }
        /* M5b entry pad: one commit per OK, mapped by target (ui_entry.h) */
        if (acts.entry_commit) {
          const entry::Target t = acts.entry_tgt;
          const aio::ui::PairingView pv =
              g_pair.is_open() ? g_pair.view(now) : aio::ui::PairingView{};
          const aio::ui::WizardView wv = g_wiz.view();
          switch (t) {
            case entry::ENT_PAIR_NAME:
              g_pair.set_form(acts.entry_text, pv.mech_sel, pv.cap_total, pv.cap_now);
              break;
            case entry::ENT_PAIR_TOTAL:
              g_pair.set_form(pv.name, pv.mech_sel, acts.entry_u32, pv.cap_now);
              break;
            case entry::ENT_PAIR_NOW:
              g_pair.set_form(pv.name, pv.mech_sel, pv.cap_total, acts.entry_u32);
              break;
            case entry::ENT_WIZ_SSID:
              if (g_wiz.step() == aio::ui::WIZ_WIFI) g_wiz.set_wifi_ssid(acts.entry_text);
              break;
            case entry::ENT_WIZ_TOTAL:
              if (g_wiz.step() == aio::ui::WIZ_CAP)
                g_wiz.set_capacity(acts.entry_u32, wv.cap_now);
              break;
            case entry::ENT_WIZ_NOW:
              if (g_wiz.step() == aio::ui::WIZ_CAP)
                g_wiz.set_capacity(wv.cap_total, acts.entry_u32);
              break;
            case entry::ENT_WIZ_TIME:
              if (g_wiz.step() == aio::ui::WIZ_SCHED)
                g_wiz.set_schedule((uint16_t)acts.entry_u32, wv.s_days, wv.s_amount);
              break;
            case entry::ENT_WIZ_AMOUNT:
              if (g_wiz.step() == aio::ui::WIZ_SCHED)
                g_wiz.set_schedule(wv.s_min, wv.s_days, (uint16_t)acts.entry_u32);
              break;
            case entry::ENT_CAL_WEIGH:
              g_calib.set_weigh_grams_x100(acts.entry_u32);
              break;
            case entry::ENT_EDIT_TIME:
              aio::ui::editor_set_time(s_ed_draft, (uint16_t)acts.entry_u32);
              ed_publish();
              break;
            case entry::ENT_EDIT_AMOUNT:
              aio::ui::editor_set_amount(s_ed_draft, (uint16_t)acts.entry_u32);
              ed_publish();
              break;
            default:
              break;
          }
          /* instant feedback on the session screens (the 400 ms publish
           * also refreshes; this keeps the pads feeling live) */
          if (t >= entry::ENT_PAIR_NAME && t <= entry::ENT_PAIR_NOW)
            aio::ui::publish_pairing(g_pair.view(now));
          if (t >= entry::ENT_WIZ_SSID && t <= entry::ENT_WIZ_AMOUNT)
            aio::ui::publish_wizard(g_wiz.view());
          if (t == entry::ENT_CAL_WEIGH) aio::ui::publish_calib(g_calib.view(now));
        }
        /* pairing form: mech chip cycles the template (§8.5 — caps reset to
         * the template defaults on a mech change) */
        if (acts.pair_mech && g_pair.is_open()) {
          const aio::ui::PairingView pv2 = g_pair.view(now);
          const uint8_t m = (uint8_t)((pv2.mech_sel + 1) % 3);
          g_pair.set_form(pv2.name, m, aio::ui::pair_template_capacity(m), 0);
          aio::ui::publish_pairing(g_pair.view(now));
        }
        /* setup wizard (§9.13): tz rows + next/back/save/close; the name,
         * capacity and schedule fields arrive through the entry pad */
        if (acts.wiz_open) g_wiz.open();
        if (acts.wiz_tz >= 0 && g_wiz.step() == aio::ui::WIZ_TZ)
          g_wiz.set_tz((uint8_t)acts.wiz_tz, g_set.ntp_on != 0);
        /* M5b wizard extras: node-picker rows, pair-new, NTP toggle, days
         * preset cycle (view() carries the drafts these mutate) */
        if (acts.wiz_node >= 0 && g_wiz.step() == aio::ui::WIZ_NODE)
          g_wiz.choose_existing((uint8_t)acts.wiz_node);
        if (acts.wiz_pair_new && g_wiz.step() == aio::ui::WIZ_NODE)
          g_wiz.choose_pair_new();
        if (acts.wiz_ntp && g_wiz.step() == aio::ui::WIZ_TZ) {
          const aio::ui::WizardView wv2 = g_wiz.view();
          g_wiz.set_tz(wv2.tz_rule, !wv2.ntp_on);
        }
        if (acts.wiz_days && g_wiz.step() == aio::ui::WIZ_SCHED) {
          const aio::ui::WizardView wv3 = g_wiz.view();
          g_wiz.set_schedule(wv3.s_min, aio::ui::wiz_cycle_days_preset(wv3.s_days),
                             wv3.s_amount);
        }
        if (acts.wiz_back) g_wiz.back();
        if (acts.wiz_next) g_wiz.next();
        if (acts.wiz_commit) {
          uint8_t nid = 0;
          if (g_wiz.commit(reg, g_set, &nid) ==
              aio::WizardSession::WZ_COMMIT_OK) {
            settings_persist();
            reg.save(nvs);
            aio::HubNode *n = reg.by_id(nid);
            if (n) {
              eng.send_schedule_set(nid, n->sched.version, n->sched.entries,
                                    n->sched.entry_count, n->sched.skip_until,
                                    n->sched.hold_until, now);
              eng.send_config(nid, n->profile, 1, now);
              n->sched.pushed_at = (uint32_t)hub_unix_now();
              reg.save(nvs);
            }
          }
        }
        if (acts.wiz_close) g_wiz.close();
        /* calibration: close/run/weigh-entry/save — the weigh pad commits
         * through entry_commit above; the bench CLI keeps its flow too */
        if (acts.cal_close) g_calib.close();
        if (acts.cal_run) calib_run_send(now);
        if (acts.cal_save) {
          const uint8_t cnid = g_calib.node_id();
          node_profile_t np{};
          if (cnid && g_calib.apply(&np)) {
            aio::HubNode *cn = reg.by_id(cnid);
            if (cn) {
              cn->profile = np; /* median + re-CRC'd (apply) */
              reg.save(nvs);
              eng.send_config(cnid, cn->profile, 1, now);
              g_calib.mark_saved();
              printf("cal: median saved for F%u and pushed\n", (unsigned)cnid);
            }
          }
        }
        /* detail ops (M4b affordances wired in M5b): Feed now / Skip next /
         * Hold 1 h — the PIN scope guards manual feeds (§8) */
        if (acts.detail_feed >= 0) {
          const uint8_t fid = (uint8_t)acts.detail_feed;
          if (g_pin.blocks_manual_feed()) {
            printf("feed now: PIN required (scope covers manual feeds)\n");
          } else {
            const aio::HubNode *n = reg.by_id(fid);
            if (n) {
              uint16_t amt = 20; /* preview default when nothing is scheduled */
              uint16_t nmin = 0, namt = 0;
              int32_t nin = 0;
              const aio::LocalTime lt =
                  time_valid()
                      ? aio::local_from_unix(hub_unix_now(), reg.data().tz_rule)
                      : aio::LocalTime{};
              if (time_valid() &&
                  aio::ui::next_occurrence(*n, lt.min_of_day, lt.wday, &nmin, &nin,
                                           &namt))
                amt = namt;
              if (!core.request_manual(fid, amt, now,
                                       time_valid() ? hub_unix_now() : 0))
                printf("feed now: refused (busy / no node)\n");
            }
          }
        }
        if (acts.detail_skip || acts.detail_hold) {
          aio::HubNode *n = reg.by_id(aio::ui::detail_node());
          if (n && time_valid()) {
            if (acts.detail_hold) {
              n->sched.hold_until = (uint32_t)hub_unix_now() + 3600;
            } else {
              uint16_t nmin = 0, namt = 0;
              int32_t nin = 0;
              const aio::LocalTime lt =
                  aio::local_from_unix(hub_unix_now(), reg.data().tz_rule);
              if (aio::ui::next_occurrence(*n, lt.min_of_day, lt.wday, &nmin, &nin,
                                           &namt))
                n->sched.skip_until =
                    (uint32_t)((hub_unix_now() / 60) * 60 + (int64_t)nin * 60);
              else
                n->sched.skip_until = (uint32_t)hub_unix_now() + 86400;
            }
            reg.save(nvs);
            sched_push(n->id);
          }
        }
        /* schedule editor taps (M5b): the engine-owned draft (§8.3) */
        if (acts.ed_row != -2) {
          if (acts.ed_row < 0)
            s_ed_draft.selected = -1;
          else
            aio::ui::editor_select(s_ed_draft, acts.ed_row);
          ed_publish();
        }
        if (acts.ed_new) {
          s_ed_draft.selected = -1; /* draft a fresh row from the current values */
          ed_publish();
        }
        if (acts.ed_day != 255) {
          aio::ui::editor_toggle_day(s_ed_draft, acts.ed_day);
          ed_publish();
        }
        if (acts.ed_apply) {
          const uint8_t nid = s_ed_draft.node_id;
          if (aio::ui::editor_apply_draft(s_ed_draft)) {
            ed_sync_registry(nid);
            ed_rebuild(nid); /* canonical copy back from the registry */
          }
        }
        if (acts.ed_del) {
          const uint8_t nid = s_ed_draft.node_id;
          aio::ui::editor_remove_selected(s_ed_draft);
          ed_sync_registry(nid);
          ed_rebuild(nid);
        }
        if (acts.ed_toggle) {
          aio::HubNode *n = reg.by_id(s_ed_draft.node_id);
          if (n) {
            n->sched.enabled = n->sched.enabled ? 0 : 1;
            reg.save(nvs); /* hub-side gate: enabled is not part of SCHEDULE_SET */
            s_ed_draft.enabled = n->sched.enabled != 0;
            ed_publish();
          }
        }
        /* settings panes: backup/export, restore latest, restart */
        if (acts.backup_export) backup_op(false);
        if (acts.backup_restore && g_backup_last[0])
          backup_restore_op(g_backup_last);
        if (acts.restart) {
          printf("restart requested from the settings screen\n");
          fflush(stdout);
          vTaskDelay(pdMS_TO_TICKS(200));
          esp_restart();
        }
      }

      /* pairing window expiry (§6.3): the shot driver manages its own
       * lifecycle on the rotation grid, so only the non-shot build ticks */
#if !(defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT)
      g_pair.tick(now);
#endif
      /* FEED_RESULT deadline (§9.3.2) — both builds: the shot driver's runs
       * resolve in ~50 ms with the sim node, far inside the 15 s window */
      g_calib.tick(now);
      /* §8: the PIN unlock window auto re-locks after 60 s */
      g_pin.tick(now);
      /* §7.5 retention: prune the RAM ring once a minute (feeding never
       * waits for this — the ring self-evicts at CAP anyway) */
      {
        static uint32_t last_prune_ms = 0;
        if (now - last_prune_ms > 60000) {
          last_prune_ms = now;
          if (time_valid()) evlog.prune_older_than((uint32_t)hub_unix_now(),
                                                   g_set.history_days);
        }
      }

      /* feedloop driver: one feed in flight at a time (§9.5) — M2 bench path,
       * deliberately bypassing feed_manager so the radio loopback measures the
       * link alone. REPL-only: compiled out of the QEMU shot build. */
#if !(defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT)
      if (loop_target && loop_sent < loop_target && !eng.feed_pending() &&
          (int32_t)(now - loop_next_ms) >= 0) {
        const feed_payload_t f{++reg.data().feed_id_seq, 1, UNIT_GRAMS, 0};
        if (eng.send_feed(loop_node, f, now)) {
          loop_sent++;
          loop_next_ms = now + 20; /* next after this resolves (or times out) */
        } else {
          loop_next_ms = now + 100;
        }
      }
      if (loop_target && loop_sent >= loop_target && !eng.feed_pending()) {
        printf("feedloop done: sent=%u ok=%u dup_errors=%u (expect sent==ok, 0 dups)\n",
               (unsigned)loop_sent, (unsigned)loop_ok, (unsigned)loop_dup_errors);
        loop_target = 0;
      }
#endif
    }
    remote_net_service(); /* deferred web/MQTT restarts — mutex NOT held here */
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

/* --- boot ------------------------------------------------------------------------ */

extern "C" void app_main(void) {
  printf("\n%s %s (ESP-IDF) — proto v%d\n", AIO_FIRMWARE_NAME,
         esp_app_get_description()->version, PROTO_VER);

  g_state_mu = xSemaphoreCreateMutex();

  if (!nvs.begin()) printf("NVS init FAILED (persistence off)\n");
  if (!reg.load(nvs)) {
    printf("registry: fresh (no valid blob)\n");
    reg.reset();
  }
  if (!settings_load(nvs, &g_set)) {
    g_set = settings_defaults();
    printf("settings: defaults (no valid blob)\n");
  }
  if (!aio::remote_load(nvs, &g_rset)) {
    g_rset = aio::remote_defaults();
    printf("remote: defaults (no valid blob)\n");
  }
  g_pin.configure(g_set);
  g_wiz.bind_registry(&reg);
  g_sd = backup_store_create();
  if (!g_sd->mount()) printf("backup store: %s unavailable\n", backup_store_kind());
  sd_refresh_note();
  backup_list_note();
  printf("settings: brightness=%u screenoff=%us retention=%ud pin=%s sd=%s\n",
         (unsigned)g_set.brightness, (unsigned)g_set.screen_off_s,
         (unsigned)g_set.history_days, g_set.pin_enabled ? "on" : "off", g_sd_note);

#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT
  /* pin the wall clock + seed the demo registry before the boot scan (M4a
   * QEMU screenshot build — see tools/qemu/README.md) */
  aio::sim::seed(reg, evlog, g_alerts_ack);
  /* shot scene: PIN lock engaged (scope: settings only) + a real backup file
   * on the virtual SD so the settings panes show live data */
  g_set.brightness = 70;
  pin_set(&g_set, "1234", [](uint8_t *o, size_t n) { esp_fill_random(o, n); });
  g_pin.configure(g_set);
  {
    char *buf = static_cast<char *>(malloc(BACKUP_JSON_MAX));
    char name[BACKUP_NAME_LEN];
    size_t len = 0;
    if (buf && backup_write_now(*g_sd, reg, g_set, hub_unix_now(), buf,
                                BACKUP_JSON_MAX, name, &len))
      printf("virtual SD: seeded %s (%zu B)\n", name, len);
    free(buf);
    backup_list_note();
  }
#endif

  time_begin(); /* PCF85063 → system clock; SNTP starts with the radio */
  evlog.on_push = remote_net_on_event; /* M6: feed_result + notify publishes */
  core.begin(&reg, &link_adapter, &nvs, &evlog, hub_unix_now());
  g_pair.begin([](uint8_t *out, size_t n) { esp_fill_random(out, n); });
  g_pair.set_channel(HUB_CHAN_DEFAULT);

  /* QEMU boot marker — everything above works without the radio */
  printf("core ready (radio starting)\n");

#if defined(CONFIG_AIO_NO_RADIO) && CONFIG_AIO_NO_RADIO
  printf("radio disabled (QEMU smoke build) — core+console only\n");
#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT
  eng.begin(aio::LinkEngine::Role::Hub, aio::sim::hub_transport(), make_handlers(), ms_now());
#else
  eng.begin(aio::LinkEngine::Role::Hub, nullptr, make_handlers(), ms_now());
#endif
  eng.set_self(aio::LINK_HUB_ID, 0);
  eng.set_channel(HUB_CHAN_DEFAULT);
#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT
  aio::sim::begin_nodes();
#endif
#else
  if (!nowt.begin(HUB_CHAN_DEFAULT)) printf("ESP-NOW init FAILED\n");
  eng.begin(aio::LinkEngine::Role::Hub, &nowt, make_handlers(), ms_now());
  eng.set_self(aio::LINK_HUB_ID, 0);
  eng.set_channel(HUB_CHAN_DEFAULT);
#endif

  /* registry → link peers */
  for (int i = 0; i < aio::HUB_MAX_NODES; i++) {
    aio::HubNode *n = reg.node(i);
    if (!n || !n->id) continue;
    eng.add_peer(n->mac, n->id, n->key, n->key_len);
  }

  /* M7: pending unpairs — peers back so heartbeat retries work (FMEA #15) */
  if (g_unpair.load(nvs)) {
    for (uint8_t i = 0; i < g_unpair.count(); i++) {
      const aio::PendingUnpair *q = g_unpair.at(i);
      if (q) eng.add_peer(q->mac, q->id, q->key, q->key_len);
    }
    if (g_unpair.count())
      printf("pending unpairs: %u (UNPAIR retried on check-in)\n", g_unpair.count());
  }

  {
    StateLock lock;
    printf("registry nodes=%u tz=%u stagger=%u feed_id_seq=%lu\n", reg.count(),
           reg.data().tz_rule, reg.data().stagger_max,
           (unsigned long)reg.data().feed_id_seq);
  }

  /* M6 remote: web dashboard + MQTT (no-op under AIO_NO_RADIO) */
  {
    RemoteNetCtx rctx{&reg,      &evlog, &core, &g_set, &g_rset,
                      &nvs,       &eng,   &g_unpair,
                      esp_app_get_description()->version,
                      ms_now,     hub_unix_now, g_state_mu};
    remote_net_init(&rctx);
    remote_net_apply();
  }

  /* 8 KB: the pairing commit path (session commit → PAIR_ACCEPT send →
   * registry NVS save) digs deep into spi_flash; 6 KB overflowed in the
   * QEMU shot build (docs/bench-m4b.md). The REPL skip below pays for it. */
  xTaskCreate(aio_loop_task, "aio-loop", 8192, nullptr, 5, nullptr);

#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT
  aio::ui::start(); /* LVGL dashboard + AIO_SHOT markers (QEMU shot build) */
#endif

#if !(defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT)
  /* bench REPL on UART0 — skipped in the QEMU shot build: the REPL task
   * stack + console state are several KB of internal heap the LVGL build
   * needs more than an interactive shell it cannot expose. */
  esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_cfg.prompt = "aio-feeder";
  repl_cfg.max_cmdline_length = 200;
  register_commands();
  esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
  esp_console_repl_t *repl = nullptr;
  if (esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl) == ESP_OK) {
    printf("ready — 'help' for commands\n");
    hub_help();
    esp_console_start_repl(repl); /* returns after starting the REPL task */
  } else {
    printf("console REPL init FAILED (commands unavailable)\n");
  }
#else
  printf("ready (shot build — REPL off)\n");
#endif
}
