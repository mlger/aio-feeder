/*
 * AIO Feeder — fake feeder node (PLAN.md §13, D25).
 *
 * A host-runnable node speaking the M2/M3 proto over UDP, for bench and
 * (later) QEMU testing: heartbeats every 30 s, answers FEED with
 * FEED_ACK + FEED_RESULT after a simulated dispense, keeps an SR6 journal
 * (duplicate FEEDs are answered, never actuated twice), honors ABORT,
 * TIME_SYNC, SCHEDULE_SET / CONFIG (acks) and STATUS_REQ.
 *
 * Build:  g++ -std=c++17 -Ilib/proto -Ilink_core -Itools/sim \
 *             tools/sim/fake_node.cpp -o build/fake_node
 * Run:    fake_node [--port 9001] [--hub 127.0.0.1:9000] [--id 1]
 *                   [--mac 02:00:00:00:00:02] [--key <32-byte hex>]
 *                   [--on-ms 800] [--max-amount 200] [--min-interval-s 60]
 *
 * On the bench: `tools/sim` pairs with the hub firmware's UDP build or, in
 * the sandbox, with the host e2e suite (test/test_e2e_sim) which embeds the
 * same engine pair over loopback UDP.
 */
#include <getopt.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "link_udp.h"

using namespace aio;

static uint32_t ms_now() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
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
    if (++nib == 2) { out[byte++] = (uint8_t)val; nib = 0; val = 0; }
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
    if (++nib == 2) { buf[byte++] = (uint8_t)val; nib = 0; val = 0; }
  }
  if (nib != 0 || (byte != 32 && byte != 16 && byte != 0)) return false;
  memset(out, 0, 32);
  if (byte) memcpy(out, buf, byte);
  *len = (uint8_t)byte;
  return true;
}

/* --- simulated actuator -------------------------------------------------------- */

static LinkEngine *g_eng = nullptr; /* handlers are plain fns, not capturing lambdas */
static uint32_t sim_unix_now = 0;

struct SimActuator {
  LinkEngine *eng = nullptr;
  uint32_t on_ms = 800;
  uint16_t max_amount = 200;
  uint32_t min_interval_s = 60;

  struct Journal {             /* SR6: duplicates answered, actuated once */
    uint32_t feed_id = 0;
    uint8_t status = 0;
    uint16_t on_ms = 0;
    uint32_t t_unix = 0;
  };
  Journal journal[16];
  uint8_t journal_n = 0;

  struct Dispense { uint32_t feed_id; uint16_t amount; uint32_t done_ms; };
  Dispense pending{};
  uint32_t last_start_ms = 0;
  uint32_t day_units = 0;
  uint32_t unix_now = 0;

  const Journal *journal_find(uint32_t feed_id) const {
    for (uint8_t i = 0; i < journal_n; i++)
      if (journal[i].feed_id == feed_id) return &journal[i];
    return nullptr;
  }

  void on_feed(const uint8_t mac[6], const feed_payload_t &f) {
    const uint32_t now = ms_now();
    printf("[fake-node] FEED id=%u amount=%u unit=%u\n", f.feed_id, f.amount, f.unit);
    const Journal *j = journal_find(f.feed_id);
    if (eng->send_feed_ack(f.feed_id, /*accepted=*/1, /*reason=*/0, now))
      printf("[fake-node]   -> FEED_ACK id=%u accepted=1\n", f.feed_id);
    if (j) {
      /* duplicate: repeat the original result from the journal (SR6) */
      eng->send_feed_result(j->feed_id, j->status, j->on_ms, 0, 1, j->t_unix, now);
      return;
    }
    if (last_start_ms && now - last_start_ms < min_interval_s * 1000U) {
      printf("[fake-node]   -> rejected (min interval)\n");
      eng->send_feed_ack(f.feed_id, 0, /*reason=*/1, now);
      return;
    }
    if (f.amount > max_amount) {
      printf("[fake-node]   -> rejected (amount cap)\n");
      eng->send_feed_ack(f.feed_id, 0, /*reason=*/2, now);
      return;
    }
    last_start_ms = now;
    pending = Dispense{f.feed_id, f.amount, now + on_ms};
  }

  void poll(uint32_t unix) {
    unix_now = sim_unix_now = unix;
    if (pending.feed_id && (int32_t)(ms_now() - pending.done_ms) >= 0) {
      const uint32_t now = ms_now();
      printf("[fake-node]   -> FEED_RESULT id=%u status=OK on_ms=%u\n", pending.feed_id,
             on_ms);
      eng->send_feed_result(pending.feed_id, /*status=*/0, (uint16_t)on_ms,
                            /*pulses=*/0, /*gate_ok=*/1, (uint32_t)unix, now);
      if (journal_n < 16) {
        journal[journal_n++] = Journal{pending.feed_id, 0, (uint16_t)on_ms,
                                       (uint32_t)unix};
      }
      pending.feed_id = 0;
      day_units += pending.amount;
    }
  }
};

