/*
 * AIO Feeder — remote glue implementation (PLAN.md §6.7; M6).
 * See remote_net.h. esp_http_server + esp-mqtt, all engine access under the
 * caller-provided mutex. QEMU no-radio builds compile to the stub block at
 * the bottom.
 */
#include "remote_net.h"

#if !(defined(CONFIG_AIO_NO_RADIO) && CONFIG_AIO_NO_RADIO)

#include <esp_event.h>
#include <esp_http_server.h>
#include <mqtt_client.h>
#include <esp_netif.h>

#include <cstdlib>
#include <cstring>

#include "backup.h"
#include "remote_page.h"

static RemoteNetCtx s_ctx; /* zeroed: apply/poll/on_event no-op until init */
static bool s_inited = false;
static bool s_have_ip = false;
static bool s_apply_pending = false;
static httpd_handle_t s_http = nullptr;
static esp_mqtt_client_handle_t s_mqtt = nullptr;
static int s_mqtt_up = 0; /* 0 off, 1 down, 2 up */
static char s_ip[16] = "";
static aio::RemoteNodeLive s_live[aio::HUB_MAX_NODES];
/* seeded OFFLINE so the first tick never announces a boot-time transition */
static uint8_t s_prev_state[aio::HUB_MAX_NODES] = {2, 2, 2, 2, 2, 2, 2, 2};
static bool s_low_latch[aio::HUB_MAX_NODES];
static uint32_t s_last_pub_ms = 0;

/* --- state snapshot (caller holds the mutex) --------------------------------- */

static void rebuild_live() {
  for (uint8_t i = 0; i < aio::HUB_MAX_NODES; i++) {
    s_live[i] = aio::RemoteNodeLive{};
    const aio::HubNode *n = s_ctx.reg->node(i);
    if (!n || !n->id) continue;
    const aio::LinkPeer *p = s_ctx.eng->peer_by_node(n->id);
    const uint32_t now_ms = s_ctx.ms_now();
    s_live[i].state = aio::ui::state_of(now_ms, p ? p->last_rx_ms : 0);
    s_live[i].vin_mv = p ? p->last_hb.vin_mv : 0;
    s_live[i].hb_age_s = p && p->last_hb_ms
                             ? (now_ms - p->last_hb_ms) / 1000
                             : 0;
  }
}

static const char *notify_node_name(uint8_t node_id) {
  const aio::HubNode *n = s_ctx.reg->by_id(node_id);
  return n ? n->profile.name : "?";
}

/* --- publish helpers (caller holds the mutex; MQTT publish only enqueues) ---- */

static void mq_publish(const char *topic, const char *payload, int qos, bool retain) {
  if (s_mqtt && s_mqtt_up == 2)
    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, qos, retain ? 1 : 0);
}

static void mq_notify(const char *text, uint8_t node_id) {
  char pl[192];
  char esc[96];
  aio::json_escape(esc, sizeof(esc), 0, text);
  snprintf(pl, sizeof(pl), "{\"text\":\"%s\",\"node\":%u}", esc, (unsigned)node_id);
  mq_publish("aio/hub/notify", pl, 1, false);
  printf("notify: %s\n", text);
}

static void publish_node(uint8_t slot) {
  const aio::HubNode *n = s_ctx.reg->node(slot);
  if (!n || !n->id) return;
  char topic[40], pl[384];
  snprintf(topic, sizeof(topic), "aio/node/%u/state", (unsigned)n->id);
  if (aio::remote_node_state_json(*n, *s_ctx.log, s_live[slot],
                                  s_ctx.unix_now(), pl, sizeof(pl)))
    mq_publish(topic, pl, 1, true);
  snprintf(topic, sizeof(topic), "aio/node/%u/config", (unsigned)n->id);
  if (aio::remote_node_config_json(*n, pl, sizeof(pl)))
    mq_publish(topic, pl, 1, true);
}

static void publish_states() {
  rebuild_live();
  for (uint8_t i = 0; i < aio::HUB_MAX_NODES; i++)
    if (s_ctx.reg->node(i) && s_ctx.reg->node(i)->id) publish_node(i);
}

