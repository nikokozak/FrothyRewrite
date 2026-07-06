/*
 * An instruction stream is a borrowed byte view. Operands are raw fields; the
 * VM encodes an operand only when it must become a tagged word. This module
 * reads and renders bytes; profile trust policy owns semantic validation.
 */

#include "instruction.h"

#include "tagged.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint16_t fr_read_u16_little_endian(const uint8_t *bytes) {
  return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static fr_int_t fr_read_int_operand_little_endian(const uint8_t *bytes) {
  return (fr_int_t)(int32_t)fr_read_u32_le(bytes);
}

static fr_err_t fr_require_bytes(const fr_instruction_stream_t *view,
                                 fr_code_offset_t ip, uint16_t width) {
  if (view == NULL) {
    return FR_ERR_INVALID;
  }
  if (ip > view->length || width > view->length - ip) {
    return FR_ERR_INVALID;
  }
  return FR_OK;
}

static fr_err_t fr_append_char(char *out, uint16_t out_cap, uint16_t *used,
                               char ch) {
  if (out == NULL || out_cap == 0 || used == NULL) {
    return FR_ERR_INVALID;
  }
  if (*used >= out_cap - 1u) {
    return FR_ERR_CAPACITY;
  }

  out[*used] = ch;
  *used = (uint16_t)(*used + 1u);
  out[*used] = '\0';
  return FR_OK;
}

static fr_err_t fr_append_text(char *out, uint16_t out_cap, uint16_t *used,
                               const char *text) {
  if (text == NULL) {
    return FR_ERR_INVALID;
  }
  while (*text != '\0') {
    FR_TRY(fr_append_char(out, out_cap, used, *text));
    text += 1;
  }
  return FR_OK;
}

#if FR_FEATURE_RECORDS
static fr_err_t fr_append_bytes_as_text(char *out, uint16_t out_cap,
                                        uint16_t *used, const uint8_t *bytes,
                                        uint8_t length) {
  if (bytes == NULL && length > 0) {
    return FR_ERR_INVALID;
  }
  for (uint8_t i = 0; i < length; i++) {
    FR_TRY(fr_append_char(out, out_cap, used, (char)bytes[i]));
  }
  return FR_OK;
}
#endif

static fr_err_t fr_append_u32(char *out, uint16_t out_cap, uint16_t *used,
                              uint32_t value) {
  char digits[10];
  uint8_t count = 0;

  do {
    digits[count] = (char)('0' + (value % 10u));
    count += 1;
    value /= 10u;
  } while (value > 0);

  while (count > 0) {
    count -= 1;
    FR_TRY(fr_append_char(out, out_cap, used, digits[count]));
  }
  return FR_OK;
}

static fr_err_t fr_append_i32(char *out, uint16_t out_cap, uint16_t *used,
                              int32_t value) {
  uint32_t magnitude = 0;

  if (value < 0) {
    FR_TRY(fr_append_char(out, out_cap, used, '-'));
    magnitude = (uint32_t)(-(value + 1)) + 1u;
  } else {
    magnitude = (uint32_t)value;
  }
  return fr_append_u32(out, out_cap, used, magnitude);
}

static fr_err_t fr_begin_text(char *out, uint16_t out_cap,
                              uint16_t *out_used) {
  if (out == NULL || out_cap == 0 || out_used == NULL) {
    return FR_ERR_INVALID;
  }

  out[0] = '\0';
  *out_used = 0;
  return FR_OK;
}

static fr_err_t fr_finish_instruction_text(uint16_t used, uint16_t *out_len,
                                           fr_code_offset_t next,
                                           fr_code_offset_t *next_ip) {
  if (next_ip == NULL) {
    return FR_ERR_INVALID;
  }
  if (out_len != NULL) {
    *out_len = used;
  }
  *next_ip = next;
  return FR_OK;
}

fr_err_t fr_instruction_stream_init(fr_instruction_stream_t *view,
                                    const uint8_t *bytes, uint16_t length) {
  if (view == NULL) {
    return FR_ERR_INVALID;
  }
  if (length > FR_PROFILE_MAX_INSTRUCTION_BYTES) {
    return FR_ERR_RANGE;
  }
  if (bytes == NULL && length > 0) {
    return FR_ERR_INVALID;
  }

  view->bytes = bytes;
  view->length = length;
  return FR_OK;
}

