/* Host tests for the shared entry-pad model (PLAN.md §12 M5b).
 *
 * Covers: target kinds, seed/commit round-trips, digit limits, decimal
 * (×100) parsing, HHMM time parsing with clamping, backspace/clear, and
 * the preview formatting the view renders.
 */
#include <string.h>

#include <unity.h>

#include "ui_entry.h"

using namespace aio;
using aio::entry::Pad;
using aio::entry::Target;

void setUp(void) {}
void tearDown(void) {}

/* --- kinds & limits ----------------------------------------------------- */

static void test_kinds_and_limits(void) {
  TEST_ASSERT_EQUAL(entry::K_TEXT, entry::kind_of(entry::ENT_PAIR_NAME));
  TEST_ASSERT_EQUAL(entry::K_TEXT, entry::kind_of(entry::ENT_WIZ_SSID));
  TEST_ASSERT_EQUAL(entry::K_TIME, entry::kind_of(entry::ENT_WIZ_TIME));
  TEST_ASSERT_EQUAL(entry::K_TIME, entry::kind_of(entry::ENT_EDIT_TIME));
  TEST_ASSERT_EQUAL(entry::K_X100, entry::kind_of(entry::ENT_CAL_WEIGH));
  TEST_ASSERT_EQUAL(entry::K_UINT, entry::kind_of(entry::ENT_PAIR_TOTAL));
  TEST_ASSERT_EQUAL(entry::K_UINT, entry::kind_of(entry::ENT_EDIT_AMOUNT));

  TEST_ASSERT_EQUAL(12, entry::max_of(entry::ENT_PAIR_NAME));
  TEST_ASSERT_EQUAL(32, entry::max_of(entry::ENT_WIZ_SSID));
  TEST_ASSERT_EQUAL(1439, entry::max_of(entry::ENT_EDIT_TIME));
  TEST_ASSERT_EQUAL(999, entry::max_of(entry::ENT_EDIT_AMOUNT));
}

/* --- text pad ----------------------------------------------------------- */

static void test_text_push_and_cap(void) {
  Pad p;
  p.open(entry::ENT_PAIR_NAME, "kitA", nullptr);
  TEST_ASSERT_TRUE(p.is_open());
  TEST_ASSERT_EQUAL_STRING("kitA", p.text);
  p.push_char('b');
  p.push_char('c');
  TEST_ASSERT_EQUAL_STRING("kitAbc", p.text);
  p.backspace();
  TEST_ASSERT_EQUAL_STRING("kitAb", p.text);

  /* 12-char cap on the pairing name */
  p.clear_all();
  for (int i = 0; i < 20; i++) p.push_char('x');
  TEST_ASSERT_EQUAL_UINT32(12, strlen(p.text));
}

static void test_text_seed_truncates_to_target_cap(void) {
  Pad p;
  p.open(entry::ENT_PAIR_NAME, "0123456789012345", nullptr);
  TEST_ASSERT_EQUAL_UINT32(12, strlen(p.text));
  /* the wizard SSID keeps 32 */
  p.open(entry::ENT_WIZ_SSID, "0123456789012345678901234567890123456789", nullptr);
  TEST_ASSERT_EQUAL_UINT32(32, strlen(p.text));
}

static void test_text_ignores_control_bytes(void) {
  Pad p;
  p.open(entry::ENT_WIZ_SSID, "", nullptr);
  p.push_char('a');
  p.push_char((char)0x1B);
  p.push_char((char)0x7F);
  p.push_char('b');
  TEST_ASSERT_EQUAL_STRING("ab", p.text);
}

/* --- numeric pad -------------------------------------------------------- */

static void test_uint_parse_and_clamp(void) {
  Pad p;
  p.open(entry::ENT_PAIR_TOTAL, nullptr, "1500");
  TEST_ASSERT_EQUAL_UINT32(1500, p.value());
  p.push_char('0');
  TEST_ASSERT_EQUAL_UINT32(15000, p.value());
  /* capacity ceiling 100000 */
  p.clear_all();
  for (int i = 0; i < 8; i++) p.push_char('9');
  TEST_ASSERT_EQUAL_UINT32(100000, p.value());
}

static void test_amount_clamps_at_999(void) {
  Pad p;
  p.open(entry::ENT_EDIT_AMOUNT, nullptr, "40");
  p.push_char('0');
  p.push_char('0'); /* 4000 → clamped at the editor's ceiling */
  TEST_ASSERT_EQUAL_UINT32(999, p.value());
}

