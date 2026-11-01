/*
 * AIO Feeder — LVGL dashboard view (PLAN.md §8; M4a phase 1, QEMU shot mode).
 *
 * Renders aio::ui::Dashboard snapshots (hub/src/ui_model.h) into a static
 * 384×240 RGB565 framebuffer — same aspect family as the 800×480 panel,
 * sized to internal SRAM because QEMU has no PSRAM. On hardware the same
 * scene code renders at 800×480 once the RGB panel backend lands (M5).
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
 * LVGL 9 notes: single full-size buffer + RENDER_MODE_FULL — every flush
 * redraws the whole screen, so g_shot_fb always holds the complete frame
 * (9.3 removed lv_display_set_full_refresh; FULL mode is the portable way).
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

#define UI_W 352
#define UI_H 240
/* UI_W is 352, not the panel-proportional 400: the QEMU build has no PSRAM
 * and every static byte lives in internal DRAM — 400-wide overflowed
 * dram0_0_seg by ~14 KB once the fifth screen landed (see tools/qemu/
 * README.md). 384 fit the two-column card grid until v0.6's three extra
 * screens pushed .bss over again; 352 (2 × 170 px cards) frees ~15 KB,
 * which funds the 84 KB LVGL pool the ten screens need. Same scene code
 * renders at 800×480 on hardware. */

/* QEMU pmemsave target — LVGL renders here (single full buffer, FULL mode). */
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
static AlertsView s_alr_snap[2];
static volatile uint32_t s_seq = 0;
static volatile uint32_t s_det_seq = 0;
static volatile uint32_t s_edit_seq = 0;
static volatile uint32_t s_hist_seq = 0;
static volatile uint32_t s_alr_seq = 0;
static PairingView s_pair_snap[2];
static volatile uint32_t s_pair_seq = 0;
static uint32_t s_pair_seen = 0;
static CalibView s_cal_snap[2];
static volatile uint32_t s_cal_seq = 0;
static uint32_t s_cal_seen = 0;
static SettingsView s_set_snap[2];
static volatile uint32_t s_set_seq = 0;
static uint32_t s_set_seen = 0;
static WizardView s_wiz_snap[2];
static volatile uint32_t s_wiz_seq = 0;
static uint32_t s_wiz_seen = 0;
static char s_pin_note[32] = {0}; /* PIN pad status line (view-side) */
static uint32_t s_seen = 0;
static uint32_t s_det_seen = 0;
static uint32_t s_edit_seen = 0;
static uint32_t s_hist_seen = 0;
static uint32_t s_alr_seen = 0;
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

void publish_alerts(const AlertsView &v) {
  const uint32_t s = s_alr_seq;
  s_alr_snap[s & 1] = v;
  s_alr_seq = s + 1;
}

void publish_pairing(const PairingView &p) {
  const uint32_t s = s_pair_seq;
  s_pair_snap[s & 1] = p;
  s_pair_seq = s + 1;
}

void publish_calib(const CalibView &c) {
  const uint32_t s = s_cal_seq;
  s_cal_snap[s & 1] = c;
  s_cal_seq = s + 1;
}

void publish_settings(const SettingsView &v) {
  const uint32_t s = s_set_seq;
  s_set_snap[s & 1] = v;
  s_set_seq = s + 1;
}

void publish_wizard(const WizardView &v) {
  const uint32_t s = s_wiz_seq;
  s_wiz_snap[s & 1] = v;
  s_wiz_seq = s + 1;
}

/* --- PIN pad (§8 lock: 4 digits, view-side state) -------------------------------- */

static char s_pin[5] = {0};
static uint8_t s_pin_len = 0;

void pin_press(char c) {
  if (c == 'C') {
    pin_clear();
    return;
  }
  if (c < '0' || c > '9' || s_pin_len >= 4) return;
  s_pin[s_pin_len++] = c;
  s_pin[s_pin_len] = '\0';
}

void pin_clear() {
  s_pin_len = 0;
  s_pin[0] = '\0';
}

uint8_t pin_entered_len() { return s_pin_len; }
const char *pin_entered() { return s_pin; }

