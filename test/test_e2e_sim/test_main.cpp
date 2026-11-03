/* Host e2e: the full hub pipeline over real loopback UDP sockets to a fake
 * node — HubRegistry + HubCore + LinkEngine (hub role) on one side, the same
 * engine pair as tools/sim/fake_node.cpp (LinkEngine node role + simulated
 * actuator) on the other. PLAN.md §13 fake node, D25.
 *
 * Virtual clock: 1 simulated ms per loop step; localhost UDP delivers
 * datagrams immediately, so retry/repeat timing (§6.6) is exercised in
 * milliseconds of wall time. Covers §9.5 single-flight feed, §9.7 result
 * recording, §6.6 retry/repeat-on-loss and §7.4 TIME_SYNC push.
 */
#include <unity.h>

#include <cstring>

#include "../../hub/src/event_log.h"
#include "../../hub/src/hub_core.h"
#include "../../hub/src/hub_registry.h"
#include "../../hub/src/hub_store.h"
#include "../../hub/src/hub_time.h"
#include "../../link_core/link.h"
#include "link_udp.h"

using namespace aio;

void setUp(void) {}
void tearDown(void) {}

static const uint8_t HUB_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t NODE_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
/* both sides need a key: the engine refuses keyless FEED/RESULT */
static const uint8_t TEST_KEY[32] = {
    0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5,
    0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5,
    0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5,
};

static uint16_t sim_port_base() { /* fresh UDP ports per test */
  static uint16_t p = 9200;
  const uint16_t r = p;
  p = (uint16_t)(p + 8);
  return r;
}

/* ---- storage (same shape as the unit-test RamStore) ---------------------------- */

struct E2eStore : IHubStore {
  uint8_t blob[4096];
  size_t len = 0;
  bool present = false;
  bool put(const char *key, const uint8_t *data, size_t n) override {
    if (n > sizeof(blob)) return false;
    memcpy(blob, data, n);
    len = n;
    present = true;
    return true;
  }
  size_t get(const char *key, uint8_t *out, size_t max_len) override {
    if (!present) return 0;
    size_t n = len < max_len ? len : max_len;
    memcpy(out, blob, n);
    return len;
  }
  bool del(const char *key) override {
    const bool was = present;
    present = false;
    len = 0;
    return was;
  }
};

/* ---- fake node (mirrors tools/sim/fake_node.cpp) -------------------------------- */

struct FakeNode {
  LinkEngine eng;
  UdpLink link;
  uint32_t on_ms = 30; /* dispense time in simulated ms */
  uint16_t max_amount = 200;
  bool drop_results = false; /* e2e hook: lose RESULT repeats 1–2 (§6.6) */

  struct Dispense { uint32_t feed_id; uint16_t amount; uint32_t done_ms; };
  Dispense pending{};
  uint32_t journal[16];
  uint8_t journal_unit[16]; /* UNIT_GRAMS / UNIT_ML the FEED carried */
  uint8_t journal_n = 0;
  uint16_t day_units = 0;
  bool time_invalid_hb = true; /* flag heartbeats until a TIME_SYNC arrives */
  int64_t last_sync_unix = 0;
  uint8_t syncs_seen = 0;
  uint8_t self_id = 1;

  FakeNode(uint16_t port, uint16_t hub_port)
      : link(port, "127.0.0.1", hub_port, HUB_MAC) {}

  void begin(uint32_t now_ms, uint8_t node_id) {
    eng.begin(LinkEngine::Role::Node, &link, handlers(), now_ms);
    self_id = node_id;
    eng.set_self(node_id, node_id);
    eng.add_peer(HUB_MAC, LINK_HUB_ID, TEST_KEY, sizeof(TEST_KEY));
  }

