/*
 * AIO Feeder — node profile (PLAN.md §5).
 * Stored on the hub, mirrored to the node (CONFIG); one firmware image, the
 * profile decides behaviour. CRC16 covers all fields except crc16 itself.
 */
#ifndef AIO_PROFILE_H
#define AIO_PROFILE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "crc16.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILE_VERSION 1
#define PROFILE_NAME_LEN 24

typedef enum { MECH_SOLENOID_GATE = 0, MECH_MOTOR_ONEWAY, MECH_PUMP } mech_t;
typedef enum { UNIT_PULSES = 0, UNIT_GRAMS, UNIT_ML } unit_t;

typedef struct {
  uint8_t   version;
  char      name[PROFILE_NAME_LEN];
  uint8_t   mech;                    /* mech_t */
  uint8_t   unit;                    /* unit_t */
  uint16_t  cal_ms_per_unit_x100;    /* open-time per unit (fixed-point), or per pulse */
  uint16_t  pulse_on_ms;             /* pulse-count mode */
  uint16_t  pulse_off_ms;
  uint16_t  max_on_ms_single;        /* hard cap per actuation (SR2) */
  uint16_t  cooldown_ms;             /* min gap between actuations (SR4) */
  uint32_t  max_units_per_hour;      /* SR3 */
  uint32_t  max_units_per_day;       /* SR3 */
  uint8_t   flags;                   /* bit 0: has_supply_monitor, 1: has_gate_sensor, 2: has_hopper_sensor */
  uint8_t   missed_policy;           /* 0 = skip, 1 = catch up once within window */
  uint8_t   catchup_window_min;      /* default 30 */
  uint8_t   reserved;
  uint32_t  storage_capacity;        /* full hopper/reservoir, in g or ml (matches unit) */
  uint16_t  crc16;                   /* over bytes 0..offsetof(crc16) */
} node_profile_t;

#define PROFILE_FLAG_SUPPLY_MONITOR 0x01
#define PROFILE_FLAG_GATE_SENSOR    0x02
#define PROFILE_FLAG_HOPPER_SENSOR  0x04

/* [NEW v0.7] Gate calibration mode raises the per-actuation cap to 10 s
 * (PLAN.md §5, D27) — verify against the real solenoid duty rating. */
#define GATE_MAX_ON_MS 10000
/* Timed-gate test runs and calibration assume ≥ 5 g measurable (§9.3). */

static inline uint16_t profile_max_on_ms(const node_profile_t *p) {
  return (p && p->mech == MECH_SOLENOID_GATE &&
          p->unit == UNIT_GRAMS && p->max_on_ms_single > GATE_MAX_ON_MS)
             ? GATE_MAX_ON_MS
             : (p ? p->max_on_ms_single : 0);
}

/* [NEW v0.7] Largest single actuation in the profile's unit (SR2): the max
 * on-time divided by the calibrated rate. The setup wizard clamps its first
 * schedule amount to this (§9.13 "amount within the node's per-feed cap").
 * Pulse mode is out of scope for v1 time-based cal. */
static inline uint32_t profile_feed_cap_units(const node_profile_t *p) {
  if (!p || !p->cal_ms_per_unit_x100) return 0;
  const uint32_t ms = profile_max_on_ms(p);
  return (uint32_t)(((uint64_t)ms * 100u) / p->cal_ms_per_unit_x100);
}

static inline uint16_t profile_crc16(const node_profile_t *p) {
  return crc16_ccitt(0xFFFF, (const uint8_t *)p,
                     (size_t)((const uint8_t *)&p->crc16 - (const uint8_t *)p));
}

static inline int profile_valid(const node_profile_t *p) {
  if (!p || p->version != PROFILE_VERSION) return 0;
  return p->crc16 == profile_crc16(p);
}

/* Bench defaults: solenoid gate, grams, 1 g ≈ 250 ms open (until calibrated). */
static inline node_profile_t profile_default(void) {
  node_profile_t p;
  memset(&p, 0, sizeof(p));
  p.version = PROFILE_VERSION;
  snprintf(p.name, sizeof(p.name), "node");
  p.mech = MECH_SOLENOID_GATE;
  p.unit = UNIT_GRAMS;
  p.cal_ms_per_unit_x100 = 25000;   /* 250 ms per gram */
  p.pulse_on_ms = 50;
  p.pulse_off_ms = 100;
  p.max_on_ms_single = 10000;       /* gate mode cap (D27) [VERIFY duty] */
  p.cooldown_ms = 3000;
  p.max_units_per_hour = 200;
  p.max_units_per_day = 500;
  p.flags = PROFILE_FLAG_SUPPLY_MONITOR;
  p.missed_policy = 0;
  p.catchup_window_min = 30;
  p.storage_capacity = 1500;        /* gate default, §9.7 */
  p.crc16 = profile_crc16(&p);
  return p;
}

#ifdef __cplusplus
}
#endif

#endif /* AIO_PROFILE_H */