void pin_note(const char *note) {
  snprintf(s_pin_note, sizeof(s_pin_note), "%s", note ? note : "");
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

/* alerts screen (§8.7) */
static lv_obj_t *s_alr_scr = nullptr;
static lv_obj_t *s_alr_clock_lbl = nullptr;
static lv_obj_t *s_alr_summary_lbl = nullptr;
static lv_obj_t *s_alr_rows[UI_ALERTS_MAX_ACTIVE];
static lv_obj_t *s_alr_acked_hdr_lbl = nullptr;
static lv_obj_t *s_alr_acked_rows[UI_ALERTS_MAX_ACKED];
static lv_obj_t *s_alr_footer_lbl = nullptr;

/* pairing screen (§8.5, M4b) */
static lv_obj_t *s_pair_scr = nullptr;
static lv_obj_t *s_pair_head_lbl = nullptr; /* step title */
static lv_obj_t *s_pair_left_lbl = nullptr; /* window countdown */
static lv_obj_t *s_pair_cand_lbl = nullptr; /* discovery list, multi-line */
static lv_obj_t *s_pair_code_lbl = nullptr; /* big digits (CODE step) */
static lv_obj_t *s_pair_body_lbl = nullptr; /* multi-line body (all steps) */

/* calibration wizard screen (§8.4, M4b) */
static lv_obj_t *s_cal_scr = nullptr;
static lv_obj_t *s_cal_head_lbl = nullptr;  /* step title */
static lv_obj_t *s_cal_left_lbl = nullptr;  /* dispense countdown */
static lv_obj_t *s_cal_list_lbl = nullptr;  /* rate + runs summary */
static lv_obj_t *s_cal_body_lbl = nullptr;  /* per-step hint + note */

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
  c.root = new_panel(parent, 170, 88, C_CARD);
  lv_obj_set_style_radius(c.root, 10, 0);
  lv_obj_set_style_pad_all(c.root, 8, 0);
  lv_obj_set_flex_flow(c.root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(c.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(c.root, 4, 0);

  /* row 0: status dot + name */
  lv_obj_t *row = lv_obj_create(c.root);
  lv_obj_set_size(row, 154, 18);
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
    lv_obj_set_x(col, (int32_t)(6 + i * 46));
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

/* --- alerts screen (§8.7) ----------------------------------------------------------- */

static void alr_row_text(const AlertRow &r, char *buf, size_t n) {
  if (r.cond) {
    if (r.never_heard)
      snprintf(buf, n, "F%u  offline - no data this boot", (unsigned)r.node);
    else {
      char age[24];
      fmt_age(age, sizeof(age), r.age_s);
      snprintf(buf, n, "F%u  %s - last seen %s", (unsigned)r.node,
               r.state == NODE_OFFLINE ? "offline" : "stale", age);
    }
    return;
  }
  char age[24];
  fmt_age(age, sizeof(age), r.age_s);
  if (r.amount)
    snprintf(buf, n, "F%u  %s - %u g - %s", (unsigned)r.node, alert_text(r.kind),
             (unsigned)r.amount, age);
  else
    snprintf(buf, n, "F%u  %s - %s", (unsigned)r.node, alert_text(r.kind), age);
}

static uint32_t alr_row_color(const AlertRow &r, bool acked) {
  if (acked) return C_MUTED;
  if (r.cond) return r.state == NODE_OFFLINE ? C_ALERT : C_TEXT;
  return C_ALERT; /* FAIL / REJ / UNKNOWN / missed feed */
}

static void build_alerts_screen() {
  s_alr_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_alr_scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_pad_all(s_alr_scr, 0, 0);

  /* header */
  lv_obj_t *header = new_panel(s_alr_scr, UI_W, 30, C_PANEL);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(header, 10, 0);
  lv_obj_set_style_pad_right(header, 10, 0);
  lv_obj_set_style_pad_column(header, 8, 0);
  lv_obj_t *back = new_label(header, "<", &lv_font_montserrat_20, C_MUTED);
  (void)back;
  lv_obj_t *title = new_label(header, "Alerts", &lv_font_montserrat_16, C_TEXT);
  (void)title;
  lv_obj_t *spacer = lv_obj_create(header);
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spacer, 0, 0);
  lv_obj_set_flex_grow(spacer, 1);
  s_alr_clock_lbl = new_label(header, "--:--", &lv_font_montserrat_14, C_MUTED);

  s_alr_summary_lbl = new_label(s_alr_scr, "", &lv_font_montserrat_12, C_TEXT);
  lv_obj_set_pos(s_alr_summary_lbl, 10, 34);

  for (int i = 0; i < UI_ALERTS_MAX_ACTIVE; i++) {
    lv_obj_t *row = lv_obj_create(s_alr_scr);
    lv_obj_set_size(row, UI_W - 20, 15);
    lv_obj_set_pos(row, 10, (int32_t)(52 + i * 17));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_t *l = new_label(row, "", &lv_font_montserrat_12, C_ALERT);
    (void)l;
    s_alr_rows[i] = row;
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
  }

  s_alr_acked_hdr_lbl = new_label(s_alr_scr, "", &lv_font_montserrat_12, C_MUTED);
  lv_obj_set_pos(s_alr_acked_hdr_lbl, 10, 156);
  lv_obj_add_flag(s_alr_acked_hdr_lbl, LV_OBJ_FLAG_HIDDEN);

  for (int i = 0; i < UI_ALERTS_MAX_ACKED; i++) {
    lv_obj_t *row = lv_obj_create(s_alr_scr);
    lv_obj_set_size(row, UI_W - 20, 15);
    lv_obj_set_pos(row, 10, (int32_t)(172 + i * 15));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_t *l = new_label(row, "", &lv_font_montserrat_12, C_MUTED);
    (void)l;
    s_alr_acked_rows[i] = row;
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
  }

  s_alr_footer_lbl = new_label(s_alr_scr, "", &lv_font_montserrat_12, C_MUTED);
  lv_obj_set_pos(s_alr_footer_lbl, 10, UI_H - 18);
}

static void update_alerts(const AlertsView &v) {
  char buf[80];
  if (s_alr_clock_lbl) lv_label_set_text(s_alr_clock_lbl, v.clock);

  if (s_alr_summary_lbl) {
    if (!v.time_ok)
      snprintf(buf, sizeof(buf), "waiting for time sync...");
    else
      snprintf(buf, sizeof(buf), "%u of %u feeders online - %u active alert%s",
               (unsigned)v.online, (unsigned)v.total, (unsigned)v.active_count,
               v.active_count == 1 ? "" : "s");
    lv_label_set_text(s_alr_summary_lbl, buf);
  }

  for (int i = 0; i < UI_ALERTS_MAX_ACTIVE; i++) {
    lv_obj_t *row = s_alr_rows[i];
    if (!row) continue;
    if (i >= v.active_count) {
      lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
    const AlertRow &r = v.active[i];
    alr_row_text(r, buf, sizeof(buf));
    lv_obj_t *l = lv_obj_get_child(row, 0);
    if (l) {
      lv_label_set_text((lv_obj_t *)l, buf);
      lv_obj_set_style_text_color((lv_obj_t *)l, lv_color_hex(alr_row_color(r, false)), 0);
    }
  }

  if (s_alr_acked_hdr_lbl) {
    if (v.acked_count) {
      snprintf(buf, sizeof(buf), "Acknowledged (%u)", (unsigned)v.acked_count);
      lv_label_set_text(s_alr_acked_hdr_lbl, buf);
      lv_obj_remove_flag(s_alr_acked_hdr_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_alr_acked_hdr_lbl, LV_OBJ_FLAG_HIDDEN);
    }
  }
  for (int i = 0; i < UI_ALERTS_MAX_ACKED; i++) {
    lv_obj_t *row = s_alr_acked_rows[i];
    if (!row) continue;
    if (i >= v.acked_count) {
      lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
    const AlertRow &r = v.acked[i];
    alr_row_text(r, buf, sizeof(buf));
    lv_obj_t *l = lv_obj_get_child(row, 0);
    if (l) {
      lv_label_set_text((lv_obj_t *)l, buf);
      lv_obj_set_style_text_color((lv_obj_t *)l, lv_color_hex(alr_row_color(r, true)), 0);
    }
  }

  if (s_alr_footer_lbl) {
    snprintf(buf, sizeof(buf), "ack resets on reboot - SD history lands M4");
    lv_label_set_text(s_alr_footer_lbl, buf);
  }
}

/* --- pairing screen (§8.5) ----------------------------------------------------------- */

static const char *pair_mech_name(uint8_t mech_sel) {
  switch (mech_sel) {
    case 1: return "auger";
    case 2: return "pump";
    default: return "gate";
  }
}

static void build_pairing_screen() {
  s_pair_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_pair_scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_pad_all(s_pair_scr, 0, 0);

  /* header */
  lv_obj_t *header = new_panel(s_pair_scr, UI_W, 30, C_PANEL);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(header, 10, 0);
  lv_obj_set_style_pad_right(header, 10, 0);
  lv_obj_set_style_pad_column(header, 8, 0);
  lv_obj_t *back = new_label(header, "<", &lv_font_montserrat_20, C_MUTED);
  (void)back;
  s_pair_head_lbl = new_label(header, "Pairing", &lv_font_montserrat_16, C_TEXT);
  lv_obj_t *spacer = lv_obj_create(header);
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spacer, 0, 0);
  lv_obj_set_flex_grow(spacer, 1);
  s_pair_left_lbl = new_label(header, "", &lv_font_montserrat_14, C_MUTED);

  /* discovery list (DISCOVER step) — one multi-line label instead of one
   * widget per row: the QEMU build counts LVGL heap bytes (the first cut
   * with per-row objects exhausted the internal heap during render).
   * Fixed lanes so nothing overlaps: list 38..94 (≤4 lines), code at 52 is
   * only shown in the CODE step (list empty then), body always at 110. */
  s_pair_cand_lbl = new_label(s_pair_scr, "", &lv_font_montserrat_12, C_TEXT);
  lv_obj_set_pos(s_pair_cand_lbl, 10, 38);

  /* confirmation code (CODE step) */
  s_pair_code_lbl = new_label(s_pair_scr, "- - - -", &lv_font_montserrat_20, C_TEXT);
  lv_obj_set_pos(s_pair_code_lbl, 10, 52);
  lv_obj_add_flag(s_pair_code_lbl, LV_OBJ_FLAG_HIDDEN);

  /* body (all steps; the per-step hint is folded into the text) */
  s_pair_body_lbl = new_label(s_pair_scr, "", &lv_font_montserrat_12, C_TEXT);
  lv_label_set_long_mode(s_pair_body_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_pair_body_lbl, UI_W - 20);
  lv_obj_set_pos(s_pair_body_lbl, 10, 110);
}

static void update_pairing(const PairingView &v) {
  char buf[200];
  if (s_pair_head_lbl) {
    static const char *TITLES[] = {
        "Pairing", "Pairing - add a feeder", "Confirm the code", "New feeder", "Paired"};
    lv_label_set_text(s_pair_head_lbl, TITLES[v.step]);
  }
  if (s_pair_left_lbl) {
    if (v.step == PAIR_IDLE)
      lv_label_set_text(s_pair_left_lbl, "");
    else {
      snprintf(buf, sizeof(buf), "%us left", (unsigned)v.window_left_s);
      lv_label_set_text(s_pair_left_lbl, buf);
    }
  }

  /* discovery list (one label, one line per candidate) */
  if (s_pair_cand_lbl) {
    buf[0] = '\0';
    if (v.step == PAIR_DISCOVER) {
      size_t off = 0;
      for (int i = 0; i < v.cand_count && off + 40 < sizeof(buf); i++) {
        const PairCandidateRow &c = v.cand[i];
        off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                                "%02X:%02X:%02X:%02X:%02X:%02X fw %u.%u %d dBm%s\n",
                                c.mac[0], c.mac[1], c.mac[2], c.mac[3], c.mac[4],
                                c.mac[5], (unsigned)(c.fw_ver >> 8),
                                (unsigned)(c.fw_ver & 0xFF), (int)c.rssi,
                                c.selected ? "  <" : "");
      }
      if (!v.cand_count)
        snprintf(buf, sizeof(buf), "Listening...\n");
    }
    lv_label_set_text(s_pair_cand_lbl, buf);
  }

  /* code step */
  if (s_pair_code_lbl) {
    if (v.step == PAIR_CODE) {
      snprintf(buf, sizeof(buf), "%c  %c  %c  %c", v.code_digits[0], v.code_digits[1],
               v.code_digits[2], v.code_digits[3]);
      lv_label_set_text(s_pair_code_lbl, buf);
      lv_obj_remove_flag(s_pair_code_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_pair_code_lbl, LV_OBJ_FLAG_HIDDEN);
    }
  }

  /* body per step */
  if (s_pair_body_lbl) {
    buf[0] = '\0';
    switch (v.step) {
      case PAIR_IDLE:
        snprintf(buf, sizeof(buf),
                 "No pairing window open.\n\n\"Add node\" listens for\nunpaired feeders for 60 s.");
        break;
      case PAIR_DISCOVER:
        snprintf(buf, sizeof(buf),
                 "Hold the node's button at boot\nto announce it.");
        break;
      case PAIR_CODE:
        snprintf(buf, sizeof(buf),
                 "The feeder shows this code\n(serial/LED). Does it match?\n\nConfirm there to continue.");
        break;
      case PAIR_FORM:
        snprintf(buf, sizeof(buf),
                 "Name:  %s\nType:  %s (%s)\nFull:  %lu %s\nIn it: %lu %s\n\n%s",
                 v.name[0] ? v.name : "(default)", pair_mech_name(v.mech_sel),
                 pair_template_unit(v.mech_sel), (unsigned long)v.cap_total,
                 pair_template_unit(v.mech_sel), (unsigned long)v.cap_now,
                 pair_template_unit(v.mech_sel), v.summary);
        break;
      case PAIR_DONE:
        snprintf(buf, sizeof(buf),
                 "Feeder #%u added.\n\nThe key was sent wrapped with\nthe confirmed code - it never\ncrossed the air in the clear.",
                 (unsigned)v.new_node_id);
        break;
    }
    lv_label_set_text(s_pair_body_lbl, buf);
    /* amber headline colour while the level is low (§8 UX) */
    lv_obj_set_style_text_color(s_pair_body_lbl,
                                lv_color_hex(v.step == PAIR_FORM && v.amber ? C_ALERT : C_TEXT),
                                0);
  }
}

/* --- calibration wizard screen (§8.4, §9.3) ------------------------------------------- */

static void cal_rate_text(uint16_t rate_x100, uint8_t mech_sel, char *buf, size_t n) {
  /* ms per unit × 100 → "20.0 g/s" (§8.4: results show as g/s) */
  const uint32_t x10 = cal_g_per_s_x10(rate_x100);
  snprintf(buf, n, "%lu.%lu %s/s", (unsigned long)(x10 / 10),
           (unsigned long)(x10 % 10), cal_unit_label(mech_sel));
}

static void cal_list_text(const CalibView &v, char *buf, size_t n) {
  char now_rate[24], was[24];
  cal_rate_text(v.median_x100 ? v.median_x100 : v.old_rate_x100, v.mech_sel, now_rate,
                sizeof(now_rate));
  cal_rate_text(v.old_rate_x100, v.mech_sel, was, sizeof(was));
  /* lane budget: ≤ 6 lines (38..~140) — the body lane starts at 150 */
  size_t off = 0;
  off += (size_t)snprintf(buf + off, n - off, "F%u %s (%s)\n", (unsigned)v.node_id,
                          v.name[0] ? v.name : "?", pair_mech_name(v.mech_sel));
  if (v.median_x100)
    off += (size_t)snprintf(buf + off, n - off, "Median: %s (was %s)\n", now_rate, was);
  else
    off += (size_t)snprintf(buf + off, n - off, "Was: %s\n", was);
  if (v.step == CAL_RUNTIME || v.step == CAL_DISPENSE)
    off += (size_t)snprintf(buf + off, n - off, "Test: %lu s (max %lu) - about %lu %s\n",
                            (unsigned long)v.test_s, (unsigned long)v.max_test_s,
                            (unsigned long)v.test_grams, cal_unit_label(v.mech_sel));
  /* last 3 runs only — the lane caps at 6 lines */
  for (int i = v.sample_count > 3 ? v.sample_count - 3 : 0;
       i < v.sample_count && off + 28 < n; i++) {
    off += (size_t)snprintf(buf + off, n - off, "Run %d: %lu.%02lu %s in %lu ms\n",
                            i + 1, (unsigned long)(v.samples[i].grams_x100 / 100),
                            (unsigned long)(v.samples[i].grams_x100 % 100),
                            cal_unit_label(v.mech_sel),
                            (unsigned long)v.samples[i].on_ms);
  }
}

static void build_calib_screen() {
  s_cal_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_cal_scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_pad_all(s_cal_scr, 0, 0);

  /* header */
  lv_obj_t *header = new_panel(s_cal_scr, UI_W, 30, C_PANEL);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(header, 10, 0);
  lv_obj_set_style_pad_right(header, 10, 0);
  lv_obj_set_style_pad_column(header, 8, 0);
  lv_obj_t *back = new_label(header, "<", &lv_font_montserrat_20, C_MUTED);
  (void)back;
  s_cal_head_lbl = new_label(header, "Calibration", &lv_font_montserrat_16, C_TEXT);
  lv_obj_t *spacer = lv_obj_create(header);
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spacer, 0, 0);
  lv_obj_set_flex_grow(spacer, 1);
  s_cal_left_lbl = new_label(header, "", &lv_font_montserrat_14, C_MUTED);

  /* list lane (38..~140, ≤ 6 lines) and body lane (150..) — fixed like the
   * pairing screen: the QEMU build has no LVGL heap to spare for per-row
   * widgets, so both lanes are single multi-line labels */
  s_cal_list_lbl = new_label(s_cal_scr, "", &lv_font_montserrat_12, C_TEXT);
  lv_obj_set_pos(s_cal_list_lbl, 10, 38);

  s_cal_body_lbl = new_label(s_cal_scr, "", &lv_font_montserrat_12, C_TEXT);
  lv_label_set_long_mode(s_cal_body_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_cal_body_lbl, UI_W - 20);
  lv_obj_set_pos(s_cal_body_lbl, 10, 150);
}

static void update_calib(const CalibView &v) {
  char buf[240];
  if (s_cal_head_lbl) {
    static const char *TITLES[] = {"Calibration", "Calibrate a feeder",
                                   "Calibrating", "Weigh the run",
                                   "Calibrated"};
    lv_label_set_text(s_cal_head_lbl, TITLES[v.step]);
  }
  if (s_cal_left_lbl) {
    if (v.step == CAL_DISPENSE) {
      snprintf(buf, sizeof(buf), "%lus left", (unsigned long)v.left_s);
      lv_label_set_text(s_cal_left_lbl, buf);
    } else {
      lv_label_set_text(s_cal_left_lbl, "");
    }
  }

  if (s_cal_list_lbl) {
    if (v.step == CAL_IDLE)
      buf[0] = '\0';
    else
      cal_list_text(v, buf, sizeof(buf));
    lv_label_set_text(s_cal_list_lbl, buf);
  }

  if (s_cal_body_lbl) {
    buf[0] = '\0';
    switch (v.step) {
      case CAL_IDLE:
        snprintf(buf, sizeof(buf),
                 "Open from a feeder's page.\n\nA test run measures the flow\nrate so feeds match grams.");
        break;
      case CAL_RUNTIME:
        if (v.note[0]) {
          snprintf(buf, sizeof(buf), "%s\n\nRun again or save the median.", v.note);
        } else if (v.sample_count) {
          snprintf(buf, sizeof(buf),
                   "Keep the food that came out,\nweigh the next run, or save\nthe median now.");
        } else {
          snprintf(buf, sizeof(buf),
                   "Put a bowl under the gate and\nrun the test. Weigh what came\nout (min 5 g or more).");
        }
        break;
      case CAL_DISPENSE:
        snprintf(buf, sizeof(buf),
                 "Dispensing about %lu %s...\n\nThe node enforces its own caps\nwhile it runs.",
                 (unsigned long)v.test_grams, cal_unit_label(v.mech_sel));
        break;
      case CAL_WEIGH:
        snprintf(buf, sizeof(buf),
                 "Weigh the food from run %u.\nHow much came out? (min 5 %s)\nOn-time was %lu ms.",
                 (unsigned)(v.sample_count + 1), cal_unit_label(v.mech_sel),
                 (unsigned long)v.weigh_on_ms);
        break;
      case CAL_DONE:
        snprintf(buf, sizeof(buf),
                 "Feeder #%u calibrated, saved\nto the profile and pushed.\n\nRe-run when the food or the\nhopper changes.",
                 (unsigned)v.saved_node_id);
        break;
    }
    lv_label_set_text(s_cal_body_lbl, buf);
    /* amber while a note / rejection is showing (§8 UX) */
    lv_obj_set_style_text_color(s_cal_body_lbl,
                                lv_color_hex(v.note[0] && v.step == CAL_RUNTIME ? C_ALERT
                                                                                : C_TEXT),
                                0);
  }
}

/* --- settings screen (§8.8; M4b) ------------------------------------------------------ */

/* Section list (left lane) + active pane body (right lane), both single
 * multi-line labels — the QEMU build has no LVGL heap for per-row widgets
 * (see the pairing screen note). */
static const char *SET_SECTIONS[UI_SET_SECTION_COUNT] = {
    "Restart", "Display", "Time", "Network", "Remote",
    "Security", "Storage", "Backup", "Wizard", "Danger"};

static lv_obj_t *s_set_scr = nullptr;
static lv_obj_t *s_set_clock_lbl = nullptr;
static lv_obj_t *s_set_list_lbl = nullptr;
static lv_obj_t *s_set_body_lbl = nullptr;

static void build_settings_screen() {
  s_set_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_set_scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_pad_all(s_set_scr, 0, 0);

  lv_obj_t *header = new_panel(s_set_scr, UI_W, 30, C_PANEL);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(header, 10, 0);
  lv_obj_set_style_pad_right(header, 10, 0);
  lv_obj_set_style_pad_column(header, 8, 0);
  lv_obj_t *back = new_label(header, "<", &lv_font_montserrat_20, C_MUTED);
  (void)back;
  lv_obj_t *title = new_label(header, "Settings", &lv_font_montserrat_16, C_TEXT);
  (void)title;
  lv_obj_t *spacer = lv_obj_create(header);
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spacer, 0, 0);
  lv_obj_set_flex_grow(spacer, 1);
  s_set_clock_lbl = new_label(header, "--:--", &lv_font_montserrat_14, C_MUTED);

  s_set_list_lbl = new_label(s_set_scr, "", &lv_font_montserrat_12, C_TEXT);
  lv_obj_set_pos(s_set_list_lbl, 10, 38);

  s_set_body_lbl = new_label(s_set_scr, "", &lv_font_montserrat_12, C_TEXT);
  lv_label_set_long_mode(s_set_body_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_set_body_lbl, UI_W - 158);
  lv_obj_set_pos(s_set_body_lbl, 148, 38);
}

