#include "code.h"

#include "runtime.h"

#include <string.h>

void fr_code_reset(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }

  memset(&runtime->code, 0, sizeof(runtime->code));
}

void fr_code_clear_overlay(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }

  if (runtime->code.image_count > runtime->code.count) {
    runtime->code.image_count = runtime->code.count;
  }
  memset(&runtime->code.entries[runtime->code.image_count], 0,
         (FR_PROFILE_CODE_OBJECT_TABLE_SIZE - runtime->code.image_count) *
             sizeof(runtime->code.entries[0]));
  memset(runtime->code.overlay_instruction_bytes, 0,
         sizeof(runtime->code.overlay_instruction_bytes));
  memset(runtime->code.overlay_param_name_bytes, 0,
         sizeof(runtime->code.overlay_param_name_bytes));
  runtime->code.count = runtime->code.image_count;
  runtime->code.overlay_used_instruction_bytes = 0;
  runtime->code.overlay_used_param_name_bytes = 0;
}

void fr_code_mark_base(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }

  fr_code_clear_overlay(runtime);
  runtime->code.base_image_count = runtime->code.image_count;
  runtime->code.base_image_used_instruction_bytes =
      runtime->code.image_used_instruction_bytes;
  runtime->code.base_image_used_param_name_bytes =
      runtime->code.image_used_param_name_bytes;
  runtime->code.image_mounted = true;
}

void fr_code_restore_base(fr_runtime_t *runtime) {
  uint16_t keep_count = 0;
  uint16_t keep_instruction_bytes = 0;
  uint16_t keep_param_name_bytes = 0;

  if (runtime == NULL) {
    return;
  }

  fr_code_clear_overlay(runtime);
  keep_count = runtime->code.base_image_count;
  keep_instruction_bytes = runtime->code.base_image_used_instruction_bytes;
  keep_param_name_bytes = runtime->code.base_image_used_param_name_bytes;

  if (keep_count > runtime->code.image_count) {
    keep_count = runtime->code.image_count;
  }
  if (keep_instruction_bytes > runtime->code.image_used_instruction_bytes) {
    keep_instruction_bytes = runtime->code.image_used_instruction_bytes;
  }
  if (keep_param_name_bytes > runtime->code.image_used_param_name_bytes) {
    keep_param_name_bytes = runtime->code.image_used_param_name_bytes;
  }

  memset(&runtime->code.entries[keep_count], 0,
         (FR_PROFILE_CODE_OBJECT_TABLE_SIZE - keep_count) *
             sizeof(runtime->code.entries[0]));
  memset(&runtime->code.image_instruction_bytes[keep_instruction_bytes], 0,
         sizeof(runtime->code.image_instruction_bytes) - keep_instruction_bytes);
  memset(&runtime->code.image_param_name_bytes[keep_param_name_bytes], 0,
         sizeof(runtime->code.image_param_name_bytes) - keep_param_name_bytes);

  runtime->code.image_count = keep_count;
  runtime->code.count = keep_count;
  runtime->code.image_used_instruction_bytes = keep_instruction_bytes;
  runtime->code.image_used_param_name_bytes = keep_param_name_bytes;
}

static fr_err_t fr_code_install_in_store(fr_runtime_t *runtime,
                                         fr_code_storage_kind_t kind,
                                         const fr_instruction_stream_t *view,
                                         const char *param_names,
                                         uint16_t param_names_length,
                                         fr_code_object_id_t *out_code_object_id) {
  fr_code_object_t *entry = NULL;
  uint8_t *instruction_bytes = NULL;
  char *param_name_bytes = NULL;
  uint16_t *used_instruction_bytes = NULL;
  uint16_t *used_param_name_bytes = NULL;
  uint32_t instruction_capacity = 0, param_name_capacity = 0;

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

  switch (kind) {
  case FR_CODE_STORAGE_IMAGE:
    if (runtime->code.count != runtime->code.image_count) {
      return FR_ERR_INVALID;
    }
    instruction_bytes = runtime->code.image_instruction_bytes;
    param_name_bytes = runtime->code.image_param_name_bytes;
    used_instruction_bytes = &runtime->code.image_used_instruction_bytes;
    used_param_name_bytes = &runtime->code.image_used_param_name_bytes;
    instruction_capacity = sizeof(runtime->code.image_instruction_bytes);
    param_name_capacity = sizeof(runtime->code.image_param_name_bytes);
    break;
  case FR_CODE_STORAGE_OVERLAY_RAM:
    instruction_bytes = runtime->code.overlay_instruction_bytes;
    param_name_bytes = runtime->code.overlay_param_name_bytes;
    used_instruction_bytes = &runtime->code.overlay_used_instruction_bytes;
    used_param_name_bytes = &runtime->code.overlay_used_param_name_bytes;
    instruction_capacity = sizeof(runtime->code.overlay_instruction_bytes);
    param_name_capacity = sizeof(runtime->code.overlay_param_name_bytes);
    break;
  default:
    return FR_ERR_INVALID;
  }

  if ((uint32_t)*used_instruction_bytes + view->length > instruction_capacity) {
    return FR_ERR_CAPACITY;
  }
  /* Names are introspection metadata, not semantics. If the selected pool is
   * full, drop them and let the renderer fall back rather than fail a compile
   * the instructions accept. */
  if ((uint32_t)*used_param_name_bytes + param_names_length > param_name_capacity) {
    param_names_length = 0;
  }

  entry = &runtime->code.entries[runtime->code.count];
  entry->storage_kind = kind;
  entry->instruction_byte_offset = *used_instruction_bytes;
  entry->instruction_byte_length = view->length;
  entry->param_name_byte_offset = *used_param_name_bytes;
  entry->param_name_byte_length = param_names_length;
  memcpy(&instruction_bytes[entry->instruction_byte_offset], view->bytes,
         view->length);
  if (param_names_length > 0) {
    memcpy(&param_name_bytes[entry->param_name_byte_offset], param_names,
           param_names_length);
  }

  *out_code_object_id = runtime->code.count;
  runtime->code.count = (uint16_t)(runtime->code.count + 1u);
  *used_instruction_bytes = (uint16_t)((uint32_t)*used_instruction_bytes + view->length);
  *used_param_name_bytes = (uint16_t)((uint32_t)*used_param_name_bytes + param_names_length);
  if (kind == FR_CODE_STORAGE_IMAGE) {
    runtime->code.image_count = runtime->code.count;
  }
  return FR_OK;
}

