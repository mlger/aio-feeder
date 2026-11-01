/*
 * AIO Feeder — M2 message payload layouts (PLAN.md §6.5, §9.6, §9.9).
 * Wire structs only: packed, host-testable, one source of truth for hub and
 * nodes. Frame envelope (header/HMAC) lives in proto.h.
 *
 * Key rule (§6.4/D13): discovery frames (HELLO, HELLO_ACK, PAIR_REQ,
 * PAIR_ACCEPT) are MACed with the all-zero key because unpaired peers share
 * no secret yet; every other frame uses the per-peer key.
 */
#ifndef AIO_MESSAGES_H
#define AIO_MESSAGES_H

#include <stdint.h>
#include <string.h>
#include "proto.h"
#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HELLO (node → broadcast): who am I. state: 0 = unpaired, 1 = paired. */
typedef struct __attribute__((packed)) {
  uint16_t fw_ver;
  uint8_t caps;   /* PROFILE_FLAG_* bits */
  uint8_t state;
} hello_payload_t;

/* HELLO_ACK (hub → node): channel assignment / discovery answer.
 * node_id 0 = "seen, not assigned yet" (full pairing is M3). */
typedef struct __attribute__((packed)) {
  uint8_t paired;
  uint8_t node_id;
  uint8_t channel;
} hello_ack_payload_t;

/* TIME_SYNC (hub → node): unix seconds (valid to 2106) + tz rule id. */
typedef struct __attribute__((packed)) {
  uint32_t unix_time;
  uint8_t tz_rule;
} time_sync_payload_t;

/* HEARTBEAT (node → hub), PLAN.md §6.5. flags = HB_FLAG_*. */
typedef struct __attribute__((packed)) {
  uint32_t uptime_ms;
  int8_t rssi;            /* dBm of last hub frame; 0 = unknown on these SDKs */
  uint16_t vin_mv;
  uint8_t hopper_low;
  uint32_t last_feed_id;
  uint16_t day_units;
  uint8_t flags;
  int32_t rtc_drift_ms;   /* RTC offset measured at last TIME_SYNC */
} heartbeat_payload_t;

/* FEED_RESULT (node → hub): dispensed truth (§9.7 uses actual_on_ms/pulses). */
typedef struct __attribute__((packed)) {
  uint32_t feed_id;
  uint8_t status;        /* feed_result_t */
  uint16_t actual_on_ms;
  uint16_t pulses;
  uint8_t gate_ok;       /* 0xFF = no gate sensor */
  uint32_t t_unix;       /* 0 = node has no valid time */
} feed_result_payload_t;

/* ABORT (hub → node). */
typedef struct __attribute__((packed)) {
  uint32_t feed_id;
} abort_payload_t;

/* SCHEDULE_ACK / CONFIG_ACK (node → hub). */
typedef struct __attribute__((packed)) {
  uint16_t version;
  uint8_t ok;
} ack_status_payload_t;

/* SCHEDULE_SET (hub → node): recurring entries + skip/hold windows (§9.6,
 * §9.9). days_mask bit0 = Sunday … bit6 = Saturday (tm_wday order). */
typedef struct __attribute__((packed)) {
  uint16_t min_of_day;
  uint8_t days_mask;
  uint16_t amount;
} sched_entry_t;

typedef struct __attribute__((packed)) {
  uint16_t version;
  uint8_t entry_count;
  uint32_t skip_until;   /* unix; scheduled feeds before this are SKIPPED_USER */
  uint32_t hold_until;   /* unix; scheduled feeds before this are SKIPPED_HOLD */
} schedule_hdr_t;

#define SCHEDULE_MAX_ENTRIES \
  ((FRAME_MAX_PAYLOAD - sizeof(schedule_hdr_t)) / sizeof(sched_entry_t))

/* PAIR_REQ (node → broadcast, M3 flow) and PAIR_ACCEPT (hub → node). The key
 * is wrapped with a stream derived from both nonces + the 4-digit code the
 * user confirms (§6.3, D14) — see pairing.h. */
typedef struct __attribute__((packed)) {
  uint16_t fw_ver;
  uint8_t caps;
  uint8_t node_nonce[8];
} pair_req_payload_t;

typedef struct __attribute__((packed)) {
  uint8_t node_id;
  uint8_t channel;
  uint8_t code_digits[4];   /* ASCII '0'-'9' so the user can compare */
  uint8_t hub_nonce[8];
  uint8_t wrapped_key[32];
} pair_accept_payload_t;

/* Frame MAC domain (§6.4): discovery frames use the all-zero key. */
static const uint8_t AIO_ZERO_KEY[32] = {0};
#define FRAME_MAC_DATA_MAX (FRAME_HEADER_SIZE + FRAME_MAX_PAYLOAD)

/* mac = first 8 bytes of HMAC-SHA256(key, header ++ payload) */
static inline void frame_compute_mac(const uint8_t key[32], const uint8_t *header_and_payload,
                                     size_t hp_len, uint8_t mac_out[8]) {
  uint8_t digest[SHA256_DIGEST];
  hmac_sha256(key, 32, header_and_payload, hp_len, digest);
  memcpy(mac_out, digest, 8);
}

static inline int frame_mac_equal(const uint8_t a[8], const uint8_t b[8]) {
  uint8_t diff = 0;
  for (int i = 0; i < 8; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

#ifdef __cplusplus
}
#endif

/* Layout guards (compiled in every TU that includes this header). */
AIO_STATIC_ASSERT(sizeof(hello_payload_t) == 4, "hello payload drift");
AIO_STATIC_ASSERT(sizeof(hello_ack_payload_t) == 3, "hello_ack payload drift");
AIO_STATIC_ASSERT(sizeof(time_sync_payload_t) == 5, "time_sync payload drift");
AIO_STATIC_ASSERT(sizeof(heartbeat_payload_t) == 19, "heartbeat payload drift");
AIO_STATIC_ASSERT(sizeof(feed_result_payload_t) == 14, "feed_result payload drift");
AIO_STATIC_ASSERT(sizeof(abort_payload_t) == 4, "abort payload drift");
AIO_STATIC_ASSERT(sizeof(ack_status_payload_t) == 3, "ack_status payload drift");
AIO_STATIC_ASSERT(sizeof(sched_entry_t) == 5, "sched_entry drift");
AIO_STATIC_ASSERT(sizeof(schedule_hdr_t) == 11, "schedule_hdr drift");
AIO_STATIC_ASSERT(SCHEDULE_MAX_ENTRIES >= 32, "schedule capacity too small");
AIO_STATIC_ASSERT(sizeof(pair_req_payload_t) == 11, "pair_req payload drift");
AIO_STATIC_ASSERT(sizeof(pair_accept_payload_t) == 46, "pair_accept payload drift");
AIO_STATIC_ASSERT(sizeof(sched_entry_t) * SCHEDULE_MAX_ENTRIES + sizeof(schedule_hdr_t) <=
                      FRAME_MAX_PAYLOAD,
                  "schedule does not fit a frame");

#endif /* AIO_MESSAGES_H */
