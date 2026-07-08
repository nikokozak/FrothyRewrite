#pragma once

#include "instruction.h"
#include "types.h"

#include <stdbool.h>

typedef enum fr_code_storage_kind_t {
  FR_CODE_STORAGE_BASE_RAM = 0,
  FR_CODE_STORAGE_IMAGE = 1,
  FR_CODE_STORAGE_OVERLAY_RAM = 2,
  FR_CODE_STORAGE_PERSIST_IMAGE = 3,
} fr_code_storage_kind_t;

typedef struct fr_code_object_t {
  fr_code_storage_kind_t storage_kind;
  const uint8_t *instruction_bytes;
  uint16_t instruction_byte_length;
  uint16_t instruction_storage_offset;
  const char *param_names;
  uint16_t param_name_byte_length;
} fr_code_object_t;

typedef struct fr_code_table_t {
  /*
   * Code ids are table indexes. The stable partition is:
   * [0,base_image_count) permanent base image code,
   * [base_image_count,image_count) restored image code,
   * [image_count,count) live overlay code.
   * restore_base may drop restored image code and overlay code; clear_overlay
   * may drop only the overlay range.
   */
  fr_code_object_t entries[FR_PROFILE_CODE_OBJECT_TABLE_SIZE];
  uint8_t overlay_instruction_bytes[FR_PROFILE_MAX_OVERLAY_CODE_BYTES > 0
                                        ? FR_PROFILE_MAX_OVERLAY_CODE_BYTES
                                        : 1];
  char overlay_param_name_bytes[FR_PROFILE_MAX_OVERLAY_PARAM_NAME_BYTES > 0
                                    ? FR_PROFILE_MAX_OVERLAY_PARAM_NAME_BYTES
                                    : 1];
  uint16_t overlay_used_instruction_bytes;
  uint16_t overlay_used_param_name_bytes;
  uint16_t base_ram_used_instruction_bytes;
  uint16_t base_ram_used_param_name_bytes;
  uint16_t count;
  uint16_t image_count;
  uint16_t base_image_count;
  bool image_mounted;
} fr_code_table_t;

void fr_code_reset(fr_runtime_t *runtime);
void fr_code_mark_base(fr_runtime_t *runtime);
void fr_code_mark_persist_image(fr_runtime_t *runtime);
void fr_code_restore_base(fr_runtime_t *runtime);
void fr_code_rebase_ram_pointers(fr_runtime_t *runtime,
                                 const fr_runtime_t *source);
fr_err_t fr_code_install_base(fr_runtime_t *runtime,
                              const fr_instruction_stream_t *view,
                              const char *param_names,
                              uint16_t param_names_length,
                              fr_code_object_id_t *out_code_object_id);
fr_err_t fr_code_mount_image(fr_runtime_t *runtime,
                             const fr_instruction_stream_t *view,
                             const char *param_names,
                             uint16_t param_names_length,
                             fr_code_object_id_t *out_code_object_id);
fr_err_t fr_code_install_overlay(fr_runtime_t *runtime,
                                 const fr_instruction_stream_t *view,
                                 const char *param_names,
                                 uint16_t param_names_length,
                                 fr_code_object_id_t *out_code_object_id);
void fr_code_clear_overlay(fr_runtime_t *runtime);
/* param_names is a NUL-separated run of one name per arg, in order; pass NULL
 * with param_names_length 0 when no names are known (the renderer falls back). */
fr_err_t fr_code_install(fr_runtime_t *runtime,
                         const fr_instruction_stream_t *view,
                         const char *param_names,
                         uint16_t param_names_length,
                         fr_code_object_id_t *out_code_object_id);
fr_err_t fr_code_get_instructions(const fr_runtime_t *runtime,
                                  fr_code_object_id_t code_object_id,
                                  fr_instruction_stream_t *out_instructions);
fr_err_t fr_code_read(const fr_runtime_t *runtime,
                      fr_code_object_id_t code_object_id, uint16_t offset,
                      uint8_t *dst, uint16_t len);
fr_err_t fr_code_read_u8(const fr_runtime_t *runtime,
                         fr_code_object_id_t code_object_id, uint16_t offset,
                         uint8_t *out);
fr_err_t fr_code_read_u16(const fr_runtime_t *runtime,
                          fr_code_object_id_t code_object_id, uint16_t offset,
                          uint16_t *out);
fr_err_t fr_code_get_param_names(const fr_runtime_t *runtime,
                                 fr_code_object_id_t code_object_id,
                                 const char **out_param_names,
                                 uint16_t *out_param_names_length);
