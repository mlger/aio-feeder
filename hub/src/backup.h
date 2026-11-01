/*
 * AIO Feeder — backup, restore and CSV export (PLAN.md §9.12, FR19, D18;
 * M4b).
 *
 * Pure logic, host-tested. The firmware binds IBackupStore to the SD card
 * (hub/main/backup_store.h); host tests and the QEMU shot build use the RAM
 * store (QEMU does not emulate SD, §7.7).
 *
 * Backup file — one JSON object, written to the card as
 * "aio-backup-<YYYYMMDD-HHMM>.json" (local time):
 *
 *   {"aio_backup":1,"ver":1,"created":"20261101-2114","tz":1,"stagger":1,
 *    "sv":9,"nnid":4,"fid":1234,
 *    "set":{"br":80,"soff":60,"ret":30,"ntp":1,"ssid":"home"},
 *    "nodes":[
 *      {"id":1,"mac":"020000000011","key":"a5…","klen":32,"chan":1,
 *       "p":{"name":"kitA","mech":0,"unit":1,"cal":25000,"pon":50,"poff":100,
 *            "maxon":10000,"cool":3000,"maxh":200,"maxd":500,"flags":1,
 *            "miss":0,"cwin":30,"cap":1500},
 *       "s":{"en":1,"n":1,"e":[{"m":450,"d":127,"a":40}],"skip":0,"hold":0}},
 *      …]}
 *
 * Exclusions (D18, §9.12): the PIN (only {salt, hash} exist anyway) and the
 * MQTT password are NEVER written; the Wi-Fi PASSWORD also stays on the
 * device (D32) — restore keeps the current Wi-Fi credentials and the current
 * PIN, and never touches the event history. Per-node LINK KEYS are included:
 * the primary restore case is a replaced hub, and re-pairing every node
 * after a box swap defeats the point (the card is the user's own).
 *
 * Restore semantics (§9.12): replaces nodes + schedules + settings, bumps
 * sched_version and clears pushed_at so the hub re-pushes everything, keeps
 * feed_id_seq monotonic (max(backup, current)), re-CRCs every profile.
 *
 * Export — "aio-history-<YYYYMMDD-HHMM>.csv", §9.12 columns
 * time,node,amount,unit,result,source with RFC-4180 quoting (node names may
 * contain commas or quotes).
 */
#ifndef AIO_BACKUP_H
#define AIO_BACKUP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "event_log.h"
#include "hub_registry.h"
#include "hub_store.h"
#include "hub_time.h"
#include "settings_store.h"

namespace aio {

constexpr uint8_t BACKUP_SLOTS_MAX = 8;      /* files a list() may return */
constexpr size_t BACKUP_NAME_LEN = 32;       /* incl. NUL */
constexpr uint32_t BACKUP_JSON_MAX = 8192;   /* restore read/write cap */

/* --- backup store interface ----------------------------------------------------- */

class IBackupStore {
 public:
  virtual ~IBackupStore() = default;
  virtual bool mount() = 0;               /* idempotent */
  virtual bool present() const = 0;       /* card detected? */
  virtual const char *kind() const = 0;   /* "SD" / "ram" */
  virtual bool write(const char *name, const char *data, size_t len) = 0;
  /* Returns the number of names written (newest first). */
  virtual int list(char out[][BACKUP_NAME_LEN], uint8_t max_names) = 0;
  /* Returns payload length or -1 (missing / too large). */
  virtual int read(const char *name, char *out, size_t cap) = 0;
};

/* RAM-backed store: host tests, and the QEMU shot build's "virtual SD"
 * (heap-allocated there — the QEMU machine has no PSRAM and static DRAM is
 * the binding budget, see tools/qemu/README.md). */
class RamBackupStore : public IBackupStore {
 public:
  RamBackupStore(uint8_t slots, size_t slot_bytes)
      : slots_(slots > BACKUP_SLOTS_MAX ? BACKUP_SLOTS_MAX : slots),
        slot_bytes_(slot_bytes) {
    for (uint8_t i = 0; i < slots_; i++) data_[i] = new char[slot_bytes_];
  }
  ~RamBackupStore() override {
    for (uint8_t i = 0; i < slots_; i++) delete[] data_[i];
  }
  RamBackupStore(const RamBackupStore &) = delete;
  RamBackupStore &operator=(const RamBackupStore &) = delete;

  bool mount() override { return true; }
  bool present() const override { return true; }
  const char *kind() const override { return "ram"; }

  bool write(const char *name, const char *data, size_t len) override {
    if (!name || !name[0] || len > slot_bytes_ || strlen(name) >= BACKUP_NAME_LEN)
      return false;
    int slot = find(name);
    if (slot < 0) {
      slot = used_ < slots_ ? (int)used_ : -1;
      if (slot < 0) return false; /* full */
      used_++;
    }
    strncpy(names_[slot], name, BACKUP_NAME_LEN - 1);
    names_[slot][BACKUP_NAME_LEN - 1] = '\0';
    memcpy(data_[slot], data, len);
    lens_[slot] = len;
    return true;
  }

