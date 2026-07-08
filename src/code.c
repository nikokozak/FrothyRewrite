#include "code.h"

#include "platform.h"
#include "runtime.h"

#include <stdint.h>
#include <string.h>

static bool fr_code_pointer_in_range(const void *ptr, uint16_t length,
                                     const void *base, uint16_t used) {
  uintptr_t p = (uintptr_t)ptr;
  uintptr_t b = (uintptr_t)base;

  if (length == 0) {
    return true;
  }
  if (ptr == NULL || base == NULL || p < b) {
    return false;
  }
  return (uint32_t)(p - b) + length <= used;
}

static uint16_t fr_code_read_u16_little_endian(const uint8_t bytes[2]) {
  return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static bool fr_code_storage_kind_valid(fr_code_storage_kind_t kind) {
  return kind == FR_CODE_STORAGE_BASE_RAM || kind == FR_CODE_STORAGE_IMAGE ||
         kind == FR_CODE_STORAGE_PERSIST_IMAGE ||
         kind == FR_CODE_STORAGE_OVERLAY_RAM;
}

static bool fr_code_storage_kind_image(fr_code_storage_kind_t kind) {
  return kind == FR_CODE_STORAGE_IMAGE ||
         kind == FR_CODE_STORAGE_PERSIST_IMAGE;
}

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
  memset(&runtime->code.overlay_instruction_bytes
             [runtime->code.base_ram_used_instruction_bytes],
         0, sizeof(runtime->code.overlay_instruction_bytes) -
                runtime->code.base_ram_used_instruction_bytes);
  memset(&runtime->code.overlay_param_name_bytes
             [runtime->code.base_ram_used_param_name_bytes],
         0, sizeof(runtime->code.overlay_param_name_bytes) -
                runtime->code.base_ram_used_param_name_bytes);
  runtime->code.count = runtime->code.image_count;
  runtime->code.overlay_used_instruction_bytes =
      runtime->code.base_ram_used_instruction_bytes;
  runtime->code.overlay_used_param_name_bytes =
      runtime->code.base_ram_used_param_name_bytes;
}

void fr_code_mark_base(fr_runtime_t *runtime) {
  uint16_t keep_count = 0;

  if (runtime == NULL) {
    return;
  }

  keep_count = runtime->code.image_count;
  if (keep_count > runtime->code.count) {
    keep_count = runtime->code.count;
  }
  memset(&runtime->code.entries[keep_count], 0,
         (FR_PROFILE_CODE_OBJECT_TABLE_SIZE - keep_count) *
             sizeof(runtime->code.entries[0]));
  runtime->code.count = keep_count;
  runtime->code.image_count = keep_count;
  runtime->code.base_image_count = keep_count;
  runtime->code.base_ram_used_instruction_bytes =
      runtime->code.overlay_used_instruction_bytes;
  runtime->code.base_ram_used_param_name_bytes =
      runtime->code.overlay_used_param_name_bytes;
  runtime->code.image_mounted = true;
}

void fr_code_mark_persist_image(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }

  for (fr_code_object_id_t code_id = runtime->code.base_image_count;
       code_id < runtime->code.image_count; code_id++) {
    if (runtime->code.entries[code_id].storage_kind ==
        FR_CODE_STORAGE_IMAGE) {
      runtime->code.entries[code_id].storage_kind =
          FR_CODE_STORAGE_PERSIST_IMAGE;
    }
  }
}

void fr_code_restore_base(fr_runtime_t *runtime) {
  uint16_t keep_count = 0;

  if (runtime == NULL) {
    return;
  }

  fr_code_clear_overlay(runtime);
  keep_count = runtime->code.base_image_count;
  if (keep_count > runtime->code.image_count) {
    keep_count = runtime->code.image_count;
  }

  memset(&runtime->code.entries[keep_count], 0,
         (FR_PROFILE_CODE_OBJECT_TABLE_SIZE - keep_count) *
             sizeof(runtime->code.entries[0]));
  memset(&runtime->code.overlay_instruction_bytes
             [runtime->code.base_ram_used_instruction_bytes],
         0, sizeof(runtime->code.overlay_instruction_bytes) -
                runtime->code.base_ram_used_instruction_bytes);
  memset(&runtime->code.overlay_param_name_bytes
             [runtime->code.base_ram_used_param_name_bytes],
         0, sizeof(runtime->code.overlay_param_name_bytes) -
                runtime->code.base_ram_used_param_name_bytes);

  runtime->code.image_count = keep_count;
  runtime->code.count = keep_count;
  runtime->code.overlay_used_instruction_bytes =
      runtime->code.base_ram_used_instruction_bytes;
  runtime->code.overlay_used_param_name_bytes =
      runtime->code.base_ram_used_param_name_bytes;
}

