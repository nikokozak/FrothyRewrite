/*
 * Slot identity is the array index. Rebinding changes the current tagged word,
 * not the identity used by code, images, or host manifests.
 */
#include "slot.h"

#include "base_defs.h"
#include "lib_native.h"
#include "platform.h"

#include <stdint.h>
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

#if FR_FEATURE_SOURCE_BASE
  for (uint16_t i = 0; i < fr_base_source_record_count(); i++) {
    fr_slot_id_t source_slot = 0;

    if (fr_base_source_record_slot_id_at(i, &source_slot) != FR_OK) {
      continue;
    }
    if (source_slot >= next) {
      next = (fr_slot_id_t)(source_slot + 1);
    }
  }
#endif

  for (uint16_t i = 0; i < fr_lib_native_record_count(); i++) {
    fr_slot_id_t lib_slot = 0;

    if (fr_lib_native_record_slot_id_at(i, &lib_slot) != FR_OK) {
      continue;
    }
    if (lib_slot >= next) {
      next = (fr_slot_id_t)(lib_slot + 1);
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

#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
static char fr_slot_name_scratch[FR_PROFILE_MAX_NAME_BYTES + 1];

static bool fr_slot_name_pointer_in_overlay_ram(const fr_runtime_t *runtime,
                                                const char *ptr,
                                                uint8_t length) {
  if (length == 0 || runtime == NULL || ptr == NULL) {
    return false;
  }
  for (uint16_t i = 0; i < FR_PROFILE_MAX_OVERLAY_NAMES; i++) {
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t b = (uintptr_t)runtime->slots.overlay_name_bytes[i];

    if (p >= b &&
        (uint32_t)(p - b) + length <=
            (uint32_t)FR_PROFILE_MAX_NAME_BYTES + 1u) {
      return true;
    }
  }
  return false;
}

static fr_err_t fr_slot_project_name_entry_view(
    const fr_runtime_t *runtime, const fr_slot_project_name_entry_t *entry,
    const char **out_name, uint8_t *out_length) {
  if (runtime == NULL || entry == NULL || out_name == NULL ||
      out_length == NULL) {
    return FR_ERR_INVALID;
  }
  if (entry->length == 0 || entry->length > FR_PROFILE_MAX_NAME_BYTES ||
      entry->bytes == NULL) {
    return FR_ERR_CORRUPT;
  }

  switch (entry->storage_kind) {
  case FR_SLOT_NAME_STORAGE_OVERLAY_RAM:
    if (!fr_slot_name_pointer_in_overlay_ram(runtime, entry->bytes,
                                             (uint8_t)(entry->length + 1u)) ||
        entry->bytes[entry->length] != '\0') {
      return FR_ERR_CORRUPT;
    }
    break;
  case FR_SLOT_NAME_STORAGE_IMAGE:
    break;
  case FR_SLOT_NAME_STORAGE_PERSIST_IMAGE:
#if FR_FEATURE_PERSISTENCE
    if (!fr_platform_persist_pointer_is_mounted(entry->bytes,
                                                entry->length)) {
      return FR_ERR_CORRUPT;
    }
    break;
#else
    return FR_ERR_CORRUPT;
#endif
  default:
    return FR_ERR_CORRUPT;
  }

  *out_name = entry->bytes;
  *out_length = entry->length;
  return FR_OK;
}

static void fr_slot_project_name_clear(fr_runtime_t *runtime, uint16_t index) {
  runtime->slots.overlay_names[index] = (fr_slot_project_name_entry_t){
      .storage_kind = FR_SLOT_NAME_STORAGE_OVERLAY_RAM,
      .slot_id = 0,
      .bytes = NULL,
      .length = 0,
  };
  runtime->slots.overlay_name_bytes[index][0] = '\0';
}

static void fr_slot_project_name_move(fr_runtime_t *runtime, uint16_t dst,
                                      uint16_t src) {
  if (dst == src) {
    return;
  }
  runtime->slots.overlay_names[dst] = runtime->slots.overlay_names[src];
  if (runtime->slots.overlay_names[src].storage_kind ==
      FR_SLOT_NAME_STORAGE_OVERLAY_RAM) {
    memcpy(runtime->slots.overlay_name_bytes[dst],
           runtime->slots.overlay_name_bytes[src],
           (size_t)FR_PROFILE_MAX_NAME_BYTES + 1);
    runtime->slots.overlay_names[dst].bytes =
        runtime->slots.overlay_name_bytes[dst];
  }
}
#endif

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
  fr_bytes_ref_t bytes_ref = {0};

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (slot_id >= FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_RANGE;
  }
  if (fr_tagged_decode_bytes_ref(tagged, &bytes_ref) == FR_OK) {
    return FR_ERR_VOLATILE;
  }
  runtime->slots.current[slot_id] = tagged;
  runtime->slots.overlay[slot_id] = tagged != runtime->slots.base[slot_id];
  fr_slot_note_id(&runtime->slots, slot_id);
  return FR_OK;
}

fr_err_t fr_slot_set_base(fr_runtime_t *runtime, fr_slot_id_t slot_id,
                          fr_tagged_t tagged) {
  fr_handle_ref_t handle_ref = {0};
  fr_bytes_ref_t bytes_ref = {0};

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (slot_id >= FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_RANGE;
  }
  if (fr_tagged_decode_handle_ref(tagged, &handle_ref) == FR_OK) {
    return FR_ERR_VOLATILE;
  }
  if (fr_tagged_decode_bytes_ref(tagged, &bytes_ref) == FR_OK) {
    return FR_ERR_VOLATILE;
  }
  runtime->slots.base[slot_id] = tagged;
  runtime->slots.current[slot_id] = tagged;
  runtime->slots.overlay[slot_id] = false;
  runtime->slots.library_base[slot_id] = fr_tagged_nil();
  runtime->slots.library_base_present[slot_id] = false;
  runtime->slots.base_tier[slot_id] = 0;
  fr_slot_note_base_id(&runtime->slots, slot_id);
  return FR_OK;
}

fr_err_t fr_slot_set_mounted_base(fr_runtime_t *runtime, fr_slot_id_t slot_id,
                                  fr_tagged_t tagged,
                                  fr_install_tier_t tier) {
  fr_handle_ref_t handle_ref = {0};
  fr_bytes_ref_t bytes_ref = {0};

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (slot_id >= FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_RANGE;
  }
  if (tier != FR_INSTALL_TIER_LIBRARY && tier != FR_INSTALL_TIER_USER) {
    return FR_ERR_INVALID;
  }
  if (fr_tagged_decode_handle_ref(tagged, &handle_ref) == FR_OK) {
    return FR_ERR_VOLATILE;
  }
  if (fr_tagged_decode_bytes_ref(tagged, &bytes_ref) == FR_OK) {
    return FR_ERR_VOLATILE;
  }

  if (tier == FR_INSTALL_TIER_LIBRARY) {
    runtime->slots.library_base[slot_id] = tagged;
    runtime->slots.library_base_present[slot_id] = true;
  }
  runtime->slots.base[slot_id] = tagged;
  runtime->slots.current[slot_id] = tagged;
  runtime->slots.overlay[slot_id] = false;
  runtime->slots.base_tier[slot_id] = tier;
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
  uint16_t name_length = 0;

  if (runtime == NULL || name == NULL || out_slot_id == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_slot_name_length(name, &name_length));

  if (fr_base_slot_id_for_name(name, out_slot_id) == FR_OK) {
    return FR_OK;
  }
  if (fr_lib_native_slot_id_for_name(name, out_slot_id) == FR_OK) {
    return FR_OK;
  }

#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  for (uint16_t i = 0; i < runtime->slots.overlay_name_count; i++) {
    const char *stored = NULL;
    uint8_t stored_length = 0;

    FR_TRY(fr_slot_project_name_entry_view(
        runtime, &runtime->slots.overlay_names[i], &stored,
        &stored_length));
    if (stored_length == name_length &&
        memcmp(stored, name, name_length) == 0) {
      *out_slot_id = runtime->slots.overlay_names[i].slot_id;
      return FR_OK;
    }
  }
#endif

  return FR_ERR_NOT_FOUND;
}

