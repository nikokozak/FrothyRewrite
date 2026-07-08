#include "persist_format.h"

#if !FR_FEATURE_PERSISTENCE
#error "persist_format.c should only be compiled when persistence is enabled"
#endif

#include "crc.h"
#include "profile.h"

#include <string.h>

enum {
  FR_PERSIST_FORMAT_VERSION = 2,
};

static const uint8_t fr_persist_format_magic[4] = {'F', 'R', 'P', 'H'};

static uint32_t fr_persist_format_read_u32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void fr_persist_format_write_u32(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)(value & 0xffu);
  bytes[1] = (uint8_t)((value >> 8) & 0xffu);
  bytes[2] = (uint8_t)((value >> 16) & 0xffu);
  bytes[3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint32_t fr_persist_format_payload_cap(void) {
  return (uint32_t)FR_PROFILE_PERSISTENCE_BYTES -
         (uint32_t)FR_PERSIST_HEADER_BYTES;
}

static void fr_persist_format_write_header_crc(uint8_t *bytes) {
  memset(&bytes[FR_PERSIST_HEADER_CRC_OFFSET], 0, 4);
  fr_persist_format_write_u32(
      &bytes[FR_PERSIST_HEADER_CRC_OFFSET],
      fr_crc32(bytes, (uint16_t)FR_PERSIST_HEADER_BYTES));
}

fr_err_t fr_persist_format_build_header(uint8_t *bytes,
                                        uint32_t payload_length,
                                        uint32_t payload_crc) {
  if (bytes == NULL) {
    return FR_ERR_INVALID;
  }
  if (payload_length > fr_persist_format_payload_cap()) {
    return FR_ERR_CAPACITY;
  }

  memset(bytes, 0, FR_PERSIST_HEADER_BYTES);
  memcpy(bytes, fr_persist_format_magic, sizeof(fr_persist_format_magic));
  bytes[4] = FR_PERSIST_FORMAT_VERSION;
  bytes[5] = FR_PERSIST_HEADER_BYTES;
  fr_persist_format_write_u32(&bytes[FR_PERSIST_PROFILE_HASH_OFFSET],
                              fr_profile_hash());
  fr_persist_format_write_u32(&bytes[FR_PERSIST_PAYLOAD_LENGTH_OFFSET],
                              payload_length);
  fr_persist_format_write_u32(&bytes[FR_PERSIST_PAYLOAD_CRC_OFFSET],
                              payload_crc);
  fr_persist_format_write_header_crc(bytes);
  return FR_OK;
}

fr_err_t fr_persist_format_validate(const uint8_t *bytes,
                                    uint16_t stored_length,
                                    fr_persist_format_info_t *out) {
  uint8_t scratch[FR_PERSIST_HEADER_BYTES];
  uint32_t payload_length = 0;
  uint32_t total_length = 0;
  uint32_t stored_crc = 0;
  uint32_t payload_crc = 0;

  if (bytes == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (stored_length > FR_PROFILE_PERSISTENCE_BYTES ||
      stored_length < FR_PERSIST_HEADER_BYTES) {
    return FR_ERR_CORRUPT;
  }
  if (memcmp(bytes, fr_persist_format_magic, sizeof(fr_persist_format_magic)) !=
      0) {
    return FR_ERR_NOT_FOUND;
  }
  if (bytes[4] != FR_PERSIST_FORMAT_VERSION ||
      bytes[5] != FR_PERSIST_HEADER_BYTES) {
    return FR_ERR_CORRUPT;
  }
  if (fr_persist_format_read_u32(&bytes[FR_PERSIST_PROFILE_HASH_OFFSET]) !=
      fr_profile_hash()) {
    return FR_ERR_CORRUPT;
  }

  memcpy(scratch, bytes, sizeof(scratch));
  stored_crc =
      fr_persist_format_read_u32(&scratch[FR_PERSIST_HEADER_CRC_OFFSET]);
  memset(&scratch[FR_PERSIST_HEADER_CRC_OFFSET], 0, 4);
  if (fr_crc32(scratch, (uint16_t)FR_PERSIST_HEADER_BYTES) != stored_crc) {
    return FR_ERR_CORRUPT;
  }

  payload_length =
      fr_persist_format_read_u32(&bytes[FR_PERSIST_PAYLOAD_LENGTH_OFFSET]);
  if (payload_length > fr_persist_format_payload_cap()) {
    return FR_ERR_CORRUPT;
  }
  total_length = (uint32_t)FR_PERSIST_HEADER_BYTES + payload_length;
  if (stored_length < total_length) {
    return FR_ERR_CORRUPT;
  }

  payload_crc = fr_persist_format_read_u32(&bytes[FR_PERSIST_PAYLOAD_CRC_OFFSET]);
  if (fr_crc32(&bytes[FR_PERSIST_HEADER_BYTES], payload_length) !=
      payload_crc) {
    return FR_ERR_CORRUPT;
  }

  out->backend_generation = fr_persist_format_read_u32(
      &bytes[FR_PERSIST_BACKEND_GENERATION_OFFSET]);
  out->payload_length = payload_length;
  out->total_length = total_length;
  out->payload_crc = payload_crc;
  return FR_OK;
}

fr_err_t fr_persist_format_stamp_generation(uint8_t *bytes,
                                            uint16_t stored_length,
                                            uint32_t backend_generation) {
  if (bytes == NULL) {
    return FR_ERR_INVALID;
  }
  if (stored_length < FR_PERSIST_HEADER_BYTES ||
      stored_length > FR_PROFILE_PERSISTENCE_BYTES) {
    return FR_ERR_CORRUPT;
  }

  fr_persist_format_write_u32(&bytes[FR_PERSIST_BACKEND_GENERATION_OFFSET],
                              backend_generation);
  fr_persist_format_write_header_crc(bytes);
  return FR_OK;
}
