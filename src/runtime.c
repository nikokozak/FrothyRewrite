#include "runtime.h"

#include "handle.h"
#include "object.h"
#include "pad.h"
#include "tagged.h"

#include <string.h>

fr_err_t fr_runtime_init(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  for (fr_slot_id_t slot_id = 0; slot_id < FR_PROFILE_MAX_SLOTS; slot_id++) {
    runtime->slots.current[slot_id] = fr_tagged_nil();
    runtime->slots.base[slot_id] = fr_tagged_nil();
    runtime->slots.overlay[slot_id] = false;
  }
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  for (uint16_t i = 0; i < FR_PROFILE_MAX_OVERLAY_NAMES; i++) {
    runtime->slots.overlay_names[i][0] = '\0';
    runtime->slots.overlay_name_slots[i] = 0;
  }
  runtime->slots.overlay_name_count = 0;
#endif
  runtime->slots.base_count = 0;
  runtime->slots.count = 0;
  fr_code_reset(runtime);
  fr_native_reset(runtime);
  fr_object_reset(runtime);
  fr_handle_reset(runtime);
#if FR_FEATURE_PAD
  FR_TRY(fr_pad_reset(runtime));
#endif
  memset(&runtime->events, 0, sizeof(runtime->events));
  runtime->interrupted = false;
  return FR_OK;
}

fr_err_t fr_runtime_clear_project(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  fr_handle_close_all(runtime);
  for (fr_slot_id_t slot_id = 0; slot_id < FR_PROFILE_MAX_SLOTS; slot_id++) {
    runtime->slots.current[slot_id] = runtime->slots.base[slot_id];
    runtime->slots.overlay[slot_id] = false;
  }
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  for (uint16_t i = 0; i < runtime->slots.overlay_name_count; i++) {
    runtime->slots.overlay_names[i][0] = '\0';
    runtime->slots.overlay_name_slots[i] = 0;
  }
  runtime->slots.overlay_name_count = 0;
#endif
  runtime->slots.count = runtime->slots.base_count;
  fr_code_restore_base(runtime);
  fr_native_restore_base(runtime);
  fr_object_restore_base(runtime);
#if FR_FEATURE_PAD
  FR_TRY(fr_pad_reset(runtime));
#endif
  memset(&runtime->events, 0, sizeof(runtime->events));
  runtime->interrupted = false;
  return FR_OK;
}

fr_err_t fr_runtime_reset(fr_runtime_t *runtime) {
  return fr_runtime_clear_project(runtime);
}

void fr_runtime_interrupt(fr_runtime_t *runtime) {
  if (runtime != NULL) {
    runtime->interrupted = true;
  }
}

void fr_runtime_clear_interrupt(fr_runtime_t *runtime) {
  if (runtime != NULL) {
    runtime->interrupted = false;
  }
}

bool fr_runtime_is_interrupted(const fr_runtime_t *runtime) {
  return runtime != NULL && runtime->interrupted;
}

fr_runtime_limits_t fr_runtime_get_limits(void) {
  return (fr_runtime_limits_t){
      .max_slots = FR_PROFILE_MAX_SLOTS,
      .max_instruction_bytes = FR_PROFILE_MAX_INSTRUCTION_BYTES,
      .max_code_objects = FR_PROFILE_CODE_OBJECT_TABLE_SIZE,
      .max_natives = FR_PROFILE_NATIVE_TABLE_SIZE,
      .max_handles = FR_PROFILE_MAX_HANDLES,
      .max_objects = FR_PROFILE_OBJECT_TABLE_SIZE,
      .max_cell_words = FR_PROFILE_MAX_CELL_WORDS,
      .max_text_bytes = FR_PROFILE_MAX_TEXT_BYTES,
      .max_record_name_bytes = FR_PROFILE_MAX_RECORD_NAME_BYTES,
      .max_record_fields_per_shape =
          FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE,
      .max_record_shape_fields = FR_PROFILE_MAX_RECORD_SHAPE_FIELDS,
      .max_record_value_fields = FR_PROFILE_MAX_RECORD_VALUE_FIELDS,
      .max_pad_bytes = FR_PROFILE_PAD_BYTES,
  };
}
