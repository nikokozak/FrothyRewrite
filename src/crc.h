#pragma once

#include <stdint.h>

uint32_t fr_crc32_update(uint32_t crc, const uint8_t *bytes, uint32_t length);
uint32_t fr_crc32(const uint8_t *bytes, uint32_t length);
