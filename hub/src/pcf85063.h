/*
 * AIO Feeder — PCF85063 RTC driver, bus-abstracted (PLAN.md §7.4, §4.1).
 *
 * The Waveshare ESP32-S3-Touch-LCD-4.3B carries a PCF85063 (I²C 0x51, CR927
 * backup) on GPIO8/9. The hub uses it when Wi-Fi/NTP is unavailable. UTC is
 * stored; local time is derived via hub_time.h rules. The oscillator-stop
 * flag (seconds bit 7) is surfaced so the boot path can range-check and
 * distrust a stale clock.
 *
 * Pure header, no Wire/IDF dependency: the runtime injects a PcfI2cIf
 * (hub/main/pcf85063_idf.h implements it with driver/i2c_master). Device-only
 * (not host-tested) — bench item in docs/bench-m3.md. 24 h mode, BCD regs.
 */
#ifndef AIO_PCF85063_H
#define AIO_PCF85063_H

#include <stdint.h>

#include "hub_time.h" /* days_in_month for the calendar-valid mday check */

namespace aio {

static inline uint8_t pcf_bcd_dec(uint8_t v) { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }
static inline uint8_t pcf_bcd_enc(uint8_t v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

struct PcfTime {
  uint8_t sec, min, hour, wday, mday, mon; /* mon 1-12; wday 0-6, PCF: 0=Sun */
  uint16_t year;                           /* full year, 2000-2099 */
};

/* Minimal register-level I2C bus (implemented per-platform). */
struct PcfI2cIf {
  /* write `reg` then read `n` bytes (repeated start) */
  virtual int read_regs(uint8_t reg, uint8_t *out, uint8_t n) = 0;
  /* write `n` bytes starting with the register address */
  virtual int write_regs(const uint8_t *tx, uint8_t n) = 0;
  /* probe: 0 = ACK at the device address */
  virtual int probe() = 0;
  virtual ~PcfI2cIf() {}
};

class Pcf85063 {
 public:
  bool begin(PcfI2cIf &bus) {
    bus_ = &bus;
    if (bus_->probe() != 0) return false;
    /* ensure 24 h mode + normal running: clear STOP in control_1 (0x00) */
    const uint8_t c1 = read_reg(0x00);
    if (write_reg(0x00, (uint8_t)(c1 & ~0x20)) != 0) return false;
    return true;
  }

  /* os_flag = oscillator stopped at some point → distrust value (§7.4). */
  bool read(PcfTime &t, bool &os_flag) {
    uint8_t buf[7];
    if (!bus_ || read_regs(0x03, buf, 7) != 0) return false;
    os_flag = (buf[0] & 0x80) != 0;
    t.sec = pcf_bcd_dec((uint8_t)(buf[0] & 0x7F));
    t.min = pcf_bcd_dec((uint8_t)(buf[1] & 0x7F));
    t.hour = pcf_bcd_dec((uint8_t)(buf[2] & 0x3F));
    t.wday = (uint8_t)(buf[3] & 0x07);
    t.mday = pcf_bcd_dec((uint8_t)(buf[4] & 0x3F));
    t.mon = pcf_bcd_dec((uint8_t)(buf[5] & 0x1F));
    t.year = (uint16_t)(2000 + pcf_bcd_dec(buf[6]));
    if (t.mon < 1 || t.mon > 12 || t.mday < 1 || t.hour > 23 ||
        t.min > 59 || t.sec > 59)
      return false;
    /* month length incl. leap years — "Feb 31" from a corrupt read used to
     * pass the 1..31 check and shift days_from_civil into March (§7.4). */
    if (t.mday > aio::days_in_month((int64_t)t.year, (unsigned)t.mon))
      return false;
    return true;
  }

  bool write(const PcfTime &t) {
    if (!bus_) return false;
    /* STOP while updating, then release (datasheet order) */
    const uint8_t c1 = read_reg(0x00);
    write_reg(0x00, (uint8_t)(c1 | 0x20));
    const uint8_t buf[7] = {
        pcf_bcd_enc(t.sec),  pcf_bcd_enc(t.min),
        pcf_bcd_enc(t.hour), (uint8_t)(t.wday & 0x07),
        pcf_bcd_enc(t.mday), pcf_bcd_enc(t.mon),
        pcf_bcd_enc((uint8_t)(t.year % 100)),
    };
    const uint8_t tx[8] = {0x03, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]};
    const int rc = write_regs(tx, 8);
    write_reg(0x00, (uint8_t)(c1 & ~0x20));
    return rc == 0;
  }

 private:
  static constexpr uint8_t ADDR = 0x51;
  PcfI2cIf *bus_ = nullptr;

  int read_regs(uint8_t reg, uint8_t *out, uint8_t n) { return bus_->read_regs(reg, out, n); }
  int write_regs(const uint8_t *tx, uint8_t n) { return bus_->write_regs(tx, n); }
  uint8_t read_reg(uint8_t reg) {
    uint8_t v = 0;
    read_regs(reg, &v, 1);
    return v;
  }
  int write_reg(uint8_t reg, uint8_t val) {
    const uint8_t tx[2] = {reg, val};
    return write_regs(tx, 2);
  }
};

}  // namespace aio

#endif /* AIO_PCF85063_H */