static SimActuator act;

static void hb_fill(heartbeat_payload_t *pl) {
  memset(pl, 0, sizeof(*pl));
  pl->uptime_ms = ms_now();
  pl->vin_mv = 12000;
  pl->day_units = (uint16_t)act.day_units;
  pl->flags = 0;
  pl->rtc_drift_ms = 0;
}

int main(int argc, char **argv) {
  uint16_t port = 9001, hub_port = 9000;
  const char *hub_ip = "127.0.0.1";
  const char *hub_arg = nullptr;
  uint8_t id = 1;
  uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
  uint8_t hub_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  uint8_t key[32], klen = 0;

  static option longopts[] = {
      {"port", required_argument, nullptr, 'p'},
      {"hub", required_argument, nullptr, 'H'},
      {"id", required_argument, nullptr, 'i'},
      {"mac", required_argument, nullptr, 'm'},
      {"key", required_argument, nullptr, 'k'},
      {"on-ms", required_argument, nullptr, 'o'},
      {"max-amount", required_argument, nullptr, 'a'},
      {"min-interval-s", required_argument, nullptr, 'M'},
      {nullptr, 0, nullptr, 0},
  };
  int c;
  while ((c = getopt_long(argc, argv, "p:H:i:m:k:o:a:M:", longopts, nullptr)) != -1) {
    switch (c) {
      case 'p': port = (uint16_t)atoi(optarg); break;
      case 'H': hub_arg = optarg; break;
      case 'i': id = (uint8_t)atoi(optarg); break;
      case 'm': if (!parse_mac(optarg, mac)) { fprintf(stderr, "bad --mac\n"); return 2; } break;
      case 'k': if (!parse_key(optarg, key, &klen)) { fprintf(stderr, "bad --key (16/32-byte hex)\n"); return 2; } break;
      case 'o': act.on_ms = (uint32_t)atoi(optarg); break;
      case 'a': act.max_amount = (uint16_t)atoi(optarg); break;
      case 'M': act.min_interval_s = (uint32_t)atoi(optarg); break;
      default: return 2;
    }
  }
  static std::string hub_ip_hold, hub_ip_hold2;
  if (hub_arg) {
    hub_ip_hold = hub_arg;
    auto pos = hub_ip_hold.rfind(':');
    if (pos == std::string::npos) { fprintf(stderr, "bad --hub ip:port\n"); return 2; }
    hub_ip_hold2 = hub_ip_hold.substr(0, pos);
    hub_ip = hub_ip_hold2.c_str();
    hub_port = (uint16_t)atoi(hub_ip_hold.c_str() + pos + 1);
  }
  if (!klen) {
    /* no key given: fixed sim default so standalone use just works */
    memset(key, 0xA5, 32);
    klen = 32;
    printf("[fake-node] note: using default sim key 32 x 0xA5\n");
  }

  LinkEngine eng;
  g_eng = &eng;
  UdpLink link(port, hub_ip, hub_port, hub_mac);
  act.eng = &eng;

  LinkHandlers h{};
  h.on_feed = [](const uint8_t mac[6], const feed_payload_t &f) { act.on_feed(mac, f); };
  h.hb_fill = hb_fill;
  h.on_time_sync = [](const time_sync_payload_t &t) {
    printf("[fake-node] TIME_SYNC unix=%u tz=%u\n", t.unix_time, t.tz_rule);
  };
  h.on_schedule_set = [](const schedule_hdr_t &hdr, const sched_entry_t *entries) {
    printf("[fake-node] SCHEDULE_SET v=%u entries=%u\n", hdr.version, hdr.entry_count);
    g_eng->send_schedule_ack(hdr.version, 1, ms_now());
  };
  h.on_config = [](const node_profile_t &p, uint8_t confirmed) {
    printf("[fake-node] CONFIG confirmed=%u\n", confirmed);
    g_eng->send_config_ack(/*profile_crc=*/0x1234, 1, ms_now());
  };
  h.on_status_req = []() { printf("[fake-node] STATUS_REQ\n"); };
  h.on_abort = [](const abort_payload_t &a) {
    printf("[fake-node] ABORT feed_id=%u\n", a.feed_id);
    act.pending.feed_id = 0;
  };

  eng.begin(LinkEngine::Role::Node, &link, h, ms_now());
  eng.set_self(id, 1 /* paired beacon state */);
  eng.add_peer(hub_mac, LINK_HUB_ID, key, klen);

  printf("[fake-node] id=%u mac=%02X:%02X:%02X:%02X:%02X:%02X port=%u -> hub %s:%u "
         "on_ms=%u max_amount=%u min_interval_s=%u key_len=%u\n",
         id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], port, hub_ip, hub_port,
         act.on_ms, act.max_amount, act.min_interval_s, klen);

  for (;;) {
    const uint32_t now = ms_now();
    link.pump(eng, now);
    eng.tick(now);
    act.poll((uint32_t)time(nullptr));
    usleep(1000);
  }
}
