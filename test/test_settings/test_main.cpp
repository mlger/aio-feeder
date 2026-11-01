/* Host tests for the hub settings blob + PIN lock (PLAN.md §8.8, FR10; M4b).
 *
 * Scene: settings blob round-trips through the RAM store like the registry
 * does in test_hub_core; the PIN rules follow §8: scope is explicit, a
 * correct code unlocks for 60 s, schedules are never gated (the scheduler
 * never consults the lock). */
#include <string.h>

#include <unity.h>

#include "event_log.h"
#include "hub_registry.h"
#include "pin_lock.h"
#include "settings_store.h"

using namespace aio;

#include <map>
#include <string>
#include <vector>

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
static HubSettings set;

/* deterministic salt rng (testable pin_set) */
static uint8_t g_salt = 0;
static void det_rng(uint8_t *out, size_t n) {
  for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(0xA0 + g_salt++);
}

void setUp(void) {
  store = RamStore{};
  set = settings_defaults();
  g_salt = 0;
}

void tearDown(void) {}

static void test_defaults(void) {
  TEST_ASSERT_EQUAL_UINT8(SET_BRIGHTNESS_DEF, set.brightness);
  TEST_ASSERT_EQUAL_UINT16(SET_SCREEN_OFF_DEF, set.screen_off_s);
  TEST_ASSERT_EQUAL_UINT8(SET_RETENTION_DEF, set.history_days);
  TEST_ASSERT_EQUAL_UINT8(1, set.ntp_on);
  TEST_ASSERT_EQUAL_UINT8(0, set.pin_enabled);
  TEST_ASSERT_EQUAL_UINT8(PIN_SCOPE_SETTINGS, set.pin_scope);
}

static void test_round_trip(void) {
  set.brightness = 65;
  set.screen_off_s = 120;
  set.history_days = 90;
  set.ntp_on = 0;
  snprintf(set.wifi_ssid, sizeof(set.wifi_ssid), "home-net");
  snprintf(set.wifi_pass, sizeof(set.wifi_pass), "s3cret");
  TEST_ASSERT_TRUE(settings_save(store, set));

  HubSettings back = settings_defaults();
  TEST_ASSERT_TRUE(settings_load(store, &back));
  TEST_ASSERT_EQUAL_UINT8(65, back.brightness);
  TEST_ASSERT_EQUAL_UINT16(120, back.screen_off_s);
  TEST_ASSERT_EQUAL_UINT8(90, back.history_days);
  TEST_ASSERT_EQUAL_UINT8(0, back.ntp_on);
  TEST_ASSERT_EQUAL_STRING("home-net", back.wifi_ssid);
  TEST_ASSERT_EQUAL_STRING("s3cret", back.wifi_pass);
}

static void test_corrupt_rejected(void) {
  TEST_ASSERT_TRUE(settings_save(store, set));
  store.corrupt();
  HubSettings back = settings_defaults();
  TEST_ASSERT_FALSE(settings_load(store, &back));
}

static void test_validators(void) {
  TEST_ASSERT_EQUAL_UINT8(SET_BRIGHTNESS_MIN, set_clamp_brightness(1));
  TEST_ASSERT_EQUAL_UINT8(SET_BRIGHTNESS_MAX, set_clamp_brightness(255));
  TEST_ASSERT_EQUAL_UINT8(73, set_clamp_brightness(73));
  TEST_ASSERT_TRUE(set_screen_off_valid(0));
  TEST_ASSERT_TRUE(set_screen_off_valid(300));
  TEST_ASSERT_FALSE(set_screen_off_valid(45));
  TEST_ASSERT_TRUE(set_retention_valid(7));
  TEST_ASSERT_FALSE(set_retention_valid(14));
  TEST_ASSERT_EQUAL_UINT16(120, set_screen_off_next(60));
  TEST_ASSERT_EQUAL_UINT16(0, set_screen_off_next(300));
  TEST_ASSERT_EQUAL_UINT16(30, set_screen_off_next(0));
}

static void test_pin_hash_salted(void) {
  TEST_ASSERT_TRUE(pin_valid_digits("1234"));
  TEST_ASSERT_FALSE(pin_valid_digits("123"));
  TEST_ASSERT_FALSE(pin_valid_digits("12a4"));
  TEST_ASSERT_FALSE(pin_valid_digits(nullptr));

  HubSettings a = set, b = set;
  pin_set(&a, "1234", det_rng);
  pin_set(&b, "1234", det_rng); /* fresh salt each set */
  TEST_ASSERT_NOT_EQUAL(a.pin_salt, b.pin_salt);
  TEST_ASSERT_NOT_EQUAL(a.pin_hash, b.pin_hash);
  TEST_ASSERT_TRUE(pin_verify(a, "1234"));
  TEST_ASSERT_FALSE(pin_verify(a, "4321"));
  TEST_ASSERT_TRUE(pin_verify(b, "1234"));
}

