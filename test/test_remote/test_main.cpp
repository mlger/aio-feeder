/* Host tests for the M6 remote core (PLAN.md §6.7, FR2, FR12, FR20).
 *
 * Scene: the pure layer in hub/src/remote.h — settings blob, the JSON
 * builders/parsers, HA discovery entities, notify texts — driven the same
 * way the firmware glue drives it: seeded registry + event log, RAM store.
 */
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include <unity.h>

#include "backup.h"
#include "hub_registry.h"
#include "remote.h"
#include "settings_store.h"

using namespace aio;

struct RamStore : IHubStore {
  std::map<std::string, std::vector<uint8_t>> m;
  bool put(const char *key, const uint8_t *data, size_t len) override {
    m[key] = std::vector<uint8_t>(data, data + len);
    return true;
  }
  size_t get(const char *key, uint8_t *out, size_t max_len) override {
    auto it = m.find(key);
    if (it == m.end()) return 0;
    const size_t n = it->second.size() < max_len ? it->second.size() : max_len;
    memcpy(out, it->second.data(), n);
    return it->second.size();
  }
  bool del(const char *key) override { return m.erase(key) > 0; }
  void corrupt() {
    if (!m.empty()) m.begin()->second.back() ^= 0xFF;
  }
};

static RamStore store;
static HubRegistry reg;
static EventLog log_;

static void seed_two_nodes() {
  reg.reset();
  log_ = EventLog{};
  const uint8_t mac1[6] = {2, 0, 0, 0, 0, 1};
  const uint8_t mac2[6] = {2, 0, 0, 0, 0, 2};
  HubNode *a = reg.add(mac1, 1);
  HubNode *b = reg.add(mac2, 1);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);
  snprintf(a->profile.name, sizeof(a->profile.name), "kitA");
  a->profile.unit = UNIT_GRAMS;
  a->profile.storage_capacity = 1500;
  a->profile.flags = PROFILE_FLAG_SUPPLY_MONITOR;
  snprintf(b->profile.name, sizeof(b->profile.name), "pump1");
  b->profile.unit = UNIT_ML;
  b->profile.mech = MECH_PUMP;
  b->profile.storage_capacity = 2000;
  b->profile.flags = 0; /* no supply monitor → discovery skips that entity */
  /* history: OK feed, then a fail on node 2 (newest alert) */
  log_.push(1730000000u, 1, EV_FEED_OK, 0, 1001, 40, 0, EV_SRC_MANUAL);
  log_.push(1730000100u, 2, EV_FEED_FAIL, 0, 1002, 15);
}

void setUp(void) {
  store = RamStore{};
  seed_two_nodes();
}

void tearDown(void) {}

/* --- settings blob ---------------------------------------------------------------- */

static void test_defaults(void) {
  const RemoteSettings s = remote_defaults();
  TEST_ASSERT_EQUAL_UINT8(1, s.web_on);
  TEST_ASSERT_EQUAL_UINT16(RMT_WEB_PORT_DEF, s.web_port);
  TEST_ASSERT_EQUAL_UINT8(0, s.web_auth);
  TEST_ASSERT_EQUAL_UINT8(0, s.mqtt_on);
  TEST_ASSERT_EQUAL_UINT8(1, s.mqtt_disc);
  TEST_ASSERT_EQUAL_UINT8(0, s.mqtt_url[0]);
}

static void test_roundtrip(void) {
  RemoteSettings s = remote_defaults();
  TEST_ASSERT_TRUE(remote_set_mqtt(&s, "mqtt://home:1883", "user", "pw"));
  s.web_port = 8080;
  s.web_auth = 1;
  TEST_ASSERT_TRUE(remote_save(store, s));

  RemoteSettings out;
  TEST_ASSERT_TRUE(remote_load(store, &out));
  TEST_ASSERT_EQUAL_STRING("mqtt://home:1883", out.mqtt_url);
  TEST_ASSERT_EQUAL_STRING("user", out.mqtt_user);
  TEST_ASSERT_EQUAL_STRING("pw", out.mqtt_pass);
  TEST_ASSERT_EQUAL_UINT16(8080, out.web_port);
  TEST_ASSERT_EQUAL_UINT8(1, out.web_auth);
}

