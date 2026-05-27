#include "crc.h"

uint32_t fr_crc32_update(uint32_t crc, const uint8_t *bytes, uint16_t length) {
  for (uint16_t i = 0; i < length; i++) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0xedb88320u & mask);
    }
  }
  return crc;
}

uint32_t fr_crc32(const uint8_t *bytes, uint16_t length) {
  return ~fr_crc32_update(0xffffffffu, bytes, length);
}