fr_err_t fr_code_mount_image(fr_runtime_t *runtime,
                             const fr_instruction_stream_t *view,
                             const char *param_names,
                             uint16_t param_names_length,
                             fr_code_object_id_t *out_code_object_id) {
  return fr_code_install_in_store(runtime, FR_CODE_STORAGE_IMAGE, view,
                                  param_names, param_names_length,
                                  out_code_object_id);
}

fr_err_t fr_code_install_overlay(fr_runtime_t *runtime,
                                 const fr_instruction_stream_t *view,
                                 const char *param_names,
                                 uint16_t param_names_length,
                                 fr_code_object_id_t *out_code_object_id) {
  return fr_code_install_in_store(runtime, FR_CODE_STORAGE_OVERLAY_RAM, view,
                                  param_names, param_names_length,
                                  out_code_object_id);
}

fr_err_t fr_code_install(fr_runtime_t *runtime,
                         const fr_instruction_stream_t *view,
                         const char *param_names,
                         uint16_t param_names_length,
                         fr_code_object_id_t *out_code_object_id) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (runtime->code.image_mounted) {
    return fr_code_install_overlay(runtime, view, param_names,
                                   param_names_length, out_code_object_id);
  }
  return fr_code_mount_image(runtime, view, param_names, param_names_length,
                             out_code_object_id);
}

fr_err_t fr_code_get_instructions(const fr_runtime_t *runtime,
                                  fr_code_object_id_t code_object_id,
                                  fr_instruction_stream_t *out_instructions) {
  const fr_code_object_t *entry = NULL;
  const uint8_t *instruction_bytes = NULL;
  uint32_t used_instruction_bytes = 0;
  uint32_t end = 0;

  if (runtime == NULL || out_instructions == NULL) {
    return FR_ERR_INVALID;
  }
  if (code_object_id >= runtime->code.count) {
    return FR_ERR_NOT_FOUND;
  }

  entry = &runtime->code.entries[code_object_id];
  switch (entry->storage_kind) {
  case FR_CODE_STORAGE_IMAGE:
    instruction_bytes = runtime->code.image_instruction_bytes;
    used_instruction_bytes = runtime->code.image_used_instruction_bytes;
    break;
  case FR_CODE_STORAGE_OVERLAY_RAM:
    instruction_bytes = runtime->code.overlay_instruction_bytes;
    used_instruction_bytes = runtime->code.overlay_used_instruction_bytes;
    break;
  default:
    return FR_ERR_CORRUPT;
  }

  end = entry->instruction_byte_offset + entry->instruction_byte_length;
  if (end > used_instruction_bytes) {
    return FR_ERR_CORRUPT;
  }

  out_instructions->bytes =
      &instruction_bytes[entry->instruction_byte_offset];
  out_instructions->length = entry->instruction_byte_length;
  return FR_OK;
}

fr_err_t fr_code_get_param_names(const fr_runtime_t *runtime,
                                 fr_code_object_id_t code_object_id,
                                 const char **out_param_names,
                                 uint16_t *out_param_names_length) {
  const fr_code_object_t *entry = NULL;
  const char *param_name_bytes = NULL;
  uint32_t used_param_name_bytes = 0;
  uint32_t end = 0;

  if (runtime == NULL || out_param_names == NULL ||
      out_param_names_length == NULL) {
    return FR_ERR_INVALID;
  }
  if (code_object_id >= runtime->code.count) {
    return FR_ERR_NOT_FOUND;
  }

  entry = &runtime->code.entries[code_object_id];
  switch (entry->storage_kind) {
  case FR_CODE_STORAGE_IMAGE:
    param_name_bytes = runtime->code.image_param_name_bytes;
    used_param_name_bytes = runtime->code.image_used_param_name_bytes;
    break;
  case FR_CODE_STORAGE_OVERLAY_RAM:
    param_name_bytes = runtime->code.overlay_param_name_bytes;
    used_param_name_bytes = runtime->code.overlay_used_param_name_bytes;
    break;
  default:
    return FR_ERR_CORRUPT;
  }

  end = entry->param_name_byte_offset + entry->param_name_byte_length;
  if (end > used_param_name_bytes) {
    return FR_ERR_CORRUPT;
  }

  *out_param_names = &param_name_bytes[entry->param_name_byte_offset];
  *out_param_names_length = entry->param_name_byte_length;
  return FR_OK;
}
