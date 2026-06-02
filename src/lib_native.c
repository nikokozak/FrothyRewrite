#include "lib_native.h"

#include "native.h"
#include "slot.h"
#include "tagged.h"

#include <string.h>

/* Empty defaults so kernel-only builds (no frothy.toml driving the generator)
   link cleanly. The build defines FR_LIB_NATIVES_PROVIDED when the generator
   emits a strong override at .frothy/build/<target>/lib_natives.c; without
   that define, this file provides the strong empty defaults instead. Switched
   from __attribute__((weak)) because weak-in-archive lost to non-weak-in-other-
   archive-object at link time on ESP-IDF (the weak def satisfied the
   reference before the linker pulled the strong override's .obj). */
#ifndef FR_LIB_NATIVES_PROVIDED
const fr_lib_native_def_t fr_lib_natives[1] = {{0}};
const uint16_t fr_lib_natives_count = 0;
#endif

/* Name records for the slots the install loop bound. Static lifetime mirrors
   the base name table so reset/restore can't drop them. def->name points to
   the link-time string in fr_lib_natives[], so we borrow rather than copy. */
enum { FR_LIB_NATIVE_RECORD_MAX = 64 };

typedef struct {
  fr_slot_id_t slot_id;
  const char *name;
} fr_lib_native_record_t;

static fr_lib_native_record_t fr_lib_native_records[FR_LIB_NATIVE_RECORD_MAX];
static uint16_t fr_lib_native_record_count_n;

static fr_err_t fr_lib_native_record_add(fr_slot_id_t slot_id,
                                         const char *name) {
  if (fr_lib_native_record_count_n >= FR_LIB_NATIVE_RECORD_MAX) {
    return FR_ERR_CAPACITY;
  }
  fr_lib_native_records[fr_lib_native_record_count_n].slot_id = slot_id;
  fr_lib_native_records[fr_lib_native_record_count_n].name = name;
  fr_lib_native_record_count_n =
      (uint16_t)(fr_lib_native_record_count_n + 1);
  return FR_OK;
}

const char *fr_lib_native_slot_name(fr_slot_id_t slot_id) {
  for (uint16_t i = 0; i < fr_lib_native_record_count_n; i++) {
    if (fr_lib_native_records[i].slot_id == slot_id) {
      return fr_lib_native_records[i].name;
    }
  }
  return NULL;
}

fr_err_t fr_lib_native_slot_id_for_name(const char *name,
                                        fr_slot_id_t *out_slot_id) {
  if (name == NULL || out_slot_id == NULL) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < fr_lib_native_record_count_n; i++) {
    const char *record_name = fr_lib_native_records[i].name;
    if (record_name != NULL && strcmp(record_name, name) == 0) {
      *out_slot_id = fr_lib_native_records[i].slot_id;
      return FR_OK;
    }
  }
  return FR_ERR_NOT_FOUND;
}

uint16_t fr_lib_native_record_count(void) {
  return fr_lib_native_record_count_n;
}

fr_err_t fr_lib_native_record_slot_id_at(uint16_t index,
                                         fr_slot_id_t *out_slot_id) {
  if (out_slot_id == NULL || index >= fr_lib_native_record_count_n) {
    return FR_ERR_INVALID;
  }
  *out_slot_id = fr_lib_native_records[index].slot_id;
  return FR_OK;
}

void fr_lib_native_records_reset(void) { fr_lib_native_record_count_n = 0; }

fr_err_t fr_lib_natives_install(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < fr_lib_natives_count; i++) {
    const fr_lib_native_def_t *def = &fr_lib_natives[i];
    fr_native_id_t native_id = 0;
    fr_slot_id_t slot_id = 0;
    fr_tagged_t tagged = 0;

    if (def->name == NULL || def->fn == NULL) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_native_install(runtime, def->fn, def->arity, NULL, &native_id));
    FR_TRY(fr_slot_prepare_project_name(runtime, def->name, &slot_id));
    FR_TRY(fr_tagged_encode_native_id(native_id, &tagged));
    FR_TRY(fr_slot_set_base(runtime, slot_id, tagged));
    FR_TRY(fr_lib_native_record_add(slot_id, def->name));
  }
  return FR_OK;
}
