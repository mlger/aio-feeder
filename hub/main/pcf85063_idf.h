/*
 * AIO Feeder — PCF85063 over ESP-IDF driver/i2c_master (Waveshare 4.3B:
 * onboard RTC at 0x51 on GPIO8/SDA, GPIO9/SCL — the shared bus also carries
 * the CH422G expander and the GT911 touch; i2c_bus.h owns the bus handle).
 */
#ifndef AIO_PCF85063_IDF_H
#define AIO_PCF85063_IDF_H

#include "driver/i2c_master.h"

#include "i2c_bus.h"

#include "../../hub/src/pcf85063.h"

namespace aio {

class PcfI2cIdf : public PcfI2cIf {
 public:
  bool begin(int sda_gpio, int scl_gpio) {
    i2c_master_bus_handle_t bus = i2c::ensure_bus(sda_gpio, scl_gpio);
    if (!bus) return false;

    i2c_device_config_t dev{};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = Pcf85063Dev::ADDR;
    dev.scl_speed_hz = 100000;
    return i2c_master_bus_add_device(bus, &dev, &dev_) == ESP_OK;
  }

  /* Bounded timeouts: the shared bus is also carried by the display/touch
   * path; a wedged RTC must fail the read (caller distrusts and re-syncs),
   * never hang boot or the UI task forever (§3.3, §7.4). */
  static constexpr int I2C_TIMEOUT_MS = 100;
  int read_regs(uint8_t reg, uint8_t *out, uint8_t n) override {
    return i2c_master_transmit_receive(dev_, &reg, 1, out, n, I2C_TIMEOUT_MS) == ESP_OK ? 0 : -1;
  }
  int write_regs(const uint8_t *tx, uint8_t n) override {
    return i2c_master_transmit(dev_, tx, n, I2C_TIMEOUT_MS) == ESP_OK ? 0 : -1;
  }
  int probe() override {
    return i2c_master_probe(i2c::bus(), Pcf85063Dev::ADDR, I2C_TIMEOUT_MS) == ESP_OK ? 0 : -1;
  }

 private:
  struct Pcf85063Dev { static constexpr uint8_t ADDR = 0x51; };
  i2c_master_dev_handle_t dev_ = nullptr;
};

}  // namespace aio

#endif /* AIO_PCF85063_IDF_H */