fr_err_t fr_slot_name_view(const fr_runtime_t *runtime, fr_slot_id_t slot_id,
                           const char **out_name, uint8_t *out_length) {
  const char *base_name = NULL;

  if (out_name == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }

  base_name = fr_base_slot_name(slot_id);
  if (base_name != NULL) {
    uint16_t length = 0;

    FR_TRY(fr_slot_name_length(base_name, &length));
    *out_name = base_name;
    *out_length = (uint8_t)length;
    return FR_OK;
  }

  {
    const char *lib_name = fr_lib_native_slot_name(slot_id);
    if (lib_name != NULL) {
      uint16_t length = 0;

      FR_TRY(fr_slot_name_length(lib_name, &length));
      *out_name = lib_name;
      *out_length = (uint8_t)length;
      return FR_OK;
    }
  }

#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  if (runtime != NULL) {
    for (uint16_t i = 0; i < runtime->slots.overlay_name_count; i++) {
      if (runtime->slots.overlay_names[i].slot_id == slot_id) {
        return fr_slot_project_name_entry_view(
            runtime, &runtime->slots.overlay_names[i], out_name, out_length);
      }
    }
  }
#else
  (void)runtime;
#endif

  return FR_ERR_NOT_FOUND;
}

const char *fr_slot_name(const fr_runtime_t *runtime, fr_slot_id_t slot_id) {
  const char *name = NULL;
  uint8_t length = 0;

  if (fr_slot_name_view(runtime, slot_id, &name, &length) != FR_OK) {
    return NULL;
  }
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  memcpy(fr_slot_name_scratch, name, length);
  fr_slot_name_scratch[length] = '\0';
  return fr_slot_name_scratch;
#else
  (void)runtime;
#endif
  return name;
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
  const char *name = NULL;
  uint8_t length = 0;

  if (fr_slot_project_name_view_at(runtime, index, &name, &length) != FR_OK) {
    return NULL;
  }
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  memcpy(fr_slot_name_scratch, name, length);
  fr_slot_name_scratch[length] = '\0';
  return fr_slot_name_scratch;
#else
  (void)index;
#endif
  return name;
}

