/*
 * AIO Feeder — in-firmware sim nodes for the QEMU screenshot build
 * (PLAN.md §13, D25; CONFIG_AIO_UI_SHOT).
 *
 * Two live "feeder nodes" run inside the hub firmware as real LinkEngine
 * instances in the node role, joined to the hub engine by an in-memory
 * loopback transport. Everything on the wire is a genuine M2/M3 frame —
 * HMAC'd, seq'd, deduped — so the dashboard under QEMU shows what the real
 * link would show: heartbeats, feed ACKs and results. A third registry node
 * stays silent to exercise the offline/alert states.
 *
 * Virtual clock: node engines run at ACCEL× real time (10×), so the first
 * heartbeat lands 200 ms after boot and every 3 s after that; the hub side
 * keeps its real clock. Wall clock is pinned to Sunday 2026-11-01 07:20
 * Asia/Manila so screenshots are deterministic (next scheduled feed 07:30).
 *
 * Single-include header: defines objects (only ever included by main.cpp).
 * All entry points are called under the hub state mutex from the 1 ms loop.
 */
#ifndef AIO_SIM_NODES_H
#define AIO_SIM_NODES_H

#include <sys/time.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../hub/src/event_log.h"
#include "../../hub/src/hub_registry.h"
#include "../../hub/src/hub_time.h"
#include "../../hub/src/ui_model.h"
#include "../../link_core/link.h"

namespace aio {
namespace sim {

constexpr int SIM_NODES = 2;
constexpr uint32_t ACCEL = 10; /* node virtual clock = real × ACCEL */

constexpr uint8_t SIM_HUB_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
constexpr uint8_t SIM_NODE_MAC[SIM_NODES][6] = {
    {0x02, 0x00, 0x00, 0x00, 0x00, 0x11},
    {0x02, 0x00, 0x00, 0x00, 0x00, 0x12},
};
constexpr uint8_t SIM_OFFLINE_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x13};
/* pairing candidate (M4b shot): unpaired node beaconing PAIR_REQ (§6.3.2) */
constexpr uint8_t SIM_PAIR_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x14};

constexpr size_t SIM_MAX_FRAME = FRAME_HEADER_SIZE + FRAME_MAX_PAYLOAD + FRAME_MAC_SIZE;

/* --- loopback rings ------------------------------------------------------------- */

struct SimFrame {
  uint8_t src[6];
  uint8_t len;
  uint8_t data[SIM_MAX_FRAME];
};

struct SimRing {
  /* 3 slots: frames are drained every 1 ms loop iteration; the ring only
   * bridges tick-to-tick bursts. Kept small — the QEMU build has no PSRAM
   * and the static rings share internal DRAM with the LVGL framebuffer
   * (4 slots overflowed dram0_0_seg when the pairing screen landed; the
   * pair beacon below is phase-shifted so bursts stay ≤ 2 frames). */
  static constexpr uint8_t CAP = 3;
  SimFrame f[CAP];
  uint8_t head = 0, tail = 0;
  bool push(const uint8_t src[6], const uint8_t *data, size_t len) {
    if (len > SIM_MAX_FRAME) return false;
    const uint8_t next = (uint8_t)((head + 1) % CAP);
    if (next == tail) return false; /* drop newest on overflow */
    memcpy(f[head].src, src, 6);
    memcpy(f[head].data, data, len);
    f[head].len = (uint8_t)len;
    head = next;
    return true;
  }
  bool pop(SimFrame *out) {
    if (tail == head) return false;
    *out = f[tail];
    tail = (uint8_t)((tail + 1) % CAP);
    return true;
  }
};

static SimRing g_to_hub;             /* node → hub */
static SimRing g_to_node[SIM_NODES]; /* hub → node i */

/* node-role transport: everything lands in the hub's ring */
class NodeSideTransport : public ILinkTransport {
 public:
  explicit NodeSideTransport(int idx) : idx_(idx) {}
  bool send(const uint8_t mac[6], const uint8_t *data, size_t len) override {
    (void)mac; /* node engines have exactly one peer: the hub */
    return g_to_hub.push(SIM_NODE_MAC[idx_], data, len);
  }
  void set_channel(uint8_t channel) override { (void)channel; }
  uint8_t channel() const override { return 1; }

 private:
  int idx_;
};

