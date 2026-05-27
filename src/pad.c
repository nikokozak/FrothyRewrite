#include "pad.h"

#if FR_FEATURE_PAD
#include "runtime.h"

fr_err_t fr_pad_reset(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  runtime->pad.length = 0;
  return FR_OK;
}

fr_err_t fr_pad_emit_byte(fr_runtime_t *runtime, uint8_t byte) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (runtime->pad.length >= FR_PROFILE_PAD_BYTES) {
    return FR_ERR_CAPACITY;
  }

  runtime->pad.bytes[runtime->pad.length] = byte;
  runtime->pad.length = (uint16_t)(runtime->pad.length + 1);
  return FR_OK;
}

fr_err_t fr_pad_length(const fr_runtime_t *runtime, uint16_t *out_length) {
  if (runtime == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }

  *out_length = runtime->pad.length;
  return FR_OK;
}

fr_err_t fr_pad_view(const fr_runtime_t *runtime, const uint8_t **out_bytes,
                     uint16_t *out_length) {
  if (runtime == NULL || out_bytes == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }

  *out_bytes = runtime->pad.bytes;
  *out_length = runtime->pad.length;
  return FR_OK;
}
#endif
