/*
 * AIO Feeder — CRC16-CCITT (poly 0x1021, init 0xFFFF, no reflect, no final xor).
 * Shared by config slots, profile integrity and frame MAC chains. Host-tested
 * against the standard check value 0x29B1 for "123456789".
 */
#ifndef AIO_CRC16_H
#define AIO_CRC16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint16_t crc16_ccitt(uint16_t crc, const uint8_t *data, size_t len) {
  while (len--) {
    crc ^= (uint16_t)(*data++) << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

#ifdef __cplusplus
}
#endif

#endif /* AIO_CRC16_H */
