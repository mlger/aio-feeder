/*
 * AIO Feeder — LVGL dashboard view (PLAN.md §8; M4a scenes, M5 touch wiring).
 *
 * Renders aio::ui::Dashboard / NodeDetail / … snapshots (hub/src/ui_model.h)
 * at the logical 352×240 — hardware (display_hw.cpp) expands ×2 into the
 * 704×480 window of the 800×480 ST7262 panel; the QEMU shot build renders
 * into a static framebuffer sized to internal SRAM (QEMU has no PSRAM).
 *
 * M5 touch backend: the screens carry tap targets (cards, settings rows,
 * PIN keypad, session buttons) that set nav::Actions request flags; the
 * engine loop services them (hub/src/ui_nav.h) — taps never touch the
 * registry from the LVGL task. The hardware loop is driven by the GT911
 * indev; the shot build additionally offers inject_tap() so CI can drive a
 * real touch path end-to-end (hit-test → callback → request → engine).
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
 * redraws the whole screen, so the framebuffer always holds the complete
 * frame (9.3 removed lv_display_set_full_refresh; FULL mode is portable).
 */
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

#include "lvgl.h"

#include "ui.h"

#include "../../src/ui_nav.h"
#include "../display_hw.h" /* inline no-ops when CONFIG_AIO_HW_DISPLAY is off */

namespace aio {
namespace ui {

#define UI_W 352
#define UI_H 240
/* Logical resolution on BOTH builds (§7.1 display HAL): the hardware
 * expands ×2 into the 704×480 window of the 800×480 panel (48 px bars,
 * display_hw.cpp). 352 rather than the panel-proportional 400 because the
 * QEMU build has no PSRAM and every static byte lives in internal DRAM —
 * 400-wide overflowed dram0_0_seg by ~14 KB once the fifth screen landed
 * (see tools/qemu/README.md); 352 (2 × 170 px cards) funds the LVGL pool
 * the eleven screens need. */

#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT

/* QEMU pmemsave target — LVGL renders here (single full buffer, FULL mode). */
extern "C" uint8_t g_shot_fb[UI_W * UI_H * 2];
uint8_t g_shot_fb[UI_W * UI_H * 2];

#endif /* shot-only framebuffer */

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

/* node detail screen (§8.2) — schedule + recent-feeds lanes are single
 * multi-line labels (the 64 KiB LVGL pool is the hard budget; per-row
 * widgets blew it on the eighth screen) */
static lv_obj_t *s_det_scr = nullptr;
static lv_obj_t *s_det_clock_lbl = nullptr;
static lv_obj_t *s_det_title_lbl = nullptr;
static lv_obj_t *s_det_status_lbl = nullptr;
static lv_obj_t *s_det_next_lbl = nullptr;
static lv_obj_t *s_det_gate_lbl = nullptr;   /* hidden unless a gate is active */
static lv_obj_t *s_det_sched_lbl = nullptr;
static lv_obj_t *s_det_ev_lbl = nullptr;

/* schedule editor screen (§8.3) */
static lv_obj_t *s_ed_scr = nullptr;
static lv_obj_t *s_ed_title_lbl = nullptr;
static lv_obj_t *s_ed_clock_lbl = nullptr;
static lv_obj_t *s_ed_tz_lbl = nullptr;
static lv_obj_t *s_ed_list_lbl = nullptr;              /* entry list, one label */
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
static lv_obj_t *s_hi_ev_lbl = nullptr; /* event list, one label */
static lv_obj_t *s_hi_footer_lbl = nullptr;

/* alerts screen (§8.7) */
static lv_obj_t *s_alr_scr = nullptr;
static lv_obj_t *s_alr_clock_lbl = nullptr;
static lv_obj_t *s_alr_summary_lbl = nullptr;
static lv_obj_t *s_alr_rows[UI_ALERTS_MAX_ACTIVE];
static lv_obj_t *s_alr_acked_lbl = nullptr; /* acked list, one label */
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

#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT
static volatile uint32_t s_last_flush_ms = 0; /* marker pacing (shot only) */
static uint32_t s_last_marker_ms = 0;
#endif

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

  /* schedule + recent-feeds panels: one multi-line label each — the pool
   * budget forbids per-row widgets (see the statics note) */
  lv_obj_t *sched_panel = new_panel(col, UI_W - 20, 0, C_PANEL);
  lv_obj_set_height(sched_panel, LV_SIZE_CONTENT); /* grow with the label */
  lv_obj_set_style_pad_all(sched_panel, 6, 0);
  s_det_sched_lbl = new_label(sched_panel, "", &lv_font_montserrat_12, C_TEXT);
  lv_label_set_long_mode(s_det_sched_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_det_sched_lbl, UI_W - 44);