  static void on_feed(const uint8_t *mac, const feed_payload_t &f) {
    FakeNode &n = *g_node;
    n.eng.send_feed_ack(f.feed_id, 1, 0, g_ms);
    if (n.drop_results) {
      /* the next two node→hub datagrams are RESULT repeats 1 and 2 */
      n.link.drop_next();
      n.link.drop_next();
    }
    for (uint8_t i = 0; i < n.journal_n; i++)
      if (n.journal[i] == f.feed_id) return; /* SR6: answered, never re-actuated */
    if (n.journal_n < 16) {
      n.journal[n.journal_n] = f.feed_id;
      n.journal_unit[n.journal_n] = f.unit;
      n.journal_n++;
    }
    n.pending = Dispense{f.feed_id, f.amount, g_ms + n.on_ms};
  }
  static void hb_fill(heartbeat_payload_t *pl) {
    FakeNode &n = *g_node;
    memset(pl, 0, sizeof(*pl));
    pl->uptime_ms = g_ms;
    pl->vin_mv = 12000;
    pl->day_units = n.day_units;
    pl->flags = n.time_invalid_hb ? HB_FLAG_TIME_INVALID : 0;
  }
  static void on_time_sync(const time_sync_payload_t &t) {
    FakeNode &n = *g_node;
    n.last_sync_unix = t.unix_time;
    n.syncs_seen++;
    n.time_invalid_hb = false;
  }
  static LinkHandlers handlers() {
    LinkHandlers h{};
    h.on_feed = on_feed;
    h.hb_fill = hb_fill;
    h.on_time_sync = on_time_sync;
    return h;
  }

  void poll() {
    if (pending.feed_id && (int32_t)(g_ms - pending.done_ms) >= 0) {
      eng.send_feed_result(pending.feed_id, 0, (uint16_t)on_ms, 0, 1,
                           (uint32_t)(g_unix - 8 * 3600), g_ms);
      day_units += pending.amount;
      pending.feed_id = 0;
    }
  }

  static FakeNode *g_node;
  static uint32_t g_ms;
  static int64_t g_unix;
};
FakeNode *FakeNode::g_node = nullptr;
uint32_t FakeNode::g_ms = 0;
int64_t FakeNode::g_unix = 0;

/* ---- hub side ------------------------------------------------------------------- */

struct HubSide {
  E2eStore store;
  EventLog log;
  HubRegistry reg;
  HubCore core;
  LinkEngine eng;
  UdpLink link;

  struct Adapter : IHubLink {
    LinkEngine *e = nullptr;
    bool send_feed(uint8_t node, const feed_payload_t &f, uint32_t now) override {
      return e->send_feed(node, f, now);
    }
  } adapter;

  uint32_t ms_now = 0;
  int64_t unix_now = 0;

  HubSide(uint16_t port, uint16_t node_port)
      : link(port, "127.0.0.1", node_port, NODE_MAC) {
    adapter.e = &eng;
    reg.reset();
    reg.data().tz_rule = TZ_MANILA;
    TEST_ASSERT_NOT_NULL(reg.add(NODE_MAC, 1));
    eng.begin(LinkEngine::Role::Hub, &link, hub_handlers(), 0);
    eng.set_self(LINK_HUB_ID, 0);
    eng.add_peer(NODE_MAC, 1, TEST_KEY, sizeof(TEST_KEY));
  }

  static void cb_feed_ack(uint8_t node_id, const feed_ack_payload_t &a) {
    g_hub->core.on_feed_ack(node_id, a, (uint32_t)g_hub->unix_now);
  }
  static void cb_feed_result(uint8_t node_id, const feed_result_payload_t &r) {
    g_hub->core.on_feed_result(node_id, r, (uint32_t)g_hub->unix_now);
  }
  static void cb_feed_unknown(uint32_t feed_id, uint8_t node_id) {
    g_hub->core.on_feed_unknown(feed_id, node_id, (uint32_t)g_hub->unix_now);
  }
  static void cb_heartbeat(uint8_t node_id, const heartbeat_payload_t &hb, uint32_t rel_ver,
                           uint8_t mcu, int8_t rssi) {
    (void)rel_ver; (void)mcu;
    /* §7.4: push TIME_SYNC when the node's time looks off */
    if (hb.flags & HB_FLAG_TIME_INVALID) {
      const time_sync_payload_t t{(uint32_t)g_hub->unix_now, g_hub->reg.data().tz_rule};
      g_hub->eng.send_time_sync(node_id, t, g_hub->ms_now);
    }
  }
  static LinkHandlers hub_handlers() {
    LinkHandlers h{};
    h.on_feed_ack = cb_feed_ack;
    h.on_feed_result = cb_feed_result;
    h.on_feed_unknown = cb_feed_unknown;
    h.on_heartbeat = cb_heartbeat;
    return h;
  }

