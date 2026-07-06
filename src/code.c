#include "code.h"

#include "runtime.h"

#include <string.h>

void fr_code_reset(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }

  runtime->code.count = 0;
  runtime->code.used_instruction_bytes = 0;
  runtime->code.used_param_name_bytes = 0;
  runtime->code.base_count = 0;
  runtime->code.base_used_instruction_bytes = 0;
  runtime->code.base_used_param_name_bytes = 0;
  memset(runtime->code.entries, 0, sizeof(runtime->code.entries));
  memset(runtime->code.instruction_bytes, 0,
         sizeof(runtime->code.instruction_bytes));
  memset(runtime->code.param_name_bytes, 0,
         sizeof(runtime->code.param_name_bytes));
}

void fr_code_mark_base(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }

  runtime->code.base_count = runtime->code.count;
  runtime->code.base_used_instruction_bytes = runtime->code.used_instruction_bytes;
  runtime->code.base_used_param_name_bytes = runtime->code.used_param_name_bytes;
}

void fr_code_restore_base(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }

  runtime->code.count = runtime->code.base_count;
  runtime->code.used_instruction_bytes = runtime->code.base_used_instruction_bytes;
  runtime->code.used_param_name_bytes = runtime->code.base_used_param_name_bytes;
  memset(&runtime->code.entries[runtime->code.count], 0,
         (FR_PROFILE_CODE_OBJECT_TABLE_SIZE - runtime->code.count) *
             sizeof(runtime->code.entries[0]));
  memset(&runtime->code.instruction_bytes[runtime->code.used_instruction_bytes],
         0,
         FR_PROFILE_MAX_INSTRUCTION_BYTES -
             runtime->code.used_instruction_bytes);
  memset(&runtime->code.param_name_bytes[runtime->code.used_param_name_bytes], 0,
         sizeof(runtime->code.param_name_bytes) -
             runtime->code.used_param_name_bytes);
}

fr_err_t fr_code_install(fr_runtime_t *runtime,
                         const fr_instruction_stream_t *view,
                         const char *param_names,
                         uint16_t param_names_length,
                         fr_code_object_id_t *out_code_object_id) {
  fr_code_object_t *entry = NULL;

  if (runtime == NULL || view == NULL || out_code_object_id == NULL) {
    return FR_ERR_INVALID;
  }
  if (view->length == 0 || view->bytes == NULL) {
    return FR_ERR_INVALID;
  }
  if (param_names == NULL) {
    param_names_length = 0;
  }
  if (runtime->code.count >= FR_PROFILE_CODE_OBJECT_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }

  if ((uint32_t)runtime->code.used_instruction_bytes + view->length >
      FR_PROFILE_MAX_INSTRUCTION_BYTES) {
    return FR_ERR_CAPACITY;
  }
  FR_TRY(fr_verify_code_object(view));
  /* Names are introspection metadata, not semantics. If the shared pool is
   * full, drop them and let the renderer fall back rather than fail a compile
   * the instructions accept. */
  if ((uint32_t)runtime->code.used_param_name_bytes + param_names_length >
      sizeof(runtime->code.param_name_bytes)) {
    param_names_length = 0;
  }

  entry = &runtime->code.entries[runtime->code.count];
  entry->instruction_byte_offset = runtime->code.used_instruction_bytes;
  entry->instruction_byte_length = view->length;
  entry->param_name_byte_offset = runtime->code.used_param_name_bytes;
  entry->param_name_byte_length = param_names_length;
  memcpy(&runtime->code.instruction_bytes[entry->instruction_byte_offset],
         view->bytes, view->length);
  if (param_names_length > 0) {
    memcpy(&runtime->code.param_name_bytes[entry->param_name_byte_offset],
           param_names, param_names_length);
  }

  *out_code_object_id = runtime->code.count;
  runtime->code.count += 1;
  runtime->code.used_instruction_bytes =
      (uint16_t)(runtime->code.used_instruction_bytes + view->length);
  runtime->code.used_param_name_bytes =
      (uint16_t)(runtime->code.used_param_name_bytes + param_names_length);
  return FR_OK;
}

fr_err_t fr_code_get_instructions(const fr_runtime_t *runtime,
                                  fr_code_object_id_t code_object_id,
                                  fr_instruction_stream_t *out_instructions) {
  const fr_code_object_t *entry = NULL;
  uint32_t end = 0;

  if (runtime == NULL || out_instructions == NULL) {
    return FR_ERR_INVALID;
  }
  if (code_object_id >= runtime->code.count) {
    return FR_ERR_NOT_FOUND;
  }

  entry = &runtime->code.entries[code_object_id];
  end =
      (uint32_t)entry->instruction_byte_offset + entry->instruction_byte_length;
  if (end > runtime->code.used_instruction_bytes) {
    return FR_ERR_CORRUPT;
  }

  out_instructions->bytes =
      &runtime->code.instruction_bytes[entry->instruction_byte_offset];
  out_instructions->length = entry->instruction_byte_length;
  return FR_OK;
}

fr_err_t fr_code_get_param_names(const fr_runtime_t *runtime,
                                 fr_code_object_id_t code_object_id,
                                 const char **out_param_names,
                                 uint16_t *out_param_names_length) {
  const fr_code_object_t *entry = NULL;
  uint32_t end = 0;

  if (runtime == NULL || out_param_names == NULL ||
      out_param_names_length == NULL) {
    return FR_ERR_INVALID;
  }
  if (code_object_id >= runtime->code.count) {
    return FR_ERR_NOT_FOUND;
  }

  entry = &runtime->code.entries[code_object_id];
  end = (uint32_t)entry->param_name_byte_offset + entry->param_name_byte_length;
  if (end > runtime->code.used_param_name_bytes) {
    return FR_ERR_CORRUPT;
  }

  *out_param_names =
      &runtime->code.param_name_bytes[entry->param_name_byte_offset];
  *out_param_names_length = entry->param_name_byte_length;
  return FR_OK;
}