fr_err_t fr_instruction_read_header(const fr_instruction_stream_t *view,
                                    fr_instruction_header_t *header) {
  if (view == NULL || header == NULL) {
    return FR_ERR_INVALID;
  }
  if (view->bytes == NULL && view->length > 0) {
    return FR_ERR_INVALID;
  }
  if (view->length < FR_INSTRUCTION_MIN_HEADER_SIZE) {
    return FR_ERR_INVALID;
  }

  header->format_version = view->bytes[0];
  header->header_size = view->bytes[1];
  header->arity = 0;
  header->local_count = 0;

  if (header->format_version != FR_INSTRUCTION_FORMAT_VERSION) {
    return FR_ERR_UNSUPPORTED;
  }
  if (header->header_size < FR_INSTRUCTION_MIN_HEADER_SIZE) {
    return FR_ERR_INVALID;
  }
  if (header->header_size > view->length) {
    return FR_ERR_INVALID;
  }
  if (header->header_size > FR_INSTRUCTION_MAX_HEADER_SIZE) {
    return FR_ERR_INVALID;
  }
  if (header->header_size >= FR_INSTRUCTION_ARITY_HEADER_SIZE) {
    header->arity = view->bytes[2];
    if (header->arity > FR_PROFILE_MAX_STACK_DEPTH) {
      return FR_ERR_RANGE;
    }
  }
  if (header->header_size >= FR_INSTRUCTION_LOCALS_HEADER_SIZE) {
    header->local_count = view->bytes[3];
    if ((uint16_t)header->arity + header->local_count >
        FR_PROFILE_MAX_STACK_DEPTH) {
      return FR_ERR_RANGE;
    }
  }

  return FR_OK;
}

/* Operand readers below trust their out-pointers: every call site passes the
   address of a caller-owned stack local (verified across vm/image/persist/source_render).
   The out==NULL guard was a dead internal branch and is intentionally omitted. */
fr_err_t fr_instruction_decode_slot(const fr_instruction_stream_t *view,
                                    fr_code_offset_t ip,
                                    fr_slot_id_t *out_slot_id) {
  *out_slot_id = fr_read_u16_little_endian(&view->bytes[ip + 1]);
  return FR_OK;
}

fr_err_t fr_instruction_decode_int(const fr_instruction_stream_t *view,
                                   fr_code_offset_t ip, fr_int_t *out_int) {
  *out_int = fr_read_int_operand_little_endian(&view->bytes[ip + 1]);
  return FR_OK;
}

fr_err_t fr_instruction_decode_object_id(
    const fr_instruction_stream_t *view, fr_code_offset_t ip,
    fr_object_id_t *out_object_id) {
  *out_object_id =
      (fr_object_id_t)fr_read_u16_little_endian(&view->bytes[ip + 1]);
  return FR_OK;
}

fr_err_t fr_instruction_decode_code_id(
    const fr_instruction_stream_t *view, fr_code_offset_t ip,
    fr_code_object_id_t *out_code_object_id) {
  *out_code_object_id =
      (fr_code_object_id_t)fr_read_u16_little_endian(&view->bytes[ip + 1]);
  return FR_OK;
}

fr_err_t fr_instruction_decode_jump(const fr_instruction_stream_t *view,
                                    fr_code_offset_t ip,
                                    fr_code_offset_t *out_target) {
  *out_target = fr_read_u16_little_endian(&view->bytes[ip + 1]);
  return FR_OK;
}

fr_err_t fr_instruction_decode_arg(const fr_instruction_stream_t *view,
                                   fr_code_offset_t ip,
                                   uint8_t *out_arg_index) {
  *out_arg_index = view->bytes[ip + 1];
  return FR_OK;
}

fr_err_t fr_instruction_decode_local(const fr_instruction_stream_t *view,
                                     fr_code_offset_t ip,
                                     uint8_t *out_local_index) {
  *out_local_index = view->bytes[ip + 1];
  return FR_OK;
}

fr_err_t fr_instruction_decode_call_slot_arg(
    const fr_instruction_stream_t *view, fr_code_offset_t ip,
    fr_slot_id_t *out_slot_id, uint8_t *out_arg_count) {
  *out_slot_id = fr_read_u16_little_endian(&view->bytes[ip + 1]);
  *out_arg_count = view->bytes[ip + 3];
  return FR_OK;
}

