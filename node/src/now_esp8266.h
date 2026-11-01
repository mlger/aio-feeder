/*
 * AIO Feeder — ESP-NOW transport for ESP8266 (PLAN.md §6.1, §7.1).
 * Wraps the SDK behind aio::ILinkTransport; frames are queued from the SDK
 * callback (WiFi task context) and drained by the main loop — the link
 * engine never runs in callback context.
 */
#ifndef AIO_NOW_ESP8266_H
#define AIO_NOW_ESP8266_H

#include <Arduino.h>
#include <ESP8266WiFi.h>
extern "C" {
#include <espnow.h>
#include <user_interface.h>
}

#include "../../link_core/link.h"

/* Station MAC for the §6.3 pairing code derivation (works pre-begin). */
static inline void now_sta_mac(uint8_t out[6]) {
  wifi_get_macaddr(STATION_IF, out);
}

constexpr size_t AIO_NOW_MAX_FRAME = FRAME_HEADER_SIZE + FRAME_MAX_PAYLOAD + FRAME_MAC_SIZE;
constexpr uint8_t AIO_NOW_RX_RING = 8;

struct NowRx {
  uint8_t mac[6];
  uint8_t len;
  uint8_t data[AIO_NOW_MAX_FRAME];
};

static volatile uint8_t g_now_rx_head = 0;
static volatile uint8_t g_now_rx_tail = 0;
static NowRx g_now_rx_ring[AIO_NOW_RX_RING];

static void IRAM_ATTR now_recv_cb_8266(uint8_t *mac, uint8_t *data, uint8_t len) {
  uint8_t next = (uint8_t)((g_now_rx_head + 1) % AIO_NOW_RX_RING);
  if (next == g_now_rx_tail || len > AIO_NOW_MAX_FRAME) return; /* full / too big */
  NowRx &slot = g_now_rx_ring[g_now_rx_head];
  memcpy(slot.mac, mac, 6);
  memcpy(slot.data, data, len);
  slot.len = len;
  g_now_rx_head = next;
}

struct NowTransport8266 : public aio::ILinkTransport {
  uint8_t channel_ = 1;

  bool begin(uint8_t channel) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false);
    wifi_set_channel(channel);
    channel_ = channel;
    if (esp_now_init() != 0) return false;
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_register_recv_cb(now_recv_cb_8266);
    /* broadcast peer so HELLO/HELLO_ACK can flow both ways */
    ensure_peer(aio::LINK_BROADCAST);
    return true;
  }

  /* ESP8266 requires a registered peer (MAC + channel) before sending. */
  void ensure_peer(const uint8_t mac[6]) {
    uint8_t *m = const_cast<uint8_t *>(mac);
    if (esp_now_is_peer_exist(m)) esp_now_del_peer(m);
    esp_now_add_peer(m, ESP_NOW_ROLE_COMBO, channel_, nullptr, 0);
  }

  void set_channel(uint8_t channel) override {
    channel_ = channel;
    wifi_set_channel(channel);
    /* peers are channel-bound on this SDK: re-register them */
    ensure_peer(aio::LINK_BROADCAST);
  }

  uint8_t channel() const override { return channel_; }

  bool send(const uint8_t mac[6], const uint8_t *data, size_t len) override {
    if (len > AIO_NOW_MAX_FRAME) return false;
    ensure_peer(mac);
    return esp_now_send(const_cast<uint8_t *>(mac), const_cast<uint8_t *>(data),
                        (uint8_t)len) == 0;
  }

  /* Drain the callback ring into the engine; call once per loop(). */
  void pump(aio::LinkEngine &engine, uint32_t now_ms) {
    while (g_now_rx_tail != g_now_rx_head) {
      const NowRx &slot = g_now_rx_ring[g_now_rx_tail];
      engine.on_rx(slot.mac, slot.data, slot.len, 0 /* no RSSI on this SDK */, now_ms);
      g_now_rx_tail = (uint8_t)((g_now_rx_tail + 1) % AIO_NOW_RX_RING);
    }
  }
};

#endif /* AIO_NOW_ESP8266_H */
