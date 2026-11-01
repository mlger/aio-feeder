/*
 * AIO Feeder — DS3231 driver over Wire (both node MCUs share this file).
 * Stores UTC (PLAN.md §7.4). Range-check + oscillator-stop flag decide
 * "healthy" (SR7): a node must never accept the RTC after backup-cell loss.
 */
#ifndef AIO_DS3231_H
#define AIO_DS3231_H

#include <stdint.h>
#include <Wire.h>

#define DS3231_ADDR 0x68
#define DS3231_REG_STATUS 0x0F
#define DS3231_OSF 0x80 /* oscillator stopped — backup cell was lost */

struct RtcTime {
  uint8_t sec, min, hour, wday, mday, mon;
  uint16_t year; /* full year, e.g. 2026 */
};

struct RtcReading {
  RtcTime t;
  bool osf;          /* oscillator-stop flag set */
  bool range_ok;     /* all fields in plausible ranges */
  bool present;      /* device acknowledged */
};

/* unix ↔ civil datetime (Howard Hinnant's algorithms, public domain). */
static inline int64_t rtc_days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static inline void rtc_civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp + (mp < 10 ? 3 : -9);
  *y = (int)(yy + (*m <= 2));
}

static inline int64_t rtc_make_unix(const RtcTime &t) {
  return rtc_days_from_civil(t.year, t.mon, t.mday) * 86400 + t.hour * 3600 +
         t.min * 60 + t.sec;
}

static inline RtcTime rtc_from_unix(int64_t unix) {
  int64_t days = unix >= 0 ? unix / 86400 : (unix - 86399) / 86400;
  int64_t secs = unix - days * 86400;
  RtcTime t{};
  int y;
  unsigned m, d;
  rtc_civil_from_days(days, &y, &m, &d);
  t.year = (uint16_t)y;
  t.mon = (uint8_t)m;
  t.mday = (uint8_t)d;
  t.sec = (uint8_t)(secs % 60);
  t.min = (uint8_t)((secs / 60) % 60);
  t.hour = (uint8_t)(secs / 3600);
  t.wday = (uint8_t)((days + 4) % 7 + 1); /* 1970-01-01 = Thursday */
  return t;
}

static inline uint8_t rtc_bcd_dec(uint8_t v) { return (v & 0x0F) + (v >> 4) * 10; }
static inline uint8_t rtc_bcd_enc(uint8_t v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

static inline bool rtc_wire_write(const uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(buf, len);
  return Wire.endTransmission() == 0;
}

static inline uint8_t rtc_read_status() {
  const uint8_t reg = DS3231_REG_STATUS;
  if (!rtc_wire_write(&reg, 1)) return 0xFF;
  if (Wire.requestFrom((int)DS3231_ADDR, 1) != 1) return 0xFF;
  return Wire.read();
}

static inline RtcReading rtc_read() {
  RtcReading r{};
  const uint8_t start = 0x00;
  r.present = rtc_wire_write(&start, 1);
  if (!r.present) return r;
  if (Wire.requestFrom((int)DS3231_ADDR, 7) != 7) {
    r.present = false;
    return r;
  }
  r.t.sec = rtc_bcd_dec(Wire.read() & 0x7F);
  r.t.min = rtc_bcd_dec(Wire.read() & 0x7F);
  r.t.hour = rtc_bcd_dec(Wire.read() & 0x3F);
  r.t.wday = Wire.read() & 0x07;
  r.t.mday = rtc_bcd_dec(Wire.read() & 0x3F);
  r.t.mon = rtc_bcd_dec(Wire.read() & 0x1F);
  r.t.year = 2000 + rtc_bcd_dec(Wire.read());
  const uint8_t st = rtc_read_status();
  r.osf = (st & DS3231_OSF) != 0;
  r.range_ok = r.t.sec < 60 && r.t.min < 60 && r.t.hour < 24 && r.t.wday >= 1 &&
               r.t.wday <= 7 && r.t.mday >= 1 && r.t.mday <= 31 && r.t.mon >= 1 &&
               r.t.mon <= 12 && r.t.year >= 2000 && r.t.year <= 2099;
  return r;
}

static inline bool rtc_write_time(const RtcTime &t) {
  uint8_t buf[8] = {0x00, rtc_bcd_enc(t.sec), rtc_bcd_enc(t.min),
                    rtc_bcd_enc(t.hour), rtc_bcd_enc(t.wday ? t.wday : 1),
                    rtc_bcd_enc(t.mday), rtc_bcd_enc(t.mon),
                    rtc_bcd_enc((uint8_t)(t.year % 100))};
  if (!rtc_wire_write(buf, 8)) return false;
  /* clear OSF: the clock was just set, the oscillator flag no longer applies */
  const uint8_t wr[2] = {DS3231_REG_STATUS, (uint8_t)(rtc_read_status() & ~DS3231_OSF)};
  return rtc_wire_write(wr, 2);
}

#endif /* AIO_DS3231_H */
