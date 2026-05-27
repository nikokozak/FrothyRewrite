/*
 * Slot identity is the array index. Rebinding changes the current tagged word,
 * not the identity used by code, images, or host manifests.
 */
#include "slot.h"

#include "base_defs.h"

#include <string.h>

static void fr_slot_note_id(fr_slot_table_t *slots, fr_slot_id_t slot_id) {
  if (slot_id >= slots->count) {
    slots->count = (uint16_t)(slot_id + 1);
  }
}

static void fr_slot_note_base_id(fr_slot_table_t *slots, fr_slot_id_t slot_id) {
  fr_slot_note_id(slots, slot_id);
  if (slot_id >= slots->base_count) {
    slots->base_count = (uint16_t)(slot_id + 1);
  }
}

fr_slot_id_t fr_slot_first_project_id(void) {
  fr_slot_id_t next = 0;

  for (uint16_t i = 0; i < fr_base_def_count(); i++) {
    const fr_base_def_t *def = NULL;
    fr_base_layer_t layer = FR_BASE_LAYER_CORE;

    if (fr_base_def_at(i, &def, &layer) != FR_OK) {
      return next;
    }
    (void)layer;
    if (def->slot_id >= next) {
      next = (fr_slot_id_t)(def->slot_id + 1);
    }
  }

  return next;
}

bool fr_slot_is_project_id(fr_slot_id_t slot_id) {
  return slot_id >= fr_slot_first_project_id();
}

static fr_err_t fr_slot_name_length(const char *name, uint16_t *out_length) {
  uint16_t length = 0;

  if (name == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }

  while (name[length] != '\0') {
    if (length >= FR_PROFILE_MAX_NAME_BYTES) {
      return FR_ERR_RANGE;
    }
    length += 1;
  }
  if (length == 0) {
    return FR_ERR_INVALID;
  }

  *out_length = length;
  return FR_OK;
}

static fr_err_t fr_slot_check_name(const char *name) {
  uint16_t length = 0;

  return fr_slot_name_length(name, &length);
}

fr_err_t fr_slot_read(fr_runtime_t *runtime, fr_slot_id_t slot_id,
                      fr_tagged_t *out_tagged) {
  if (runtime == NULL || out_tagged == NULL) {
    return FR_ERR_INVALID;
  }
  if (slot_id >= FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_RANGE;
  }
  *out_tagged = runtime->slots.current[slot_id];
  return FR_OK;
}

fr_err_t fr_slot_write(fr_runtime_t *runtime, fr_slot_id_t slot_id,
                       fr_tagged_t tagged) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (slot_id >= FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_RANGE;
  }
  runtime->slots.current[slot_id] = tagged;
  runtime->slots.overlay[slot_id] = tagged != runtime->slots.base[slot_id];
  fr_slot_note_id(&runtime->slots, slot_id);
  return FR_OK;
}

fr_err_t fr_slot_set_base(fr_runtime_t *runtime, fr_slot_id_t slot_id,
                          fr_tagged_t tagged) {
  fr_handle_ref_t handle_ref = {0};

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (slot_id >= FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_RANGE;
  }
  if (fr_tagged_decode_handle_ref(tagged, &handle_ref) == FR_OK) {
    return FR_ERR_VOLATILE;
  }
  runtime->slots.base[slot_id] = tagged;
  runtime->slots.current[slot_id] = tagged;
  runtime->slots.overlay[slot_id] = false;
  fr_slot_note_base_id(&runtime->slots, slot_id);
  return FR_OK;
}

fr_err_t fr_slot_restore(fr_runtime_t *runtime, fr_slot_id_t slot_id) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (slot_id >= FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_RANGE;
  }
  runtime->slots.current[slot_id] = runtime->slots.base[slot_id];
  runtime->slots.overlay[slot_id] = false;
  return FR_OK;
}

bool fr_slot_is_overlay(const fr_runtime_t *runtime, fr_slot_id_t slot_id) {
  if (runtime == NULL || slot_id >= FR_PROFILE_MAX_SLOTS) {
    return false;
  }
  return runtime->slots.overlay[slot_id];
}

fr_slot_id_t fr_slot_count(const fr_runtime_t *runtime) {
  return runtime == NULL ? 0 : runtime->slots.count;
}

fr_err_t fr_slot_id_for_name(const fr_runtime_t *runtime, const char *name,
                             fr_slot_id_t *out_slot_id) {
  if (runtime == NULL || name == NULL || out_slot_id == NULL) {
    return FR_ERR_INVALID;
  }

  if (fr_base_slot_id_for_name(name, out_slot_id) == FR_OK) {
    return FR_OK;
  }

#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  for (uint16_t i = 0; i < runtime->slots.overlay_name_count; i++) {
    if (strcmp(runtime->slots.overlay_names[i], name) == 0) {
      *out_slot_id = runtime->slots.overlay_name_slots[i];
      return FR_OK;
    }
  }
#endif

  return FR_ERR_NOT_FOUND;
}

const char *fr_slot_name(const fr_runtime_t *runtime, fr_slot_id_t slot_id) {
  const char *base_name = fr_base_slot_name(slot_id);

  if (base_name != NULL) {
    return base_name;
  }

#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  if (runtime != NULL) {
    for (uint16_t i = 0; i < runtime->slots.overlay_name_count; i++) {
      if (runtime->slots.overlay_name_slots[i] == slot_id) {
        return runtime->slots.overlay_names[i];
      }
    }
  }
#else
  (void)runtime;
#endif

  return NULL;
}

