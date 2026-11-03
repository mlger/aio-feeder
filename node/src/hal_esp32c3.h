/*
 * AIO Feeder — HAL v1 for ESP32-C3 SuperMini (PLAN.md §4.5, §7.1).
 *
 * Relay GPIO3, I²C on GPIO6/7, status LED GPIO8 (active-low), supply monitor
 * on GPIO1 (ADC1). Relay-off backstop = hardware timer; its ISR clears the
 * relay pin directly and outlives a stalled main loop (SR2, SR1).
 *
 * arduino-esp32 2.0.x timer API (espressif32 platform 6.x).
 */
#ifndef AIO_HAL_ESP32C3_H
#define AIO_HAL_ESP32C3_H

#include <Arduino.h>
#include <soc/gpio_struct.h>
#include <Preferences.h>
#include <Wire.h>
#include <WiFi.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include "ds3231.h"

#define HAL_RELAY_PIN 3     /* not a strapping pin [VERIFY vs silkscreen] */
#define HAL_RELAY_ACTIVE_HIGH 1 /* relay module input network [VERIFY] */
#define HAL_SDA_PIN 6
#define HAL_SCL_PIN 7
#define HAL_LED_PIN 8       /* onboard, active-low; strapping pin — LED load ok */
#define HAL_VIN_PIN 1       /* ADC1; external divider 12 V → ≤ 2.5 V [VERIFY] */
/* M7 optional inputs (FMEA #2/#8/#17; PLAN §4.2, SR9). All [VERIFY] vs rig. */
#define HAL_STOP_PIN 2      /* SR9 physical stop switch, pullup, active-low */
#define HAL_HOPPER_PIN 4    /* hopper-low sensor, pullup, active-low */
#define HAL_GATE_PIN 5      /* gate reed to GND, pullup: LOW = closed */

static volatile bool g_relay_off_fired = false;
static hw_timer_t *g_timer = nullptr;

void IRAM_ATTR hal_relay_off_isr() {
  if (HAL_RELAY_ACTIVE_HIGH)
    GPIO.out_w1tc.val = 1UL << HAL_RELAY_PIN;
  else
    GPIO.out_w1ts.val = 1UL << HAL_RELAY_PIN;
  g_relay_off_fired = true;
}

struct Hal {
  Preferences prefs;

  void init() {
    /* SR1 first thing: relay OFF before anything else runs */
    pinMode(HAL_RELAY_PIN, OUTPUT);
    relay(false);
    pinMode(HAL_LED_PIN, OUTPUT);
    led(false);
    pinMode(HAL_STOP_PIN, INPUT_PULLUP);
    pinMode(HAL_HOPPER_PIN, INPUT_PULLUP);
    pinMode(HAL_GATE_PIN, INPUT_PULLUP);
    prefs.begin("aio-cfg", false);
    Wire.begin(HAL_SDA_PIN, HAL_SCL_PIN);
    g_timer = timerBegin(0, 80, true); /* 1 MHz, up-counter */
    timerAttachInterrupt(g_timer, &hal_relay_off_isr, true);
    enableLoopWDT();
  }

  void relay(bool on) {
    digitalWrite(HAL_RELAY_PIN, on == HAL_RELAY_ACTIVE_HIGH ? HIGH : LOW);
  }

  void arm_relay_off(uint32_t ms) {
    g_relay_off_fired = false;
    timerWrite(g_timer, 0);
    timerAlarmWrite(g_timer, ms * 1000, false); /* µs, single shot */
    timerAlarmEnable(g_timer);
  }
  void cancel_relay_off() { timerAlarmDisable(g_timer); }
  bool relay_off_fired() { return g_relay_off_fired; }

  void feed_wdt() { feedLoopWDT(); }
  void stall_for_wdt_test() { for (;;) {} } /* loop WDT resets */
  uint32_t random32() { return esp_random(); }

  void led(bool on) { digitalWrite(HAL_LED_PIN, on ? LOW : HIGH); } /* active-low */

  /* M7 inputs: stop switch (SR9), hopper-low sensor (§4.2), gate reed (FMEA #2). */
  bool stop_active() { return digitalRead(HAL_STOP_PIN) == LOW; }
  bool hopper_low() { return digitalRead(HAL_HOPPER_PIN) == LOW; }
  bool gate_closed() { return digitalRead(HAL_GATE_PIN) == LOW; }

  /* Millivolts at the divider tap × 1 (scale set by the external divider). */
  uint16_t vin_mv() { return (uint16_t)analogReadMilliVolts(HAL_VIN_PIN); }

  bool rtc_healthy(int64_t *unix_out, bool *osf) {
    RtcReading r = rtc_read();
    *osf = r.osf;
    if (!r.present || r.osf || !r.range_ok) return false;
    *unix_out = rtc_make_unix(r.t);
    return true;
  }
  bool rtc_write_unix(int64_t unix) { return rtc_write_time(rtc_from_unix(unix)); }

  bool storage_read(uint8_t *buf, size_t len) {
    return prefs.getBytes("cfg", buf, len) == len;
  }
  bool storage_write(const uint8_t *buf, size_t len) {
    return prefs.putBytes("cfg", buf, len) == len;
  }

  /* --- M8b node OTA (§7.6): join the hub's Wi-Fi, stream an image ---------- */
  bool wifi_join(const char *ssid, const char *pass, uint32_t timeout_ms) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
      if (millis() - t0 > timeout_ms) return false;
      delay(50);
      feed_wdt();
    }
    return true;
  }
  void wifi_off() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  bool ota_begin(size_t len) { return Update.begin(len ? len : UPDATE_SIZE_UNKNOWN); }
  size_t ota_write(const uint8_t *buf, size_t len) { return Update.write((uint8_t *)buf, len); }
  bool ota_end() { return Update.end(true); }
  void ota_abort() { Update.abort(); }
  /* C3 bootloader rollback (FMEA #11 parity with the hub, §7.6): confirm the
   * new slot once setup completed — a build that never gets here rolls back. */
  void ota_mark_valid() {
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY)
      esp_ota_mark_app_valid_cancel_rollback();
  }
  void reboot() { ESP.restart(); }
};

#endif /* AIO_HAL_ESP32C3_H */
