/*
 * AIO Feeder — hub firmware, M2 bench link host (PLAN.md §12 M2).
 *
 * The real hub core (scheduler, storage, UI, feed_manager) is M3/M4; this
 * bench firmware runs the same LinkEngine the production hub will use, over
 * a serial CLI:
 *
 *   peer add <mac> <id> <key>   register a node (key printed by `pair` on it)
 *   feed <id> <amount>          FEED with the §6.6 retry policy
 *   feedloop <id> <n>           the 1000-feed loopback (no duplicates)
 *   hb / stats / chan / sync / sched / config / abort / statusreq
 *
 * Answers HELLO with HELLO_ACK (channel discovery, §6.2). Peer table is RAM
 * for the bench; persistence arrives with hub storage in M3.
 */
#include <Arduino.h>

#include "now_esp32.h"

#include "../../lib/proto/messages.h"
#include "../../lib/proto/profile.h"
#include "../../lib/proto/proto.h"
#include "../../link_core/link.h"

#ifndef AIO_FIRMWARE_NAME
#define AIO_FIRMWARE_NAME "aio-feeder-hub"
#endif

constexpr uint8_t HUB_CHAN_DEFAULT = 1;

static NowTransportEsp32 nowt;
static aio::LinkEngine eng;
static uint8_t hub_chan = HUB_CHAN_DEFAULT;
static uint32_t feed_id_seq = 5000;
static uint16_t sched_version = 1;
static uint32_t loop_start_ms = 0;
static uint32_t loop_target = 0;
static uint32_t loop_sent = 0;
static uint32_t loop_ok = 0;
static uint32_t loop_dup_errors = 0;
static uint32_t loop_last_feed_id = 0;
static uint32_t loop_next_ms = 0;
static uint8_t loop_node = 0;
static uint32_t seen_results[1024];

/* --- engine callbacks -------------------------------------------------------- */

static void cb_hello(const uint8_t mac[6], const hello_payload_t &pl, int8_t rssi) {
  Serial.printf("HELLO from %02X:%02X:%02X:%02X:%02X:%02X fw=%u state=%u rssi=%d%s\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], pl.fw_ver, pl.state, rssi,
                pl.state == 0 ? " (unpaired: peer add this mac)" : "");
}

static void cb_feed_ack(uint8_t node_id, const feed_ack_payload_t &a) {
  Serial.printf("FEED_ACK node=%u id=%u accepted=%u reason=%u\n", node_id, a.feed_id,
                a.accepted, a.reason);
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
}

static void cb_heartbeat(uint8_t node_id, const heartbeat_payload_t &hb, int8_t rssi) {
  Serial.printf("HB node=%u up=%us vin=%u day=%u flags=0x%02X drift=%d rssi=%d\n", node_id,
                (unsigned)(hb.uptime_ms / 1000), hb.vin_mv, hb.day_units, hb.flags,
                hb.rtc_drift_ms, rssi);
  /* Ack non-reply heartbeats with STATUS_REQ so the node's freshness window
   * (§6.2 scan trigger) stays fed on an otherwise quiet link. The node's
   * flagged reply is not re-acked (frame flags bit0). */
  aio::LinkPeer *p = eng.peer_by_node(node_id);
  if (p && p->last_flags == 0) eng.send_status_req(node_id, millis());
}

