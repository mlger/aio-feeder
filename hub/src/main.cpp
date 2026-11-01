/*
 * AIO Feeder — hub firmware, M3 bench host (PLAN.md §12 M3).
 *
 * M3 core now runs on the hub, wired to the same LinkEngine as M2:
 *
 *   - schedule engine (hub_core.h): 1 Hz eval, fire-once occurrences, §9.4
 *     catch-up-once / skip on reboot, §9.9 skip/hold gates;
 *   - feed_manager: the only FEED originator (§7.2 FSM, §9.5 stagger);
 *   - persistence (hub_store.h on NVS): registry (peers, keys, profiles,
 *     schedules) + per-node run state + feed WAL — schedules survive reboot;
 *   - time: NTP over Wi-Fi when configured, PCF85063 RTC fallback (§7.4);
 *     TIME_SYNC pushed to nodes on drift > 2 s or time-invalid heartbeats.
 *
 * Serial CLI (M2 bench commands still present):
 *
 *   peer add <mac> <id> <key>   register + persist a node
 *   feed <id> <amount>          manual feed via feed_manager
 *   feedloop <id> <n>           M2 1000-feed loopback (bypasses feed_manager!)
 *   sched <id> list|add <min> <days> <amt>|del <i>|clear|push
 *   time / tz <0-3> / stagger <n> / events [n]
 *   hb / stats / chan / sync / config / abort / statusreq
 *
 * Wi-Fi (optional): -D AIO_WIFI_SSID='"..."' -D AIO_WIFI_PASS='"..."'.
 */
#include <Arduino.h>
#include <Preferences.h>
#include <sys/time.h>
#include <time.h>

#include "hub_core.h"
#include "hub_registry.h"
#include "hub_store.h"
#include "hub_time.h"
#include "now_esp32.h"
#include "pcf85063.h"

#include "../../lib/proto/messages.h"
#include "../../lib/proto/profile.h"
#include "../../lib/proto/proto.h"
#include "../../link_core/link.h"

#ifndef AIO_FIRMWARE_NAME
#define AIO_FIRMWARE_NAME "aio-feeder-hub"
#endif

constexpr uint8_t HUB_CHAN_DEFAULT = 1;
constexpr int64_t TIME_VALID_MIN = 1600000000; /* 2020-09 (SR7 range check) */

/* --- NVS store ----------------------------------------------------------------- */

namespace aio {
class NvsHubStore : public IHubStore {
 public:
  bool begin() { return prefs_.begin("aio-hub", false); }
  bool put(const char *key, const uint8_t *data, size_t len) override {
    return prefs_.putBytes(key, data, len) == len;
  }
  size_t get(const char *key, uint8_t *out, size_t max_len) override {
    const size_t n = prefs_.getBytesLength(key);
    if (!n || n > max_len) return 0;
    return prefs_.getBytes(key, out, max_len) ? n : 0;
  }
  bool del(const char *key) override {
    prefs_.remove(key);
    return true;
  }

 private:
  Preferences prefs_;
};
}  // namespace aio

static NowTransportEsp32 nowt;
static aio::LinkEngine eng;
static aio::NvsHubStore nvs;
static aio::HubRegistry reg;
static aio::EventLog evlog;
static aio::HubCore core;
static aio::Pcf85063 rtc;
static bool rtc_ok = false;

struct LinkAdapter : aio::IHubLink {
  bool send_feed(uint8_t node, const feed_payload_t &f, uint32_t now_ms) override {
    return eng.send_feed(node, f, now_ms);
  }
};
static LinkAdapter link_adapter;

static uint32_t loop_start_ms = 0;
static uint32_t loop_target = 0;
static uint32_t loop_sent = 0;
static uint32_t loop_ok = 0;
static uint32_t loop_dup_errors = 0;
static uint32_t loop_last_feed_id = 0;
static uint32_t loop_next_ms = 0;
static uint8_t loop_node = 0;
static uint32_t seen_results[1024];
static uint32_t last_full_hb_push_ms = 0;

/* --- time --------------------------------------------------------------------- */

static int64_t hub_unix_now() { return (int64_t)time(nullptr); }

static bool time_valid() { return hub_unix_now() >= TIME_VALID_MIN; }

static const char *time_source_name() {
  if (!time_valid()) return "none";
  return "ntp-or-rtc"; /* UI (M4) distinguishes via staleness; CLI prints raw */
}

