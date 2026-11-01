/*
 * AIO Feeder — HAL v1 for ESP8266 (Wemos D1 mini class; PLAN.md §4.4, §7.1).
 *
 * Relay GPIO14 (D5, boot-safe), I²C on GPIO4/5, status LED GPIO2 (active-low),
 * ADC A0 for the supply monitor. The relay-off backstop is hardware timer1:
 * its ISR clears GPIO14 directly and outlives a stalled main loop (SR2, SR1).
 */
#ifndef AIO_HAL_ESP8266_H
#define AIO_HAL_ESP8266_H

#include <Arduino.h>
#include <EEPROM.h>
#include <Wire.h>
#include "ds3231.h"

#define HAL_RELAY_PIN 14    /* D5 — boot-safe [VERIFY vs your module] */
#define HAL_RELAY_ACTIVE_HIGH 1 /* relay module input network [VERIFY] */
#define HAL_SDA_PIN 4       /* D2 */
#define HAL_SCL_PIN 5       /* D1 */
#define HAL_LED_PIN 2       /* D4, onboard, active-low */
#define HAL_VIN_ADC_MAX 1023.0
#define HAL_VIN_REF_MV 3300.0 /* D1 mini divider tops at 3.3 V [VERIFY] */

static volatile bool g_relay_off_fired = false;

void IRAM_ATTR hal_relay_off_isr() {
  /* force the relay pin LOW straight from the timer ISR (SR2) */
  if (HAL_RELAY_ACTIVE_HIGH)
    GPIO_REG_WRITE(GPIO_OUT_W1TC_ADDRESS, 1UL << HAL_RELAY_PIN);
  else
    GPIO_REG_WRITE(GPIO_OUT_W1TS_ADDRESS, 1UL << HAL_RELAY_PIN);
  g_relay_off_fired = true;
}

struct Hal {
  void init() {
    /* SR1 first thing: relay OFF before anything else runs */
    pinMode(HAL_RELAY_PIN, OUTPUT);
    relay(false);
    pinMode(HAL_LED_PIN, OUTPUT);
    led(false);
    EEPROM.begin(kStorageRegion);
    Wire.begin(HAL_SDA_PIN, HAL_SCL_PIN);
    timer1_isr_init();
    timer1_attachInterrupt(hal_relay_off_isr);
    timer1_enable(TIM_DIV256, TIM_EDGE, TIM_SINGLE_LOOP);
    ESP.wdtEnable(0); /* software WDT feeds alongside the always-on HW WDT */
  }

  void relay(bool on) {
    digitalWrite(HAL_RELAY_PIN, on == HAL_RELAY_ACTIVE_HIGH ? HIGH : LOW);
  }

  /* Arm the relay-off backstop; max single-loop ≈ 26.8 s at 3.2 µs/tick. */
  void arm_relay_off(uint32_t ms) {
    g_relay_off_fired = false;
    timer1_write(ms * 312UL); /* 80 MHz / 256 = 3.2 µs per tick */
  }
  void cancel_relay_off() { timer1_disable(); timer1_enable(TIM_DIV256, TIM_EDGE, TIM_SINGLE_LOOP); }
  bool relay_off_fired() { return g_relay_off_fired; }

  void feed_wdt() { ESP.wdtFeed(); }
  void stall_for_wdt_test() { ESP.wdtDisable(); for (;;) {} } /* HW WDT resets */

  void led(bool on) { digitalWrite(HAL_LED_PIN, on ? LOW : HIGH); } /* active-low */

  /* Supply monitor: returns millivolts at the ADC pin, or 0 when unplugged. */
  uint16_t vin_mv() {
    uint32_t raw = analogRead(A0);
    return (uint16_t)(raw * HAL_VIN_REF_MV / HAL_VIN_ADC_MAX);
  }

  bool rtc_healthy(int64_t *unix_out, bool *osf) {
    RtcReading r = rtc_read();
    *osf = r.osf;
    if (!r.present || r.osf || !r.range_ok) return false;
    *unix_out = rtc_make_unix(r.t);
    return true;
  }
  bool rtc_write_unix(int64_t unix) { return rtc_write_time(rtc_from_unix(unix)); }

  /* Storage: whole A/B region as one buffer (EEPROM is RAM-backed + commit). */
  bool storage_read(uint8_t *buf, size_t len) {
    if (len > kStorageRegion) return false;
    for (size_t i = 0; i < len; i++) buf[i] = EEPROM.read(i);
    return true;
  }
  bool storage_write(const uint8_t *buf, size_t len) {
    if (len > kStorageRegion) return false;
    for (size_t i = 0; i < len; i++) EEPROM.write(i, buf[i]);
    return EEPROM.commit(); /* flash erase+write; debounced by the caller */
  }
};

#endif /* AIO_HAL_ESP8266_H */