  int list(char out[][BACKUP_NAME_LEN], uint8_t max_names) override {
    /* newest first: names embed the timestamp, so descending name order is
     * chronological; keep it stable for equal names (should not happen) */
    uint8_t n = 0;
    bool taken[BACKUP_SLOTS_MAX] = {false};
    for (uint8_t round = 0; round < used_ && n < max_names; round++) {
      int best = -1;
      for (uint8_t i = 0; i < used_; i++)
        if (!taken[i] && (best < 0 || strcmp(names_[i], names_[best]) > 0))
          best = i;
      if (best < 0) break;
      taken[best] = true;
      strncpy(out[n], names_[best], BACKUP_NAME_LEN);
      out[n][BACKUP_NAME_LEN - 1] = '\0';
      n++;
    }
    return (int)n;
  }

  int read(const char *name, char *out, size_t cap) override {
    const int slot = find(name);
    if (slot < 0) return -1;
    if (lens_[slot] > cap) return -1;
    memcpy(out, data_[slot], lens_[slot]);
    return (int)lens_[slot];
  }

  uint8_t used() const { return used_; }
  size_t slot_bytes() const { return slot_bytes_; }

 private:
  int find(const char *name) const {
    for (uint8_t i = 0; i < used_; i++)
      if (!strcmp(names_[i], name)) return i;
    return -1;
  }

