/*
 * AIO Feeder — link engine (PLAN.md §6.2, §6.4–§6.6, §12 M2).
 *
 * Transport-agnostic, host-testable: all I/O goes through ILinkTransport
 * (ESP-NOW shims on the MCUs, a loopback/mock in the host tests), all time
 * comes in as milliseconds. One source of truth for both roles:
 *
 *   Hub: sends FEED with the retry policy (3 resends at 300 ms / 1 s / 3 s,
 *        then UNKNOWN + alert — never a blind re-issue, §6.6); receives
 *        HELLO (answers HELLO_ACK), FEED_ACK, FEED_RESULT (seq-deduped),
 *        HEARTBEAT, SCHEDULE/CONFIG_ACK, PAIR_REQ.
 *   Node: answers FEED (journal dedup by feed_id, SR6 — duplicates are
 *        answered, never actuated twice), repeats FEED_RESULT 3×, sends
 *        HEARTBEAT every 30 s, scans channels 1–13 when the hub is silent
 *        for 60 s and locks the channel that answers HELLO_ACK (§6.2).
 *
 * Security (§6.4, D13): every non-discovery frame carries a truncated
 * HMAC-SHA256 (8 bytes) over header+payload with the per-peer key, plus a
 * per-sender monotonic seq with a 32-entry replay window. Discovery frames
 * (HELLO, HELLO_ACK, PAIR_REQ, PAIR_ACCEPT) use the all-zero key.
 *
 * Wire order: little-endian on every supported ABI (x86-64 host, Xtensa,
 * RISC-V). Header fields are serialized explicitly; payloads are packed
 * structs copied verbatim.
 */
#ifndef AIO_LINK_H
#define AIO_LINK_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../lib/proto/messages.h"
#include "../lib/proto/profile.h"
#include "../lib/proto/proto.h"

namespace aio {

constexpr uint8_t LINK_BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr uint8_t LINK_HUB_ID = 0;   /* hub is node_id 0 on the wire */
constexpr size_t LINK_KEY_LEN = 32;
constexpr size_t LINK_MAX_PEERS = 20; /* ESP-NOW unencrypted limit [VERIFY on both SDKs] */

/* §6.6 retry policy and §6.2 scan parameters. */
constexpr uint32_t FEED_RETRY_DELAYS_MS[3] = {300, 1000, 3000};
constexpr uint32_t RESULT_REPEAT_MS = 250;   /* node repeats RESULT 3× total */
constexpr uint32_t HB_PERIOD_MS = 30000;
constexpr uint32_t LINK_LOST_MS = 60000;     /* hub silence → channel scan */
constexpr uint32_t SCAN_HOP_MS = 250;
constexpr uint32_t UNPAIRED_HELLO_MS = 5000; /* unpaired beacon for discovery */

struct ILinkTransport {
  virtual bool send(const uint8_t mac[6], const uint8_t *data, size_t len) = 0;
  virtual void set_channel(uint8_t channel) = 0;
  virtual uint8_t channel() const = 0;
  virtual ~ILinkTransport() {}
};

struct LinkStats {
  uint32_t tx = 0, tx_fail = 0;
  uint32_t rx_ok = 0, rx_dup = 0, rx_replay = 0;
  uint32_t rx_bad_len = 0, rx_bad_ver = 0, rx_bad_mac = 0, rx_no_peer = 0;
  uint32_t feed_retries = 0, feed_unknown = 0, results_deduped = 0;
  uint32_t test_rx = 0; /* MSG_TARE_OR_CAL used as a bench loss-test marker */
};

struct LinkPeer {
  uint8_t mac[6] = {};
  uint8_t node_id = 0;
  uint8_t key[LINK_KEY_LEN] = {};
  uint8_t key_len = 0;          /* 0 = key not provisioned yet */
  uint16_t tx_seq = 0;          /* per-sender monotonic (persisted by runtime) */
  bool have_rx = false;         /* seq window primed */
  uint16_t rx_last = 0;
  uint32_t rx_win = 0;          /* bit i = seq (rx_last - i - 1) seen */
  uint32_t last_rx_ms = 0;
  int8_t last_rssi = 0;
  uint8_t last_flags = 0; /* frame flags byte of the last valid frame */
  heartbeat_payload_t last_hb{};
  uint32_t last_hb_ms = 0;
};

struct LinkHandlers {
  /* hub side */
  void (*on_hello)(const uint8_t mac[6], const hello_payload_t &pl, int8_t rssi) = nullptr;
  void (*on_feed_ack)(uint8_t node_id, const feed_ack_payload_t &pl) = nullptr;
  void (*on_feed_result)(uint8_t node_id, const feed_result_payload_t &pl) = nullptr;
  void (*on_heartbeat)(uint8_t node_id, const heartbeat_payload_t &pl, int8_t rssi) = nullptr;
  void (*on_pair_req)(const uint8_t mac[6], const pair_req_payload_t &pl, int8_t rssi) = nullptr;
  void (*on_feed_unknown)(uint32_t feed_id, uint8_t node_id) = nullptr;
  void (*on_schedule_ack)(uint8_t node_id, const ack_status_payload_t &pl) = nullptr;
  void (*on_config_ack)(uint8_t node_id, const ack_status_payload_t &pl) = nullptr;
  /* node side */
  void (*on_feed)(const uint8_t mac[6], const feed_payload_t &pl) = nullptr;
  void (*on_time_sync)(const time_sync_payload_t &pl) = nullptr;
  void (*on_schedule_set)(const schedule_hdr_t &hdr, const sched_entry_t *entries) = nullptr;
  void (*on_config)(const node_profile_t &p, uint8_t limits_confirmed) = nullptr;
  void (*on_status_req)(void) = nullptr;
  void (*on_abort)(const abort_payload_t &pl) = nullptr;
  /* node lifecycle */
  void (*hb_fill)(heartbeat_payload_t *pl) = nullptr;
  void (*on_channel_locked)(uint8_t channel) = nullptr;
  void (*on_link_up)(void) = nullptr;
  void (*on_link_lost)(void) = nullptr;
};

class LinkEngine {
 public:
  enum class Role : uint8_t { Hub, Node };

