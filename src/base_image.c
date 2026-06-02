#include "base_image.h"

#include "lib_native.h"
#include "native.h"
#include "runtime.h"
#include "slot.h"
#include "tagged.h"

#if FR_FEATURE_SOURCE_BASE
#include "compile.h"
#include "fr_source_base.h"
#include "image.h"

#include <string.h>

enum { FR_SOURCE_BASE_RECORD_MAX = 16 };

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

fr_err_t fr_base_source_slot_id_for_name(const char *name,
                                         fr_slot_id_t *out_slot_id) {
  if (name == NULL || out_slot_id == NULL) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < fr_source_base_slot_count; i++) {
    if (strcmp(fr_source_base_records[i].name, name) == 0) {
      *out_slot_id = fr_source_base_records[i].slot_id;
      return FR_OK;
    }
  }
  return FR_ERR_NOT_FOUND;
}

uint16_t fr_base_source_record_count(void) { return fr_source_base_slot_count; }

fr_err_t fr_base_source_record_slot_id_at(uint16_t index,
                                          fr_slot_id_t *out_slot_id) {
  if (out_slot_id == NULL || index >= fr_source_base_slot_count) {
    return FR_ERR_INVALID;
  }
  *out_slot_id = fr_source_base_records[index].slot_id;
  return FR_OK;
}

static fr_err_t fr_base_compile_source_line(fr_runtime_t *runtime,
                                            const char *line) {
  fr_compile_overlay_update_t compiled = {0};

  FR_TRY(fr_compile_overlay_update_for_runtime(runtime, line, &compiled));
  if (compiled.overlay_update.slot_init_count != 1 ||
      compiled.overlay_update.slot_name_count != 1) {
    return FR_ERR_INVALID;
  }
  fr_slot_id_t slot_id = compiled.overlay_update.slot_inits[0].slot_id;
  FR_TRY(fr_overlay_apply_base(runtime, &compiled.overlay_update));
  return fr_base_source_record_add(slot_id, compiled.slot_name.name);
}

/* Compile base/core.frothy one definition per line into base bindings. */
static fr_err_t fr_base_compile_source(fr_runtime_t *runtime, const char *bytes,
                                       uint16_t length) {
  char line[FR_PROFILE_REPL_LINE_BYTES];
  uint16_t i = 0;

  while (i < length) {
    uint16_t n = 0;
    bool blank = true;

    while (i < length && bytes[i] != '\n') {
      if (n + 1 >= sizeof(line)) {
        return FR_ERR_CAPACITY;
      }
      if (bytes[i] != ' ' && bytes[i] != '\t' && bytes[i] != '\r') {
        blank = false;
      }
      line[n++] = bytes[i++];
    }
    if (i < length) {
      i++;
    }
    line[n] = '\0';
    if (!blank) {
      FR_TRY(fr_base_compile_source_line(runtime, line));
    }
  }
  return FR_OK;
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
  fr_lib_native_records_reset();
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

#if FR_FEATURE_SOURCE_BASE
  FR_TRY(fr_base_compile_source(&next, fr_source_base_bytes,
                                fr_source_base_bytes_len));
#endif

  FR_TRY(fr_lib_natives_install(&next));

  fr_code_mark_base(&next);
  fr_native_mark_base(&next);
  *runtime = next;
  return FR_OK;
}
