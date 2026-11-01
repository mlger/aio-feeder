/* Host tests for backup / restore / CSV export (PLAN.md §9.12, FR19, D18;
 * M4b).
 *
 * Checks the §11.1 test-list items for this feature: backup round-trip
 * without PIN or Wi-Fi password, restore keeps history + secrets, CSV
 * quoting of node names containing commas or quotes. */
#include <string.h>

#include <unity.h>

#include "backup.h"

using namespace aio;

static char g_buf[BACKUP_JSON_MAX];
static char g_csv[BACKUP_JSON_MAX];

static void seed_node(HubRegistry &reg, uint8_t id, const char *name, uint8_t mech,
                      uint16_t min_of_day, uint16_t amount) {
  const uint8_t mac[6] = {0x02, 0, 0, 0, 0, id};
  HubNode *n = reg.add(mac, 1);
  TEST_ASSERT_NOT_NULL(n);
  memset(n->key, id * 0x11, 32);
  n->key_len = 32;
  snprintf(n->profile.name, sizeof(n->profile.name), "%s", name);
  n->profile.mech = mech;
  n->profile.unit = mech == 2 ? UNIT_ML : UNIT_GRAMS;
  n->profile.storage_capacity = 1500 + id;
  n->profile.crc16 = profile_crc16(&n->profile);
  n->sched.entry_count = 1;
  n->sched.entries[0].min_of_day = min_of_day;
  n->sched.entries[0].days_mask = 0x7F;
  n->sched.entries[0].amount = amount;
  n->sched.version = reg.data().sched_version;
  n->sched.pushed_at = 12345;
}

void setUp(void) {
  memset(g_buf, 0, sizeof(g_buf));
  memset(g_csv, 0, sizeof(g_csv));
}

void tearDown(void) {}

static const int64_t T_UNIX = 1793540400; /* 2026-11-01 21:40 +08:00 */

static void test_round_trip_fields(void) {
  HubRegistry reg;
  reg.data().tz_rule = TZ_MANILA;
  reg.data().stagger_max = 2;
  reg.data().feed_id_seq = 4242;
  seed_node(reg, 1, "kit A", 0, 450, 40);
  seed_node(reg, 2, "auger", 1, 455, 25);

  HubSettings set = settings_defaults();
  set.brightness = 70;
  set.screen_off_s = 120;
  set.history_days = 90;
  set.ntp_on = 0;
  snprintf(set.wifi_ssid, sizeof(set.wifi_ssid), "home-net");
  snprintf(set.wifi_pass, sizeof(set.wifi_pass), "keep-me-local");

  char name[BACKUP_NAME_LEN];
  size_t len = 0;
  RamBackupStore store(2, BACKUP_JSON_MAX);
  TEST_ASSERT_TRUE(backup_write_now(store, reg, set, T_UNIX, g_buf, sizeof(g_buf), name, &len));
  TEST_ASSERT_TRUE(len > 400);
  TEST_ASSERT_EQUAL_STRING("aio-backup-20261101-2140.json", name);

  BackupImage img;
  TEST_ASSERT_TRUE(backup_json_parse(g_buf, &img));
  TEST_ASSERT_EQUAL_UINT8(TZ_MANILA, img.tz);
  TEST_ASSERT_EQUAL_UINT8(2, img.stagger);
  TEST_ASSERT_EQUAL_UINT32(4242, img.fid);
  TEST_ASSERT_EQUAL_UINT8(70, img.brightness);
  TEST_ASSERT_EQUAL_UINT16(120, img.soff);
  TEST_ASSERT_EQUAL_UINT8(90, img.ret);
  TEST_ASSERT_EQUAL_UINT8(0, img.ntp);
  TEST_ASSERT_EQUAL_STRING("home-net", img.ssid);
  TEST_ASSERT_EQUAL_UINT8(2, img.node_count);
  TEST_ASSERT_EQUAL_UINT8(1, img.nodes[0].id);
  TEST_ASSERT_EQUAL_UINT8(0x02, img.nodes[0].mac[0]);
  TEST_ASSERT_EQUAL_UINT8(0x01, img.nodes[0].mac[5]);
  TEST_ASSERT_EQUAL_UINT8(32, img.nodes[0].key_len);
  TEST_ASSERT_EQUAL_UINT8(0x11, img.nodes[0].key[0]);
  TEST_ASSERT_EQUAL_STRING("kit A", img.nodes[0].profile.name);
  TEST_ASSERT_EQUAL_UINT8(0, img.nodes[0].profile.mech);
  TEST_ASSERT_EQUAL_UINT32(1501, img.nodes[0].profile.storage_capacity);
  TEST_ASSERT_EQUAL_UINT16(450, img.nodes[0].sched.entries[0].min_of_day);
  TEST_ASSERT_EQUAL_UINT8(0x7F, img.nodes[0].sched.entries[0].days_mask);
  TEST_ASSERT_EQUAL_UINT16(40, img.nodes[0].sched.entries[0].amount);
  TEST_ASSERT_TRUE(profile_valid(&img.nodes[0].profile));
}