#if FR_FEATURE_CELLS
fr_err_t fr_instruction_decode_cell(const fr_instruction_stream_t *view,
                                    fr_code_offset_t ip,
                                    fr_slot_id_t *out_slot_id,
                                    uint16_t *out_index) {
  *out_slot_id = fr_read_u16_little_endian(&view->bytes[ip + 1]);
  *out_index = fr_read_u16_little_endian(&view->bytes[ip + 3]);
  return FR_OK;
}
#endif

fr_err_t fr_instruction_read_slot_operand(const fr_instruction_stream_t *view,
                                          fr_code_offset_t ip,
                                          fr_slot_id_t *out_slot_id) {
  FR_TRY(fr_require_bytes(view, ip, 3));

  FR_TRY(fr_instruction_decode_slot(view, ip, out_slot_id));
  if (*out_slot_id >= FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_RANGE;
  }
  return FR_OK;
}

fr_err_t fr_instruction_read_int_operand(const fr_instruction_stream_t *view,
                                         fr_code_offset_t ip,
                                         fr_int_t *out_int) {
  FR_TRY(fr_require_bytes(view, ip, FR_INSTRUCTION_PUSH_INT_SIZE));

  FR_TRY(fr_instruction_decode_int(view, ip, out_int));
  if (!fr_tagged_can_encode_int(*out_int)) {
    return FR_ERR_RANGE;
  }
  return FR_OK;
}

fr_err_t fr_instruction_read_object_id_operand(
    const fr_instruction_stream_t *view, fr_code_offset_t ip,
    fr_object_id_t *out_object_id) {
  FR_TRY(fr_require_bytes(view, ip, FR_INSTRUCTION_PUSH_OBJECT_ID_SIZE));

  return fr_instruction_decode_object_id(view, ip, out_object_id);
}

fr_err_t fr_instruction_read_code_id_operand(
    const fr_instruction_stream_t *view, fr_code_offset_t ip,
    fr_code_object_id_t *out_code_object_id) {
  FR_TRY(fr_require_bytes(view, ip, FR_INSTRUCTION_PUSH_CODE_ID_SIZE));

  return fr_instruction_decode_code_id(view, ip, out_code_object_id);
}

fr_err_t fr_instruction_read_jump_operand(const fr_instruction_stream_t *view,
                                          fr_code_offset_t ip,
                                          fr_code_offset_t *out_target) {
  FR_TRY(fr_require_bytes(view, ip, 3));

  FR_TRY(fr_instruction_decode_jump(view, ip, out_target));
  if (*out_target >= view->length) {
    return FR_ERR_INVALID;
  }
  return FR_OK;
}

fr_err_t fr_instruction_read_arg_operand(const fr_instruction_stream_t *view,
                                         fr_code_offset_t ip,
                                         uint8_t *out_arg_index) {
  fr_instruction_header_t header;

  FR_TRY(fr_require_bytes(view, ip, 2));
  FR_TRY(fr_instruction_read_header(view, &header));

  FR_TRY(fr_instruction_decode_arg(view, ip, out_arg_index));
  if (*out_arg_index >= header.arity) {
    return FR_ERR_RANGE;
  }
  return FR_OK;
}

fr_err_t fr_instruction_read_local_operand(const fr_instruction_stream_t *view,
                                           fr_code_offset_t ip,
                                           uint8_t *out_local_index) {
  fr_instruction_header_t header;

  FR_TRY(fr_require_bytes(view, ip, 2));
  FR_TRY(fr_instruction_read_header(view, &header));

  FR_TRY(fr_instruction_decode_local(view, ip, out_local_index));
  if (*out_local_index >= header.local_count) {
    return FR_ERR_RANGE;
  }
  return FR_OK;
}

fr_err_t fr_instruction_read_call_slot_arg_operands(
    const fr_instruction_stream_t *view, fr_code_offset_t ip,
    fr_slot_id_t *out_slot_id, uint8_t *out_arg_count) {
  FR_TRY(fr_require_bytes(view, ip, 4));

  FR_TRY(
      fr_instruction_decode_call_slot_arg(view, ip, out_slot_id, out_arg_count));
  if (*out_slot_id >= FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_RANGE;
  }
  if (*out_arg_count > FR_PROFILE_MAX_STACK_DEPTH) {
    return FR_ERR_RANGE;
  }
  return FR_OK;
}

