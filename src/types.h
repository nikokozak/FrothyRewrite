#pragma once

#include "config.h"

#include <stddef.h>
#include <stdint.h>

typedef struct fr_runtime_t fr_runtime_t;

typedef enum fr_err_t {
  FR_OK = 0,
  FR_ERR_RANGE,
  FR_ERR_TYPE,
  FR_ERR_DOMAIN,
  FR_ERR_CAPACITY,
  FR_ERR_OVERFLOW,
  FR_ERR_UNDERFLOW,
  FR_ERR_NOT_FOUND,
  FR_ERR_INVALID,
  FR_ERR_UNSUPPORTED,
  FR_ERR_INTERRUPTED,
  FR_ERR_CORRUPT,
  FR_ERR_IO,
  FR_ERR_VOLATILE,
  FR_ERR_HANDLE,
} fr_err_t;

const char *fr_err_name(fr_err_t err);

#if FR_WORD_SIZE == 16
typedef int16_t fr_int_t;
#elif FR_WORD_SIZE == 32
typedef int32_t fr_int_t;
#else
#error "FR_WORD_SIZE must be 16 or 32"
#endif
typedef uint16_t fr_slot_id_t;
typedef uint16_t fr_code_object_id_t;
typedef uint16_t fr_native_id_t;
typedef uint16_t fr_object_id_t;
typedef uint8_t fr_handle_id_t;
typedef uint8_t fr_handle_generation_t;
typedef uint8_t fr_handle_kind_t;
typedef uint16_t fr_code_offset_t;
typedef uintptr_t fr_addr_t;

typedef struct fr_handle_ref_t {
  fr_handle_id_t id;
  fr_handle_generation_t generation;
} fr_handle_ref_t;

/* Wire-byte values match the persist record tier tag (D7). */
typedef enum fr_install_tier_t {
  FR_INSTALL_TIER_LIBRARY = 1,
  FR_INSTALL_TIER_USER = 2,
} fr_install_tier_t;

#define FR_TRY(expr)                                                           \
  do {                                                                         \
    fr_err_t fr_try_err = (expr);                                              \
    if (fr_try_err != FR_OK) {                                                 \
      return fr_try_err;                                                       \
    }                                                                          \
  } while (0)
