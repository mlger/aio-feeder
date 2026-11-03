/*
 * AIO Feeder — remote access core (PLAN.md §6.7 web dashboard + MQTT, FR20,
 * FR2; M6).
 *
 * Pure logic, host-tested. The firmware glue (hub/main/remote_net.cpp) binds
 * this to esp_http_server + esp-mqtt; here live the JSON builders/parsers,
 * the HA discovery entities, the notify texts and the remote settings blob.
 *
 * Remote settings get their own NVS blob ("rmt") instead of growing the
 * settings blob — a v0.8 → v0.9 field upgrade keeps the existing PIN/Wi-Fi
 * settings untouched (store_get version skew → defaults would wipe them).
 * The MQTT password never enters backups (D18/D32: backup.h predates this
 * blob and excludes it).
 *
 * Topic map (§6.7):
 *   aio/hub/status               retained online/offline (LWT offline)
 *   aio/hub/notify               {"text":"…","node":N}
 *   aio/node/<id>/state          retained telemetry JSON
 *   aio/node/<id>/cmd/feed       {"amount":N,"unit":"…"} (unit accepted, ignored
 *                                — the node's profile owns the unit)
 *   aio/node/<id>/event/feed_result   one row per terminal feed event
 *   aio/node/<id>/config         retained profile summary
 *   homeassistant/<comp>/aio-n<id>-<ent>/config   HA discovery (retained)
 *
 * Auth (§6.7 "login requirement"): when web_auth is set, every /api request
 * must carry the hub's 4-digit PIN (header X-AIO-Pin). Fail-closed: if the
 * PIN is not configured, access stays denied.
 */
#ifndef AIO_REMOTE_H
#define AIO_REMOTE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "backup.h" /* JSON helpers, csv_history_write, BACKUP_NAME_LEN */
#include "hub_registry.h"
#include "hub_store.h"
#include "hub_time.h"
#include "nodefw.h"
#include "ota_core.h"
#include "settings_store.h"
#include "ui_model.h" /* NodeState, state_of, next_occurrence, is_alert_kind */
#include "unpair_queue.h"