#if FR_FEATURE_CELLS
fr_err_t fr_instruction_read_cell_operands(const fr_instruction_stream_t *view,
                                           fr_code_offset_t ip,
                                           fr_slot_id_t *out_slot_id,
                                           uint16_t *out_index) {
  FR_TRY(fr_require_bytes(view, ip, 5));

  FR_TRY(fr_instruction_decode_cell(view, ip, out_slot_id, out_index));
  if (*out_slot_id >= FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_RANGE;
  }
  return FR_OK;
}
#endif

#if FR_FEATURE_RECORDS
fr_err_t fr_instruction_read_field_operand(const fr_instruction_stream_t *view,
                                           fr_code_offset_t ip,
                                           const uint8_t **out_name,
                                           uint8_t *out_length) {
  uint8_t length = 0;

  FR_TRY(fr_require_bytes(view, ip, 2));
  length = view->bytes[ip + 1];
  if (length == 0 || length > FR_PROFILE_MAX_NAME_BYTES) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_require_bytes(view, ip, (uint16_t)(2u + length)));
  *out_name = &view->bytes[ip + 2];
  *out_length = length;
  return FR_OK;
}
#endif

static fr_err_t fr_instruction_length_at(const fr_instruction_stream_t *view,
                                         fr_code_offset_t ip,
                                         fr_code_offset_t *next_ip) {
  if (view == NULL || view->bytes == NULL || next_ip == NULL ||
      ip >= view->length) {
    return FR_ERR_INVALID;
  }

  switch ((fr_opcode_t)view->bytes[ip]) {
  case FR_OP_RETURN:
  case FR_OP_ADD_INT:
  case FR_OP_SUB_INT:
  case FR_OP_MUL_INT:
  case FR_OP_DIV_INT:
  case FR_OP_LT_INT:
  case FR_OP_GT_INT:
  case FR_OP_LE_INT:
  case FR_OP_GE_INT:
  case FR_OP_EQ_INT:
  case FR_OP_NE_INT:
  case FR_OP_DROP:
  case FR_OP_PUSH_NIL:
  case FR_OP_PUSH_FALSE:
  case FR_OP_PUSH_TRUE:
  case FR_OP_BYTES_RESET:
    FR_TRY(fr_require_bytes(view, ip, 1));
    *next_ip = (fr_code_offset_t)(ip + 1u);
    return FR_OK;

  case FR_OP_LOAD_ARG: {
    uint8_t arg_index = 0;
    FR_TRY(fr_instruction_read_arg_operand(view, ip, &arg_index));
    *next_ip = (fr_code_offset_t)(ip + 2u);
    return FR_OK;
  }
  case FR_OP_LOAD_LOCAL:
  case FR_OP_STORE_LOCAL: {
    uint8_t local_index = 0;
    FR_TRY(fr_instruction_read_local_operand(view, ip, &local_index));
    *next_ip = (fr_code_offset_t)(ip + 2u);
    return FR_OK;
  }

#if FR_FEATURE_RECORDS
  case FR_OP_LOAD_FIELD:
  case FR_OP_STORE_FIELD: {
    const uint8_t *name = NULL;
    uint8_t length = 0;
    FR_TRY(fr_instruction_read_field_operand(view, ip, &name, &length));
    *next_ip = (fr_code_offset_t)(ip + 2u + length);
    return FR_OK;
  }
#else
  case FR_OP_LOAD_FIELD:
  case FR_OP_STORE_FIELD:
    return FR_ERR_UNSUPPORTED;
#endif

  case FR_OP_LOAD_SLOT:
  case FR_OP_STORE_SLOT:
  case FR_OP_CALL_SLOT:
  case FR_OP_CALL_NATIVE_SLOT: {
    fr_slot_id_t slot_id = 0;
    FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
    *next_ip = (fr_code_offset_t)(ip + 3u);
    return FR_OK;
  }

  case FR_OP_PUSH_INT: {
    fr_int_t int_operand = 0;
    FR_TRY(fr_instruction_read_int_operand(view, ip, &int_operand));
    *next_ip = (fr_code_offset_t)(ip + FR_INSTRUCTION_PUSH_INT_SIZE);
    return FR_OK;
  }
  case FR_OP_PUSH_OBJECT_ID: {
    fr_object_id_t object_id = 0;
    FR_TRY(fr_instruction_read_object_id_operand(view, ip, &object_id));
    if (object_id >= FR_PROFILE_OBJECT_TABLE_SIZE) {
      return FR_ERR_RANGE;
    }
    *next_ip = (fr_code_offset_t)(ip + FR_INSTRUCTION_PUSH_OBJECT_ID_SIZE);
    return FR_OK;
  }
  case FR_OP_PUSH_CODE_ID: {
    fr_code_object_id_t code_id = 0;
    FR_TRY(fr_instruction_read_code_id_operand(view, ip, &code_id));
    if (code_id >= FR_PROFILE_CODE_OBJECT_TABLE_SIZE) {
      return FR_ERR_RANGE;
    }
    *next_ip = (fr_code_offset_t)(ip + FR_INSTRUCTION_PUSH_CODE_ID_SIZE);
    return FR_OK;
  }

  case FR_OP_CALL_SLOT_ARG: {
    fr_slot_id_t slot_id = 0;
    uint8_t arg_count = 0;
    FR_TRY(fr_instruction_read_call_slot_arg_operands(view, ip, &slot_id,
                                                      &arg_count));
    *next_ip = (fr_code_offset_t)(ip + 4u);
    return FR_OK;
  }

#if FR_FEATURE_CELLS
  case FR_OP_LOAD_CELL:
  case FR_OP_STORE_CELL: {
    fr_slot_id_t slot_id = 0;
    uint16_t index = 0;
    FR_TRY(fr_instruction_read_cell_operands(view, ip, &slot_id, &index));
    *next_ip = (fr_code_offset_t)(ip + 5u);
    return FR_OK;
  }
  case FR_OP_LOAD_CELL_DYNAMIC:
  case FR_OP_STORE_CELL_DYNAMIC: {
    fr_slot_id_t slot_id = 0;
    FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
    *next_ip = (fr_code_offset_t)(ip + 3u);
    return FR_OK;
  }
#else
  case FR_OP_LOAD_CELL:
  case FR_OP_STORE_CELL:
  case FR_OP_LOAD_CELL_DYNAMIC:
  case FR_OP_STORE_CELL_DYNAMIC:
    return FR_ERR_UNSUPPORTED;
#endif

  case FR_OP_JUMP:
  case FR_OP_JUMP_IF_FALSY:
  case FR_OP_REPEAT_BEGIN:
  case FR_OP_REPEAT_NEXT: {
    fr_code_offset_t target = 0;
    FR_TRY(fr_instruction_read_jump_operand(view, ip, &target));
    *next_ip = (fr_code_offset_t)(ip + 3u);
    return FR_OK;
  }

  default:
    return FR_ERR_INVALID;
  }
}

