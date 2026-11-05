/* Host tests for the M8a OTA core (PLAN.md §7.6, §12 M8a).
 *
 * Scene: hub/src/ota_core.h — URL validation, advisory semver, the
 * esp_image streaming length scanner (fed every ugly chunking we could
 * think of), the "ota"/"ourl" NVS blobs and the brief view. The firmware
 * glue (esp_ota_ops + esp_http_client) is bench-validated; everything
 * with a wrong-answer risk on the host lives here.
 */
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include <unity.h>

#include "messages.h"
#include "ota_core.h"
#include "nodefw.h"

using namespace aio;

void setUp(void) {}
void tearDown(void) {}

struct RamStore : IHubStore {
  std::map<std::string, std::vector<uint8_t>> m;
  bool put(const char *key, const uint8_t *data, size_t len) override {
    m[key] = std::vector<uint8_t>(data, data + len);
    return true;
  }
  size_t get(const char *key, uint8_t *out, size_t max_len) override {
    auto it = m.find(key);
    if (it == m.end()) return 0;
    size_t n = it->second.size() < max_len ? it->second.size() : max_len;
    memcpy(out, it->second.data(), n);
    return n;
  }
  bool del(const char *key) override { return m.erase(key) > 0; }
};

/* --- URL validation --------------------------------------------------------------- */

static void test_url_valid_accepts_schemes(void) {
  TEST_ASSERT_TRUE(ota_url_valid("http://10.0.2.2:8000/firmware.bin"));
  TEST_ASSERT_TRUE(ota_url_valid(
      "https://mlger.github.io/aio-feeder/hub/firmware.bin"));
  TEST_ASSERT_TRUE(ota_url_valid(OTA_URL_DEF));
}

static void test_url_valid_rejects_garbage(void) {
  TEST_ASSERT_FALSE(ota_url_valid(nullptr));
  TEST_ASSERT_FALSE(ota_url_valid(""));
  TEST_ASSERT_FALSE(ota_url_valid("ftp://host/fw.bin"));
  TEST_ASSERT_FALSE(ota_url_valid("10.0.2.2/fw.bin"));
  TEST_ASSERT_FALSE(ota_url_valid("http://host/a b.bin")); /* whitespace */
  TEST_ASSERT_FALSE(ota_url_valid("http://host/a\tb.bin"));
  TEST_ASSERT_FALSE(ota_url_valid("https://host/\x01.bin"));
  std::string too_long(OTA_URL_LEN, 'a');
  TEST_ASSERT_FALSE(ota_url_valid(too_long.c_str()));
  std::string max_ok = std::string("http://h/") + std::string(OTA_URL_LEN - 10, 'a');
  TEST_ASSERT_TRUE(ota_url_valid(max_ok.c_str()));
}

/* --- advisory semver --------------------------------------------------------------- */

static void test_semver_parse(void) {
  OtaSemver v;
  TEST_ASSERT_TRUE(ota_semver("v0.11.0", &v));
  TEST_ASSERT_EQUAL_UINT8(0, v.maj);
  TEST_ASSERT_EQUAL_UINT8(11, v.min);
  TEST_ASSERT_EQUAL_UINT8(0, v.pat);
  TEST_ASSERT_TRUE(ota_semver("0.10.2-3-gdeadbeef", &v));
  TEST_ASSERT_EQUAL_UINT8(10, v.min);
  TEST_ASSERT_EQUAL_UINT8(2, v.pat);
  TEST_ASSERT_TRUE(ota_semver("v1.2.3", &v));
  TEST_ASSERT_EQUAL_UINT8(1, v.maj);
}

static void test_semver_parse_rejects(void) {
  OtaSemver v;
  TEST_ASSERT_FALSE(ota_semver(nullptr, &v));
  TEST_ASSERT_FALSE(ota_semver("", &v));
  TEST_ASSERT_FALSE(ota_semver("release", &v));
  TEST_ASSERT_FALSE(ota_semver("v1.2", &v));
  TEST_ASSERT_FALSE(ota_semver("v256.0.0", &v)); /* field overflow */
}

