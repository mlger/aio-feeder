/* Wire-format guard tests for lib/proto (PLAN.md §6.5).
 * The frame layout is a cross-vendor contract (hub S3 ↔ ESP8266 ↔ C3);
 * if any of these break, the wire protocol broke. */
#include <stddef.h>
#include <unity.h>
#include "proto.h"

void setUp(void) {}
void tearDown(void) {}

void test_frame_layout_is_wire_stable(void) {
  TEST_ASSERT_EQUAL_UINT(8, FRAME_HEADER_SIZE);
  TEST_ASSERT_EQUAL_UINT(8, FRAME_MAC_SIZE);
  TEST_ASSERT_EQUAL_UINT(250, ESPNOW_MAX_PAYLOAD);
  TEST_ASSERT_EQUAL_UINT(234, FRAME_MAX_PAYLOAD);
  /* header fields: ver, type, node_id, flags, seq(2), len(2) = 8 */
  TEST_ASSERT_EQUAL_UINT(8, offsetof(frame_t, payload));
  TEST_ASSERT_EQUAL_UINT(8 + FRAME_MAX_PAYLOAD, offsetof(frame_t, mac));
  TEST_ASSERT_EQUAL_UINT(250, sizeof(frame_t));
}

void test_feed_payload_layout(void) {
  TEST_ASSERT_EQUAL_UINT(8, sizeof(feed_payload_t));
  TEST_ASSERT_EQUAL_UINT(4, offsetof(feed_payload_t, amount));
}

void test_proto_version_is_1(void) {
  TEST_ASSERT_EQUAL_INT(1, PROTO_VER);
}

void test_msg_type_values_are_stable(void) {
  TEST_ASSERT_EQUAL_INT(MSG_HELLO, 0);
  TEST_ASSERT_EQUAL_INT(MSG_FEED, 4);
  TEST_ASSERT_EQUAL_INT(MSG_HEARTBEAT, 13);
  /* append-only history: v0.11 tail was TARE_OR_CAL; M8b added two more */
  TEST_ASSERT_EQUAL_INT(MSG_TARE_OR_CAL, 16);
  TEST_ASSERT_EQUAL_INT(MSG_ENTER_OTA, 17);
  TEST_ASSERT_EQUAL_INT(MSG_ENTER_OTA_ACK, 18);
  TEST_ASSERT_EQUAL_INT(MSG_ENTER_OTA_ACK, MSG_TYPE_COUNT - 1);
}

void test_heartbeat_flag_bits_unique(void) {
  int bits = HB_FLAG_MANUAL_STOP | HB_FLAG_TIME_INVALID | HB_FLAG_LIMITS_UNCONFIRMED |
             HB_FLAG_RTC_OK | HB_FLAG_RTC_LOST | HB_FLAG_OTA_LOCK;
  int count = 0;
  while (bits) { count += bits & 1; bits >>= 1; }
  TEST_ASSERT_EQUAL_INT(6, count);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_frame_layout_is_wire_stable);
  RUN_TEST(test_feed_payload_layout);
  RUN_TEST(test_proto_version_is_1);
  RUN_TEST(test_msg_type_values_are_stable);
  RUN_TEST(test_heartbeat_flag_bits_unique);
  return UNITY_END();
}
