/*
 * AIO Feeder — hub PCF85063 RTC driver (PLAN.md §7.4, §4.1).
 *
 * The Waveshare ESP32-S3-Touch-LCD-4.3B carries a PCF85063 (I²C 0x51, CR927
 * backup). The hub uses it when Wi-Fi/NTP is unavailable. UTC is stored;
 * local time is derived via hub_time.h rules (same split as the node's
 * DS3231). The oscillator-stop flag (seconds bit 7) is surfaced so the boot
 * path can range-check and distrust a stale clock.
 *
 * Device-only (Arduino/Wire); not host-tested — bench item in
 * docs/bench-m3.md. DS3231-style BCD registers, 24 h mode.
 */
#ifndef AIO_PCF85063_H
#define AIO_PCF85063_H

#include <stdint.h>

#include <Wire.h>

namespace aio {

static inline uint8_t pcf_bcd_dec(uint8_t v) { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }
static inline uint8_t pcf_bcd_enc(uint8_t v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

struct PcfTime {
  uint8_t sec, min, hour, wday, mday, mon; /* mon 1-12; wday 0-6, PCF: 0=Sun */
  uint16_t year;                           /* full year, 2000-2099 */
};

class Pcf85063 {
 public:
  bool begin(TwoWire &w = Wire) {
    wire_ = &w;
    w.beginTransmission(ADDR);
    if (w.endTransmission() != 0) return false;
    /* ensure 24 h mode + normal running: clear STOP in control_1 (0x00) */
    uint8_t c1 = read_reg(0x00);
    if (write_reg(0x00, (uint8_t)(c1 & ~0x20)) != 0) return false;
    return true;
  }

  /* os_flag = oscillator stopped at some point → distrust value (§7.4). */
  bool read(PcfTime &t, bool &os_flag) {
    uint8_t buf[7];
    if (read_regs(0x03, buf, 7) != 0) return false;
    os_flag = (buf[0] & 0x80) != 0;
    t.sec = pcf_bcd_dec((uint8_t)(buf[0] & 0x7F));
    t.min = pcf_bcd_dec((uint8_t)(buf[1] & 0x7F));
    t.hour = pcf_bcd_dec((uint8_t)(buf[2] & 0x3F));
    t.wday = (uint8_t)(buf[3] & 0x07);
    t.mday = pcf_bcd_dec((uint8_t)(buf[4] & 0x3F));
    t.mon = pcf_bcd_dec((uint8_t)(buf[5] & 0x1F));
    t.year = (uint16_t)(2000 + pcf_bcd_dec(buf[6]));
    if (t.mon < 1 || t.mon > 12 || t.mday < 1 || t.mday > 31 || t.hour > 23 ||
        t.min > 59 || t.sec > 59)
      return false;
    return true;
  }

  bool write(const PcfTime &t) {
    /* STOP while updating, then release (datasheet order) */
    uint8_t c1 = read_reg(0x00);
    write_reg(0x00, (uint8_t)(c1 | 0x20));
    uint8_t buf[7] = {
        pcf_bcd_enc(t.sec),          pcf_bcd_enc(t.min),
        pcf_bcd_enc(t.hour),         (uint8_t)(t.wday & 0x07),
        pcf_bcd_enc(t.mday),         pcf_bcd_enc(t.mon),
        pcf_bcd_enc((uint8_t)(t.year % 100)),
    };
    uint8_t tx[8] = {0x03, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]};
    int rc = write_regs(tx, 8);
    write_reg(0x00, (uint8_t)(c1 & ~0x20));
    return rc == 0;
  }

 private:
  static constexpr uint8_t ADDR = 0x51;
  TwoWire *wire_ = nullptr;

  int read_regs(uint8_t reg, uint8_t *out, uint8_t n) {
    wire_->beginTransmission(ADDR);
    wire_->write(reg);
    if (wire_->endTransmission(false) != 0) return -1;
    if (wire_->requestFrom((int)ADDR, (int)n) != n) return -1;
    for (uint8_t i = 0; i < n; i++) out[i] = wire_->read();
    return 0;
  }
  int write_regs(const uint8_t *tx, uint8_t n) {
    wire_->beginTransmission(ADDR);
    wire_->write(tx, n);
    return wire_->endTransmission() == 0 ? 0 : -1;
  }
  uint8_t read_reg(uint8_t reg) {
    uint8_t v = 0;
    read_regs(reg, &v, 1);
    return v;
  }
  int write_reg(uint8_t reg, uint8_t val) {
    uint8_t tx[2] = {reg, val};
    return write_regs(tx, 2);
  }
};

}  // namespace aio

#endif /* AIO_PCF85063_H */
