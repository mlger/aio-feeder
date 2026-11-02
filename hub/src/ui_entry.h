/*
 * AIO Feeder — hub UI entry-pad model (PLAN.md §12 M5b: touchscreen
 * numeric/text entry).
 *
 * Pure logic, host-tested. One shared modal entry pad lives in the LVGL
 * view (built once, pool-cheap); this model owns its buffer and parsing so
 * the tap → value path is testable without LVGL. The view opens the pad
 * for a Target seeded with the current value, mutates it from the keypad,
 * and on OK hands the parsed value to the engine as one nav::Actions
 * commit (the engine maps targets to session calls — ui_nav.h).
 *
 *   K_TEXT  free text (letters/digits/punct, ≤ max_len)   → entry_text
 *   K_UINT  integer digits                                → entry_u32
 *   K_TIME  4-digit HHMM string editor                    → entry_u32 (min of day)
 *   K_X100  digits with ≤ 2 decimals (weighed grams)      → entry_u32 (value × 100)
 */
#ifndef AIO_UI_ENTRY_H
#define AIO_UI_ENTRY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace aio {
namespace entry {

enum Target : uint8_t {
  ENT_NONE = 0,
  /* pairing form (§8.5) */
  ENT_PAIR_NAME,
  ENT_PAIR_TOTAL,
  ENT_PAIR_NOW,
  /* setup wizard (§9.13) */
  ENT_WIZ_SSID,
  ENT_WIZ_TOTAL,
  ENT_WIZ_NOW,
  ENT_WIZ_TIME,
  ENT_WIZ_AMOUNT,
  /* calibration weigh step (§9.3.3) */
  ENT_CAL_WEIGH,
  /* schedule editor drafts (§8.3) */
  ENT_EDIT_TIME,
  ENT_EDIT_AMOUNT,
  ENT_COUNT
};

enum Kind : uint8_t { K_TEXT = 0, K_UINT, K_TIME, K_X100 };

static inline Kind kind_of(Target t) {
  switch (t) {
    case ENT_PAIR_NAME:
    case ENT_WIZ_SSID: return K_TEXT;
    case ENT_WIZ_TIME:
    case ENT_EDIT_TIME: return K_TIME;
    case ENT_CAL_WEIGH: return K_X100;
    default: return K_UINT;
  }
}

/* Hard ceilings per target (engine re-validates against session rules). */
static inline uint32_t max_of(Target t) {
  switch (t) {
    case ENT_PAIR_NAME: return 12;
    case ENT_WIZ_SSID: return 32;
    case ENT_WIZ_TIME:
    case ENT_EDIT_TIME: return 1439; /* min of day */
    case ENT_CAL_WEIGH: return 99999; /* ×100 → 999.99 */
    case ENT_EDIT_AMOUNT: return 999;
    default: return 100000; /* capacities (§9.7) */
  }
}

static inline uint8_t max_len_of(Target t) {
  return kind_of(t) == K_TEXT ? (uint8_t)max_of(t) : 10;
}

/* One shared pad state. The view renders preview() and feeds push_char /
 * backspace / clear_all from its keypad; commit hands out the parsed value. */
struct Pad {
  Target tgt = ENT_NONE;
  char text[33] = "";   /* K_TEXT buffer */
  char digits[11] = ""; /* K_UINT/K_TIME/K_X100 typed digits ('.' allowed on K_X100) */
  uint8_t shift = 0;    /* K_TEXT keyboard case (0 lower, 1 upper) */

  bool is_open() const { return tgt != ENT_NONE; }

  /* Opens for a target. seed_text seeds K_TEXT; seed_digits seeds the
   * numeric string (e.g. the current value, "0730" for time). */
  void open(Target t, const char *seed_text, const char *seed_digits) {
    tgt = t;
    shift = 0;
    text[0] = '\0';
    digits[0] = '\0';
    if (t == ENT_NONE) return;
    if (kind_of(t) == K_TEXT) {
      if (seed_text) {
        strncpy(text, seed_text, sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
        text[max_len_of(t)] = '\0'; /* enforce the target's own cap */
      }
    } else if (seed_digits) {
      strncpy(digits, seed_digits, sizeof(digits) - 1);
      digits[sizeof(digits) - 1] = '\0';
    }
  }

  void close() {
    tgt = ENT_NONE;
    text[0] = '\0';
    digits[0] = '\0';
    shift = 0;
  }

  /* One key. Numeric kinds accept 0-9 ('.' only on K_X100, one at most);
   * text accepts printable chars from the on-screen set (any non-control
   * byte the keypad can produce). */
  void push_char(char c) {
    if (tgt == ENT_NONE) return;
    if (kind_of(tgt) == K_TEXT) {
      if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) return;
      const size_t len = strlen(text);
      if (len >= max_len_of(tgt)) return;
      text[len] = c;
      text[len + 1] = '\0';
      return;
    }
    if (c >= '0' && c <= '9') {
      size_t len = strlen(digits);
      if (len >= sizeof(digits) - 1 || len >= max_len_of(tgt)) return;
      digits[len++] = c;
      digits[len] = '\0';
      return;
    }
    if (c == '.' && kind_of(tgt) == K_X100 && !strchr(digits, '.')) {
      size_t len = strlen(digits);
      if (len >= sizeof(digits) - 1 || len >= max_len_of(tgt)) return;
      digits[len++] = '.';
      digits[len] = '\0';
    }
  }

