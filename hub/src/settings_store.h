/*
 * AIO Feeder — hub settings blob (PLAN.md §8.8 settings, §9.12, FR10, FR19;
 * M4b).
 *
 * Pure logic, host-tested. Everything the Settings screen edits that is NOT
 * node data lives here, persisted as its own CRC'd NVS blob ("set") beside
 * the registry ("reg"):
 *
 *   - display: brightness %, screen-off timeout (§8 "brightness/screen-off");
 *   - time: NTP on/off (the tz RULE itself stays in the registry — one
 *     source of truth the scheduler and TIME_SYNC already read);
 *   - network: Wi-Fi SSID + password (overrides the Kconfig bench values on
 *     hardware; the PASSWORD never leaves the device — see backup.h);
 *   - security: PIN lock — 4 digits, stored as a salted hash, scope
 *     "settings only" or "settings and manual feeds" (§8 UX rules);
 *   - storage: history retention 7/30/90 days (§7.5).
 *
 * The PIN is NOT recoverable and NOT included in backups (D18): only
 * {salt, hash} live here, the hash is a non-cryptographic 32-bit mix keyed
 * by the salt — the threat model is a curious toddler at the touchscreen,
 * not a determined attacker with the NVS image.
 */
#ifndef AIO_SETTINGS_STORE_H
#define AIO_SETTINGS_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hub_store.h"

