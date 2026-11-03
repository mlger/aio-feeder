/*
 * AIO Feeder — remote glue: web dashboard + MQTT bridge on ESP-IDF
 * (PLAN.md §6.7, FR2/FR20; M6). Pure logic lives in hub/src/remote.h.
 *
 * All engine state is touched under the main loop's g_state_mu (passed in
 * the ctx); MQTT publishes only enqueue (thread-safe, non-blocking), so the
 * EventLog hook and the loop-task tick never stall feeding.
 *
 * QEMU (CONFIG_AIO_NO_RADIO): the whole networking half compiles out; the
 * exported functions stay as no-op stubs so main.cpp is unchanged there.
 */
#ifndef AIO_REMOTE_NET_H
#define AIO_REMOTE_NET_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../../link_core/link.h"
#include "hub_core.h"
#include "nodefw.h"
#include "remote.h"

struct RemoteNetCtx {
  aio::HubRegistry *reg;
  aio::EventLog *log;
  aio::HubCore *core;
  aio::HubSettings *set;       /* PIN verify source */
  aio::RemoteSettings *rset;   /* live remote settings */
  aio::IHubStore *store;       /* persist rset */
  aio::LinkEngine *eng;        /* peer freshness */
  aio::UnpairQueue *unpair;    /* pending unpairs → status JSON (FMEA #15) */
  aio::NodeFw *nfw;            /* M8b: node image meta + per-node fw identity */
  const char *fw;
  uint32_t (*ms_now)(void);
  int64_t (*unix_now)(void);
  SemaphoreHandle_t mu;        /* the main loop's g_state_mu */
};

/* One-time: store the ctx + register Wi-Fi/IP event handlers (which start
 * remote_net_apply on connect). Call after eng.begin(), radio branch only. */
void remote_net_init(RemoteNetCtx *ctx);

/* (Re)start web + MQTT per *rset. Called by the IP handler and after a
 * settings change. */
void remote_net_apply(void);

/* Poll from the aio loop task with g_state_mu held (cheap when idle): the
 * 30 s state/notify cadence + offline/low-stock transitions.
 * Applies (server restarts) are deferred: httpd_stop must never run while
 * the engine mutex is held — a request handler could be waiting on it.
 * remote_net_service() does the restarts; call it from the loop task AFTER
 * the mutex is released. */
void remote_net_poll(uint32_t now_ms, int64_t unix_now);
void remote_net_service(void);
void remote_net_request_apply(void); /* thread-safe flag set */

/* EventLog::on_push hook — publishes feed_result + notify rows. */
void remote_net_on_event(const aio::HubEvent &e);

/* Status strings for the Settings pane / CLI. */
const char *remote_net_ip(void);         /* dotted quad or "" */
const char *remote_net_mqtt_state(void); /* "off" / "down" / "up" */
bool remote_net_web_up(void);

#endif /* AIO_REMOTE_NET_H */
