/*
 * AIO Feeder — shared I2C master bus (Waveshare ESP32-S3-Touch-LCD-4.3B).
 *
 * One bus on I2C_NUM_0 carries the onboard PCF85063 RTC (0x51), the CH422G
 * expander (display/touch resets, backlight, SD CS) and the GT911 touch
 * controller — all on GPIO8/SDA, GPIO9/SCL. Whoever inits first creates the
 * bus; everyone else joins it (PLAN.md §4.3 board reference, M5).
 */
#ifndef AIO_I2C_BUS_H
#define AIO_I2C_BUS_H

#include "driver/i2c_master.h"

namespace aio {
namespace i2c {

/* Create-or-get the single I2C master bus (GPIO8/9, internal pullups on —
 * the board has external ones too). Idempotent; thread-unsafe by design:
 * bring-up happens on one task. */
i2c_master_bus_handle_t ensure_bus(int sda_gpio, int scl_gpio);

/* nullptr until ensure_bus() succeeded. */
i2c_master_bus_handle_t bus();

}  // namespace i2c
}  // namespace aio

#endif /* AIO_I2C_BUS_H */
