#pragma once

#include "types.h"

#if FR_FEATURE_PAD
typedef struct fr_pad_t {
  uint8_t bytes[FR_PROFILE_PAD_BYTES];
  uint16_t length;
} fr_pad_t;

fr_err_t fr_pad_reset(fr_runtime_t *runtime);
fr_err_t fr_pad_emit_byte(fr_runtime_t *runtime, uint8_t byte);
fr_err_t fr_pad_length(const fr_runtime_t *runtime, uint16_t *out_length);
fr_err_t fr_pad_view(const fr_runtime_t *runtime, const uint8_t **out_bytes,
                     uint16_t *out_length);
#endif