namespace aio {

#define RMT_VERSION 2 /* v2 (M7): + notify trigger switches, §9.11 */
constexpr char RMT_STORE_KEY[] = "rmt";

constexpr uint16_t RMT_WEB_PORT_DEF = 80;
constexpr uint16_t RMT_WEB_PORT_MAX = 65535;
constexpr uint16_t RMT_MQTT_PORT_DEF = 1883;

struct RemoteSettings {
  uint16_t version;  /* RMT_VERSION */
  uint8_t web_on;    /* dashboard switch (§6.7) */
  uint8_t web_auth;  /* require the hub PIN on /api */
  uint16_t web_port; /* dashboard TCP port */
  uint8_t mqtt_on;
  uint8_t mqtt_disc;       /* HA discovery publishes */
  char mqtt_url[96];       /* mqtt[s]://host[:port] or bare host[:port] */
  char mqtt_user[33];      /* "" = anonymous */
  char mqtt_pass[65];      /* never backed up, never echoed back */
  /* v2 (M7): switchable notify triggers (§9.11) — master + per trigger. */
  uint8_t notify_master;
  uint8_t notify_offline;  /* node offline transitions */
  uint8_t notify_fault;    /* feed fail / reject / unknown */
  uint8_t notify_missed;   /* missed feeds (§9.4) */
  uint8_t notify_stock;    /* low stock estimate */
  uint8_t notify_sensor;   /* hopper-low (FMEA #8) */
};

/* v1 layout = v2 without the trailing trigger switches (strict prefix), so
 * a v1 blob can be migrated field-for-field (D36: upgrades never wipe). */
struct RemoteSettingsV1 {
  uint16_t version;
  uint8_t web_on;
  uint8_t web_auth;
  uint16_t web_port;
  uint8_t mqtt_on;
  uint8_t mqtt_disc;
  char mqtt_url[96];
  char mqtt_user[33];
  char mqtt_pass[65];
};

static inline RemoteSettings remote_defaults() {
  RemoteSettings s;
  memset(&s, 0, sizeof(s));
  s.version = RMT_VERSION;
  s.web_on = 1;
  s.web_auth = 0;
  s.web_port = RMT_WEB_PORT_DEF;
  s.mqtt_on = 0;
  s.mqtt_disc = 1;
  s.notify_master = 1;
  s.notify_offline = 1;
  s.notify_fault = 1;
  s.notify_missed = 1;
  s.notify_stock = 1;
  s.notify_sensor = 1;
  return s;
}

static inline bool remote_load(IHubStore &st, RemoteSettings *out) {
  RemoteSettings s;
  if (store_get(st, RMT_STORE_KEY, RMT_VERSION, &s, sizeof(s)) == (int)sizeof(s)) {
    if (s.web_port == 0) return false;
    *out = s;
    return true;
  }
  /* v1 → v2 migration: prefix copy keeps web + MQTT fields (D36). */
  RemoteSettingsV1 v1;
  if (store_get(st, RMT_STORE_KEY, 1, &v1, sizeof(v1)) == (int)sizeof(v1) &&
      v1.web_port != 0) {
    static_assert(sizeof(RemoteSettingsV1) < sizeof(RemoteSettings),
                  "v1 must be a strict prefix of v2");
    RemoteSettings v2 = remote_defaults();
    memcpy(&v2, &v1, sizeof(v1));
    v2.version = RMT_VERSION;
    *out = v2;
    return true;
  }
  return false;
}

static inline bool remote_save(IHubStore &st, const RemoteSettings &s) {
  return store_put(st, RMT_STORE_KEY, RMT_VERSION, &s, sizeof(s));
}

static inline bool remote_set_web(RemoteSettings *s, uint8_t on, uint16_t port,
                                  uint8_t auth) {
  if (port == 0) return false;
  s->web_on = on ? 1 : 0;
  s->web_auth = auth ? 1 : 0;
  s->web_port = port;
  return true;
}

static inline bool remote_set_mqtt(RemoteSettings *s, const char *url,
                                   const char *user, const char *pass) {
  if (!url || strlen(url) >= sizeof(s->mqtt_url)) return false;
  snprintf(s->mqtt_url, sizeof(s->mqtt_url), "%s", url);
  snprintf(s->mqtt_user, sizeof(s->mqtt_user), "%s", user ? user : "");
  snprintf(s->mqtt_pass, sizeof(s->mqtt_pass), "%s", pass ? pass : "");
  return true;
}

/* esp-mqtt's broker uri needs a scheme; accept bare "host[:port]" too. */
static inline bool remote_mqtt_uri(const char *in, char *out, size_t cap) {
  if (!in || !in[0] || strlen(in) >= cap) return false;
  if (strstr(in, "://")) {
    snprintf(out, cap, "%s", in);
    return true;
  }
  snprintf(out, cap, "mqtt://%s", in);
  return true;
}

/* §6.7 auth: open LAN when web_auth is clear; else the hub PIN. A web_auth
 * flag without a configured PIN denies everything (fail-closed). */
static inline bool remote_auth_ok(const RemoteSettings &r, const HubSettings &set,
                                  const char *pin) {
  if (!r.web_auth) return true;
  return pin_verify(set, pin ? pin : "");
}

/* --- telemetry snapshot --------------------------------------------------------- */

/* Per-node link freshness, filled by the glue from LinkEngine peers (slot
 * order = registry.node(i)). Pure layer only reads it. */
struct RemoteNodeLive {
  uint8_t state = ui::NODE_OFFLINE; /* ui::NodeState */
  uint16_t vin_mv = 0;
  uint32_t hb_age_s = 0;
  bool ota_lock = false; /* M8b: HB_FLAG_OTA_LOCK from the last heartbeat */
};

static inline const char *remote_unit_name(uint8_t unit) {
  return unit == UNIT_ML ? "ml" : unit == UNIT_PULSES ? "pulses" : "g";
}

static inline const char *remote_mech_name(uint8_t mech) {
  return mech == MECH_PUMP ? "pump" : mech == MECH_MOTOR_ONEWAY ? "motor" : "gate";
}

static inline const char *remote_state_name(uint8_t st) {
  return st == ui::NODE_ONLINE ? "online" : st == ui::NODE_STALE ? "stale" : "offline";
}

/* Newest terminal event for a node (0 = none). */
static inline const HubEvent *remote_last_event(const EventLog &log, uint8_t node) {
  for (uint16_t k = log.count(); k > 0; k--) {
    const HubEvent *e = log.at(k - 1);
    if (e->node == node &&
        (e->kind == EV_FEED_OK || e->kind == EV_FEED_FAIL || e->kind == EV_FEED_REJ ||
         e->kind == EV_FEED_UNKNOWN))
      return e;
  }
  return nullptr;
}

/* Stock estimate (FR12): capacity left, from the OK feeds in the RAM ring.
 * Always an estimate — the ring forgets after 128 events / retention prune.
 * days_left uses the average daily rate over the ring window (0 = unknown). */
static inline void remote_stock(const EventLog &log, const HubNode &n,
                                int64_t unix_now, uint32_t *left, uint32_t *days) {
  uint32_t dispensed = 0, oldest = 0, newest = 0;
  for (uint16_t i = 0; i < log.count(); i++) {
    const HubEvent *e = log.at(i);
    if (e->node != n.id || e->kind != EV_FEED_OK) continue;
    dispensed += e->amount;
    if (!oldest || e->unix < oldest) oldest = e->unix;
    if (e->unix > newest) newest = e->unix;
  }
  const uint32_t cap = n.profile.storage_capacity;
  if (dispensed >= cap) {
    *left = 0;
  } else {
    *left = cap - dispensed;
  }
  *days = 0;
  if (newest > oldest && unix_now > newest) {
    /* rate over the observed window, extrapolated on the fresh side */
    const uint32_t span_s = (uint32_t)((newest - oldest) + (unix_now - newest));
    if (span_s >= 3600) {
      const uint32_t per_day = (uint32_t)((uint64_t)dispensed * 86400 / span_s);
      if (per_day) *days = (uint32_t)((uint64_t)(*left) * 86400 / (uint64_t)per_day);
    }
  }
  (void)unix_now;
}

/* Per-node telemetry JSON (retained `aio/node/<id>/state`). */
static inline size_t remote_node_state_json(const HubNode &n, const EventLog &log,
                                            const RemoteNodeLive &live,
                                            int64_t unix_now, char *out, size_t cap) {
  const uint32_t left_ = [&] {
    uint32_t l = 0, d = 0;
    remote_stock(log, n, unix_now, &l, &d);
    return l;
  }();
  const HubEvent *last = remote_last_event(log, n.id);
  const bool online = live.state == ui::NODE_ONLINE;
  size_t o = (size_t)snprintf(out, cap,
                              "{\"online\":%s,\"name\":\"%s\",\"unit\":\"%s\","
                              "\"vin\":%u.%02u,\"hb_age\":%u",
                              online ? "true" : "false", n.profile.name,
                              remote_unit_name(n.profile.unit),
                              (unsigned)(live.vin_mv / 1000),
                              (unsigned)((live.vin_mv % 1000) / 10),
                              (unsigned)live.hb_age_s);
  if (o >= cap) return 0;
  if (n.profile.storage_capacity) {
    o += (size_t)snprintf(out + o, cap - o, ",\"cap\":%u,\"left\":%u",
                          (unsigned)n.profile.storage_capacity, (unsigned)left_);
    if (o >= cap) return 0;
  }
  if (last) {
    o += (size_t)snprintf(out + o, cap - o,
                          ",\"last_amount\":%u,\"last_result\":\"%s\",\"last_age\":%u",
                          (unsigned)last->amount,
                          csv_result_text(last->kind, last->result),
                          (unsigned)(unix_now > (int64_t)last->unix
                                         ? (uint32_t)(unix_now - (int64_t)last->unix)
                                         : 0));
    if (o >= cap) return 0;
  }
  o += (size_t)snprintf(out + o, cap - o, "}");
  return o >= cap ? 0 : o;
}

/* Profile summary (retained `aio/node/<id>/config`). */
static inline size_t remote_node_config_json(const HubNode &n, char *out, size_t cap) {
  const node_profile_t &p = n.profile;
  const size_t o = (size_t)snprintf(
      out, cap,
      "{\"name\":\"%s\",\"mech\":\"%s\",\"unit\":\"%s\",\"cal_ms\":%u,"
      "\"cool\":%u,\"max_hour\":%u,\"max_day\":%u,\"cap\":%u,"
      "\"policy\":\"%s\",\"en\":%s}",
      p.name, remote_mech_name(p.mech), remote_unit_name(p.unit),
      (unsigned)p.cal_ms_per_unit_x100, (unsigned)p.cooldown_ms,
      (unsigned)p.max_units_per_hour, (unsigned)p.max_units_per_day,
      (unsigned)p.storage_capacity, p.missed_policy == 1 ? "catchup" : "skip",
      n.sched.enabled ? "true" : "false");
  return o >= cap ? 0 : o;
}

/* --- hub status (GET /api/status) ------------------------------------------------- */

static inline size_t remote_status_json(const HubRegistry &reg, const EventLog &log,
                                        const RemoteSettings &rset, bool mqtt_up,
                                        const RemoteNodeLive *live, const char *fw,
                                        const LocalTime &now, int64_t unix_now,
                                        char *out, size_t cap,
                                        const UnpairQueue *unpair = nullptr,
                                        const OtaBrief *ota = nullptr,
                                        const NodeFw *nfw = nullptr) {
  uint8_t online = 0;
  bool has_alert = false;
  uint8_t alert_node = 0;
  uint8_t alert_kind = 0;
  for (uint8_t i = 0; i < HUB_MAX_NODES; i++) {
    const HubNode *n = reg.node(i);
    if (!n || !n->id) continue;
    if (live[i].state == ui::NODE_ONLINE) online++;
  }
  /* newest alert-worthy event, any node (same rule as the dashboard strip) */
  for (uint16_t k = log.count(); k > 0 && !has_alert; k--) {
    const HubEvent *e = log.at(k - 1);
    if (ui::is_alert_kind(e->kind)) {
      has_alert = true;
      alert_node = e->node;
      alert_kind = e->kind;
    }
  }

  size_t o = (size_t)snprintf(
      out, cap,
      "{\"fw\":\"%s\",\"time_ok\":%s,\"unix\":%lld,\"tz\":%u,\"nodes\":%u,"
      "\"online\":%u,\"mqtt\":\"%s\",\"web_auth\":%s,\"port\":%u",
      fw ? fw : "", unix_now >= 1600000000 ? "true" : "false",
      (long long)unix_now, (unsigned)reg.data().tz_rule, (unsigned)reg.count(),
      (unsigned)online, mqtt_up ? "up" : rset.mqtt_on ? "down" : "off",
      rset.web_auth ? "true" : "false", (unsigned)rset.web_port);
  if (o >= cap) return 0;
  if (has_alert) {
    o += (size_t)snprintf(out + o, cap - o, ",\"alert\":{\"node\":%u,\"kind\":\"%s\"}",
                          (unsigned)alert_node, csv_result_text(alert_kind, 0));
    if (o >= cap) return 0;
  }
  if (unpair && unpair->count()) {
    /* FMEA #15: nodes removed while offline — UNPAIR still pending. */
    o += (size_t)snprintf(out + o, cap - o, ",\"unpair\":[");
    if (o >= cap) return 0;
    for (uint8_t i = 0; i < unpair->count(); i++) {
      const PendingUnpair *q = unpair->at(i);
      if (!q) continue;
      o += (size_t)snprintf(out + o, cap - o,
                            "%s{\"id\":%u,\"mac\":\"%02X%02X%02X%02X%02X%02X\","
                            "\"sends\":%u}",
                            i ? "," : "", (unsigned)q->id, q->mac[0], q->mac[1],
                            q->mac[2], q->mac[3], q->mac[4], q->mac[5],
                            (unsigned)q->sends);
      if (o >= cap) return 0;
    }
    o += (size_t)snprintf(out + o, cap - o, "]");
    if (o >= cap) return 0;
  }
  if (ota) {
    /* M8a: firmware/OTA brief (phase name + pct + slot/target) */
    o += (size_t)snprintf(
        out + o, cap - o,
        ",\"ota\":{\"phase\":\"%s\",\"pct\":%u,\"slot\":\"%s\","
        "\"ver\":\"%s\",\"target\":\"%s\",\"err\":\"%s\"}",
        ota_phase_name(ota->phase), (unsigned)ota->pct, ota->slot, ota->ver,
        ota->target, ota_err_name(ota->err));
    if (o >= cap) return 0;
  }
  if (nfw && nfw->img_valid()) {
    /* M8b: the stored node image + which nodes it can go to */
    char sha_hex[65];
    nodefw_sha_hex(nfw->img().sha, sha_hex, sizeof(sha_hex));
    o += (size_t)snprintf(
        out + o, cap - o,
        ",\"nodefw\":{\"present\":true,\"target\":\"%s\",\"ver\":\"%s\","
        "\"size\":%lu,\"sha\":\"%s\"}",
        nodefw_target_name(nfw->img().target), nfw->img().ver,
        (unsigned long)nfw->img().size, sha_hex);
    if (o >= cap) return 0;
  }
  o += (size_t)snprintf(out + o, cap - o, ",\"n\":[");
  if (o >= cap) return 0;

  bool first = true;
  for (uint8_t i = 0; i < HUB_MAX_NODES; i++) {
    const HubNode *n = reg.node(i);
    if (!n || !n->id) continue;
    uint32_t left_ = 0, days = 0;
    remote_stock(log, *n, unix_now, &left_, &days);
    uint16_t next_min = 0, next_amt = 0;
    int32_t next_in = 0;
    const bool has_next =
        ui::next_occurrence(*n, now.min_of_day, now.wday, &next_min, &next_in, &next_amt);
    char name_esc[PROFILE_NAME_LEN * 2 + 2];
    json_escape(name_esc, sizeof(name_esc), 0, n->profile.name);
    o += (size_t)snprintf(
        out + o, cap - o,
        "%s{\"id\":%u,\"name\":\"%s\",\"mech\":\"%s\",\"unit\":\"%s\","
        "\"state\":\"%s\",\"vin\":%u.%02u,\"hb_age\":%u,\"left\":%u,\"days\":%u",
        first ? "" : ",", (unsigned)n->id, name_esc,
        remote_mech_name(n->profile.mech), remote_unit_name(n->profile.unit),
        remote_state_name(live[i].state), (unsigned)(live[i].vin_mv / 1000),
        (unsigned)((live[i].vin_mv % 1000) / 10), (unsigned)live[i].hb_age_s,
        (unsigned)left_, (unsigned)days);
    first = false;
    if (o >= cap) return 0;
    /* M8b: node fw identity + ota lock, when known */
    if (nfw) {
      const NodeFwNode *nf = nfw->node(n->id);
      if (nf && (nf->rel_ver || nf->mcu != MCU_NONE)) {
        char nver[16];
        nodefw_ver_str(nf->rel_ver, nver, sizeof(nver));
        o += (size_t)snprintf(out + o, cap - o,
                              ",\"fw\":\"%s\",\"mcu\":\"%s\",\"lock\":%s",
                              nver,
                              nf->mcu == MCU_ESP8266 ? "esp8266"
                              : nf->mcu == MCU_ESP32C3 ? "esp32c3" : "?",
                              live[i].ota_lock ? "true" : "false");
        if (o >= cap) return 0;
      } else if (live[i].ota_lock) {
        o += (size_t)snprintf(out + o, cap - o, ",\"lock\":true");
        if (o >= cap) return 0;
      }
    } else if (live[i].ota_lock) {
      o += (size_t)snprintf(out + o, cap - o, ",\"lock\":true");
      if (o >= cap) return 0;
    }
    const HubEvent *last = remote_last_event(log, n->id);
    if (last) {
      o += (size_t)snprintf(out + o, cap - o,
                            ",\"last\":{\"amount\":%u,\"result\":\"%s\",\"age\":%u}",
                            (unsigned)last->amount,
                            csv_result_text(last->kind, last->result),
                            (unsigned)(unix_now > (int64_t)last->unix
                                           ? (uint32_t)(unix_now - (int64_t)last->unix)
                                           : 0));
      if (o >= cap) return 0;
    }
    if (has_next) {
      o += (size_t)snprintf(out + o, cap - o,
                            ",\"next\":{\"min\":\"%02u:%02u\",\"in\":%d,\"amount\":%u}",
                            (unsigned)(next_min / 60), (unsigned)(next_min % 60),
                            (int)next_in, (unsigned)next_amt);
      if (o >= cap) return 0;
    }
    o += (size_t)snprintf(out + o, cap - o, ",\"en\":%s}",
                          n->sched.enabled ? "true" : "false");
    if (o >= cap) return 0;
  }
  o += (size_t)snprintf(out + o, cap - o, "]}");
  return o >= cap ? 0 : o;
}

/* --- event rows (GET /api/events) + terminal-event publish row --------------------- */

/* limit = 0 → all. Rows newest-first (dashboard table convention). */
static inline size_t remote_events_json(const HubRegistry &reg, const EventLog &log,
                                        uint16_t limit, char *out, size_t cap) {
  size_t o = (size_t)snprintf(out, cap, "{\"e\":[");
  if (o >= cap) return 0;
  const uint16_t count = log.count();
  const uint16_t skip = limit && count > limit ? (uint16_t)(count - limit) : 0;
  bool first = true;
  for (uint16_t i = skip; i < count; i++) {
    const HubEvent *e = log.at(i);
    const HubNode *n = reg.by_id(e->node);
    char nbuf[PROFILE_NAME_LEN + 8];
    if (n)
      snprintf(nbuf, sizeof(nbuf), "%s", n->profile.name);
    else
      snprintf(nbuf, sizeof(nbuf), "node%u", (unsigned)e->node);
    char name_esc[sizeof(nbuf) * 2];
    json_escape(name_esc, sizeof(name_esc), 0, nbuf);
    o += (size_t)snprintf(out + o, cap - o,
                          "%s{\"unix\":%u,\"node\":%u,\"name\":\"%s\","
                          "\"amount\":%u,\"kind\":\"%s\",\"result\":%u,"
                          "\"src\":\"%s\"}",
                          first ? "" : ",", (unsigned)e->unix, (unsigned)e->node,
                          name_esc, (unsigned)e->amount,
                          csv_result_text(e->kind, e->result), (unsigned)e->result,
                          ev_src_name(e->src));
    first = false;
    if (o >= cap) return 0;
  }
  o += (size_t)snprintf(out + o, cap - o, "]}");
  return o >= cap ? 0 : o;
}

/* CSV into a caller buffer (FR6 phone download; same columns as §9.12). */
static inline size_t remote_csv(const HubRegistry &reg, const EventLog &log,
                                int64_t unix_now, char *out, size_t cap) {
  char name[BACKUP_NAME_LEN];
  size_t len = 0;
  if (unix_now < 1600000000) return 0;
  if (!csv_history_write(reg, log, unix_now, out, cap, name, &len)) return 0;
  return len;
}

/* --- command parsing --------------------------------------------------------------- */

/* POST /api/feed and the MQTT cmd topic share the body: {"node":N,"amount":M}.
 * A "unit" member is tolerated and ignored (§6.7 — the profile owns it). */
static inline bool remote_feed_parse(const char *body, uint8_t *node,
                                     uint16_t *amount) {
  if (!body) return false;
  const char *p = body;
  char key[16];
  if (!json_expect(&p, '{')) return false;
  bool have_node = false, have_amount = false;
  uint32_t n = 0, a = 0;
  while (json_next_key(&p, key, sizeof(key))) {
    if (!strcmp(key, "node")) {
      if (!json_parse_uint(&p, &n)) return false;
      have_node = true;
    } else if (!strcmp(key, "amount")) {
      if (!json_parse_uint(&p, &a)) return false;
      have_amount = true;
    } else if (!strcmp(key, "unit")) {
      char u[12];
      if (!json_parse_string(&p, u, sizeof(u))) return false;
    } else {
      return false; /* unknown key: refuse rather than half-read */
    }
  }
  if (!have_node || !have_amount) return false;
  if (n == 0 || n > HUB_MAX_NODES || a == 0 || a > 65535) return false;
  *node = (uint8_t)n;
  *amount = (uint16_t)a;
  return true;
}

/* POST /api/ota (M8a): {"url":"http(s)://…"} starts a download+stage,
 * {"factory":1} arms the factory-image escape hatch. No sha256 field in
 * v1: the release ota.json pins the digest; a URL pointing at the served
 * .bin is enough. */
static inline bool remote_ota_parse(const char *body, char *url, size_t cap,
                                    bool *factory = nullptr) {
  if (!body || !url || cap < OTA_URL_LEN) return false;
  url[0] = '\0';
  if (factory) *factory = false;
  const char *p = body;
  char key[16];
  if (!json_expect(&p, '{')) return false;
  while (json_next_key(&p, key, sizeof(key))) {
    if (!strcmp(key, "url")) {
      char u[OTA_URL_LEN];
      if (!json_parse_string(&p, u, sizeof(u))) return false;
      snprintf(url, cap, "%s", u);
    } else if (!strcmp(key, "factory") && factory) {
      uint32_t v;
      if (!json_parse_uint(&p, &v) || v > 1) return false;
      *factory = v != 0;
    } else {
      return false; /* unknown key: refuse rather than half-read */
    }
  }
  if (factory && *factory) return true; /* the escape hatch needs no URL */
  return ota_url_valid(url);
}

/* MQTT topic aio/node/<id>/cmd/feed → id (0 = not a feed topic). Strict:
 * the suffix must match exactly, not just the sscanf prefix. */
static inline uint8_t mqtt_feed_topic_parse(const char *topic) {
  if (!topic) return 0;
  constexpr char PREFIX[] = "aio/node/";
  if (strncmp(topic, PREFIX, sizeof(PREFIX) - 1) != 0) return 0;
  char *end = nullptr;
  const unsigned long id = strtoul(topic + sizeof(PREFIX) - 1, &end, 10);
  if (!end || strcmp(end, "/cmd/feed") != 0) return 0;
  if (id == 0 || id > HUB_MAX_NODES) return 0;
  return (uint8_t)id;
}

/* Partial settings patch (POST /api/settings): only provided keys change.
 * mqtt_pass:"" clears the stored password. */
static inline bool remote_settings_patch(const char *body, RemoteSettings *s) {
  if (!body || !s) return false;
  const char *p = body;
  char key[16];
  if (!json_expect(&p, '{')) return false;
  json_skip_ws(&p);
  while (*p != '}') {
    if (!json_next_key(&p, key, sizeof(key))) return false; /* malformed or EOF */
    if (!strcmp(key, "web_on") || !strcmp(key, "web_auth") ||
        !strcmp(key, "mqtt_on") || !strcmp(key, "mqtt_disc")) {
      uint32_t v;
      if (!json_parse_uint(&p, &v)) return false;
      if (v > 1) return false;
      if (!strcmp(key, "web_on")) s->web_on = (uint8_t)v;
      else if (!strcmp(key, "web_auth")) s->web_auth = (uint8_t)v;
      else if (!strcmp(key, "mqtt_on")) s->mqtt_on = (uint8_t)v;
      else s->mqtt_disc = (uint8_t)v;
    } else if (!strcmp(key, "web_port")) {
      uint32_t v;
      if (!json_parse_uint(&p, &v) || v == 0 || v > RMT_WEB_PORT_MAX) return false;
      s->web_port = (uint16_t)v;
    } else if (!strcmp(key, "notify_master") || !strcmp(key, "notify_offline") ||
               !strcmp(key, "notify_fault") || !strcmp(key, "notify_missed") ||
               !strcmp(key, "notify_stock") || !strcmp(key, "notify_sensor")) {
      uint32_t v;
      if (!json_parse_uint(&p, &v)) return false;
      if (v > 1) return false;
      if (!strcmp(key, "notify_master")) s->notify_master = (uint8_t)v;
      else if (!strcmp(key, "notify_offline")) s->notify_offline = (uint8_t)v;
      else if (!strcmp(key, "notify_fault")) s->notify_fault = (uint8_t)v;
      else if (!strcmp(key, "notify_missed")) s->notify_missed = (uint8_t)v;
      else if (!strcmp(key, "notify_stock")) s->notify_stock = (uint8_t)v;
      else s->notify_sensor = (uint8_t)v;
    } else if (!strcmp(key, "mqtt_url")) {
      char u[sizeof(s->mqtt_url)];
      if (!json_parse_string(&p, u, sizeof(u))) return false;
      snprintf(s->mqtt_url, sizeof(s->mqtt_url), "%s", u);
    } else if (!strcmp(key, "mqtt_user")) {
      char u[sizeof(s->mqtt_user)];
      if (!json_parse_string(&p, u, sizeof(u))) return false;
      snprintf(s->mqtt_user, sizeof(s->mqtt_user), "%s", u);
    } else if (!strcmp(key, "mqtt_pass")) {
      char u[sizeof(s->mqtt_pass)];
      if (!json_parse_string(&p, u, sizeof(u))) return false;
      snprintf(s->mqtt_pass, sizeof(s->mqtt_pass), "%s", u);
    } else {
      return false;
    }
    json_skip_ws(&p);
  }
  return s->web_port != 0;
}

/* --- HA discovery (§6.7 button + sensors per node) ----------------------------------- */

/* Entities per node: 0 = feed button, 1 = last-feed sensor, 2 = online
 * binary sensor, 3 = supply-voltage sensor (flagged profiles only),
 * 4 = stock estimate (capacity > 0 only). Returns false past the last. */
static inline bool mqtt_discovery_entity(const HubNode &n, uint8_t idx, char *topic,
                                         size_t tcap, char *pl, size_t pcap) {
  const uint8_t id = n.id;
  char devid[16];
  snprintf(devid, sizeof(devid), "aio-node%u", (unsigned)id);
  char dev[96];
  snprintf(dev, sizeof(dev),
           "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"aio-feeder\"}",
           devid, n.profile.name);
  const char *state_t = "aio/node/%u/state";
  char st[32];
  snprintf(st, sizeof(st), state_t, (unsigned)id);
  char cmd[32];
  snprintf(cmd, sizeof(cmd), "aio/node/%u/cmd/feed", (unsigned)id);
  const char *avty = "\"avty_t\":\"aio/hub/status\"";

  if (idx == 0) {
    /* payload amount: first schedule entry, else a conservative 10 */
    uint16_t amt = n.sched.entry_count ? n.sched.entries[0].amount : 10;
    snprintf(topic, tcap, "homeassistant/button/aio-n%u-feed/config", (unsigned)id);
    const size_t o = (size_t)snprintf(
        pl, pcap,
        "{\"name\":\"Feed now\",\"uniq_id\":\"%s-feed\",\"cmd_t\":\"%s\","
        "\"payload_json\":\"{\\\"amount\\\":%u}\",%s,%s}",
        devid, cmd, (unsigned)amt, avty, dev);
    return o < pcap;
  }
  if (idx == 1) {
    snprintf(topic, tcap, "homeassistant/sensor/aio-n%u-last/config", (unsigned)id);
    const size_t o = (size_t)snprintf(
        pl, pcap,
        "{\"name\":\"Last feed\",\"uniq_id\":\"%s-last\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.last_amount }}\",\"unit_of_meas\":\"%s\","
        "%s,%s}",
        devid, st, remote_unit_name(n.profile.unit), avty, dev);
    return o < pcap;
  }
  if (idx == 2) {
    snprintf(topic, tcap, "homeassistant/binary_sensor/aio-n%u-online/config",
             (unsigned)id);
    const size_t o = (size_t)snprintf(
        pl, pcap,
        "{\"name\":\"Online\",\"uniq_id\":\"%s-online\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.online }}\",\"pl_on\":true,\"pl_off\":false,"
        "\"dev_cls\":\"connectivity\",%s,%s}",
        devid, st, avty, dev);
    return o < pcap;
  }
  if (idx == 3 && (n.profile.flags & PROFILE_FLAG_SUPPLY_MONITOR)) {
    snprintf(topic, tcap, "homeassistant/sensor/aio-n%u-supply/config", (unsigned)id);
    const size_t o = (size_t)snprintf(
        pl, pcap,
        "{\"name\":\"Supply\",\"uniq_id\":\"%s-supply\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.vin }}\",\"unit_of_meas\":\"V\","
        "\"dev_cls\":\"voltage\",%s,%s}",
        devid, st, avty, dev);
    return o < pcap;
  }
  if (idx == 4 && n.profile.storage_capacity) {
    snprintf(topic, tcap, "homeassistant/sensor/aio-n%u-stock/config", (unsigned)id);
    const size_t o = (size_t)snprintf(
        pl, pcap,
        "{\"name\":\"Stock estimate\",\"uniq_id\":\"%s-stock\",\"stat_t\":\"%s\","
        "\"val_tpl\":\"{{ value_json.left }}\",\"unit_of_meas\":\"%s\",%s,%s}",
        devid, st, remote_unit_name(n.profile.unit), avty, dev);
    return o < pcap;
  }
  return false;
}

/* --- notify texts (FR20 §9.11) -------------------------------------------------------- */

/* Sentinels for notifies that do not ride the event log. */
constexpr uint8_t RMT_NOTIFY_OFFLINE = 0xFE;
constexpr uint8_t RMT_NOTIFY_STOCK = 0xFD;

/* §9.11: a master switch plus one switch per trigger. Administrative rows
 * (UNPAIR_PENDING) always notify — they are rare and actionable. */
static inline bool notify_switch_allows(const RemoteSettings &s, uint8_t kind) {
  if (!s.notify_master) return false;
  switch (kind) {
    case RMT_NOTIFY_OFFLINE: return s.notify_offline != 0;
    case RMT_NOTIFY_STOCK: return s.notify_stock != 0;
    case EV_FEED_FAIL:
    case EV_FEED_REJ:
    case EV_FEED_UNKNOWN: return s.notify_fault != 0;
    case EV_SKIPPED_MISSED: return s.notify_missed != 0;
    case EV_HOPPER_LOW: return s.notify_sensor != 0;
    default: return true;
  }
}

/* One short line per alert-worthy event. Returns false for kinds that do
 * not notify (user-initiated skips, plain OK feeds — those ride the state
 * topic instead). */
static inline bool remote_notify_text(uint8_t kind, uint8_t node_id,
                                      const char *name, uint16_t amount, int16_t aux,
                                      char *out, size_t cap) {
  switch (kind) {
    case EV_FEED_FAIL:
      snprintf(out, cap, "feed failed on %s (%u)", name ? name : "?", (unsigned)amount);
      return true;
    case EV_FEED_REJ:
      snprintf(out, cap, "feed rejected on %s (reason %u)", name ? name : "?",
               (unsigned)aux);
      return true;
    case EV_FEED_UNKNOWN:
      snprintf(out, cap, "feed result unknown on %s (%u)", name ? name : "?",
               (unsigned)amount);
      return true;
    case EV_SKIPPED_MISSED:
      snprintf(out, cap, "%d missed feed%s on %s", (int)aux, aux == 1 ? "" : "s",
               name ? name : "?");
      return true;
    case EV_HOPPER_LOW:
      snprintf(out, cap, "hopper low on %s", name ? name : "?");
      return true;
    case EV_UNPAIR_PENDING:
      snprintf(out, cap, "%s removed — will unpair when it checks in",
               name ? name : "?");
      return true;
    default:
      (void)node_id;
      return false;
  }
}

/* Low-stock notify (FR20 "low estimate"): fires once per node per boot when
 * the estimate first drops below 20%% of capacity. */
static inline bool remote_notify_low_stock(const EventLog &log, const HubNode &n,
                                           int64_t unix_now, char *out, size_t cap) {
  if (!n.profile.storage_capacity) return false;
  uint32_t left_ = 0, days = 0;
  remote_stock(log, n, unix_now, &left_, &days);
  if ((uint64_t)left_ * 5 > (uint64_t)n.profile.storage_capacity) return false;
  snprintf(out, cap, "low stock on %s: about %u %s left",
           n.profile.name, (unsigned)left_, remote_unit_name(n.profile.unit));
  return true;
}

}  // namespace aio

#endif /* AIO_REMOTE_H */