static void test_x100_parsing(void) {
  Pad p;
  p.open(entry::ENT_CAL_WEIGH, nullptr, "");
  p.push_char('8');
  TEST_ASSERT_EQUAL_UINT32(800, p.value()); /* 8 → 8.00 */
  p.push_char('.');
  p.push_char('2');
  TEST_ASSERT_EQUAL_UINT32(820, p.value()); /* 8.2 → 8.20 */
  p.push_char('5');
  TEST_ASSERT_EQUAL_UINT32(825, p.value());
  p.push_char('9'); /* a third decimal is dropped */
  TEST_ASSERT_EQUAL_UINT32(825, p.value());
  /* no second '.' sneaks in */
  p.clear_all();
  p.push_char('1');
  p.push_char('.');
  p.push_char('.');
  p.push_char('5');
  TEST_ASSERT_EQUAL_UINT32(150, p.value());
}

static void test_time_parsing_and_clamp(void) {
  Pad p;
  p.open(entry::ENT_EDIT_TIME, nullptr, "0730");
  TEST_ASSERT_EQUAL_UINT32(7 * 60 + 30, p.time_value());

  /* "730" zero-pads to 07:30 */
  p.open(entry::ENT_EDIT_TIME, nullptr, "");
  p.push_char('7');
  p.push_char('3');
  p.push_char('0');
  TEST_ASSERT_EQUAL_UINT32(7 * 60 + 30, p.time_value());

  /* clamps: 9959 → 23:59 */
  p.clear_all();
  p.push_char('9');
  p.push_char('9');
  p.push_char('5');
  p.push_char('9');
  TEST_ASSERT_EQUAL_UINT32(23 * 60 + 59, p.time_value());

  /* minutes clamp 80 → 59 */
  p.clear_all();
  p.push_char('0');
  p.push_char('7');
  p.push_char('8');
  p.push_char('0');
  TEST_ASSERT_EQUAL_UINT32(7 * 60 + 59, p.time_value());

  /* empty → 00:00 */
  p.clear_all();
  TEST_ASSERT_EQUAL_UINT32(0, p.time_value());
}

/* --- pad lifecycle ------------------------------------------------------ */

static void test_open_close_and_empty_push(void) {
  Pad p;
  TEST_ASSERT_FALSE(p.is_open());
  TEST_ASSERT_EQUAL_UINT32(0, p.value());
  p.push_char('5'); /* closed: ignored */
  TEST_ASSERT_EQUAL_UINT32(0, p.value());

  p.open(entry::ENT_WIZ_NOW, nullptr, "400");
  TEST_ASSERT_TRUE(p.is_open());
  p.close();
  TEST_ASSERT_FALSE(p.is_open());
  TEST_ASSERT_EQUAL_UINT32(0, p.value());
}

static void test_backspace_on_empty_is_safe(void) {
  Pad p;
  p.open(entry::ENT_WIZ_TIME, nullptr, "");
  p.backspace();
  p.backspace();
  TEST_ASSERT_EQUAL_UINT32(0, p.time_value());
  p.open(entry::ENT_PAIR_NAME, "", nullptr);
  p.backspace();
  TEST_ASSERT_EQUAL_UINT32(0, strlen(p.text));
}

static void test_preview_formatting(void) {
  char buf[48];
  Pad p;
  p.open(entry::ENT_EDIT_TIME, nullptr, "");
  p.push_char('0');
  TEST_ASSERT_EQUAL_STRING("0_:__", p.preview(buf, sizeof(buf)));
  p.push_char('7');
  p.push_char('3');
  TEST_ASSERT_EQUAL_STRING("07:3_", p.preview(buf, sizeof(buf)));

  p.open(entry::ENT_CAL_WEIGH, nullptr, "");
  TEST_ASSERT_EQUAL_STRING(".00", p.preview(buf, sizeof(buf)));
  p.push_char('8');
  TEST_ASSERT_EQUAL_STRING("8.00", p.preview(buf, sizeof(buf)));

  p.open(entry::ENT_PAIR_TOTAL, nullptr, "");
  p.push_char('1');
  p.push_char('5');
  TEST_ASSERT_EQUAL_STRING("15", p.preview(buf, sizeof(buf)));

  p.open(entry::ENT_PAIR_NAME, "kit", nullptr);
  TEST_ASSERT_EQUAL_STRING("kit", p.preview(buf, sizeof(buf)));
}

static void test_closed_pad_preview_empty(void) {
  char buf[48];
  Pad p;
  buf[0] = 'x';
  p.preview(buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("", buf);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_kinds_and_limits);
  RUN_TEST(test_text_push_and_cap);
  RUN_TEST(test_text_seed_truncates_to_target_cap);
  RUN_TEST(test_text_ignores_control_bytes);
  RUN_TEST(test_uint_parse_and_clamp);
  RUN_TEST(test_amount_clamps_at_999);
  RUN_TEST(test_x100_parsing);
  RUN_TEST(test_time_parsing_and_clamp);
  RUN_TEST(test_open_close_and_empty_push);
  RUN_TEST(test_backspace_on_empty_is_safe);
  RUN_TEST(test_preview_formatting);
  RUN_TEST(test_closed_pad_preview_empty);
  return UNITY_END();
}
