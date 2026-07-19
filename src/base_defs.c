#include "base_defs.h"

#if FR_FEATURE_PERSISTENCE
#include "persist.h"
#endif
#include "tagged.h"

#include <stdbool.h>
#include <string.h>

enum {
  FR_BASE_REQUIRED_LAYER_COUNT = 3,
  FR_BASE_PERSISTENCE_LAYER_COUNT = FR_BASE_REQUIRED_LAYER_COUNT + 1,
};

#if FR_FEATURE_PERSISTENCE
static fr_err_t fr_native_save(fr_runtime_t *runtime, const fr_tagged_t *args,
                               uint8_t arg_count, fr_tagged_t *out) {
  fr_err_t err = FR_OK;

  if (args == NULL && arg_count > 0) {
    return FR_ERR_INVALID;
  }
  if (arg_count != 0) {
    return FR_ERR_INVALID;
  }

  err = fr_persist_save(runtime);
  if (err == FR_ERR_VOLATILE && runtime != NULL && runtime->diag != NULL) {
    if (runtime->diag->kind == FR_DIAG_NONE) {
      runtime->diag->kind = FR_DIAG_NOTE;
      runtime->diag->message_id = FR_DIAG_MSG_RUNTIME_SLOT_UNPERSISTABLE;
      runtime->diag->got = FR_DIAG_UNPERSISTABLE_UNSUPPORTED_VALUE;
    }
    runtime->diag->presentation = FR_DIAG_PRESENT_NOTICE;
  }
  FR_TRY(err);
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_restore(fr_runtime_t *runtime, const fr_tagged_t *args,
                                  uint8_t arg_count, fr_tagged_t *out) {
  if (args == NULL && arg_count > 0) {
    return FR_ERR_INVALID;
  }
  if (arg_count != 0) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_persist_restore(runtime));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_wipe(fr_runtime_t *runtime, const fr_tagged_t *args,
                               uint8_t arg_count, fr_tagged_t *out) {
  if (args == NULL && arg_count > 0) {
    return FR_ERR_INVALID;
  }
  if (arg_count != 0) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_persist_wipe(runtime));
  *out = fr_tagged_nil();
  return FR_OK;
}
#endif

static const fr_base_def_t fr_core_base_defs[] = {
    {
        .slot_id = FR_SLOT_BOOT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "boot",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TAGGED_NIL,
    },
    {
        .slot_id = FR_SLOT_ONE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "one",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TAGGED_INT_LITERAL(1),
    },
};

#if FR_FEATURE_PERSISTENCE && FR_FEATURE_NATIVE_SIGNATURES
static const fr_native_signature_t fr_native_save_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "write the current slot image to persistent storage",
};

static const fr_native_signature_t fr_native_nil_to_nil_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_NIL,
    .help = NULL,
};
#endif

#if FR_FEATURE_PERSISTENCE
static const fr_base_def_t fr_persistence_base_defs[] = {
    {
        .slot_id = FR_SLOT_SAVE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "save",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_save,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_save_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_RESTORE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "restore",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_restore,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_nil_to_nil_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_WIPE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "dangerous.wipe",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_wipe,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_nil_to_nil_signature,
#endif
    },
};
#endif

uint16_t fr_base_def_layer_count(void) {
#if FR_FEATURE_PERSISTENCE
  return FR_BASE_PERSISTENCE_LAYER_COUNT;
#else
  return FR_BASE_REQUIRED_LAYER_COUNT;
#endif
}

