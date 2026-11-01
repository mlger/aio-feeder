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

namespace aio {
namespace ui {

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

/* --- snapshot inbox (lock-free double buffers) ------------------------------------ */

static Dashboard s_snap[2];
static NodeDetail s_det_snap[2];
static SchedEditor s_edit_snap[2];
static HistoryView s_hist_snap[2];
static volatile uint32_t s_seq = 0;
static volatile uint32_t s_det_seq = 0;
static volatile uint32_t s_edit_seq = 0;
static volatile uint32_t s_hist_seq = 0;
static uint32_t s_seen = 0;
static uint32_t s_det_seen = 0;
static uint32_t s_edit_seen = 0;
static uint32_t s_hist_seen = 0;
static volatile uint8_t s_detail_id = 1; /* which node the detail screen shows */

void publish(const Dashboard &d) {
  const uint32_t s = s_seq;
  s_snap[s & 1] = d;
  s_seq = s + 1;
}

void publish_detail(const NodeDetail &d) {
  const uint32_t s = s_det_seq;
  s_det_snap[s & 1] = d;
  s_det_seq = s + 1;
}

void publish_sched_editor(const SchedEditor &e) {
  const uint32_t s = s_edit_seq;
  s_edit_snap[s & 1] = e;
  s_edit_seq = s + 1;
}

void publish_history(const HistoryView &h) {
  const uint32_t s = s_hist_seq;
  s_hist_snap[s & 1] = h;
  s_hist_seq = s + 1;
}

uint8_t detail_node() { return s_detail_id; }

void set_detail_node(uint8_t node_id) {
  if (node_id) s_detail_id = node_id;
}

/* --- view state ------------------------------------------------------------------ */

static lv_display_t *s_disp = nullptr;
static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_clock_lbl = nullptr;
static lv_obj_t *s_footer_lbl = nullptr;

/* node detail screen (§8.2) */
static lv_obj_t *s_det_scr = nullptr;
static lv_obj_t *s_det_clock_lbl = nullptr;
static lv_obj_t *s_det_title_lbl = nullptr;
static lv_obj_t *s_det_status_lbl = nullptr;
static lv_obj_t *s_det_next_lbl = nullptr;
static lv_obj_t *s_det_gate_lbl = nullptr;   /* hidden unless a gate is active */
static lv_obj_t *s_det_sched_panel = nullptr;
static lv_obj_t *s_det_sched_rows[UI_DETAIL_MAX_ENTRIES];
static lv_obj_t *s_det_ev_panel = nullptr;
static lv_obj_t *s_det_ev_rows[UI_DETAIL_MAX_EVENTS];

/* schedule editor screen (§8.3) */
static lv_obj_t *s_ed_scr = nullptr;
static lv_obj_t *s_ed_title_lbl = nullptr;
static lv_obj_t *s_ed_clock_lbl = nullptr;
static lv_obj_t *s_ed_tz_lbl = nullptr;
static lv_obj_t *s_ed_rows[UI_DETAIL_MAX_ENTRIES];     /* entry list rows */
static lv_obj_t *s_ed_time_lbl = nullptr;              /* draft time */
static lv_obj_t *s_ed_amt_lbl = nullptr;               /* draft amount */
static lv_obj_t *s_ed_day_btns[7];                     /* weekday chips */
static lv_obj_t *s_ed_gate_lbl = nullptr;              /* gate/skip footer */
static const uint32_t C_SEL = 0x35383F;                /* selected row bg */
static const uint32_t C_BAR = 0x6FBF73;                /* chart bars (online green) */

/* history screen (§8.6) */
static lv_obj_t *s_hi_scr = nullptr;
static lv_obj_t *s_hi_clock_lbl = nullptr;
static lv_obj_t *s_hi_bars[UI_HISTORY_DAYS];
static lv_obj_t *s_hi_day_lbls[UI_HISTORY_DAYS];
static lv_obj_t *s_hi_nodes_lbl = nullptr;
static lv_obj_t *s_hi_ev_rows[UI_HISTORY_ROWS];
static lv_obj_t *s_hi_footer_lbl = nullptr;

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

static void days_text(uint8_t mask, char *buf, size_t n) {
  static const char *WD[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  if (mask == 0x7F) {
    snprintf(buf, n, "daily");
    return;
  }
  if (mask == 0x1F) { /* Mon–Fri */
    snprintf(buf, n, "weekdays");
    return;
  }
  if (mask == 0x60) { /* Sat+Sun */
    snprintf(buf, n, "weekends");
    return;
  }
  size_t off = 0;
  buf[0] = 0;
  for (int d = 0; d < 7 && off < n - 4; d++) {
    if (!(mask & (1u << d))) continue;
    off += (size_t)snprintf(buf + off, n - off, "%s%s", off ? "," : "", WD[d]);
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
      snprintf(buf, sizeof(buf), "Feeder %u: %s - check feeder", (unsigned)d.alert_node,
               alert_text(d.alert_kind));
    else if (d.row_count == 0)
      snprintf(buf, sizeof(buf), "No feeders yet - pair one to start");
    else
      snprintf(buf, sizeof(buf), "%u feeders, %u online, next feed in %ld min",
               (unsigned)d.total, (unsigned)d.online, (long)d.next_in_min);
    lv_label_set_text(s_footer_lbl, buf);
    lv_obj_set_style_text_color(s_footer_lbl,
                                lv_color_hex(d.has_alert ? C_ALERT : C_MUTED), 0);
  }
}

/* --- node detail screen (§8.2) ----------------------------------------------------- */

static void build_detail_screen() {
  s_det_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_det_scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_pad_all(s_det_scr, 0, 0);

  /* header: back affordance + title + clock */
  lv_obj_t *header = new_panel(s_det_scr, UI_W, 32, C_PANEL);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(header, 10, 0);
  lv_obj_set_style_pad_right(header, 10, 0);
  lv_obj_set_style_pad_column(header, 8, 0);

  lv_obj_t *back = new_label(header, "<", &lv_font_montserrat_20, C_MUTED);
  (void)back;
  s_det_title_lbl = new_label(header, "Feeder", &lv_font_montserrat_16, C_TEXT);
  lv_obj_t *spacer = lv_obj_create(header);
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spacer, 0, 0);
  lv_obj_set_flex_grow(spacer, 1);
  s_det_clock_lbl = new_label(header, "--:--", &lv_font_montserrat_14, C_MUTED);

  /* scrollable content column */
  lv_obj_t *col = lv_obj_create(s_det_scr);
  lv_obj_set_size(col, UI_W, UI_H - 32);
  lv_obj_set_pos(col, 0, 32);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 0, 0);
  lv_obj_set_style_pad_column(col, 0, 0);
  lv_obj_set_style_pad_row(col, 4, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_scroll_dir(col, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_AUTO);

  /* status strip */
  lv_obj_t *status = new_panel(col, UI_W, 24, C_PANEL);
  lv_obj_set_style_pad_left(status, 10, 0);
  lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(status, 6, 0);
  s_det_status_lbl = new_label(status, "-", &lv_font_montserrat_12, C_TEXT);

  /* next feed + schedule push freshness */
  lv_obj_t *nextrow = new_panel(col, UI_W, 22, C_BG);
  lv_obj_set_style_pad_left(nextrow, 10, 0);
  lv_obj_set_flex_flow(nextrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(nextrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  s_det_next_lbl = new_label(nextrow, "no schedule", &lv_font_montserrat_12, C_TEXT);

  /* gate banner (skip/hold), hidden by default */
  s_det_gate_lbl = new_label(col, "", &lv_font_montserrat_12, C_ALERT);
  lv_obj_set_style_pad_left(s_det_gate_lbl, 10, 0);
  lv_obj_add_flag(s_det_gate_lbl, LV_OBJ_FLAG_HIDDEN);

  /* schedule panel: header + up to UI_DETAIL_MAX_ENTRIES rows */
  s_det_sched_panel = new_panel(col, UI_W, 0, C_PANEL);
  lv_obj_set_size(s_det_sched_panel, UI_W - 20, 0);
  lv_obj_set_style_pad_all(s_det_sched_panel, 6, 0);
  lv_obj_set_flex_flow(s_det_sched_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_det_sched_panel, 2, 0);
  lv_obj_t *sh = new_label(s_det_sched_panel, "SCHEDULE", &lv_font_montserrat_12, C_MUTED);
  (void)sh;
  for (int i = 0; i < UI_DETAIL_MAX_ENTRIES; i++) {
    lv_obj_t *row = lv_obj_create(s_det_sched_panel);
    lv_obj_set_size(row, UI_W - 32 - 12, 18);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_t *t = new_label(row, "", &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_width(t, 64);
    lv_obj_t *w = new_label(row, "", &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_flex_grow(w, 1);
    lv_obj_t *a = new_label(row, "", &lv_font_montserrat_14, C_TEXT);
    s_det_sched_rows[i] = row;
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
  }

  /* recent feeds panel: header + up to UI_DETAIL_MAX_EVENTS rows */
  s_det_ev_panel = new_panel(col, UI_W, 0, C_PANEL);
  lv_obj_set_size(s_det_ev_panel, UI_W - 20, 0);
  lv_obj_set_style_pad_all(s_det_ev_panel, 6, 0);
  lv_obj_set_flex_flow(s_det_ev_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_det_ev_panel, 2, 0);
  lv_obj_t *eh = new_label(s_det_ev_panel, "RECENT FEEDS", &lv_font_montserrat_12, C_MUTED);
  (void)eh;
  for (int i = 0; i < UI_DETAIL_MAX_EVENTS; i++) {
    lv_obj_t *row = lv_obj_create(s_det_ev_panel);
    lv_obj_set_size(row, UI_W - 32 - 12, 18);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_t *l = new_label(row, "", &lv_font_montserrat_12, C_TEXT);
    (void)l;
    s_det_ev_rows[i] = row;
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
  }

  /* footer actions (static in the shot build; wired to the engine in M4b) */
  lv_obj_t *actions = new_panel(s_det_scr, UI_W, 26, C_PANEL);
  lv_obj_set_pos(actions, 0, UI_H - 26);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  const char *labels[3] = {"Feed now (hold)", "Skip next", "Hold 1 h"};
  for (int i = 0; i < 3; i++) {
    lv_obj_t *btn = lv_btn_create(actions);
    lv_obj_set_size(btn, 118, 20);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_CARD), 0);
    new_label(btn, labels[i], &lv_font_montserrat_12, C_TEXT);
  }
}

static void update_detail(const NodeDetail &d) {
  char buf[64];
  if (s_det_clock_lbl) lv_label_set_text(s_det_clock_lbl, d.clock);
  snprintf(buf, sizeof(buf), "Feeder %u", (unsigned)s_detail_id);
  if (s_det_title_lbl) lv_label_set_text(s_det_title_lbl, buf);

  if (!s_det_status_lbl) return;
  if (!d.found) {
    lv_label_set_text(s_det_status_lbl, "unknown node");
    return;
  }

  const char *st = d.state == NODE_ONLINE ? "online" : d.state == NODE_STALE ? "stale" : "offline";
  const uint32_t dotc = (d.state == NODE_ONLINE)   ? C_ONLINE
                        : (d.state == NODE_STALE)  ? C_STALE
                                                   : C_OFFLINE;
  if (d.heard) {
    char age[24];
    fmt_age(age, sizeof(age), d.last_rx_age_s);
    snprintf(buf, sizeof(buf), "%s - %u.%u V - seen %s",
             st, (unsigned)(d.vin_mv / 1000), (unsigned)((d.vin_mv % 1000) / 100), age);
  } else {
    snprintf(buf, sizeof(buf), "%s - %u.%u V - not heard this boot",
             st, (unsigned)(d.vin_mv / 1000), (unsigned)((d.vin_mv % 1000) / 100));
  }
  lv_label_set_text(s_det_status_lbl, buf);
  lv_obj_set_style_text_color(s_det_status_lbl, lv_color_hex(dotc), 0);

  if (d.has_next)
    snprintf(buf, sizeof(buf), "next %02u:%02u - %u g - %s",
             (unsigned)(d.next_min_of_day / 60), (unsigned)(d.next_min_of_day % 60),
             (unsigned)d.next_amount, d.pushed);
  else
    snprintf(buf, sizeof(buf), "no schedule - %s", d.pushed);
  lv_label_set_text(s_det_next_lbl, buf);

  if (d.has_skip || d.has_hold) {
    snprintf(buf, sizeof(buf), "%s active - %ld min left",
             d.has_hold ? "hold" : "skip", (long)d.gate_left_min);
    lv_label_set_text(s_det_gate_lbl, buf);
    lv_obj_remove_flag(s_det_gate_lbl, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_det_gate_lbl, LV_OBJ_FLAG_HIDDEN);
  }

  for (int i = 0; i < UI_DETAIL_MAX_ENTRIES; i++) {
    lv_obj_t *row = s_det_sched_rows[i];
    if (!row) continue;
    if (!d.sched_enabled || i >= d.entry_count) {
      lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *t = lv_obj_get_child(row, 0);
    lv_obj_t *w = lv_obj_get_child(row, 1);
    lv_obj_t *a = lv_obj_get_child(row, 2);
    const DetailSchedEntry &e = d.entries[i];
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(e.min_of_day / 60),
             (unsigned)(e.min_of_day % 60));
    if (t) lv_label_set_text((lv_obj_t *)t, buf);
    days_text(e.days_mask, buf, sizeof(buf));
    if (w) lv_label_set_text((lv_obj_t *)w, buf);
    snprintf(buf, sizeof(buf), "%u g", (unsigned)e.amount);
    if (a) lv_label_set_text((lv_obj_t *)a, buf);
  }

  for (int i = 0; i < UI_DETAIL_MAX_EVENTS; i++) {
    lv_obj_t *row = s_det_ev_rows[i];
    if (!row) continue;
    if (i >= d.event_count) {
      lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
    const DetailEvent &ev = d.events[i];
    char age[24];
    fmt_age(age, sizeof(age), ev.age_s);
    snprintf(buf, sizeof(buf), "%s - %u g - %s", kind_short(ev.kind),
             (unsigned)ev.amount, age);
    lv_obj_t *l = lv_obj_get_child(row, 0);
    if (l) {
      lv_label_set_text((lv_obj_t *)l, buf);
      lv_obj_set_style_text_color(
          (lv_obj_t *)l, lv_color_hex(ev.kind == EV_FEED_OK ? C_TEXT : C_ALERT), 0);
    }
  }
}

/* --- schedule editor screen (§8.3) -------------------------------------------------- */

static lv_obj_t *ed_row(lv_obj_t *parent, lv_obj_t **t, lv_obj_t **a) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_size(row, UI_W - 20, 20);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_style_pad_left(row, 8, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_t *tl = new_label(row, "", &lv_font_montserrat_14, C_TEXT);
  lv_obj_set_width(tl, 58);
  lv_obj_t *w = new_label(row, "", &lv_font_montserrat_12, C_MUTED);
  lv_obj_set_flex_grow(w, 1);
  lv_obj_t *al = new_label(row, "", &lv_font_montserrat_14, C_TEXT);
  if (t) *t = tl;
  if (a) *a = al;
  return row;
}

static void build_edit_screen() {
  s_ed_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_ed_scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_pad_all(s_ed_scr, 0, 0);

  /* header: back + title + tz + clock */
  lv_obj_t *header = new_panel(s_ed_scr, UI_W, 32, C_PANEL);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(header, 10, 0);
  lv_obj_set_style_pad_right(header, 10, 0);
  lv_obj_set_style_pad_column(header, 8, 0);
  lv_obj_t *back = new_label(header, "<", &lv_font_montserrat_20, C_MUTED);
  (void)back;
  s_ed_title_lbl = new_label(header, "Schedule", &lv_font_montserrat_16, C_TEXT);
  lv_obj_t *spacer = lv_obj_create(header);
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spacer, 0, 0);
  lv_obj_set_flex_grow(spacer, 1);
  s_ed_tz_lbl = new_label(header, "UTC", &lv_font_montserrat_12, C_MUTED);
  s_ed_clock_lbl = new_label(header, "--:--", &lv_font_montserrat_14, C_MUTED);

  /* scrollable column: entry list + edit panel */
  lv_obj_t *col = lv_obj_create(s_ed_scr);
  lv_obj_set_size(col, UI_W, UI_H - 32 - 24);
  lv_obj_set_pos(col, 0, 32);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 0, 0);
  lv_obj_set_style_pad_row(col, 4, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_scroll_dir(col, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_AUTO);

  lv_obj_t *list = new_panel(col, UI_W - 20, 0, C_PANEL);
  lv_obj_set_style_pad_all(list, 4, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 2, 0);
  lv_obj_t *lh = new_label(list, "ENTRIES", &lv_font_montserrat_12, C_MUTED);
  (void)lh;
  for (int i = 0; i < UI_DETAIL_MAX_ENTRIES; i++) {
    lv_obj_t *t = nullptr, *a = nullptr;
    s_ed_rows[i] = ed_row(list, &t, &a);
    (void)t;
    (void)a;
    lv_obj_add_flag(s_ed_rows[i], LV_OBJ_FLAG_HIDDEN);
  }

  lv_obj_t *edit = new_panel(col, UI_W - 20, 96, C_PANEL);
  lv_obj_set_style_pad_all(edit, 6, 0);
  lv_obj_set_flex_flow(edit, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(edit, 4, 0);

  lv_obj_t *eh = new_label(edit, "EDIT SELECTED", &lv_font_montserrat_12, C_MUTED);
  (void)eh;

  /* time + amount draft row */
  lv_obj_t *ta = lv_obj_create(edit);
  lv_obj_set_size(ta, UI_W - 32, 22);
  lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ta, 0, 0);
  lv_obj_set_style_pad_all(ta, 0, 0);
  lv_obj_set_flex_flow(ta, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(ta, 10, 0);
  lv_obj_t *tlbl = new_label(ta, "time", &lv_font_montserrat_12, C_MUTED);
  (void)tlbl;
  s_ed_time_lbl = new_label(ta, "07:30", &lv_font_montserrat_14, C_TEXT);
  lv_obj_t *albl = new_label(ta, "amount", &lv_font_montserrat_12, C_MUTED);
  (void)albl;
  s_ed_amt_lbl = new_label(ta, "40 g", &lv_font_montserrat_14, C_TEXT);

  /* weekday chips */
  lv_obj_t *days = lv_obj_create(edit);
  lv_obj_set_size(days, UI_W - 32, 20);
  lv_obj_set_style_bg_opa(days, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(days, 0, 0);
  lv_obj_set_style_pad_all(days, 0, 0);
  lv_obj_set_flex_flow(days, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(days, 3, 0);
  static const char *DCH[7] = {"S", "M", "T", "W", "T", "F", "S"};
  for (int d = 0; d < 7; d++) {
    lv_obj_t *b = lv_btn_create(days);
    lv_obj_set_size(b, 46, 18);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
    s_ed_day_btns[d] = b;
    lv_obj_t *l = new_label(b, DCH[d], &lv_font_montserrat_12, C_MUTED);
    (void)l;
  }

  /* footer: gate state / push freshness */
  s_ed_gate_lbl = new_label(s_ed_scr, "", &lv_font_montserrat_12, C_MUTED);
  lv_obj_set_pos(s_ed_gate_lbl, 10, UI_H - 20);
}

static void update_edit(const SchedEditor &e) {
  char buf[48];
  if (s_ed_clock_lbl) lv_label_set_text(s_ed_clock_lbl, e.clock);
  if (s_ed_tz_lbl) lv_label_set_text(s_ed_tz_lbl, e.tz);
  snprintf(buf, sizeof(buf), "Feeder %u schedule", (unsigned)e.node_id);
  if (s_ed_title_lbl) lv_label_set_text(s_ed_title_lbl, buf);

  for (int i = 0; i < UI_DETAIL_MAX_ENTRIES; i++) {
    lv_obj_t *row = s_ed_rows[i];
    if (!row) continue;
    if (!e.found || i >= e.entry_count) {
      lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(row, lv_color_hex(i == e.selected ? C_SEL : C_PANEL), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_t *t = lv_obj_get_child(row, 0);
    lv_obj_t *w = lv_obj_get_child(row, 1);
    lv_obj_t *a = lv_obj_get_child(row, 2);
    const DetailSchedEntry &en = e.entries[i];
    if (t) {
      snprintf(buf, sizeof(buf), "%s%02u:%02u", i == e.selected ? "> " : "  ",
               (unsigned)(en.min_of_day / 60), (unsigned)(en.min_of_day % 60));
      lv_label_set_text((lv_obj_t *)t, buf);
    }
    if (w) {
      days_text(en.days_mask, buf, sizeof(buf));
      lv_label_set_text((lv_obj_t *)w, buf);
    }
    if (a) {
      snprintf(buf, sizeof(buf), "%u g", (unsigned)en.amount);
      lv_label_set_text((lv_obj_t *)a, buf);
    }
  }

  if (s_ed_time_lbl) {
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(e.d_min_of_day / 60),
             (unsigned)(e.d_min_of_day % 60));
    lv_label_set_text(s_ed_time_lbl, buf);
  }
  if (s_ed_amt_lbl) {
    snprintf(buf, sizeof(buf), "%u g", (unsigned)e.d_amount);
    lv_label_set_text(s_ed_amt_lbl, buf);
  }
  for (int d = 0; d < 7; d++) {
    lv_obj_t *b = s_ed_day_btns[d];
    if (!b) continue;
    const bool on = e.d_days & (1u << d);
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? C_SEL : C_CARD), 0);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (l)
      lv_obj_set_style_text_color((lv_obj_t *)l,
                                  lv_color_hex(on ? C_TEXT : C_MUTED), 0);
  }
  if (s_ed_gate_lbl) {
    if (e.has_skip || e.has_hold)
      snprintf(buf, sizeof(buf), "%s active - %ld min left",
               e.has_hold ? "hold" : "skip", (long)e.gate_left_min);
    else
      snprintf(buf, sizeof(buf), "no gates - %s",
               e.enabled ? "schedule enabled" : "schedule paused");
    lv_label_set_text(s_ed_gate_lbl, buf);
    lv_obj_set_style_text_color(s_ed_gate_lbl,
                                lv_color_hex((e.has_skip || e.has_hold) ? C_ALERT : C_MUTED),
                                0);
  }
}

/* --- history screen (§8.6) ----------------------------------------------------------- */

#define HI_CHART_H 64 /* bar area height inside the chart panel */

static void build_history_screen() {
  s_hi_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_hi_scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_pad_all(s_hi_scr, 0, 0);

  /* header */
  lv_obj_t *header = new_panel(s_hi_scr, UI_W, 30, C_PANEL);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(header, 10, 0);
  lv_obj_set_style_pad_right(header, 10, 0);
  lv_obj_set_style_pad_column(header, 8, 0);
  lv_obj_t *back = new_label(header, "<", &lv_font_montserrat_20, C_MUTED);
  (void)back;
  lv_obj_t *title = new_label(header, "History", &lv_font_montserrat_16, C_TEXT);
  (void)title;
  lv_obj_t *spacer = lv_obj_create(header);
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spacer, 0, 0);
  lv_obj_set_flex_grow(spacer, 1);
  s_hi_clock_lbl = new_label(header, "--:--", &lv_font_montserrat_14, C_MUTED);

  /* chart panel: 7 bottom-aligned bars + day labels */
  lv_obj_t *chart = new_panel(s_hi_scr, UI_W - 20, HI_CHART_H + 22, C_PANEL);
  lv_obj_set_pos(chart, 10, 32);
  lv_obj_set_style_pad_all(chart, 4, 0);
  for (int i = 0; i < UI_HISTORY_DAYS; i++) {
    lv_obj_t *col = lv_obj_create(chart);
    lv_obj_set_size(col, 44, HI_CHART_H + 14);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 0, 0);
    lv_obj_set_x(col, (int32_t)(6 + i * 50));
    lv_obj_set_y(col, 2);
    lv_obj_t *bar = lv_obj_create(col);
    lv_obj_set_size(bar, 30, 4);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    s_hi_bars[i] = bar;
    lv_obj_t *lbl = new_label(col, "--", &lv_font_montserrat_12, C_MUTED);
    s_hi_day_lbls[i] = lbl;
  }

  /* per-node totals strip */
  s_hi_nodes_lbl = new_label(s_hi_scr, "", &lv_font_montserrat_12, C_TEXT);
  lv_obj_set_pos(s_hi_nodes_lbl, 10, 32 + HI_CHART_H + 26);

  /* event list rows */
  for (int i = 0; i < UI_HISTORY_ROWS; i++) {
    lv_obj_t *row = lv_obj_create(s_hi_scr);
    lv_obj_set_size(row, UI_W - 20, 15);
    lv_obj_set_pos(row, 10, (int32_t)(32 + HI_CHART_H + 42 + i * 15));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_t *l = new_label(row, "", &lv_font_montserrat_12, C_TEXT);
    (void)l;
    s_hi_ev_rows[i] = row;
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
  }

  /* footer */
  s_hi_footer_lbl = new_label(s_hi_scr, "", &lv_font_montserrat_12, C_MUTED);
  lv_obj_set_pos(s_hi_footer_lbl, 10, UI_H - 18);
}

static void update_history(const HistoryView &h) {
  char buf[72];
  if (s_hi_clock_lbl) lv_label_set_text(s_hi_clock_lbl, h.clock);

  uint32_t max_g = 1;
  for (int i = 0; i < UI_HISTORY_DAYS; i++)
    if (h.days[i].grams > max_g) max_g = h.days[i].grams;
  for (int i = 0; i < UI_HISTORY_DAYS; i++) {
    const uint32_t g = h.days[i].grams;
    lv_obj_t *bar = s_hi_bars[i];
    if (bar) {
      const int32_t hpx = (int32_t)(g ? 4 + (HI_CHART_H - 8) * g / max_g : 4);
      lv_obj_set_height(bar, hpx);
    }
    if (s_hi_day_lbls[i]) {
      snprintf(buf, sizeof(buf), "%s", h.day_labels[i]);
      lv_label_set_text((lv_obj_t *)s_hi_day_lbls[i], buf);
    }
  }

  if (s_hi_nodes_lbl) {
    size_t off = 0;
    buf[0] = 0;
    for (int i = 0; i < h.node_count && off < sizeof(buf) - 12; i++) {
      off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%sF%u %lu g",
                              i ? "  " : "", (unsigned)h.nodes[i].id,
                              (unsigned long)h.nodes[i].grams);
    }
    if (!h.node_count) snprintf(buf, sizeof(buf), "no feeders yet");
    lv_label_set_text(s_hi_nodes_lbl, buf);
  }

  for (int i = 0; i < UI_HISTORY_ROWS; i++) {
    lv_obj_t *row = s_hi_ev_rows[i];
    if (!row) continue;
    if (i >= h.event_count) {
      lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
    const HistoryEventRow &ev = h.events[i];
    char age[24];
    fmt_age(age, sizeof(age), ev.age_s);
    snprintf(buf, sizeof(buf), "F%u  %s  %u g  %s", (unsigned)ev.node,
             kind_short(ev.kind), (unsigned)ev.amount, age);
    lv_obj_t *l = lv_obj_get_child(row, 0);
    if (l) {
      lv_label_set_text((lv_obj_t *)l, buf);
      lv_obj_set_style_text_color(
          (lv_obj_t *)l, lv_color_hex(ev.kind == EV_FEED_OK ? C_TEXT : C_ALERT), 0);
    }
  }

  if (s_hi_footer_lbl) {
    snprintf(buf, sizeof(buf), "7-day: %lu feeds, %lu g - RAM ring 128 (SD deep history M4)",
             (unsigned long)h.total_feeds, (unsigned long)h.total_grams);
    lv_label_set_text(s_hi_footer_lbl, buf);
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
  build_detail_screen();
  build_edit_screen();
  build_history_screen();
  lv_screen_load(s_scr);

  const uint32_t boot_ms = (uint32_t)(esp_timer_get_time() / 1000);
  uint32_t last_tick = boot_ms;
  printf("ui: LVGL %d.%d.%d ready (%dx%d shot framebuffer)\n", LVGL_VERSION_MAJOR,
         LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, UI_W, UI_H);

  /* deterministic rotation for the QEMU screenshot run: each screen gets a
   * 25 s window, then the cycle repeats (the runner grabs one shot per
   * screen name). On hardware this is replaced by touch navigation (M4b). */
  enum ShotScreen { SH_DASH = 0, SH_DETAIL = 1, SH_EDIT = 2, SH_HIST = 3, SH_COUNT };
  constexpr uint32_t SHOT_WINDOW_MS = 25000;
  ShotScreen shot = SH_DASH;
  ShotScreen loaded = SH_DASH;

  for (;;) {
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    lv_tick_inc(now - last_tick);
    last_tick = now;

    shot = (ShotScreen)((now - boot_ms) / SHOT_WINDOW_MS % SH_COUNT);
    if (shot != loaded) {
      lv_screen_load(shot == SH_DASH    ? s_scr
                     : shot == SH_DETAIL  ? s_det_scr
                     : shot == SH_EDIT    ? s_ed_scr
                                          : s_hi_scr);
      loaded = shot;
      s_last_marker_ms = now - 3000; /* next marker 2 s after the switch */
    }

    const uint32_t s = s_seq;
    if (s != s_seen) {
      update_dashboard(s_snap[(s - 1) & 1]);
      s_seen = s;
    }
    const uint32_t ds = s_det_seq;
    if (ds != s_det_seen) {
      update_detail(s_det_snap[(ds - 1) & 1]);
      s_det_seen = ds;
    }
    const uint32_t es = s_edit_seq;
    if (es != s_edit_seen) {
      update_edit(s_edit_snap[(es - 1) & 1]);
      s_edit_seen = es;
    }
    const uint32_t hs = s_hist_seq;
    if (hs != s_hist_seen) {
      update_history(s_hist_snap[(hs - 1) & 1]);
      s_hist_seen = hs;
    }

    lv_timer_handler();

    /* screenshot markers once the first render has settled (runner dumps
     * g_shot_fb after each marker; repeats every 5 s for reliability) */
    if (now - boot_ms > 6000 && s_last_flush_ms > 0 &&
        now - s_last_flush_ms > 400 && now - s_last_marker_ms > 5000) {
      printf("\nAIO_SHOT %s\n", loaded == SH_DASH   ? "dashboard"
                                : loaded == SH_DETAIL ? "node_detail"
                                : loaded == SH_EDIT   ? "sched_editor"
                                                      : "history");
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
void publish_detail(const NodeDetail &) {}
void publish_sched_editor(const SchedEditor &) {}
void publish_history(const HistoryView &) {}
uint8_t detail_node() { return 1; }
void set_detail_node(uint8_t) {}

#endif /* CONFIG_AIO_UI_SHOT */

}  // namespace ui
}  // namespace aio