static void fr_instruction_start_set(uint8_t starts[], fr_code_offset_t offset) {
  starts[offset / 8u] |= (uint8_t)(1u << (offset % 8u));
}

static bool fr_instruction_start_has(const uint8_t starts[],
                                     fr_code_offset_t offset) {
  return (starts[offset / 8u] & (uint8_t)(1u << (offset % 8u))) != 0;
}

static bool fr_instruction_opcode_has_jump_target(fr_opcode_t op) {
  return op == FR_OP_JUMP || op == FR_OP_JUMP_IF_FALSY ||
         op == FR_OP_REPEAT_BEGIN || op == FR_OP_REPEAT_NEXT;
}

fr_err_t fr_verify_code_object(const fr_instruction_stream_t *view) {
  fr_instruction_header_t header;
  uint8_t instruction_starts[(FR_PROFILE_MAX_INSTRUCTION_BYTES / 8u) + 1u];
  fr_code_offset_t ip = 0;

  if (view == NULL) {
    return FR_ERR_INVALID;
  }
  if (view->length > FR_PROFILE_MAX_INSTRUCTION_BYTES) {
    return FR_ERR_RANGE;
  }

  FR_TRY(fr_instruction_read_header(view, &header));
  memset(instruction_starts, 0, sizeof(instruction_starts));

  ip = header.header_size;
  while (ip < view->length) {
    fr_code_offset_t next_ip = 0;

    FR_TRY(fr_instruction_length_at(view, ip, &next_ip));
    if (next_ip <= ip || next_ip > view->length) {
      return FR_ERR_INVALID;
    }
    fr_instruction_start_set(instruction_starts, ip);
    ip = next_ip;
  }
  if (ip != view->length) {
    return FR_ERR_INVALID;
  }

  ip = header.header_size;
  while (ip < view->length) {
    fr_opcode_t op = (fr_opcode_t)view->bytes[ip];
    fr_code_offset_t next_ip = 0;

    FR_TRY(fr_instruction_length_at(view, ip, &next_ip));
    if (fr_instruction_opcode_has_jump_target(op)) {
      fr_code_offset_t target = 0;

      FR_TRY(fr_instruction_read_jump_operand(view, ip, &target));
      if (target < header.header_size || target >= view->length ||
          !fr_instruction_start_has(instruction_starts, target)) {
        return FR_ERR_INVALID;
      }
    }
    ip = next_ip;
  }

  return FR_OK;
}

