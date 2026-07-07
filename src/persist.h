#pragma once

#include "persist_format.h"
#include "runtime.h"

#if FR_FEATURE_PERSISTENCE

fr_err_t fr_persist_save(const fr_runtime_t *runtime);
fr_err_t fr_persist_restore(fr_runtime_t *runtime);
fr_err_t fr_persist_wipe(fr_runtime_t *runtime);

/* Drop every L2 overlay binding from the runtime and save so NVS only retains
 * L1 records. */
fr_err_t fr_persist_wipe_user(fr_runtime_t *runtime);
/* D10 install-library implicit L1 wipe. Drops L1 from the runtime and commits
 * the post-wipe runtime before the REPL flips the session install tier. */
fr_err_t fr_persist_install_library(fr_runtime_t *runtime);
/* D3 install-library mode persists new definitions with tier tag L1 after each
 * successful overlay-apply or value-binding. */
fr_err_t fr_persist_save_full(const fr_runtime_t *runtime);
/* D6 boot two-call sequence. Restore library first, then user against the
 * runtime state left by the library pass. */
fr_err_t fr_persist_restore_library(fr_runtime_t *runtime);
fr_err_t fr_persist_restore_user(fr_runtime_t *runtime);

uint16_t fr_persist_debug_last_payload_bytes(void);
/* Debug observability for tests and size checks; not a user protocol field. */
uint32_t fr_persist_debug_profile_hash(void);

#endif