  void begin_core(int64_t unix_now) { core.begin(&reg, &adapter, &store, &log, unix_now); }

  static HubSide *g_hub;
};
HubSide *HubSide::g_hub = nullptr;

/* ---- simulation driver ------------------------------------------------------------ */

struct Sim {
  HubSide hub;
  FakeNode node;

  explicit Sim(uint16_t base)
      : hub(base, (uint16_t)(base + 2)), /* hub: binds base, sends to node base+2 */
        node((uint16_t)(base + 2), base) /* node: binds base+2, sends to hub base */ {}

  static void unix_frac_reset() { unix_frac_ = 0; }
  static uint32_t unix_frac_;

  void begin(int64_t unix_now) {
    unix_frac_reset();
    hub.unix_now = unix_now;
    hub.begin_core(unix_now);
    node.begin(0, 1);
    FakeNode::g_node = &node;
    HubSide::g_hub = &hub;
    FakeNode::g_ms = 0;
    FakeNode::g_unix = unix_now;
    hub.ms_now = 0;
  }

  /* advance `steps` simulated milliseconds (unix clock: 1 s per 1000 ms) */
  void run(uint32_t steps) {
    for (uint32_t i = 0; i < steps; i++) {
      hub.ms_now++;
      if (++unix_frac_ >= 1000) {
        unix_frac_ = 0;
        hub.unix_now++;
      }
      FakeNode::g_ms = hub.ms_now;
      FakeNode::g_unix = hub.unix_now;
      hub.link.pump(hub.eng, hub.ms_now);
      node.link.pump(node.eng, hub.ms_now);
      hub.eng.tick(hub.ms_now);
      node.eng.tick(hub.ms_now);
      hub.core.tick(hub.ms_now, hub.unix_now);
      node.poll();
    }
  }

  uint16_t current_min_of_day() const {
    const LocalTime t = local_from_unix(hub.unix_now, TZ_MANILA);
    return t.min_of_day;
  }
};

uint32_t Sim::unix_frac_ = 0;

/* ---- mixed-type sim (M5b): gate + auger + pump over one hub -------------- *
 * The UDP shim attributes every inbound datagram to ONE peer mac, so a
 * single-socket hub cannot tell three nodes apart. This sim uses a tiny
 * in-process bus that carries real src/dst MACs instead. */

struct BusFrame {
  uint8_t src[6];
  uint8_t dst[6];
  uint8_t data[512];
  size_t len;
};

struct LoopBus : ILinkTransport {
  static BusFrame q[64];
  static uint8_t q_n;

  uint8_t self[6];
  uint8_t channel_ = 1;

  explicit LoopBus(const uint8_t mac[6]) { memcpy(self, mac, 6); }

  static void reset() { q_n = 0; }

  bool send(const uint8_t mac[6], const uint8_t *data, size_t len) override {
    if (q_n >= 64) return false;
    BusFrame &f = q[q_n++];
    memcpy(f.src, self, 6);
    memcpy(f.dst, mac, 6);
    f.len = len < sizeof(f.data) ? len : sizeof(f.data);
    memcpy(f.data, data, f.len);
    return true;
  }
  void set_channel(uint8_t ch) override { channel_ = ch; }
  uint8_t channel() const override { return channel_; }

  /* deliver every frame addressed to us, plus broadcasts (ESP-NOW
   * semantics: heartbeats ride the broadcast mac) not sent by ourselves */
  void pump(LinkEngine &engine, uint32_t now_ms) {
    static const uint8_t BC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    for (uint8_t i = 0; i < q_n;) {
      const bool unicast = memcmp(q[i].dst, self, 6) == 0;
      const bool bcast = memcmp(q[i].dst, BC, 6) == 0 &&
                         memcmp(q[i].src, self, 6) != 0;
      if (unicast || bcast) {
        engine.on_rx(q[i].src, q[i].data, q[i].len, -50, now_ms);
        for (uint8_t k = i; k + 1 < q_n; k++) q[k] = q[k + 1];
        q_n--;
      } else {
        i++;
      }
    }
  }
};
BusFrame LoopBus::q[64];
uint8_t LoopBus::q_n = 0;

struct BusNode {
  LinkEngine eng;
  LoopBus link;
  uint32_t on_ms = 30;

