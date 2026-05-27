/*
 * An instruction stream is a borrowed byte view. Operands are raw fields; the
 * VM encodes an operand only when it must become a tagged word. This module
 * reads and renders bytes; profile trust policy owns semantic validation.
 */

#include "instruction.h"

#include "tagged.h"

#include <stddef.h>
#include <stdint.h>

static uint16_t fr_read_u16_little_endian(const uint8_t *bytes) {
  return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

#if FR_WORD_SIZE == 16
static fr_int_t fr_read_i16_little_endian(const uint8_t *bytes) {
  return (fr_int_t)(int16_t)fr_read_u16_little_endian(bytes);
}
#endif

static fr_int_t fr_read_int_operand_little_endian(const uint8_t *bytes) {
#if FR_WORD_SIZE == 16
  return fr_read_i16_little_endian(bytes);
#else
  return (fr_int_t)(int32_t)fr_read_u32_le(bytes);
#endif
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

  return FR_OK;
}

fr_err_t fr_instruction_read_slot_operand(const fr_instruction_stream_t *view,
                                          fr_code_offset_t ip,
                                          fr_slot_id_t *out_slot_id) {
  if (out_slot_id == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_require_bytes(view, ip, 3));

  *out_slot_id = fr_read_u16_little_endian(&view->bytes[ip + 1]);
  if (*out_slot_id >= FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_RANGE;
  }
  return FR_OK;
}

fr_err_t fr_instruction_read_int_operand(const fr_instruction_stream_t *view,
                                         fr_code_offset_t ip,
                                         fr_int_t *out_int) {
  if (out_int == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_require_bytes(view, ip, FR_INSTRUCTION_PUSH_INT_SIZE));

  *out_int = fr_read_int_operand_little_endian(&view->bytes[ip + 1]);
  if (!fr_tagged_can_encode_int(*out_int)) {
    return FR_ERR_RANGE;
  }
  return FR_OK;
}

fr_err_t fr_instruction_read_jump_operand(const fr_instruction_stream_t *view,
                                          fr_code_offset_t ip,
                                          fr_code_offset_t *out_target) {
  if (out_target == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_require_bytes(view, ip, 3));

  *out_target = fr_read_u16_little_endian(&view->bytes[ip + 1]);
  if (*out_target >= view->length) {
    return FR_ERR_INVALID;
  }
  return FR_OK;
}

fr_err_t fr_instruction_read_arg_operand(const fr_instruction_stream_t *view,
                                         fr_code_offset_t ip,
                                         uint8_t *out_arg_index) {
  fr_instruction_header_t header;

  if (out_arg_index == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_require_bytes(view, ip, 2));
  FR_TRY(fr_instruction_read_header(view, &header));

  *out_arg_index = view->bytes[ip + 1];
  if (*out_arg_index >= header.arity) {
    return FR_ERR_RANGE;
  }
  return FR_OK;
}

fr_err_t fr_instruction_read_call_slot_arg_operands(
    const fr_instruction_stream_t *view, fr_code_offset_t ip,
    fr_slot_id_t *out_slot_id, uint8_t *out_arg_count) {
  if (out_slot_id == NULL || out_arg_count == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_require_bytes(view, ip, 4));

  *out_slot_id = fr_read_u16_little_endian(&view->bytes[ip + 1]);
  *out_arg_count = view->bytes[ip + 3];
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
  if (out_slot_id == NULL || out_index == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_require_bytes(view, ip, 5));

  *out_slot_id = fr_read_u16_little_endian(&view->bytes[ip + 1]);
  *out_index = fr_read_u16_little_endian(&view->bytes[ip + 3]);
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

  if (out_name == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }
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

fr_err_t fr_instruction_disassemble_at(const fr_instruction_stream_t *view,
                                       fr_code_offset_t ip, char *out,
                                       uint16_t out_cap, uint16_t *out_len,
                                       fr_code_offset_t *next_ip) {
  uint16_t used = 0;

  if (view == NULL || view->bytes == NULL || out == NULL || next_ip == NULL ||
      ip >= view->length) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_begin_text(out, out_cap, &used));

  switch ((fr_opcode_t)view->bytes[ip]) {
  case FR_OP_RETURN:
    FR_TRY(fr_append_text(out, out_cap, &used, "RETURN"));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 1),
                                      next_ip);
  case FR_OP_ADD_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "ADD_INT"));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 1),
                                      next_ip);
  case FR_OP_SUB_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "SUB_INT"));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 1),
                                      next_ip);
  case FR_OP_MUL_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "MUL_INT"));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 1),
                                      next_ip);
  case FR_OP_DIV_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "DIV_INT"));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 1),
                                      next_ip);
  case FR_OP_LT_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "LT_INT"));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 1),
                                      next_ip);
  case FR_OP_GT_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "GT_INT"));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 1),
                                      next_ip);
  case FR_OP_LE_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "LE_INT"));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 1),
                                      next_ip);
  case FR_OP_GE_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "GE_INT"));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 1),
                                      next_ip);
  case FR_OP_EQ_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "EQ_INT"));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 1),
                                      next_ip);
  case FR_OP_NE_INT:
    FR_TRY(fr_append_text(out, out_cap, &used, "NE_INT"));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 1),
                                      next_ip);
  case FR_OP_DROP:
    FR_TRY(fr_append_text(out, out_cap, &used, "DROP"));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 1),
                                      next_ip);
  case FR_OP_PUSH_NIL:
    FR_TRY(fr_append_text(out, out_cap, &used, "PUSH_NIL"));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 1),
                                      next_ip);
  case FR_OP_PUSH_FALSE:
    FR_TRY(fr_append_text(out, out_cap, &used, "PUSH_FALSE"));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 1),
                                      next_ip);
  case FR_OP_PUSH_TRUE:
    FR_TRY(fr_append_text(out, out_cap, &used, "PUSH_TRUE"));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 1),
                                      next_ip);
  case FR_OP_LOAD_ARG: {
    uint8_t arg_index = 0;
    FR_TRY(fr_instruction_read_arg_operand(view, ip, &arg_index));
    FR_TRY(fr_append_text(out, out_cap, &used, "LOAD_ARG "));
    FR_TRY(fr_append_u32(out, out_cap, &used, arg_index));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 2),
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
    return fr_finish_instruction_text(
        used, out_len, (fr_code_offset_t)(ip + 2u + length), next_ip);
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
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 3),
                                      next_ip);
  }
  case FR_OP_STORE_SLOT: {
    fr_slot_id_t slot_id = 0;
    FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
    FR_TRY(fr_append_text(out, out_cap, &used, "STORE_SLOT "));
    FR_TRY(fr_append_u32(out, out_cap, &used, slot_id));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 3),
                                      next_ip);
  }
  case FR_OP_PUSH_INT: {
    fr_int_t int_operand = 0;
    FR_TRY(fr_instruction_read_int_operand(view, ip, &int_operand));
    FR_TRY(fr_append_text(out, out_cap, &used, "PUSH_INT "));
    FR_TRY(fr_append_i32(out, out_cap, &used, (int32_t)int_operand));
    return fr_finish_instruction_text(
        used, out_len, (fr_code_offset_t)(ip + FR_INSTRUCTION_PUSH_INT_SIZE),
        next_ip);
  }
  case FR_OP_CALL_SLOT: {
    fr_slot_id_t slot_id = 0;
    FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
    FR_TRY(fr_append_text(out, out_cap, &used, "CALL_SLOT "));
    FR_TRY(fr_append_u32(out, out_cap, &used, slot_id));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 3),
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
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 4),
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
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 5),
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
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 5),
                                      next_ip);
  }