  void backspace() {
    if (tgt == ENT_NONE) return;
    size_t len = strlen(kind_of(tgt) == K_TEXT ? text : digits);
    if (kind_of(tgt) == K_TEXT)
      text[len ? len - 1 : 0] = '\0';
    else
      digits[len ? len - 1 : 0] = '\0';
  }

  void clear_all() {
    if (tgt == ENT_NONE) return;
    text[0] = '\0';
    digits[0] = '\0';
  }

  /* K_UINT / K_X100 parse (leading 0s fine; '.' handled for K_X100).
   * Returns the clamped value; K_X100 scales up ×100 (8.25 → 825). */
  uint32_t value() const {
    if (tgt == ENT_NONE) return 0;
    const Kind k = kind_of(tgt);
    if (k == K_TEXT) return 0;
    uint64_t whole = 0, frac = 0; /* frac in hundredths as typed */
    uint8_t frac_digits = 0;
    bool in_frac = false;
    for (const char *p = digits; *p; p++) {
      if (*p == '.') {
        in_frac = true;
        continue;
      }
      if (!in_frac) {
        whole = whole * 10 + (uint32_t)(*p - '0');
        if (whole > 4000000000ull) whole = 4000000000ull; /* keep u32 math safe */
      } else if (frac_digits < 2) {
        frac = frac * 10 + (uint32_t)(*p - '0');
        frac_digits++;
      }
    }
    while (frac_digits++ < 2) frac *= 10;
    uint64_t v = k == K_X100 ? whole * 100 + frac : whole;
    const uint32_t cap = max_of(tgt);
    if (v > cap) v = cap;
    return (uint32_t)v;
  }

  /* K_TIME: the digit string is read as a HHMM number ("730" = 07:30,
   * "7" = 00:07 — the numeric value carries the leading zeros), clamped to
   * 00:00..23:59. Returns minutes of day. */
  uint16_t time_value() const {
    if (tgt == ENT_NONE || kind_of(tgt) != K_TIME) return 0;
    size_t len = strlen(digits);
    if (!len) return 0;
    if (len > 4) len = 4;
    uint32_t n = 0;
    for (size_t i = 0; i < len; i++) n = n * 10 + (uint32_t)(digits[i] - '0');
    uint32_t hh = n / 100, mm = n % 100;
    if (hh > 23) hh = 23;
    if (mm > 59) mm = 59;
    return (uint16_t)(hh * 60 + mm);
  }

  /* What the view shows while typing: time renders "HH:MM" (with '_' for
   * untyped digits), x100 renders "d.dd", others the raw buffer. */
  const char *preview(char *buf, size_t n) const {
    if (!buf || !n) return "";
    if (tgt == ENT_NONE) {
      buf[0] = '\0';
      return buf;
    }
    switch (kind_of(tgt)) {
      case K_TIME: {
        char d[5] = "____";
        size_t len = strlen(digits);
        if (len > 4) len = 4;
        memcpy(d, digits, len);
        snprintf(buf, n, "%c%c:%c%c", d[0], d[1], d[2], d[3]);
        return buf;
      }
      case K_X100: {
        const char *dot = strchr(digits, '.');
        if (dot) {
          snprintf(buf, n, "%s", digits);
        } else {
          snprintf(buf, n, "%s.00", digits);
        }
        return buf;
      }
      case K_TEXT:
        snprintf(buf, n, "%s", text);
        return buf;
      default:
        snprintf(buf, n, "%s", digits);
        return buf;
    }
  }
};

}  // namespace entry
}  // namespace aio

#endif /* AIO_UI_ENTRY_H */