fr_err_t fr_base_def_layer_at(uint16_t index,
                              fr_base_def_layer_t *out_layer) {
  if (out_layer == NULL) {
    return FR_ERR_INVALID;
  }

  switch (index) {
  case 0:
    *out_layer = (fr_base_def_layer_t){
        .layer = FR_BASE_LAYER_CORE,
        .defs = fr_core_base_defs,
        .count = (uint16_t)(sizeof(fr_core_base_defs) /
                            sizeof(fr_core_base_defs[0])),
    };
    return FR_OK;
  case 1:
    *out_layer = (fr_base_def_layer_t){
        .layer = FR_BASE_LAYER_TARGET,
        .defs = fr_target_base_defs,
        .count = fr_target_base_def_count,
    };
    return FR_OK;
  case 2:
    *out_layer = (fr_base_def_layer_t){
        .layer = FR_BASE_LAYER_BOARD,
        .defs = fr_board_base_defs,
        .count = fr_board_base_def_count,
    };
    return FR_OK;
#if FR_FEATURE_PERSISTENCE
  case 3:
    *out_layer = (fr_base_def_layer_t){
        .layer = FR_BASE_LAYER_PERSISTENCE,
        .defs = fr_persistence_base_defs,
        .count = (uint16_t)(sizeof(fr_persistence_base_defs) /
                            sizeof(fr_persistence_base_defs[0])),
    };
    return FR_OK;
#endif
  default:
    return FR_ERR_NOT_FOUND;
  }
}

uint16_t fr_base_def_count(void) {
  uint16_t count = 0;

  for (uint16_t layer = 0; layer < fr_base_def_layer_count(); layer++) {
    fr_base_def_layer_t def_layer = {0};

    if (fr_base_def_layer_at(layer, &def_layer) != FR_OK) {
      return count;
    }
    count = (uint16_t)(count + def_layer.count);
  }

  return count;
}

fr_err_t fr_base_def_at(uint16_t index, const fr_base_def_t **out_def,
                        fr_base_layer_t *out_layer) {
  if (out_def == NULL || out_layer == NULL) {
    return FR_ERR_INVALID;
  }

  for (uint16_t layer = 0; layer < fr_base_def_layer_count(); layer++) {
    fr_base_def_layer_t def_layer = {0};

    FR_TRY(fr_base_def_layer_at(layer, &def_layer));
    if (def_layer.count > 0 && def_layer.defs == NULL) {
      return FR_ERR_INVALID;
    }
    if (index < def_layer.count) {
      *out_def = &def_layer.defs[index];
      *out_layer = def_layer.layer;
      return FR_OK;
    }

    index = (uint16_t)(index - def_layer.count);
  }

  return FR_ERR_NOT_FOUND;
}

fr_err_t fr_base_def_for_slot(fr_slot_id_t slot_id,
                              const fr_base_def_t **out_def,
                              fr_base_layer_t *out_layer) {
  if (out_def == NULL || out_layer == NULL) {
    return FR_ERR_INVALID;
  }

  for (uint16_t i = 0; i < fr_base_def_count(); i++) {
    const fr_base_def_t *def = NULL;
    fr_base_layer_t layer = FR_BASE_LAYER_CORE;

    FR_TRY(fr_base_def_at(i, &def, &layer));
    if (def->slot_id == slot_id) {
      *out_def = def;
      *out_layer = layer;
      return FR_OK;
    }
  }

  return FR_ERR_NOT_FOUND;
}

uint16_t fr_base_slot_count(void) {
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  return fr_base_def_count();
#else
  return 0;
#endif
}

const char *fr_base_slot_name_at(uint16_t index) {
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  const fr_base_def_t *selected = NULL;
  fr_slot_id_t previous_slot_id = 0;
  bool has_previous = false;

  if (index >= fr_base_slot_count()) {
    return NULL;
  }

  for (uint16_t selected_index = 0; selected_index <= index; selected_index++) {
    const fr_base_def_t *best = NULL;
    fr_slot_id_t best_slot_id = 0;

    for (uint16_t i = 0; i < fr_base_def_count(); i++) {
      const fr_base_def_t *def = NULL;
      fr_base_layer_t layer = FR_BASE_LAYER_CORE;

      if (fr_base_def_at(i, &def, &layer) != FR_OK) {
        return NULL;
      }
      (void)layer;
      if (has_previous && def->slot_id <= previous_slot_id) {
        continue;
      }
      if (best == NULL || def->slot_id < best_slot_id) {
        best = def;
        best_slot_id = def->slot_id;
      }
    }

    if (best == NULL) {
      return NULL;
    }
    selected = best;
    previous_slot_id = best_slot_id;
    has_previous = true;
  }

  return selected->name;
#else
  (void)index;
  return NULL;
#endif
}