static void cb_pair_req(const uint8_t mac[6], const pair_req_payload_t &pr, int8_t rssi) {
  Serial.printf("PAIR_REQ from %02X:%02X:%02X:%02X:%02X:%02X fw=%u (pairing UI is M3)\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], pr.fw_ver);
}

static void cb_feed_unknown(uint32_t feed_id, uint8_t node_id) {
  Serial.printf("FEED UNKNOWN id=%u node=%u (alert: never re-issued, §6.6)\n", feed_id,
                node_id);
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

static void hub_help() {
  Serial.println(F("AIO feeder hub CLI (M2 bench link)"));
  Serial.println(F("  info                   mac, channel, counters"));
  Serial.println(F("  chan <1-13>            set ESP-NOW channel"));
  Serial.println(F("  peer add <mac> <id> <keyhex>"));
  Serial.println(F("  peer del <id>"));
  Serial.println(F("  peers                  list peers + last heartbeat"));
  Serial.println(F("  feed <id> <amount>     one FEED (retry policy §6.6)"));
  Serial.println(F("  feedloop <id> <n>      1000-feed loopback, reports duplicates"));
  Serial.println(F("  sync <id> <unix>       TIME_SYNC"));
  Serial.println(F("  sched <id> <skip> <hold>  SCHEDULE_SET (1 sample entry)"));
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

static void cli_poll() {
  static char line[128];
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

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
      hub_help();
    } else if (!strcmp(cmd, "info")) {
      uint8_t mac[6];
      WiFi.macAddress(mac);
      Serial.print(F("hub mac="));
      print_mac(mac);
      Serial.printf(" proto=v%d chan=%u peers=%u\n", PROTO_VER, eng.channel(),
                    eng.peer_count());
    } else if (!strcmp(cmd, "chan")) {
      if (!a1) { Serial.println(F("usage: chan <1-13>")); continue; }
      uint32_t ch = atol(a1);
      if (ch < 1 || ch > 13) { Serial.println(F("bad channel")); continue; }
      hub_chan = (uint8_t)ch;
      eng.set_channel(hub_chan);
      nowt.set_channel(hub_chan);
      Serial.printf("chan=%u (HELLO_ACK advertises this)\n", hub_chan);
    } else if (!strcmp(cmd, "peer") && a1 && !strcmp(a1, "add")) {
      uint8_t mac[6], key[32];
      uint8_t klen = 0;
      uint32_t id = a3 ? (uint32_t)atol(a3) : 0;
      if (!a2 || !a3 || !a4 || !parse_mac(a2, mac) || !parse_key(a4, key, &klen) || id == 0 ||
          id > 255) {
        Serial.println(F("usage: peer add <mac> <id> <keyhex>"));
        continue;
      }
      aio::LinkPeer *p = eng.add_peer(mac, (uint8_t)id, key, klen);
      Serial.println(p ? F("peer added") : F("peer table full"));
    } else if (!strcmp(cmd, "peer") && a1 && !strcmp(a1, "del")) {
      uint8_t mac[6];
      if (!a2) {
        Serial.println(F("usage: peer del <id|mac>"));
        continue;
      }
      if (!parse_mac(a2, mac)) {
        aio::LinkPeer *p = eng.peer_by_node((uint8_t)atol(a2));
        if (!p) { Serial.println(F("no such peer")); continue; }
        memcpy(mac, p->mac, 6);
      }
      Serial.println(eng.remove_peer(mac) ? F("peer removed") : F("no such peer"));
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
      feed_payload_t f{++feed_id_seq, (uint16_t)atol(a2), UNIT_GRAMS, 0};
      Serial.printf("FEED node=%s id=%u amount=%u\n", a1, f.feed_id, f.amount);
      if (!eng.send_feed((uint8_t)atol(a1), f, now)) Serial.println(F("feed refused (no peer / busy)"));
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
      time_sync_payload_t t{(uint32_t)atol(a2), 0};
      Serial.println(eng.send_time_sync((uint8_t)atol(a1), t, now) ? F("TIME_SYNC sent")
                                                                    : F("send failed"));
    } else if (!strcmp(cmd, "sched")) {
      if (!a1) { Serial.println(F("usage: sched <node_id> [skip_unix] [hold_unix]")); continue; }
      sched_entry_t e{8 * 60, 0x7F, 20}; /* 08:00 daily, 20 g sample */
      uint32_t skip = a2 ? (uint32_t)atol(a2) : 0;
      uint32_t hold = a3 ? (uint32_t)atol(a3) : 0;
      Serial.println(eng.send_schedule_set((uint8_t)atol(a1), ++sched_version, &e, 1, skip,
                                           hold, now)
                         ? F("SCHEDULE_SET sent")
                         : F("send failed"));
    } else if (!strcmp(cmd, "config")) {
      if (!a1) { Serial.println(F("usage: config <node_id>")); continue; }
      node_profile_t p = profile_default();
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
      Serial.printf("feed_retries=%u feed_unknown=%u results_deduped=%u pending=%u\n",
                    s.feed_retries, s.feed_unknown, s.results_deduped,
                    eng.feed_pending() ? 1u : 0u);
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
  Serial.printf("%s boot (M2 bench link) — proto v%d\n", AIO_FIRMWARE_NAME, PROTO_VER);
  if (!nowt.begin(hub_chan)) Serial.println(F("ESP-NOW init FAILED"));
  eng.begin(aio::LinkEngine::Role::Hub, &nowt, make_handlers(), millis());
  eng.set_self(aio::LINK_HUB_ID, 0);
  eng.set_channel(hub_chan);
  Serial.println(F("ready — 'help' for commands"));
  hub_help();
}

void loop() {
  uint32_t now = millis();
  nowt.pump(eng, now);
  eng.tick(now);
  cli_poll();

  /* feedloop driver: one feed in flight at a time (§9.5) */
  if (loop_target && loop_sent < loop_target && !eng.feed_pending() &&
      (int32_t)(now - loop_next_ms) >= 0) {
    feed_payload_t f{++feed_id_seq, 1, UNIT_GRAMS, 0};
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
