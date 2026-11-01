/*
 * AIO Feeder — shared protocol definitions (single source of truth).
 * Host-testable: plain C, no MCU headers. See PLAN.md §6.5 / §6.6.
 *
 * One frame = one ESP-NOW payload (or UDP/MQTT body in fallback transport).
 * Wire layout: 8-byte header (ver,type,node_id,flags,seq,len) + payload +
 * 8-byte truncated HMAC trailer; payload caps at ESPNOW_MAX_PAYLOAD - 16.
 */
#ifndef AIO_PROTO_H
#define AIO_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_VER 1

/* ESP-NOW max payload is 250 bytes; header 8 + HMAC 8 = 16 (PLAN.md §6.5). */
#define FRAME_HEADER_SIZE 8
#define FRAME_MAC_SIZE 8
#define ESPNOW_MAX_PAYLOAD 250
#define FRAME_MAX_PAYLOAD (ESPNOW_MAX_PAYLOAD - FRAME_HEADER_SIZE - FRAME_MAC_SIZE)

typedef enum {
  MSG_HELLO = 0,
  MSG_HELLO_ACK,
  MSG_PAIR_REQ,
  MSG_PAIR_ACCEPT,
  MSG_FEED,
  MSG_FEED_ACK,
  MSG_FEED_RESULT,
  MSG_CONFIG,
  MSG_CONFIG_ACK,
  MSG_SCHEDULE_SET,
  MSG_SCHEDULE_ACK,
  MSG_TIME_SYNC,
  MSG_STATUS_REQ,
  MSG_HEARTBEAT,
  MSG_ABORT,
  MSG_TARE_OR_CAL,
  MSG_TYPE_COUNT
} msg_type_t;

/* FEED_RESULT.status values (PLAN.md §9.2). */
typedef enum {
  RESULT_OK = 0,
  RESULT_FAIL,
  RESULT_TIMEOUT_UNKNOWN,
  RESULT_COUNT
} result_status_t;

/* Node result codes for FEED_RESULT / FEED_ACK.reason (PLAN.md §9.2). */
typedef enum {
  FEED_OK = 0,
  FEED_REJ_COOLDOWN,
  FEED_REJ_LIMIT_HOUR,
  FEED_REJ_LIMIT_DAY,
  FEED_REJ_TOO_LONG,
  FEED_REJ_NO_TIME,
  FEED_REJ_FAULT,
  FEED_REJ_LOW_VOLT,
  FEED_REJ_LIMITS_UNCONFIRMED,
  FEED_REJ_MANUAL_STOP,
  FEED_ABORTED,
  FEED_GATE_NOT_OPENED,
  FEED_GATE_STUCK_OPEN,
} feed_result_t;

/* HEARTBEAT.flags bits (PLAN.md §6.5). */
#define HB_FLAG_MANUAL_STOP 0x01
#define HB_FLAG_TIME_INVALID 0x02
#define HB_FLAG_LIMITS_UNCONFIRMED 0x04
#define HB_FLAG_RTC_OK 0x08
#define HB_FLAG_RTC_LOST 0x10

typedef struct __attribute__((packed)) {
  uint8_t ver;
  uint8_t type;
  uint8_t node_id;
  uint8_t flags;
  uint16_t seq;   /* per-sender, monotonic */
  uint16_t len;   /* payload bytes */
  uint8_t payload[FRAME_MAX_PAYLOAD];
  uint8_t mac[FRAME_MAC_SIZE]; /* truncated HMAC over header+payload */
} frame_t;

/* FEED payload (PLAN.md §6.5). */
typedef struct __attribute__((packed)) {
  uint32_t feed_id;
  uint16_t amount;
  uint8_t unit;
  uint8_t max_ms_override_forbidden;
} feed_payload_t;

/* FEED_ACK payload. */
typedef struct __attribute__((packed)) {
  uint32_t feed_id;
  uint8_t accepted;
  uint8_t reason;
} feed_ack_payload_t;

/* C11 _Static_assert vs C++11 static_assert — one spelling per language. */
#if defined(__cplusplus)
#define AIO_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define AIO_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

AIO_STATIC_ASSERT(sizeof(frame_t) == FRAME_HEADER_SIZE + FRAME_MAX_PAYLOAD + FRAME_MAC_SIZE,
                  "frame_t layout drifted");
AIO_STATIC_ASSERT(sizeof(feed_payload_t) == 8, "feed_payload_t layout drifted");

#ifdef __cplusplus
}
#endif

#endif /* AIO_PROTO_H */