static void set_body_text(const SettingsView &v, char *buf, size_t n) {
  switch (v.section) {
    case SET_SEC_RESTART:
      snprintf(buf, n,
               "Restart hub\n\nBridges stay down and the\nschedule survives a restart.\nAsk for confirmation first.");
      break;
    case SET_SEC_DISPLAY:
      snprintf(buf, n, "Brightness: %u %%\nScreen off: %s\n\nDim first, then off;\nfirst touch only wakes.",
               (unsigned)v.brightness,
               v.screen_off_s ? "on (timeout)" : "never");
      break;
    case SET_SEC_TIME:
      snprintf(buf, n, "Zone: %s\nNTP: %s\nSource: %s\n\nShown on every screen.",
               v.tz_rule == TZ_MANILA   ? "Asia/Manila"
               : v.tz_rule == TZ_EU_CENTRAL ? "EU Central"
               : v.tz_rule == TZ_US_EASTERN ? "US Eastern"
                                            : "UTC",
               v.ntp_on ? "on" : "off", v.time_src);
      break;
    case SET_SEC_NETWORK:
      if (v.wifi_set)
        snprintf(buf, n, "Wi-Fi: %s\nESP-NOW ch: %u\n\nPassword stays on the hub\n(and out of backups).",
                 v.wifi_ssid, (unsigned)v.chan);
      else
        snprintf(buf, n, "Wi-Fi: not set\nESP-NOW ch: %u\n\nSet it in the wizard or\nwith 'set wifi'.",
                 (unsigned)v.chan);
      break;
    case SET_SEC_REMOTE:
      snprintf(buf, n,
               "Web dashboard + MQTT\nland with remote access\n(the next milestone).\n\nNotifications ride on MQTT.");
      break;
    case SET_SEC_SECURITY:
      if (v.pin_enabled)
        snprintf(buf, n, "PIN lock: ON\nScope: %s\n%s%s",
                 v.pin_scope == PIN_SCOPE_SETTINGS_FEEDS
                     ? "settings + feeds"
                     : "settings only",
                 v.locked_now ? "Locked - enter PIN." : "Unlocked now.",
                 v.unlock_left_s ? "" : "");
      else
        snprintf(buf, n,
                 "PIN lock: OFF\n\nA 4-digit code gates this\nscreen (and manual feeds\nif the scope says so).\n\nSchedules never lock.");
      break;
    case SET_SEC_STORAGE:
      snprintf(buf, n, "History kept: %u d\nCard: %s\n\nFeeding never waits for\nthe card.",
               (unsigned)v.retention_days, v.sd_status);
      break;
    case SET_SEC_BACKUP:
      if (v.backup_count)
        snprintf(buf, n, "Card: %s\nFiles: %u\nNewest: %s\n\nBack up now / restore /\nCSV export.",
                 v.sd_status, (unsigned)v.backup_count, v.backup_last);
      else
        snprintf(buf, n, "Card: %s\nFiles: 0\n\nBack up now / restore /\nCSV export.",
                 v.sd_status);
      break;
    case SET_SEC_WIZARD:
      snprintf(buf, n,
               "Run setup wizard\n\nSix steps: zone, Wi-Fi,\nfeeder, hopper, schedule,\ndone. Nothing is written\nuntil you save.");
      break;
    case SET_SEC_DANGER:
      snprintf(buf, n,
               "Factory reset\n\nWipes nodes, schedules,\nsettings and the PIN.\nType RESET to confirm.\n\nHistory stays on the card.");
      break;
    default:
      snprintf(buf, n, "Settings");
      break;
  }
}

