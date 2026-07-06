#pragma once

#include "tagged.h"

enum {
  FR_INSTRUCTION_FORMAT_VERSION = 1u,
  FR_INSTRUCTION_MIN_HEADER_SIZE = 2u,
  FR_INSTRUCTION_ARITY_HEADER_SIZE = 3u,
  FR_INSTRUCTION_LOCALS_HEADER_SIZE = 4u,
  FR_INSTRUCTION_MAX_HEADER_SIZE = FR_INSTRUCTION_LOCALS_HEADER_SIZE,
  FR_INSTRUCTION_INT_OPERAND_BYTES = FR_TAGGED_WORD_BYTES,
  FR_INSTRUCTION_PUSH_INT_SIZE = 1u + FR_INSTRUCTION_INT_OPERAND_BYTES,
  FR_INSTRUCTION_OBJECT_ID_OPERAND_BYTES = 2u,
  FR_INSTRUCTION_PUSH_OBJECT_ID_SIZE =
      1u + FR_INSTRUCTION_OBJECT_ID_OPERAND_BYTES,
  FR_INSTRUCTION_CODE_ID_OPERAND_BYTES = 2u,
  FR_INSTRUCTION_PUSH_CODE_ID_SIZE = 1u + FR_INSTRUCTION_CODE_ID_OPERAND_BYTES,
};

typedef struct fr_instruction_stream_t {
  const uint8_t *bytes;
  uint16_t length;
} fr_instruction_stream_t;

typedef struct fr_instruction_header_t {
  uint8_t format_version;
  uint8_t header_size;
  uint8_t arity;
  uint8_t local_count;
} fr_instruction_header_t;

typedef enum fr_opcode_t {
  FR_OP_RETURN = 0x00,
  FR_OP_LOAD_SLOT = 0x01,
  FR_OP_STORE_SLOT = 0x02,
  FR_OP_PUSH_INT = 0x03,
  FR_OP_CALL_SLOT = 0x04,
  FR_OP_CALL_NATIVE_SLOT = 0x05,
  FR_OP_JUMP = 0x06,
  FR_OP_JUMP_IF_FALSY = 0x07,
  FR_OP_ADD_INT = 0x08,
  FR_OP_DROP = 0x09,
  FR_OP_PUSH_NIL = 0x0A,
  FR_OP_REPEAT_BEGIN = 0x0B,
  FR_OP_REPEAT_NEXT = 0x0C,
  FR_OP_LOAD_ARG = 0x0D,
  FR_OP_CALL_SLOT_ARG = 0x0E,
  FR_OP_LOAD_CELL = 0x0F,
  FR_OP_STORE_CELL = 0x10,
  FR_OP_LOAD_FIELD = 0x11,
  FR_OP_STORE_FIELD = 0x12,
  FR_OP_PUSH_FALSE = 0x13,
  FR_OP_PUSH_TRUE = 0x14,
  FR_OP_LT_INT = 0x15,
  FR_OP_GT_INT = 0x16,
  FR_OP_LE_INT = 0x17,
  FR_OP_GE_INT = 0x18,
  FR_OP_EQ_INT = 0x19,
  FR_OP_NE_INT = 0x1A,
  FR_OP_SUB_INT = 0x1B,
  FR_OP_MUL_INT = 0x1C,
  FR_OP_DIV_INT = 0x1D,
  FR_OP_PUSH_OBJECT_ID = 0x1E,
  FR_OP_LOAD_LOCAL = 0x1F,
  FR_OP_STORE_LOCAL = 0x20,
  FR_OP_PUSH_CODE_ID = 0x21,
  FR_OP_BYTES_RESET = 0x22,
  FR_OP_LOAD_CELL_DYNAMIC = 0x23,
  FR_OP_STORE_CELL_DYNAMIC = 0x24,
} fr_opcode_t;

fr_err_t fr_instruction_stream_init(fr_instruction_stream_t *view,
                                    const uint8_t *bytes, uint16_t length);
fr_err_t fr_instruction_read_header(const fr_instruction_stream_t *view,
                                    fr_instruction_header_t *header);
fr_err_t fr_verify_code_object(const fr_instruction_stream_t *view);
fr_err_t fr_instruction_read_slot_operand(const fr_instruction_stream_t *view,
                                          fr_code_offset_t ip,
                                          fr_slot_id_t *out_slot_id);
fr_err_t fr_instruction_read_int_operand(const fr_instruction_stream_t *view,
                                         fr_code_offset_t ip,
                                         fr_int_t *out_int);
fr_err_t fr_instruction_read_object_id_operand(
    const fr_instruction_stream_t *view, fr_code_offset_t ip,
    fr_object_id_t *out_object_id);
fr_err_t fr_instruction_read_code_id_operand(
    const fr_instruction_stream_t *view, fr_code_offset_t ip,
    fr_code_object_id_t *out_code_object_id);
fr_err_t fr_instruction_read_jump_operand(const fr_instruction_stream_t *view,
                                          fr_code_offset_t ip,
                                          fr_code_offset_t *out_target);
fr_err_t fr_instruction_read_arg_operand(const fr_instruction_stream_t *view,
                                         fr_code_offset_t ip,
                                         uint8_t *out_arg_index);
fr_err_t fr_instruction_read_local_operand(const fr_instruction_stream_t *view,
                                           fr_code_offset_t ip,
                                           uint8_t *out_local_index);
fr_err_t fr_instruction_read_call_slot_arg_operands(
    const fr_instruction_stream_t *view, fr_code_offset_t ip,
    fr_slot_id_t *out_slot_id, uint8_t *out_arg_count);
#if FR_FEATURE_CELLS
fr_err_t fr_instruction_read_cell_operands(const fr_instruction_stream_t *view,
                                           fr_code_offset_t ip,
                                           fr_slot_id_t *out_slot_id,
                                           uint16_t *out_index);
#endif
#if FR_FEATURE_RECORDS
fr_err_t fr_instruction_read_field_operand(const fr_instruction_stream_t *view,
                                           fr_code_offset_t ip,
                                           const uint8_t **out_name,
                                           uint8_t *out_length);
#endif
fr_err_t fr_instruction_disassemble_at(const fr_instruction_stream_t *view,
                                       fr_code_offset_t ip, char *out,
                                       uint16_t out_cap, uint16_t *out_len,
                                       fr_code_offset_t *next_ip);
fr_err_t fr_instruction_stream_disassemble(const fr_instruction_stream_t *view,
                                           char *out, uint16_t out_cap,
                                           uint16_t *out_len);