void fr_code_rebase_ram_pointers(fr_runtime_t *runtime,
                                 const fr_runtime_t *source) {
  if (runtime == NULL || source == NULL) {
    return;
  }

  for (fr_code_object_id_t code_id = 0; code_id < runtime->code.count;
       code_id++) {
    fr_code_object_t *entry = &runtime->code.entries[code_id];

    if (entry->storage_kind != FR_CODE_STORAGE_BASE_RAM &&
        entry->storage_kind != FR_CODE_STORAGE_OVERLAY_RAM) {
      continue;
    }
    if (entry->instruction_bytes != NULL) {
      uintptr_t src = (uintptr_t)source->code.overlay_instruction_bytes;
      uintptr_t ptr = (uintptr_t)entry->instruction_bytes;

      if (ptr >= src) {
        uintptr_t offset = ptr - src;
        if (offset < sizeof(runtime->code.overlay_instruction_bytes)) {
          entry->instruction_bytes =
              &runtime->code.overlay_instruction_bytes[offset];
        }
      }
    }
    if (entry->param_names != NULL) {
      uintptr_t src = (uintptr_t)source->code.overlay_param_name_bytes;
      uintptr_t ptr = (uintptr_t)entry->param_names;

      if (ptr >= src) {
        uintptr_t offset = ptr - src;
        if (offset < sizeof(runtime->code.overlay_param_name_bytes)) {
          entry->param_names = &runtime->code.overlay_param_name_bytes[offset];
        }
      }
    }
  }
}

static fr_err_t
fr_code_install_in_ram(fr_runtime_t *runtime, fr_code_storage_kind_t kind,
                       const fr_instruction_stream_t *view,
                       const char *param_names, uint16_t param_names_length,
                       fr_code_object_id_t *out_code_object_id) {
  fr_code_object_t *entry = NULL;

  if (runtime == NULL || view == NULL || out_code_object_id == NULL) {
    return FR_ERR_INVALID;
  }
  if (view->length == 0 || view->bytes == NULL) {
    return FR_ERR_INVALID;
  }
  if (kind != FR_CODE_STORAGE_BASE_RAM &&
      kind != FR_CODE_STORAGE_OVERLAY_RAM) {
    return FR_ERR_INVALID;
  }
  if (param_names == NULL) {
    param_names_length = 0;
  }
  if (runtime->code.count >= FR_PROFILE_CODE_OBJECT_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }
  if ((uint32_t)runtime->code.overlay_used_instruction_bytes + view->length >
      sizeof(runtime->code.overlay_instruction_bytes)) {
    return FR_ERR_CAPACITY;
  }
  if ((uint32_t)runtime->code.overlay_used_param_name_bytes +
          param_names_length >
      sizeof(runtime->code.overlay_param_name_bytes)) {
    param_names_length = 0;
  }

  entry = &runtime->code.entries[runtime->code.count];
  entry->storage_kind = kind;
  entry->instruction_bytes =
      &runtime->code.overlay_instruction_bytes
           [runtime->code.overlay_used_instruction_bytes];
  entry->instruction_byte_length = view->length;
  entry->instruction_storage_offset = 0;
  entry->param_names =
      &runtime->code.overlay_param_name_bytes
           [runtime->code.overlay_used_param_name_bytes];
  entry->param_name_byte_length = param_names_length;

  memcpy((uint8_t *)entry->instruction_bytes, view->bytes, view->length);
  if (param_names_length > 0) {
    memcpy((char *)entry->param_names, param_names, param_names_length);
  }

  *out_code_object_id = runtime->code.count;
  runtime->code.count = (uint16_t)(runtime->code.count + 1u);
  runtime->code.overlay_used_instruction_bytes =
      (uint16_t)((uint32_t)runtime->code.overlay_used_instruction_bytes +
                 view->length);
  runtime->code.overlay_used_param_name_bytes =
      (uint16_t)((uint32_t)runtime->code.overlay_used_param_name_bytes +
                 param_names_length);
  if (kind == FR_CODE_STORAGE_BASE_RAM) {
    runtime->code.image_count = runtime->code.count;
  }
  return FR_OK;
}

