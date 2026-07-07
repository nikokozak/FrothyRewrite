#pragma once

#include "types.h"

#if FR_FEATURE_PERSISTENCE

enum {
  FR_PERSIST_HEADER_BYTES = 32,
  FR_PERSIST_PROFILE_HASH_OFFSET = 8,
  FR_PERSIST_BACKEND_GENERATION_OFFSET = 12,
  FR_PERSIST_PAYLOAD_LENGTH_OFFSET = 16,
  FR_PERSIST_PAYLOAD_CRC_OFFSET = 20,
  FR_PERSIST_HEADER_CRC_OFFSET = 24,
};

typedef struct fr_persist_format_info_t {
  uint32_t backend_generation;
  uint16_t payload_length;
  uint16_t total_length;
  uint32_t payload_crc;
} fr_persist_format_info_t;

fr_err_t fr_persist_format_build_header(uint8_t *bytes,
                                        uint16_t payload_length,
                                        uint32_t payload_crc);
fr_err_t fr_persist_format_validate(const uint8_t *bytes,
                                    uint16_t stored_length,
                                    fr_persist_format_info_t *out);
fr_err_t fr_persist_format_stamp_generation(uint8_t *bytes,
                                            uint16_t stored_length,
                                            uint32_t backend_generation);

#endif
