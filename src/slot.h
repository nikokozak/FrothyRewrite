#pragma once

#include "tagged.h"
#include "runtime.h"

typedef struct fr_slot_name_t {
  fr_slot_id_t slot_id;
  const char *name;
} fr_slot_name_t;

fr_err_t fr_slot_read(fr_runtime_t *runtime, fr_slot_id_t slot_id,
                      fr_tagged_t *out_tagged);
fr_err_t fr_slot_write(fr_runtime_t *runtime, fr_slot_id_t slot_id,
                       fr_tagged_t tagged);
fr_err_t fr_slot_set_base(fr_runtime_t *runtime, fr_slot_id_t slot_id,
                          fr_tagged_t tagged);
fr_err_t fr_slot_restore(fr_runtime_t *runtime, fr_slot_id_t slot_id);
bool fr_slot_is_overlay(const fr_runtime_t *runtime, fr_slot_id_t slot_id);
fr_slot_id_t fr_slot_count(const fr_runtime_t *runtime);

/* Project slots are live/user-defined slots above the installed base contract. */
fr_slot_id_t fr_slot_first_project_id(void);
bool fr_slot_is_project_id(fr_slot_id_t slot_id);

fr_err_t fr_slot_id_for_name(const fr_runtime_t *runtime, const char *name,
                             fr_slot_id_t *out_slot_id);
const char *fr_slot_name(const fr_runtime_t *runtime, fr_slot_id_t slot_id);
/* Project names are retained only when the active profile keeps live names. */
uint16_t fr_slot_project_name_count(const fr_runtime_t *runtime);
const char *fr_slot_project_name_at(const fr_runtime_t *runtime,
                                    uint16_t index);
fr_err_t fr_slot_prepare_project_name(const fr_runtime_t *runtime,
                                      const char *name,
                                      fr_slot_id_t *out_slot_id);
fr_err_t fr_slot_rollback_project_name(fr_runtime_t *runtime,
                                       const char *name,
                                       fr_slot_id_t slot_id);
/* Lower slots.count back over trailing slots that are free (unoverlaid, base
 * value, unnamed). Call after freeing user slots so the next project name gets
 * the lowest free id. */
void fr_slot_reclaim_free_tail(fr_runtime_t *runtime);
fr_err_t fr_slot_validate_project_names(const fr_runtime_t *runtime,
                                        const fr_slot_name_t names[],
                                        uint16_t name_count,
                                        fr_slot_id_t slot_count_after_writes);
fr_err_t fr_slot_bind_project_name(fr_runtime_t *runtime, const char *name,
                                   fr_slot_id_t slot_id);