fr_err_t fr_slot_project_name_view_at(const fr_runtime_t *runtime,
                                      uint16_t index, const char **out_name,
                                      uint8_t *out_length) {
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  if (runtime == NULL || index >= runtime->slots.overlay_name_count) {
    return FR_ERR_NOT_FOUND;
  }
  return fr_slot_project_name_entry_view(
      runtime, &runtime->slots.overlay_names[index], out_name, out_length);
#else
  (void)runtime;
  (void)index;
  (void)out_name;
  (void)out_length;
  return FR_ERR_NOT_FOUND;
#endif
}

static bool fr_slot_project_name_points_to(const fr_runtime_t *runtime,
                                           fr_slot_id_t slot_id) {
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  if (runtime == NULL) {
    return false;
  }
  for (uint16_t i = 0; i < runtime->slots.overlay_name_count; i++) {
    if (runtime->slots.overlay_names[i].slot_id == slot_id) {
      return true;
    }
  }
  return false;
#else
  (void)runtime;
  (void)slot_id;
  return false;
#endif
}

/* Drop the top of the slot table back down over any trailing slots that are no
 * longer in use: not overlaid, holding their base value, and unnamed. Keeps
 * slots.count == (highest live slot)+1 so the next project name is assigned the
 * lowest free id. Both name rollback and a user wipe free trailing slots and
 * must call this, or a later definition is assigned an id past what the install
 * validator will accept. */
void fr_slot_reclaim_free_tail(fr_runtime_t *runtime) {
  fr_slot_id_t first_project_id = fr_slot_first_project_id();

  if (runtime == NULL) {
    return;
  }
  while (runtime->slots.count > runtime->slots.base_count) {
    fr_slot_id_t last = (fr_slot_id_t)(runtime->slots.count - 1);

    if (runtime->slots.overlay[last] ||
        runtime->slots.current[last] != runtime->slots.base[last] ||
        fr_slot_project_name_points_to(runtime, last)) {
      break;
    }
    runtime->slots.count = last;
  }
  while (runtime->slots.base_count > first_project_id) {
    fr_slot_id_t last = (fr_slot_id_t)(runtime->slots.base_count - 1);

    if (runtime->slots.base_tier[last] != 0 ||
        runtime->slots.library_base_present[last]) {
      break;
    }
    if (runtime->slots.overlay[last] ||
        runtime->slots.current[last] != runtime->slots.base[last] ||
        fr_slot_project_name_points_to(runtime, last)) {
      break;
    }
    runtime->slots.base_count = last;
  }
  while (runtime->slots.count > runtime->slots.base_count) {
    fr_slot_id_t last = (fr_slot_id_t)(runtime->slots.count - 1);

    if (runtime->slots.overlay[last] ||
        runtime->slots.current[last] != runtime->slots.base[last] ||
        fr_slot_project_name_points_to(runtime, last)) {
      break;
    }
    runtime->slots.count = last;
  }
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

fr_err_t fr_slot_rollback_project_name(fr_runtime_t *runtime,
                                       const char *name,
                                       fr_slot_id_t slot_id) {
  bool removed_name = false;

  if (runtime == NULL || name == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_slot_is_project_id(slot_id) || slot_id >= FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_slot_check_name(name));

#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  for (uint16_t i = 0; i < runtime->slots.overlay_name_count; i++) {
    const char *stored = NULL;
    uint8_t stored_length = 0;
    uint16_t name_length = 0;

    FR_TRY(fr_slot_name_length(name, &name_length));
    FR_TRY(fr_slot_project_name_entry_view(
        runtime, &runtime->slots.overlay_names[i], &stored,
        &stored_length));
    if (runtime->slots.overlay_names[i].slot_id == slot_id &&
        stored_length == name_length &&
        memcmp(stored, name, name_length) == 0) {
      for (uint16_t j = i + 1; j < runtime->slots.overlay_name_count; j++) {
        fr_slot_project_name_move(runtime, (uint16_t)(j - 1), j);
      }
      runtime->slots.overlay_name_count =
          (uint16_t)(runtime->slots.overlay_name_count - 1);
      fr_slot_project_name_clear(runtime, runtime->slots.overlay_name_count);
      removed_name = true;
      break;
    }
  }
#endif

  if (!removed_name && fr_slot_project_name_points_to(runtime, slot_id)) {
    return FR_ERR_NOT_FOUND;
  }
  runtime->slots.current[slot_id] = runtime->slots.base[slot_id];
  runtime->slots.overlay[slot_id] = false;
  fr_slot_reclaim_free_tail(runtime);
  return FR_OK;
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

  memcpy(runtime->slots.overlay_name_bytes[runtime->slots.overlay_name_count],
         name, (uint16_t)(length + 1));
  runtime->slots.overlay_names[runtime->slots.overlay_name_count] =
      (fr_slot_project_name_entry_t){
          .storage_kind = FR_SLOT_NAME_STORAGE_OVERLAY_RAM,
          .slot_id = slot_id,
          .bytes =
              runtime->slots
                  .overlay_name_bytes[runtime->slots.overlay_name_count],
          .length = (uint8_t)length,
      };
  runtime->slots.overlay_name_count =
      (uint16_t)(runtime->slots.overlay_name_count + 1);
  return FR_OK;
#else
  (void)slot_id;
  return FR_ERR_UNSUPPORTED;
#endif
}