/* hub-role transport: route by destination mac (broadcast → all nodes) */
class HubSideTransport : public ILinkTransport {
 public:
  bool send(const uint8_t mac[6], const uint8_t *data, size_t len) override {
    bool ok = true;
    for (int i = 0; i < SIM_NODES; i++) {
      if (!memcmp(mac, SIM_NODE_MAC[i], 6) || !memcmp(mac, LINK_BROADCAST, 6))
        ok &= g_to_node[i].push(SIM_HUB_MAC, data, len);
    }
    return ok;
  }
  void set_channel(uint8_t channel) override { (void)channel; }
  uint8_t channel() const override { return 1; }
};

static HubSideTransport g_hub_tr;

inline ILinkTransport *hub_transport() { return &g_hub_tr; }

/* --- sim nodes ------------------------------------------------------------------ */

static uint32_t g_virt_ms = 0;
static int64_t g_wall_unix = 0;

struct SimNode {
  LinkEngine eng;
  uint8_t id = 0;
  uint16_t vin_mv = 12000;
  uint16_t day_units = 0;
  bool pending = false;
  uint32_t feed_id = 0;
  uint16_t amount = 0;
  uint32_t done_vms = 0;

  void on_feed(const feed_payload_t &f) {
    eng.send_feed_ack(f.feed_id, /*accepted=*/1, /*reason=*/0, g_virt_ms);
    printf("[sim-node %u] FEED id=%u amount=%u\n", id, (unsigned)f.feed_id, f.amount);
    if (!pending) {
      pending = true;
      feed_id = f.feed_id;
      amount = f.amount;
      done_vms = g_virt_ms + 400; /* simulated dispense (virtual ms) */
    }
  }

  void tick(uint32_t vms) {
    if (pending && (int32_t)(vms - done_vms) >= 0) {
      eng.send_feed_result(feed_id, FEED_OK, /*actual_on_ms=*/400, /*pulses=*/0,
                           /*gate_ok=*/1, (uint32_t)g_wall_unix, vms);
      printf("[sim-node %u] FEED_RESULT id=%u OK\n", id, (unsigned)feed_id);
      day_units = (uint16_t)(day_units + amount);
      pending = false;
    }
    eng.tick(vms);
  }
};

static SimNode g_nodes[SIM_NODES];
static NodeSideTransport g_node_tr[SIM_NODES] = {NodeSideTransport(0), NodeSideTransport(1)};
static uint8_t g_keys[SIM_NODES][32];

template <int N>
static void th_on_feed(const uint8_t mac[6], const feed_payload_t &f) {
  (void)mac;
  g_nodes[N].on_feed(f);
}

template <int N>
static void th_hb_fill(heartbeat_payload_t *pl) {
  memset(pl, 0, sizeof(*pl));
  pl->uptime_ms = g_virt_ms;
  pl->vin_mv = g_nodes[N].vin_mv;
  pl->day_units = g_nodes[N].day_units;
  pl->flags = 0; /* hub answers with STATUS_REQ + TIME_SYNC → link stays warm */
  pl->rtc_drift_ms = 0;
}

template <int N>
static void th_schedule_set(const schedule_hdr_t &hdr, const sched_entry_t *) {
  g_nodes[N].eng.send_schedule_ack(hdr.version, 1, g_virt_ms);
}

template <int N>
static void th_config(const node_profile_t &p, uint8_t) {
  (void)p;
  g_nodes[N].eng.send_config_ack(0x1234, 1, g_virt_ms);
}

/* --- pairing candidate ------------------------------------------------------------ */

/* An unpaired node that never pairs: instead of a third LinkEngine (~770 B
 * of static DRAM the QEMU build does not have), it hand-builds a genuine
 * PAIR_REQ every 1 s of virtual time — real frame layout, zero-key HMAC via
 * frame_compute_mac — and pushes it straight into the hub ring. The hub's
 * PAIR_ACCEPT (unicast) is dropped by the hub-side transport, exactly like
 * a node whose user never confirmed. Beacon phase 350 ms so it never lands
 * in the same 1 ms iteration as the two live nodes' heartbeats (which burst
 * together every 3 s virtual): ring bursts stay ≤ 2 frames. */