  struct Dispense { uint32_t feed_id; uint16_t amount; uint32_t done_ms; };
  Dispense pending{};
  uint32_t journal[16];
  uint8_t journal_unit[16];
  uint8_t journal_n = 0;
  uint16_t day_units = 0;
  uint8_t self_id = 1;

  explicit BusNode(const uint8_t mac[6]) : link(mac) {}

  void begin(uint32_t now_ms, uint8_t node_id) {
    eng.begin(LinkEngine::Role::Node, &link, handlers(), now_ms);
    self_id = node_id;
    eng.set_self(node_id, node_id);
    eng.add_peer(HUB_MAC, LINK_HUB_ID, TEST_KEY, sizeof(TEST_KEY));
  }

  static void on_feed(const uint8_t *mac, const feed_payload_t &f) {
    (void)mac;
    BusNode &n = *g_node;
    n.eng.send_feed_ack(f.feed_id, 1, 0, g_ms);
    for (uint8_t i = 0; i < n.journal_n; i++)
      if (n.journal[i] == f.feed_id) return; /* SR6: never re-actuate */
    if (n.journal_n < 16) {
      n.journal[n.journal_n] = f.feed_id;
      n.journal_unit[n.journal_n] = f.unit;
      n.journal_n++;
    }
    n.pending = Dispense{f.feed_id, f.amount, g_ms + n.on_ms};
  }
  static void hb_fill(heartbeat_payload_t *pl) {
    memset(pl, 0, sizeof(*pl));
    pl->uptime_ms = g_ms;
    pl->vin_mv = 12000;
    pl->day_units = g_node->day_units;
  }
  static void on_time_sync(const time_sync_payload_t &) {}
  static LinkHandlers handlers() {
    LinkHandlers h{};
    h.on_feed = on_feed;
    h.hb_fill = hb_fill;
    h.on_time_sync = on_time_sync;
    return h;
  }

  void poll() {
    if (pending.feed_id && (int32_t)(g_ms - pending.done_ms) >= 0) {
      eng.send_feed_result(pending.feed_id, 0, (uint16_t)on_ms, 0, 1,
                           (uint32_t)(g_unix - 8 * 3600), g_ms);
      day_units += pending.amount;
      pending.feed_id = 0;
    }
  }

  static BusNode *g_node;
  static uint32_t g_ms;
  static int64_t g_unix;
};
BusNode *BusNode::g_node = nullptr;
uint32_t BusNode::g_ms = 0;
int64_t BusNode::g_unix = 0;

struct MixedHub {
  E2eStore store;
  EventLog log;
  HubRegistry reg;
  HubCore core;
  LinkEngine eng;
  LoopBus link;

  struct Adapter : IHubLink {
    LinkEngine *e = nullptr;
    bool send_feed(uint8_t node, const feed_payload_t &f, uint32_t now) override {
      return e->send_feed(node, f, now);
    }
  } adapter;

  uint32_t ms_now = 0;
  int64_t unix_now = 0;

  explicit MixedHub(const uint8_t mac[6]) : link(mac) {
    adapter.e = &eng;
    reg.reset();
    reg.data().tz_rule = TZ_MANILA;
    eng.begin(LinkEngine::Role::Hub, &link, hub_handlers(), 0);
    eng.set_self(LINK_HUB_ID, 0);
  }

  static void cb_feed_ack(uint8_t node_id, const feed_ack_payload_t &a) {
    g_mhub->core.on_feed_ack(node_id, a, (uint32_t)g_mhub->unix_now);
  }
  static void cb_feed_result(uint8_t node_id, const feed_result_payload_t &r) {
    g_mhub->core.on_feed_result(node_id, r, (uint32_t)g_mhub->unix_now);
  }
  static void cb_feed_unknown(uint32_t feed_id, uint8_t node_id) {
    g_mhub->core.on_feed_unknown(feed_id, node_id, (uint32_t)g_mhub->unix_now);
  }
  static LinkHandlers hub_handlers() {
    LinkHandlers h{};
    h.on_feed_ack = cb_feed_ack;
    h.on_feed_result = cb_feed_result;
    h.on_feed_unknown = cb_feed_unknown;
    return h;
  }