namespace aio {

#define SET_VERSION 1
constexpr char SET_STORE_KEY[] = "set";

/* §8 UX: "Settings only" or "Settings and manual feeds". Schedules and
 * automatic feeds are NEVER blocked by the PIN (the scheduler never asks). */
constexpr uint8_t PIN_SCOPE_SETTINGS = 0;
constexpr uint8_t PIN_SCOPE_SETTINGS_FEEDS = 1;
constexpr uint8_t PIN_SCOPE_COUNT = 2;

constexpr uint8_t SET_BRIGHTNESS_MIN = 10;
constexpr uint8_t SET_BRIGHTNESS_MAX = 100;
constexpr uint8_t SET_BRIGHTNESS_DEF = 80;

/* screen-off presets (§8.8 Display pane cycles these; 0 = never dim) */
constexpr uint16_t SET_SCREEN_OFF_S[5] = {0, 30, 60, 120, 300};
constexpr uint8_t SET_SCREEN_OFF_COUNT = 5;
constexpr uint16_t SET_SCREEN_OFF_DEF = 60;

/* §7.5 history retention choices */
constexpr uint8_t SET_RETENTION_DAYS[3] = {7, 30, 90};
constexpr uint8_t SET_RETENTION_COUNT = 3;
constexpr uint8_t SET_RETENTION_DEF = 30;

constexpr uint32_t PIN_UNLOCK_MS = 60000; /* §8: correct PIN unlocks for 60 s */
constexpr uint8_t PIN_DIGITS = 4;

struct HubSettings {
  uint16_t version;      /* SET_VERSION */
  uint8_t brightness;    /* % backlight, 10..100 */
  uint16_t screen_off_s; /* 0 = never, else a SET_SCREEN_OFF_S value */
  uint8_t ntp_on;        /* SNTP enable (§9.13 wizard step 1) */
  uint8_t history_days;  /* 7 / 30 / 90 */
  uint8_t pin_enabled;
  uint8_t pin_scope;   /* PIN_SCOPE_* */
  uint8_t reserved0;
  uint32_t pin_salt;   /* random, regenerated on every pin set */
  uint32_t pin_hash;   /* pin_hash_compute(digits, salt) */
  char wifi_ssid[33];  /* 32 + NUL; "" = not set (Kconfig fallback) */
  char wifi_pass[65];  /* 64 + NUL; never leaves the device */
};

static inline HubSettings settings_defaults() {
  HubSettings s;
  memset(&s, 0, sizeof(s));
  s.version = SET_VERSION;
  s.brightness = SET_BRIGHTNESS_DEF;
  s.screen_off_s = SET_SCREEN_OFF_DEF;
  s.ntp_on = 1;
  s.history_days = SET_RETENTION_DEF;
  s.pin_enabled = 0;
  s.pin_scope = PIN_SCOPE_SETTINGS;
  s.pin_salt = 0;
  s.pin_hash = 0;
  return s;
}

static inline bool settings_load(IHubStore &st, HubSettings *out) {
  HubSettings s;
  if (store_get(st, SET_STORE_KEY, SET_VERSION, &s, sizeof(s)) != (int)sizeof(s))
    return false;
  if (s.brightness < SET_BRIGHTNESS_MIN || s.brightness > SET_BRIGHTNESS_MAX)
    return false;
  bool soff_ok = (s.screen_off_s == 0);
  for (int i = 0; i < SET_SCREEN_OFF_COUNT && !soff_ok; i++)
    soff_ok = (s.screen_off_s == SET_SCREEN_OFF_S[i]);
  if (!soff_ok) return false;
  bool ret_ok = false;
  for (int i = 0; i < SET_RETENTION_COUNT && !ret_ok; i++)
    ret_ok = (s.history_days == SET_RETENTION_DAYS[i]);
  if (!ret_ok) return false;
  if (s.pin_scope >= PIN_SCOPE_COUNT) return false;
  *out = s;
  return true;
}

static inline bool settings_save(IHubStore &st, const HubSettings &s) {
  return store_put(st, SET_STORE_KEY, SET_VERSION, &s, sizeof(s));
}

/* --- PIN ---------------------------------------------------------------------- */

/* Exactly 4 ASCII digits, NUL-terminated. */
static inline bool pin_valid_digits(const char *s) {
  if (!s || strlen(s) != PIN_DIGITS) return false;
  for (int i = 0; i < PIN_DIGITS; i++)
    if (s[i] < '0' || s[i] > '9') return false;
  return true;
}

/* 32-bit FNV-1a over "aio-pin:<digits>:<salt>" — a mix, not crypto: the
 * backup excludes the PIN entirely (D18) and the NVS image is out of
 * threat model (§8: touchscreen lock, §3.3 caps are the real fence). */
static inline uint32_t pin_hash_compute(const char *digits4, uint32_t salt) {
  uint32_t h = 2166136261u;
  const char *p = "aio-pin:";
  while (*p) {
    h ^= (uint8_t)*p++;
    h *= 16777619u;
  }
  for (int i = 0; i < PIN_DIGITS; i++) {
    h ^= (uint8_t)digits4[i];
    h *= 16777619u;
  }
  for (int i = 0; i < 4; i++) {
    h ^= (uint8_t)((salt >> (8 * i)) & 0xFF);
    h *= 16777619u;
  }
  return h;
}

/* Set/change the PIN: new random salt each time so equal codes hash
 * differently across hubs. rng fills 4 bytes of salt. */
static inline void pin_set(HubSettings *s, const char *digits4,
                           void (*rng)(uint8_t *, size_t)) {
  uint8_t saltb[4] = {0};
  if (rng) rng(saltb, sizeof(saltb));
  s->pin_salt = (uint32_t)saltb[0] | ((uint32_t)saltb[1] << 8) |
                ((uint32_t)saltb[2] << 16) | ((uint32_t)saltb[3] << 24);
  s->pin_hash = pin_hash_compute(digits4, s->pin_salt);
  s->pin_enabled = 1;
}

static inline bool pin_verify(const HubSettings &s, const char *digits4) {
  if (!s.pin_enabled || !pin_valid_digits(digits4)) return false;
  return pin_hash_compute(digits4, s.pin_salt) == s.pin_hash;
}

/* --- value validators (Settings screen + CLI share these) ---------------------- */

static inline uint8_t set_clamp_brightness(int v) {
  if (v < SET_BRIGHTNESS_MIN) return SET_BRIGHTNESS_MIN;
  if (v > SET_BRIGHTNESS_MAX) return SET_BRIGHTNESS_MAX;
  return (uint8_t)v;
}

static inline bool set_screen_off_valid(uint16_t s) {
  if (s == 0) return true;
  for (int i = 0; i < SET_SCREEN_OFF_COUNT; i++)
    if (s == SET_SCREEN_OFF_S[i]) return true;
  return false;
}

static inline bool set_retention_valid(uint8_t d) {
  for (int i = 0; i < SET_RETENTION_COUNT; i++)
    if (d == SET_RETENTION_DAYS[i]) return true;
  return false;
}

/* Next screen-off preset after `cur` (Display pane tap-cycles). */
static inline uint16_t set_screen_off_next(uint16_t cur) {
  for (int i = 0; i < SET_SCREEN_OFF_COUNT; i++)
    if (SET_SCREEN_OFF_S[i] == cur) return SET_SCREEN_OFF_S[(i + 1) % SET_SCREEN_OFF_COUNT];
  return SET_SCREEN_OFF_DEF;
}

}  // namespace aio

#endif /* AIO_SETTINGS_STORE_H */