  void begin(Role role, ILinkTransport *transport, const LinkHandlers &handlers,
             uint32_t now_ms) {
    role_ = role;
    transport_ = transport;
    handlers_ = handlers;
    channel_ = transport ? transport->channel() : 1;
    bcast_seq_ = 0;
    pending_ = PendingFeed{};
    result_q_ = ResultQ{};
    scanning_ = false;
    scan_idx_ = 0;
    hop_next_ms_ = 0;
    last_hb_tx_ms_ = now_ms - HB_PERIOD_MS + 2000;   /* first HB 2 s after boot */
    last_hello_ms_ = now_ms;
    last_hub_rx_ms_ = now_ms - LINK_LOST_MS + 10000; /* scan after 10 s grace */
  }

  /* --- peers ------------------------------------------------------------ */
  LinkPeer *add_peer(const uint8_t mac[6], uint8_t node_id, const uint8_t *key,
                     uint8_t key_len) {
    LinkPeer *p = peer_by_mac(mac);
    if (!p) {
      if (peer_count_ >= LINK_MAX_PEERS) return nullptr;
      p = &peers_[peer_count_++];
    }
    memcpy(p->mac, mac, 6);
    p->node_id = node_id;
    set_key(p, key, key_len);
    return p;
  }
  void clear_peers() { peer_count_ = 0; }
  bool remove_peer(const uint8_t mac[6]) {
    for (uint8_t i = 0; i < peer_count_; i++) {
      if (!memcmp(peers_[i].mac, mac, 6)) {
        for (uint8_t j = i; j + 1 < peer_count_; j++) peers_[j] = peers_[j + 1];
        peer_count_--;
        return true;
      }
    }
    return false;
  }
  uint8_t peer_count() const { return peer_count_; }
  LinkPeer *peer_by_index(uint8_t i) { return i < peer_count_ ? &peers_[i] : nullptr; }
  LinkPeer *peer_by_node(uint8_t node_id) {
    for (uint8_t i = 0; i < peer_count_; i++)
      if (peers_[i].node_id == node_id) return &peers_[i];
    return nullptr;
  }
  LinkPeer *peer_by_mac(const uint8_t mac[6]) {
    for (uint8_t i = 0; i < peer_count_; i++)
      if (!memcmp(peers_[i].mac, mac, 6)) return &peers_[i];
    return nullptr;
  }
  /* Node role: the single hub peer. */
  LinkPeer *hub_peer() { return peer_count_ ? &peers_[0] : nullptr; }
  bool has_peer() const { return peer_count_ > 0; }
  void set_key(LinkPeer *p, const uint8_t *key, uint8_t key_len) {
    if (!p) return;
    memset(p->key, 0, LINK_KEY_LEN);
    if (key && key_len) {
      memcpy(p->key, key, key_len > LINK_KEY_LEN ? LINK_KEY_LEN : key_len);
      p->key_len = key_len > LINK_KEY_LEN ? LINK_KEY_LEN : key_len;
    }
  }