static void update_settings(const SettingsView &v) {
  char buf[320];
  if (s_set_clock_lbl) lv_label_set_text(s_set_clock_lbl, v.clock);

  if (s_set_list_lbl) {
    size_t off = 0;
    buf[0] = '\0';
    for (int i = 0; i < UI_SET_SECTION_COUNT && off + 16 < sizeof(buf); i++) {
      off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%s %s\n",
                              i == (int)v.section ? ">" : " ", SET_SECTIONS[i]);
    }
    lv_label_set_text(s_set_list_lbl, buf);
  }
  if (s_set_body_lbl) {
    set_body_text(v, buf, sizeof(buf));
    lv_label_set_text(s_set_body_lbl, buf);
    lv_obj_set_style_text_color(
        s_set_body_lbl, lv_color_hex(v.section == SET_SEC_DANGER ? C_ALERT : C_TEXT), 0);
  }
}

/* --- PIN entry screen (§8 lock; M4b) -------------------------------------------------- */

static lv_obj_t *s_pin_scr = nullptr;
static lv_obj_t *s_pin_dots_lbl = nullptr;
static lv_obj_t *s_pin_note_lbl = nullptr;
static lv_obj_t *s_pin_scope_lbl = nullptr;
static lv_obj_t *s_pin_kb = nullptr; /* single btnmatrix keypad */

