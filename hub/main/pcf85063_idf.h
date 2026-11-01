/*
 * AIO Feeder — PCF85063 over ESP-IDF driver/i2c_master (Waveshare 4.3B:
 * onboard RTC at 0x51 on GPIO8/SDA, GPIO9/SCL, ~100 kHz shared bus).
 */
#ifndef AIO_PCF85063_IDF_H
#define AIO_PCF85063_IDF_H

#include "driver/i2c_master.h"

#include "../../hub/src/pcf85063.h"

namespace aio {

class PcfI2cIdf : public PcfI2cIf {
 public:
  bool begin(int sda_gpio, int scl_gpio) {
    i2c_master_bus_config_t cfg{};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = (gpio_num_t)sda_gpio;
    cfg.scl_io_num = (gpio_num_t)scl_gpio;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true; /* board has external pull-ups too */
    if (i2c_new_master_bus(&cfg, &bus_) != ESP_OK) return false;

    i2c_device_config_t dev{};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = Pcf85063Dev::ADDR;
    dev.scl_speed_hz = 100000;
    return i2c_master_bus_add_device(bus_, &dev, &dev_) == ESP_OK;
  }

  int read_regs(uint8_t reg, uint8_t *out, uint8_t n) override {
    return i2c_master_transmit_receive(dev_, &reg, 1, out, n, -1) == ESP_OK ? 0 : -1;
  }
  int write_regs(const uint8_t *tx, uint8_t n) override {
    return i2c_master_transmit(dev_, tx, n, -1) == ESP_OK ? 0 : -1;
  }
  int probe() override {
    return i2c_master_probe(bus_, Pcf85063Dev::ADDR, -1) == ESP_OK ? 0 : -1;
  }

 private:
  struct Pcf85063Dev { static constexpr uint8_t ADDR = 0x51; };
  i2c_master_bus_handle_t bus_ = nullptr;
  i2c_master_dev_handle_t dev_ = nullptr;
};

}  // namespace aio

#endif /* AIO_PCF85063_IDF_H */
