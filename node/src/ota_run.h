/*
 * AIO Feeder — node OTA download flow (M8b, PLAN §7.6, FMEA #11).
 *
 * Shared glue for both MCU targets: join the hub's Wi-Fi, stream the image
 * through the HAL into the Update partition while hashing it, verify the
 * pinned sha256 BEFORE the reboot, and report why it failed. The caller
 * (main.cpp) owns the persisted lock lifecycle around this — a false return
 * leaves the node ota-locked with the radio re-armed for the link engine.
 *
 * TLS is deliberately not trusted here (D39): certificates on these parts
 * are unmanageable, and integrity comes from the digest pin that rode in
 * the HMAC-wrapped ENTER_OTA frame. https URLs still encrypt in transit.
 */
#ifndef AIO_OTA_RUN_H
#define AIO_OTA_RUN_H

#include <Arduino.h>
#include <HTTPClient.h>
#if defined(AIO_NODE_8266)
#include <WiFiClient.h>
#include <WiFiClientSecureBearSSL.h>
#else
#include <WiFiClientSecure.h>
#endif

#include "../../lib/proto/sha256.h"

#include "node_config.h"

/* C3 slot size (partitions_c3_ota.csv); 8266 caps itself via free sketch
 * space inside Update.begin. One cap keeps the branch count down. */
#if defined(AIO_NODE_8266)
constexpr size_t AIO_OTA_MAX_IMAGE = 1024u * 1024u;
#else
constexpr size_t AIO_OTA_MAX_IMAGE = 0x180000u;
#endif

struct OtaRunResult {
  bool ok;
  uint8_t reason; /* ota_ack_reason_t — what the node would have answered */
};

/* Why the branch: the 8266 HTTPClient wants a client object handed in; the
 * C3 core builds one from the URL. Both stream identically via getStreamPtr(). */
static inline bool ota_http_begin(HTTPClient &http, const char *url) {
#if defined(AIO_NODE_8266)
  static WiFiClient plain;
  static BearSSL::WiFiClientSecure tls;
  if (!strncmp(url, "https:", 6)) {
    tls.setInsecure(); /* digest pin is the trust root (header comment) */
    tls.setBufferSizes(512, 512);
    return http.begin(tls, url);
  }
  return http.begin(plain, url);
#else
  if (!strncmp(url, "https:", 6)) {
    static WiFiClientSecure tls;
    tls.setInsecure();
    return http.begin(tls, url);
  }
  return http.begin(url);
#endif
}

/* Stream one opened HTTP response into the Update partition, hashing as we
 * go. Returns true only when the full image is verified and finalized. */
static inline bool ota_stream(Hal &hal, HTTPClient &http, int total, const uint8_t *sha_pin) {
  if (total > 0 && (size_t)total > AIO_OTA_MAX_IMAGE) {
    Serial.printf("OTA: image %d too big\n", total);
    return false;
  }
  if (!hal.ota_begin(total > 0 ? (size_t)total : 0)) {
    Serial.println("OTA: Update.begin failed");
    return false;
  }

  sha256_ctx ctx;
  sha256_init(&ctx);
  uint8_t buf[512];
  size_t got = 0;
  bool io_ok = true;
  WiFiClient *stream = http.getStreamPtr();
  const uint32_t t0 = millis();
  while (http.connected() && (total < 0 || got < (size_t)total)) {
    const int avail = stream->available();
    if (avail > 0) {
      const size_t n = stream->readBytes(buf, (size_t)avail < sizeof(buf) ? (size_t)avail : sizeof(buf));
      if (!n || hal.ota_write(buf, n) != n) { io_ok = false; break; }
      sha256_update(&ctx, buf, n);
      got += n;
      hal.feed_wdt();
    } else if (millis() - t0 > 60000) {
      io_ok = false; /* stalled 60 s */
      break;
    }
  }
  if (!io_ok || (total > 0 && got != (size_t)total)) {
    Serial.printf("OTA: short/unreadable stream (%u/%d)\n", (unsigned)got, total);
    hal.ota_abort();
    return false;
  }

  uint8_t digest[SHA256_DIGEST];
  sha256_final(&ctx, digest);
  if (sha_pin && memcmp(digest, sha_pin, SHA256_DIGEST) != 0) {
    Serial.println("OTA: sha256 MISMATCH — image refused");
    hal.ota_abort();
    return false;
  }
  if (!hal.ota_end()) {
    Serial.println("OTA: Update.end failed");
    hal.ota_abort();
    return false;
  }
  Serial.printf("OTA: flashed %u bytes\n", (unsigned)got);
  return true;
}

/* Blocks for the whole download (minutes on real Wi-Fi) — the loop WDT is
 * fed inside; nothing else runs. On failure the node keeps its ota lock. */
static inline OtaRunResult ota_run(Hal &hal, const char *ssid, const char *pass,
                                   const char *url, const uint8_t *sha_pin) {
  OtaRunResult r{false, OTA_ACK_REJ_BAD};
  Serial.printf("OTA: joining %s\n", ssid);
  if (!hal.wifi_join(ssid, pass, 30000)) {
    Serial.println("OTA: Wi-Fi join failed");
    return r;
  }
  Serial.printf("OTA: wifi up, ip %s\n", WiFi.localIP().toString().c_str());

  HTTPClient http;
  if (ota_http_begin(http, url) && http.GET() == HTTP_CODE_OK) {
    const bool flashed = ota_stream(hal, http, http.getSize(), sha_pin);
    http.end();
    if (flashed) {
      r.ok = true;
      return r; /* caller persists stage DONE, then reboots */
    }
  } else {
    http.end();
    Serial.println("OTA: http GET failed");
  }
  hal.wifi_off();
  return r;
}

#endif /* AIO_OTA_RUN_H */