static void test_semver_compare(void) {
  OtaSemver a, b;
  TEST_ASSERT_TRUE(ota_semver("v0.11.0", &a));
  TEST_ASSERT_TRUE(ota_semver("v0.10.9", &b));
  TEST_ASSERT_TRUE(ota_semver_cmp(a, b) > 0);
  TEST_ASSERT_TRUE(ota_ver_newer("v0.10.9", "v0.11.0"));
  TEST_ASSERT_TRUE(ota_ver_newer("0.10.0", "0.10.1"));
  TEST_ASSERT_FALSE(ota_ver_newer("v0.11.0", "v0.11.0")); /* equal */
  TEST_ASSERT_FALSE(ota_ver_newer("v0.11.0", "v0.10.9")); /* older */
  TEST_ASSERT_FALSE(ota_ver_newer("v0.11.0", "3-gabc"));  /* unparseable */
  TEST_ASSERT_FALSE(ota_ver_newer("dev", "v99.0.0"));     /* cur unparseable */
}

/* --- image length scanner ----------------------------------------------------------- */

/* Build a synthetic esp_image: header(24) + segments + padding + checksum
 * byte (+32 B hash). Returns the length the IDF parser would report. */
static uint32_t make_image(std::vector<uint8_t> *img, const uint32_t *seg_lens,
                           int segs, bool hash) {
  img->clear();
  img->push_back(0xE9);
  img->push_back((uint8_t)segs);
  img->push_back(2); /* spi_mode */
  img->push_back(0x22);
  for (int i = 0; i < 4; i++) img->push_back(0x40); /* entry */
  for (int i = 0; i < 16; i++) img->push_back(0);   /* rest of header */
  img->back() = hash ? 1 : 0;                       /* byte 23: hash_appended */
  uint32_t off = 24;
  for (int s = 0; s < segs; s++) {
    img->push_back(0x00); /* load addr */
    img->push_back(0x10);
    img->push_back(0x00);
    img->push_back(0x40);
    img->push_back(seg_lens[s] & 0xFF);
    img->push_back((seg_lens[s] >> 8) & 0xFF);
    img->push_back((seg_lens[s] >> 16) & 0xFF);
    img->push_back((seg_lens[s] >> 24) & 0xFF);
    off += 8;
    for (uint32_t i = 0; i < seg_lens[s]; i++) {
      img->push_back((uint8_t)(i ^ 0xA5));
      off++;
    }
  }
  img->push_back(0x5A); /* checksum byte right after the data */
  off++;
  while (off % 16) { /* 0xEF pad to the next 16-byte block — IDF pads AFTER
                      * the checksum (esp_image_format.c process_checksum) */
    img->push_back(0xEF);
    off++;
  }
  if (hash) {
    for (int i = 0; i < 32; i++) img->push_back((uint8_t)i);
    off += 32;
  }
  return off;
}

static void scan_all(OtaImgScan *s, const std::vector<uint8_t> &img,
                     size_t chunk) {
  for (size_t i = 0; i < img.size(); i += chunk)
    ota_img_scan(s, img.data() + i, chunk < img.size() - i ? chunk : img.size() - i);
}

static void test_scan_two_segments_hash(void) {
  std::vector<uint8_t> img;
  const uint32_t lens[2] = {5, 3};
  const uint32_t expect = make_image(&img, lens, 2, true);
  TEST_ASSERT_EQUAL_UINT(96, expect); /* 24+13+11=48, checksum+pad→64, +32 */
  TEST_ASSERT_EQUAL_UINT(96, img.size());

  OtaImgScan s;
  scan_all(&s, img, 7); /* ugly chunking across all boundaries */
  TEST_ASSERT_FALSE(s.bad);
  TEST_ASSERT_TRUE(s.done);
  TEST_ASSERT_EQUAL_UINT32(expect, s.img_len);
  TEST_ASSERT_TRUE(s.hash_appended);
}