fr_err_t fr_instruction_disassemble_at(const fr_instruction_stream_t *view,
                                       fr_code_offset_t ip, char *out,
                                       uint16_t out_cap, uint16_t *out_len,
                                       fr_code_offset_t *next_ip) {
  fr_code_offset_t following_ip = 0;
  uint16_t used = 0;

  if (view == NULL || view->bytes == NULL || out == NULL || next_ip == NULL ||
      ip >= view->length) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_instruction_length_at(view, ip, &following_ip));
  FR_TRY(fr_begin_text(out, out_cap, &used));

  switch ((fr_opcode_t)view->bytes[ip]) {
  case FR_OP_RETURN:
    FR_TRY(fr_append_text(out, out_cap, &used, "RETURN"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  case FR_OP_ADD_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "ADD_INT"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  case FR_OP_SUB_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "SUB_INT"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  case FR_OP_MUL_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "MUL_INT"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  case FR_OP_DIV_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "DIV_INT"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  case FR_OP_LT_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "LT_INT"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  case FR_OP_GT_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "GT_INT"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  case FR_OP_LE_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "LE_INT"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  case FR_OP_GE_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "GE_INT"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  case FR_OP_EQ_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "EQ_INT"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  case FR_OP_NE_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "NE_INT"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  case FR_OP_DROP:
    FR_TRY(fr_append_text(out, out_cap, &used, "DROP"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  case FR_OP_PUSH_NIL:
    FR_TRY(fr_append_text(out, out_cap, &used, "PUSH_NIL"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  case FR_OP_PUSH_FALSE:
    FR_TRY(fr_append_text(out, out_cap, &used, "PUSH_FALSE"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  case FR_OP_PUSH_TRUE:
    FR_TRY(fr_append_text(out, out_cap, &used, "PUSH_TRUE"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  case FR_OP_LOAD_ARG: {
    uint8_t arg_index = 0;
    FR_TRY(fr_instruction_read_arg_operand(view, ip, &arg_index));
    FR_TRY(fr_append_text(out, out_cap, &used, "LOAD_ARG "));
    FR_TRY(fr_append_u32(out, out_cap, &used, arg_index));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
  case FR_OP_LOAD_LOCAL: {
    uint8_t local_index = 0;
    FR_TRY(fr_instruction_read_local_operand(view, ip, &local_index));
    FR_TRY(fr_append_text(out, out_cap, &used, "LOAD_LOCAL "));
    FR_TRY(fr_append_u32(out, out_cap, &used, local_index));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
  case FR_OP_STORE_LOCAL: {
    uint8_t local_index = 0;
    FR_TRY(fr_instruction_read_local_operand(view, ip, &local_index));
    FR_TRY(fr_append_text(out, out_cap, &used, "STORE_LOCAL "));
    FR_TRY(fr_append_u32(out, out_cap, &used, local_index));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
#if FR_FEATURE_RECORDS
  case FR_OP_LOAD_FIELD:
  case FR_OP_STORE_FIELD: {
    const uint8_t *name = NULL;
    uint8_t length = 0;
    const char *op_name =
        view->bytes[ip] == FR_OP_LOAD_FIELD ? "LOAD_FIELD " : "STORE_FIELD ";

    FR_TRY(fr_instruction_read_field_operand(view, ip, &name, &length));
    FR_TRY(fr_append_text(out, out_cap, &used, op_name));
    FR_TRY(fr_append_bytes_as_text(out, out_cap, &used, name, length));
    return fr_finish_instruction_text(used, out_len, following_ip, next_ip);
  }
#else
  case FR_OP_LOAD_FIELD:
  case FR_OP_STORE_FIELD:
    return FR_ERR_UNSUPPORTED;
#endif
  case FR_OP_LOAD_SLOT: {
    fr_slot_id_t slot_id = 0;
    FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
    FR_TRY(fr_append_text(out, out_cap, &used, "LOAD_SLOT "));
    FR_TRY(fr_append_u32(out, out_cap, &used, slot_id));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
  case FR_OP_STORE_SLOT: {
    fr_slot_id_t slot_id = 0;
    FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
    FR_TRY(fr_append_text(out, out_cap, &used, "STORE_SLOT "));
    FR_TRY(fr_append_u32(out, out_cap, &used, slot_id));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
  case FR_OP_PUSH_INT: {
    fr_int_t int_operand = 0;
    FR_TRY(fr_instruction_read_int_operand(view, ip, &int_operand));
    FR_TRY(fr_append_text(out, out_cap, &used, "PUSH_INT "));
    FR_TRY(fr_append_i32(out, out_cap, &used, (int32_t)int_operand));
    return fr_finish_instruction_text(used, out_len, following_ip, next_ip);
  }
  case FR_OP_PUSH_OBJECT_ID: {
    fr_object_id_t object_id = 0;
    FR_TRY(fr_instruction_read_object_id_operand(view, ip, &object_id));
    FR_TRY(fr_append_text(out, out_cap, &used, "PUSH_OBJECT_ID "));
    FR_TRY(fr_append_u32(out, out_cap, &used, object_id));
    return fr_finish_instruction_text(used, out_len, following_ip, next_ip);
  }
  case FR_OP_PUSH_CODE_ID: {
    fr_code_object_id_t code_id = 0;
    FR_TRY(fr_instruction_read_code_id_operand(view, ip, &code_id));
    FR_TRY(fr_append_text(out, out_cap, &used, "PUSH_CODE_ID "));
    FR_TRY(fr_append_u32(out, out_cap, &used, code_id));
    return fr_finish_instruction_text(used, out_len, following_ip, next_ip);
  }
  case FR_OP_CALL_SLOT: {
    fr_slot_id_t slot_id = 0;
    FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
    FR_TRY(fr_append_text(out, out_cap, &used, "CALL_SLOT "));
    FR_TRY(fr_append_u32(out, out_cap, &used, slot_id));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
  case FR_OP_CALL_SLOT_ARG: {
    fr_slot_id_t slot_id = 0;
    uint8_t arg_count = 0;
    FR_TRY(fr_instruction_read_call_slot_arg_operands(view, ip, &slot_id,
                                                      &arg_count));
    FR_TRY(fr_append_text(out, out_cap, &used, "CALL_SLOT_ARG "));
    FR_TRY(fr_append_u32(out, out_cap, &used, slot_id));
    FR_TRY(fr_append_char(out, out_cap, &used, ' '));
    FR_TRY(fr_append_u32(out, out_cap, &used, arg_count));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
#if FR_FEATURE_CELLS
  case FR_OP_LOAD_CELL: {
    fr_slot_id_t slot_id = 0;
    uint16_t index = 0;
    FR_TRY(fr_instruction_read_cell_operands(view, ip, &slot_id, &index));
    FR_TRY(fr_append_text(out, out_cap, &used, "LOAD_CELL "));
    FR_TRY(fr_append_u32(out, out_cap, &used, slot_id));
    FR_TRY(fr_append_char(out, out_cap, &used, ' '));
    FR_TRY(fr_append_u32(out, out_cap, &used, index));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
  case FR_OP_STORE_CELL: {
    fr_slot_id_t slot_id = 0;
    uint16_t index = 0;
    FR_TRY(fr_instruction_read_cell_operands(view, ip, &slot_id, &index));
    FR_TRY(fr_append_text(out, out_cap, &used, "STORE_CELL "));
    FR_TRY(fr_append_u32(out, out_cap, &used, slot_id));
    FR_TRY(fr_append_char(out, out_cap, &used, ' '));
    FR_TRY(fr_append_u32(out, out_cap, &used, index));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
  case FR_OP_LOAD_CELL_DYNAMIC: {
    fr_slot_id_t slot_id = 0;
    FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
    FR_TRY(fr_append_text(out, out_cap, &used, "LOAD_CELL_DYNAMIC "));
    FR_TRY(fr_append_u32(out, out_cap, &used, slot_id));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
  case FR_OP_STORE_CELL_DYNAMIC: {
    fr_slot_id_t slot_id = 0;
    FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
    FR_TRY(fr_append_text(out, out_cap, &used, "STORE_CELL_DYNAMIC "));
    FR_TRY(fr_append_u32(out, out_cap, &used, slot_id));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
#else
  case FR_OP_LOAD_CELL:
  case FR_OP_STORE_CELL:
  case FR_OP_LOAD_CELL_DYNAMIC:
  case FR_OP_STORE_CELL_DYNAMIC:
    return FR_ERR_UNSUPPORTED;
#endif
  case FR_OP_CALL_NATIVE_SLOT: {
    fr_slot_id_t slot_id = 0;
    FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
    FR_TRY(fr_append_text(out, out_cap, &used, "CALL_NATIVE_SLOT "));
    FR_TRY(fr_append_u32(out, out_cap, &used, slot_id));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
  case FR_OP_JUMP: {
    fr_code_offset_t target = 0;
    FR_TRY(fr_instruction_read_jump_operand(view, ip, &target));
    FR_TRY(fr_append_text(out, out_cap, &used, "JUMP "));
    FR_TRY(fr_append_u32(out, out_cap, &used, target));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
  case FR_OP_JUMP_IF_FALSY: {
    fr_code_offset_t target = 0;
    FR_TRY(fr_instruction_read_jump_operand(view, ip, &target));
    FR_TRY(fr_append_text(out, out_cap, &used, "JUMP_IF_FALSY "));
    FR_TRY(fr_append_u32(out, out_cap, &used, target));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
  case FR_OP_REPEAT_BEGIN: {
    fr_code_offset_t target = 0;
    FR_TRY(fr_instruction_read_jump_operand(view, ip, &target));
    FR_TRY(fr_append_text(out, out_cap, &used, "REPEAT_BEGIN "));
    FR_TRY(fr_append_u32(out, out_cap, &used, target));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
  case FR_OP_REPEAT_NEXT: {
    fr_code_offset_t target = 0;
    FR_TRY(fr_instruction_read_jump_operand(view, ip, &target));
    FR_TRY(fr_append_text(out, out_cap, &used, "REPEAT_NEXT "));
    FR_TRY(fr_append_u32(out, out_cap, &used, target));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  }
  case FR_OP_BYTES_RESET:
    FR_TRY(fr_append_text(out, out_cap, &used, "BYTES_RESET"));
    return fr_finish_instruction_text(used, out_len, following_ip,
                                      next_ip);
  default:
    return FR_ERR_INVALID;
  }
}

fr_err_t fr_instruction_stream_disassemble(const fr_instruction_stream_t *view,
                                           char *out, uint16_t out_cap,
                                           uint16_t *out_len) {
  fr_instruction_header_t header;
  char line[32];
  uint16_t used = 0;
  fr_code_offset_t ip = 0;

  if (view == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_begin_text(out, out_cap, &used));

  FR_TRY(fr_instruction_read_header(view, &header));
  if (view->length == header.header_size) {
    return FR_ERR_INVALID;
  }

  ip = header.header_size;
  while (ip < view->length) {
    uint16_t line_len = 0;
    fr_code_offset_t next_ip = 0;

    FR_TRY(fr_instruction_disassemble_at(view, ip, line, sizeof(line),
                                         &line_len, &next_ip));
    (void)line_len;
    FR_TRY(fr_append_text(out, out_cap, &used, line));
    FR_TRY(fr_append_char(out, out_cap, &used, '\n'));
    ip = next_ip;
  }

  if (out_len != NULL) {
    *out_len = used;
  }
  return FR_OK;
}
