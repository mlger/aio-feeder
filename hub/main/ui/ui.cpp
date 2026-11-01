/*
 * AIO Feeder — LVGL dashboard view (PLAN.md §8; M4a phase 1, QEMU shot mode).
 *
 * Renders aio::ui::Dashboard snapshots (hub/src/ui_model.h) into a static
 * 400×240 RGB565 framebuffer — same aspect as the 800×480 panel, sized to
 * internal SRAM because QEMU has no PSRAM. On hardware the same scene code
 * renders at 800×480 once the RGB panel backend lands (M5).
 *
 * Screenshot protocol (CONFIG_AIO_UI_SHOT, captured by tools/qemu/
 * capture.py): after the scene settles, the task prints
 *
 *     AIO_SHOT dashboard
 *
 * every 5 s; the runner dumps g_shot_fb via the QEMU monitor `pmemsave`
 * (symbol address resolved from the ELF with nm) and converts it to PNG.
 * The frame is stable between markers because the snapshot only changes on
 * engine updates and the sim clock is pinned.
 *
 * LVGL 9 notes: single full-size buffer + RENDER_MODE_DIRECT + full_refresh
 * → g_shot_fb always holds the complete frame.
 */
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

#include "lvgl.h"

#include "ui.h"

#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT

#define UI_W 400
#define UI_H 240

/* QEMU pmemsave target — LVGL renders here (single full buffer, DIRECT). */
extern "C" uint8_t g_shot_fb[UI_W * UI_H * 2];
uint8_t g_shot_fb[UI_W * UI_H * 2];

/* --- palette (warm dark theme, §8) ---------------------------------------------- */

static const uint32_t C_BG = 0x15161A;
static const uint32_t C_PANEL = 0x1E2025;
static const uint32_t C_CARD = 0x232529;
static const uint32_t C_TEXT = 0xE8E4D8;
static const uint32_t C_MUTED = 0x9A9890;
static const uint32_t C_ONLINE = 0x6FBF73;
static const uint32_t C_STALE = 0xE0A458;
static const uint32_t C_OFFLINE = 0x676B70;
static const uint32_t C_ALERT = 0xE0A458;

/* --- snapshot inbox (lock-free double buffer) ------------------------------------ */

static Dashboard s_snap[2];
static volatile uint32_t s_seq = 0;
static uint32_t s_seen = 0;

void publish(const Dashboard &d) {
  const uint32_t s = s_seq;
  s_snap[s & 1] = d;
  s_seq = s + 1;
}

/* --- view state ------------------------------------------------------------------ */

static lv_display_t *s_disp = nullptr;
static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_clock_lbl = nullptr;
static lv_obj_t *s_footer_lbl = nullptr;

struct Card {
  lv_obj_t *root = nullptr;
  lv_obj_t *dot = nullptr;
  lv_obj_t *name = nullptr;
  lv_obj_t *next = nullptr;
  lv_obj_t *last = nullptr;
};
static Card s_cards[HUB_MAX_NODES];

static volatile uint32_t s_last_flush_ms = 0;
static uint32_t s_last_marker_ms = 0;

/* --- helpers --------------------------------------------------------------------- */

static void fmt_age(char *buf, size_t n, uint32_t s) {
  if (s < 90)
    snprintf(buf, n, "just now");
  else if (s < 3600)
    snprintf(buf, n, "%lu min ago", (unsigned long)(s / 60));
  else if (s < 86400)
    snprintf(buf, n, "%lu h %lu m ago", (unsigned long)(s / 3600),
             (unsigned long)((s % 3600) / 60));
  else
    snprintf(buf, n, "%lu d ago", (unsigned long)(s / 86400));
}

static const char *kind_short(uint8_t kind) {
  switch (kind) {
    case EV_FEED_OK: return "OK";
    case EV_FEED_FAIL: return "FAIL";
    case EV_FEED_REJ: return "REJECTED";
    case EV_FEED_UNKNOWN: return "NO LINK";
    default: return "?";
  }
}