fr_err_t fr_code_install_base(fr_runtime_t *runtime,
                              const fr_instruction_stream_t *view,
                              const char *param_names,
                              uint16_t param_names_length,
                              fr_code_object_id_t *out_code_object_id) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (runtime->code.count != runtime->code.image_count) {
    return FR_ERR_INVALID;
  }
  return fr_code_install_in_ram(runtime, FR_CODE_STORAGE_BASE_RAM, view,
                                param_names, param_names_length,
                                out_code_object_id);
}

fr_err_t fr_code_mount_image(fr_runtime_t *runtime,
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
  if (runtime->code.count != runtime->code.image_count) {
    return FR_ERR_INVALID;
  }
  if (runtime->code.count >= FR_PROFILE_CODE_OBJECT_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }

  entry = &runtime->code.entries[runtime->code.count];
  entry->storage_kind = FR_CODE_STORAGE_IMAGE;
  entry->instruction_bytes = view->bytes;
  entry->instruction_byte_length = view->length;
  entry->instruction_storage_offset = 0;
  entry->param_names = param_names;
  entry->param_name_byte_length = param_names_length;

#if FR_FEATURE_PERSISTENCE
  {
    uint16_t offset = 0;
    fr_err_t offset_err =
        fr_platform_persist_mounted_offset(view->bytes, view->length, &offset);

    if (offset_err == FR_OK) {
      entry->instruction_storage_offset = offset;
      if (!fr_platform_persist_code_pointer_is_direct(view->bytes,
                                                      view->length)) {
        entry->instruction_bytes = NULL;
      }
    }
  }
#endif

  *out_code_object_id = runtime->code.count;
  runtime->code.count = (uint16_t)(runtime->code.count + 1u);
  runtime->code.image_count = runtime->code.count;
  return FR_OK;
}