  void begin_core(int64_t unix_now) { core.begin(&reg, &adapter, &store, &log, unix_now); }

  static MixedHub *g_mhub;
};
MixedHub *MixedHub::g_mhub = nullptr;

struct MixedSim {
  MixedHub hub;
  BusNode n1, n2, n3;

  static const uint8_t MAC1[6];
  static const uint8_t MAC2[6];
  static const uint8_t MAC3[6];

  explicit MixedSim(uint16_t base)
      : hub(HUB_MAC), n1(MAC1), n2(MAC2), n3(MAC3) {
    (void)base;
    LoopBus::reset();
    HubNode *g = hub.reg.add(MAC1, 1);
    TEST_ASSERT_NOT_NULL(g);
    g->profile.mech = MECH_SOLENOID_GATE;
    g->profile.unit = UNIT_GRAMS;
    g->profile.max_on_ms_single = 10000;
    g->profile.cal_ms_per_unit_x100 = 1000; /* 100 g/s */
    g->profile.crc16 = profile_crc16(&g->profile);
    hub.eng.add_peer(MAC1, 1, TEST_KEY, sizeof(TEST_KEY));

    HubNode *a = hub.reg.add(MAC2, 2);
    TEST_ASSERT_NOT_NULL(a);
    a->profile.mech = MECH_MOTOR_ONEWAY;
    a->profile.unit = UNIT_GRAMS;
    a->profile.max_on_ms_single = 10000;
    a->profile.cal_ms_per_unit_x100 = 2000; /* 50 g/s */
    a->profile.crc16 = profile_crc16(&a->profile);
    hub.eng.add_peer(MAC2, 2, TEST_KEY, sizeof(TEST_KEY));

    HubNode *p = hub.reg.add(MAC3, 3);
    TEST_ASSERT_NOT_NULL(p);
    p->profile.mech = MECH_PUMP;
    p->profile.unit = UNIT_ML;
    p->profile.max_on_ms_single = 10000;
    p->profile.cal_ms_per_unit_x100 = 4000; /* 25 ml/s */
    p->profile.crc16 = profile_crc16(&p->profile);
    hub.eng.add_peer(MAC3, 3, TEST_KEY, sizeof(TEST_KEY));
  }

  void begin(int64_t unix_now) {
    Sim::unix_frac_reset();
    hub.unix_now = unix_now;
    hub.begin_core(unix_now);
    n1.begin(0, 1);
    n2.begin(0, 2);
    n3.begin(0, 3);
    BusNode::g_node = &n1;
    MixedHub::g_mhub = &hub;
    BusNode::g_ms = 0;
    BusNode::g_unix = unix_now;
    hub.ms_now = 0;
  }

  void run(uint32_t steps) {
    for (uint32_t i = 0; i < steps; i++) {
      hub.ms_now++;
      if (++Sim::unix_frac_ >= 1000) {
        Sim::unix_frac_ = 0;
        hub.unix_now++;
      }
      BusNode::g_ms = hub.ms_now;
      BusNode::g_unix = hub.unix_now;
      hub.link.pump(hub.eng, hub.ms_now);
      BusNode::g_node = &n1;
      n1.link.pump(n1.eng, hub.ms_now);
      BusNode::g_node = &n2;
      n2.link.pump(n2.eng, hub.ms_now);
      BusNode::g_node = &n3;
      n3.link.pump(n3.eng, hub.ms_now);
      hub.eng.tick(hub.ms_now);
      n1.eng.tick(hub.ms_now);
      n2.eng.tick(hub.ms_now);
      n3.eng.tick(hub.ms_now);
      hub.core.tick(hub.ms_now, hub.unix_now);
      BusNode::g_node = &n1;
      n1.poll();
      BusNode::g_node = &n2;
      n2.poll();
      BusNode::g_node = &n3;
      n3.poll();
    }
  }
};