static void test_load_rejects_corrupt(void) {
  RemoteSettings s = remote_defaults();
  remote_save(store, s);
  store.corrupt();
  RemoteSettings out;
  TEST_ASSERT_FALSE(remote_load(store, &out)); /* caller falls back to defaults */
}

static void test_set_web_validation(void) {
  RemoteSettings s = remote_defaults();
  TEST_ASSERT_FALSE(remote_set_web(&s, 1, 0, 0)); /* port 0 */
  TEST_ASSERT_TRUE(remote_set_web(&s, 1, 8081, 1));
  TEST_ASSERT_EQUAL_UINT16(8081, s.web_port);
  TEST_ASSERT_EQUAL_UINT8(1, s.web_auth);
}

static void test_set_mqtt_validation(void) {
  RemoteSettings s = remote_defaults();
  char long_url[120];
  memset(long_url, 'a', sizeof(long_url) - 1);
  long_url[sizeof(long_url) - 1] = '\0';
  TEST_ASSERT_FALSE(remote_set_mqtt(&s, long_url, "", ""));
  TEST_ASSERT_TRUE(remote_set_mqtt(&s, "host", "u", "p"));
  TEST_ASSERT_EQUAL_STRING("host", s.mqtt_url);
}

static void test_mqtt_uri_scheme(void) {
  char uri[110];
  TEST_ASSERT_TRUE(remote_mqtt_uri("192.168.1.10", uri, sizeof(uri)));
  TEST_ASSERT_EQUAL_STRING("mqtt://192.168.1.10", uri);
  TEST_ASSERT_TRUE(remote_mqtt_uri("mqtts://b:8883", uri, sizeof(uri)));
  TEST_ASSERT_EQUAL_STRING("mqtts://b:8883", uri);
  TEST_ASSERT_FALSE(remote_mqtt_uri("", uri, sizeof(uri)));
}

/* --- auth (§6.7 login requirement) --------------------------------------------------- */

static uint8_t g_salt = 0;
static void det_rng(uint8_t *out, size_t n) {
  for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(0xB0 + g_salt++);
}

static void test_auth(void) {
  RemoteSettings r = remote_defaults();
  HubSettings set = settings_defaults();
  /* open when web_auth is clear */
  TEST_ASSERT_TRUE(remote_auth_ok(r, set, "1234"));
  TEST_ASSERT_TRUE(remote_auth_ok(r, set, ""));
  /* auth flag without a configured PIN denies everything (fail-closed) */
  r.web_auth = 1;
  TEST_ASSERT_FALSE(remote_auth_ok(r, set, "1234"));
  /* with a real PIN */
  pin_set(&set, "1234", det_rng);
  TEST_ASSERT_TRUE(remote_auth_ok(r, set, "1234"));
  TEST_ASSERT_FALSE(remote_auth_ok(r, set, "9999"));
  TEST_ASSERT_FALSE(remote_auth_ok(r, set, ""));
}

/* --- command parsing ------------------------------------------------------------------ */

static void test_feed_parse(void) {
  uint8_t node = 0;
  uint16_t amount = 0;
  TEST_ASSERT_TRUE(remote_feed_parse("{\"node\":2,\"amount\":15}", &node, &amount));
  TEST_ASSERT_EQUAL_UINT8(2, node);
  TEST_ASSERT_EQUAL_UINT16(15, amount);
  /* §6.7 example carries a unit — accepted, ignored (profile owns it) */
  TEST_ASSERT_TRUE(remote_feed_parse("{\"amount\":2,\"unit\":\"pulses\",\"node\":1}",
                                     &node, &amount));
  TEST_ASSERT_EQUAL_UINT8(1, node);
  TEST_ASSERT_EQUAL_UINT16(2, amount);
  TEST_ASSERT_FALSE(remote_feed_parse("{\"node\":2}", &node, &amount));
  TEST_ASSERT_FALSE(remote_feed_parse("{\"amount\":2}", &node, &amount));
  TEST_ASSERT_FALSE(remote_feed_parse("not json", &node, &amount));
  TEST_ASSERT_FALSE(remote_feed_parse("{\"node\":0,\"amount\":2}", &node, &amount));
  TEST_ASSERT_FALSE(remote_feed_parse("{\"node\":9,\"amount\":2}", &node, &amount));
  TEST_ASSERT_FALSE(remote_feed_parse("{\"node\":2,\"amount\":0}", &node, &amount));
}