static void publish_discovery() {
  if (!s_ctx.rset->mqtt_disc) return;
  char topic[64], pl[512];
  for (uint8_t i = 0; i < aio::HUB_MAX_NODES; i++) {
    const aio::HubNode *n = s_ctx.reg->node(i);
    if (!n || !n->id) continue;
    for (uint8_t e = 0; aio::mqtt_discovery_entity(*n, e, topic, sizeof(topic), pl,
                                                   sizeof(pl)); e++)
      mq_publish(topic, pl, 1, true);
  }
}

/* --- engine mutex ---------------------------------------------------------------- */

/* Every dispatcher (httpd task, mqtt task) touches engine state under the
 * main loop's mutex, exactly like the REPL command bodies in main.cpp. */
struct SessLock {
  SessLock() { xSemaphoreTake(s_ctx.mu, portMAX_DELAY); }
  ~SessLock() { xSemaphoreGive(s_ctx.mu); }
};

/* MQTT cmd topic → manual feed (runs in the MQTT task, same feed path as
 * the REPL 'feed' command). */
static void mqtt_cmd_feed(uint8_t node, uint16_t amount) {
  if (!node) return;
  SessLock lock;
  const aio::HubNode *n = s_ctx.reg->by_id(node);
  if (!n) {
    mq_notify("feed refused: unknown node", node);
    return;
  }
  if (!s_ctx.core->request_manual(node, amount, s_ctx.ms_now(), s_ctx.unix_now()))
    mq_notify("feed refused: node busy", node);
}

static void mqtt_event(void *h, esp_event_base_t base, int32_t id, void *ed) {
  (void)h;
  (void)base;
  esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)ed;
  switch (id) {
    case MQTT_EVENT_CONNECTED: {
      s_mqtt_up = 2;
      esp_mqtt_client_subscribe(s_mqtt, "aio/node/+/cmd/feed", 1);
      mq_publish("aio/hub/status", "online", 1, true);
      SessLock lock; /* registry/log reads below race the loop task otherwise */
      publish_discovery();
      publish_states();
      break;
    }
    case MQTT_EVENT_DISCONNECTED:
      s_mqtt_up = 1;
      break;
    case MQTT_EVENT_DATA: {
      char tbuf[64] = "";
      if (ev->topic_len > 0 && (size_t)ev->topic_len < sizeof(tbuf)) {
        memcpy(tbuf, ev->topic, (size_t)ev->topic_len);
        tbuf[ev->topic_len] = '\0';
      }
      char body[128] = "";
      if (ev->data_len > 0 && (size_t)ev->data_len < sizeof(body)) {
        memcpy(body, ev->data, (size_t)ev->data_len);
        body[ev->data_len] = '\0';
      }
      uint8_t node = 0;
      uint16_t amount = 0;
      const uint8_t topic_node = aio::mqtt_feed_topic_parse(tbuf);
      if (topic_node && aio::remote_feed_parse(body, &node, &amount) &&
          node == topic_node)
        mqtt_cmd_feed(node, amount);
      else if (topic_node)
        mq_notify("feed command: bad payload", topic_node);
      break;
    }
    default:
      break;
  }
}

/* --- web server ----------------------------------------------------------------- */

static bool req_pin(httpd_req_t *req, char *out, size_t cap) {
  const size_t l = httpd_req_get_hdr_value_len(req, "X-AIO-Pin");
  if (l == 0 || l >= cap) return false;
  return httpd_req_get_hdr_value_str(req, "X-AIO-Pin", out, cap) == ESP_OK;
}

static bool req_body(httpd_req_t *req, char *out, size_t cap) {
  const int total = req->content_len;
  if (total <= 0 || (size_t)total >= cap) return false;
  int got = 0;
  while (got < total) {
    const int r = httpd_req_recv(req, out + got, (size_t)(total - got));
    if (r <= 0) return false;
    got += r;
  }
  out[got] = '\0';
  return true;
}

/* Auth gate for /api: 401 unless the settings allow open access. */
static bool api_auth(httpd_req_t *req) {
  char pin[8];
  req_pin(req, pin, sizeof(pin));
  if (aio::remote_auth_ok(*s_ctx.rset, *s_ctx.set, pin)) return true;
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"error\":\"pin\"}");
  return false;
}

