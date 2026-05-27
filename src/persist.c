#include "persist.h"

#if !FR_FEATURE_PERSISTENCE
#error "persist.c should only be compiled when FR_FEATURE_PERSISTENCE is enabled"
#endif

#include "base_defs.h"
#include "crc.h"
#include "persist_payload.h"
#include "platform.h"
#include "profile.h"

#include <stdbool.h>
#include <string.h>

enum {
  /* v1 widens profile identity beyond persistence-only limits. */
  FR_PERSIST_FORMAT_VERSION = 1,
};

static const uint8_t fr_persist_header_magic[4] = {'F', 'R', 'P', 'H'};
static uint8_t fr_persist_payload[FR_PROFILE_PERSISTENCE_BYTES -
                                  FR_PERSIST_HEADER_BYTES];
static uint16_t fr_persist_last_payload_bytes = 0;

typedef struct fr_persist_header_t {
  uint32_t generation;
  uint16_t payload_length;
  uint32_t payload_crc;
} fr_persist_header_t;

static uint16_t fr_persist_read_u16(const uint8_t *bytes) {
  return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t fr_persist_read_u32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void fr_persist_write_u16_raw(uint8_t *bytes, uint16_t value) {
  bytes[0] = (uint8_t)(value & 0xffu);
  bytes[1] = (uint8_t)(value >> 8);
}

static void fr_persist_write_u32_raw(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)(value & 0xffu);
  bytes[1] = (uint8_t)((value >> 8) & 0xffu);
  bytes[2] = (uint8_t)((value >> 16) & 0xffu);
  bytes[3] = (uint8_t)((value >> 24) & 0xffu);
}

static fr_err_t fr_persist_header_build(uint8_t *bytes,
                                        const fr_persist_header_t *header) {
  if (bytes == NULL || header == NULL) {
    return FR_ERR_INVALID;
  }

  memset(bytes, 0, FR_PERSIST_HEADER_BYTES);
  memcpy(bytes, fr_persist_header_magic, sizeof(fr_persist_header_magic));
  bytes[4] = FR_PERSIST_FORMAT_VERSION;
  bytes[5] = FR_PERSIST_HEADER_BYTES;
  fr_persist_write_u32_raw(&bytes[FR_PERSIST_PROFILE_HASH_OFFSET],
                           fr_profile_hash());
  fr_persist_write_u32_raw(&bytes[12], header->generation);
  fr_persist_write_u16_raw(&bytes[16], header->payload_length);
  fr_persist_write_u32_raw(&bytes[20], header->payload_crc);
  fr_persist_write_u32_raw(&bytes[24],
                           fr_crc32(bytes, FR_PERSIST_HEADER_BYTES));
  return FR_OK;
}