const char *fr_base_slot_name(fr_slot_id_t slot_id) {
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  const fr_base_def_t *def = NULL;
  fr_base_layer_t layer = FR_BASE_LAYER_CORE;

  if (fr_base_def_for_slot(slot_id, &def, &layer) == FR_OK) {
    (void)layer;
    return def->name;
  }
#else
  (void)slot_id;
#endif
#if FR_FEATURE_SOURCE_BASE
  return fr_base_source_slot_name(slot_id);
#else
  return NULL;
#endif
}

fr_err_t fr_base_slot_id_for_name(const char *name, fr_slot_id_t *out_slot_id) {
  if (name == NULL || out_slot_id == NULL) {
    return FR_ERR_INVALID;
  }
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  for (uint16_t i = 0; i < fr_base_def_count(); i++) {
    const fr_base_def_t *def = NULL;
    fr_base_layer_t layer = FR_BASE_LAYER_CORE;

    if (fr_base_def_at(i, &def, &layer) != FR_OK) {
      return FR_ERR_INVALID;
    }
    if (strcmp(def->name, name) == 0 ||
        (def->alias != NULL && strcmp(def->alias, name) == 0)) {
      (void)layer;
      *out_slot_id = def->slot_id;
      return FR_OK;
    }
  }
#if FR_FEATURE_SOURCE_BASE
  return fr_base_source_slot_id_for_name(name, out_slot_id);
#else
  return FR_ERR_NOT_FOUND;
#endif
#else
  return FR_ERR_UNSUPPORTED;
#endif
}

fr_err_t fr_base_slot_layer(fr_slot_id_t slot_id, fr_base_layer_t *out_layer) {
  const fr_base_def_t *def = NULL;

  if (out_layer == NULL) {
    return FR_ERR_INVALID;
  }

  if (fr_base_def_for_slot(slot_id, &def, out_layer) == FR_OK) {
    return FR_OK;
  }
#if FR_FEATURE_SOURCE_BASE
  if (fr_base_is_source_slot(slot_id)) {
    *out_layer = FR_BASE_LAYER_SOURCE;
    return FR_OK;
  }
#endif
  return FR_ERR_NOT_FOUND;
}

fr_err_t fr_base_slot_ref(fr_slot_id_t slot_id, fr_image_ref_t *out_ref) {
  uint16_t native_index = 0;

  if (out_ref == NULL) {
    return FR_ERR_INVALID;
  }

  for (uint16_t i = 0; i < fr_base_def_count(); i++) {
    const fr_base_def_t *def = NULL;
    fr_base_layer_t layer = FR_BASE_LAYER_CORE;

    FR_TRY(fr_base_def_at(i, &def, &layer));
    if (def->slot_id == slot_id) {
      if (def->kind == FR_BASE_DEF_LITERAL) {
        *out_ref = (fr_image_ref_t){FR_IMAGE_REF_LITERAL_TAGGED,
                                    def->literal_tagged, 0};
        return FR_OK;
      }
      if (def->kind == FR_BASE_DEF_NATIVE) {
        *out_ref = (fr_image_ref_t){FR_IMAGE_REF_NATIVE, 0, native_index};
        return FR_OK;
      }
      return FR_ERR_INVALID;
    }

    if (def->kind == FR_BASE_DEF_NATIVE) {
      native_index += 1;
    }
  }

  return FR_ERR_NOT_FOUND;
}