static void test_scan_one_segment_nohash_odd_chunks(void) {
  std::vector<uint8_t> img;
  const uint32_t lens[1] = {19};
  const uint32_t expect = make_image(&img, lens, 1, false);
  /* 24 + 8 + 19 = 51 → checksum at 51, pad to 64 → 64 */
  TEST_ASSERT_EQUAL_UINT(64, expect);

  const size_t chunks[] = {1, 3, 13, 24, 1000};
  for (size_t c : chunks) {
    OtaImgScan s;
    scan_all(&s, img, c);
    TEST_ASSERT_FALSE(s.bad);
    TEST_ASSERT_TRUE(s.done);
    TEST_ASSERT_EQUAL_UINT32(expect, s.img_len);
    TEST_ASSERT_FALSE(s.hash_appended);
  }
}

static void test_scan_zero_len_segment(void) {
  std::vector<uint8_t> img;
  const uint32_t lens[2] = {0, 4};
  const uint32_t expect = make_image(&img, lens, 2, true);
  OtaImgScan s;
  scan_all(&s, img, 5);
  TEST_ASSERT_TRUE(s.done);
  TEST_ASSERT_FALSE(s.bad);
  TEST_ASSERT_EQUAL_UINT32(expect, s.img_len);
}

static void test_scan_bad_magic_and_bad_counts(void) {
  std::vector<uint8_t> img;
  const uint32_t lens[1] = {8};
  (void)make_image(&img, lens, 1, false);
  img[0] = 0xEA;
  OtaImgScan s;
  scan_all(&s, img, 64);
  TEST_ASSERT_TRUE(s.bad);
  TEST_ASSERT_FALSE(s.done);

  (void)make_image(&img, lens, 1, false);
  img[1] = 0; /* segment count 0 */
  OtaImgScan s2;
  scan_all(&s2, img, 64);
  TEST_ASSERT_TRUE(s2.bad);

  (void)make_image(&img, lens, 1, false);
  img[1] = 17; /* over the 16-segment cap */
  OtaImgScan s3;
  scan_all(&s3, img, 64);
  TEST_ASSERT_TRUE(s3.bad);
}

static void test_scan_incomplete_never_done(void) {
  std::vector<uint8_t> img;
  const uint32_t lens[2] = {1000, 1000};
  (void)make_image(&img, lens, 2, true);
  OtaImgScan s;
  /* feed only the first 300 bytes — no done, no bad */
  ota_img_scan(&s, img.data(), 300);
  TEST_ASSERT_FALSE(s.done);
  TEST_ASSERT_FALSE(s.bad);
}

/* --- status + URL blobs --------------------------------------------------------------- */

static void test_status_blob_roundtrip(void) {
  RamStore st;
  OtaStatus s;
  TEST_ASSERT_FALSE(ota_status_load(st, &s));

  s.version = OTA_STATUS_VERSION;
  s.last_result = OTA_RES_OK;
  s.last_err = OTA_ERR_NONE;
  snprintf(s.last_ver, sizeof(s.last_ver), "v0.11.0");
  s.last_unix = 1735689600;
  TEST_ASSERT_TRUE(ota_status_save(st, s));

  OtaStatus r;
  TEST_ASSERT_TRUE(ota_status_load(st, &r));
  TEST_ASSERT_EQUAL_UINT8(OTA_RES_OK, r.last_result);
  TEST_ASSERT_EQUAL_STRING("v0.11.0", r.last_ver);
  TEST_ASSERT_EQUAL_UINT32(1735689600, r.last_unix);

  /* corruption → rejected */
  std::vector<uint8_t> &blob = st.m[std::string(OTA_STORE_KEY)];
  blob[6] ^= 0xFF; /* payload bytes follow the 6-byte header */
  TEST_ASSERT_FALSE(ota_status_load(st, &r));
}