static esp_err_t h_root(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, RMT_PAGE, (ssize_t)strlen(RMT_PAGE));
}

static esp_err_t h_status(httpd_req_t *req) {
  if (!api_auth(req)) return ESP_FAIL;
  char *buf = (char *)malloc(3072);
  if (!buf) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  {
    SessLock lock;
    rebuild_live();
    const aio::LocalTime now =
        aio::local_from_unix(s_ctx.unix_now(), s_ctx.reg->data().tz_rule);
    const size_t len = aio::remote_status_json(
        *s_ctx.reg, *s_ctx.log, *s_ctx.rset, s_mqtt_up == 2, s_live, s_ctx.fw, now,
        s_ctx.unix_now(), buf, 3072);
    httpd_resp_set_type(req, "application/json");
    if (!len)
      httpd_resp_send_500(req);
    else
      httpd_resp_send(req, buf, (ssize_t)len);
  }
  free(buf);
  return ESP_OK;
}

static esp_err_t h_feed(httpd_req_t *req) {
  if (!api_auth(req)) return ESP_FAIL;
  char body[256];
  uint8_t node = 0;
  uint16_t amount = 0;
  if (!req_body(req, body, sizeof(body)) ||
      !aio::remote_feed_parse(body, &node, &amount)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad body\"}");
    return ESP_FAIL;
  }
  SessLock lock;
  const aio::HubNode *n = s_ctx.reg->by_id(node);
  if (!n) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unknown node\"}");
    return ESP_FAIL;
  }
  const bool ok =
      s_ctx.core->request_manual(node, amount, s_ctx.ms_now(), s_ctx.unix_now());
  httpd_resp_set_type(req, "application/json");
  if (!ok) {
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"busy\"}");
    return ESP_FAIL;
  }
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t h_events(httpd_req_t *req) {
  if (!api_auth(req)) return ESP_FAIL;
  uint16_t limit = 0;
  char q[64];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
    char v[12] = "";
    if (httpd_query_key_value(q, "limit", v, sizeof(v)) == ESP_OK)
      limit = (uint16_t)atoi(v);
  }
  /* 4 KB response buffer: 24 rows × ~130 B — 0 ("all") clamps too */
  if (limit == 0 || limit > 24) limit = 24;
  char *buf = (char *)malloc(4096);
  if (!buf) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  SessLock lock;
  const size_t len = aio::remote_events_json(*s_ctx.reg, *s_ctx.log, limit, buf, 4096);
  httpd_resp_set_type(req, "application/json");
  esp_err_t res = ESP_OK;
  if (!len)
    res = httpd_resp_send_500(req);
  else
    res = httpd_resp_send(req, buf, (ssize_t)len);
  free(buf);
  return res;
}

static esp_err_t h_csv(httpd_req_t *req) {
  if (!api_auth(req)) return ESP_FAIL;
  char *buf = (char *)malloc(8192);
  if (!buf) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  SessLock lock;
  const size_t len =
      aio::remote_csv(*s_ctx.reg, *s_ctx.log, s_ctx.unix_now(), buf, 8192);
  esp_err_t res = ESP_OK;
  if (!len) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "clock not set");
  } else {
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=aio-history.csv");
    res = httpd_resp_send(req, buf, (ssize_t)len);
  }
  free(buf);
  return res;
}

static esp_err_t h_backup(httpd_req_t *req) {
  if (!api_auth(req)) return ESP_FAIL;
  /* buffer = BACKUP_JSON_MAX (8 KB) + slack for the escaping the writer does */
  char *buf = (char *)malloc(aio::BACKUP_JSON_MAX + 1024);
  if (!buf) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  SessLock lock;
  char name[aio::BACKUP_NAME_LEN];
  size_t len = 0;
  esp_err_t res = ESP_OK;
  if (aio::backup_json_write(*s_ctx.reg, *s_ctx.set, s_ctx.unix_now(), buf,
                             aio::BACKUP_JSON_MAX + 1024, name, &len)) {
    httpd_resp_set_type(req, "application/json");
    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=%s", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    res = httpd_resp_send(req, buf, (ssize_t)len);
  } else {
    httpd_resp_send_500(req);
    res = ESP_FAIL;
  }
  free(buf);
  return res;
}

