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
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <time.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>

#include "hub_core.h"
#include "hub_registry.h"
#include "hub_store.h"
#include "hub_time.h"
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

/* feedloop state (M2 1000-feed loopback, §12 M2 exit criterion) */
static uint32_t loop_target = 0;
static uint32_t loop_sent = 0;
static uint32_t loop_ok = 0;
static uint32_t loop_dup_errors = 0;
static uint32_t loop_last_feed_id = 0;
static uint32_t loop_next_ms = 0;
static uint8_t loop_node = 0;
static uint32_t seen_results[1024];
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

/* --- engine callbacks ----------------------------------------------------------- */

static void cb_hello(const uint8_t mac[6], const hello_payload_t &pl, int8_t rssi) {
  printf("HELLO from %02X:%02X:%02X:%02X:%02X:%02X fw=%u state=%u rssi=%d%s\n", mac[0],
         mac[1], mac[2], mac[3], mac[4], mac[5], pl.fw_ver, pl.state, rssi,
         pl.state == 0 ? " (unpaired: peer add this mac)" : "");
}

static void cb_feed_ack(uint8_t node_id, const feed_ack_payload_t &a) {
  printf("FEED_ACK node=%u id=%u accepted=%u reason=%u\n", node_id, (unsigned)a.feed_id,
         a.accepted, a.reason);
  core.on_feed_ack(node_id, a, (uint32_t)hub_unix_now());
}

static void cb_feed_result(uint8_t node_id, const feed_result_payload_t &r) {
  /* duplicate detection for the 1000-feed loopback (§12 M2 exit criterion) */
  if (r.feed_id == loop_last_feed_id && loop_target) return; /* engine dedups; belt+braces */
  if (loop_target && r.feed_id > loop_last_feed_id) {
    if (seen_results[r.feed_id % 1024] == r.feed_id) loop_dup_errors++;
    seen_results[r.feed_id % 1024] = r.feed_id;
    loop_last_feed_id = r.feed_id;
    loop_ok++;
  }
  printf("FEED_RESULT node=%u id=%u status=%u on_ms=%u pulses=%u t=%u\n", node_id,
         (unsigned)r.feed_id, r.status, r.actual_on_ms, r.pulses, (unsigned)r.t_unix);
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
}

static void cb_pair_req(const uint8_t mac[6], const pair_req_payload_t &pr, int8_t rssi) {
  const bool added = g_pair.on_pair_req(mac, pr, rssi, ms_now(), &reg);
  printf("PAIR_REQ from %02X:%02X:%02X:%02X:%02X:%02X fw=%u rssi=%d%s\n", mac[0], mac[1],
         mac[2], mac[3], mac[4], mac[5], pr.fw_ver, rssi,
         added ? " (candidate)" : "");
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
                                                  "CATCHUP"};
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
    eng.remove_peer(n->mac);
    reg.remove(id);
    nvs.del(aio::FeedManager::pend_key_for(id));
    reg.save(nvs);
    printf("peer removed\n");
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
  if (argc != 3) { printf("usage: feed <node_id> <amount>\n"); return 1; }
  StateLock lock;
  const uint32_t now = ms_now();
  const int64_t unix_now = hub_unix_now();
  if (!time_valid()) printf("note: clock invalid — manual feed still allowed (SR7)\n");
  const bool ok = core.request_manual((uint8_t)parse_num(argv[1]), (uint16_t)parse_num(argv[2]),
                                      now, unix_now > 0 ? unix_now : 0);
  printf("%s\n", ok ? "feed submitted (feed_manager)" : "feed refused (no node / busy)");
  return 0;
}

static int cmd_feedloop(int argc, char **argv) {
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

#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT
      aio::sim::tick_nodes(now); /* node engines + hub → node sim frames */

      /* pairing flow driver (M4b shot): the session advances on the same
       * 25 s grid the UI rotation uses; slot 5 of 6 is the pairing screen.
       * Keep 25000 and the slot index in sync with ui.cpp SHOT_WINDOW_MS
       * and the SH_PAIR rotation entry. Stage bounds (8.5/12/16 s) sit
       * between the runner's 5 s marker cadence so each step gets its own
       * capture: discover [0,8.5) code [8.5,12) form [12,16) done [16,25). */
      static uint32_t pair_cycle = UINT32_MAX;
      static uint8_t pair_stage = 0;
      const uint32_t cyc = now / 25000;
      if (cyc % 6 == 5) {
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
        if (pair_stage == 0 && m >= 8500) {
          g_pair.select(0); /* → CODE step: the big 4-digit display */
          pair_stage = 1;
        }
        if (pair_stage == 1 && m >= 12000) {
          g_pair.confirm_code(); /* → FORM step */
          pair_stage = 2;
        }
        if (pair_stage == 2 && m >= 16000) {
          g_pair.set_form("kitA", 0, 1500, 400);
          pair_accept_payload_t acc{};
          uint8_t nid = 0;
          if (g_pair.commit(reg, &acc, &nid) == aio::PairingSession::PAIR_COMMIT_OK) {
            pairing_send_accept(aio::sim::SIM_PAIR_MAC, acc, now);
            printf("pairing: committed node %u (PAIR_ACCEPT sent, key wrapped)\n", nid);
          }
          pair_stage = 3;
        }
      }

      /* dashboard + detail snapshots → UI task every 400 ms */
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
        aio::ui::publish_sched_editor(
            aio::ui::build_sched_editor(reg, dn, hub_unix_now()));
        aio::ui::publish_history(aio::ui::build_history(reg, evlog, hub_unix_now()));
        aio::ui::publish_alerts(
            aio::ui::build_alerts(reg, evlog, hub_unix_now(), now, fr, fn, g_alerts_ack));
        aio::ui::publish_pairing(g_pair.view(now));
      }
#endif

      /* pairing window expiry (§6.3): the shot driver manages its own
       * lifecycle on the rotation grid, so only the non-shot build ticks */
#if !(defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT)
      g_pair.tick(now);
#endif

      /* feedloop driver: one feed in flight at a time (§9.5) — M2 bench path,
       * deliberately bypassing feed_manager so the radio loopback measures the
       * link alone. */
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
    }
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

#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT
  /* pin the wall clock + seed the demo registry before the boot scan (M4a
   * QEMU screenshot build — see tools/qemu/README.md) */
  aio::sim::seed(reg, evlog, g_alerts_ack);
#endif

  time_begin(); /* PCF85063 → system clock; SNTP starts with the radio */
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

  {
    StateLock lock;
    printf("registry nodes=%u tz=%u stagger=%u feed_id_seq=%lu\n", reg.count(),
           reg.data().tz_rule, reg.data().stagger_max,
           (unsigned long)reg.data().feed_id_seq);
  }

  xTaskCreate(aio_loop_task, "aio-loop", 6144, nullptr, 5, nullptr);

#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT
  aio::ui::start(); /* LVGL dashboard + AIO_SHOT markers (QEMU shot build) */
#endif

  /* bench REPL on UART0 */
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
}