fr_err_t fr_code_install_overlay(fr_runtime_t *runtime,
                                 const fr_instruction_stream_t *view,
                                 const char *param_names,
                                 uint16_t param_names_length,
                                 fr_code_object_id_t *out_code_object_id) {
  return fr_code_install_in_ram(runtime, FR_CODE_STORAGE_OVERLAY_RAM, view,
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
  return fr_code_install_base(runtime, view, param_names, param_names_length,
                              out_code_object_id);
}

fr_err_t fr_code_get_instructions(const fr_runtime_t *runtime,
                                  fr_code_object_id_t code_object_id,
                                  fr_instruction_stream_t *out_instructions) {
  const fr_code_object_t *entry = NULL;

  if (runtime == NULL || out_instructions == NULL) {
    return FR_ERR_INVALID;
  }
  if (code_object_id >= runtime->code.count) {
    return FR_ERR_NOT_FOUND;
  }

  entry = &runtime->code.entries[code_object_id];
  if (entry->instruction_byte_length == 0) {
    return FR_ERR_CORRUPT;
  }
  if (!fr_code_storage_kind_valid(entry->storage_kind)) {
    return FR_ERR_CORRUPT;
  }
  if (entry->instruction_bytes == NULL) {
    if (!fr_code_storage_kind_image(entry->storage_kind)) {
      return FR_ERR_CORRUPT;
    }

    out_instructions->bytes = NULL;
    out_instructions->length = entry->instruction_byte_length;
    out_instructions->code_id = code_object_id;
    return FR_OK;
  }
  if ((entry->storage_kind == FR_CODE_STORAGE_BASE_RAM ||
       entry->storage_kind == FR_CODE_STORAGE_OVERLAY_RAM) &&
      !fr_code_pointer_in_range(entry->instruction_bytes,
                                entry->instruction_byte_length,
                                runtime->code.overlay_instruction_bytes,
                                runtime->code.overlay_used_instruction_bytes)) {
    return FR_ERR_CORRUPT;
  }
#if FR_FEATURE_PERSISTENCE
  if (entry->storage_kind == FR_CODE_STORAGE_PERSIST_IMAGE &&
      !fr_platform_persist_pointer_is_mounted(entry->instruction_bytes,
                                              entry->instruction_byte_length)) {
    return FR_ERR_CORRUPT;
  }
#endif

  out_instructions->bytes = entry->instruction_bytes;
  out_instructions->length = entry->instruction_byte_length;
  out_instructions->code_id = code_object_id;
  return FR_OK;
}

fr_err_t fr_code_read(const fr_runtime_t *runtime,
                      fr_code_object_id_t code_object_id, uint16_t offset,
                      uint8_t *dst, uint16_t len) {
  fr_instruction_stream_t view;
  const fr_code_object_t *entry = NULL;

  if ((dst == NULL && len > 0) || runtime == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_code_get_instructions(runtime, code_object_id, &view));
  if (offset > view.length || len > view.length - offset) {
    return FR_ERR_RANGE;
  }
  if (len == 0) {
    return FR_OK;
  }
  if (view.bytes != NULL) {
    memcpy(dst, &view.bytes[offset], len);
    return FR_OK;
  }

  entry = &runtime->code.entries[code_object_id];
#if FR_FEATURE_PERSISTENCE
  if (fr_code_storage_kind_image(entry->storage_kind)) {
    uint32_t storage_offset =
        (uint32_t)entry->instruction_storage_offset + offset;

    if (storage_offset > UINT16_MAX) {
      return FR_ERR_RANGE;
    }
    return fr_platform_persist_read_mounted((uint16_t)storage_offset, dst, len);
  }
#else
  (void)entry;
#endif
  return FR_ERR_CORRUPT;
}

fr_err_t fr_code_read_u8(const fr_runtime_t *runtime,
                         fr_code_object_id_t code_object_id, uint16_t offset,
                         uint8_t *out) {
  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  return fr_code_read(runtime, code_object_id, offset, out, 1);
}

fr_err_t fr_code_read_u16(const fr_runtime_t *runtime,
                          fr_code_object_id_t code_object_id, uint16_t offset,
                          uint16_t *out) {
  uint8_t bytes[2];

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_code_read(runtime, code_object_id, offset, bytes,
                      (uint16_t)sizeof(bytes)));
  *out = fr_code_read_u16_little_endian(bytes);
  return FR_OK;
}

fr_err_t fr_code_get_param_names(const fr_runtime_t *runtime,
                                 fr_code_object_id_t code_object_id,
                                 const char **out_param_names,
                                 uint16_t *out_param_names_length) {
  const fr_code_object_t *entry = NULL;

  if (runtime == NULL || out_param_names == NULL ||
      out_param_names_length == NULL) {
    return FR_ERR_INVALID;
  }
  if (code_object_id >= runtime->code.count) {
    return FR_ERR_NOT_FOUND;
  }

  entry = &runtime->code.entries[code_object_id];
  if (entry->param_name_byte_length > 0 && entry->param_names == NULL) {
    return FR_ERR_CORRUPT;
  }
  if ((entry->storage_kind == FR_CODE_STORAGE_BASE_RAM ||
       entry->storage_kind == FR_CODE_STORAGE_OVERLAY_RAM) &&
      !fr_code_pointer_in_range(entry->param_names,
                                entry->param_name_byte_length,
                                runtime->code.overlay_param_name_bytes,
                                runtime->code.overlay_used_param_name_bytes)) {
    return FR_ERR_CORRUPT;
  }
#if FR_FEATURE_PERSISTENCE
  if (entry->storage_kind == FR_CODE_STORAGE_PERSIST_IMAGE &&
      !fr_platform_persist_pointer_is_mounted(entry->param_names,
                                              entry->param_name_byte_length)) {
    return FR_ERR_CORRUPT;
  }
#endif
  if (entry->storage_kind != FR_CODE_STORAGE_BASE_RAM &&
      entry->storage_kind != FR_CODE_STORAGE_IMAGE &&
      entry->storage_kind != FR_CODE_STORAGE_PERSIST_IMAGE &&
      entry->storage_kind != FR_CODE_STORAGE_OVERLAY_RAM) {
    return FR_ERR_CORRUPT;
  }

  *out_param_names = entry->param_names;
  *out_param_names_length = entry->param_name_byte_length;
  return FR_OK;
}