static esp_err_t h_settings(httpd_req_t *req) {
  if (!api_auth(req)) return ESP_FAIL;
  char body[512];
  if (req->method != HTTP_POST || !req_body(req, body, sizeof(body))) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad body\"}");
    return ESP_FAIL;
  }
  {
    SessLock lock;
    if (!aio::remote_settings_patch(body, s_ctx.rset)) {
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad keys\"}");
      return ESP_FAIL;
    }
    remote_save(*s_ctx.store, *s_ctx.rset); /* durable before the apply */
    s_apply_pending = true; /* apply from remote_net_service — never mid-request */
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* --- lifecycle -------------------------------------------------------------------- */

static void http_stop() {
  if (!s_http) return;
  httpd_stop(s_http);
  s_http = nullptr;
}

static void mqtt_start() {
  if (!s_ctx.rset->mqtt_url[0]) return;
  char uri[110];
  if (!aio::remote_mqtt_uri(s_ctx.rset->mqtt_url, uri, sizeof(uri))) return;
  esp_mqtt_client_config_t cfg{};
  cfg.broker.address.uri = uri;
  if (s_ctx.rset->mqtt_user[0]) {
    cfg.credentials.username = s_ctx.rset->mqtt_user;
    cfg.credentials.authentication.password = s_ctx.rset->mqtt_pass;
  }
  /* §6.7 LWT: the broker flips hub/status to offline if the hub dies */
  cfg.session.last_will.topic = "aio/hub/status";
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  s_mqtt = esp_mqtt_client_init(&cfg);
  if (!s_mqtt) return;
  esp_mqtt_client_register_event(s_mqtt, (esp_event_handler_t)mqtt_event, nullptr);
  esp_mqtt_client_start(s_mqtt);
  s_mqtt_up = 1;
}

static void mqtt_stop() {
  if (!s_mqtt) return;
  esp_mqtt_client_stop(s_mqtt);
  esp_mqtt_client_destroy(s_mqtt);
  s_mqtt = nullptr;
  s_mqtt_up = 0;
}

static void ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (id == IP_EVENT_STA_GOT_IP) {
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (netif) {
      esp_netif_ip_info_t info{};
      if (esp_netif_get_ip_info(netif, &info) == ESP_OK)
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&info.ip));
    }
    s_have_ip = true;
    s_apply_pending = true; /* restart from remote_net_service, no locks held */
  } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
    s_have_ip = false;
    s_ip[0] = '\0';
    http_stop();
    mqtt_stop();
  }
}

void remote_net_init(RemoteNetCtx *ctx) {
  s_ctx = *ctx;
  s_inited = true;
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler, nullptr);
  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, ip_handler,
                             nullptr);
}

void remote_net_apply(void) {
  if (!s_inited) return;
  http_stop();
  mqtt_stop();
  if (s_have_ip) {
    if (s_ctx.rset->web_on) {
      httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
      cfg.stack_size = 6144;
      cfg.max_uri_handlers = 8;
      cfg.lru_purge_enable = true;
      if (httpd_start(&s_http, &cfg) == ESP_OK) {
        const httpd_uri_t uris[] = {
            {"/", HTTP_GET, h_root, nullptr},
            {"/api/status", HTTP_GET, h_status, nullptr},
            {"/api/feed", HTTP_POST, h_feed, nullptr},
            {"/api/events", HTTP_GET, h_events, nullptr},
            {"/api/events.csv", HTTP_GET, h_csv, nullptr},
            {"/api/backup", HTTP_GET, h_backup, nullptr},
            {"/api/settings", HTTP_POST, h_settings, nullptr},
        };
        for (const auto &u : uris) httpd_register_uri_handler(s_http, &u);
        printf("web dashboard on :%u%s\n", (unsigned)s_ctx.rset->web_port,
               s_ip[0] ? "" : " (ip pending)");
      } else {
        s_http = nullptr;
        printf("web dashboard start FAILED\n");
      }
    }
    if (s_ctx.rset->mqtt_on) mqtt_start();
  }
}

