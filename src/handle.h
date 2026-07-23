#pragma once

#include "tagged.h"
#include "types.h"

#include <stdbool.h>
#include <stdint.h>

/* The disabled-profile capacity avoids zero-length arrays in public structs;
 * fr_runtime_t only embeds the table when FR_FEATURE_HANDLES is enabled. */
#define FR_HANDLE_TABLE_CAPACITY                                             \
  (FR_PROFILE_MAX_HANDLES > 0 ? FR_PROFILE_MAX_HANDLES : 1)
#define FR_HANDLE_PLATFORM_NONE UINT16_MAX

enum {
  FR_HANDLE_KIND_NONE = 0,
  FR_HANDLE_KIND_UART = 1,
  FR_HANDLE_KIND_PWM = 2,
  FR_HANDLE_KIND_I2C_BUS = 3,
  FR_HANDLE_KIND_I2C_DEVICE = 4,
  FR_HANDLE_KIND_SPI = 5,
  FR_HANDLE_KIND_TCP = 6,
  FR_HANDLE_KIND_TRACE = 7,
  FR_HANDLE_KIND_PULSE = 8,
  FR_HANDLE_KIND_BLE_CONNECTION = 9,
  FR_HANDLE_KIND_COUNT = 10,
};

typedef struct fr_handle_entry_t {
  uint16_t platform_index;
  fr_handle_kind_t kind;
  fr_handle_generation_t generation;
  bool retired;
} fr_handle_entry_t;

typedef struct fr_handle_table_t {
  fr_handle_entry_t entries[FR_HANDLE_TABLE_CAPACITY];
} fr_handle_table_t;

void fr_handle_reset(fr_runtime_t *runtime);
void fr_handle_close_all(fr_runtime_t *runtime);
fr_err_t fr_handle_close_kind(fr_runtime_t *runtime, fr_handle_kind_t kind);

fr_err_t fr_handle_reserve(fr_runtime_t *runtime, fr_handle_kind_t kind,
                           fr_handle_ref_t *out_ref,
                           fr_tagged_t *out_tagged);
fr_err_t fr_handle_activate(fr_runtime_t *runtime, fr_handle_ref_t ref,
                            uint16_t platform_index);
fr_err_t fr_handle_release_reserved(fr_runtime_t *runtime, fr_handle_ref_t ref);
fr_err_t fr_handle_lookup(const fr_runtime_t *runtime, fr_handle_ref_t ref,
                          fr_handle_kind_t expected_kind,
                          fr_handle_kind_t *out_kind,
                          uint16_t *out_platform_index);
/* Reverse lookup for idempotent open (ADR 0067): the live tagged handle for
 * an active (kind, platform_index) pair, or FR_ERR_NOT_FOUND. */
fr_err_t fr_handle_find_active(const fr_runtime_t *runtime,
                               fr_handle_kind_t kind, uint16_t platform_index,
                               fr_tagged_t *out_tagged);
fr_err_t fr_handle_close(fr_runtime_t *runtime, fr_handle_ref_t ref);
const char *fr_handle_kind_name(fr_handle_kind_t kind);
