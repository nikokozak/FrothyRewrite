#pragma once

#include "instruction.h"
#include "types.h"

typedef struct fr_code_object_t {
  uint16_t instruction_byte_offset;
  uint16_t instruction_byte_length;
} fr_code_object_t;

typedef struct fr_code_table_t {
  fr_code_object_t entries[FR_PROFILE_CODE_OBJECT_TABLE_SIZE];
  uint8_t instruction_bytes[FR_PROFILE_MAX_INSTRUCTION_BYTES];
  uint16_t count;
  uint16_t used_instruction_bytes;
  uint16_t base_count;
  uint16_t base_used_instruction_bytes;
} fr_code_table_t;

void fr_code_reset(fr_runtime_t *runtime);
void fr_code_mark_base(fr_runtime_t *runtime);
void fr_code_restore_base(fr_runtime_t *runtime);
fr_err_t fr_code_install(fr_runtime_t *runtime,
                         const fr_instruction_stream_t *view,
                         fr_code_object_id_t *out_code_object_id);
fr_err_t fr_code_get_instructions(const fr_runtime_t *runtime,
                                  fr_code_object_id_t code_object_id,
                                  fr_instruction_stream_t *out_instructions);
