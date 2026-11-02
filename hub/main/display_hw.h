/*
 * AIO Feeder — Waveshare ESP32-S3-Touch-LCD-4.3B display + touch bring-up
 * (PLAN.md §7.1 display HAL, §12 M5; board reference docs.waveshare.com /
 * ESP32-S3-Touch-LCD-4.3B + the official demo's esp_panel_board_custom_conf.h).
 *
 *   - ST7262 plain-RGB panel, 800×480 RGB565, 16 MHz PCLK — no control
 *     panel, no init sequence (the demo sets USE_CONTROL_PANEL=0).
 *   - CH422G expander on the shared I2C bus: EXIO3 = LCD_RST,
 *     EXIO1 = TP_RST, EXIO2 = DISP (backlight, active high),
 *     EXIO4 = SD_CS (high = deasserted in SDMMC 1-bit mode).
 *   - GT911 touch on the same bus at 0x5D (INT GPIO4 low during the reset
 *     dance selects 0x5D).
 *   - LVGL runs at the UI's logical 352×240 (ui.cpp UI_W/UI_H); flush
 *     expands ×2 into the 704×480 window centered on the panel (48 px bars
 *     each side) and restarts the panel's one-shot DMA refresh
 *     (refresh_on_demand = 1 → no tearing mid-write).
 *
 * Compiled but inert unless CONFIG_AIO_HW_DISPLAY; the QEMU shot build
 * never calls it (Kconfig: depends on !AIO_UI_SHOT). A display failure
 * must never take the feeder down (§3.3): init() reports false and the
 * caller keeps running headless.
 */
#ifndef AIO_DISPLAY_HW_H
#define AIO_DISPLAY_HW_H

#include "lvgl.h"

namespace aio {
namespace display_hw {

/* Full bring-up: shared I2C bus → CH422G (outputs on, reset dances) →
 * ST7262 RGB panel (single PSRAM fb, 8000 px bounce, refresh on demand) →
 * GT911 → backlight ON. Creates the LVGL display (352×240 logical, full
 * PSRAM draw buffer) and the touch POINTER indev. Returns false on any
 * failure; safe to leave uncalled (hardware builds call it from ui::start). */
bool init();

/* Valid only after init() returned true. */
lv_display_t *display();
lv_indev_t *indev();

/* CH422G EXIO2 (DISP), active high. */
void backlight(bool on);

#if !defined(CONFIG_AIO_HW_DISPLAY) || !CONFIG_AIO_HW_DISPLAY
/* No-display build (QEMU shot / console-only): inline no-ops so ui.cpp
 * compiles unchanged and display_hw.cpp contributes nothing. */
inline bool init() { return false; }
inline lv_display_t *display() { return nullptr; }
inline lv_indev_t *indev() { return nullptr; }
inline void backlight(bool) {}
#endif

}  // namespace display_hw
}  // namespace aio

#endif /* AIO_DISPLAY_HW_H */
