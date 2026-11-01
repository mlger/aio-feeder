/*
 * AIO Feeder — SHA-256 + HMAC-SHA256 (FIPS 180-4 / RFC 2104, RFC 6234 vectors).
 * Plain C, no MCU headers: the identical code runs on the host tests and on
 * both firmware targets, so frame MACs and pairing derivations are
 * bit-identical everywhere. Used for:
 *   - the truncated 8-byte frame HMAC (PLAN.md §6.4/§6.5, D13)
 *   - the pairing confirmation code and key wrap (§6.3, D14)
 * Not constant-time and not a general-purpose crypto library — it only
 * authenticates our own frames against passive forgery.
 */
#ifndef AIO_SHA256_H
#define AIO_SHA256_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_BLOCK 64
#define SHA256_DIGEST 32

typedef struct {
  uint32_t h[8];
  uint64_t total;
  uint8_t buf[SHA256_BLOCK];
  size_t buf_len;
} sha256_ctx;

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static inline uint32_t sha256_ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static inline void sha256_compress(sha256_ctx *c, const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
           ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = sha256_ror(w[i - 15], 7) ^ sha256_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = sha256_ror(w[i - 2], 17) ^ sha256_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
  uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = sha256_ror(e, 6) ^ sha256_ror(e, 11) ^ sha256_ror(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
    uint32_t S0 = sha256_ror(a, 2) ^ sha256_ror(a, 13) ^ sha256_ror(a, 22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = cc; cc = b; b = a; a = t1 + t2;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
  c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static inline void sha256_init(sha256_ctx *c) {
  static const uint32_t iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  memcpy(c->h, iv, sizeof(iv));
  c->total = 0;
  c->buf_len = 0;
}

static inline void sha256_update(sha256_ctx *c, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  c->total += len;
  if (c->buf_len) {
    size_t take = SHA256_BLOCK - c->buf_len;
    if (take > len) take = len;
    memcpy(c->buf + c->buf_len, p, take);
    c->buf_len += take;
    p += take;
    len -= take;
    if (c->buf_len == SHA256_BLOCK) {
      sha256_compress(c, c->buf);
      c->buf_len = 0;
    }
  }
  while (len >= SHA256_BLOCK) {
    sha256_compress(c, p);
    p += SHA256_BLOCK;
    len -= SHA256_BLOCK;
  }
  if (len) {
    memcpy(c->buf, p, len);
    c->buf_len = len;
  }
}

static inline void sha256_final(sha256_ctx *c, uint8_t out[SHA256_DIGEST]) {
  uint64_t bits = c->total * 8;
  uint8_t pad = 0x80;
  sha256_update(c, &pad, 1);
  uint8_t zero = 0;
  while (c->buf_len != 56) sha256_update(c, &zero, 1);
  uint8_t len_be[8];
  for (int i = 0; i < 8; i++) len_be[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha256_update(c, len_be, 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)(c->h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
    out[4 * i + 3] = (uint8_t)(c->h[i]);
  }
}

static inline void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST]) {
  sha256_ctx c;
  sha256_init(&c);
  sha256_update(&c, data, len);
  sha256_final(&c, out);
}

/* HMAC-SHA256 (RFC 2104). key_len may be 0..64+ (hashed down if > block). */
static inline void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data,
                               size_t data_len, uint8_t out[SHA256_DIGEST]) {
  uint8_t k[SHA256_BLOCK], ipad[SHA256_BLOCK], opad[SHA256_BLOCK];
  memset(k, 0, sizeof(k));
  if (key_len > SHA256_BLOCK) {
    sha256(key, key_len, k); /* long keys are hashed to 32 bytes */
  } else if (key_len) {
    memcpy(k, key, key_len);
  }
  for (int i = 0; i < SHA256_BLOCK; i++) {
    ipad[i] = k[i] ^ 0x36;
    opad[i] = k[i] ^ 0x5c;
  }
  uint8_t inner[SHA256_DIGEST];
  sha256_ctx c;
  sha256_init(&c);
  sha256_update(&c, ipad, SHA256_BLOCK);
  sha256_update(&c, data, data_len);
  sha256_final(&c, inner);
  sha256_init(&c);
  sha256_update(&c, opad, SHA256_BLOCK);
  sha256_update(&c, inner, SHA256_DIGEST);
  sha256_final(&c, out);
}

#ifdef __cplusplus
}
#endif

#endif /* AIO_SHA256_H */