static void test_mqtt_topic_parse(void) {
  TEST_ASSERT_EQUAL_UINT8(3, mqtt_feed_topic_parse("aio/node/3/cmd/feed"));
  TEST_ASSERT_EQUAL_UINT8(0, mqtt_feed_topic_parse("aio/node/3/state"));
  TEST_ASSERT_EQUAL_UINT8(0, mqtt_feed_topic_parse("aio/node/0/cmd/feed"));
  TEST_ASSERT_EQUAL_UINT8(0, mqtt_feed_topic_parse("aio/node/12/cmd/feed"));
  TEST_ASSERT_EQUAL_UINT8(0, mqtt_feed_topic_parse(nullptr));
}

static void test_settings_patch(void) {
  RemoteSettings s = remote_defaults();
  TEST_ASSERT_TRUE(remote_settings_patch("{\"mqtt_on\":1,\"mqtt_url\":\"b:1883\"}", &s));
  TEST_ASSERT_EQUAL_UINT8(1, s.mqtt_on);
  TEST_ASSERT_EQUAL_STRING("b:1883", s.mqtt_url);
  TEST_ASSERT_EQUAL_UINT8(1, s.web_on); /* untouched */
  /* unknown key refuses the whole patch (half-read settings are worse) */
  TEST_ASSERT_FALSE(remote_settings_patch("{\"nope\":1}", &s));
  TEST_ASSERT_FALSE(remote_settings_patch("{\"web_port\":0}", &s));
  TEST_ASSERT_FALSE(remote_settings_patch("{", &s));
  TEST_ASSERT_TRUE(remote_settings_patch("{}", &s));
  /* empty password clears it */
  TEST_ASSERT_TRUE(remote_set_mqtt(&s, "b", "u", "secret"));
  TEST_ASSERT_TRUE(remote_settings_patch("{\"mqtt_pass\":\"\"}", &s));
  TEST_ASSERT_EQUAL_UINT8(0, s.mqtt_pass[0]);
}

/* --- telemetry JSON -------------------------------------------------------------------- */

static void test_node_state_json(void) {
  const HubNode *a = reg.by_id(1);
  RemoteNodeLive live;
  live.state = ui::NODE_ONLINE;
  live.vin_mv = 5120;
  live.hb_age_s = 12;
  char buf[384];
  TEST_ASSERT_TRUE(remote_node_state_json(*a, log_, live, 1730000200, buf, sizeof(buf)));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"online\":true"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"kitA\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"unit\":\"g\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"vin\":5.12"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"cap\":1500"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"last_amount\":40"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"last_result\":\"OK\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"last_age\":200"));
  TEST_ASSERT_NULL(strstr(buf, "pulses"));
}

static void test_node_config_json(void) {
  const HubNode *b = reg.by_id(2);
  char buf[384];
  TEST_ASSERT_TRUE(remote_node_config_json(*b, buf, sizeof(buf)));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"mech\":\"pump\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"unit\":\"ml\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"policy\":\"skip\""));
}

