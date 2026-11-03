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

/* UNPAIR (hub → node, M7 FMEA #15): keyed with the hub's per-node key — the
 * node verifies it exactly like any other frame, clears its pairing and
 * re-beacons. Sent on remove, retried from the hub's pending-unpair queue
 * until the node announces itself unpaired again (HELLO/PAIR_REQ). */
typedef struct __attribute__((packed)) {
  uint8_t reason;   /* 0 = removed by operator */
} unpair_payload_t;

/* ENTER_OTA (hub → node, M8b FMEA #11, PLAN §7.6). Fixed-size payload: the
 * ssid/pass/url blob is XOR-wrapped with the per-node key keystream (D39) so
 * Wi-Fi credentials never ride in the clear — the frame MAC only proves who
 * sent it, not who else can read it. ks_seed is public wrap salt. */
#define OTA_FLAG_SHA 0x01     /* sha256[] is a pin, verify before reboot */
#define OTA_FLAG_WRAPPED 0x02 /* blob is keystream-wrapped (always set by v0.12+) */
#define OTA_SSID_MAX 32
#define OTA_PASS_MAX 64
#define OTA_URL_MAX 98        /* hub-served URLs are short by construction */
typedef struct __attribute__((packed)) {
  uint8_t ssid_len;
  uint8_t pass_len;
  uint8_t url_len;
  uint8_t flags;          /* OTA_FLAG_* */
  uint8_t ks_seed[4];     /* wrap keystream salt */
  uint8_t sha256[32];     /* image digest pin (OTA_FLAG_SHA) */
  uint8_t blob[OTA_SSID_MAX + OTA_PASS_MAX + OTA_URL_MAX];
} enter_ota_payload_t;

/* ENTER_OTA_ACK (node → hub): accepted + why not (ota_ack_reason_t). */
typedef enum {
  OTA_ACK_OK = 0,
  OTA_ACK_REJ_BUSY,      /* actuator mid-feed or fault — try again later */
  OTA_ACK_REJ_BENCH,     /* bench mode, relay safety core under test */
  OTA_ACK_REJ_BAD,       /* payload would not unwrap / lengths insane */
} ota_ack_reason_t;
typedef struct __attribute__((packed)) {
  uint8_t ok;
  uint8_t reason;        /* ota_ack_reason_t */
} ota_ack_payload_t;

/* Identity extension (M8b): appended (length-tolerant tail) to HEARTBEAT and
 * HELLO so the hub learns the node's release version + MCU. A peer that
 * parses only the base payload still works (§6.5 payloads may grow at the
 * tail); paired nodes carry it on every heartbeat (D40). */
#define MCU_NONE 0
#define MCU_ESP8266 1
#define MCU_ESP32C3 2
typedef struct __attribute__((packed)) {
  uint32_t rel_ver;      /* AIO_NODE_FW_VER build constant, 0 = unknown */
  uint8_t mcu;           /* MCU_* */
} fw_id_ext_t;

/* D39 wrap: keystream block i = HMAC(key, "AIO-OTA-WRAP-v1" ++ seed ++ i32(i)). */
static const uint8_t AIO_OTA_WRAP_DOMAIN[16] = "AIO-OTA-WRAP-v1";
static inline void ota_keystream(const uint8_t key[32], const uint8_t seed[4],
                                 uint32_t block, uint8_t out[32]) {
  uint8_t data[sizeof(AIO_OTA_WRAP_DOMAIN) + 4 + 4];
  size_t n = 0;
  memcpy(data + n, AIO_OTA_WRAP_DOMAIN, sizeof(AIO_OTA_WRAP_DOMAIN));
  n += sizeof(AIO_OTA_WRAP_DOMAIN);
  memcpy(data + n, seed, 4); n += 4;
  data[n++] = (uint8_t)(block >> 24);
  data[n++] = (uint8_t)(block >> 16);
  data[n++] = (uint8_t)(block >> 8);
  data[n++] = (uint8_t)block;
  hmac_sha256(key, 32, data, n, out);
}

/* buf ^= keystream(key, seed) — symmetric; wrap == unwrap. */
static inline void ota_wrap(const uint8_t key[32], const uint8_t seed[4], uint8_t *buf,
                            size_t len) {
  for (size_t off = 0; off < len; off += SHA256_DIGEST) {
    uint8_t ks[SHA256_DIGEST];
    ota_keystream(key, seed, (uint32_t)(off / SHA256_DIGEST), ks);
    size_t n = len - off > SHA256_DIGEST ? SHA256_DIGEST : len - off;
    for (size_t i = 0; i < n; i++) buf[off + i] = (uint8_t)(buf[off + i] ^ ks[i]);
  }
}

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
AIO_STATIC_ASSERT(sizeof(unpair_payload_t) == 1, "unpair payload drift");
AIO_STATIC_ASSERT(sizeof(ack_status_payload_t) == 3, "ack_status payload drift");
AIO_STATIC_ASSERT(sizeof(sched_entry_t) == 5, "sched_entry drift");
AIO_STATIC_ASSERT(sizeof(schedule_hdr_t) == 11, "schedule_hdr drift");
AIO_STATIC_ASSERT(SCHEDULE_MAX_ENTRIES >= 32, "schedule capacity too small");
AIO_STATIC_ASSERT(sizeof(pair_req_payload_t) == 11, "pair_req payload drift");
AIO_STATIC_ASSERT(sizeof(pair_accept_payload_t) == 46, "pair_accept payload drift");
AIO_STATIC_ASSERT(
    sizeof(enter_ota_payload_t) ==
        4 + 4 + 32 + OTA_SSID_MAX + OTA_PASS_MAX + OTA_URL_MAX,
    "enter_ota payload drift");
AIO_STATIC_ASSERT(sizeof(enter_ota_payload_t) <= FRAME_MAX_PAYLOAD,
                  "enter_ota does not fit a frame");
AIO_STATIC_ASSERT(sizeof(ota_ack_payload_t) == 2, "ota_ack payload drift");
AIO_STATIC_ASSERT(sizeof(fw_id_ext_t) == 5, "fw_id_ext payload drift");
AIO_STATIC_ASSERT(sizeof(sched_entry_t) * SCHEDULE_MAX_ENTRIES + sizeof(schedule_hdr_t) <=
                      FRAME_MAX_PAYLOAD,
                  "schedule does not fit a frame");

#endif /* AIO_MESSAGES_H */
