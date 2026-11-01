/*
 * AIO Feeder — hub UI entry points (PLAN.md §7.2, §8; M4a).
 *
 * LVGL-free on purpose: main.cpp includes this in both builds. The view
 * implementation (ui.cpp) renders aio::ui::Dashboard / aio::ui::NodeDetail
 * snapshots; it never touches the registry or the link engine.
 */
#ifndef AIO_UI_H
#define AIO_UI_H

#include "../../hub/src/ui_model.h"

namespace aio {
namespace ui {

/* Create the UI task (LVGL init, dashboard screen, screenshot markers).
 * No-op unless CONFIG_AIO_UI_SHOT (QEMU build). */
void start();

/* Publish a fresh dashboard snapshot (call from the engine loop under the
 * state mutex; copied into a lock-free double buffer for the UI task). */
void publish(const Dashboard &d);

/* Publish the detail snapshot for the node chosen by set_detail_node. */
void publish_detail(const NodeDetail &d);

/* Publish the schedule-editor base snapshot for the same node. */
void publish_sched_editor(const SchedEditor &e);

/* Publish the history snapshot (7-day chart + per-node totals + events). */
void publish_history(const HistoryView &h);

/* Publish the alerts snapshot (§8.7 active + acknowledged). */
void publish_alerts(const AlertsView &v);

/* Publish the pairing snapshot (§8.5 — one per pairing step). */
void publish_pairing(const PairingView &p);

/* Publish the calibration wizard snapshot (§8.4 — one per wizard step). */
void publish_calib(const CalibView &c);

/* Publish the settings snapshot (§8.8 — section list + active pane). */
void publish_settings(const SettingsView &v);

/* Publish the setup-wizard snapshot (§8.9/§9.13 — one per step). */
void publish_wizard(const WizardView &v);

/* PIN pad (view-side state for the §8 lock screen; the settings flow on
 * hardware calls pin_verify with pin_entered() once 4 digits are in). */
void pin_press(char c); /* '0'..'9' digit, or 'C' to clear */
void pin_clear();
uint8_t pin_entered_len();
const char *pin_entered(); /* digits typed so far, NUL-terminated */
void pin_note(const char *note); /* status line: "wrong PIN", "unlocked" */

/* Which node the detail screen shows (dashboard tap target later; the QEMU
 * shot build pins it). */
uint8_t detail_node();
void set_detail_node(uint8_t node_id);

}  // namespace ui
}  // namespace aio

#endif /* AIO_UI_H */