static void test_url_blob_roundtrip_and_validation(void) {
  RamStore st;
  char url[OTA_URL_LEN];
  TEST_ASSERT_FALSE(ota_url_load(st, url, sizeof(url)));

  TEST_ASSERT_TRUE(ota_url_save(st, "http://10.0.2.2:8000/firmware.bin"));
  TEST_ASSERT_TRUE(ota_url_load(st, url, sizeof(url)));
  TEST_ASSERT_EQUAL_STRING("http://10.0.2.2:8000/firmware.bin", url);

  TEST_ASSERT_FALSE(ota_url_save(st, "not a url"));
  TEST_ASSERT_FALSE(ota_url_save(st, ""));
  TEST_ASSERT_TRUE(ota_url_load(st, url, sizeof(url))); /* unchanged */
}

static void test_brief_phase_names(void) {
  TEST_ASSERT_EQUAL_STRING("idle", ota_phase_name(OTA_IDLE));
  TEST_ASSERT_EQUAL_STRING("downloading", ota_phase_name(OTA_DOWNLOAD));
  TEST_ASSERT_EQUAL_STRING("need reboot", ota_phase_name(OTA_NEED_REBOOT));
  TEST_ASSERT_EQUAL_STRING("pending verify", ota_phase_name(OTA_PENDING));
  TEST_ASSERT_EQUAL_STRING("valid", ota_phase_name(OTA_VALID));
  TEST_ASSERT_EQUAL_STRING("failed", ota_phase_name(OTA_FAIL));
  TEST_ASSERT_EQUAL_STRING("?", ota_phase_name(OTA_PHASE_COUNT));
  TEST_ASSERT_EQUAL_STRING("setboot", ota_err_name(OTA_ERR_SETBOOT));
}

/* --- M8b: nodefw meta store + helpers --------------------------------------- */

static void test_nodefw_store_roundtrip(void) {
  RamStore st;
  NodeFw nfw;
  TEST_ASSERT_FALSE(nfw.img_valid());
  TEST_ASSERT_FALSE(nfw.load(st)); /* fresh blob → reset */

  NodeFwImage img{};
  img.magic = NFW_IMAGE_MAGIC;
  img.size = 412345;
  img.target = NFW_TARGET_C3;
  strcpy(img.ver, "0.12.0");
  for (int i = 0; i < 32; i++) img.sha[i] = (uint8_t)i;
  nfw.img_set(img);
  nfw.hello(1, 0x000C00, MCU_ESP32C3);
  nfw.pushed(1, 1700000000);
  nfw.ack(1, 1, OTA_ACK_OK);
  TEST_ASSERT_TRUE(nfw.save(st));

  NodeFw nfw2;
  TEST_ASSERT_TRUE(nfw2.load(st));
  TEST_ASSERT_TRUE(nfw2.img_valid());
  TEST_ASSERT_EQUAL_UINT32(412345, nfw2.img().size);
  TEST_ASSERT_EQUAL_UINT8(NFW_TARGET_C3, nfw2.img().target);
  TEST_ASSERT_EQUAL_STRING("0.12.0", nfw2.img().ver);
  const NodeFwNode *n = nfw2.node(1);
  TEST_ASSERT_NOT_NULL(n);
  TEST_ASSERT_EQUAL_UINT32(0x000C00, n->rel_ver);
  TEST_ASSERT_EQUAL_UINT8(MCU_ESP32C3, n->mcu);
  TEST_ASSERT_EQUAL_UINT8(1, n->ack_ok);
  TEST_ASSERT_EQUAL_UINT32(1700000000, n->push_unix);
  TEST_ASSERT_NULL(nfw2.node(9));

  nfw2.node_removed(1);
  TEST_ASSERT_NULL(nfw2.node(1));
}

