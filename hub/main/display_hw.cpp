/*
 * AIO Feeder — display + touch bring-up for the Waveshare 4.3B (M5).
 * See display_hw.h for the board facts and design notes.
 */
#include "display_hw.h"

#if defined(CONFIG_AIO_HW_DISPLAY) && CONFIG_AIO_HW_DISPLAY

#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_gt911.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#include "driver/gpio.h"
#include "i2c_bus.h"

namespace aio {
namespace display_hw {

static const char *TAG = "aio-display";

/* --- board constants (Waveshare demo + wiki, verified 2026-11) ---------- */

namespace board {
/* Shared touch/RTC/expander I2C bus (Kconfig AIO_RTC_SDA/SCL). */
constexpr int I2C_SDA = CONFIG_AIO_RTC_SDA;
constexpr int I2C_SCL = CONFIG_AIO_RTC_SCL;

/* CH422G register addresses (one per function — the chip decodes the
 * register in the I2C address). From Waveshare's esp_io_expander_ch422g.c:
 * WR-SET 0x24 (bit0 IO_OE = push-pull outputs enabled), WR-IO 0x38 (the
 * EXIO output latch, bits 0..5 = EXIO0..5), WR-OC 0x23 (open-drain —
 * unused here, power-on default 0x0F). */
constexpr uint8_t CH422G_WR_SET = 0x24;
constexpr uint8_t CH422G_WR_IO = 0x38;
constexpr uint8_t CH422G_IO_OE = 0x01;

/* EXIO bit indices (demo ordering). */
constexpr int EXIO_TP_RST = 1;
constexpr int EXIO_DISP = 2;    /* backlight, active high */
constexpr int EXIO_LCD_RST = 3;
constexpr int EXIO_SD_CS = 4;   /* high = deasserted (SDMMC 1-bit) */

/* GT911. */
constexpr gpio_num_t TP_INT = GPIO_NUM_4;
constexpr uint8_t GT911_ADDR = 0x5D; /* INT low during reset selects 0x5D */

/* ST7262 timings (demo: 16 MHz, 4/8/8 × 4/8/8, sample on falling PCLK). */
constexpr int PCLK_HZ = 16 * 1000 * 1000;
constexpr uint16_t HPW = 4, HBP = 8, HFP = 8;
constexpr uint16_t VPW = 4, VBP = 8, VFP = 8;

/* RGB565 data pins, esp_lcd data0..data15 = B0..B4, G0..G5, R0..R4. */
constexpr int DATA0_15[16] = {14, 38, 18, 17, 10, 39, 0, 45,
                              48, 47, 21, 1,  2,  42, 41, 40};
constexpr int HSYNC = 46, VSYNC = 3, DE = 5, PCLK = 7;

/* LVGL logical size (ui.cpp UI_W/UI_H) and the ×2 window on the panel. */
constexpr int LOGICAL_W = 352, LOGICAL_H = 240;
constexpr int PANEL_W = 800, PANEL_H = 480;
constexpr int SCALE = 2;
constexpr int WIN_W = LOGICAL_W * SCALE; /* 704 */
constexpr int WIN_H = LOGICAL_H * SCALE; /* 480 */
constexpr int WIN_X = (PANEL_W - WIN_W) / 2; /* 48 px bars */
constexpr int BOUNCE_PX = PANEL_W * 10;      /* demo: width × 10 */

}  // namespace board

/* --- state -------------------------------------------------------------- */

namespace {
lv_display_t *s_disp = nullptr;
lv_indev_t *s_indev = nullptr;
esp_lcd_panel_handle_t s_panel = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;
i2c_master_dev_handle_t s_ch422g_set = nullptr;
i2c_master_dev_handle_t s_ch422g_io = nullptr;
uint16_t *s_panel_fb = nullptr; /* the panel's single PSRAM framebuffer */
uint16_t *s_lv_buf = nullptr;   /* LVGL full-frame draw buffer (PSRAM) */
}  // namespace

/* --- CH422G ------------------------------------------------------------- */

static bool ch422g_init() {
  i2c_master_bus_handle_t bus = i2c::ensure_bus(board::I2C_SDA, board::I2C_SCL);
  if (!bus) return false;

  i2c_device_config_t dev{};
  dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev.scl_speed_hz = 400 * 1000;
  dev.device_address = board::CH422G_WR_SET;
  if (i2c_master_bus_add_device(bus, &dev, &s_ch422g_set) != ESP_OK) return false;
  dev.device_address = board::CH422G_WR_IO;
  if (i2c_master_bus_add_device(bus, &dev, &s_ch422g_io) != ESP_OK) return false;

  /* push-pull outputs enabled (WR-SET bit0 IO_OE; power-on default is 1,
   * write it anyway so the chip is in a known state) */
  uint8_t set = board::CH422G_IO_OE;
  if (i2c_master_transmit(s_ch422g_set, &set, 1, -1) != ESP_OK) return false;

  /* EXIO levels: everything high except the backlight (DISP off until the
   * panel is up). SD_CS deasserted; LCD_RST / TP_RST high after the dances
   * below. */
  uint8_t io = 0xFF & ~(1u << board::EXIO_DISP);
  if (i2c_master_transmit(s_ch422g_io, &io, 1, -1) != ESP_OK) return false;
  return true;
}

static bool ch422g_exio(int bit, int level) {
  static uint8_t levels = 0xFF & ~(1u << board::EXIO_DISP); /* mirror */
  if (level)
    levels |= (uint8_t)(1u << bit);
  else
    levels &= (uint8_t)~(1u << bit);
  return i2c_master_transmit(s_ch422g_io, &levels, 1, -1) == ESP_OK;
}

/* --- reset dances (ordering from the Waveshare demo) -------------------- */

static void panel_reset_dance() {
  ch422g_exio(board::EXIO_LCD_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(10));
  ch422g_exio(board::EXIO_LCD_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(100));
}

static bool touch_reset_dance() {
  /* INT output low while TP_RST comes back up → GT911 at 0x5D */
  gpio_config_t io{};
  io.pin_bit_mask = 1ULL << board::TP_INT;
  io.mode = GPIO_MODE_OUTPUT;
  if (gpio_config(&io) != ESP_OK) return false;
  gpio_set_level(board::TP_INT, 0);
  vTaskDelay(pdMS_TO_TICKS(10));

  ch422g_exio(board::EXIO_TP_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(100));
  ch422g_exio(board::EXIO_TP_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(200));

  /* INT back to the peripheral/input domain */
  if (gpio_reset_pin(board::TP_INT) != ESP_OK) return false;
  return true;
}

/* --- ST7262 RGB panel --------------------------------------------------- */

static bool panel_init() {
  esp_lcd_rgb_panel_config_t cfg{};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.data_width = 16; /* RGB565 in 16-bit bus mode */
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 1;
  cfg.bounce_buffer_size_px = board::BOUNCE_PX;
  cfg.hsync_gpio_num = board::HSYNC;
  cfg.vsync_gpio_num = board::VSYNC;
  cfg.de_gpio_num = board::DE;
  cfg.pclk_gpio_num = board::PCLK;
  cfg.disp_gpio_num = -1;
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = board::DATA0_15[i];
  cfg.timings.pclk_hz = board::PCLK_HZ;
  cfg.timings.h_res = board::PANEL_W;
  cfg.timings.v_res = board::PANEL_H;
  cfg.timings.hsync_pulse_width = board::HPW;
  cfg.timings.hsync_back_porch = board::HBP;
  cfg.timings.hsync_front_porch = board::HFP;
  cfg.timings.vsync_pulse_width = board::VPW;
  cfg.timings.vsync_back_porch = board::VBP;
  cfg.timings.vsync_front_porch = board::VFP;
  cfg.timings.flags.pclk_active_neg = 1; /* demo: sample on the falling edge */
  cfg.flags.fb_in_psram = 1;
  cfg.flags.refresh_on_demand = 1; /* flush_cb restarts the one-shot DMA */

  if (esp_lcd_new_rgb_panel(&cfg, &s_panel) != ESP_OK) {
    ESP_LOGE(TAG, "rgb panel init failed");
    return false;
  }
  if (esp_lcd_panel_reset(s_panel) != ESP_OK ||
      esp_lcd_panel_init(s_panel) != ESP_OK) {
    ESP_LOGE(TAG, "rgb panel start failed");
    return false;
  }

  /* the panel's one framebuffer — bars and all */
  uint8_t *fb = nullptr;
  if (esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb) != ESP_OK || !fb) {
    return false;
  }
  s_panel_fb = (uint16_t *)fb;
  memset(s_panel_fb, 0, board::PANEL_W * board::PANEL_H * sizeof(uint16_t));
  return true;
}

/* --- LVGL glue ---------------------------------------------------------- */

/* 2× nearest-neighbor expand: LVGL 352×240 → 704×480 centered in 800×480.
 * Straight rows for the top/bottom bars stay black from init. */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  (void)area;
  const uint16_t *src = (const uint16_t *)px_map;
  const int x0 = board::WIN_X;
  for (int y = 0; y < board::LOGICAL_H; y++) {
    uint16_t *dst = s_panel_fb + (size_t)(y * board::SCALE) * board::PANEL_W + x0;
    const uint16_t *row = src + (size_t)y * board::LOGICAL_W;
    for (int x = 0; x < board::LOGICAL_W; x++) {
      const uint16_t px = row[x];
      dst[2 * x] = px;
      dst[2 * x + 1] = px;
    }
    memcpy(dst + board::PANEL_W, dst, board::WIN_W * sizeof(uint16_t));
  }
  esp_lcd_rgb_panel_restart(s_panel);
  lv_display_flush_ready(disp);
}