static void build_pin_screen() {
  s_pin_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_pin_scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_pad_all(s_pin_scr, 0, 0);

  lv_obj_t *header = new_panel(s_pin_scr, UI_W, 30, C_PANEL);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(header, 10, 0);
  lv_obj_set_style_pad_column(header, 8, 0);
  lv_obj_t *back = new_label(header, "<", &lv_font_montserrat_20, C_MUTED);
  (void)back;
  lv_obj_t *title = new_label(header, "Enter PIN", &lv_font_montserrat_16, C_TEXT);
  (void)title;

  s_pin_scope_lbl = new_label(s_pin_scr, "Settings are locked", &lv_font_montserrat_12,
                              C_MUTED);
  lv_obj_set_pos(s_pin_scope_lbl, 10, 38);

  s_pin_dots_lbl = new_label(s_pin_scr, "-  -  -  -", &lv_font_montserrat_20, C_TEXT);
  lv_obj_set_pos(s_pin_dots_lbl, 10, 58);

  s_pin_note_lbl = new_label(s_pin_scr, "", &lv_font_montserrat_12, C_ALERT);
  lv_obj_set_pos(s_pin_note_lbl, 10, 88);

  /* 4×3 keypad on the right half — ONE lv_btnmatrix widget: the QEMU build
   * counts both LVGL-pool and static-DRAM bytes, and 12 separate buttons
   * cost ~1.5 KB of pool for pixels the shot never touches. The M5 touch
   * backend wires the events; the shot build only renders it. */
  static const char *KEYS[13] = {"1", "2", "3", "4", "5", "6",
                                 "7", "8", "9", "C", "0", "OK", nullptr};
  lv_obj_t *kb = lv_btnmatrix_create(s_pin_scr);
  lv_btnmatrix_set_map(kb, KEYS);
  lv_obj_set_size(kb, 164, 148);
  lv_obj_set_pos(kb, 178, 38);
  lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(kb, 0, 0);
  lv_obj_set_style_pad_all(kb, 2, 0);
  lv_obj_set_style_bg_color(kb, lv_color_hex(C_CARD), LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb, lv_color_hex(C_TEXT), LV_PART_ITEMS);
  lv_obj_set_style_radius(kb, 6, LV_PART_ITEMS);
  s_pin_kb = kb;

  lv_obj_t *hint = new_label(s_pin_scr, "Unlocks for 60 s.\nSchedules never lock.",
                             &lv_font_montserrat_12, C_MUTED);
  lv_obj_set_pos(hint, 10, UI_H - 40);
}