static void test_pin_set_off(void) {
  pin_set(&set, "9999", det_rng);
  TEST_ASSERT_EQUAL_UINT8(1, set.pin_enabled);
  set.pin_enabled = 0; /* pin off clears the gate; verify is gated by it */
  TEST_ASSERT_FALSE(pin_verify(set, "9999"));
}

static void test_lock_basic(void) {
  pin_set(&set, "1234", det_rng);
  PinLock lock;
  lock.configure(set);
  TEST_ASSERT_TRUE(lock.enabled());
  TEST_ASSERT_TRUE(lock.locked());
  TEST_ASSERT_TRUE(lock.blocks_settings());
  TEST_ASSERT_FALSE(lock.blocks_manual_feed()); /* scope = settings only */

  TEST_ASSERT_FALSE(lock.unlock("1111", 1000));
  TEST_ASSERT_TRUE(lock.locked());
  TEST_ASSERT_TRUE(lock.unlock("1234", 1000));
  TEST_ASSERT_FALSE(lock.locked());
  TEST_ASSERT_FALSE(lock.blocks_settings());
  TEST_ASSERT_TRUE(lock.unlock_left_s(1000) <= 60 && lock.unlock_left_s(1000) > 55);

  lock.tick(1000 + PIN_UNLOCK_MS - 1);
  TEST_ASSERT_FALSE(lock.locked());
  lock.tick(1000 + PIN_UNLOCK_MS);
  TEST_ASSERT_TRUE(lock.locked());
}

static void test_lock_scope_feeds(void) {
  pin_set(&set, "2468", det_rng);
  set.pin_scope = PIN_SCOPE_SETTINGS_FEEDS;
  PinLock lock;
  lock.configure(set);
  TEST_ASSERT_TRUE(lock.blocks_manual_feed());
  TEST_ASSERT_TRUE(lock.unlock("2468", 100));
  TEST_ASSERT_FALSE(lock.blocks_manual_feed());
  lock.relock();
  TEST_ASSERT_TRUE(lock.blocks_manual_feed());
}

static void test_lock_disabled_never_blocks(void) {
  PinLock lock;
  lock.configure(settings_defaults()); /* pin_enabled = 0 */
  TEST_ASSERT_FALSE(lock.enabled());
  TEST_ASSERT_FALSE(lock.locked());
  TEST_ASSERT_FALSE(lock.blocks_settings());
  TEST_ASSERT_FALSE(lock.blocks_manual_feed());
  TEST_ASSERT_TRUE(lock.unlock("0000", 0)); /* vacuously succeeds */
}

static void test_retention_prune(void) {
  EventLog log;
  const uint32_t now = 40u * 86400;
  log.push(now - 1 * 86400, 1, EV_FEED_OK, FEED_OK, 1, 10, 0);  /* 39 d old */
  log.push(now - 8 * 86400, 1, EV_FEED_OK, FEED_OK, 2, 10, 0);  /* 32 d: > 7 */
  log.push(now - 31 * 86400, 1, EV_FEED_OK, FEED_OK, 3, 10, 0); /* 9 d: > 30 */
  log.push(now - 100, 2, EV_FEED_OK, FEED_OK, 4, 5, 0);         /* fresh */
  TEST_ASSERT_EQUAL_UINT16(4, log.count());

  TEST_ASSERT_EQUAL_UINT16(2, log.prune_older_than(now, 7));
  TEST_ASSERT_EQUAL_UINT16(2, log.count());
  TEST_ASSERT_EQUAL_UINT32(1, log.at(0)->feed_id); /* oldest kept first */
  TEST_ASSERT_EQUAL_UINT32(4, log.at(1)->feed_id);
  TEST_ASSERT_EQUAL_UINT16(0, log.prune_older_than(now, 7)); /* idempotent */

  EventLog log2;
  log2.push(now - 35 * 86400, 1, EV_FEED_OK, FEED_OK, 5, 10, 0);
  log2.push(now - 9 * 86400, 1, EV_FEED_OK, FEED_OK, 6, 10, 0);
  log2.push(now - 100, 1, EV_FEED_OK, FEED_OK, 7, 10, 0);
  TEST_ASSERT_EQUAL_UINT16(1, log2.prune_older_than(now, 30)); /* 35 d old gone */
  TEST_ASSERT_EQUAL_UINT16(2, log2.count());
  TEST_ASSERT_EQUAL_UINT32(6, log2.at(0)->feed_id);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_defaults);
  RUN_TEST(test_round_trip);
  RUN_TEST(test_corrupt_rejected);
  RUN_TEST(test_validators);
  RUN_TEST(test_pin_hash_salted);
  RUN_TEST(test_pin_set_off);
  RUN_TEST(test_lock_basic);
  RUN_TEST(test_lock_scope_feeds);
  RUN_TEST(test_lock_disabled_never_blocks);
  RUN_TEST(test_retention_prune);
  return UNITY_END();
}
