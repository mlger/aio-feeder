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

/* Which node the detail screen shows (dashboard tap target later; the QEMU
 * shot build pins it). */
uint8_t detail_node();
void set_detail_node(uint8_t node_id);

}  // namespace ui
}  // namespace aio

#endif /* AIO_UI_H */