static void update_pin_screen() {
  char buf[24];
  if (s_pin_dots_lbl) {
    if (s_pin_len == 0)
      snprintf(buf, sizeof(buf), "-  -  -  -");
    else {
      buf[0] = '\0';
      size_t off = 0;
      for (int i = 0; i < 4 && off + 4 < sizeof(buf); i++) {
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%s%c",
                                i ? "  " : "", i < (int)s_pin_len ? s_pin[i] : '-');
      }
    }
    lv_label_set_text(s_pin_dots_lbl, buf);
  }
  if (s_pin_note_lbl) {
    lv_label_set_text(s_pin_note_lbl, s_pin_note);
    lv_obj_set_style_text_color(
        s_pin_note_lbl,
        lv_color_hex(strncmp(s_pin_note, "unlocked", 8) == 0 ? C_ONLINE : C_ALERT), 0);
  }
}

/* --- setup wizard screen (§8.9, §9.13; M4b) ------------------------------------------- */

static lv_obj_t *s_wiz_scr = nullptr;
static lv_obj_t *s_wiz_step_lbl = nullptr; /* "Step 3 of 6" */
static lv_obj_t *s_wiz_head_lbl = nullptr;
static lv_obj_t *s_wiz_list_lbl = nullptr; /* choices / current values */
static lv_obj_t *s_wiz_body_lbl = nullptr; /* per-step hint + note */

static void build_wizard_screen() {
  s_wiz_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_wiz_scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_pad_all(s_wiz_scr, 0, 0);

  lv_obj_t *header = new_panel(s_wiz_scr, UI_W, 30, C_PANEL);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(header, 10, 0);
  lv_obj_set_style_pad_right(header, 10, 0);
  lv_obj_set_style_pad_column(header, 8, 0);
  lv_obj_t *back = new_label(header, "<", &lv_font_montserrat_20, C_MUTED);
  (void)back;
  s_wiz_head_lbl = new_label(header, "Setup", &lv_font_montserrat_16, C_TEXT);
  lv_obj_t *spacer = lv_obj_create(header);
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spacer, 0, 0);
  lv_obj_set_flex_grow(spacer, 1);
  s_wiz_step_lbl = new_label(header, "", &lv_font_montserrat_12, C_MUTED);

  s_wiz_list_lbl = new_label(s_wiz_scr, "", &lv_font_montserrat_12, C_TEXT);
  lv_obj_set_pos(s_wiz_list_lbl, 10, 38);

  s_wiz_body_lbl = new_label(s_wiz_scr, "", &lv_font_montserrat_12, C_TEXT);
  lv_label_set_long_mode(s_wiz_body_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_wiz_body_lbl, UI_W - 20);
  lv_obj_set_pos(s_wiz_body_lbl, 10, 150);
}