  static constexpr uint8_t BACKUP_SLOTS_MAX_ = 8;
  uint8_t slots_;
  size_t slot_bytes_;
  char names_[BACKUP_SLOTS_MAX_][BACKUP_NAME_LEN] = {};
  char *data_[BACKUP_SLOTS_MAX_] = {};
  size_t lens_[BACKUP_SLOTS_MAX_] = {};
  uint8_t used_ = 0;
};

/* --- tiny JSON helpers ----------------------------------------------------------- */

static inline size_t json_escape(char *dst, size_t cap, size_t off, const char *s) {
  for (const char *p = s; *p; p++) {
    char tmp[8];
    size_t n = 0;
    const char c = *p;
    if (c == '"' || c == '\\') {
      tmp[n++] = '\\';
      tmp[n++] = c;
    } else if ((uint8_t)c < 0x20) {
      n = (size_t)snprintf(tmp, sizeof(tmp), "\\u%04X", (unsigned)(uint8_t)c);
    } else {
      tmp[n++] = c;
    }
    if (off + n + 1 > cap) return cap; /* overflow: writer checks and fails */
    memcpy(dst + off, tmp, n);
    off += n;
  }
  if (off < cap) dst[off] = '\0';
  return off;
}

static inline bool json_skip_ws(const char **p) {
  while (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t') (*p)++;
  return true;
}

static inline bool json_expect(const char **p, char c) {
  json_skip_ws(p);
  if (**p != c) return false;
  (*p)++;
  return true;
}

/* Parse a quoted string into out (cap incl. NUL). Returns false on
 * malformed input or buffer overflow. */
static inline bool json_parse_string(const char **p, char *out, size_t cap) {
  if (!json_expect(p, '"')) return false;
  size_t n = 0;
  while (**p && **p != '"') {
    char c = *(*p)++;
    if (c == '\\') {
      const char e = *(*p)++;
      switch (e) {
        case '"': c = '"'; break;
        case '\\': c = '\\'; break;
        case '/': c = '/'; break;
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'u': {
          if ((*p)[0] && (*p)[1] && (*p)[2] && (*p)[3]) {
            unsigned v = 0;
            for (int i = 0; i < 4; i++) {
              const char h = (*p)[i];
              v = (v << 4) | (unsigned)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
            }
            *p += 4;
            if (n + 4 < cap) n += (size_t)snprintf(out + n, cap - n, "%u", v & 0xFF);
            continue;
          }
          return false;
        }
        default: return false;
      }
    }
    if (n + 1 >= cap) return false;
    out[n++] = c;
  }
  out[n] = '\0';
  return json_expect(p, '"');
}

static inline bool json_parse_uint(const char **p, uint32_t *out) {
  json_skip_ws(p);
  if (**p < '0' || **p > '9') return false;
  uint32_t v = 0;
  while (**p >= '0' && **p <= '9') {
    v = v * 10u + (uint32_t)(**p - '0');
    (*p)++;
  }
  *out = v;
  return true;
}

/* Reads "key": then the caller parses the value. Skips the comma that
 * separates members (the value parsers never consume it). */
static inline bool json_next_key(const char **p, char *key, size_t cap) {
  json_skip_ws(p);
  if (**p == '}' || **p == ']') return false; /* object end — caller checks */
  if (**p == ',') {
    (*p)++;
    json_skip_ws(p);
  }
  if (!json_parse_string(p, key, cap)) return false;
  return json_expect(p, ':');
}

/* --- image structs ---------------------------------------------------------------- */

struct BackupNodeImg {
  uint8_t id = 0;
  uint8_t mac[6] = {0};
  uint8_t key[32] = {0};
  uint8_t key_len = 0;
  uint8_t chan = 1;
  node_profile_t profile{};
  HubSched sched{};
};

struct BackupImage {
  char created[24] = {0};
  uint8_t tz = 0, stagger = 1;
  uint16_t sv = 1;
  uint8_t nnid = 1;
  uint32_t fid = 0;
  /* settings patch (PIN + Wi-Fi password deliberately absent) */
  uint8_t brightness = SET_BRIGHTNESS_DEF;
  uint16_t soff = SET_SCREEN_OFF_DEF;
  uint8_t ret = SET_RETENTION_DEF;
  uint8_t ntp = 1;
  char ssid[33] = {0};
  uint8_t node_count = 0;
  BackupNodeImg nodes[HUB_MAX_NODES];
};

/* --- writer ----------------------------------------------------------------------- */

/* Local timestamp "20261101-2114" for file names + the created field. */
static inline void backup_ts(int64_t unix_now, uint8_t tz_rule, char *out, size_t cap) {
  if (unix_now < 1600000000) {
    snprintf(out, cap, "unknown");
    return;
  }
  const LocalTime t = local_from_unix(unix_now, tz_rule);
  int64_t y;
  unsigned m, d;
  civil_from_days(t.day_start / 86400, &y, &m, &d);
  snprintf(out, cap, "%04lld%02u%02u-%02u%02u", (long long)y, m, d,
           (unsigned)(t.min_of_day / 60), (unsigned)(t.min_of_day % 60));
}

static inline void backup_default_name(int64_t unix_now, uint8_t tz_rule, char *out,
                                       size_t cap) {
  char ts[16];
  backup_ts(unix_now, tz_rule, ts, sizeof(ts)); /* 13 chars when valid */
  snprintf(out, cap, "aio-backup-%.13s.json", ts);
}

static inline void csv_default_name(int64_t unix_now, uint8_t tz_rule, char *out,
                                    size_t cap) {
  char ts[16];
  backup_ts(unix_now, tz_rule, ts, sizeof(ts)); /* 13 chars when valid */
  snprintf(out, cap, "aio-history-%.13s.csv", ts);
}

/* Serialize the registry + settings patch into buf. Returns false when the
 * buffer is too small (nothing is written in that case). */
static inline bool backup_json_write(const HubRegistry &reg, const HubSettings &set,
                                     int64_t unix_now, char *buf, size_t cap,
                                     char name_out[BACKUP_NAME_LEN],
                                     size_t *out_len) {
  backup_default_name(unix_now, reg.data().tz_rule, name_out, BACKUP_NAME_LEN);
  char ts[24];
  backup_ts(unix_now, reg.data().tz_rule, ts, sizeof(ts));
  size_t o = 0;
  char tmp[256];
  auto put = [&](const char *s) -> bool {
    const size_t n = strlen(s);
    if (o + n + 1 > cap) return false;
    memcpy(buf + o, s, n);
    o += n;
    buf[o] = '\0';
    return true;
  };
  auto put_uint = [&](const char *key, uint32_t v) {
    snprintf(tmp, sizeof(tmp), ",\"%s\":%lu", key, (unsigned long)v);
    return put(tmp);
  };

  if (!put("{\"aio_backup\":1,\"ver\":1,\"created\":\"")) return false;
  o = json_escape(buf, cap, o, ts);
  if (o >= cap) return false;
  if (!put("\",")) return false;
  snprintf(tmp, sizeof(tmp), "\"tz\":%u,\"stagger\":%u,\"sv\":%u,\"nnid\":%u",
           (unsigned)reg.data().tz_rule, (unsigned)reg.data().stagger_max,
           (unsigned)reg.data().sched_version, (unsigned)reg.data().next_node_id);
  if (!put(tmp)) return false;
  snprintf(tmp, sizeof(tmp), ",\"fid\":%lu", (unsigned long)reg.data().feed_id_seq);
  if (!put(tmp)) return false;
  /* settings patch — no PIN, no Wi-Fi password, no MQTT (D18/D32) */
  if (!put(",\"set\":{\"br\":")) return false;
  snprintf(tmp, sizeof(tmp), "%u,\"soff\":%u,\"ret\":%u,\"ntp\":%u,\"ssid\":\"",
           (unsigned)set.brightness, (unsigned)set.screen_off_s,
           (unsigned)set.history_days, (unsigned)set.ntp_on);
  if (!put(tmp)) return false;
  o = json_escape(buf, cap, o, set.wifi_ssid);
  if (o >= cap) return false;
  if (!put("\"}")) return false;
  if (!put(",\"nodes\":[")) return false;

  bool first = true;
  for (int i = 0; i < HUB_MAX_NODES; i++) {
    const HubNode *n = reg.node(i);
    if (!n || !n->id) continue;
    if (!first && !put(",")) return false;
    first = false;
    if (!put("{\"id\":")) return false;
    snprintf(tmp, sizeof(tmp), "%u,\"mac\":\"", (unsigned)n->id);
    if (!put(tmp)) return false;
    for (int b = 0; b < 6; b++) {
      snprintf(tmp, sizeof(tmp), "%02X", n->mac[b]);
      if (!put(tmp)) return false;
    }
    if (!put("\",\"key\":\"")) return false;
    for (int b = 0; b < n->key_len && b < 32; b++) {
      snprintf(tmp, sizeof(tmp), "%02X", n->key[b]);
      if (!put(tmp)) return false;
    }
    snprintf(tmp, sizeof(tmp), "\",\"klen\":%u,\"chan\":%u,\"p\":{\"name\":\"",
             (unsigned)n->key_len, (unsigned)n->channel);
    if (!put(tmp)) return false;
    o = json_escape(buf, cap, o, n->profile.name);
    if (o >= cap) return false;
    snprintf(tmp, sizeof(tmp),
             "\",\"mech\":%u,\"unit\":%u,\"cal\":%u,\"pon\":%u,\"poff\":%u,"
             "\"maxon\":%u,\"cool\":%u,\"maxh\":%lu,\"maxd\":%lu,\"flags\":%u,"
             "\"miss\":%u,\"cwin\":%u,\"cap\":%lu}",
             (unsigned)n->profile.mech, (unsigned)n->profile.unit,
             (unsigned)n->profile.cal_ms_per_unit_x100, (unsigned)n->profile.pulse_on_ms,
             (unsigned)n->profile.pulse_off_ms, (unsigned)n->profile.max_on_ms_single,
             (unsigned)n->profile.cooldown_ms,
             (unsigned long)n->profile.max_units_per_hour,
             (unsigned long)n->profile.max_units_per_day, (unsigned)n->profile.flags,
             (unsigned)n->profile.missed_policy, (unsigned)n->profile.catchup_window_min,
             (unsigned long)n->profile.storage_capacity);
    if (!put(tmp)) return false;
    snprintf(tmp, sizeof(tmp), ",\"s\":{\"en\":%u,\"n\":%u,\"e\":[",
             (unsigned)n->sched.enabled, (unsigned)n->sched.entry_count);
    if (!put(tmp)) return false;
    for (uint8_t e = 0; e < n->sched.entry_count && e < HUB_SCHED_MAX_ENTRIES; e++) {
      const sched_entry_t &se = n->sched.entries[e];
      snprintf(tmp, sizeof(tmp), "%s{\"m\":%u,\"d\":%u,\"a\":%u}", e ? "," : "",
               (unsigned)se.min_of_day, (unsigned)se.days_mask, (unsigned)se.amount);
      if (!put(tmp)) return false;
    }
    snprintf(tmp, sizeof(tmp), "],\"skip\":%lu,\"hold\":%lu}}",
             (unsigned long)n->sched.skip_until, (unsigned long)n->sched.hold_until);
    if (!put(tmp)) return false;
  }
  (void)put_uint; /* reserved for future top-level numerics */
  if (!put("]}")) return false;
  *out_len = o;
  return true;
}

/* --- parser ------------------------------------------------------------------------ */

static inline bool backup_parse_hex(const char **p, uint8_t *out, uint8_t bytes) {
  for (int b = 0; b < bytes; b++) {
    char hex[3] = {0};
    hex[0] = *(*p)++;
    hex[1] = *(*p)++;
    char *end = nullptr;
    const long v = strtol(hex, &end, 16);
    if (end != hex + 2) return false;
    out[b] = (uint8_t)v;
  }
  return true;
}

static inline bool backup_parse_node_obj(const char **p, BackupNodeImg *n) {
  char key[12];
  *n = BackupNodeImg{};
  if (!json_expect(p, '{')) return false;
  bool have_profile = false, have_sched = false;
  for (;;) {
    if (!json_next_key(p, key, sizeof(key))) break;
    if (!strcmp(key, "id")) {
      uint32_t v;
      if (!json_parse_uint(p, &v) || !v || v > 255) return false;
      n->id = (uint8_t)v;
    } else if (!strcmp(key, "mac")) {
      json_skip_ws(p);
      if (!json_expect(p, '"')) return false;
      if (!backup_parse_hex(p, n->mac, 6)) return false;
      if (!json_expect(p, '"')) return false;
    } else if (!strcmp(key, "key")) {
      json_skip_ws(p);
      if (!json_expect(p, '"')) return false;
      uint8_t raw[32];
      uint8_t cnt = 0;
      json_skip_ws(p);
      while (**p != '"') {
        if (cnt >= 32) return false;
        if (!backup_parse_hex(p, &raw[cnt], 1)) return false;
        cnt++;
      }
      if (!json_expect(p, '"')) return false;
      memcpy(n->key, raw, sizeof(raw));
      n->key_len = cnt;
    } else if (!strcmp(key, "klen")) {
      uint32_t v;
      if (!json_parse_uint(p, &v) || v > 32) return false;
      n->key_len = (uint8_t)v;
    } else if (!strcmp(key, "chan")) {
      uint32_t v;
      if (!json_parse_uint(p, &v) || !v || v > 13) return false;
      n->chan = (uint8_t)v;
    } else if (!strcmp(key, "p")) {
      if (!json_expect(p, '{')) return false;
      n->profile = profile_default();
      for (;;) {
        if (!json_next_key(p, key, sizeof(key))) break;
        uint32_t v;
        if (!strcmp(key, "name")) {
          if (!json_parse_string(p, n->profile.name, PROFILE_NAME_LEN)) return false;
        } else if (!strcmp(key, "mech")) {
          if (!json_parse_uint(p, &v) || v > 2) return false;
          n->profile.mech = (uint8_t)v;
        } else if (!strcmp(key, "unit")) {
          if (!json_parse_uint(p, &v) || v > 2) return false;
          n->profile.unit = (uint8_t)v;
        } else if (!strcmp(key, "cal")) {
          if (!json_parse_uint(p, &v)) return false;
          n->profile.cal_ms_per_unit_x100 = (uint16_t)v;
        } else if (!strcmp(key, "pon")) {
          if (!json_parse_uint(p, &v)) return false;
          n->profile.pulse_on_ms = (uint16_t)v;
        } else if (!strcmp(key, "poff")) {
          if (!json_parse_uint(p, &v)) return false;
          n->profile.pulse_off_ms = (uint16_t)v;
        } else if (!strcmp(key, "maxon")) {
          if (!json_parse_uint(p, &v)) return false;
          n->profile.max_on_ms_single = (uint16_t)v;
        } else if (!strcmp(key, "cool")) {
          if (!json_parse_uint(p, &v)) return false;
          n->profile.cooldown_ms = (uint16_t)v;
        } else if (!strcmp(key, "maxh")) {
          if (!json_parse_uint(p, &v)) return false;
          n->profile.max_units_per_hour = v;
        } else if (!strcmp(key, "maxd")) {
          if (!json_parse_uint(p, &v)) return false;
          n->profile.max_units_per_day = v;
        } else if (!strcmp(key, "flags")) {
          if (!json_parse_uint(p, &v)) return false;
          n->profile.flags = (uint8_t)v;
        } else if (!strcmp(key, "miss")) {
          if (!json_parse_uint(p, &v) || v > 1) return false;
          n->profile.missed_policy = (uint8_t)v;
        } else if (!strcmp(key, "cwin")) {
          if (!json_parse_uint(p, &v) || v > 255) return false;
          n->profile.catchup_window_min = (uint8_t)v;
        } else if (!strcmp(key, "cap")) {
          if (!json_parse_uint(p, &v)) return false;
          n->profile.storage_capacity = v;
        } else {
          return false; /* unknown profile key — refuse */
        }
      }
      if (!json_expect(p, '}')) return false;
      n->profile.version = PROFILE_VERSION;
      n->profile.reserved = 0;
      n->profile.crc16 = profile_crc16(&n->profile);
      have_profile = true;
    } else if (!strcmp(key, "s")) {
      if (!json_expect(p, '{')) return false;
      memset(&n->sched, 0, sizeof(n->sched));
      for (;;) {
        if (!json_next_key(p, key, sizeof(key))) break;
        if (!strcmp(key, "en")) {
          uint32_t v;
          if (!json_parse_uint(p, &v) || v > 1) return false;
          n->sched.enabled = (uint8_t)v;
        } else if (!strcmp(key, "n")) {
          uint32_t v;
          if (!json_parse_uint(p, &v) || v > HUB_SCHED_MAX_ENTRIES) return false;
          n->sched.entry_count = (uint8_t)v;
        } else if (!strcmp(key, "e")) {
          if (!json_expect(p, '[')) return false;
          uint8_t i = 0;
          if (!json_expect(p, ']')) {
            for (;;) {
              if (!json_expect(p, '{')) return false;
              uint32_t m = 0, d = 0, a = 0;
              for (;;) {
                if (!json_next_key(p, key, sizeof(key))) break;
                if (!strcmp(key, "m")) {
                  if (!json_parse_uint(p, &m) || m >= 1440) return false;
                } else if (!strcmp(key, "d")) {
                  if (!json_parse_uint(p, &d) || !d || (d & 0x7F) != d) return false;
                } else if (!strcmp(key, "a")) {
                  if (!json_parse_uint(p, &a) || !a) return false;
                } else {
                  return false;
                }
              }
              if (!json_expect(p, '}')) return false;
              if (i >= HUB_SCHED_MAX_ENTRIES) return false;
              n->sched.entries[i++] = {(uint16_t)m, (uint8_t)d, (uint16_t)a};
              json_skip_ws(p);
              if (**p == ',') {
                (*p)++;
                continue;
              }
              break;
            }
            if (!json_expect(p, ']')) return false;
          }
        } else if (!strcmp(key, "skip")) {
          uint32_t v;
          if (!json_parse_uint(p, &v)) return false;
          n->sched.skip_until = v;
        } else if (!strcmp(key, "hold")) {
          uint32_t v;
          if (!json_parse_uint(p, &v)) return false;
          n->sched.hold_until = v;
        } else {
          return false;
        }
      }
      if (!json_expect(p, '}')) return false;
      have_sched = true;
    } else {
      return false;
    }
  }
  if (!json_expect(p, '}')) return false;
  return n->id && have_profile && have_sched &&
         n->sched.entry_count <= HUB_SCHED_MAX_ENTRIES;
}

/* Strict parser for the format backup_json_write emits (unknown keys are
 * refused — a half-understood restore is worse than a failed one). */
static inline bool backup_json_parse(const char *json, BackupImage *img) {
  const char *p = json;
  *img = BackupImage{};
  char key[12];
  if (!json_expect(&p, '{')) return false;
  bool have_set = false, have_nodes = false;
  uint32_t ver = 0;
  for (;;) {
    if (!json_next_key(&p, key, sizeof(key))) break;
    if (!strcmp(key, "aio_backup")) {
      uint32_t v;
      if (!json_parse_uint(&p, &v) || v != 1) return false;
    } else if (!strcmp(key, "ver")) {
      if (!json_parse_uint(&p, &ver) || ver != 1) return false;
    } else if (!strcmp(key, "created")) {
      if (!json_parse_string(&p, img->created, sizeof(img->created))) return false;
    } else if (!strcmp(key, "tz")) {
      uint32_t v;
      if (!json_parse_uint(&p, &v) || v >= TZ_RULE_COUNT) return false;
      img->tz = (uint8_t)v;
    } else if (!strcmp(key, "stagger")) {
      uint32_t v;
      if (!json_parse_uint(&p, &v) || !v || v > HUB_MAX_NODES) return false;
      img->stagger = (uint8_t)v;
    } else if (!strcmp(key, "sv")) {
      uint32_t v;
      if (!json_parse_uint(&p, &v) || !v) return false;
      img->sv = (uint16_t)v;
    } else if (!strcmp(key, "nnid")) {
      uint32_t v;
      if (!json_parse_uint(&p, &v) || !v) return false;
      img->nnid = (uint8_t)v;
    } else if (!strcmp(key, "fid")) {
      if (!json_parse_uint(&p, &img->fid)) return false;
    } else if (!strcmp(key, "set")) {
      if (!json_expect(&p, '{')) return false;
      for (;;) {
        if (!json_next_key(&p, key, sizeof(key))) break;
        if (!strcmp(key, "br")) {
          uint32_t v;
          if (!json_parse_uint(&p, &v) || v < SET_BRIGHTNESS_MIN ||
              v > SET_BRIGHTNESS_MAX)
            return false;
          img->brightness = (uint8_t)v;
        } else if (!strcmp(key, "soff")) {
          uint32_t v;
          if (!json_parse_uint(&p, &v) || !set_screen_off_valid((uint16_t)v)) return false;
          img->soff = (uint16_t)v;
        } else if (!strcmp(key, "ret")) {
          uint32_t v;
          if (!json_parse_uint(&p, &v) || !set_retention_valid((uint8_t)v)) return false;
          img->ret = (uint8_t)v;
        } else if (!strcmp(key, "ntp")) {
          uint32_t v;
          if (!json_parse_uint(&p, &v) || v > 1) return false;
          img->ntp = (uint8_t)v;
        } else if (!strcmp(key, "ssid")) {
          if (!json_parse_string(&p, img->ssid, sizeof(img->ssid))) return false;
        } else {
          return false;
        }
      }
      if (!json_expect(&p, '}')) return false;
      have_set = true;
    } else if (!strcmp(key, "nodes")) {
      if (!json_expect(&p, '[')) return false;
      if (!json_expect(&p, ']')) {
        for (;;) {
          if (img->node_count >= HUB_MAX_NODES) return false;
          if (!backup_parse_node_obj(&p, &img->nodes[img->node_count])) return false;
          img->node_count++;
          json_skip_ws(&p);
          if (*p == ',') {
            p++;
            continue;
          }
          break;
        }
        if (!json_expect(&p, ']')) return false;
      }
      have_nodes = true;
    } else {
      return false;
    }
  }
  if (!json_expect(&p, '}')) return false;
  return ver == 1 && have_set && have_nodes;
}

/* --- restore (§9.12) ---------------------------------------------------------------- */

enum RestoreResult : uint8_t {
  RESTORE_OK = 0,
  RESTORE_BAD_JSON,
  RESTORE_BAD_IMAGE, /* structurally parsed but invalid */
};

static inline RestoreResult backup_restore_apply(BackupImage &img, HubRegistry &reg,
                                                 HubSettings &set) {
  /* settings patch — history, PIN, Wi-Fi password, MQTT untouched (§9.12) */
  set.brightness = img.brightness;
  set.screen_off_s = img.soff;
  set.history_days = img.ret;
  set.ntp_on = img.ntp;
  strncpy(set.wifi_ssid, img.ssid, sizeof(set.wifi_ssid) - 1);
  set.wifi_ssid[sizeof(set.wifi_ssid) - 1] = '\0';

  /* registry: fresh nodes block, versions bumped so everything re-pushes */
  uint8_t max_id = 0;
  for (int i = 0; i < img.node_count; i++)
    if (img.nodes[i].id > max_id) max_id = img.nodes[i].id;
  if (max_id > 0 && (uint16_t)(max_id + 1) > img.nnid) {
    /* nnid below an existing id would double-assign on the next pair */
    img.nnid = (uint8_t)(max_id + 1);
  }

  RegistryData d;
  memset(&d, 0, sizeof(d));
  d.version = REG_VERSION;
  d.count = img.node_count;
  d.next_node_id = img.nnid ? img.nnid : (uint8_t)(max_id + 1);
  d.sched_version = (uint16_t)(img.sv + 1); /* forces a SCHEDULE_SET re-push */
  d.tz_rule = img.tz;
  d.stagger_max = img.stagger ? img.stagger : 1;
  d.feed_id_seq = img.fid;
  for (int i = 0; i < img.node_count; i++) {
    const BackupNodeImg &src = img.nodes[i];
    HubNode &dst = d.nodes[i];
    dst.id = src.id;
    memcpy(dst.mac, src.mac, 6);
    memcpy(dst.key, src.key, 32);
    dst.key_len = src.key_len;
    dst.channel = src.chan;
    dst.profile = src.profile;
    dst.profile.crc16 = profile_crc16(&dst.profile);
    dst.sched = src.sched;
    dst.sched.version = d.sched_version;
    dst.sched.pushed_at = 0; /* "never pushed" → detail screen + re-push */
  }

  /* validate before commit: every profile must re-CRC valid, ids unique */
  for (int i = 0; i < d.count; i++) {
    if (!d.nodes[i].id || !profile_valid(&d.nodes[i].profile)) return RESTORE_BAD_IMAGE;
    for (int j = i + 1; j < d.count; j++)
      if (d.nodes[i].id == d.nodes[j].id) return RESTORE_BAD_IMAGE;
  }
  if (d.feed_id_seq < reg.data().feed_id_seq)
    d.feed_id_seq = reg.data().feed_id_seq; /* never re-issue feed ids (§6.6) */

  reg.data() = d;
  return RESTORE_OK;
}

/* --- orchestration (store + buffers owned by the caller) ---------------------------- */

struct BackupFileMeta {
  char name[BACKUP_NAME_LEN];
  uint32_t bytes;
};

static inline int backup_list(IBackupStore &st, BackupFileMeta *out, uint8_t max) {
  char names[BACKUP_SLOTS_MAX][BACKUP_NAME_LEN];
  const int n = st.list(names, max > BACKUP_SLOTS_MAX ? BACKUP_SLOTS_MAX : max);
  for (int i = 0; i < n; i++) {
    strncpy(out[i].name, names[i], BACKUP_NAME_LEN);
    out[i].name[BACKUP_NAME_LEN - 1] = '\0';
    out[i].bytes = 0;
  }
  return n;
}

/* backup_json_write → store.write. buf must hold the JSON (caller-owned,
 * typically heap on device). Returns false on overflow or write failure. */
static inline bool backup_write_now(IBackupStore &st, const HubRegistry &reg,
                                    const HubSettings &set, int64_t unix_now, char *buf,
                                    size_t cap, char name_out[BACKUP_NAME_LEN],
                                    size_t *out_len) {
  if (!st.present() && !st.mount()) return false;
  size_t len = 0;
  if (!backup_json_write(reg, set, unix_now, buf, cap, name_out, &len)) return false;
  if (!st.write(name_out, buf, len)) return false;
  *out_len = len;
  return true;
}

static inline RestoreResult backup_restore_now(IBackupStore &st, const char *name,
                                               HubRegistry &reg, HubSettings &set,
                                               char *buf, size_t cap) {
  const int len = st.read(name, buf, cap - 1);
  if (len <= 0) return RESTORE_BAD_JSON;
  buf[len] = '\0';
  BackupImage img;
  if (!backup_json_parse(buf, &img)) return RESTORE_BAD_JSON;
  return backup_restore_apply(img, reg, set);
}

/* --- CSV export (§9.12 columns: time,node,amount,unit,result,source) ----------------- */

static inline const char *csv_result_text(uint8_t kind, uint8_t result) {
  switch (kind) {
    case EV_FEED_OK: return "OK";
    case EV_FEED_FAIL: return result == EVENT_RESULT_NO_LINK ? "NO-LINK" : "FAIL";
    case EV_FEED_REJ: {
      static char buf[12];
      snprintf(buf, sizeof(buf), "REJ:%u", (unsigned)result);
      return buf;
    }
    case EV_FEED_UNKNOWN: return "NO-LINK";
    case EV_SKIPPED_USER: return "SKIP-USER";
    case EV_SKIPPED_HOLD: return "SKIP-HOLD";
    case EV_SKIPPED_MISSED: return "MISSED";
    case EV_CATCHUP: return "CATCHUP";
    default: return "?";
  }
}

/* RFC-4180: quote when the field contains comma, quote, CR or LF; double
 * embedded quotes ("CSV quoting of node names containing commas or quotes",
 * §11.1). Returns the advanced offset. */
static inline size_t csv_field(char *dst, size_t cap, size_t off, const char *s) {
  const bool needs_quote = strpbrk(s, ",\"\r\n") != nullptr;
  char tmp[4];
  if (!needs_quote) {
    const size_t n = strlen(s);
    if (off + n + 1 > cap) return cap;
    memcpy(dst + off, s, n);
    return off + n;
  }
  if (off + 1 > cap) return cap;
  dst[off++] = '"';
  for (const char *p = s; *p; p++) {
    size_t n = 0;
    if (*p == '"') {
      tmp[n++] = '"';
      tmp[n++] = '"';
    } else {
      tmp[n++] = *p;
    }
    if (off + n + 1 > cap) return cap;
    memcpy(dst + off, tmp, n);
    off += n;
  }
  if (off + 1 > cap) return cap;
  dst[off++] = '"';
  return off;
}

static inline bool csv_history_write(const HubRegistry &reg, const EventLog &log,
                                     int64_t unix_now, char *buf, size_t cap,
                                     char name_out[BACKUP_NAME_LEN], size_t *out_len) {
  csv_default_name(unix_now, reg.data().tz_rule, name_out, BACKUP_NAME_LEN);
  size_t o = 0;
  if (o + 40 > cap) return false;
  o += (size_t)snprintf(buf + o, cap - o, "time,node,amount,unit,result,source\n");
  if (unix_now < 1600000000) return false;

  for (uint16_t i = 0; i < log.count(); i++) {
    const HubEvent *e = log.at(i);
    if (o + 64 > cap) return false; /* need room for the row prefix */
    const LocalTime t = local_from_unix((int64_t)e->unix, reg.data().tz_rule);
    int64_t y;
    unsigned m, d;
    civil_from_days(t.day_start / 86400, &y, &m, &d);
    o += (size_t)snprintf(buf + o, cap - o, "%04lld-%02u-%02u %02u:%02u,",
                          (long long)y, m, d, (unsigned)(t.min_of_day / 60),
                          (unsigned)(t.min_of_day % 60));
    char nbuf[PROFILE_NAME_LEN + 16];
    const HubNode *n = reg.by_id(e->node);
    if (n)
      snprintf(nbuf, sizeof(nbuf), "%s", n->profile.name);
    else
      snprintf(nbuf, sizeof(nbuf), "node%u", (unsigned)e->node);
    o = csv_field(buf, cap, o, nbuf);
    if (o >= cap) return false;
    const HubNode *pn = reg.by_id(e->node);
    o += (size_t)snprintf(buf + o, cap - o, ",%u,%s,%s,%s\n", (unsigned)e->amount,
                          pn && pn->profile.unit == UNIT_ML ? "ml" : "g",
                          csv_result_text(e->kind, e->result), ev_src_name(e->src));
    if (o >= cap) return false;
  }
  *out_len = o;
  return true;
}

static inline bool csv_export_now(IBackupStore &st, const HubRegistry &reg,
                                  const EventLog &log, int64_t unix_now, char *buf,
                                  size_t cap, char name_out[BACKUP_NAME_LEN],
                                  size_t *out_len) {
  if (!st.present() && !st.mount()) return false;
  size_t len = 0;
  if (!csv_history_write(reg, log, unix_now, buf, cap, name_out, &len)) return false;
  if (!st.write(name_out, buf, len)) return false;
  *out_len = len;
  return true;
}

}  // namespace aio

#endif /* AIO_BACKUP_H */
