/*
 * AIO Feeder — hub time: timezone rules + local-time conversion (PLAN.md §7.4).
 *
 * Pure logic, host-tested (§11.1). One source of truth for local time so the
 * scheduler and the node TIME_SYNC pushes agree: UTC unix in, local
 * {day, min_of_day, weekday} out.
 *
 * v1 rules (rule ids travel on the wire in TIME_SYNC.tz_rule):
 *   0 UTC, 1 Asia/Manila (+08:00, no DST), 2 Europe/Central (CET/CEST),
 *   3 US Eastern (EST/EDT). DST transitions are defined in UTC:
 *   EU: last Sunday March 01:00 UTC → last Sunday October 01:00 UTC.
 *   US: 2nd Sunday March 07:00 UTC → 1st Sunday November 06:00 UTC.
 *   Southern-hemisphere rules are out of scope in v1.
 *
 * Occurrence timestamps ("local seconds space"): local wall time expressed as
 * unix shifted by the current offset (day_start + min*60). Differences within
 * one offset region are exact; across a DST jump ±1 h skew is accepted —
 * occurrence keys and windows are day/minute based, so this is invisible to
 * the scheduler.
 */
#ifndef AIO_HUB_TIME_H
#define AIO_HUB_TIME_H

#include <stddef.h>
#include <stdint.h>

namespace aio {

constexpr uint8_t TZ_UTC = 0;
constexpr uint8_t TZ_MANILA = 1;
constexpr uint8_t TZ_EU_CENTRAL = 2;
constexpr uint8_t TZ_US_EASTERN = 3;
constexpr uint8_t TZ_RULE_COUNT = 4;

/* DST transition point: month (1-12), week 1..4 or 0 = last, weekday
 * (0 = Sunday, tm_wday order), hour of day in UTC. */
struct TzPoint {
  uint8_t month;
  int8_t week;
  uint8_t wday;
  uint8_t hour;
};

struct TzRule {
  int16_t std_min;
  int16_t dst_min;
  TzPoint dst_start;
  TzPoint dst_end;
};

static inline const TzRule *tz_rule(uint8_t id) {
  static const TzRule rules[TZ_RULE_COUNT] = {
      {0, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},                       /* UTC */
      {8 * 60, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},                  /* Manila */
      {60, 120, {3, 0, 0, 1}, {10, 0, 0, 1}},                   /* CET/CEST */
      {-300, -240, {3, 2, 0, 7}, {11, 1, 0, 6}},                /* EST/EDT */
  };
  return (id < TZ_RULE_COUNT) ? &rules[id] : &rules[TZ_UTC];
}

/* Days since 1970-01-01 (proleptic Gregorian). Howard Hinnant's algorithm. */
static inline int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static inline void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp + (mp < 10 ? 3 : -9);
  *y = yy + (*m <= 2);
}

/* Weekday from day number, 0 = Sunday. 1970-01-01 = Thursday(4). */
static inline uint8_t wday_from_days(int64_t z) {
  return (uint8_t)(((z % 7) + 4 + 7) % 7);
}

static inline unsigned days_in_month(int64_t y, unsigned m) {
  static const unsigned dim[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 30;
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
  return dim[m];
}

/* UTC unix of a TzPoint in year y. */
static inline int64_t tz_point_unix(const TzPoint &p, int64_t y) {
  const int64_t first = days_from_civil(y, p.month, 1);
  const uint8_t wd1 = wday_from_days(first);
  int64_t day;
  if (p.week <= 0) { /* last <wday> of month */
    const int64_t last = first + days_in_month(y, p.month) - 1;
    day = last - ((wday_from_days(last) - p.wday + 7) % 7);
  } else {
    day = first + ((p.wday - wd1 + 7) % 7) + 7 * (p.week - 1);
  }
  return day * 86400 + (int64_t)p.hour * 3600;
}

static inline bool tz_is_dst(const TzRule &r, int64_t unix_s) {
  if (r.dst_min == r.std_min) return false;
  const int64_t y = 1970 + (unix_s >= 0 ? unix_s : unix_s - 86399) / 31556952; /* ~year */
  const int64_t start = tz_point_unix(r.dst_start, y);
  const int64_t end = tz_point_unix(r.dst_end, y);
  return start <= unix_s && unix_s < end;
}

static inline int32_t tz_offset_min(uint8_t rule_id, int64_t unix_s) {
  const TzRule &r = *tz_rule(rule_id);
  return tz_is_dst(r, unix_s) ? r.dst_min : r.std_min;
}

/* Local breakdown in "local seconds space". day_start is local midnight in
 * that space (NOT raw unix when offset != 0). */
struct LocalTime {
  int64_t local_s;
  int64_t day_start;     /* local-space midnight */
  uint16_t min_of_day;
  uint8_t wday;          /* 0 = Sunday */
};

static inline LocalTime local_from_unix(int64_t unix_s, uint8_t rule_id) {
  const int64_t local = unix_s + (int64_t)tz_offset_min(rule_id, unix_s) * 60;
  const int64_t day = local >= 0 ? local / 86400 : (local - 86399) / 86400;
  LocalTime t;
  t.local_s = local;
  t.day_start = day * 86400;
  t.min_of_day = (uint16_t)((local - t.day_start) / 60);
  t.wday = wday_from_days(day);
  return t;
}

/* Occurrence identity: which local day + which minute. Packable to u64 for
 * ring/journal keys. */
static inline uint64_t occ_pack(int64_t day_start, uint16_t min_of_day) {
  return ((uint64_t)day_start << 16) | min_of_day;
}
static inline int64_t occ_day(uint64_t k) { return (int64_t)(k >> 16); }
static inline uint16_t occ_min(uint64_t k) { return (uint16_t)(k & 0xFFFF); }

/* Estimated real unix of an occurrence (±1 h around DST jumps; only used for
 * freshness windows and gate checks, never for firing). */
static inline int64_t occ_unix_est(int64_t day_start, uint16_t min_of_day,
                                   uint8_t rule_id, int64_t now_unix) {
  return day_start + (int64_t)min_of_day * 60 -
         (int64_t)tz_offset_min(rule_id, now_unix) * 60;
}

}  // namespace aio

#endif /* AIO_HUB_TIME_H */
