#include "base_image.h"

#include "native.h"
#include "runtime.h"
#include "slot.h"
#include "tagged.h"

#if FR_FEATURE_SOURCE_BASE
#include <string.h>

enum { FR_SOURCE_BASE_RECORD_MAX = 8 };

typedef struct {
  fr_slot_id_t slot_id;
  char name[FR_PROFILE_MAX_NAME_BYTES + 1];
} fr_source_base_record_t;

static fr_source_base_record_t fr_source_base_records[FR_SOURCE_BASE_RECORD_MAX];
static uint16_t fr_source_base_slot_count;

void fr_base_source_record_reset(void) { fr_source_base_slot_count = 0; }

fr_err_t fr_base_source_record_add(fr_slot_id_t slot_id, const char *name) {
  size_t name_len;

  if (name == NULL) {
    return FR_ERR_INVALID;
  }
  name_len = strlen(name);
  if (name_len > FR_PROFILE_MAX_NAME_BYTES) {
    return FR_ERR_CAPACITY;
  }
  if (fr_source_base_slot_count >= FR_SOURCE_BASE_RECORD_MAX) {
    return FR_ERR_CAPACITY;
  }
  fr_source_base_record_t *record =
      &fr_source_base_records[fr_source_base_slot_count++];
  record->slot_id = slot_id;
  memcpy(record->name, name, name_len + 1);
  return FR_OK;
}

bool fr_base_is_source_slot(fr_slot_id_t slot_id) {
  for (uint16_t i = 0; i < fr_source_base_slot_count; i++) {
    if (fr_source_base_records[i].slot_id == slot_id) {
      return true;
    }
  }
  return false;
}

const char *fr_base_source_slot_name(fr_slot_id_t slot_id) {
  for (uint16_t i = 0; i < fr_source_base_slot_count; i++) {
    if (fr_source_base_records[i].slot_id == slot_id) {
      return fr_source_base_records[i].name;
    }
  }
  return NULL;
}
#endif

static fr_err_t fr_base_install_def(fr_runtime_t *runtime,
                                    const fr_base_def_t *def) {
  fr_tagged_t tagged = 0;

  if (def == NULL) {
    return FR_ERR_INVALID;
  }

  if (def->kind == FR_BASE_DEF_LITERAL) {
    if (!fr_tagged_is_valid(def->literal_tagged)) {
      return FR_ERR_INVALID;
    }
    return fr_slot_set_base(runtime, def->slot_id, def->literal_tagged);
  }

  if (def->kind == FR_BASE_DEF_NATIVE) {
    fr_native_id_t native_id = 0;
    const fr_native_signature_t *signature = NULL;

#if FR_FEATURE_NATIVE_SIGNATURES
    signature = def->native_signature;
#endif
    FR_TRY(fr_native_install(runtime, def->native_fn, def->native_arity,
                             signature, &native_id));
    FR_TRY(fr_tagged_encode_native_id(native_id, &tagged));
    return fr_slot_set_base(runtime, def->slot_id, tagged);
  }

  return FR_ERR_INVALID;
}

fr_err_t fr_base_image_install(fr_runtime_t *runtime) {
  fr_runtime_t next;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

#if FR_FEATURE_SOURCE_BASE
  fr_base_source_record_reset();
#endif
  FR_TRY(fr_runtime_init(&next));
  for (uint16_t layer = 0; layer < fr_base_def_layer_count(); layer++) {
    fr_base_def_layer_t def_layer = {0};

    FR_TRY(fr_base_def_layer_at(layer, &def_layer));
    if (def_layer.count > 0 && def_layer.defs == NULL) {
      return FR_ERR_INVALID;
    }
    for (uint16_t i = 0; i < def_layer.count; i++) {
      FR_TRY(fr_base_install_def(&next, &def_layer.defs[i]));
    }
  }

  fr_code_mark_base(&next);
  fr_native_mark_base(&next);
  *runtime = next;
  return FR_OK;
}