static void test_backup_excludes_secrets(void) {
  HubRegistry reg;
  seed_node(reg, 1, "n1", 0, 450, 40);
  HubSettings set = settings_defaults();
  pin_set(&set, "7381", [](uint8_t *o, size_t n) { memset(o, 7, n); });
  snprintf(set.wifi_ssid, sizeof(set.wifi_ssid), "home-net");
  snprintf(set.wifi_pass, sizeof(set.wifi_pass), "topsecret");

  char name[BACKUP_NAME_LEN];
  size_t len = 0;
  RamBackupStore store(2, BACKUP_JSON_MAX);
  TEST_ASSERT_TRUE(backup_write_now(store, reg, set, T_UNIX, g_buf, sizeof(g_buf), name, &len));
  /* D18/D32: the JSON never carries the PIN hash/salt or the Wi-Fi password */
  TEST_ASSERT_EQUAL(NULL, strstr(g_buf, "topsecret"));
  TEST_ASSERT_EQUAL(NULL, strstr(g_buf, "pin"));
  TEST_ASSERT_EQUAL(NULL, strstr(g_buf, "wifi_pass"));

  /* restore into a DIFFERENT hub: its PIN + Wi-Fi password survive */
  HubRegistry reg2;
  seed_node(reg2, 9, "old", 0, 400, 10);
  HubSettings set2 = settings_defaults();
  pin_set(&set2, "1111", [](uint8_t *o, size_t n) { memset(o, 1, n); });
  snprintf(set2.wifi_ssid, sizeof(set2.wifi_ssid), "other");
  snprintf(set2.wifi_pass, sizeof(set2.wifi_pass), "otherpass");

  BackupImage img;
  TEST_ASSERT_TRUE(backup_json_parse(g_buf, &img));
  TEST_ASSERT_EQUAL(RESTORE_OK, backup_restore_apply(img, reg2, set2));
  TEST_ASSERT_EQUAL_STRING("home-net", set2.wifi_ssid);  /* from backup */
  TEST_ASSERT_EQUAL_STRING("otherpass", set2.wifi_pass); /* kept */
  TEST_ASSERT_EQUAL_UINT8(1, set2.pin_enabled);          /* PIN untouched */
  TEST_ASSERT_TRUE(pin_verify(set2, "1111"));
  TEST_ASSERT_EQUAL_UINT8(1, reg2.count());           /* nodes replaced */
  TEST_ASSERT_NOT_NULL(reg2.by_id(1));
  TEST_ASSERT_NULL(reg2.by_id(9));
}

static void test_restore_keeps_feed_seq_monotonic(void) {
  HubRegistry reg;
  seed_node(reg, 1, "n1", 0, 450, 40);
  reg.data().feed_id_seq = 9000;
  HubSettings set = settings_defaults();

  char name[BACKUP_NAME_LEN];
  size_t len = 0;
  RamBackupStore store(2, BACKUP_JSON_MAX);
  TEST_ASSERT_TRUE(backup_write_now(store, reg, set, T_UNIX, g_buf, sizeof(g_buf), name, &len));

  HubRegistry reg2;
  reg2.data().feed_id_seq = 5000;
  HubSettings set2 = settings_defaults();
  BackupImage img;
  TEST_ASSERT_TRUE(backup_json_parse(g_buf, &img));
  TEST_ASSERT_EQUAL(RESTORE_OK, backup_restore_apply(img, reg2, set2));
  TEST_ASSERT_EQUAL_UINT32(9000, reg2.data().feed_id_seq);
  /* sched_version bumped + pushed_at cleared → the hub re-pushes (§9.12) */
  TEST_ASSERT_EQUAL_UINT16(2, reg2.data().sched_version);
  const HubNode *n = reg2.by_id(1);
  TEST_ASSERT_NOT_NULL(n);
  TEST_ASSERT_EQUAL_UINT16(2, n->sched.version);
  TEST_ASSERT_EQUAL_UINT32(0, n->sched.pushed_at);
}

static void test_restore_rejects_corrupt(void) {
  RamBackupStore store(2, BACKUP_JSON_MAX);
  HubRegistry reg;
  HubSettings set = settings_defaults();
  TEST_ASSERT_EQUAL(RESTORE_BAD_JSON,
                    backup_restore_now(store, "missing.json", reg, set, g_buf, sizeof(g_buf)));

  /* truncated JSON */
  HubRegistry reg1;
  seed_node(reg1, 1, "n1", 0, 450, 40);
  char name[BACKUP_NAME_LEN];
  size_t len = 0;
  TEST_ASSERT_TRUE(backup_json_write(reg1, set, T_UNIX, g_buf, sizeof(g_buf), name, &len));
  TEST_ASSERT_TRUE(store.write("cut.json", g_buf, len / 2));
  TEST_ASSERT_EQUAL(RESTORE_BAD_JSON,
                    backup_restore_now(store, "cut.json", reg, set, g_buf, sizeof(g_buf)));

  /* unknown key refused — half-understood restores are worse than failures */
  char bad[128];
  snprintf(bad, sizeof(bad), "{\"aio_backup\":1,\"ver\":1,\"mystery\":5}");
  TEST_ASSERT_TRUE(store.write("bad.json", bad, strlen(bad)));
  TEST_ASSERT_EQUAL(RESTORE_BAD_JSON,
                    backup_restore_now(store, "bad.json", reg, set, g_buf, sizeof(g_buf)));
}