static void rtc_to_system() {
  aio::PcfTime t;
  bool os_flag = false;
  if (!rtc_ok || !rtc.read(t, os_flag) || os_flag) {
    Serial.printf("RTC %s — not trusted (os=%d)\n",
                  rtc_ok ? "out of range" : "absent", (int)os_flag);
    return;
  }
  const int64_t days = aio::days_from_civil(t.year, t.mon, t.mday);
  /* PCF weekday: 0 = Sunday; wday_from_days same order */
  const int64_t unix = days * 86400 + (int64_t)t.hour * 3600 +
                       (int64_t)t.min * 60 + t.sec;
  if (unix < TIME_VALID_MIN) {
    Serial.println(F("RTC value out of range — ignored"));
    return;
  }
  struct timeval tv;
  tv.tv_sec = (time_t)unix;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  Serial.printf("RTC → system time: %lld (os_flag=%d)\n", (long long)unix,
                (int)os_flag);
}

static void time_begin() {
  rtc_ok = rtc.begin();
#if defined(AIO_WIFI_SSID) && defined(AIO_WIFI_PASS)
  Serial.printf("Wi-Fi connecting to %s …\n", AIO_WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(AIO_WIFI_SSID, AIO_WIFI_PASS);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
#endif
  rtc_to_system();
}

/* --- engine callbacks -------------------------------------------------------- */

static void cb_hello(const uint8_t mac[6], const hello_payload_t &pl, int8_t rssi) {
  Serial.printf("HELLO from %02X:%02X:%02X:%02X:%02X:%02X fw=%u state=%u rssi=%d%s\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], pl.fw_ver, pl.state, rssi,
                pl.state == 0 ? " (unpaired: peer add this mac)" : "");
}

static void cb_feed_ack(uint8_t node_id, const feed_ack_payload_t &a) {
  Serial.printf("FEED_ACK node=%u id=%u accepted=%u reason=%u\n", node_id, a.feed_id,
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
  Serial.printf("FEED_RESULT node=%u id=%u status=%u on_ms=%u pulses=%u t=%u\n", node_id,
                r.feed_id, r.status, r.actual_on_ms, r.pulses, (unsigned)r.t_unix);
  core.on_feed_result(node_id, r, (uint32_t)hub_unix_now());
}

static void cb_heartbeat(uint8_t node_id, const heartbeat_payload_t &hb, int8_t rssi) {
  Serial.printf("HB node=%u up=%us vin=%u day=%u flags=0x%02X drift=%d rssi=%d\n", node_id,
                (unsigned)(hb.uptime_ms / 1000), hb.vin_mv, hb.day_units, hb.flags,
                hb.rtc_drift_ms, rssi);
  /* Ack non-reply heartbeats with STATUS_REQ so the node's freshness window
   * (§6.2 scan trigger) stays fed on an otherwise quiet link. */
  aio::LinkPeer *p = eng.peer_by_node(node_id);
  if (p && p->last_flags == 0) eng.send_status_req(node_id, millis());
  /* §7.4: TIME_SYNC at least per heartbeat when the node's time looks off. */
  const uint32_t now = millis();
  const bool stale_push = (uint32_t)(now - last_full_hb_push_ms) > 10u * 60u * 1000u;
  if (time_valid() && p &&
      ((hb.flags & HB_FLAG_TIME_INVALID) || abs(hb.rtc_drift_ms) > 2000 || stale_push)) {
    time_sync_payload_t t{(uint32_t)hub_unix_now(), reg.data().tz_rule};
    eng.send_time_sync(node_id, t, now);
  }
}

static void cb_pair_req(const uint8_t mac[6], const pair_req_payload_t &pr, int8_t rssi) {
  Serial.printf("PAIR_REQ from %02X:%02X:%02X:%02X:%02X:%02X fw=%u (pairing UI is M4)\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], pr.fw_ver);
}

static void cb_feed_unknown(uint32_t feed_id, uint8_t node_id) {
  Serial.printf("FEED UNKNOWN id=%u node=%u (alert: never re-issued, §6.6)\n", feed_id,
                node_id);
  core.on_feed_unknown(feed_id, node_id, (uint32_t)hub_unix_now());
}

static void cb_schedule_ack(uint8_t node_id, const ack_status_payload_t &a) {
  Serial.printf("SCHEDULE_ACK node=%u v=%u ok=%u\n", node_id, a.version, a.ok);
}
static void cb_config_ack(uint8_t node_id, const ack_status_payload_t &a) {
  Serial.printf("CONFIG_ACK node=%u crc=0x%04X ok=%u\n", node_id, a.version, a.ok);
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

/* --- CLI ---------------------------------------------------------------------- */

static const char *ev_kind_name(uint8_t k) {
  static const char *names[aio::EV_KIND_COUNT] = {"FEED_OK", "FEED_FAIL", "FEED_REJ",
                                                  "FEED_UNKNOWN", "SKIP_USER",
                                                  "SKIP_HOLD", "SKIP_MISSED",
                                                  "CATCHUP"};
  return k < aio::EV_KIND_COUNT ? names[k] : "?";
}

static void hub_help() {
  Serial.println(F("AIO feeder hub CLI (M3 bench core)"));
  Serial.println(F("  info                   mac, channel, counters"));
  Serial.println(F("  chan <1-13>            set ESP-NOW channel"));
  Serial.println(F("  peer add <mac> <id> <keyhex>   register + persist"));
  Serial.println(F("  peer del <id>"));
  Serial.println(F("  peers                  list peers + last heartbeat"));
  Serial.println(F("  feed <id> <amount>     manual feed (feed_manager FSM)"));
  Serial.println(F("  feedloop <id> <n>      M2 loopback, bypasses feed_manager"));
  Serial.println(F("  sched <id> list | add <min> <days> <amt> | del <i> | clear | push"));
  Serial.println(F("  time | tz <0-3>        clock state / timezone rule"));
  Serial.println(F("  stagger <n>            max simultaneous node feeds (§9.5)"));
  Serial.println(F("  events [n]             recent event log"));
  Serial.println(F("  sync <id> <unix>       TIME_SYNC"));
  Serial.println(F("  config <id>            push default profile"));
  Serial.println(F("  statusreq <id>         ask for a heartbeat now"));
  Serial.println(F("  abort <id> <feed_id>   ABORT"));
  Serial.println(F("  stats                  link counters"));
}

static void print_mac(const uint8_t mac[6]) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                mac[5]);
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

static void sched_push(uint8_t node_id) {
  aio::HubNode *n = reg.by_id(node_id);
  if (!n) { Serial.println(F("no such node")); return; }
  Serial.println(eng.send_schedule_set(node_id, ++reg.data().sched_version,
                                       n->sched.entries, n->sched.entry_count,
                                       n->sched.skip_until, n->sched.hold_until,
                                       millis())
                     ? F("SCHEDULE_SET sent")
                     : F("send failed"));
}

static void cli_sched(char *id, char *sub, char *a1, char *a2, char *a3) {
  if (!id) { Serial.println(F("usage: sched <id> list|add <min> <days> <amt>|del <i>|clear|push")); return; }
  aio::HubNode *n = reg.by_id((uint8_t)atol(id));
  if (!n) { Serial.println(F("no such node")); return; }
  if (!sub || !strcmp(sub, "list")) {
    Serial.printf("node %u sched: enabled=%u entries=%u skip=%lu hold=%lu\n", n->id,
                  n->sched.enabled, n->sched.entry_count,
                  (unsigned long)n->sched.skip_until, (unsigned long)n->sched.hold_until);
    for (uint8_t i = 0; i < n->sched.entry_count && i < aio::HUB_SCHED_MAX_ENTRIES; i++) {
      const sched_entry_t &e = n->sched.entries[i];
      Serial.printf("  [%u] %02u:%02u days=0x%02X amount=%u\n", i, e.min_of_day / 60,
                    e.min_of_day % 60, e.days_mask, e.amount);
    }
  } else if (!strcmp(sub, "add")) {
    if (!a1 || !a2 || !a3) { Serial.println(F("usage: sched <id> add <min_of_day> <days> <amount>")); return; }
    if (n->sched.entry_count >= aio::HUB_SCHED_MAX_ENTRIES) { Serial.println(F("sched full")); return; }
    uint32_t min = parse_num(a1), days = parse_num(a2), amt = parse_num(a3);
    if (min >= 1440 || !days || (days & 0x7F) != days || !amt) {
      Serial.println(F("bad values (min<1440, days 0x01-0x7F, amt>0)"));
      return;
    }
    n->sched.entries[n->sched.entry_count++] = {(uint16_t)min, (uint8_t)days, (uint16_t)amt};
    n->sched.version = ++reg.data().sched_version;
    n->sched.pushed_at = (uint32_t)hub_unix_now();
    reg.save(nvs);
    Serial.println(F("entry added (use 'sched push' to send to the node)"));
  } else if (!strcmp(sub, "del")) {
    if (!a1) { Serial.println(F("usage: sched <id> del <index>")); return; }
    uint8_t i = (uint8_t)atol(a1);
    if (i >= n->sched.entry_count) { Serial.println(F("bad index")); return; }
    memmove(&n->sched.entries[i], &n->sched.entries[i + 1],
            (n->sched.entry_count - i - 1) * sizeof(sched_entry_t));
    n->sched.entry_count--;
    n->sched.version = ++reg.data().sched_version;
    reg.save(nvs);
    Serial.println(F("entry removed"));
  } else if (!strcmp(sub, "clear")) {
    n->sched.entry_count = 0;
    n->sched.version = ++reg.data().sched_version;
    memset(n->run.last_done, 0, sizeof(n->run.last_done));
    reg.save(nvs);
    Serial.println(F("schedule cleared"));
  } else if (!strcmp(sub, "push")) {
    sched_push((uint8_t)atol(id));
  } else {
    Serial.println(F("unknown sched subcommand"));
  }
}

static void cli_events(char *a1) {
  uint8_t show = a1 ? (uint8_t)atol(a1) : 10;
  if (show > evlog.count()) show = evlog.count();
  Serial.printf("event log: %u entries (showing %u newest)\n", evlog.count(), show);
  for (uint16_t i = 0; i < show; i++) {
    const aio::HubEvent *e = evlog.at(evlog.count() - 1 - i);
    Serial.printf("  %lu node=%u %s result=%u id=%lu amt=%u aux=%d\n",
                  (unsigned long)e->unix, e->node, ev_kind_name(e->kind), e->result,
                  (unsigned long)e->feed_id, e->amount, e->aux);
  }
}

static void cli_poll() {
  static char line[160];
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
    char *a4 = strtok(nullptr, " ");
    uint32_t now = millis();
    const int64_t unix_now = hub_unix_now();

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
      hub_help();
    } else if (!strcmp(cmd, "info")) {
      uint8_t mac[6];
      WiFi.macAddress(mac);
      Serial.print(F("hub mac="));
      print_mac(mac);
      Serial.printf(" proto=v%d chan=%u peers=%u stagger=%u\n", PROTO_VER, eng.channel(),
                    eng.peer_count(), reg.data().stagger_max);
      Serial.printf("inflight=%u sched_entries_total=%u\n", core.fm().inflight_count(),
                    reg.count());
    } else if (!strcmp(cmd, "time")) {
      Serial.printf("unix=%lld src=%s tz=%u valid=%d\n", (long long)unix_now,
                    time_source_name(), reg.data().tz_rule, (int)time_valid());
      if (time_valid()) {
        const aio::LocalTime t = aio::local_from_unix(unix_now, reg.data().tz_rule);
        Serial.printf("local min_of_day=%u wday=%u day_start=%lld\n", t.min_of_day,
                      t.wday, (long long)t.day_start);
      }
    } else if (!strcmp(cmd, "tz")) {
      if (!a1) { Serial.println(F("usage: tz <0=UTC 1=Manila 2=CET 3=US-East>")); continue; }
      uint32_t r = parse_num(a1);
      if (r >= aio::TZ_RULE_COUNT) { Serial.println(F("bad rule")); continue; }
      reg.data().tz_rule = (uint8_t)r;
      reg.save(nvs);
      Serial.printf("tz rule=%u\n", r);
    } else if (!strcmp(cmd, "stagger")) {
      if (!a1) { Serial.println(F("usage: stagger <1-8>")); continue; }
      uint32_t n = parse_num(a1);
      if (!n || n > aio::HUB_MAX_NODES) { Serial.println(F("bad n")); continue; }
      reg.data().stagger_max = (uint8_t)n;
      core.fm().set_stagger((uint8_t)n);
      reg.save(nvs);
      Serial.printf("stagger_max=%u\n", n);
    } else if (!strcmp(cmd, "events")) {
      cli_events(a1);
    } else if (!strcmp(cmd, "sched")) {
      cli_sched(a1, a2, a3, a4, nullptr);
    } else if (!strcmp(cmd, "chan")) {
      if (!a1) { Serial.println(F("usage: chan <1-13>")); continue; }
      uint32_t ch = atol(a1);
      if (ch < 1 || ch > 13) { Serial.println(F("bad channel")); continue; }
      eng.set_channel((uint8_t)ch);
      nowt.set_channel((uint8_t)ch);
      Serial.printf("chan=%u (HELLO_ACK advertises this)\n", ch);
    } else if (!strcmp(cmd, "peer") && a1 && !strcmp(a1, "add")) {
      uint8_t mac[6], key[32];
      uint8_t klen = 0;
      uint32_t id = a3 ? (uint32_t)atol(a3) : 0;
      if (!a2 || !a3 || !a4 || !parse_mac(a2, mac) || !parse_key(a4, key, &klen) || id == 0 ||
          id > 255) {
        Serial.println(F("usage: peer add <mac> <id> <keyhex>"));
        continue;
      }
      if (reg.by_id((uint8_t)id) || reg.by_mac(mac)) { Serial.println(F("node already registered")); continue; }
      aio::HubNode *n = reg.add(mac, eng.channel());
      if (!n) { Serial.println(F("registry full")); continue; }
      n->id = (uint8_t)id;
      if (reg.data().next_node_id <= n->id) reg.data().next_node_id = (uint8_t)(n->id + 1);
      memcpy(n->key, key, 32);
      n->key_len = klen;
      reg.save(nvs);
      Serial.println(eng.add_peer(mac, (uint8_t)id, key, klen) ? F("peer added + persisted")
                                                               : F("link peer table full"));
    } else if (!strcmp(cmd, "peer") && a1 && !strcmp(a1, "del")) {
      if (!a2) { Serial.println(F("usage: peer del <id>")); continue; }
      uint8_t id = (uint8_t)atol(a2);
      aio::HubNode *n = reg.by_id(id);
      if (!n) { Serial.println(F("no such node")); continue; }
      eng.remove_peer(n->mac);
      reg.remove(id);
      nvs.del(aio::FeedManager::pend_key_for(id));
      reg.save(nvs);
      Serial.println(F("peer removed"));
    } else if (!strcmp(cmd, "peers")) {
      for (uint8_t i = 0; i < eng.peer_count(); i++) {
        aio::LinkPeer *p = eng.peer_by_index(i);
        Serial.print(F("  id="));
        Serial.print(p->node_id);
        Serial.print(F(" mac="));
        print_mac(p->mac);
        Serial.printf(" key_len=%u", p->key_len);
        if (p->last_hb_ms)
          Serial.printf(" last_hb=%us ago vin=%u day=%u flags=0x%02X",
                        (unsigned)((now - p->last_hb_ms) / 1000), p->last_hb.vin_mv,
                        p->last_hb.day_units, p->last_hb.flags);
        Serial.println();
      }
      if (!eng.peer_count()) Serial.println(F("  (none)"));
    } else if (!strcmp(cmd, "feed")) {
      if (!a1 || !a2) { Serial.println(F("usage: feed <node_id> <amount>")); continue; }
      if (!time_valid()) Serial.println(F("note: clock invalid — manual feed still allowed (SR7)"));
      bool ok = core.request_manual((uint8_t)atol(a1), (uint16_t)atol(a2), now,
                                    unix_now > 0 ? unix_now : 0);
      Serial.println(ok ? F("feed submitted (feed_manager)") : F("feed refused (no node / busy)"));
    } else if (!strcmp(cmd, "feedloop")) {
      if (!a1 || !a2) { Serial.println(F("usage: feedloop <node_id> <count>")); continue; }
      loop_node = (uint8_t)atol(a1);
      loop_target = (uint32_t)atol(a2);
      loop_sent = 0;
      loop_ok = 0;
      loop_dup_errors = 0;
      loop_last_feed_id = 0;
      loop_next_ms = now;
      memset(seen_results, 0, sizeof(seen_results));
      Serial.printf("feedloop start: node=%u n=%u (bench radio mode on the node!)\n",
                    loop_node, loop_target);
    } else if (!strcmp(cmd, "sync")) {
      if (!a1 || !a2) { Serial.println(F("usage: sync <node_id> <unix>")); continue; }
      time_sync_payload_t t{(uint32_t)atol(a2), reg.data().tz_rule};
      Serial.println(eng.send_time_sync((uint8_t)atol(a1), t, now) ? F("TIME_SYNC sent")
                                                                    : F("send failed"));
    } else if (!strcmp(cmd, "config")) {
      if (!a1) { Serial.println(F("usage: config <node_id>")); continue; }
      aio::HubNode *n = reg.by_id((uint8_t)atol(a1));
      node_profile_t p = n ? n->profile : profile_default();
      Serial.println(eng.send_config((uint8_t)atol(a1), p, 1, now) ? F("CONFIG sent")
                                                                    : F("send failed"));
    } else if (!strcmp(cmd, "statusreq")) {
      if (!a1) { Serial.println(F("usage: statusreq <node_id>")); continue; }
      Serial.println(eng.send_status_req((uint8_t)atol(a1), now) ? F("STATUS_REQ sent")
                                                                  : F("send failed"));
    } else if (!strcmp(cmd, "abort")) {
      if (!a1 || !a2) { Serial.println(F("usage: abort <node_id> <feed_id>")); continue; }
      Serial.println(eng.send_abort((uint8_t)atol(a1), (uint32_t)atol(a2), now)
                         ? F("ABORT sent")
                         : F("send failed"));
    } else if (!strcmp(cmd, "stats")) {
      const aio::LinkStats &s = eng.stats();
      Serial.printf("tx=%u tx_fail=%u rx_ok=%u rx_dup=%u rx_replay=%u\n", s.tx, s.tx_fail,
                    s.rx_ok, s.rx_dup, s.rx_replay);
      Serial.printf("bad_len=%u bad_ver=%u bad_mac=%u no_peer=%u test_rx=%u\n", s.rx_bad_len,
                    s.rx_bad_ver, s.rx_bad_mac, s.rx_no_peer, s.test_rx);
      Serial.printf("feed_retries=%u feed_unknown=%u results_deduped=%u inflight=%u\n",
                    s.feed_retries, s.feed_unknown, s.results_deduped,
                    core.fm().inflight_count());
      Serial.printf("events: ok=%u rej=%u unknown=%u missed=%u\n", evlog.count_of(aio::EV_FEED_OK),
                    evlog.count_of(aio::EV_FEED_REJ), evlog.count_of(aio::EV_FEED_UNKNOWN),
                    evlog.count_of(aio::EV_SKIPPED_MISSED));
    } else {
      Serial.print(F("unknown: "));
      Serial.println(cmd);
      hub_help();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.printf("%s boot (M3 bench core) — proto v%d\n", AIO_FIRMWARE_NAME, PROTO_VER);

  if (!nvs.begin()) Serial.println(F("NVS init FAILED (persistence off)"));
  if (!nowt.begin(HUB_CHAN_DEFAULT)) Serial.println(F("ESP-NOW init FAILED"));
  eng.begin(aio::LinkEngine::Role::Hub, &nowt, make_handlers(), millis());
  eng.set_self(aio::LINK_HUB_ID, 0);
  eng.set_channel(HUB_CHAN_DEFAULT);

  if (!reg.load(nvs)) {
    Serial.println(F("registry: fresh (no valid blob)"));
    reg.reset();
  }
  /* registry → link peers */
  for (int i = 0; i < aio::HUB_MAX_NODES; i++) {
    aio::HubNode *n = reg.node(i);
    if (!n || !n->id) continue;
    eng.add_peer(n->mac, n->id, n->key, n->key_len);
  }

  time_begin();
  core.begin(&reg, &link_adapter, &nvs, &evlog, hub_unix_now());

  Serial.printf("registry nodes=%u tz=%u stagger=%u feed_id_seq=%lu\n", reg.count(),
                reg.data().tz_rule, reg.data().stagger_max,
                (unsigned long)reg.data().feed_id_seq);
  Serial.println(F("ready — 'help' for commands"));
  hub_help();
}

void loop() {
  uint32_t now = millis();
  nowt.pump(eng, now);
  eng.tick(now);
  core.tick(now, hub_unix_now());
  cli_poll();

  /* feedloop driver: one feed in flight at a time (§9.5) — M2 bench path,
   * deliberately bypassing feed_manager so the radio loopback measures the
   * link alone. */
  if (loop_target && loop_sent < loop_target && !eng.feed_pending() &&
      (int32_t)(now - loop_next_ms) >= 0) {
    feed_payload_t f{++reg.data().feed_id_seq, 1, UNIT_GRAMS, 0};
    if (eng.send_feed(loop_node, f, now)) {
      loop_sent++;
      loop_next_ms = now + 20; /* next after this resolves (or times out) */
    } else {
      loop_next_ms = now + 100;
    }
  }
  if (loop_target && loop_sent >= loop_target && !eng.feed_pending()) {
    Serial.printf("feedloop done: sent=%u ok=%u dup_errors=%u (expect sent==ok, 0 dups)\n",
                  loop_sent, loop_ok, loop_dup_errors);
    loop_target = 0;
  }
  delay(1);
}
