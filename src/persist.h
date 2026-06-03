#pragma once

#include "runtime.h"

#if FR_FEATURE_PERSISTENCE

enum {
  FR_PERSIST_STORAGE_SLOT_COUNT = 2,
  FR_PERSIST_HEADER_BYTES = 32,
  FR_PERSIST_PROFILE_HASH_OFFSET = 8,
};

fr_err_t fr_persist_save(const fr_runtime_t *runtime);
fr_err_t fr_persist_restore(fr_runtime_t *runtime);
fr_err_t fr_persist_wipe(fr_runtime_t *runtime);
fr_err_t fr_persist_wipe_user(fr_runtime_t *runtime);

uint16_t fr_persist_debug_last_payload_bytes(void);
/* Debug observability for tests and size checks; not a user protocol field. */
uint32_t fr_persist_debug_profile_hash(void);

#endif