  /* --- state ------------------------------------------------------------ */
  const LinkStats &stats() const { return stats_; }
  bool scanning() const { return scanning_; }
  uint8_t channel() const { return channel_; }
  /* Hub bench/CLI: change the operating channel (HELLO_ACK advertises it). */
  void set_channel(uint8_t ch) { channel_ = ch; }
  bool linked(uint32_t now_ms) const {
    return role_ == Role::Hub || (has_peer() && !scanning_ &&
                                  (uint32_t)(now_ms - last_hub_rx_ms_) <= LINK_LOST_MS);
  }
  void force_scan(uint32_t now_ms) {
    if (role_ != Role::Node || !has_peer() || scanning_) return;
    start_scan(now_ms);
  }
  void set_self(uint8_t self_node_id, uint8_t beacon_state) {
    self_node_id_ = self_node_id;
    beacon_state_ = beacon_state;
  }
  /* Node runtime bumps the hub peer's tx_seq into config before persisting. */
  void restore_tx_seq(uint16_t seq) {
    if (peer_count_) peers_[0].tx_seq = seq;
  }
  uint16_t current_tx_seq() const { return peer_count_ ? peers_[0].tx_seq : 0; }

  /* --- TX: hub role ------------------------------------------------------ */
  /* FEED with the §6.6 retry policy; one in-flight feed (§9.5 stagger).
   * Retries reuse the same seq so the receiver sees them as duplicates. */
  bool send_feed(uint8_t node_id, const feed_payload_t &f, uint32_t now_ms) {
    if (role_ != Role::Hub || pending_.active) return false;
    LinkPeer *p = peer_by_node(node_id);
    if (!p || !p->key_len) return false;
    uint16_t seq = p->tx_seq++;
    if (!emit_raw(p->mac, p->key, MSG_FEED, node_id, seq, &f, sizeof(f), now_ms)) return false;
    pending_.active = true;
    pending_.feed_id = f.feed_id;
    pending_.node_id = node_id;
    pending_.seq = seq;
    pending_.attempts = 1;
    pending_.next_ms = now_ms + FEED_RETRY_DELAYS_MS[0];
    memcpy(pending_.payload, &f, sizeof(f));
    return true;
  }
  bool send_time_sync(uint8_t node_id, const time_sync_payload_t &pl, uint32_t now_ms) {
    return hub_send(node_id, MSG_TIME_SYNC, &pl, sizeof(pl), now_ms);
  }
  bool send_schedule_set(uint8_t node_id, uint16_t version, const sched_entry_t *entries,
                         uint8_t count, uint32_t skip_until, uint32_t hold_until,
                         uint32_t now_ms) {
    if (count > SCHEDULE_MAX_ENTRIES) return false;
    schedule_hdr_t hdr{version, count, skip_until, hold_until};
    uint8_t buf[sizeof(hdr) + (size_t)SCHEDULE_MAX_ENTRIES * sizeof(sched_entry_t)];
    memcpy(buf, &hdr, sizeof(hdr));
    if (count) memcpy(buf + sizeof(hdr), entries, (size_t)count * sizeof(sched_entry_t));
    return hub_send(node_id, MSG_SCHEDULE_SET, buf,
                    sizeof(hdr) + (size_t)count * sizeof(sched_entry_t), now_ms);
  }
  bool send_config(uint8_t node_id, const node_profile_t &p, uint8_t limits_confirmed,
                   uint32_t now_ms) {
    uint8_t buf[sizeof(node_profile_t) + 1];
    memcpy(buf, &p, sizeof(p));
    buf[sizeof(node_profile_t)] = limits_confirmed;
    return hub_send(node_id, MSG_CONFIG, buf, sizeof(buf), now_ms);
  }
  bool send_status_req(uint8_t node_id, uint32_t now_ms) {
    return hub_send(node_id, MSG_STATUS_REQ, nullptr, 0, now_ms);
  }
  bool send_abort(uint8_t node_id, uint32_t feed_id, uint32_t now_ms) {
    abort_payload_t pl{feed_id};
    return hub_send(node_id, MSG_ABORT, &pl, sizeof(pl), now_ms);
  }