static void test_buffer_overflow_reported(void) {
  HubRegistry reg;
  seed_node(reg, 1, "n1", 0, 450, 40);
  HubSettings set = settings_defaults();
  char name[BACKUP_NAME_LEN];
  size_t len = 0;
  char tiny[64];
  TEST_ASSERT_FALSE(backup_json_write(reg, set, T_UNIX, tiny, sizeof(tiny), name, &len));
}

static void test_csv_quoting(void) {
  HubRegistry reg;
  reg.data().tz_rule = TZ_MANILA;
  seed_node(reg, 1, "kit, \"A\"", 0, 450, 40); /* comma + quotes in the name */
  seed_node(reg, 2, "auger", 1, 455, 25);
  EventLog log;
  log.push(T_UNIX - 120, 1, EV_FEED_OK, FEED_OK, 101, 40, 0, EV_SRC_MANUAL);
  log.push(T_UNIX - 60, 2, EV_FEED_REJ, FEED_REJ_COOLDOWN, 102, 25, 0, EV_SRC_SCHED);

  char name[BACKUP_NAME_LEN];
  size_t len = 0;
  TEST_ASSERT_TRUE(csv_history_write(reg, log, T_UNIX, g_csv, sizeof(g_csv), name, &len));
  TEST_ASSERT_EQUAL_STRING("aio-history-20261101-2140.csv", name);
  const char *header = "time,node,amount,unit,result,source\n";
  TEST_ASSERT_EQUAL(0, strncmp(g_csv, header, strlen(header)));
  TEST_ASSERT_NOT_EQUAL(NULL, strstr(g_csv, "\"kit, \"\"A\"\"\",40,g,OK,manual"));
  TEST_ASSERT_NOT_EQUAL(NULL, strstr(g_csv, "auger,25,g,REJ:1,scheduled"));
}

static void test_csv_export_via_store(void) {
  HubRegistry reg;
  seed_node(reg, 1, "n1", 0, 450, 40);
  EventLog log;
  log.push(T_UNIX - 10, 1, EV_FEED_OK, FEED_OK, 101, 40, 0, EV_SRC_CATCHUP);
  RamBackupStore store(2, BACKUP_JSON_MAX);
  char name[BACKUP_NAME_LEN];
  size_t len = 0;
  TEST_ASSERT_TRUE(csv_export_now(store, reg, log, T_UNIX, g_csv, sizeof(g_csv), name, &len));
  TEST_ASSERT_EQUAL_UINT8(1, store.used());
  char back[BACKUP_JSON_MAX];
  const int n = store.read(name, back, sizeof(back) - 1);
  TEST_ASSERT_TRUE(n > 0);
  back[n] = '\0';
  /* OK result row sourced from catch-up (§9.12 columns: …,result,source) */
  TEST_ASSERT_NOT_EQUAL(NULL, strstr(back, ",g,OK,catchup"));
}

static void test_ram_store_lru_and_read(void) {
  RamBackupStore store(2, 64); /* 2 slots of 64 bytes: full → oldest evicted */
  TEST_ASSERT_TRUE(store.write("a", "aaaa", 4));
  TEST_ASSERT_TRUE(store.write("b", "bbbb", 4));
  TEST_ASSERT_FALSE(store.write("c", "cc", 2)); /* full, c is not a update */
  TEST_ASSERT_TRUE(store.write("a", "a2", 2));  /* in place update */
  char out[16];
  TEST_ASSERT_EQUAL(2, store.read("a", out, sizeof(out)));
  TEST_ASSERT_EQUAL(0, memcmp(out, "a2", 2));
  TEST_ASSERT_EQUAL(-1, store.read("nope", out, sizeof(out)));
  char names[4][BACKUP_NAME_LEN];
  TEST_ASSERT_EQUAL(2, store.list(names, 4)); /* newest first: b, a */
  TEST_ASSERT_EQUAL_STRING("b", names[0]);
  TEST_ASSERT_EQUAL_STRING("a", names[1]);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip_fields);
  RUN_TEST(test_backup_excludes_secrets);
  RUN_TEST(test_restore_keeps_feed_seq_monotonic);
  RUN_TEST(test_restore_rejects_corrupt);
  RUN_TEST(test_buffer_overflow_reported);
  RUN_TEST(test_csv_quoting);
  RUN_TEST(test_csv_export_via_store);
  RUN_TEST(test_ram_store_lru_and_read);
  return UNITY_END();
}