static fr_err_t fr_persist_header_parse(const uint8_t *bytes,
                                        fr_persist_header_t *out) {
  uint8_t scratch[FR_PERSIST_HEADER_BYTES];
  uint32_t stored_crc = 0;

  if (bytes == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (memcmp(bytes, fr_persist_header_magic, sizeof(fr_persist_header_magic)) !=
      0) {
    return FR_ERR_NOT_FOUND;
  }
  if (bytes[4] != FR_PERSIST_FORMAT_VERSION ||
      bytes[5] != FR_PERSIST_HEADER_BYTES) {
    return FR_ERR_CORRUPT;
  }
  if (fr_persist_read_u32(&bytes[FR_PERSIST_PROFILE_HASH_OFFSET]) !=
      fr_profile_hash()) {
    return FR_ERR_CORRUPT;
  }

  memcpy(scratch, bytes, sizeof(scratch));
  stored_crc = fr_persist_read_u32(&scratch[24]);
  memset(&scratch[24], 0, 4);
  if (fr_crc32(scratch, FR_PERSIST_HEADER_BYTES) != stored_crc) {
    return FR_ERR_CORRUPT;
  }

  out->generation = fr_persist_read_u32(&bytes[12]);
  out->payload_length = fr_persist_read_u16(&bytes[16]);
  out->payload_crc = fr_persist_read_u32(&bytes[20]);
  if (out->payload_length > sizeof(fr_persist_payload)) {
    return FR_ERR_CORRUPT;
  }
  return FR_OK;
}

static fr_err_t fr_persist_read_header(uint8_t slot,
                                       fr_persist_header_t *out) {
  uint8_t header_bytes[FR_PERSIST_HEADER_BYTES];

  FR_TRY(fr_platform_storage_read(slot, 0, header_bytes,
                                  (uint16_t)sizeof(header_bytes)));
  return fr_persist_header_parse(header_bytes, out);
}

static fr_err_t fr_persist_pick_inactive(uint8_t *out_slot,
                                         uint32_t *out_generation) {
  fr_persist_header_t a = {0};
  fr_persist_header_t b = {0};
  bool a_valid = fr_persist_read_header(0, &a) == FR_OK;
  bool b_valid = fr_persist_read_header(1, &b) == FR_OK;

  if (out_slot == NULL || out_generation == NULL) {
    return FR_ERR_INVALID;
  }

  if (!a_valid && !b_valid) {
    *out_slot = 0;
    *out_generation = 1;
  } else if (a_valid && !b_valid) {
    *out_slot = 1;
    *out_generation = a.generation + 1;
  } else if (!a_valid && b_valid) {
    *out_slot = 0;
    *out_generation = b.generation + 1;
  } else if (a.generation <= b.generation) {
    *out_slot = 0;
    *out_generation = b.generation + 1;
  } else {
    *out_slot = 1;
    *out_generation = a.generation + 1;
  }

  return FR_OK;
}

static fr_err_t fr_persist_restore_slot(fr_runtime_t *runtime, uint8_t slot,
                                        const fr_persist_header_t *header) {
  FR_TRY(fr_platform_storage_read(slot, FR_PERSIST_HEADER_BYTES,
                                  fr_persist_payload, header->payload_length));
  if (fr_crc32(fr_persist_payload, header->payload_length) !=
      header->payload_crc) {
    return FR_ERR_CORRUPT;
  }
  return fr_persist_payload_restore(runtime, fr_persist_payload,
                                    header->payload_length);
}

fr_err_t fr_persist_save(const fr_runtime_t *runtime) {
  uint16_t payload_length = 0;
  uint8_t slot = 0;
  uint32_t generation = 0;
  fr_persist_header_t header = {0};
  uint8_t header_bytes[FR_PERSIST_HEADER_BYTES];

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_persist_payload_encode(runtime, fr_persist_payload,
                                   (uint16_t)sizeof(fr_persist_payload),
                                   &payload_length));
  fr_persist_last_payload_bytes = payload_length;
  FR_TRY(fr_persist_pick_inactive(&slot, &generation));
  FR_TRY(fr_platform_storage_erase(slot));
  FR_TRY(fr_platform_storage_write(slot, FR_PERSIST_HEADER_BYTES,
                                   fr_persist_payload, payload_length));

  header.generation = generation;
  header.payload_length = payload_length;
  header.payload_crc = fr_crc32(fr_persist_payload, payload_length);
  FR_TRY(fr_persist_header_build(header_bytes, &header));
  FR_TRY(fr_platform_storage_write(slot, 0, header_bytes,
                                   (uint16_t)sizeof(header_bytes)));
  return fr_persist_read_header(slot, &header);
}

fr_err_t fr_persist_restore(fr_runtime_t *runtime) {
  fr_persist_header_t headers[FR_PERSIST_STORAGE_SLOT_COUNT];
  bool valid[FR_PERSIST_STORAGE_SLOT_COUNT];
  fr_err_t header_err[FR_PERSIST_STORAGE_SLOT_COUNT];
  uint8_t first = 0;
  uint8_t second = 1;
  fr_err_t last_err = FR_ERR_NOT_FOUND;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  header_err[0] = fr_persist_read_header(0, &headers[0]);
  header_err[1] = fr_persist_read_header(1, &headers[1]);
  valid[0] = header_err[0] == FR_OK;
  valid[1] = header_err[1] == FR_OK;
  if (!valid[0] && !valid[1]) {
    FR_TRY(fr_runtime_reset(runtime));
    if (header_err[0] != FR_ERR_NOT_FOUND) {
      return header_err[0];
    }
    if (header_err[1] != FR_ERR_NOT_FOUND) {
      return header_err[1];
    }
    return FR_ERR_NOT_FOUND;
  }
  if (valid[0] && valid[1] && headers[1].generation > headers[0].generation) {
    first = 1;
    second = 0;
  } else if (!valid[0]) {
    first = 1;
    second = 0;
  }

  if (valid[first]) {
    last_err = fr_persist_restore_slot(runtime, first, &headers[first]);
    if (last_err == FR_OK) {
      return FR_OK;
    }
  }
  if (valid[second]) {
    last_err = fr_persist_restore_slot(runtime, second, &headers[second]);
    if (last_err == FR_OK) {
      return FR_OK;
    }
  }

  FR_TRY(fr_runtime_reset(runtime));
  return last_err;
}

fr_err_t fr_persist_wipe(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_platform_storage_erase(0));
  FR_TRY(fr_platform_storage_erase(1));
  return fr_runtime_reset(runtime);
}

uint16_t fr_persist_debug_last_payload_bytes(void) {
  return fr_persist_last_payload_bytes;
}

uint32_t fr_persist_debug_profile_hash(void) {
  return fr_profile_hash();
}
