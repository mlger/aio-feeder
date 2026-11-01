/*
 * AIO Feeder — ESP-NOW transport + radio bring-up for ESP-IDF v5.x
 * (hub ESP32-S3; PLAN.md §6.1, §7.1, decision D6 = pure ESP-IDF).
 *
 * Replaces the M0 Arduino shim (hub/src/now_esp32.h): SDK callback →
 * ring buffer → main loop, RSSI from the RX control block. Wi-Fi STA
 * bring-up lives here too (ESP-NOW needs the radio); when CONFIG_AIO_WIFI_*
 * credentials are set the hub also joins the AP and enables SNTP (§7.4).
 *
 * On QEMU the Wi-Fi peripheral is not emulated — app_main prints its boot
 * markers before radio_begin() so the CI boot smoke never depends on it.
 */
#ifndef AIO_NOWESPNOW_H
#define AIO_NOWESPNOW_H

#include <esp_event.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_now.h>
#include <esp_sntp.h>
#include <esp_wifi.h>

#include <cstring>

#include "../../link_core/link.h"

constexpr size_t AIO_NOW_MAX_FRAME = FRAME_HEADER_SIZE + FRAME_MAX_PAYLOAD + FRAME_MAC_SIZE;
constexpr uint8_t AIO_NOW_RX_RING = 8;

struct NowRx {
  uint8_t mac[6];
  int8_t rssi;
  uint8_t len;
  uint8_t data[AIO_NOW_MAX_FRAME];
};

static volatile uint8_t g_now_rx_head = 0;
static volatile uint8_t g_now_rx_tail = 0;
static NowRx g_now_rx_ring[AIO_NOW_RX_RING];

/* IDF ≥5.0 recv callback signature. Runs in the WiFi task: never block. */
static void now_recv_cb_idf(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!info || !data || len <= 0 || (size_t)len > AIO_NOW_MAX_FRAME) return;
  const uint8_t next = (uint8_t)((g_now_rx_head + 1) % AIO_NOW_RX_RING);
  if (next == g_now_rx_tail) return; /* ring full: drop newest */
  NowRx &slot = g_now_rx_ring[g_now_rx_head];
  memcpy(slot.mac, info->src_addr, 6);
  slot.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
  memcpy(slot.data, data, (size_t)len);
  slot.len = (uint8_t)len;
  g_now_rx_head = next;
}

struct NowTransportIdf : public aio::ILinkTransport {
  uint8_t channel_ = 1;
  bool wifi_up_ = false;

  /* One-time radio bring-up: STA mode + ESP-NOW (+ optional AP join/SNTP). */
  bool begin(uint8_t channel) {
    channel_ = channel;
    if (!wifi_up_) {
      esp_netif_init(); /* harmless without AP credentials */
      esp_event_loop_create_default();
      wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
      if (esp_wifi_init(&cfg) != ESP_OK) return false;
      if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
#if defined(CONFIG_AIO_WIFI_SSID) && CONFIG_AIO_WIFI_SSID[0]
      {
        wifi_config_t wc{};
        strncpy((char *)wc.sta.ssid, CONFIG_AIO_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
        strncpy((char *)wc.sta.password, CONFIG_AIO_WIFI_PASS, sizeof(wc.sta.password) - 1);
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (esp_wifi_start() != ESP_OK) return false;
        /* SNTP once the STA is up; IP arrival completes the sync in background */
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.nist.gov");
        esp_sntp_init();
      }
#else
      if (esp_wifi_start() != ESP_OK) return false;
#endif
      /* ESP-NOW latency matters (FEED retries are 300 ms spaced) */
      esp_wifi_set_ps(WIFI_PS_NONE);
      wifi_up_ = true;
    }
    set_channel(channel);
    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(now_recv_cb_idf);
    ensure_peer(aio::LINK_BROADCAST);
    return true;
  }

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
      engine.on_rx(slot.mac, slot.data, slot.len, slot.rssi, now_ms);
      g_now_rx_tail = (uint8_t)((g_now_rx_tail + 1) % AIO_NOW_RX_RING);
    }
  }

  uint8_t mac_of(uint8_t out[6]) const {
    return esp_wifi_get_mac(WIFI_IF_STA, out) == ESP_OK ? 0 : -1;
  }
};

#endif /* AIO_NOWESPNOW_H */