const uint8_t MixedSim::MAC1[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
const uint8_t MixedSim::MAC2[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x03};
const uint8_t MixedSim::MAC3[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x04};

/* ---- tests ------------------------------------------------------------------------- *//* ---- tests ------------------------------------------------------------------------- */

void test_e2e_scheduled_feed_full_pipeline(void) {
  Sim sim(sim_port_base());
  const int64_t unix0 = (days_from_civil(2026, 11, 2) * 86400 + 7 * 3600) -
                        8 * 3600; /* 2026-11-02 07:00 Manila, Sunday */
  sim.begin(unix0);

  HubNode *n = sim.hub.reg.by_id(1);
  TEST_ASSERT_NOT_NULL(n);
  /* daily 07:02 local, 20 g */
  const uint16_t fire_min = (uint16_t)(sim.current_min_of_day() + 2);
  n->sched.entries[0] = {fire_min, 0x7F, 20};
  n->sched.entry_count = 1;
  n->sched.enabled = 1;
  n->sched.version = ++sim.hub.reg.data().sched_version;
  n->sched.pushed_at = (uint32_t)unix0;

  sim.run(4 * 60 * 1000); /* 4 simulated minutes */

  for (uint16_t i = 0; i < sim.hub.log.count() && i < 12; i++) {
    const HubEvent *e = sim.hub.log.at(sim.hub.log.count() - 1 - i);
    }
  TEST_ASSERT_EQUAL_INT(1, sim.hub.log.count_of(EV_FEED_OK));
  TEST_ASSERT_EQUAL_INT(0, sim.hub.log.count_of(EV_FEED_FAIL));
  TEST_ASSERT_EQUAL_INT(0, sim.hub.core.fm().inflight_count());
  /* the node actuated exactly once, with the scheduled amount */
  TEST_ASSERT_EQUAL_INT(1, sim.node.journal_n);
  TEST_ASSERT_EQUAL_INT(20, sim.node.day_units);
}

void test_e2e_dropped_feed_retries_once_actuates_once(void) {
  Sim sim(sim_port_base());
  const int64_t unix0 = (days_from_civil(2026, 11, 3) * 86400 + 9 * 3600) - 8 * 3600;
  sim.begin(unix0);

  sim.hub.link.drop_next(); /* black-hole the first FEED datagram (§6.6) */
  TEST_ASSERT_TRUE(sim.hub.core.request_manual(1, 20, 0, unix0));
  sim.run(30 * 1000);

  TEST_ASSERT_EQUAL_INT(1, sim.hub.log.count_of(EV_FEED_OK));
  TEST_ASSERT_EQUAL_INT(1, sim.node.journal_n); /* never actuated twice (SR6) */
  TEST_ASSERT_EQUAL_INT(0, sim.hub.core.fm().inflight_count());
  TEST_ASSERT_TRUE(sim.hub.eng.stats().feed_retries >= 1);
  TEST_ASSERT_TRUE(sim.hub.eng.stats().rx_ok >= 2); /* ACK + RESULT came back */
}

void test_e2e_result_repeats_cover_lost_results(void) {
  /* RESULT is repeated 3× (§6.6). The node drops repeats 1 and 2; the third
   * still lands and the hub records exactly one event, feed settled. */
  Sim sim(sim_port_base());
  const int64_t unix0 = (days_from_civil(2026, 11, 4) * 86400 + 9 * 3600) - 8 * 3600;
  sim.begin(unix0);
  sim.node.drop_results = true;

  TEST_ASSERT_TRUE(sim.hub.core.request_manual(1, 20, 0, unix0));
  sim.run(30 * 1000);

  TEST_ASSERT_EQUAL_INT(1, sim.hub.log.count_of(EV_FEED_OK));
  TEST_ASSERT_EQUAL_INT(1, sim.node.journal_n);
  TEST_ASSERT_EQUAL_INT(0, sim.hub.core.fm().inflight_count());
}

void test_e2e_time_sync_push_on_invalid_node_clock(void) {
  Sim sim(sim_port_base());
  const int64_t unix0 = (days_from_civil(2026, 11, 5) * 86400 + 9 * 3600) - 8 * 3600;
  sim.begin(unix0);

  sim.run(45 * 1000); /* node heartbeats at 2 s, 32 s, … (HB_PERIOD_MS) */

  TEST_ASSERT_TRUE(sim.node.syncs_seen >= 1);
  TEST_ASSERT_TRUE(sim.node.last_sync_unix >= unix0);
  TEST_ASSERT_FALSE(sim.node.time_invalid_hb); /* cleared after the first sync */
}

void test_e2e_mixed_types_manual_feeds_carry_profile_units(void) {
  Sim::unix_frac_reset();
  MixedSim mx(sim_port_base());
  const int64_t unix0 = (days_from_civil(2026, 11, 6) * 86400 + 9 * 3600) - 8 * 3600;
  mx.begin(unix0);

  /* gate (id 1) and auger (id 2) take a manual 30 g feed, pump (id 3) a
   * manual 100 ml feed — the FEED payload unit follows the profile (M5b
   * mixed-type e2e: grams nodes and the ml pump share one hub) */
  TEST_ASSERT_TRUE(mx.hub.core.request_manual(1, 30, 0, unix0));
  TEST_ASSERT_TRUE(mx.hub.core.request_manual(2, 30, 0, unix0));
  TEST_ASSERT_TRUE(mx.hub.core.request_manual(3, 100, 0, unix0));
  mx.run(30 * 1000);

  TEST_ASSERT_EQUAL_INT(3, mx.hub.log.count_of(EV_FEED_OK));
  TEST_ASSERT_EQUAL_INT(0, mx.hub.log.count_of(EV_FEED_FAIL));
  TEST_ASSERT_EQUAL_INT(0, mx.hub.core.fm().inflight_count());
  TEST_ASSERT_EQUAL_INT(1, mx.n1.journal_n);
  TEST_ASSERT_EQUAL_INT(30, mx.n1.day_units);
  TEST_ASSERT_EQUAL_INT(UNIT_GRAMS, mx.n1.journal_unit[0]);
  TEST_ASSERT_EQUAL_INT(1, mx.n2.journal_n);
  TEST_ASSERT_EQUAL_INT(30, mx.n2.day_units);
  TEST_ASSERT_EQUAL_INT(UNIT_GRAMS, mx.n2.journal_unit[0]);
  TEST_ASSERT_EQUAL_INT(1, mx.n3.journal_n);
  TEST_ASSERT_EQUAL_INT(100, mx.n3.day_units);
  TEST_ASSERT_EQUAL_INT(UNIT_ML, mx.n3.journal_unit[0]);
}

void test_e2e_mixed_types_pump_scheduled_feed_in_ml(void) {
  Sim::unix_frac_reset();
  MixedSim mx(sim_port_base());
  const int64_t unix0 = (days_from_civil(2026, 11, 7) * 86400 + 9 * 3600) - 8 * 3600;
  mx.begin(unix0);

  /* the pump's first schedule fires 2 simulated minutes out (local), 100 ml */
  const LocalTime lt0 = local_from_unix(unix0, TZ_MANILA);
  const uint16_t fire_min = (uint16_t)((lt0.min_of_day + 2) % 1440);
  HubNode *p = mx.hub.reg.by_id(3);
  TEST_ASSERT_NOT_NULL(p);
  p->sched.entries[0] = {(uint16_t)fire_min, 0x7F, 100};
  p->sched.entry_count = 1;
  p->sched.enabled = 1;
  p->sched.version = ++mx.hub.reg.data().sched_version;
  p->sched.pushed_at = (uint32_t)unix0;

  mx.run(4 * 60 * 1000);

  TEST_ASSERT_EQUAL_INT(1, mx.hub.log.count_of(EV_FEED_OK));
  TEST_ASSERT_EQUAL_INT(1, mx.n3.journal_n);
  TEST_ASSERT_EQUAL_INT(100, mx.n3.day_units);
  TEST_ASSERT_EQUAL_INT(UNIT_ML, mx.n3.journal_unit[0]);
  TEST_ASSERT_EQUAL_INT(0, mx.hub.core.fm().inflight_count());
  /* the gram nodes stay quiet */
  TEST_ASSERT_EQUAL_INT(0, mx.n1.journal_n);
  TEST_ASSERT_EQUAL_INT(0, mx.n2.journal_n);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_e2e_scheduled_feed_full_pipeline);
  RUN_TEST(test_e2e_dropped_feed_retries_once_actuates_once);
  RUN_TEST(test_e2e_result_repeats_cover_lost_results);
  RUN_TEST(test_e2e_time_sync_push_on_invalid_node_clock);
  RUN_TEST(test_e2e_mixed_types_manual_feeds_carry_profile_units);
  RUN_TEST(test_e2e_mixed_types_pump_scheduled_feed_in_ml);
  return UNITY_END();
}