/* GT911 poll → logical coords (undo the ×2 window). */
static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  uint16_t x[1] = {0}, y[1] = {0};
  uint16_t n = 0;
  esp_lcd_touch_read_data(s_touch);
  bool pressed = esp_lcd_touch_get_coordinates(s_touch, x, y, nullptr, &n, 1) && n > 0;
  if (pressed) {
    int lx = ((int)x[0] - board::WIN_X) / board::SCALE;
    int ly = (int)y[0] / board::SCALE;
    if (lx < 0) lx = 0;
    if (lx >= board::LOGICAL_W) lx = board::LOGICAL_W - 1;
    if (ly < 0) ly = 0;
    if (ly >= board::LOGICAL_H) ly = board::LOGICAL_H - 1;
    data->point.x = (lv_coord_t)lx;
    data->point.y = (lv_coord_t)ly;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

/* --- public API ---------------------------------------------------------- */

bool init() {
  if (!ch422g_init()) {
    ESP_LOGE(TAG, "CH422G not responding (SDA=%d SCL=%d)", board::I2C_SDA,
             board::I2C_SCL);
    return false;
  }
  panel_reset_dance();
  if (!touch_reset_dance()) {
    ESP_LOGE(TAG, "GT911 reset dance failed");
    return false;
  }
  if (!panel_init()) return false;

  /* GT911 device on the shared bus (esp_lcd panel-IO path). Fields are
   * assigned, not brace-initialized — the component's
   * ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG() macro predates the IDF 5.4 struct
   * field order and fails as a designated initializer in C++. */
  esp_lcd_panel_io_i2c_config_t io_cfg = {};
  io_cfg.dev_addr = board::GT911_ADDR;
  io_cfg.scl_speed_hz = 400 * 1000;
  io_cfg.control_phase_bytes = 1;
  io_cfg.dc_bit_offset = 0;
  io_cfg.lcd_cmd_bits = 16;
  io_cfg.flags.disable_control_phase = 1;
  esp_lcd_panel_io_handle_t io = nullptr;
  if (esp_lcd_new_panel_io_i2c(i2c::bus(), &io_cfg, &io) != ESP_OK) {
    ESP_LOGE(TAG, "touch panel-io init failed");
    return false;
  }
  esp_lcd_touch_config_t tp_cfg{};
  tp_cfg.x_max = board::PANEL_W;
  tp_cfg.y_max = board::PANEL_H;
  tp_cfg.rst_gpio_num = GPIO_NUM_NC; /* reset went through the CH422G */
  tp_cfg.int_gpio_num = board::TP_INT;
  if (esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &s_touch) != ESP_OK) {
    ESP_LOGE(TAG, "GT911 init failed");
    return false;
  }

  /* LVGL: logical 352×240, full-frame PSRAM draw buffer */
  s_lv_buf = (uint16_t *)heap_caps_calloc(board::LOGICAL_W * board::LOGICAL_H,
                                          sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  if (!s_lv_buf) {
    ESP_LOGE(TAG, "LVGL PSRAM buffer alloc failed");
    return false;
  }
  s_disp = lv_display_create(board::LOGICAL_W, board::LOGICAL_H);
  lv_display_set_flush_cb(s_disp, flush_cb);
  lv_display_set_buffers(s_disp, s_lv_buf, nullptr,
                         board::LOGICAL_W * board::LOGICAL_H * sizeof(uint16_t),
                         LV_DISPLAY_RENDER_MODE_FULL);

  s_indev = lv_indev_create();
  lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(s_indev, touch_cb);
  lv_indev_set_display(s_indev, s_disp);

  backlight(true);
  ESP_LOGI(TAG, "panel 800x480 + GT911 up (LVGL %dx%d logical, x%d)",
           board::LOGICAL_W, board::LOGICAL_H, board::SCALE);
  return true;
}

lv_display_t *display() { return s_disp; }
lv_indev_t *indev() { return s_indev; }

void backlight(bool on) {
  if (!s_ch422g_io) return;
  ch422g_exio(board::EXIO_DISP, on ? 1 : 0);
}

}  // namespace display_hw
}  // namespace aio

#endif /* CONFIG_AIO_HW_DISPLAY */
