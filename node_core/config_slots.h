/*
 * AIO Feeder — A/B config slots with CRC (PLAN.md §7.5).
 *
 * Slot layout: [payload bytes][crc16 LE]. The runtime keeps a monotonic
 * write_seq as the first payload field; selection prefers the intact slot
 * with the higher seq. Caller provides the two raw slot buffers (read from
 * EEPROM/NVS/flash) and the payload size.
 */
#ifndef AIO_CONFIG_SLOTS_H
#define AIO_CONFIG_SLOTS_H

#include <stddef.h>
#include <stdint.h>
#include "crc16.h"

namespace aio {

static inline uint16_t slot_crc_at(const uint8_t *slot, size_t payload_len) {
  return (uint16_t)(slot[payload_len] | (slot[payload_len + 1] << 8));
}

static inline void slot_fill_crc(uint8_t *slot, size_t payload_len) {
  uint16_t crc = crc16_ccitt(0xFFFF, slot, payload_len);
  slot[payload_len] = (uint8_t)(crc & 0xFF);
  slot[payload_len + 1] = (uint8_t)(crc >> 8);
}

static inline bool slot_valid(const uint8_t *slot, size_t payload_len) {
  if (!slot) return false;
  return crc16_ccitt(0xFFFF, slot, payload_len) == slot_crc_at(slot, payload_len);
}

/* Pick the newer valid slot; returns nullptr when neither is intact. */
static inline const uint8_t *slot_select(const uint8_t *a, const uint8_t *b,
                                         size_t payload_len) {
  bool va = slot_valid(a, payload_len);
  bool vb = slot_valid(b, payload_len);
  if (va && vb) {
    uint32_t seq_a = (uint32_t)a[0] | (a[1] << 8) | ((uint32_t)a[2] << 16) | ((uint32_t)a[3] << 24);
    uint32_t seq_b = (uint32_t)b[0] | (b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return seq_b > seq_a ? b : a;
  }
  return va ? a : (vb ? b : nullptr);
}

}  // namespace aio
#endif /* AIO_CONFIG_SLOTS_H */