static void pair_tick(uint32_t vms) {
  static uint32_t next_vms = 350;
  if ((int32_t)(vms - next_vms) < 0) return;
  next_vms = vms + 1000;
  pair_req_payload_t pr{};
  pr.fw_ver = 0x0103;
  pr.caps = 0x01;
  const uint8_t nonce[8] = {0xA1, 0x55, 0x00, 0x0B, 0x5E, 0xED, 0x01, 0x42};
  memcpy(pr.node_nonce, nonce, 8);
  uint8_t buf[FRAME_HEADER_SIZE + sizeof(pr) + FRAME_MAC_SIZE];
  buf[0] = PROTO_VER;
  buf[1] = MSG_PAIR_REQ;
  buf[2] = 0; /* node_id 0: unassigned */
  buf[3] = 0;
  buf[4] = (uint8_t)(vms & 0xFF);
  buf[5] = (uint8_t)(vms >> 8);
  buf[6] = (uint8_t)sizeof(pr);
  buf[7] = 0;
  memcpy(buf + FRAME_HEADER_SIZE, &pr, sizeof(pr));
  frame_compute_mac(AIO_ZERO_KEY, buf, FRAME_HEADER_SIZE + sizeof(pr),
                    buf + FRAME_HEADER_SIZE + sizeof(pr));
  g_to_hub.push(SIM_PAIR_MAC, buf, sizeof(buf));
}

static void begin_nodes() {
  for (int i = 0; i < SIM_NODES; i++) {
    LinkHandlers h{};
    h.on_feed = (i == 0) ? &th_on_feed<0> : &th_on_feed<1>;
    h.hb_fill = (i == 0) ? &th_hb_fill<0> : &th_hb_fill<1>;
    h.on_schedule_set = (i == 0) ? &th_schedule_set<0> : &th_schedule_set<1>;
    h.on_config = (i == 0) ? &th_config<0> : &th_config<1>;
    g_nodes[i].id = (uint8_t)(i + 1);
    g_nodes[i].eng.begin(LinkEngine::Role::Node, &g_node_tr[i], h, 0);
    g_nodes[i].eng.set_self(g_nodes[i].id, 1 /* paired beacon state */);
    g_nodes[i].eng.add_peer(SIM_HUB_MAC, LINK_HUB_ID, g_keys[i], 32);
  }
  printf("sim: %d live node(s) started (accel x%u)\n", SIM_NODES, (unsigned)ACCEL);
  printf("sim: pairing candidate %02X:%02X:%02X:%02X:%02X:%02X beacons PAIR_REQ\n",
         SIM_PAIR_MAC[0], SIM_PAIR_MAC[1], SIM_PAIR_MAC[2], SIM_PAIR_MAC[3],
         SIM_PAIR_MAC[4], SIM_PAIR_MAC[5]);
}

/* --- seeding --------------------------------------------------------------------- */

/* Pin the wall clock and seed registry + history so the dashboard shows a
 * deterministic, realistic scene. Called before core.begin() (boot scan). */