  lv_obj_t *ev_panel = new_panel(col, UI_W - 20, 0, C_PANEL);
  lv_obj_set_height(ev_panel, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(ev_panel, 6, 0);
  s_det_ev_lbl = new_label(ev_panel, "", &lv_font_montserrat_12, C_TEXT);
  lv_label_set_long_mode(s_det_ev_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_det_ev_lbl, UI_W - 44);

  /* footer actions (M5b: wired to the engine — Sched/Hist/Alerts navigate,
   * Feed/Skip/Hold are engine ops; the right 56 px stay free for the
   * back/home overlay) */
  lv_obj_t *actions = new_panel(s_det_scr, UI_W - 56, 26, C_PANEL);
  lv_obj_set_pos(actions, 0, UI_H - 26);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  const char *labels[6] = {"Sched", "Hist", "Alert", "Feed", "Skip", "Hold"};
  for (int i = 0; i < 6; i++) {
    lv_obj_t *btn = lv_btn_create(actions);
    lv_obj_set_size(btn, 44, 20);
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
    snprintf(buf, sizeof(buf), "next %02u:%02u - %u %s - %s",
             (unsigned)(d.next_min_of_day / 60), (unsigned)(d.next_min_of_day % 60),
             (unsigned)d.next_amount, d.unit_ml ? "ml" : "g", d.pushed);
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

  if (s_det_sched_lbl) {
    char lane[192];
    size_t off = 0;
    if (d.sched_enabled && d.entry_count) {
      off += (size_t)snprintf(lane + off, sizeof(lane) - off, "SCHEDULE\n");
      for (int i = 0; i < d.entry_count && off + 36 < sizeof(lane); i++) {
        const DetailSchedEntry &e = d.entries[i];
        char days[24];
        days_text(e.days_mask, days, sizeof(days));
        off += (size_t)snprintf(lane + off, sizeof(lane) - off,
                                "%02u:%02u  %-9s %u %s\n",
                                (unsigned)(e.min_of_day / 60),
                                (unsigned)(e.min_of_day % 60), days,
                                (unsigned)e.amount, d.unit_ml ? "ml" : "g");
      }
    } else {
      snprintf(lane, sizeof(lane), "SCHEDULE%s\n(no entries)",
               d.sched_enabled ? "" : " (paused)");
    }
    lv_label_set_text(s_det_sched_lbl, lane);
  }

  if (s_det_ev_lbl) {
    char lane[224];
    size_t off = 0;
    off += (size_t)snprintf(lane + off, sizeof(lane) - off, "RECENT FEEDS\n");
    if (!d.event_count) {
      off += (size_t)snprintf(lane + off, sizeof(lane) - off, "(none yet)\n");
    }
    for (int i = 0; i < d.event_count && off + 36 < sizeof(lane); i++) {
      const DetailEvent &ev = d.events[i];
      char age[24];
      fmt_age(age, sizeof(age), ev.age_s);
      off += (size_t)snprintf(lane + off, sizeof(lane) - off, "%s - %u %s - %s\n",
                              kind_short(ev.kind), (unsigned)ev.amount,
                              d.unit_ml ? "ml" : "g", age);
    }
    lv_label_set_text(s_det_ev_lbl, lane);
    lv_obj_set_style_text_color(s_det_ev_lbl, lv_color_hex(C_TEXT), 0);
  }
}

/* --- schedule editor screen (§8.3) -------------------------------------------------- */

/* M5b tap callbacks (defined in the touch-wiring section below) */
static void ed_draft_tap_cb(lv_event_t *e);
static void ed_day_tap_cb(lv_event_t *e);

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
  lv_obj_set_height(list, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(list, 4, 0);
  s_ed_list_lbl = new_label(list, "", &lv_font_montserrat_12, C_TEXT);
  lv_label_set_long_mode(s_ed_list_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_ed_list_lbl, UI_W - 40);

  lv_obj_t *edit = new_panel(col, UI_W - 20, 96, C_PANEL);
  lv_obj_set_style_pad_all(edit, 6, 0);
  lv_obj_set_flex_flow(edit, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(edit, 4, 0);

  lv_obj_t *eh =
      new_label(edit, "EDIT SELECTED - tap time / amount", &lv_font_montserrat_12, C_MUTED);
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
  /* M5b: the draft row is tappable — left half opens the time pad, right
   * half the amount pad (zones split at x=112 of the 320 px row) */
  lv_obj_add_flag(ta, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(ta, ed_draft_tap_cb, LV_EVENT_CLICKED, nullptr);

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
    /* M5b: weekday chips toggle the draft mask (user_data = wday) */
    lv_obj_add_event_cb(b, ed_day_tap_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)d);
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

  if (s_ed_list_lbl) {
    char lane[224];
    size_t off = 0;
    if (!e.found || !e.entry_count)
      snprintf(lane, sizeof(lane), "ENTRIES\n(none - add one below)");
    else {
      off += (size_t)snprintf(lane + off, sizeof(lane) - off, "ENTRIES\n");
      for (int i = 0; i < e.entry_count && off + 36 < sizeof(lane); i++) {
        const DetailSchedEntry &en = e.entries[i];
        char days[24];
        days_text(en.days_mask, days, sizeof(days));
        off += (size_t)snprintf(lane + off, sizeof(lane) - off,
                                "%s%02u:%02u  %-9s %u %s\n",
                                i == e.selected ? "> " : "   ",
                                (unsigned)(en.min_of_day / 60),
                                (unsigned)(en.min_of_day % 60), days,
                                (unsigned)en.amount, e.unit_ml ? "ml" : "g");
      }
    }
    lv_label_set_text(s_ed_list_lbl, lane);
  }

  if (s_ed_time_lbl) {
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(e.d_min_of_day / 60),
             (unsigned)(e.d_min_of_day % 60));
    lv_label_set_text(s_ed_time_lbl, buf);
  }
  if (s_ed_amt_lbl) {
    snprintf(buf, sizeof(buf), "%u %s", (unsigned)e.d_amount, e.unit_ml ? "ml" : "g");
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

  /* event list — one multi-line label (pool budget) */
  s_hi_ev_lbl = new_label(s_hi_scr, "", &lv_font_montserrat_12, C_TEXT);
  lv_label_set_long_mode(s_hi_ev_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_hi_ev_lbl, UI_W - 20);
  lv_obj_set_pos(s_hi_ev_lbl, 10, 32 + HI_CHART_H + 42);

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

  if (s_hi_ev_lbl) {
    char lane[256];
    size_t off = 0;
    if (!h.event_count)
      snprintf(lane, sizeof(lane), "no feeds yet");
    else
      /* lane budget: the list starts under the chart — 4 lines clear the
       * footer (the model keeps 8; the §8.6 list is a recent-events strip) */
      for (int i = 0; i < h.event_count && i < 4 && off + 36 < sizeof(lane); i++) {
        const HistoryEventRow &ev = h.events[i];
        char age[24];
        fmt_age(age, sizeof(age), ev.age_s);
        off += (size_t)snprintf(lane + off, sizeof(lane) - off, "F%u  %s  %u g  %s\n",
                                (unsigned)ev.node, kind_short(ev.kind),
                                (unsigned)ev.amount, age);
      }
    lv_label_set_text(s_hi_ev_lbl, lane);
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

  s_alr_acked_lbl = new_label(s_alr_scr, "", &lv_font_montserrat_12, C_MUTED);
  lv_label_set_long_mode(s_alr_acked_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_alr_acked_lbl, UI_W - 20);
  lv_obj_set_pos(s_alr_acked_lbl, 10, 156);

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

  if (s_alr_acked_lbl) {
    char lane[160];
    size_t off = 0;
    if (v.acked_count)
      off += (size_t)snprintf(lane + off, sizeof(lane) - off, "Acknowledged (%u)\n",
                              (unsigned)v.acked_count);
    else
      lane[0] = '\0';
    for (int i = 0; i < v.acked_count && off + 40 < sizeof(lane); i++) {
      alr_row_text(v.acked[i], buf, sizeof(buf));
      off += (size_t)snprintf(lane + off, sizeof(lane) - off, "%s\n", buf);
    }
    lv_label_set_text(s_alr_acked_lbl, lane);
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
                 "Name:  %s\nType:  %s (%s)\nFull:  %lu %s\nIn it: %lu %s\n\n%s\nTap a row to edit.",
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
                 "Weigh the food from run %u.\nHow much came out? (min 5 %s)\nOn-time was %lu ms.\n\nTap 'Weigh' to enter it.",
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
static lv_obj_t *s_pin_kb = nullptr; /* keypad lane (label for now) */

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

  /* 4×3 keypad on the right half — a styled multi-line LABEL, the same
   * pool discipline as the pairing/calib lanes: the 64 KiB LVGL pool is
   * the hard budget (the component ignores pool-size requests and a
   * btnmatrix drew 12 rounded cells whose mask allocations exhausted it
   * mid-draw). The M5 touch backend replaces this with real buttons. */
  lv_obj_t *kb = new_label(s_pin_scr,
                           "[ 1 ]  [ 2 ]  [ 3 ]\n\n"
                           "[ 4 ]  [ 5 ]  [ 6 ]\n\n"
                           "[ 7 ]  [ 8 ]  [ 9 ]\n\n"
                           "[ C ]  [ 0 ]  [ OK ]",
                           &lv_font_montserrat_14, C_TEXT);
  lv_obj_set_pos(kb, 178, 38);
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
      /* lane budget: the body lane starts at y=150 — 8 lines max. Show the
       * first 6 paired nodes + the "pair new" row; the rest surface via the
       * bench CLI (8-node registries are an edge case). */
      uint8_t shown = 0;
      for (int i = 0; i < v.node_count && shown < 6 && off + 24 < n; i++, shown++)
        off += (size_t)snprintf(buf + off, n - off, "%s F%u %s\n",
                                v.node_sel == (int8_t)v.node_ids[i] ? ">" : " ",
                                (unsigned)v.node_ids[i], v.node_names[i]);
      if (v.node_count > shown)
        off += (size_t)snprintf(buf + off, n - off, "   +%u more (bench CLI)\n",
                                (unsigned)(v.node_count - shown));
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
      /* lane budget: the body starts at y=150 — 5 lines max on a 240 px
       * frame (the nothing-until-save rule is the sched step's hint) */
      snprintf(buf, n, "Feeder %s is set.\n\nNext: run a calibration\nand tap Refilled when\nyou top up.",
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

/* --- M5 touch wiring: navigation, tap targets, engine requests ---------
 *
 * The pool discipline (see the statics note) forbids widget-per-button:
 * every tap target is a transparent CLICKABLE catcher over a fixed zone,
 * one per screen area at most. Affordance comes from the text lanes
 * themselves (the "[ 1 ] [ 2 ] …" keypad, the "> " section marker) and
 * from one static hint lane per session screen. Requests land in
 * nav::Actions; the engine loop services them (ui_nav.h) — the LVGL task
 * never touches the registry or the sessions. */

static nav::Nav s_nav;
static nav::Actions s_acts;

/* M5b entry pad (defined with the pad block below) */
static void pad_open_num(entry::Target t, uint32_t cur);
static void pad_open_text(entry::Target t, const char *cur);

/* engine → ui handshakes (single bytes, last-writer-wins; consumed once
 * per ui tick): pin verdict from PinLock, and the §8 lock mirror so a
 * 60 s expiry under the user bounces to the dashboard. */
static volatile int8_t s_cmd_pin = 0;  /* 0 none · 1 ok · -1 wrong */
static volatile int8_t s_cmd_lock = 0; /* 0 none · 1 locked · -1 unlocked */

/* tap injector (shot build drives flows through the real indev so CI
 * exercises hit-test → callback → request → engine end-to-end) */
static volatile int16_t s_tap_x = 0, s_tap_y = 0;
static volatile uint8_t s_tap_stage = 0; /* 0 idle · 1 press · 2 release */

void inject_tap(int x, int y) {
  s_tap_x = (int16_t)x;
  s_tap_y = (int16_t)y;
  s_tap_stage = 1;
}

#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT
static void injected_read_cb(lv_indev_t *, lv_indev_data_t *data) {
  switch (s_tap_stage) {
    case 1:
      data->point.x = s_tap_x;
      data->point.y = s_tap_y;
      data->state = LV_INDEV_STATE_PRESSED;
      s_tap_stage = 2;
      break;
    case 2:
      data->state = LV_INDEV_STATE_RELEASED;
      s_tap_stage = 0;
      break;
    default:
      data->state = LV_INDEV_STATE_RELEASED;
      break;
  }
}
#endif /* shot-only tap injector read cb */

static lv_obj_t *screen_of(nav::Screen s) {
  switch (s) {
    case nav::SC_DASH: return s_scr;
    case nav::SC_DETAIL: return s_det_scr;
    case nav::SC_EDIT: return s_ed_scr;
    case nav::SC_HIST: return s_hi_scr;
    case nav::SC_ALERTS: return s_alr_scr;
    case nav::SC_PAIR: return s_pair_scr;
    case nav::SC_CAL: return s_cal_scr;
    case nav::SC_SET: return s_set_scr;
    case nav::SC_PIN: return s_pin_scr;
    case nav::SC_WIZ: return s_wiz_scr;
    default: return nullptr;
  }
}

/* Load a screen and ask the engine for the fresh snapshot it needs. */
static void show(nav::Screen s) {
  lv_obj_t *scr = screen_of(s);
  if (!scr) return;
  lv_screen_load(scr);
  switch (s) {
    case nav::SC_DETAIL:
      set_detail_node(s_nav.node);
      s_acts.detail = true;
      s_acts.detail_node = s_nav.node;
      break;
    case nav::SC_EDIT:
      set_detail_node(s_nav.node);
      s_acts.editor = true;
      s_acts.editor_node = s_nav.node;
      break;
    case nav::SC_HIST: s_acts.history = true; break;
    case nav::SC_ALERTS: s_acts.alerts = true; break;
    case nav::SC_SET: s_acts.settings = true; break;
    default: break;
  }
}

void take_actions(nav::Actions &out) {
  out = s_acts;
  s_acts.clear();
}

void nav_sync_lock(bool locked) { s_cmd_lock = locked ? 1 : -1; }
void pin_gate_result(bool ok) { s_cmd_pin = ok ? 1 : -1; }

/* consume engine handshakes (ui task only) */
static void pump_nav_cmds() {
  const int8_t pin = s_cmd_pin;
  if (pin) {
    s_cmd_pin = 0;
    show(s_nav.pin_result(pin > 0));
  }
  const int8_t lk = s_cmd_lock;
  if (lk) {
    s_cmd_lock = 0;
    const nav::Screen before = s_nav.cur;
    s_nav.set_locked(lk > 0);
    if (s_nav.cur != before) show(s_nav.cur); /* bounced to the dashboard */
  }
}

/* --- tap target callbacks ------------------------------------------------ */

/* fixed-screen opener (user_data = screen enum) */
static void open_enc_cb(lv_event_t *e) {
  const auto s = (nav::Screen)(uintptr_t)lv_event_get_user_data(e);
  show(s_nav.open(s, 0));
}

/* dashboard card grid: 2 × 170 px cards, SPACE_EVENLY → columns at ~4 and
 * ~178; rows at 36 and 36+94 (build_screen geometry) */
static void card_tap_cb(lv_event_t *e) {
  (void)e;
  lv_point_t p{0, 0};
  if (const lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
  int idx = (p.x >= 176 ? 1 : 0) + (p.y >= 36 + 91 ? 2 : 0);
  if (idx < 0 || idx > 3) idx = 0;
  const uint8_t node = (uint8_t)(idx + 1);
  set_detail_node(node);
  show(s_nav.open(nav::SC_DETAIL, node));
}

/* settings section list: one 14 px line per row at (10,38) */
static void set_section_tap_cb(lv_event_t *e) {
  (void)e;
  lv_point_t p{0, 0};
  if (const lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
  int row = (p.y - 36) / 14;
  if (row < 0) row = 0;
  if (row > UI_SET_SECTION_COUNT - 1) row = UI_SET_SECTION_COUNT - 1;
  s_acts.set_section = (int8_t)row;
}

/* settings bottom action strip (hint lane: "Restart | Add node | Export |
 * Restore | Wizard") — five 70 px zones */
static void set_action_tap_cb(lv_event_t *e) {
  (void)e;
  lv_point_t p{0, 0};
  if (const lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
  /* zone cuts follow the hint-label metrics (Restart | Add node | Export |
   * Restore | Wizard from x=10) so a tap on a label lands its own zone */
  const int zone = p.x < 58 ? 0 : p.x < 125 ? 1 : p.x < 168 ? 2 : p.x < 232 ? 3 : 4;
  switch (zone) {
    case 0: s_acts.restart = true; break;
    case 1: s_acts.pair_open = true; show(s_nav.open(nav::SC_PAIR, 0)); break;
    case 2: s_acts.backup_export = true; break;
    case 3: s_acts.backup_restore = true; break;
    case 4: s_acts.wiz_open = true; show(s_nav.open(nav::SC_WIZ, 0)); break;
  }
}

/* PIN keypad: the "[ 1 ] …" label lane at (178,38) — 3 columns ~56 px
 * apart, 4 visual rows ~32 px apart (blank line between), 12 cells:
 * 1-9, C, 0, OK */
static void pin_key_tap_cb(lv_event_t *e) {
  (void)e;
  lv_point_t p{0, 0};
  if (const lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
  int col = (p.x - 172) / 56;
  int row = (p.y - 34) / 32;
  if (col < 0 || col > 2 || row < 0 || row > 3) return;
  const char cell = "123456789C0!"[row * 3 + col];
  if (cell == '!') s_acts.pin_submit = true;
  else pin_press(cell);
}

/* pairing: candidate rows (list lane, ≤4 lines at (10,38)) + bottom strip
 * "Cancel | Confirm | Commit | Close" (4 × 88 px) */
static void pair_select_tap_cb(lv_event_t *e) {
  (void)e;
  lv_point_t p{0, 0};
  if (const lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
  int row = (p.y - 36) / 14;
  if (row < 0 || row > 3) return;
  s_acts.pair_select = (int8_t)row;
}

static void pair_action_tap_cb(lv_event_t *e) {
  (void)e;
  lv_point_t p{0, 0};
  if (const lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
  const int zone = p.x < 60 ? 0 : p.x < 122 ? 1 : p.x < 190 ? 2 : 3;
  switch (zone) {
    case 0: s_acts.pair_cancel = true; break;
    case 1: s_acts.pair_confirm = true; break;
    case 2: s_acts.pair_commit = true; break;
    case 3: s_acts.pair_close = true; break;
  }
}

/* calibration: bottom strip "Close | Run test | Weigh | Save" (4 × 88 px;
 * step validity is guarded — Run only in RUNTIME, Weigh opens the grams pad
 * only in WEIGH, Save only with samples) */
static void cal_action_tap_cb(lv_event_t *e) {
  (void)e;
  lv_point_t p{0, 0};
  if (const lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
  const int zone = p.x < 50 ? 0 : p.x < 130 ? 1 : p.x < 180 ? 2 : 3;
  switch (zone) {
    case 0: s_acts.cal_close = true; break;
    /* Run only in the runtime step (mark_sent needs it; a stray Run in the
     * weigh step would send a test FEED the session never watches) */
    case 1:
      if (s_cal_snap[(s_cal_seq - 1) & 1].step == CAL_RUNTIME)
        s_acts.cal_run = true;
      break;
    case 2:
      if (s_cal_snap[(s_cal_seq - 1) & 1].step == CAL_WEIGH)
        pad_open_num(entry::ENT_CAL_WEIGH, 0);
      break;
    case 3: s_acts.cal_save = true; break;
  }
}

/* wizard: list lane (tz rows / wifi row / node rows / capacity / sched
 * fields — step-aware) + bottom strip "Back | Next | Save | Close" */
static void wiz_list_tap_cb(lv_event_t *e) {
  (void)e;
  const WizardView &v = s_wiz_snap[(s_wiz_seq - 1) & 1];
  lv_point_t p{0, 0};
  if (const lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
  const int row = (p.y - 38) / 14;
  if (row < 0 || row > 7) return;
  switch (v.step) {
    case WIZ_TZ:
      if (row < TZ_RULE_COUNT) s_acts.wiz_tz = (int8_t)row;
      else if (row == TZ_RULE_COUNT) s_acts.wiz_ntp = true; /* the NTP line */
      break;
    case WIZ_WIFI:
      if (row == 0) pad_open_text(entry::ENT_WIZ_SSID, v.wifi_ssid);
      break;
    case WIZ_NODE: {
      const uint8_t shown = v.node_count < 6 ? v.node_count : 6;
      if (row < (int)shown && row < v.node_count)
        s_acts.wiz_node = (int8_t)v.node_ids[row];
      else if (row == (v.node_count > shown ? shown + 1 : shown))
        s_acts.wiz_pair_new = true; /* the "Pair a new feeder" row */
      break;
    }
    case WIZ_CAP:
      if (row == 0) pad_open_num(entry::ENT_WIZ_TOTAL, v.cap_total);
      else if (row == 1) pad_open_num(entry::ENT_WIZ_NOW, v.cap_now);
      break;
    case WIZ_SCHED:
      if (row == 0) pad_open_num(entry::ENT_WIZ_TIME, v.s_min);
      else if (row == 1) s_acts.wiz_days = true;
      else if (row == 2) pad_open_num(entry::ENT_WIZ_AMOUNT, v.s_amount);
      break;
    default:
      break;
  }
}

static void wiz_action_tap_cb(lv_event_t *e) {
  (void)e;
  lv_point_t p{0, 0};
  if (const lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
  const int zone = p.x < 45 ? 0 : p.x < 85 ? 1 : p.x < 125 ? 2 : 3;
  const WizardView &v = s_wiz_snap[(s_wiz_seq - 1) & 1];
  switch (zone) {
    case 0: s_acts.wiz_back = true; break;
    case 1:
      /* "pair a new feeder" chosen: Next hands over to the pairing window;
       * the wizard resumes when the commit lands (on_paired) */
      if (v.step == WIZ_NODE && v.node_sel == -2) {
        s_acts.pair_open = true;
        show(s_nav.open(nav::SC_PAIR, 0));
      } else {
        s_acts.wiz_next = true;
      }
      break;
    case 2: s_acts.wiz_commit = true; break;
    case 3: s_acts.wiz_close = true; break;
  }
}

/* pairing form rows (M5b): the body label renders "Name/Type/Full/In it"
 * lines from y=110 — row 0 opens the name pad, 1 cycles the mech template,
 * 2/3 open the capacity pads */
static void pair_form_tap_cb(lv_event_t *e) {
  (void)e;
  const PairingView &v = s_pair_snap[(s_pair_seq - 1) & 1];
  if (v.step != PAIR_FORM) return;
  lv_point_t p{0, 0};
  if (const lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
  const int row = (p.y - 110) / 14;
  switch (row) {
    case 0: pad_open_text(entry::ENT_PAIR_NAME, v.name); break;
    case 1: s_acts.pair_mech = true; break;
    case 2: pad_open_num(entry::ENT_PAIR_TOTAL, v.cap_total); break;
    case 3: pad_open_num(entry::ENT_PAIR_NOW, v.cap_now); break;
    default: break;
  }
}

/* schedule editor: entry-list rows ("ENTRIES" header line at y=36, rows
 * from y=50, 14 px apart) — tap selects; below the rows drafts a new one */
static void ed_list_tap_cb(lv_event_t *e) {
  (void)e;
  const SchedEditor &ed = s_edit_snap[(s_edit_seq - 1) & 1];
  lv_point_t p{0, 0};
  if (const lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
  if (!ed.found) return;
  const int row = (p.y - 50) / 14;
  if (!ed.entry_count || row < 0 || row >= (int)ed.entry_count) {
    s_acts.ed_new = true; /* below the rows (or empty list) → draft a new row */
    return;
  }
  s_acts.ed_row = (int8_t)row;
}

/* schedule editor draft row: left half = time pad, right half = amount pad */
static void ed_draft_tap_cb(lv_event_t *e) {
  (void)e;
  const SchedEditor &ed = s_edit_snap[(s_edit_seq - 1) & 1];
  if (!ed.found) return;
  lv_point_t p{0, 0};
  if (const lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
  if (p.x < 112) pad_open_num(entry::ENT_EDIT_TIME, ed.d_min_of_day);
  else pad_open_num(entry::ENT_EDIT_AMOUNT, ed.d_amount);
}

/* schedule editor weekday chips (user_data = wday) */
static void ed_day_tap_cb(lv_event_t *e) {
  s_acts.ed_day = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
}

/* schedule editor bottom strip "New | Save | Delete | Pause" */
static void ed_action_tap_cb(lv_event_t *e) {
  (void)e;
  lv_point_t p{0, 0};
  if (const lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
  const int zone = p.x < 45 ? 0 : p.x < 90 ? 1 : p.x < 138 ? 2 : 3;
  switch (zone) {
    case 0: s_acts.ed_new = true; break;
    case 1: s_acts.ed_apply = true; break;
    case 2: s_acts.ed_del = true; break;
    case 3: s_acts.ed_toggle = true; break;
  }
}

/* detail footer: 6 zones across the 296 px action panel — Sched | Hist |
 * Alert | Feed | Skip | Hold (the right 56 px stay with back/home) */
static void det_footer_tap_cb(lv_event_t *e) {
  (void)e;
  lv_point_t p{0, 0};
  if (const lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
  const int zone = p.x < 50  ? 0
                   : p.x < 99  ? 1
                   : p.x < 148 ? 2
                   : p.x < 198 ? 3
                   : p.x < 247 ? 4
                               : 5;
  switch (zone) {
    case 0: show(s_nav.open(nav::SC_EDIT, detail_node())); break;
    case 1: show(s_nav.open(nav::SC_HIST, 0)); break;
    case 2: show(s_nav.open(nav::SC_ALERTS, 0)); break;
    case 3: s_acts.detail_feed = (int8_t)detail_node(); break;
    case 4: s_acts.detail_skip = true; break;
    case 5: s_acts.detail_hold = true; break;
  }
}

/* --- M5b shared entry pad (numeric + text) ------------------------------
 *
 * One modal on the top layer, built once. The QEMU pool forbids btnmatrix
 * keypads (the first PIN pad died mid-draw in the mask allocations — see
 * sdkconfig.qemu.defaults) and it also forbids betting on glyph metrics:
 * every key is its own clickable LABEL at a fixed (x, y) cell, so the hit
 * test never depends on text advance widths. The numeric keypad covers
 * all the numeric targets (capacities, HHMM time, amounts, the weighed
 * grams); text (feeder name, SSID) uses a char wheel — names are typed
 * once per feeder, so tap count beats pixel-perfect keyboards here. The
 * pad state lives in the pure entry::Pad model (hub/src/ui_entry.h); OK
 * submits one nav::Actions commit with the parsed value and the engine
 * maps it to session calls. */

static entry::Pad s_pad;
static lv_obj_t *s_pad_root = nullptr;   /* blocker + panel container */
static lv_obj_t *s_pad_title_lbl = nullptr;
static lv_obj_t *s_pad_value_lbl = nullptr;
static lv_obj_t *s_pad_num_box = nullptr;  /* numeric keypad container */
static lv_obj_t *s_pad_txt_box = nullptr;  /* char-wheel container */

static const char *pad_title(entry::Target t) {
  switch (t) {
    case entry::ENT_PAIR_NAME: return "Feeder name";
    case entry::ENT_PAIR_TOTAL: return "Full capacity";
    case entry::ENT_PAIR_NOW: return "In it now";
    case entry::ENT_WIZ_SSID: return "Wi-Fi network name";
    case entry::ENT_WIZ_TOTAL: return "Hopper size";
    case entry::ENT_WIZ_NOW: return "In it now";
    case entry::ENT_WIZ_TIME: return "Feed time (HHMM)";
    case entry::ENT_WIZ_AMOUNT: return "Feed amount";
    case entry::ENT_CAL_WEIGH: return "Weighed amount";
    case entry::ENT_EDIT_TIME: return "Feed time (HHMM)";
    case entry::ENT_EDIT_AMOUNT: return "Feed amount";
    default: return "";
  }
}

/* redraw the pad for the current buffer (show/hide + labels) */
static void build_entry_pad();

static void pad_refresh() {
  if (!s_pad_root) build_entry_pad(); /* lazy: first open pays the pool */
  if (!s_pad_root) return;            /* build failed (pool) — stay closed */
  if (!s_pad.is_open()) {
    lv_obj_add_flag(s_pad_root, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_remove_flag(s_pad_root, LV_OBJ_FLAG_HIDDEN);
  const bool txt = entry::kind_of(s_pad.tgt) == entry::K_TEXT;
  if (s_pad_num_box && s_pad_txt_box) {
    if (txt) {
      lv_obj_add_flag(s_pad_num_box, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(s_pad_txt_box, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_remove_flag(s_pad_num_box, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_pad_txt_box, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (s_pad_title_lbl) lv_label_set_text(s_pad_title_lbl, pad_title(s_pad.tgt));
  if (s_pad_value_lbl) {
    char pv[48];
    s_pad.preview(pv, sizeof(pv));
    lv_label_set_text(s_pad_value_lbl, pv[0] ? pv : " ");
  }
}

static void pad_open_num(entry::Target t, uint32_t cur) {
  char seed[12];
  seed[0] = '\0';
  switch (entry::kind_of(t)) {
    case entry::K_TIME:
      snprintf(seed, sizeof(seed), "%02lu%02lu", (unsigned long)(cur / 60),
               (unsigned long)(cur % 60));
      break;
    case entry::K_UINT:
      if (cur) snprintf(seed, sizeof(seed), "%lu", (unsigned long)cur);
      break;
    default: /* K_X100: fresh entry (you weigh, then type) */
      break;
  }
  s_pad.open(t, nullptr, seed);
  pad_refresh();
}

static void pad_open_text(entry::Target t, const char *cur) {
  s_pad.open(t, cur ? cur : "", nullptr);
  pad_refresh();
}

static void pad_close_view() {
  s_pad.close();
  pad_refresh();
}

/* OK submits the parsed value as one engine request; X just closes. */
static void pad_commit() {
  s_acts.entry_tgt = s_pad.tgt;
  s_acts.entry_text[0] = '\0';
  s_acts.entry_u32 = 0;
  if (entry::kind_of(s_pad.tgt) == entry::K_TEXT) {
    strncpy(s_acts.entry_text, s_pad.text, sizeof(s_acts.entry_text) - 1);
    s_acts.entry_text[sizeof(s_acts.entry_text) - 1] = '\0';
  } else if (entry::kind_of(s_pad.tgt) == entry::K_TIME) {
    s_acts.entry_u32 = s_pad.time_value();
  } else {
    s_acts.entry_u32 = s_pad.value();
  }
  s_acts.entry_commit = true;
  pad_close_view();
}

/* the wheel's charset (v steps back, ^ steps forward, wraps) */
static const char WHEEL_CHARS[] = "abcdefghijklmnopqrstuvwxyz0123456789 -._";

/* one key press; keys ride in user_data (see the builders below) */
static void pad_key_cb(lv_event_t *e) {
  const char k = (char)(uintptr_t)lv_event_get_user_data(e);
  switch (k) {
    case 'D': /* DEL */
      s_pad.backspace();
      pad_refresh();
      return;
    case 'C':
      s_pad.clear_all();
      pad_refresh();
      return;
    case 'O': /* OK */
      pad_commit();
      return;
    case 'X':
      pad_close_view();
      return;
    case '^': { /* wheel: append the next char (an empty name starts at 'a') */
      const size_t n = sizeof(WHEEL_CHARS) - 1;
      size_t i = (size_t)-1; /* -1 + 1 = 0 → 'a' */
      const size_t len = strlen(s_pad.text);
      if (len) {
        const char *at = strchr(WHEEL_CHARS, s_pad.text[len - 1]);
        if (at) i = (size_t)(at - WHEEL_CHARS);
      }
      s_pad.push_char(WHEEL_CHARS[(i + 1) % n]);
      pad_refresh();
      return;
    }
    case 'v': { /* wheel: append the previous char */
      const size_t n = sizeof(WHEEL_CHARS) - 1;
      size_t i = 0;
      const size_t len = strlen(s_pad.text);
      if (len) {
        const char *at = strchr(WHEEL_CHARS, s_pad.text[len - 1]);
        if (at) i = (size_t)(at - WHEEL_CHARS);
      }
      s_pad.push_char(WHEEL_CHARS[(i + n - 1) % n]);
      pad_refresh();
      return;
    }
    default:
      s_pad.push_char(k);
      pad_refresh();
      return;
  }
}

/* a tappable key label at a fixed cell (metric-free hit target) */
static lv_obj_t *new_key(lv_obj_t *parent, int x, int y, int w, int h,
                         const char *txt, char key) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_pos(l, (lv_coord_t)x, (lv_coord_t)y);
  lv_obj_set_size(l, (lv_coord_t)w, (lv_coord_t)h);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(l, pad_key_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)key);
  return l;
}

/* the modal pad itself — blocker + panel + two key groups, built once */
static void build_entry_pad() {
  s_pad_root = lv_obj_create(lv_layer_top());
  lv_obj_set_pos(s_pad_root, 0, 0);
  lv_obj_set_size(s_pad_root, UI_W, UI_H);
  lv_obj_set_style_bg_color(s_pad_root, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_pad_root, LV_OPA_60, 0);
  lv_obj_set_style_border_width(s_pad_root, 0, 0);
  lv_obj_set_style_radius(s_pad_root, 0, 0);
  lv_obj_set_style_pad_all(s_pad_root, 0, 0);
  lv_obj_remove_flag(s_pad_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_pad_root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_pad_root, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *panel = new_panel(s_pad_root, 300, 210, C_PANEL);
  if (!panel) { /* pool exhausted: run pad-less rather than crash */
    printf("ui: entry pad skipped (LVGL pool)\n");
    lv_obj_delete(s_pad_root);
    s_pad_root = nullptr;
    return;
  }
  lv_obj_set_pos(panel, 26, 20);

  s_pad_title_lbl = new_label(panel, "", &lv_font_montserrat_12, C_MUTED);
  lv_obj_set_pos(s_pad_title_lbl, 16, 6);
  s_pad_value_lbl = new_label(panel, "", &lv_font_montserrat_14, C_TEXT);
  lv_obj_set_pos(s_pad_value_lbl, 16, 24);

  /* numeric keypad: 4 x 4 cells of 64x26 (x0 16, dx 68; y0 48, dy 32) */
  s_pad_num_box = lv_obj_create(panel);
  lv_obj_set_pos(s_pad_num_box, 0, 0);
  lv_obj_set_size(s_pad_num_box, 300, 210);
  lv_obj_set_style_bg_opa(s_pad_num_box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_pad_num_box, 0, 0);
  lv_obj_set_style_pad_all(s_pad_num_box, 0, 0);
  lv_obj_remove_flag(s_pad_num_box, LV_OBJ_FLAG_SCROLLABLE);
  static const char *const NUM_KEYS[4][4] = {
      {"1", "2", "3", "DEL"},
      {"4", "5", "6", "C"},
      {"7", "8", "9", "."},
      {"0", "", "OK", ""},
  };
  static const char NUM_IDS[4][4] = {
      {'1', '2', '3', 'D'},
      {'4', '5', '6', 'C'},
      {'7', '8', '9', '.'},
      {'0', 0, 'O', 0},
  };
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      if (NUM_IDS[r][c])
        new_key(s_pad_num_box, 16 + c * 68, 48 + r * 32, 64, 26, NUM_KEYS[r][c],
                NUM_IDS[r][c]);
  lv_obj_add_flag(s_pad_num_box, LV_OBJ_FLAG_HIDDEN);

  /* char wheel for text targets: wide 92 px zones, typed once per feeder */
  s_pad_txt_box = lv_obj_create(panel);
  lv_obj_set_pos(s_pad_txt_box, 0, 0);
  lv_obj_set_size(s_pad_txt_box, 300, 210);
  lv_obj_set_style_bg_opa(s_pad_txt_box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_pad_txt_box, 0, 0);
  lv_obj_set_style_pad_all(s_pad_txt_box, 0, 0);
  lv_obj_remove_flag(s_pad_txt_box, LV_OBJ_FLAG_SCROLLABLE);
  new_key(s_pad_txt_box, 16, 62, 92, 30, "^", '^');
  new_key(s_pad_txt_box, 108, 62, 92, 30, "v", 'v');
  new_key(s_pad_txt_box, 200, 62, 92, 30, "sp", ' ');
  new_key(s_pad_txt_box, 16, 102, 92, 30, "DEL", 'D');
  new_key(s_pad_txt_box, 108, 102, 92, 30, "OK", 'O');
  new_key(s_pad_txt_box, 200, 102, 92, 30, "X", 'X');
  lv_obj_t *wh = new_label(s_pad_txt_box, "^ adds the next letter, v the previous\nsp adds a space, DEL erases the last one",
                           &lv_font_montserrat_12, C_MUTED);
  lv_obj_set_pos(wh, 16, 146);
  lv_obj_add_flag(s_pad_txt_box, LV_OBJ_FLAG_HIDDEN);
}

/* transparent clickable catcher (the pool-cheap tap target) */
static lv_obj_t *new_catcher(lv_obj_t *parent, int x, int y, int w, int h,
                             lv_event_cb_t cb, void *user_data) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_pos(o, (lv_coord_t)x, (lv_coord_t)y);
  lv_obj_set_size(o, (lv_coord_t)w, (lv_coord_t)h);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, user_data);
  return o;
}

static void nav_back_cb(lv_event_t *) { show(s_nav.back()); }
static void nav_home_cb(lv_event_t *) { show(s_nav.home()); }

/* persistent back/home zones, bottom-right corner of every screen */
static void build_nav_overlay() {
  lv_obj_t *layer = lv_layer_top();
  new_catcher(layer, UI_W - 52, UI_H - 26, 26, 26, nav_back_cb, nullptr);
  new_catcher(layer, UI_W - 24, UI_H - 26, 24, 26, nav_home_cb, nullptr);
}

static lv_obj_t *new_hint(lv_obj_t *parent, const char *txt) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), 0);
  lv_obj_set_pos(l, 10, UI_H - 22);
  return l;
}

/* attach the tap targets to the built screens (called once from ui_task,
 * after build_all_screens) */
static void build_tap_targets() {
  /* dashboard: card grid + gear zone top-right */
  new_catcher(s_scr, 0, 36, UI_W, UI_H - 36 - 24, card_tap_cb, nullptr);
  new_catcher(s_scr, UI_W - 38, 4, 34, 28, open_enc_cb,
              (void *)(uintptr_t)nav::SC_SET);

  /* detail: bottom strip, 6 zones (Sched | Hist | Alert | Feed | Skip |
   * Hold) across the 296 px action panel; back/home keep the right 56 px */
  new_catcher(s_det_scr, 0, UI_H - 26, UI_W - 56, 26, det_footer_tap_cb, nullptr);

  /* settings: section list + bottom action strip + hint lane */
  new_catcher(s_set_scr, 0, 34, 146, 150, set_section_tap_cb, nullptr);
  new_hint(s_set_scr, "Restart | Add node | Export | Restore | Wizard");
  new_catcher(s_set_scr, 0, UI_H - 26, UI_W, 26, set_action_tap_cb, nullptr);

  /* PIN pad */
  new_catcher(s_pin_scr, 170, 30, UI_W - 172, 174, pin_key_tap_cb, nullptr);

  /* pairing: candidate rows + form rows + bottom strip + hint lane */
  new_catcher(s_pair_scr, 0, 34, UI_W, 64, pair_select_tap_cb, nullptr);
  new_catcher(s_pair_scr, 0, 110, UI_W, 60, pair_form_tap_cb, nullptr);
  new_hint(s_pair_scr, "Cancel | Confirm | Commit | Close");
  new_catcher(s_pair_scr, 0, UI_H - 26, UI_W, 26, pair_action_tap_cb, nullptr);

  /* calibration: 4-zone strip + hint lane */
  new_hint(s_cal_scr, "Close | Run test | Weigh | Save");
  new_catcher(s_cal_scr, 0, UI_H - 26, UI_W, 26, cal_action_tap_cb, nullptr);

  /* wizard: list lane (tz / wifi / nodes / cap / sched, step-aware) +
   * bottom strip + hint lane */
  new_catcher(s_wiz_scr, 0, 34, UI_W, 108, wiz_list_tap_cb, nullptr);
  new_hint(s_wiz_scr, "Back | Next | Save | Close");
  new_catcher(s_wiz_scr, 0, UI_H - 26, UI_W, 26, wiz_action_tap_cb, nullptr);

  /* schedule editor (M5b): entry rows + draft row + day chips (their own
   * buttons) + bottom strip + hint lane */
  new_catcher(s_ed_scr, 0, 32, UI_W, 104, ed_list_tap_cb, nullptr);
  new_hint(s_ed_scr, "New | Save | Delete | Pause");
  new_catcher(s_ed_scr, 0, UI_H - 26, UI_W, 26, ed_action_tap_cb, nullptr);
}

/* --- LVGL plumbing ----------------------------------------------------------------- */

/* All screens, built once on the active display (both builds). */
static void build_all_screens() {
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
  /* the shared M5b entry modal builds lazily on first open (pad_refresh):
   * the QEMU pool is the binding constraint and the shot build never
   * opens the pad — boot keeps the v0.7.0 footprint */
}

/* pool pressure at boot (the 64 KiB fixed pool is the binding constraint in
 * the QEMU build — this line makes the serial log tell us where we are) */
static void log_pool() {
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  printf("ui: lv pool total=%u free=%u used=%u%% frag=%u%%\n",
         (unsigned)mon.total_size, (unsigned)mon.free_size,
         (unsigned)mon.used_pct, (unsigned)mon.frag_pct);
}

/* drain snapshot buffers into the widgets (shared by both loops) */
static void pump_snapshots(bool pin_screen) {
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
  if (pin_screen) update_pin_screen(); /* view-side pad state */
}

static void ui_tick(uint32_t now, uint32_t &last_tick) {
  lv_tick_inc(now - last_tick);
  last_tick = now;
  pump_nav_cmds();
}

#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT

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

  build_all_screens();
  build_tap_targets();
  build_nav_overlay();
  lv_screen_load(s_scr);

  /* the injected-tap indev: the tap slot drives the flow through the real
   * LVGL input path (hit-test -> callback -> request -> engine) */
  lv_indev_t *tap_indev = lv_indev_create();
  lv_indev_set_type(tap_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(tap_indev, injected_read_cb);
  lv_indev_set_display(tap_indev, s_disp);

  log_pool();

  const uint32_t boot_ms = (uint32_t)(esp_timer_get_time() / 1000);
  uint32_t last_tick = boot_ms;
  printf("ui: LVGL %d.%d.%d ready (%dx%d shot framebuffer)\n", LVGL_VERSION_MAJOR,
         LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, UI_W, UI_H);

  /* deterministic rotation for the QEMU screenshot run: each screen gets a
   * 25 s window, then the cycle repeats (the runner grabs one shot per
   * screen name). Slot drivers in main.cpp mirror the slot index (pairing =
   * 5, calib = 6, settings = 7, PIN = 8, wizard = 9, tap = 10; modulus
   * SH_COUNT). */
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
    SH_TAP = 10,  /* tap-path driver: the flow below, driven by inject_tap */
    SH_COUNT
  };
  constexpr uint32_t SHOT_WINDOW_MS = 25000;
  ShotScreen shot = SH_DASH;
  ShotScreen loaded = SH_DASH;

  /* tap-path driver state: closed loop — each tap fires only after the
   * previous one confirmed (nav state / settings section), and a marker
   * name is adopted only once its target frame is live, so the runner's
   * first-dump-per-name always shows the right screen. Pacing is relative
   * to the slot switch (slot_start_ms), not the boot. */
  const char *tap_shot = "tap_idle"; /* not in the runner's list */
  uint8_t tap_stage = 0;
  uint32_t slot_start_ms = 0;
  uint32_t tap_throttle = 0;
  bool tap_marked = false;      /* a marker just printed for tap_shot */
  uint32_t tap_marked_at = 0;
  /* section of the last published settings snapshot (255 = none yet) */
  auto tap_section = [&]() -> uint8_t {
    if (!s_set_seq) return 255;
    return s_set_snap[(s_set_seq - 1) & 1].section;
  };

  for (;;) {
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    ui_tick(now, last_tick);

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
                     : shot == SH_WIZ    ? s_wiz_scr
                                         : s_scr /* SH_TAP rides nav */);
      loaded = shot;
      s_last_marker_ms = now - 3000; /* next marker 2 s after the switch */
      slot_start_ms = now;
      /* resume an unfinished walk; restart only after it completed (the
       * 25 s slot can end mid-walk — the runner's timeout spans two
       * cycles) */
      if (shot == SH_TAP && tap_stage >= 9) {
        tap_stage = 0;
        tap_shot = "tap_idle";
      }
    }

    /* tap-path driver (slot 10): dash → detail → home → settings → BACKUP
     * → SECURITY, tapped through the real catchers. Stages advance on
     * CONFIRMATIONS (nav state / published settings section) and, between
     * panes, on marker beats: the next tap fires only after one marker
     * printed for the current name, so the runner's first-dump-per-name
     * always lands on the right frame (a 4.5 s hold raced the 5 s marker
     * cadence in the first run and both pane shots showed SECURITY).
     * main.cpp opens the §8 gate at slot start (the seeded PIN would
     * otherwise route the gear tap to the PIN pad). */
    if (loaded == SH_TAP) {
      const uint32_t ms = now - slot_start_ms;
      switch (tap_stage) {
        case 0: /* normalize to the dashboard, then card 1 */
          if (s_nav.cur == nav::SC_DASH) {
            if (ms >= 1500) { inject_tap(88, 80); tap_stage = 1; }
          } else if (now - tap_throttle >= 500) {
            tap_throttle = now;
            inject_tap(UI_W - 12, UI_H - 13); /* home first */
          }
          break;
        case 1:
          if (s_nav.cur == nav::SC_DETAIL) {
            tap_shot = "tap_detail"; tap_marked = false; /* no stale beats */
            tap_stage = 2;
          }
          break;
        case 2: /* one settled marker beat on detail (runner dumps at
                 * marker+0.8 s — act at +2 s), then home */
          if (tap_marked && now - tap_marked_at >= 2000) {
            tap_marked = false; inject_tap(UI_W - 12, UI_H - 13); tap_stage = 3;
          }
          break;
        case 3:
          if (s_nav.cur == nav::SC_DASH) { inject_tap(UI_W - 20, 18); tap_stage = 4; } /* gear */
          break;
        case 4:
          if (s_nav.cur == nav::SC_SET) {
            tap_shot = "tap_settings"; tap_marked = false;
            tap_stage = 5;
          }
          break;
        case 5: /* one settled marker beat on settings, then row 7 (BACKUP) */
          if (tap_marked && now - tap_marked_at >= 2000) {
            tap_marked = false; inject_tap(70, 142); tap_stage = 6;
          }
          break;
        case 6:
          if (tap_section() == SET_SEC_BACKUP) {
            tap_shot = "tap_backup"; tap_marked = false;
            tap_stage = 7;
          }
          break;
        case 7: /* one settled marker beat on BACKUP, then row 5 (SECURITY) */
          if (tap_marked && now - tap_marked_at >= 2000) {
            tap_marked = false; inject_tap(70, 114); tap_stage = 8;
          }
          break;
        case 8:
          if (tap_section() == SET_SEC_SECURITY) {
            tap_shot = "tap_security"; tap_marked = false;
            tap_stage = 9;
          }
          break;
        default: break;
      }
    }

    pump_snapshots(loaded == SH_PIN);

    lv_timer_handler();

    /* screenshot markers once the first render has settled (runner dumps
     * g_shot_fb after each marker; repeats every 5 s for reliability). The
     * pairing slot reports its current step so the runner grabs one PNG per
     * pairing phase (discover / code / form / done). */
    const uint32_t marker_min = (loaded == SH_TAP) ? 3000u : 5000u;
    if (now - boot_ms > 6000 && s_last_flush_ms > 0 &&
        now - s_last_flush_ms > 400 && now - s_last_marker_ms > marker_min) {
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
        case SH_TAP: shot_name = tap_shot; break;
        default: break;
      }
      printf("\nAIO_SHOT %s\n", shot_name);
      fflush(stdout);
      s_last_marker_ms = now;
      if (loaded == SH_TAP && strcmp(shot_name, tap_shot) == 0) {
        tap_marked = true;
        tap_marked_at = now;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void start() {
  xTaskCreate(ui_task, "aio-ui", 16384, nullptr, 4, nullptr);
}

#else  /* !CONFIG_AIO_UI_SHOT — hardware: touch-driven loop */

static void ui_task(void *) {
  lv_init();
  if (!display_hw::init()) {
    /* §3.3: feeding never depends on the UI — run headless */
    printf("ui: display bring-up failed, running headless\n");
    vTaskDelete(nullptr);
    return;
  }
  s_disp = display_hw::display();
  build_all_screens();
  build_tap_targets();
  build_nav_overlay();
  lv_screen_load(s_scr);
  log_pool();

  uint32_t last_tick = (uint32_t)(esp_timer_get_time() / 1000);
  printf("ui: LVGL %d.%d.%d ready (%dx%d logical on hardware)\n",
         LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, UI_W, UI_H);

  for (;;) {
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    ui_tick(now, last_tick);
    pump_snapshots(s_nav.cur == nav::SC_PIN);
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void start() {
  xTaskCreate(ui_task, "aio-ui", 16384, nullptr, 4, nullptr);
}

#endif /* CONFIG_AIO_UI_SHOT */

}  // namespace ui
}  // namespace aio
