/*
 * AIO Feeder — ESP-NOW transport for ESP32 (Arduino core 2.x / IDF 4.4;
 * node ESP32-C3 SuperMini and the ESP32-S3 hub; PLAN.md §6.1, §7.1).
 * Mirrors node/src/now_esp8266.h: SDK callback → ring buffer → main loop.
 */
#ifndef AIO_NOW_ESP32_H
#define AIO_NOW_ESP32_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "../../link_core/link.h"

/* Station MAC for the §6.3 pairing code derivation (efuse — pre-begin OK). */
static inline void now_sta_mac(uint8_t out[6]) {
  esp_read_mac(out, ESP_MAC_WIFI_STA);
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

static void now_recv_cb_esp32(const uint8_t *mac, const uint8_t *data, int len) {
  uint8_t next = (uint8_t)((g_now_rx_head + 1) % AIO_NOW_RX_RING);
  if (next == g_now_rx_tail || len <= 0 || (size_t)len > AIO_NOW_MAX_FRAME) return;
  NowRx &slot = g_now_rx_ring[g_now_rx_head];
  memcpy(slot.mac, mac, 6);
  memcpy(slot.data, data, (size_t)len);
  slot.len = (uint8_t)len;
  g_now_rx_head = next;
}

struct NowTransportEsp32 : public aio::ILinkTransport {
  uint8_t channel_ = 1;

  bool begin(uint8_t channel) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false);
    channel_ = channel;
    set_channel(channel);
    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(now_recv_cb_esp32);
    ensure_peer(aio::LINK_BROADCAST);
    return true;
  }

  /* M8b OTA: release the radio for Wi-Fi; begin() re-arms after a failed pull. */
  void end() { esp_now_deinit(); }

  /* IDF 4.4 refuses esp_now_send to an unregistered peer. */
  void ensure_peer(const uint8_t mac[6]) {
    if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
    esp_now_peer_info_t info{};
    memcpy(info.peer_addr, mac, 6);
    info.channel = channel_;
    info.ifidx = WIFI_IF_STA;
    info.encrypt = false;
    esp_now_add_peer(&info);
  }

  void set_channel(uint8_t channel) override {
    channel_ = channel;
    /* channel change on a started STA needs the promiscuous dance */
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    ensure_peer(aio::LINK_BROADCAST); /* peers are channel-bound */
  }

  uint8_t channel() const override { return channel_; }

  bool send(const uint8_t mac[6], const uint8_t *data, size_t len) override {
    if (len > AIO_NOW_MAX_FRAME) return false;
    ensure_peer(mac);
    return esp_now_send(mac, data, (int)len) == ESP_OK;
  }

  void pump(aio::LinkEngine &engine, uint32_t now_ms) {
    while (g_now_rx_tail != g_now_rx_head) {
      const NowRx &slot = g_now_rx_ring[g_now_rx_tail];
      engine.on_rx(slot.mac, slot.data, slot.len, 0 /* no RSSI in this cb */, now_ms);
      g_now_rx_tail = (uint8_t)((g_now_rx_tail + 1) % AIO_NOW_RX_RING);
    }
  }
};

#endif /* AIO_NOW_ESP32_H */