void remote_net_poll(uint32_t now_ms, int64_t unix_now) {
  if (!s_inited || s_apply_pending) return; /* service() owns the restart */
  if (now_ms - s_last_pub_ms < 30000) return;
  s_last_pub_ms = now_ms;
  if (s_mqtt_up != 2) return;
  publish_states();
  /* offline transitions + low stock (FR20) */
  char txt[96];
  for (uint8_t i = 0; i < aio::HUB_MAX_NODES; i++) {
    const aio::HubNode *n = s_ctx.reg->node(i);
    if (!n || !n->id) continue;
    const uint8_t cur = s_live[i].state;
    if (s_prev_state[i] == aio::ui::NODE_ONLINE && cur != aio::ui::NODE_ONLINE) {
      snprintf(txt, sizeof(txt), "%s offline", n->profile.name);
      mq_notify(txt, n->id);
    }
    s_prev_state[i] = cur;
    /* shortcut: the low-stock latch clears only on reboot; a refill logged
     * over MQTT re-fires it until then, acceptable for v1 */
    if (!s_low_latch[i] &&
        aio::remote_notify_low_stock(*s_ctx.log, *n, unix_now, txt, sizeof(txt))) {
      mq_notify(txt, n->id);
      s_low_latch[i] = true;
    }
  }
}

void remote_net_on_event(const aio::HubEvent &e) {
  if (!s_inited) return;
  char topic[48], pl[256], txt[128];
  const bool terminal = e.kind == aio::EV_FEED_OK || e.kind == aio::EV_FEED_FAIL ||
                        e.kind == aio::EV_FEED_REJ || e.kind == aio::EV_FEED_UNKNOWN;
  if (terminal && s_mqtt_up == 2) {
    snprintf(topic, sizeof(topic), "aio/node/%u/event/feed_result", (unsigned)e.node);
    snprintf(pl, sizeof(pl),
             "{\"feed_id\":%lu,\"result\":\"%s\",\"amount\":%u,\"unix\":%lu}",
             (unsigned long)e.feed_id, aio::csv_result_text(e.kind, e.result),
             (unsigned)e.amount, (unsigned long)e.unix);
    mq_publish(topic, pl, 1, false);
    if (e.kind == aio::EV_FEED_OK) { /* fresh last-feed on the state topic */
      for (uint8_t i = 0; i < aio::HUB_MAX_NODES; i++) {
        const aio::HubNode *n = s_ctx.reg->node(i);
        if (n && n->id == e.node) {
          rebuild_live();
          publish_node(i);
          break;
        }
      }
    }
  }
  if (aio::remote_notify_text(e.kind, e.node, notify_node_name(e.node), e.amount,
                              e.aux, txt, sizeof(txt)))
    mq_notify(txt, e.node);
}

void remote_net_service(void) {
  /* runs with the engine mutex NOT held: httpd_stop waits for in-flight
   * handlers, which need the mutex to finish */
  if (!s_inited || !s_apply_pending) return;
  s_apply_pending = false;
  remote_net_apply();
}

void remote_net_request_apply(void) { s_apply_pending = true; }

const char *remote_net_ip(void) { return s_ip; }
const char *remote_net_mqtt_state(void) {
  return s_mqtt_up == 2 ? "up" : s_mqtt_up == 1 ? "down" : "off";
}
bool remote_net_web_up(void) { return s_http != nullptr; }

#else /* QEMU no-radio stubs ----------------------------------------------------------- */

static RemoteNetCtx s_ctx;
void remote_net_init(RemoteNetCtx *ctx) { s_ctx = *ctx; }
void remote_net_apply(void) {}
void remote_net_poll(uint32_t, int64_t) {}
void remote_net_service(void) {}
void remote_net_request_apply(void) {}
void remote_net_on_event(const aio::HubEvent &) {}
const char *remote_net_ip(void) { return ""; }
const char *remote_net_mqtt_state(void) { return "off"; }
bool remote_net_web_up(void) { return false; }

#endif