#endif
  case FR_OP_CALL_NATIVE_SLOT: {
    fr_slot_id_t slot_id = 0;
    FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
    FR_TRY(fr_append_text(out, out_cap, &used, "CALL_NATIVE_SLOT "));
    FR_TRY(fr_append_u32(out, out_cap, &used, slot_id));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 3),
                                      next_ip);
  }
  case FR_OP_JUMP: {
    fr_code_offset_t target = 0;
    FR_TRY(fr_instruction_read_jump_operand(view, ip, &target));
    FR_TRY(fr_append_text(out, out_cap, &used, "JUMP "));
    FR_TRY(fr_append_u32(out, out_cap, &used, target));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 3),
                                      next_ip);
  }
  case FR_OP_JUMP_IF_FALSY: {
    fr_code_offset_t target = 0;
    FR_TRY(fr_instruction_read_jump_operand(view, ip, &target));
    FR_TRY(fr_append_text(out, out_cap, &used, "JUMP_IF_FALSY "));
    FR_TRY(fr_append_u32(out, out_cap, &used, target));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 3),
                                      next_ip);
  }
  case FR_OP_REPEAT_BEGIN: {
    fr_code_offset_t target = 0;
    FR_TRY(fr_instruction_read_jump_operand(view, ip, &target));
    FR_TRY(fr_append_text(out, out_cap, &used, "REPEAT_BEGIN "));
    FR_TRY(fr_append_u32(out, out_cap, &used, target));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 3),
                                      next_ip);
  }
  case FR_OP_REPEAT_NEXT: {
    fr_code_offset_t target = 0;
    FR_TRY(fr_instruction_read_jump_operand(view, ip, &target));
    FR_TRY(fr_append_text(out, out_cap, &used, "REPEAT_NEXT "));
    FR_TRY(fr_append_u32(out, out_cap, &used, target));
    return fr_finish_instruction_text(used, out_len, (fr_code_offset_t)(ip + 3),
                                      next_ip);
  }
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