  /* --- TX: node role ----------------------------------------------------- */
  bool send_feed_ack(uint32_t feed_id, uint8_t accepted, uint8_t reason, uint32_t now_ms) {
    feed_ack_payload_t pl{feed_id, accepted, reason};
    return node_send(MSG_FEED_ACK, &pl, sizeof(pl), now_ms);
  }
  /* RESULT: sent now, repeated up to 3× total (§6.6). Repeats reuse the
   * same seq — they are retransmissions, not new results. */
  bool send_feed_result(uint32_t feed_id, uint8_t status, uint16_t actual_on_ms,
                        uint16_t pulses, uint8_t gate_ok, uint32_t t_unix, uint32_t now_ms) {
    LinkPeer *hp = hub_peer();
    if (!hp || !hp->key_len) return false;
    result_q_.active = true;
    result_q_.pl = feed_result_payload_t{feed_id, status, actual_on_ms, pulses, gate_ok, t_unix};
    result_q_.seq = hp->tx_seq++;
    result_q_.sent = 0;
    result_q_.next_ms = now_ms;
    return true;
  }
  bool send_heartbeat(const heartbeat_payload_t &pl, uint32_t now_ms,
                      uint8_t frame_flags = 0) {
    if (!node_send_f(MSG_HEARTBEAT, &pl, sizeof(pl), now_ms, frame_flags)) return false;
    last_hb_tx_ms_ = now_ms;
    return true;
  }
  bool send_hello(uint32_t now_ms) {
    hello_payload_t pl{AIO_FW_VER, 0, beacon_state_};
    return emit_raw(LINK_BROADCAST, AIO_ZERO_KEY, MSG_HELLO, self_node_id_, bcast_seq_++,
                    &pl, sizeof(pl), now_ms);
  }
  /* Bench loss test: keyed frame on the normal path (§11.2). */
  bool send_test(uint32_t counter, uint32_t now_ms) {
    return node_send(MSG_TARE_OR_CAL, &counter, sizeof(counter), now_ms);
  }
  bool send_schedule_ack(uint16_t version, uint8_t ok, uint32_t now_ms) {
    ack_status_payload_t pl{version, ok};
    return node_send(MSG_SCHEDULE_ACK, &pl, sizeof(pl), now_ms);
  }
  bool send_config_ack(uint16_t profile_crc, uint8_t ok, uint32_t now_ms) {
    ack_status_payload_t pl{profile_crc, ok};
    return node_send(MSG_CONFIG_ACK, &pl, sizeof(pl), now_ms);
  }