static void test_status_json(void) {
  RemoteSettings r = remote_defaults();
  r.mqtt_on = 1;
  RemoteNodeLive live[HUB_MAX_NODES];
  for (int i = 0; i < HUB_MAX_NODES; i++) live[i] = RemoteNodeLive{};
  live[0].state = ui::NODE_ONLINE;
  live[0].vin_mv = 4900;
  const LocalTime now = local_from_unix(1730000200, reg.data().tz_rule);
  char buf[2048];
  TEST_ASSERT_TRUE(remote_status_json(reg, log_, r, false, live, "v0.9.0", now,
                                      1730000200, buf, sizeof(buf)));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"fw\":\"v0.9.0\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"online\":1"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"mqtt\":\"down\""));
  /* newest alert-worthy event wins (node 2 feed fail) */
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"alert\":{\"node\":2,\"kind\":\"FAIL\"}"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"kitA\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"state\":\"online\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"state\":\"offline\""));
}

static void test_status_json_escapes_names(void) {
  HubNode *a = reg.by_id(1);
  snprintf(a->profile.name, sizeof(a->profile.name), "a\"b,c");
  RemoteSettings r = remote_defaults();
  RemoteNodeLive live[HUB_MAX_NODES];
  for (int i = 0; i < HUB_MAX_NODES; i++) live[i] = RemoteNodeLive{};
  const LocalTime now = local_from_unix(1730000200, reg.data().tz_rule);
  char buf[2048];
  TEST_ASSERT_TRUE(remote_status_json(reg, log_, r, false, live, "v", now,
                                      1730000200, buf, sizeof(buf)));
  TEST_ASSERT_NOT_NULL(strstr(buf, "a\\\"b,c"));
}

static void test_events_json_limit(void) {
  char buf[2048];
  TEST_ASSERT_TRUE(remote_events_json(reg, log_, 1, buf, sizeof(buf)));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"kind\":\"FAIL\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"amount\":15"));
  TEST_ASSERT_NULL(strstr(buf, "kitA")); /* only the newest row */
  TEST_ASSERT_NOT_NULL(strstr(buf, "pump1"));
}

static void test_csv(void) {
  char buf[4096];
  const size_t len = remote_csv(reg, log_, 1730000200, buf, sizeof(buf));
  TEST_ASSERT_TRUE(len > 0);
  TEST_ASSERT_NOT_NULL(strstr(buf, "time,node,amount,unit,result,source"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "kitA,40,g,OK,manual"));
  TEST_ASSERT_EQUAL_UINT8(0, remote_csv(reg, log_, 1000, buf, sizeof(buf)));
}

/* --- HA discovery ------------------------------------------------------------------------ */

static void test_discovery_entities(void) {
  const HubNode *a = reg.by_id(1);
  char topic[64], pl[512];
  TEST_ASSERT_TRUE(mqtt_discovery_entity(*a, 0, topic, sizeof(topic), pl, sizeof(pl)));
  TEST_ASSERT_EQUAL_STRING("homeassistant/button/aio-n1-feed/config", topic);
  TEST_ASSERT_NOT_NULL(strstr(pl, "\"cmd_t\":\"aio/node/1/cmd/feed\""));
  TEST_ASSERT_NOT_NULL(strstr(pl, "\\\"amount\\\":"));
  TEST_ASSERT_NOT_NULL(strstr(pl, "\"dev\":"));
  TEST_ASSERT_NOT_NULL(strstr(pl, "avty_t"));

  TEST_ASSERT_TRUE(mqtt_discovery_entity(*a, 1, topic, sizeof(topic), pl, sizeof(pl)));
  TEST_ASSERT_NOT_NULL(strstr(pl, "last_amount"));
  TEST_ASSERT_NOT_NULL(strstr(pl, "\"unit_of_meas\":\"g\""));

  TEST_ASSERT_TRUE(mqtt_discovery_entity(*a, 2, topic, sizeof(topic), pl, sizeof(pl)));
  TEST_ASSERT_NOT_NULL(strstr(topic, "binary_sensor"));
  TEST_ASSERT_NOT_NULL(strstr(pl, "connectivity"));

  TEST_ASSERT_TRUE(mqtt_discovery_entity(*a, 3, topic, sizeof(topic), pl, sizeof(pl)));
  TEST_ASSERT_NOT_NULL(strstr(pl, "\"unit_of_meas\":\"V\""));

  TEST_ASSERT_TRUE(mqtt_discovery_entity(*a, 4, topic, sizeof(topic), pl, sizeof(pl)));
  TEST_ASSERT_NOT_NULL(strstr(pl, "value_json.left"));
  TEST_ASSERT_FALSE(mqtt_discovery_entity(*a, 5, topic, sizeof(topic), pl, sizeof(pl)));

  /* pump1 has no supply monitor: idx 3 is empty, stock rides at idx 4 */
  const HubNode *b = reg.by_id(2);
  TEST_ASSERT_FALSE(mqtt_discovery_entity(*b, 3, topic, sizeof(topic), pl, sizeof(pl)));
  TEST_ASSERT_TRUE(mqtt_discovery_entity(*b, 4, topic, sizeof(topic), pl, sizeof(pl)));
  TEST_ASSERT_NOT_NULL(strstr(topic, "stock"));
  TEST_ASSERT_NOT_NULL(strstr(pl, "\"unit_of_meas\":\"ml\""));
}