fr_err_t fr_slot_mount_project_name(fr_runtime_t *runtime, const char *name,
                                    uint8_t length, fr_slot_id_t slot_id) {
  fr_slot_id_t existing = 0;
  fr_err_t err = FR_OK;
  char scratch[FR_PROFILE_MAX_NAME_BYTES + 1];

  if (runtime == NULL || name == NULL || length == 0 ||
      length > FR_PROFILE_MAX_NAME_BYTES) {
    return FR_ERR_INVALID;
  }
  for (uint8_t i = 0; i < length; i++) {
    if (name[i] == '\0') {
      return FR_ERR_INVALID;
    }
    scratch[i] = name[i];
  }
  scratch[length] = '\0';

#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  if (slot_id < fr_slot_first_project_id() || slot_id >= runtime->slots.count) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < runtime->slots.overlay_name_count; i++) {
    if (runtime->slots.overlay_names[i].slot_id == slot_id) {
      runtime->slots.overlay_names[i] = (fr_slot_project_name_entry_t){
          .storage_kind = FR_SLOT_NAME_STORAGE_IMAGE,
          .slot_id = slot_id,
          .bytes = name,
          .length = length,
      };
      return FR_OK;
    }
  }

  err = fr_slot_id_for_name(runtime, scratch, &existing);
  if (err == FR_OK) {
    return existing == slot_id ? FR_OK : FR_ERR_INVALID;
  }
  if (err != FR_ERR_NOT_FOUND) {
    return err;
  }
  if (runtime->slots.overlay_name_count >= FR_PROFILE_MAX_OVERLAY_NAMES) {
    return FR_ERR_CAPACITY;
  }
  runtime->slots.overlay_names[runtime->slots.overlay_name_count] =
      (fr_slot_project_name_entry_t){
          .storage_kind = FR_SLOT_NAME_STORAGE_IMAGE,
          .slot_id = slot_id,
          .bytes = name,
          .length = length,
      };
  runtime->slots.overlay_name_count =
      (uint16_t)(runtime->slots.overlay_name_count + 1);
  return FR_OK;
#else
  (void)existing;
  (void)err;
  (void)slot_id;
  return FR_ERR_UNSUPPORTED;
#endif
}

void fr_slot_mark_persist_image(fr_runtime_t *runtime) {
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  if (runtime == NULL) {
    return;
  }
  for (uint16_t i = 0; i < runtime->slots.overlay_name_count; i++) {
    if (runtime->slots.overlay_names[i].storage_kind ==
        FR_SLOT_NAME_STORAGE_IMAGE) {
      runtime->slots.overlay_names[i].storage_kind =
          FR_SLOT_NAME_STORAGE_PERSIST_IMAGE;
    }
  }
#else
  (void)runtime;
#endif
}

void fr_slot_clear_project_names(fr_runtime_t *runtime) {
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  if (runtime == NULL) {
    return;
  }
  for (uint16_t i = 0; i < runtime->slots.overlay_name_count; i++) {
    fr_slot_project_name_clear(runtime, i);
  }
  runtime->slots.overlay_name_count = 0;
#else
  (void)runtime;
#endif
}