  /* --- RX ----------------------------------------------------------------- */
  void on_rx(const uint8_t src[6], const uint8_t *data, size_t len, int8_t rssi,
             uint32_t now_ms) {
    if (len < FRAME_HEADER_SIZE + FRAME_MAC_SIZE || len > sizeof(frame_t)) {
      stats_.rx_bad_len++;
      return;
    }
    uint8_t type = data[1];
    if (data[0] != PROTO_VER || type >= MSG_TYPE_COUNT) {
      stats_.rx_bad_ver++;
      return;
    }
    uint16_t plen = (uint16_t)(data[6] | (data[7] << 8));
    if ((size_t)plen + FRAME_HEADER_SIZE + FRAME_MAC_SIZE != len) {
      stats_.rx_bad_len++;
      return;
    }
    const uint8_t *key = AIO_ZERO_KEY;
    LinkPeer *peer = nullptr;
    if (!is_discovery(type)) {
      peer = peer_by_mac(src);
      if (!peer || !peer->key_len) {
        stats_.rx_no_peer++;
        return;
      }
      key = peer->key;
    }
    /* truncated HMAC over header+payload (§6.4) */
    uint8_t mac_calc[8];
    frame_compute_mac(key, data, FRAME_HEADER_SIZE + plen, mac_calc);
    if (!frame_mac_equal(mac_calc, data + FRAME_HEADER_SIZE + plen)) {
      stats_.rx_bad_mac++;
      return;
    }
    uint16_t seq = (uint16_t)(data[4] | (data[5] << 8));
    const uint8_t *pl = data + FRAME_HEADER_SIZE;

    if (peer) {
      if (!peer->have_rx) {
        peer->have_rx = true;
        peer->rx_last = seq;
        peer->rx_win = 0;
      } else {
        uint16_t delta = (uint16_t)(seq - peer->rx_last);
        bool seen;
        if (delta == 0) {
          seen = true;
        } else if (delta < 0x8000) {
          seen = false; /* ahead → fresh (lost frames leave gaps) */
        } else {
          uint16_t behind = (uint16_t)(0x10000u - delta);
          if (behind <= 31)
            seen = (peer->rx_win >> (behind - 1)) & 1u;
          else
            seen = true; /* ancient → replay (§6.4) */
        }
        if (seen) {
          /* A retried FEED (same seq) must still be answered from the
           * journal when the first ACK was lost (§6.6) — never actuated
           * twice: the journal guarantees that (SR6). Every other duplicate
           * is dropped here. */
          if (role_ == Role::Node && type == MSG_FEED && handlers_.on_feed) {
            feed_payload_t f;
            memcpy(&f, pl, sizeof(f));
            handlers_.on_feed(src, f);
          } else {
            uint16_t behind = (uint16_t)(0x10000u - delta);
            if (delta != 0 && behind > 31)
              stats_.rx_replay++;
            else
              stats_.rx_dup++;
            if (type == MSG_FEED_RESULT) stats_.results_deduped++;
          }
          peer->last_rx_ms = now_ms;
          peer->last_rssi = rssi;
          return;
        }
        seq_advance(peer, seq);
      }
      peer->last_rx_ms = now_ms;
      peer->last_rssi = rssi;
      peer->last_flags = data[3];
      last_hub_rx_ms_ = now_ms;
      if (scanning_ && role_ == Role::Node) lock_channel();
    }

    switch (type) {
      case MSG_HELLO: {
        if (plen < (int)sizeof(hello_payload_t)) { stats_.rx_bad_len++; return; }
        hello_payload_t h;
        memcpy(&h, pl, sizeof(h));
        if (role_ == Role::Hub) {
          if (handlers_.on_hello) handlers_.on_hello(src, h, rssi);
          hello_ack_payload_t ack{0, 0, channel_}; /* assignment is M3 pairing */
          emit_raw(src, AIO_ZERO_KEY, MSG_HELLO_ACK, self_node_id_, bcast_seq_++, &ack,
                   sizeof(ack), now_ms);
        }
        break;
      }
      case MSG_HELLO_ACK: {
        if (plen < (int)sizeof(hello_ack_payload_t)) { stats_.rx_bad_len++; return; }
        hello_ack_payload_t ack;
        memcpy(&ack, pl, sizeof(ack));
        if (role_ == Role::Node) {
          last_hub_rx_ms_ = now_ms;
          if (ack.channel >= 1 && ack.channel <= 13 && ack.channel != channel_) {
            channel_ = ack.channel;
            if (transport_) transport_->set_channel(channel_);
          }
          lock_channel();
        }
        break;
      }
      case MSG_FEED: {
        if (plen < (int)sizeof(feed_payload_t)) { stats_.rx_bad_len++; return; }
        feed_payload_t f;
        memcpy(&f, pl, sizeof(f));
        if (role_ == Role::Node && handlers_.on_feed) handlers_.on_feed(src, f);
        break;
      }
      case MSG_FEED_ACK: {
        if (plen < (int)sizeof(feed_ack_payload_t)) { stats_.rx_bad_len++; return; }
        feed_ack_payload_t a;
        memcpy(&a, pl, sizeof(a));
        if (role_ == Role::Hub) {
          if (pending_.active && pending_.feed_id == a.feed_id) pending_ = PendingFeed{};
          if (handlers_.on_feed_ack) handlers_.on_feed_ack(data[2], a);
        }
        break;
      }
      case MSG_FEED_RESULT: {
        if (plen < (int)sizeof(feed_result_payload_t)) { stats_.rx_bad_len++; return; }
        feed_result_payload_t r;
        memcpy(&r, pl, sizeof(r));
        if (role_ == Role::Hub) {
          if (pending_.active && pending_.feed_id == r.feed_id) pending_ = PendingFeed{};
          if (handlers_.on_feed_result) handlers_.on_feed_result(data[2], r);
        }
        break;
      }
      case MSG_HEARTBEAT: {
        if (plen < (int)sizeof(heartbeat_payload_t)) { stats_.rx_bad_len++; return; }
        heartbeat_payload_t hb;
        memcpy(&hb, pl, sizeof(hb));
        if (role_ == Role::Hub && peer) {
          peer->last_hb = hb;
          peer->last_hb_ms = now_ms;
          if (handlers_.on_heartbeat) handlers_.on_heartbeat(data[2], hb, rssi);
        }
        break;
      }
      case MSG_TIME_SYNC: {
        if (plen < (int)sizeof(time_sync_payload_t)) { stats_.rx_bad_len++; return; }
        time_sync_payload_t t;
        memcpy(&t, pl, sizeof(t));
        if (role_ == Role::Node && handlers_.on_time_sync) handlers_.on_time_sync(t);
        break;
      }
      case MSG_SCHEDULE_SET: {
        if (plen < (int)sizeof(schedule_hdr_t)) { stats_.rx_bad_len++; return; }
        schedule_hdr_t hdr;
        memcpy(&hdr, pl, sizeof(hdr));
        if (hdr.entry_count > SCHEDULE_MAX_ENTRIES ||
            plen < (int)(sizeof(hdr) + (size_t)hdr.entry_count * sizeof(sched_entry_t))) {
          stats_.rx_bad_len++;
          return;
        }
        if (role_ == Role::Node && handlers_.on_schedule_set)
          handlers_.on_schedule_set(hdr, (const sched_entry_t *)(pl + sizeof(hdr)));
        break;
      }
      case MSG_CONFIG: {
        if (plen < (int)(sizeof(node_profile_t) + 1)) { stats_.rx_bad_len++; return; }
        node_profile_t p;
        memcpy(&p, pl, sizeof(p));
        if (role_ == Role::Node && handlers_.on_config)
          handlers_.on_config(p, pl[sizeof(node_profile_t)]);
        break;
      }
      case MSG_STATUS_REQ:
        if (role_ == Role::Node && handlers_.on_status_req) handlers_.on_status_req();
        break;
      case MSG_ABORT: {
        if (plen < (int)sizeof(abort_payload_t)) { stats_.rx_bad_len++; return; }
        abort_payload_t a;
        memcpy(&a, pl, sizeof(a));
        if (role_ == Role::Node && handlers_.on_abort) handlers_.on_abort(a);
        break;
      }
      case MSG_SCHEDULE_ACK:
      case MSG_CONFIG_ACK: {
        if (plen < (int)sizeof(ack_status_payload_t)) { stats_.rx_bad_len++; return; }
        ack_status_payload_t a;
        memcpy(&a, pl, sizeof(a));
        if (role_ == Role::Hub && type == MSG_SCHEDULE_ACK && handlers_.on_schedule_ack)
          handlers_.on_schedule_ack(data[2], a);
        if (role_ == Role::Hub && type == MSG_CONFIG_ACK && handlers_.on_config_ack)
          handlers_.on_config_ack(data[2], a);
        break;
      }
      case MSG_PAIR_REQ: {
        if (plen < (int)sizeof(pair_req_payload_t)) { stats_.rx_bad_len++; return; }
        pair_req_payload_t pr;
        memcpy(&pr, pl, sizeof(pr));
        if (role_ == Role::Hub && handlers_.on_pair_req) handlers_.on_pair_req(src, pr, rssi);
        break;
      }
      case MSG_TARE_OR_CAL:
        if (role_ == Role::Hub) stats_.test_rx++; /* bench loss-test marker */
        break;
      default:
        break;
    }
    stats_.rx_ok++;
  }

