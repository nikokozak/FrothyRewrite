#include "lib_native.h"

#include "native.h"
#include "slot.h"
#include "tagged.h"

/* Empty defaults so kernel-only builds (no frothy.toml driving the generator)
   link cleanly. The generator emits strong overrides at
   .frothy/build/<target>/lib_natives.c when libraries are resolved. */
__attribute__((weak)) const fr_lib_native_def_t fr_lib_natives[1] = {{0}};
__attribute__((weak)) const uint16_t fr_lib_natives_count = 0;

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
    FR_TRY(fr_slot_bind_project_name(runtime, def->name, slot_id));
  }
  return FR_OK;
}