static void wiz_list_text(const WizardView &v, char *buf, size_t n) {
  size_t off = 0;
  switch (v.step) {
    case WIZ_TZ: {
      static const char *TZS[TZ_RULE_COUNT] = {"UTC", "Asia/Manila", "EU Central",
                                               "US Eastern"};
      for (int i = 0; i < TZ_RULE_COUNT; i++)
        off += (size_t)snprintf(buf + off, n - off, "%s %s\n",
                                i == (int)v.tz_rule ? ">" : " ", TZS[i]);
      off += (size_t)snprintf(buf + off, n - off, "NTP: %s\n", v.ntp_on ? "on" : "off");
      break;
    }
    case WIZ_WIFI:
      snprintf(buf, n, "Network: %s\nState: %s", v.wifi_ssid[0] ? v.wifi_ssid : "-",
               v.wifi_state == 1 ? v.wifi_note
                                 : (v.wifi_state == 0 ? v.wifi_note : "untested"));
      break;
    case WIZ_NODE: {
      for (int i = 0; i < v.node_count && off + 24 < n; i++)
        off += (size_t)snprintf(buf + off, n - off, "%s F%u %s\n",
                                v.node_sel == (int8_t)v.node_ids[i] ? ">" : " ",
                                (unsigned)v.node_ids[i], v.node_names[i]);
      off += (size_t)snprintf(buf + off, n - off, "%s Pair a new feeder\n",
                              v.node_sel == -2 ? ">" : " ");
      break;
    }
    case WIZ_CAP:
      snprintf(buf, n, "Full: %lu %s\nIn it now: %lu %s\n\n%s",
               (unsigned long)v.cap_total, v.unit_ml ? "ml" : "g",
               (unsigned long)v.cap_now, v.unit_ml ? "ml" : "g", v.summary);
      break;
    case WIZ_SCHED:
      snprintf(buf, n, "Time: %02u:%02u\nDays: %s\nAmount: %u %s (max %lu)",
               (unsigned)(v.s_min / 60), (unsigned)(v.s_min % 60),
               wiz_days_preset(v.s_days), (unsigned)v.s_amount,
               v.unit_ml ? "ml" : "g", (unsigned long)v.feed_cap);
      break;
    case WIZ_DONE:
      snprintf(buf, n, "Feeder: %s\nFirst feed set.", v.done_node_name);
      break;
    default:
      buf[0] = '\0';
      break;
  }
}

static void wiz_body_text(const WizardView &v, char *buf, size_t n) {
  switch (v.step) {
    case WIZ_IDLE:
      snprintf(buf, n, "Run from Settings or the\nempty Home screen.");
      break;
    case WIZ_TZ:
      snprintf(buf, n, "Pick the time zone.\nNTP needs Wi-Fi; the\nonboard RTC covers\npower cuts either way.");
      break;
    case WIZ_WIFI:
      snprintf(buf, n, "Name the network and test\nthe connection. The password\nnever leaves the hub.");
      break;
    case WIZ_NODE:
      snprintf(buf, n, "Use a paired feeder, or pair\na new one - the wizard comes\nback when it lands.");
      break;
    case WIZ_CAP:
      snprintf(buf, n, "How much fits in the\nhopper, and how much is\nin it now? Drives the\nstock estimate.");
      break;
    case WIZ_SCHED:
      snprintf(buf, n, "First schedule: time, days,\namount within the feeder's\nper-feed cap. Saved last.");
      break;
    case WIZ_DONE:
      snprintf(buf, n, "Feeder %s is set.\n\nNext: run a calibration\nand tap Refilled when\nyou top up. Nothing was\nsent to the node until\nyou saved.",
               v.done_node_name);
      break;
  }
  if (v.note[0])
    snprintf(buf + strlen(buf), n - strlen(buf), "\n\n%s", v.note);
}