  /* --- periodic ----------------------------------------------------------- */
  void tick(uint32_t now_ms) {
    /* hub: FEED retry policy (§6.6) — original + 3 resends, then UNKNOWN */
    if (role_ == Role::Hub && pending_.active && (int32_t)(now_ms - pending_.next_ms) >= 0) {
      if (pending_.attempts > FEED_RETRY_DELAYS_MS_COUNT) {
        PendingFeed done = pending_;
        pending_ = PendingFeed{};
        stats_.feed_unknown++;
        if (handlers_.on_feed_unknown) handlers_.on_feed_unknown(done.feed_id, done.node_id);
      } else {
        LinkPeer *p = peer_by_node(pending_.node_id);
        if (p && p->key_len) {
          emit_raw(p->mac, p->key, MSG_FEED, pending_.node_id, pending_.seq, pending_.payload,
                   sizeof(feed_payload_t), now_ms);
          stats_.feed_retries++;
        }
        pending_.attempts++;
        pending_.next_ms = now_ms + FEED_RETRY_DELAYS_MS[pending_.attempts - 2];
      }
    }

    if (role_ != Role::Node) return;

    /* node: RESULT repeats */
    if (result_q_.active && (int32_t)(now_ms - result_q_.next_ms) >= 0) {
      LinkPeer *hp = hub_peer();
      if (hp && hp->key_len) {
        emit_raw(hp->mac, hp->key, MSG_FEED_RESULT, self_node_id_, result_q_.seq,
                 &result_q_.pl, sizeof(result_q_.pl), now_ms);
        result_q_.sent++;
        result_q_.next_ms = now_ms + RESULT_REPEAT_MS;
        if (result_q_.sent >= 3) result_q_ = ResultQ{};
      } else {
        result_q_ = ResultQ{}; /* no hub peer: nowhere to send */
      }
    }

    if (!has_peer()) {
      /* unpaired: beacon so a hub can discover us (pairing UI is M3) */
      if (now_ms - last_hello_ms_ >= UNPAIRED_HELLO_MS) {
        send_hello(now_ms);
        last_hello_ms_ = now_ms;
      }
      return;
    }

    /* node: channel scan when the hub went quiet (§6.2) — hops 1..13 */
    if (scanning_) {
      if ((int32_t)(now_ms - hop_next_ms_) >= 0) {
        hop_next_ms_ = now_ms + SCAN_HOP_MS;
        uint8_t ch = (uint8_t)(scan_idx_ + 1);
        scan_idx_ = (uint8_t)((scan_idx_ + 1) % 13);
        channel_ = ch;
        if (transport_) transport_->set_channel(ch);
        send_hello(now_ms);
      }
      return;
    }

    if ((uint32_t)(now_ms - last_hub_rx_ms_) > LINK_LOST_MS) {
      start_scan(now_ms);
      return;
    }

    /* node: heartbeat every HB_PERIOD_MS (first one 2 s after boot) */
    if ((uint32_t)(now_ms - last_hb_tx_ms_) >= HB_PERIOD_MS) {
      heartbeat_payload_t hb{};
      if (handlers_.hb_fill) handlers_.hb_fill(&hb);
      send_heartbeat(hb, now_ms);
    }
  }