/* --- notify (FR20) -------------------------------------------------------------------------- */

static void test_notify_texts(void) {
  char txt[128];
  TEST_ASSERT_TRUE(remote_notify_text(EV_FEED_FAIL, 1, "kitA", 40, 0, txt, sizeof(txt)));
  TEST_ASSERT_NOT_NULL(strstr(txt, "feed failed on kitA"));
  TEST_ASSERT_TRUE(remote_notify_text(EV_FEED_REJ, 1, "kitA", 40, 3, txt, sizeof(txt)));
  TEST_ASSERT_NOT_NULL(strstr(txt, "reason 3"));
  TEST_ASSERT_TRUE(remote_notify_text(EV_FEED_UNKNOWN, 1, "kitA", 40, 0, txt, sizeof(txt)));
  TEST_ASSERT_TRUE(remote_notify_text(EV_SKIPPED_MISSED, 1, "kitA", 40, 2, txt, sizeof(txt)));
  TEST_ASSERT_NOT_NULL(strstr(txt, "2 missed feeds"));
  TEST_ASSERT_FALSE(remote_notify_text(EV_FEED_OK, 1, "kitA", 40, 0, txt, sizeof(txt)));
  TEST_ASSERT_FALSE(remote_notify_text(EV_SKIPPED_USER, 1, "kitA", 40, 0, txt, sizeof(txt)));
}

static void test_low_stock_notify(void) {
  const HubNode *a = reg.by_id(1);
  char txt[128];
  /* 1500 cap; dispensed 40 → 1460 left, well above 20% */
  TEST_ASSERT_FALSE(remote_notify_low_stock(log_, *a, 1730000200, txt, sizeof(txt)));
  /* push it under 20% (≤300 left): 1250 more grams */
  log_.push(1730000150u, 1, EV_FEED_OK, 0, 1003, 1250);
  TEST_ASSERT_TRUE(remote_notify_low_stock(log_, *a, 1730000200, txt, sizeof(txt)));
  TEST_ASSERT_NOT_NULL(strstr(txt, "low stock on kitA"));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_defaults);
  RUN_TEST(test_roundtrip);
  RUN_TEST(test_load_rejects_corrupt);
  RUN_TEST(test_set_web_validation);
  RUN_TEST(test_set_mqtt_validation);
  RUN_TEST(test_mqtt_uri_scheme);
  RUN_TEST(test_auth);
  RUN_TEST(test_feed_parse);
  RUN_TEST(test_mqtt_topic_parse);
  RUN_TEST(test_settings_patch);
  RUN_TEST(test_node_state_json);
  RUN_TEST(test_node_config_json);
  RUN_TEST(test_status_json);
  RUN_TEST(test_status_json_escapes_names);
  RUN_TEST(test_events_json_limit);
  RUN_TEST(test_csv);
  RUN_TEST(test_discovery_entities);
  RUN_TEST(test_notify_texts);
  RUN_TEST(test_low_stock_notify);
  return UNITY_END();
}