static void update_wizard(const WizardView &v) {
  char buf[280];
  if (s_wiz_step_lbl) {
    if (v.step_no)
      snprintf(buf, sizeof(buf), "Step %u of 6%s", (unsigned)v.step_no,
               v.skip_cap ? " (hopper asked at pairing)" : "");
    else
      buf[0] = '\0';
    lv_label_set_text(s_wiz_step_lbl, buf);
  }
  if (s_wiz_head_lbl) {
    static const char *TITLES[] = {"Setup", "Time zone", "Wi-Fi", "Feeder",
                                   "Hopper", "First schedule", "Done"};
    lv_label_set_text(s_wiz_head_lbl, TITLES[v.step]);
  }
  if (s_wiz_list_lbl) {
    wiz_list_text(v, buf, sizeof(buf));
    lv_label_set_text(s_wiz_list_lbl, buf);
    lv_obj_set_style_text_color(
        s_wiz_list_lbl, lv_color_hex(v.step == WIZ_CAP && v.amber ? C_ALERT : C_TEXT), 0);
  }
  if (s_wiz_body_lbl) {
    wiz_body_text(v, buf, sizeof(buf));
    lv_label_set_text(s_wiz_body_lbl, buf);
    lv_obj_set_style_text_color(s_wiz_body_lbl,
                                lv_color_hex(v.note[0] ? C_ALERT : C_TEXT), 0);
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
                         LV_DISPLAY_RENDER_MODE_FULL);

  build_screen();
  build_detail_screen();
  build_edit_screen();
  build_history_screen();
  build_alerts_screen();
  build_pairing_screen();
  build_calib_screen();
  build_settings_screen();
  build_pin_screen();
  build_wizard_screen();
  lv_screen_load(s_scr);

  /* pool pressure at boot (the 7-screen QEMU build runs against the 64 KB
   * default ceiling — this line makes the serial log tell us where we are) */
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  printf("ui: lv pool total=%u free=%u used=%u%% frag=%u%%\n",
         (unsigned)mon.total_size, (unsigned)mon.free_size,
         (unsigned)mon.used_pct, (unsigned)mon.frag_pct);

  const uint32_t boot_ms = (uint32_t)(esp_timer_get_time() / 1000);
  uint32_t last_tick = boot_ms;
  printf("ui: LVGL %d.%d.%d ready (%dx%d shot framebuffer)\n", LVGL_VERSION_MAJOR,
         LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, UI_W, UI_H);

  /* deterministic rotation for the QEMU screenshot run: each screen gets a
   * 25 s window, then the cycle repeats (the runner grabs one shot per
   * screen name). On hardware this is replaced by touch navigation (M5).
   * Slot drivers in main.cpp mirror the slot index (pairing = 5, calib = 6,
   * modulus SH_COUNT). */
  enum ShotScreen {
    SH_DASH = 0,
    SH_DETAIL = 1,
    SH_EDIT = 2,
    SH_HIST = 3,
    SH_ALERTS = 4,
    SH_PAIR = 5,  /* slot index mirrored by the pairing driver in main.cpp */
    SH_CAL = 6,   /* slot index mirrored by the calibration driver in main.cpp */
    SH_SET = 7,   /* settings driver (main.cpp) */
    SH_PIN = 8,   /* PIN pad driver (main.cpp) */
    SH_WIZ = 9,   /* setup wizard driver (main.cpp) */
    SH_COUNT
  };
  constexpr uint32_t SHOT_WINDOW_MS = 25000;
  ShotScreen shot = SH_DASH;
  ShotScreen loaded = SH_DASH;

  for (;;) {
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    lv_tick_inc(now - last_tick);
    last_tick = now;

    shot = (ShotScreen)((now - boot_ms) / SHOT_WINDOW_MS % SH_COUNT);
    if (shot != loaded) {
      lv_screen_load(shot == SH_DASH ? s_scr
                     : shot == SH_DETAIL ? s_det_scr
                     : shot == SH_EDIT   ? s_ed_scr
                     : shot == SH_HIST   ? s_hi_scr
                     : shot == SH_ALERTS ? s_alr_scr
                     : shot == SH_PAIR   ? s_pair_scr
                     : shot == SH_CAL    ? s_cal_scr
                     : shot == SH_SET    ? s_set_scr
                     : shot == SH_PIN    ? s_pin_scr
                                         : s_wiz_scr);
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
    const uint32_t as = s_alr_seq;
    if (as != s_alr_seen) {
      update_alerts(s_alr_snap[(as - 1) & 1]);
      s_alr_seen = as;
    }
    const uint32_t ps = s_pair_seq;
    if (ps != s_pair_seen) {
      update_pairing(s_pair_snap[(ps - 1) & 1]);
      s_pair_seen = ps;
    }
    const uint32_t cs = s_cal_seq;
    if (cs != s_cal_seen) {
      update_calib(s_cal_snap[(cs - 1) & 1]);
      s_cal_seen = cs;
    }
    const uint32_t sts = s_set_seq;
    if (sts != s_set_seen) {
      update_settings(s_set_snap[(sts - 1) & 1]);
      s_set_seen = sts;
    }
    const uint32_t ws = s_wiz_seq;
    if (ws != s_wiz_seen) {
      update_wizard(s_wiz_snap[(ws - 1) & 1]);
      s_wiz_seen = ws;
    }
    if (loaded == SH_PIN) update_pin_screen(); /* view-side pad state */

    lv_timer_handler();

    /* screenshot markers once the first render has settled (runner dumps
     * g_shot_fb after each marker; repeats every 5 s for reliability). The
     * pairing slot reports its current step so the runner grabs one PNG per
     * pairing phase (discover / code / form / done). */
    if (now - boot_ms > 6000 && s_last_flush_ms > 0 &&
        now - s_last_flush_ms > 400 && now - s_last_marker_ms > 5000) {
      const char *shot_name = "alerts";
      switch (loaded) {
        case SH_DASH: shot_name = "dashboard"; break;
        case SH_DETAIL: shot_name = "node_detail"; break;
        case SH_EDIT: shot_name = "sched_editor"; break;
        case SH_HIST: shot_name = "history"; break;
        case SH_ALERTS: shot_name = "alerts"; break;
        case SH_PAIR: {
          const PairingView &pv = s_pair_snap[(s_pair_seq - 1) & 1];
          shot_name = pv.step == PAIR_CODE   ? "pairing_code"
                      : pv.step == PAIR_FORM ? "pairing_form"
                      : pv.step == PAIR_DONE ? "pairing_done"
                                             : "pairing";
          break;
        }
        case SH_CAL: {
          /* the driver in main.cpp paces the flow so WEIGH and DONE hold
           * across a marker beat; RUNTIME/DISPENSE share "calib" */
          const CalibView &cv = s_cal_snap[(s_cal_seq - 1) & 1];
          shot_name = cv.step == CAL_WEIGH ? "calib_weigh"
                      : cv.step == CAL_DONE ? "calib_done"
                                            : "calib";
          break;
        }
        case SH_SET: {
          const SettingsView &sv = s_set_snap[(s_set_seq - 1) & 1];
          shot_name = sv.section == SET_SEC_SECURITY ? "settings_security"
                      : sv.section == SET_SEC_BACKUP ? "settings_backup"
                                                     : "settings";
          break;
        }
        case SH_PIN: {
          const bool unlocked = strncmp(s_pin_note, "unlocked", 8) == 0;
          if (s_pin_len >= 4)
            shot_name = unlocked ? "pin_ok" : "pin_wrong";
          else
            shot_name = s_pin_len ? "pin_typing" : "pin_entry";
          break;
        }
        case SH_WIZ: {
          const WizardView &wv = s_wiz_snap[(s_wiz_seq - 1) & 1];
          shot_name = wv.step == WIZ_NODE  ? "wizard_node"
                      : wv.step == WIZ_CAP ? "wizard_cap"
                      : wv.step == WIZ_SCHED ? "wizard_sched"
                      : wv.step == WIZ_DONE ? "wizard_done"
                                            : "wizard";
          break;
        }
        default: break;
      }
      printf("\nAIO_SHOT %s\n", shot_name);
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
void publish_alerts(const AlertsView &) {}
void publish_pairing(const PairingView &) {}
void publish_calib(const CalibView &) {}
void publish_settings(const SettingsView &) {}
void publish_wizard(const WizardView &) {}
void pin_press(char) {}
void pin_clear() {}
uint8_t pin_entered_len() { return 0; }
const char *pin_entered() { return ""; }
void pin_note(const char *) {}
uint8_t detail_node() { return 1; }
void set_detail_node(uint8_t) {}

#endif /* CONFIG_AIO_UI_SHOT */

}  // namespace ui
}  // namespace aio