  /* Test/bench introspection. */
  bool feed_pending() const { return pending_.active; }
  uint32_t pending_feed_id() const { return pending_.feed_id; }

 private:
  enum { FEED_RETRY_DELAYS_MS_COUNT = 3 };

  struct PendingFeed {
    bool active = false;
    uint32_t feed_id = 0;
    uint8_t node_id = 0;
    uint16_t seq = 0;
    uint8_t attempts = 0;
    uint32_t next_ms = 0;
    uint8_t payload[sizeof(feed_payload_t)] = {};
  };
  struct ResultQ {
    bool active = false;
    feed_result_payload_t pl{};
    uint16_t seq = 0;
    uint8_t sent = 0;
    uint32_t next_ms = 0;
  };

  static bool is_discovery(uint8_t type) {
    return type == MSG_HELLO || type == MSG_HELLO_ACK || type == MSG_PAIR_REQ ||
           type == MSG_PAIR_ACCEPT;
  }

  static void seq_advance(LinkPeer *p, uint16_t seq) {
    uint16_t delta = (uint16_t)(seq - p->rx_last);
    if (delta == 0) return;
    if (delta >= 32) {
      p->rx_win = 0;
    } else {
      p->rx_win = (p->rx_win << delta) | (1u << (delta - 1));
    }
    p->rx_last = seq;
  }

