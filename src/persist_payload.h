#pragma once

#include "runtime.h"

#if FR_FEATURE_PERSISTENCE

fr_err_t fr_persist_payload_encode(const fr_runtime_t *runtime, uint8_t *bytes,
                                   uint16_t cap, uint16_t *out_length);
fr_err_t fr_persist_payload_restore(fr_runtime_t *runtime, const uint8_t *bytes,
                                    uint16_t length);

#endif