static void seed(HubRegistry &reg, EventLog &evlog, aio::ui::AlertsAck &ack) {
  /* Sunday 2026-11-01 07:20:00 Asia/Manila (next feed: 07:30, 10 min out) */
  const int64_t unix =
      aio::days_from_civil(2026, 11, 1) * 86400 + (7 * 3600 + 20 * 60) - 8 * 3600;
  const struct timeval tv = {(time_t)unix, 0};
  settimeofday(&tv, nullptr);
  g_wall_unix = unix;

  struct Def {
    const uint8_t *mac;
    uint8_t keyc;
    uint16_t min_of_day;
    uint16_t amount;
    bool live;
  };
  static const Def defs[SIM_NODES + 1] = {
      {SIM_NODE_MAC[0], 0xA5, 450, 40, true},  /* 07:30 @ 40 g daily */
      {SIM_NODE_MAC[1], 0x5A, 455, 25, true},  /* 07:35 @ 25 g daily */
      {SIM_OFFLINE_MAC, 0x66, 410, 30, false}, /* 06:50 @ 30 g daily */
  };

  reg.reset();
  reg.data().tz_rule = TZ_MANILA;
  const int64_t yday_start = local_from_unix(unix, TZ_MANILA).day_start - 86400;
  for (int i = 0; i < SIM_NODES + 1; i++) {
    HubNode *n = reg.add(defs[i].mac, 1);
    if (!n) continue;
    memset(n->key, defs[i].keyc, 32);
    n->key_len = 32;
    n->sched.entry_count = 1;
    n->sched.entries[0].min_of_day = defs[i].min_of_day;
    n->sched.entries[0].days_mask = 0x7F;
    n->sched.entries[0].amount = defs[i].amount;
    n->sched.pushed_at = (uint32_t)(unix - 3 * 3600); /* "pushed 3 h ago" */
    /* mark every scheduled occurrence through today handled → the engine's
     * boot scan raises no catch-up/missed-feed alerts of its own, so the
     * curated event log below is the whole deterministic scene (the shot
     * run covers ~2 min of pinned-clock time, well before the 07:30 feed) */
    for (uint8_t e = 0; e < n->sched.entry_count && e < HUB_SCHED_MAX_ENTRIES; e++) {
      n->run.last_done[e].day_start = (uint32_t)yday_start + 86400;
      n->run.last_done[e].min_of_day = 1439;
    }
    if (i < SIM_NODES) memcpy(g_keys[i], n->key, 32);
  }
  /* node 1 gets a second daily entry (17:30 @ 25 g) for a fuller detail shot */
  {
    HubNode *n = reg.by_id(1);
    if (n) {
      n->sched.entries[1].min_of_day = 17 * 60 + 30;
      n->sched.entries[1].days_mask = 0x7F;
      n->sched.entries[1].amount = 25;
      n->sched.entry_count = 2;
      n->run.last_done[1].day_start = (uint32_t)yday_start + 86400;
      n->run.last_done[1].min_of_day = 1439;
    }
  }
  /* node 2: skip gate active for the next 2 h (detail gate banner state) */
  {
    HubNode *n = reg.by_id(2);
    if (n) n->sched.skip_until = (uint32_t)(unix + 2 * 3600);
  }

  /* history: #1 fed OK 40 min ago + 5 h ago, #2 fed OK 26 h ago, plus a
   * week of daily OK feeds for a non-empty 7-day chart */
  evlog.push((uint32_t)(unix - 2400), 1, EV_FEED_OK, FEED_OK, 1001, 40, 0);
  evlog.push((uint32_t)(unix - 5 * 3600), 1, EV_FEED_OK, FEED_OK, 1003, 25, 0);
  evlog.push((uint32_t)(unix - 26 * 3600), 2, EV_FEED_OK, FEED_OK, 1002, 25, 0);
  for (int d = 1; d <= 6; d++) {
    evlog.push((uint32_t)(unix - d * 86400 - 600), 1, EV_FEED_OK, FEED_OK,
               (uint32_t)(1100 + d), (uint16_t)(40 + d), 0);
    if (d <= 4)
      evlog.push((uint32_t)(unix - d * 86400 - 1200), 2, EV_FEED_OK, FEED_OK,
                 (uint32_t)(1200 + d), 25, 0);
  }

  /* alerts (§8.7): #1 not confirmed 26 h ago (no link — acked, exercises the
   * acked section), #2 feed failed 3 h ago (aborted mid-run), #3's 06:50
   * feed was missed this morning (skip-policy alert). Oldest first so ring
   * order == time order among the alert rows. */
  evlog.push((uint32_t)(unix - 26 * 3600), 1, EV_FEED_UNKNOWN, EVENT_RESULT_NO_LINK, 1004,
             25, 0);
  evlog.push((uint32_t)(unix - 3 * 3600), 2, EV_FEED_FAIL, FEED_ABORTED, 1005, 40, 0);
  evlog.push((uint32_t)(unix - 1800), 3, EV_SKIPPED_MISSED, 0, 0, 30, 1);
  ack.ack((uint32_t)(unix - 26 * 3600), 1, EV_FEED_UNKNOWN);

  printf("sim: clock pinned to %lld, registry seeded (3 nodes, 2 live)\n",
         (long long)unix);
}

/* --- per-iteration drive (called under the state mutex) -------------------------- */

/* deliver node → hub frames queued since last iteration */
static void pump_to_hub(LinkEngine &hub_eng, uint32_t now_ms) {
  SimFrame f;
  while (g_to_hub.pop(&f)) hub_eng.on_rx(f.src, f.data, f.len, -60, now_ms);
}

/* node engines tick on the virtual clock; deliver hub → node frames */
static void tick_nodes(uint32_t now_ms) {
  const uint32_t vms = now_ms * ACCEL;
  g_virt_ms = vms;
  for (int i = 0; i < SIM_NODES; i++) g_nodes[i].tick(vms);
  pair_tick(vms);
  for (int i = 0; i < SIM_NODES; i++) {
    SimFrame f;
    while (g_to_node[i].pop(&f)) g_nodes[i].eng.on_rx(SIM_HUB_MAC, f.data, f.len, -60, vms);
  }
}

}  // namespace sim
}  // namespace aio

#endif /* AIO_SIM_NODES_H */