  void lock_channel() {
    if (!scanning_) return;
    scanning_ = false;
    if (handlers_.on_channel_locked) handlers_.on_channel_locked(channel_);
  }

  void start_scan(uint32_t now_ms) {
    scanning_ = true;
    scan_idx_ = 0;
    hop_next_ms_ = now_ms;
    if (handlers_.on_link_lost) handlers_.on_link_lost();
  }

  bool hub_send(uint8_t node_id, uint8_t type, const void *pl, size_t plen, uint32_t now_ms) {
    LinkPeer *p = peer_by_node(node_id);
    if (!p || !p->key_len) return false;
    return emit(p->mac, p->key, type, node_id, pl, plen, now_ms);
  }
  bool node_send(uint8_t type, const void *pl, size_t plen, uint32_t now_ms) {
    return node_send_f(type, pl, plen, now_ms, 0);
  }
  bool node_send_f(uint8_t type, const void *pl, size_t plen, uint32_t now_ms,
                   uint8_t frame_flags) {
    LinkPeer *hp = hub_peer();
    if (!hp || !hp->key_len) return false;
    uint16_t seq = hp->tx_seq++;
    return emit_raw(hp->mac, hp->key, type, self_node_id_, seq, pl, plen, now_ms,
                    frame_flags);
  }

  bool emit(const uint8_t dst[6], const uint8_t key[LINK_KEY_LEN], uint8_t type,
            uint8_t node_id_field, const void *pl, size_t plen, uint32_t now_ms) {
    if (plen > FRAME_MAX_PAYLOAD) return false;
    uint16_t seq;
    if (!memcmp(dst, LINK_BROADCAST, 6)) {
      seq = bcast_seq_++;
    } else {
      LinkPeer *p = peer_by_mac(dst);
      if (!p) return false;
      seq = p->tx_seq++;
    }
    return emit_raw(dst, key, type, node_id_field, seq, pl, plen, now_ms);
  }

  bool emit_raw(const uint8_t dst[6], const uint8_t key[LINK_KEY_LEN], uint8_t type,
                uint8_t node_id_field, uint16_t seq, const void *pl, size_t plen,
                uint32_t now_ms, uint8_t frame_flags = 0) {
    (void)now_ms;
    uint8_t buf[FRAME_HEADER_SIZE + FRAME_MAX_PAYLOAD + FRAME_MAC_SIZE];
    buf[0] = PROTO_VER;
    buf[1] = type;
    buf[2] = node_id_field;
    buf[3] = frame_flags; /* bit0: HB is a STATUS_REQ reply (stops the ack loop) */
    buf[4] = (uint8_t)(seq & 0xFF);
    buf[5] = (uint8_t)(seq >> 8);
    buf[6] = (uint8_t)(plen & 0xFF);
    buf[7] = (uint8_t)(plen >> 8);
    if (plen) memcpy(buf + FRAME_HEADER_SIZE, pl, plen);
    frame_compute_mac(key, buf, FRAME_HEADER_SIZE + plen, buf + FRAME_HEADER_SIZE + plen);
    stats_.tx++;
    if (!transport_ || !transport_->send(dst, buf, FRAME_HEADER_SIZE + plen + FRAME_MAC_SIZE)) {
      stats_.tx_fail++;
      return false;
    }
    return true;
  }

  Role role_ = Role::Hub;
  ILinkTransport *transport_ = nullptr;
  LinkHandlers handlers_{};
  LinkStats stats_{};
  LinkPeer peers_[LINK_MAX_PEERS]{};
  uint8_t peer_count_ = 0;
  uint8_t channel_ = 1;
  uint8_t self_node_id_ = 0;
  uint8_t beacon_state_ = 0; /* HELLO.state: 0 unpaired, 1 paired */
  uint16_t bcast_seq_ = 0;
  PendingFeed pending_{};
  ResultQ result_q_{};
  bool scanning_ = false;
  uint8_t scan_idx_ = 0;
  uint32_t hop_next_ms_ = 0;
  uint32_t last_hb_tx_ms_ = 0;
  uint32_t last_hello_ms_ = 0;
  uint32_t last_hub_rx_ms_ = 0;
};

}  // namespace aio

#endif /* AIO_LINK_H */