uint16_t fr_slot_project_name_count(const fr_runtime_t *runtime) {
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  return runtime == NULL ? 0 : runtime->slots.overlay_name_count;
#else
  (void)runtime;
  return 0;
#endif
}

const char *fr_slot_project_name_at(const fr_runtime_t *runtime,
                                    uint16_t index) {
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  if (runtime == NULL || index >= runtime->slots.overlay_name_count) {
    return NULL;
  }
  return runtime->slots.overlay_names[index];
#else
  (void)runtime;
  (void)index;
  return NULL;
#endif
}

fr_err_t fr_slot_prepare_project_name(const fr_runtime_t *runtime,
                                      const char *name,
                                      fr_slot_id_t *out_slot_id) {
  fr_slot_id_t existing = 0;
  fr_err_t err = FR_OK;

  if (runtime == NULL || out_slot_id == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_slot_check_name(name));

  err = fr_slot_id_for_name(runtime, name, &existing);
  if (err == FR_OK) {
    *out_slot_id = existing;
    return FR_OK;
  }
  if (err != FR_ERR_NOT_FOUND) {
    return err;
  }

#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  fr_slot_id_t next = runtime->slots.count;
  fr_slot_id_t first_project_id = fr_slot_first_project_id();

  if (runtime->slots.overlay_name_count >= FR_PROFILE_MAX_OVERLAY_NAMES) {
    return FR_ERR_CAPACITY;
  }

  if (next < first_project_id) {
    next = first_project_id;
  }
  if (next >= FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_CAPACITY;
  }

  *out_slot_id = next;
  return FR_OK;
#else
  return FR_ERR_UNSUPPORTED;
#endif
}

fr_err_t fr_slot_validate_project_names(const fr_runtime_t *runtime,
                                        const fr_slot_name_t names[],
                                        uint16_t name_count,
                                        fr_slot_id_t slot_count_after_writes) {
  if (runtime == NULL || (names == NULL && name_count > 0)) {
    return FR_ERR_INVALID;
  }
  if (slot_count_after_writes < fr_slot_count(runtime) ||
      slot_count_after_writes > FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_INVALID;
  }

#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  const char *new_names[FR_PROFILE_MAX_OVERLAY_NAMES];
  fr_slot_id_t new_slots[FR_PROFILE_MAX_OVERLAY_NAMES];
  fr_slot_id_t next_new_slot = 0;
  uint16_t new_name_count = 0;
  bool has_next_new_slot = false;

  for (uint16_t i = 0; i < name_count; i++) {
    fr_slot_id_t existing_slot_id = 0;
    fr_err_t err = FR_OK;
    bool already_new = false;

    if (names[i].slot_id >= FR_PROFILE_MAX_SLOTS) {
      return FR_ERR_RANGE;
    }
    if (names[i].slot_id >= slot_count_after_writes) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_slot_check_name(names[i].name));

    err = fr_slot_id_for_name(runtime, names[i].name, &existing_slot_id);
    if (err == FR_OK) {
      if (existing_slot_id != names[i].slot_id) {
        return FR_ERR_INVALID;
      }
      continue;
    }
    if (err != FR_ERR_NOT_FOUND) {
      return err;
    }

    for (uint16_t j = 0; j < new_name_count; j++) {
      if (strcmp(new_names[j], names[i].name) == 0) {
        if (new_slots[j] != names[i].slot_id) {
          return FR_ERR_INVALID;
        }
        already_new = true;
        break;
      }
    }
    if (already_new) {
      continue;
    }

    if ((uint32_t)runtime->slots.overlay_name_count + new_name_count >=
        FR_PROFILE_MAX_OVERLAY_NAMES) {
      return FR_ERR_CAPACITY;
    }
    if (!has_next_new_slot) {
      FR_TRY(fr_slot_prepare_project_name(runtime, names[i].name,
                                          &next_new_slot));
      has_next_new_slot = true;
    }
    if (names[i].slot_id != next_new_slot) {
      return FR_ERR_INVALID;
    }

    new_names[new_name_count] = names[i].name;
    new_slots[new_name_count] = names[i].slot_id;
    new_name_count = (uint16_t)(new_name_count + 1);
    next_new_slot = (fr_slot_id_t)(next_new_slot + 1);
  }

  return FR_OK;
#else
  (void)names;
  return name_count == 0 ? FR_OK : FR_ERR_UNSUPPORTED;
#endif
}

fr_err_t fr_slot_bind_project_name(fr_runtime_t *runtime, const char *name,
                                   fr_slot_id_t slot_id) {
  fr_slot_id_t existing = 0;
  uint16_t length = 0;
  fr_err_t err = FR_OK;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_slot_name_length(name, &length));

  err = fr_slot_id_for_name(runtime, name, &existing);
  if (err == FR_OK) {
    return existing == slot_id ? FR_OK : FR_ERR_INVALID;
  }
  if (err != FR_ERR_NOT_FOUND) {
    return err;
  }

#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  if (runtime->slots.overlay_name_count >= FR_PROFILE_MAX_OVERLAY_NAMES) {
    return FR_ERR_CAPACITY;
  }
  if (slot_id < fr_slot_first_project_id() || slot_id >= runtime->slots.count) {
    return FR_ERR_INVALID;
  }

  memcpy(runtime->slots.overlay_names[runtime->slots.overlay_name_count], name,
         (uint16_t)(length + 1));
  runtime->slots.overlay_name_slots[runtime->slots.overlay_name_count] =
      slot_id;
  runtime->slots.overlay_name_count =
      (uint16_t)(runtime->slots.overlay_name_count + 1);
  return FR_OK;
#else
  (void)slot_id;
  return FR_ERR_UNSUPPORTED;
#endif
}
