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
#include "ds3231.h"

#define HAL_RELAY_PIN 3     /* not a strapping pin [VERIFY vs silkscreen] */
#define HAL_RELAY_ACTIVE_HIGH 1 /* relay module input network [VERIFY] */
#define HAL_SDA_PIN 6
#define HAL_SCL_PIN 7
#define HAL_LED_PIN 8       /* onboard, active-low; strapping pin — LED load ok */
#define HAL_VIN_PIN 1       /* ADC1; external divider 12 V → ≤ 2.5 V [VERIFY] */

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
};

#endif /* AIO_HAL_ESP32C3_H */