static const char *alert_text(uint8_t kind) {
  switch (kind) {
    case EV_FEED_FAIL: return "feed failed";
    case EV_FEED_REJ: return "feed rejected";
    case EV_FEED_UNKNOWN: return "feed not confirmed";
    case EV_SKIPPED_MISSED: return "feed missed";
    default: return "check feeder";
  }
}

/* --- widget builders -------------------------------------------------------------- */

static lv_obj_t *new_panel(lv_obj_t *parent, int w, int h, uint32_t bg) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  return o;
}

static lv_obj_t *new_label(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                           uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  return l;
}

static void build_card(Card &c, lv_obj_t *parent) {
  c.root = new_panel(parent, 186, 88, C_CARD);
  lv_obj_set_style_radius(c.root, 10, 0);
  lv_obj_set_style_pad_all(c.root, 8, 0);
  lv_obj_set_flex_flow(c.root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(c.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(c.root, 4, 0);

  /* row 0: status dot + name */
  lv_obj_t *row = lv_obj_create(c.root);
  lv_obj_set_size(row, 170, 18);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 6, 0);

  c.dot = lv_obj_create(row);
  lv_obj_set_size(c.dot, 10, 10);
  lv_obj_set_style_radius(c.dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(c.dot, lv_color_hex(C_OFFLINE), 0);
  lv_obj_set_style_bg_opa(c.dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(c.dot, 0, 0);

  c.name = new_label(row, "Feeder", &lv_font_montserrat_14, C_TEXT);

  c.next = new_label(c.root, "no schedule", &lv_font_montserrat_14, C_TEXT);
  c.last = new_label(c.root, "no feeds yet", &lv_font_montserrat_12, C_MUTED);
}

static void build_screen() {
  s_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_pad_all(s_scr, 0, 0);

  /* header */
  lv_obj_t *header = new_panel(s_scr, UI_W, 36, C_PANEL);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(header, 10, 0);
  lv_obj_set_style_pad_right(header, 10, 0);
  lv_obj_set_style_pad_column(header, 8, 0);

  lv_obj_t *title = new_label(header, "aio-feeder", &lv_font_montserrat_20, C_TEXT);
  (void)title;
  lv_obj_t *spacer = lv_obj_create(header);
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spacer, 0, 0);
  lv_obj_set_flex_grow(spacer, 1);
  s_clock_lbl = new_label(header, "--:--", &lv_font_montserrat_16, C_MUTED);

  /* card grid */
  lv_obj_t *grid = lv_obj_create(s_scr);
  lv_obj_set_size(grid, UI_W, UI_H - 36 - 24);
  lv_obj_set_pos(grid, 0, 36);
  lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 0, 0);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(grid, 6, 0);
  for (int i = 0; i < HUB_MAX_NODES; i++) build_card(s_cards[i], grid);

  /* footer / alert strip */
  s_footer_lbl = new_label(s_scr, "starting...", &lv_font_montserrat_12, C_MUTED);
  lv_obj_set_pos(s_footer_lbl, 10, UI_H - 20);
}

/* --- snapshot → widgets ------------------------------------------------------------ */

static void update_dashboard(const Dashboard &d) {
  if (s_clock_lbl) lv_label_set_text(s_clock_lbl, d.clock);

  for (int i = 0; i < HUB_MAX_NODES; i++) {
    Card &c = s_cards[i];
    if (i >= d.row_count) {
      if (c.root) lv_obj_add_flag(c.root, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    if (c.root) lv_obj_remove_flag(c.root, LV_OBJ_FLAG_HIDDEN);
    const NodeRow &r = d.rows[i];

    char buf[48];
    snprintf(buf, sizeof(buf), "Feeder %u", (unsigned)r.id);
    lv_label_set_text(c.name, buf);

    const uint32_t dot = (r.state == NODE_ONLINE)   ? C_ONLINE
                         : (r.state == NODE_STALE)  ? C_STALE
                                                    : C_OFFLINE;
    lv_obj_set_style_bg_color(c.dot, lv_color_hex(dot), 0);

    if (r.has_next)
      snprintf(buf, sizeof(buf), "next %02u:%02u, %u g", (unsigned)(r.next_min_of_day / 60),
               (unsigned)(r.next_min_of_day % 60), (unsigned)r.next_amount);
    else
      snprintf(buf, sizeof(buf), "no schedule");
    lv_label_set_text(c.next, buf);

    if (r.has_last) {
      char age[24];
      fmt_age(age, sizeof(age), r.last_age_s);
      snprintf(buf, sizeof(buf), "last: %s, %u g, %s", kind_short(r.last_kind),
               (unsigned)r.last_amount, age);
      lv_obj_set_style_text_color(c.last, lv_color_hex(
                                              r.last_kind == EV_FEED_OK ? C_MUTED : C_ALERT),
                                  0);
    } else {
      snprintf(buf, sizeof(buf), "no feeds yet");
    }
    lv_label_set_text(c.last, buf);
  }

  if (s_footer_lbl) {
    char buf[80];
    if (d.has_alert)
      snprintf(buf, sizeof(buf), "Feeder %u: %s — check feeder", (unsigned)d.alert_node,
               alert_text(d.alert_kind));
    else if (d.row_count == 0)
      snprintf(buf, sizeof(buf), "No feeders yet — pair one to start");
    else
      snprintf(buf, sizeof(buf), "%u feeders, %u online, next feed in %ld min",
               (unsigned)d.total, (unsigned)d.online, (long)d.next_in_min);
    lv_label_set_text(s_footer_lbl, buf);
    lv_obj_set_style_text_color(s_footer_lbl,
                                lv_color_hex(d.has_alert ? C_ALERT : C_MUTED), 0);
  }
}

/* --- LVGL plumbing ----------------------------------------------------------------- */

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  (void)area;
  (void)px_map; /* LVGL rendered straight into g_shot_fb */
  s_last_flush_ms = (uint32_t)(esp_timer_get_time() / 1000);
  lv_display_flush_ready(disp);
}

static void ui_task(void *) {
  lv_init();
  s_disp = lv_display_create(UI_W, UI_H);
  lv_display_set_flush_cb(s_disp, flush_cb);
  lv_display_set_buffers(s_disp, g_shot_fb, nullptr, sizeof(g_shot_fb),
                         LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_full_refresh(s_disp, true);

  build_screen();
  lv_screen_load(s_scr);

  const uint32_t boot_ms = (uint32_t)(esp_timer_get_time() / 1000);
  uint32_t last_tick = boot_ms;
  printf("ui: LVGL %d.%d.%d ready (%dx%d shot framebuffer)\n", LVGL_VERSION_MAJOR,
         LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, UI_W, UI_H);

  for (;;) {
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    lv_tick_inc(now - last_tick);
    last_tick = now;

    const uint32_t s = s_seq;
    if (s != s_seen) {
      update_dashboard(s_snap[(s - 1) & 1]);
      s_seen = s;
    }

    lv_timer_handler();

    /* screenshot markers once the first render has settled (runner dumps
     * g_shot_fb after each marker; repeats every 5 s for reliability) */
    if (now - boot_ms > 6000 && s_last_flush_ms > 0 &&
        now - s_last_flush_ms > 400 && now - s_last_marker_ms > 5000) {
      printf("\nAIO_SHOT dashboard\n");
      fflush(stdout);
      s_last_marker_ms = now;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void start() {
  xTaskCreate(ui_task, "aio-ui", 16384, nullptr, 4, nullptr);
}

#else  /* !CONFIG_AIO_UI_SHOT — hardware build keeps the bench-host behaviour */

void start() {}
void publish(const Dashboard &) {}

#endif /* CONFIG_AIO_UI_SHOT */