static void test_nodefw_helpers(void) {
  TEST_ASSERT_EQUAL_STRING("esp8266", nodefw_target_name(NFW_TARGET_8266));
  TEST_ASSERT_EQUAL_STRING("esp32c3", nodefw_target_name(NFW_TARGET_C3));
  TEST_ASSERT_EQUAL_STRING("none", nodefw_target_name(NFW_TARGET_NONE));
  char v[16];
  nodefw_ver_str(0x000C00, v, sizeof(v));
  TEST_ASSERT_EQUAL_STRING("0.12.0", v);
  nodefw_ver_str(0, v, sizeof(v));
  TEST_ASSERT_EQUAL_STRING("?", v);
  nodefw_ver_str(0x00010A, v, sizeof(v));
  TEST_ASSERT_EQUAL_STRING("0.1.10", v);

  TEST_ASSERT_TRUE(nodefw_image_plausible(400000, NFW_TARGET_8266));
  TEST_ASSERT_TRUE(nodefw_image_plausible(900000, NFW_TARGET_C3));
  TEST_ASSERT_FALSE(nodefw_image_plausible(500, NFW_TARGET_C3));
  TEST_ASSERT_FALSE(nodefw_image_plausible(2000000, NFW_TARGET_C3));
  TEST_ASSERT_FALSE(nodefw_image_plausible(400000, 0));

  uint8_t sha[32];
  memset(sha, 0xAB, 32);
  char hex[65];
  nodefw_sha_hex(sha, hex, sizeof(hex));
  TEST_ASSERT_EQUAL(64, strlen(hex));
  TEST_ASSERT_EQUAL_STRING("abababababababababababababababab"
                           "abababababababababababababababab",
                           hex);
}

/* --- M8b: ENTER_OTA wire struct --------------------------------------------- */

static void test_enter_ota_wire(void) {
  /* blob = ssid 32 + pass 64 + url 98; fixed 234-byte payload exactly fits */
  TEST_ASSERT_EQUAL(234, (int)sizeof(enter_ota_payload_t));
  TEST_ASSERT_TRUE(sizeof(enter_ota_payload_t) <= FRAME_MAX_PAYLOAD);

  /* wrap/unwrap round-trip over a full-length blob (3 keystream blocks) */
  uint8_t key[32], seed[4] = {1, 2, 3, 4};
  for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x30 + i);
  uint8_t buf[OTA_SSID_MAX + OTA_PASS_MAX + OTA_URL_MAX];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i & 0xFF);
  uint8_t plain[sizeof(buf)];
  memcpy(plain, buf, sizeof(buf));
  ota_wrap(key, seed, buf, sizeof(buf));
  TEST_ASSERT_NOT_EQUAL(0, memcmp(buf, plain, sizeof(buf)));
  ota_wrap(key, seed, buf, sizeof(buf)); /* symmetric */
  TEST_ASSERT_EQUAL_MEMORY(plain, buf, sizeof(buf));

  /* a different seed must not unwrap the same ciphertext */
  uint8_t wrong[sizeof(buf)];
  memcpy(wrong, buf, sizeof(buf));
  const uint8_t seed2[4] = {1, 2, 3, 5};
  ota_wrap(key, seed2, wrong, sizeof(wrong));
  TEST_ASSERT_NOT_EQUAL(0, memcmp(wrong, plain, sizeof(plain)));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_url_valid_accepts_schemes);
  RUN_TEST(test_url_valid_rejects_garbage);
  RUN_TEST(test_semver_parse);
  RUN_TEST(test_semver_parse_rejects);
  RUN_TEST(test_semver_compare);
  RUN_TEST(test_scan_two_segments_hash);
  RUN_TEST(test_scan_one_segment_nohash_odd_chunks);
  RUN_TEST(test_scan_zero_len_segment);
  RUN_TEST(test_scan_bad_magic_and_bad_counts);
  RUN_TEST(test_scan_incomplete_never_done);
  RUN_TEST(test_status_blob_roundtrip);
  RUN_TEST(test_url_blob_roundtrip_and_validation);
  RUN_TEST(test_brief_phase_names);
  RUN_TEST(test_nodefw_store_roundtrip);
  RUN_TEST(test_nodefw_helpers);
  RUN_TEST(test_enter_ota_wire);
  return UNITY_END();
}
