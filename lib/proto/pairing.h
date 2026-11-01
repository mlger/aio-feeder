/*
 * AIO Feeder — pairing confirmation code + key wrap (PLAN.md §6.3, D14).
 *
 * Flow (hub side M3, node side ready here):
 *   node → PAIR_REQ { node_nonce }
 *   hub  → PAIR_ACCEPT { hub_nonce, code_digits, wrapped_key }
 *   code = 4 digits derived from (node_mac, node_nonce, hub_nonce) — both
 *   sides derive the same value; the user confirms they match (node shows it
 *   on serial/LED, hub on screen).
 *   The per-node key is XOR-masked with an HMAC keystream keyed by the code,
 *   so the key is never on the air in the clear.
 *
 * Residual risk, documented per §6.3.5: the wrap entropy is the 4-digit code
 * (10^4), chosen deliberately — it defeats passive listeners, not offline
 * brute force. Safety caps (SR2–SR4) remain the real protection.
 */
#ifndef AIO_PAIRING_H
#define AIO_PAIRING_H

#include <stdint.h>
#include <string.h>
#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAIRING_CODE_DOMAIN "AIO-PAIR-CODE-v1"
#define PAIRING_WRAP_DOMAIN "AIO-PAIR-WRAP-v1"

/* code = HMAC(domain, node_mac ++ node_nonce ++ hub_nonce) mod 10000,
 * returned as ASCII digits in out[4]. */
static inline void pairing_code(const uint8_t node_mac[6], const uint8_t node_nonce[8],
                                const uint8_t hub_nonce[8], uint8_t out_digits[4]) {
  uint8_t data[6 + 8 + 8 + sizeof(PAIRING_CODE_DOMAIN)];
  size_t n = 0;
  memcpy(data + n, PAIRING_CODE_DOMAIN, sizeof(PAIRING_CODE_DOMAIN));
  n += sizeof(PAIRING_CODE_DOMAIN); /* includes NUL: domain separation */
  memcpy(data + n, node_mac, 6); n += 6;
  memcpy(data + n, node_nonce, 8); n += 8;
  memcpy(data + n, hub_nonce, 8); n += 8;
  uint8_t d[32];
  sha256(data, n, d);
  uint32_t v = ((uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]) % 10000u;
  for (int i = 3; i >= 0; i--) {
    out_digits[i] = (uint8_t)('0' + (v % 10));
    v /= 10;
  }
}

/* Keystream block i = HMAC(domain ++ code ++ nonce_pair ++ i32(i)). */
static inline void pairing_keystream(const uint8_t code_digits[4], const uint8_t node_nonce[8],
                                     const uint8_t hub_nonce[8], uint32_t block,
                                     uint8_t out[32]) {
  uint8_t data[4 + 8 + 8 + 4 + sizeof(PAIRING_WRAP_DOMAIN)];
  size_t n = 0;
  memcpy(data + n, PAIRING_WRAP_DOMAIN, sizeof(PAIRING_WRAP_DOMAIN));
  n += sizeof(PAIRING_WRAP_DOMAIN);
  memcpy(data + n, code_digits, 4); n += 4;
  memcpy(data + n, node_nonce, 8); n += 8;
  memcpy(data + n, hub_nonce, 8); n += 8;
  data[n++] = (uint8_t)(block >> 24);
  data[n++] = (uint8_t)(block >> 16);
  data[n++] = (uint8_t)(block >> 8);
  data[n++] = (uint8_t)block;
  hmac_sha256(code_digits, 4, data, n, out); /* key = the confirmed code */
}

/* key → wrapped (XOR keystream). out may alias key. */
static inline void pairing_wrap_key(const uint8_t key[32], const uint8_t code_digits[4],
                                    const uint8_t node_nonce[8], const uint8_t hub_nonce[8],
                                    uint8_t out[32]) {
  for (uint32_t b = 0; b < 32; b += 16) {
    uint8_t ks[32];
    pairing_keystream(code_digits, node_nonce, hub_nonce, b / 16, ks);
    for (int i = 0; i < 16; i++) out[b + i] = (uint8_t)(key[b + i] ^ ks[i]);
  }
}

/* wrapped → key. Returns 0 on success. */
static inline int pairing_unwrap_key(const uint8_t wrapped[32], const uint8_t code_digits[4],
                                     const uint8_t node_nonce[8], const uint8_t hub_nonce[8],
                                     uint8_t out_key[32]) {
  pairing_wrap_key(wrapped, code_digits, node_nonce, hub_nonce, out_key);
  return 0; /* correctness is enforced by the code check, not the unwrap */
}

#ifdef __cplusplus
}
#endif

#endif /* AIO_PAIRING_H */
