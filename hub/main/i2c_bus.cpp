/*
 * AIO Feeder — shared I2C master bus (see i2c_bus.h).
 */
#include "i2c_bus.h"

#include <esp_log.h>

namespace aio {
namespace i2c {

static const char *TAG = "aio-i2c";
static i2c_master_bus_handle_t s_bus = nullptr;

i2c_master_bus_handle_t ensure_bus(int sda_gpio, int scl_gpio) {
  if (s_bus) return s_bus;
  i2c_master_bus_config_t cfg{};
  cfg.i2c_port = I2C_NUM_0;
  cfg.sda_io_num = (gpio_num_t)sda_gpio;
  cfg.scl_io_num = (gpio_num_t)scl_gpio;
  cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  cfg.glitch_ignore_cnt = 7;
  cfg.flags.enable_internal_pullup = true; /* board has external pull-ups too */
  if (i2c_new_master_bus(&cfg, &s_bus) != ESP_OK) {
    ESP_LOGE(TAG, "i2c master bus init failed (sda=%d scl=%d)", sda_gpio, scl_gpio);
    s_bus = nullptr;
  }
  return s_bus;
}

i2c_master_bus_handle_t bus() { return s_bus; }

}  // namespace i2c
}  // namespace aio
