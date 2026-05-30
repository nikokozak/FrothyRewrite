#include "compile.h"

#include "base_defs.h"
#include "code.h"
#include "native.h"
#include "object.h"
#include "parse.h"
#include "slot.h"

#include <stdint.h>
#include <string.h>

/* Function bodies stash each text literal here so apply can install the bytes
 * and patch the PUSH_OBJECT_ID operands before code install. NULL means the
 * caller is a bare expression that installs its own text now. */
typedef struct fr_compile_body_texts_t {
  fr_image_text_object_t *objects;
  uint8_t (*storage)[FR_PROFILE_MAX_TEXT_LENGTH > 0 ? FR_PROFILE_MAX_TEXT_LENGTH
                                                    : 1];
  uint16_t capacity;
  uint16_t count;
} fr_compile_body_texts_t;

typedef struct fr_compile_context_t {
  fr_runtime_t *runtime;
  const fr_parse_span_t *params;
  uint8_t param_count;
  fr_compile_body_texts_t *body_texts;
} fr_compile_context_t;

typedef enum fr_compile_source_feature_t {
  FR_COMPILE_SOURCE_CONTROL_FLOW,
  FR_COMPILE_SOURCE_CELLS,
  FR_COMPILE_SOURCE_TEXT,
  FR_COMPILE_SOURCE_RECORDS,
} fr_compile_source_feature_t;

static fr_err_t
fr_compile_require_source_feature(fr_compile_source_feature_t feature) {
  switch (feature) {
  case FR_COMPILE_SOURCE_CONTROL_FLOW:
    return FR_FEATURE_SOURCE_CONTROL_FLOW ? FR_OK : FR_ERR_UNSUPPORTED;
  case FR_COMPILE_SOURCE_CELLS:
    return FR_FEATURE_CELLS ? FR_OK : FR_ERR_UNSUPPORTED;
  case FR_COMPILE_SOURCE_TEXT:
    return FR_FEATURE_TEXT ? FR_OK : FR_ERR_UNSUPPORTED;
  case FR_COMPILE_SOURCE_RECORDS:
    return FR_FEATURE_RECORDS ? FR_OK : FR_ERR_UNSUPPORTED;
  default:
    return FR_ERR_INVALID;
  }
}

static fr_err_t fr_compile_copy_name(fr_parse_span_t span, char *out,
                                     uint16_t out_cap) {
  if (span.start == NULL || out == NULL || out_cap == 0) {
    return FR_ERR_INVALID;
  }
  if ((uint32_t)span.length + 1 > out_cap) {
    return FR_ERR_RANGE;
  }

  memcpy(out, span.start, span.length);
  out[span.length] = '\0';
  return FR_OK;
}

/* Pack the param spans NUL-separated, one name per arg, for the code object. */
static fr_err_t fr_compile_param_names(const fr_parse_span_t *params,
                                       uint8_t count, char *out,
                                       uint16_t out_cap, uint16_t *out_len) {
  uint16_t used = 0;

  for (uint8_t i = 0; i < count; i++) {
    if (params[i].start == NULL) {
      return FR_ERR_INVALID;
    }
    if ((uint32_t)used + params[i].length + 1 > out_cap) {
      return FR_ERR_RANGE;
    }
    memcpy(&out[used], params[i].start, params[i].length);
    used = (uint16_t)(used + params[i].length);
    out[used] = '\0';
    used += 1;
  }
  *out_len = used;
  return FR_OK;
}

static fr_err_t fr_compile_slot_for_name(const fr_compile_context_t *ctx,
                                         fr_parse_span_t name,
                                         fr_slot_id_t *out_slot_id) {
  char copied[FR_PARSE_MAX_TOKEN_BYTES + 1];

  FR_TRY(fr_compile_copy_name(name, copied, sizeof(copied)));
  if (ctx != NULL && ctx->runtime != NULL) {
    return fr_slot_id_for_name(ctx->runtime, copied, out_slot_id);
  }
  return fr_base_slot_id_for_name(copied, out_slot_id);
}

static fr_err_t
fr_compile_definition_slot_with_name_storage(
    const fr_compile_context_t *ctx, fr_parse_span_t name,
    char slot_name_text[], uint16_t slot_name_text_cap,
    fr_slot_name_t *out_slot_name, fr_slot_id_t *out_slot_id,
    bool *out_has_slot_name) {
  char copied[FR_PARSE_MAX_TOKEN_BYTES + 1];
  fr_err_t err = FR_OK;

  if (slot_name_text == NULL || out_slot_name == NULL ||
      out_slot_id == NULL || out_has_slot_name == NULL) {
    return FR_ERR_INVALID;
  }
  *out_has_slot_name = false;
  FR_TRY(fr_compile_copy_name(name, copied, sizeof(copied)));

  if (ctx == NULL || ctx->runtime == NULL) {
    return fr_base_slot_id_for_name(copied, out_slot_id);
  }

  err = fr_slot_id_for_name(ctx->runtime, copied, out_slot_id);
  if (err == FR_OK) {
    return FR_OK;
  }
  if (err != FR_ERR_NOT_FOUND) {
    return err;
  }

  FR_TRY(fr_slot_prepare_project_name(ctx->runtime, copied, out_slot_id));
  if (strlen(copied) + 1 > slot_name_text_cap) {
    return FR_ERR_RANGE;
  }
  strcpy(slot_name_text, copied);
  *out_slot_name = (fr_slot_name_t){
      .slot_id = *out_slot_id,
      .name = slot_name_text,
  };
  *out_has_slot_name = true;
  return FR_OK;
}

static fr_err_t
fr_compile_definition_slot(const fr_compile_context_t *ctx,
                           fr_parse_span_t name,
                           fr_compile_overlay_update_t *out,
                           fr_slot_id_t *out_slot_id,
                           bool *out_has_slot_name) {
  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  return fr_compile_definition_slot_with_name_storage(
      ctx, name, out->slot_name_text, (uint16_t)sizeof(out->slot_name_text),
      &out->slot_name, out_slot_id, out_has_slot_name);
}

static void fr_compile_attach_slot_name(fr_compile_overlay_update_t *out,
                                        bool has_slot_name) {
  if (has_slot_name) {
    out->overlay_update.slot_names = &out->slot_name;
    out->overlay_update.slot_name_count = 1;
  }
}

static const fr_parse_expr_t *fr_compile_expr_at(const fr_parse_line_t *parsed,
                                                 fr_parse_expr_id_t expr_id) {
  if (parsed == NULL || expr_id >= parsed->expr_count) {
    return NULL;
  }
  return &parsed->exprs[expr_id];
}

static bool fr_compile_span_same(fr_parse_span_t lhs, fr_parse_span_t rhs) {
  if (lhs.length != rhs.length || lhs.start == NULL || rhs.start == NULL) {
    return false;
  }
  for (uint16_t i = 0; i < lhs.length; i++) {
    if (lhs.start[i] != rhs.start[i]) {
      return false;
    }
  }
  return true;
}

static bool fr_compile_param_for_name(const fr_compile_context_t *ctx,
                                      fr_parse_span_t name,
                                      uint8_t *out_arg_index) {
  if (ctx == NULL || ctx->params == NULL || out_arg_index == NULL) {
    return false;
  }
  for (uint8_t i = 0; i < ctx->param_count; i++) {
    if (fr_compile_span_same(ctx->params[i], name)) {
      *out_arg_index = i;
      return true;
    }
  }
  return false;
}

static fr_err_t fr_compile_write_byte(uint8_t instruction_bytes[],
                                      uint16_t *offset, uint8_t byte) {
  if (instruction_bytes == NULL || offset == NULL) {
    return FR_ERR_INVALID;
  }
  if (*offset >= FR_COMPILE_MAX_INSTRUCTION_BYTES) {
    return FR_ERR_CAPACITY;
  }
  instruction_bytes[*offset] = byte;
  *offset = (uint16_t)(*offset + 1);
  return FR_OK;
}

static fr_err_t fr_compile_write_u16(uint8_t instruction_bytes[],
                                     uint16_t *offset, uint16_t word) {
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset,
                               (uint8_t)(word & 0xFF)));
  return fr_compile_write_byte(instruction_bytes, offset,
                               (uint8_t)(word >> 8));
}

#if FR_WORD_SIZE == 16
static fr_err_t fr_compile_write_i16(uint8_t instruction_bytes[],
                                     uint16_t *offset, fr_int_t int_operand) {
  return fr_compile_write_u16(instruction_bytes, offset,
                              (uint16_t)(int16_t)int_operand);
}
#endif

#if FR_WORD_SIZE == 32
static fr_err_t fr_compile_write_u32(uint8_t instruction_bytes[],
                                     uint16_t *offset, uint32_t word) {
  if (*offset + 4u > FR_COMPILE_MAX_INSTRUCTION_BYTES) {
    return FR_ERR_CAPACITY;
  }
  fr_write_u32_le(&instruction_bytes[*offset], word);
  *offset = (uint16_t)(*offset + 4u);
  return FR_OK;
}
#endif

static fr_err_t fr_compile_write_int_operand(uint8_t instruction_bytes[],
                                             uint16_t *offset,
                                             fr_int_t int_operand) {
#if FR_WORD_SIZE == 16
  return fr_compile_write_i16(instruction_bytes, offset, int_operand);
#else
  return fr_compile_write_u32(instruction_bytes, offset,
                              (uint32_t)(int32_t)int_operand);
#endif
}

static fr_err_t fr_compile_emit_slot_op(uint8_t instruction_bytes[],
                                        uint16_t *offset, fr_opcode_t op,
                                        fr_slot_id_t slot_id) {
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset, (uint8_t)op));
  return fr_compile_write_u16(instruction_bytes, offset, slot_id);
}

static fr_err_t fr_compile_emit_call_slot_arg(uint8_t instruction_bytes[],
                                              uint16_t *offset,
                                              fr_slot_id_t slot_id,
                                              uint8_t arg_count) {
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset,
                               FR_OP_CALL_SLOT_ARG));
  FR_TRY(fr_compile_write_u16(instruction_bytes, offset, slot_id));
  return fr_compile_write_byte(instruction_bytes, offset, arg_count);
}

#if FR_FEATURE_CELLS
static fr_err_t fr_compile_cell_index(fr_int_t raw_index,
                                      uint16_t *out_index) {
  if (out_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (raw_index < 0 || (uint32_t)raw_index > UINT16_MAX) {
    return FR_ERR_RANGE;
  }
  *out_index = (uint16_t)raw_index;
  return FR_OK;
}

static fr_err_t fr_compile_emit_cell_op(uint8_t instruction_bytes[],
                                        uint16_t *offset, fr_opcode_t op,
                                        fr_slot_id_t slot_id,
                                        uint16_t index) {
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset, (uint8_t)op));
  FR_TRY(fr_compile_write_u16(instruction_bytes, offset, slot_id));
  return fr_compile_write_u16(instruction_bytes, offset, index);
}
#endif

#if FR_FEATURE_RECORDS
static fr_err_t fr_compile_emit_field_op(uint8_t instruction_bytes[],
                                         uint16_t *offset, fr_opcode_t op,
                                         fr_parse_span_t field) {
  if (field.start == NULL || field.length == 0 ||
      field.length > FR_PROFILE_MAX_NAME_BYTES || field.length > UINT8_MAX) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset, (uint8_t)op));
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset,
                               (uint8_t)field.length));
  for (uint16_t i = 0; i < field.length; i++) {
    FR_TRY(fr_compile_write_byte(instruction_bytes, offset,
                                 (uint8_t)field.start[i]));
  }
  return FR_OK;
}
#endif

static fr_err_t fr_compile_emit_load_arg(uint8_t instruction_bytes[],
                                         uint16_t *offset,
                                         uint8_t arg_index) {
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset, FR_OP_LOAD_ARG));
  return fr_compile_write_byte(instruction_bytes, offset, arg_index);
}

static fr_err_t fr_compile_emit_push_int(uint8_t instruction_bytes[],
                                         uint16_t *offset, fr_int_t value) {
  if (!fr_tagged_can_encode_int(value)) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset, FR_OP_PUSH_INT));
  return fr_compile_write_int_operand(instruction_bytes, offset, value);
}

#if FR_FEATURE_TEXT
static fr_err_t fr_compile_emit_push_object_id(uint8_t instruction_bytes[],
                                               uint16_t *offset,
                                               fr_object_id_t object_id) {
#if FR_WORD_SIZE == 16
  if ((fr_tagged_t)object_id > FR_TAGGED_OBJECT_MAX_ID) {
    return FR_ERR_RANGE;
  }
#endif
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset,
                               FR_OP_PUSH_OBJECT_ID));
  return fr_compile_write_u16(instruction_bytes, offset, (uint16_t)object_id);
}

/* Bare expressions install the text now and emit the live runtime id. Function
 * bodies stash bytes in the overlay update's text_objects[] and emit the local
 * index; apply patches each operand to the runtime id before code install, so
 * the saved code carries no host-time runtime id. */
static fr_err_t fr_compile_emit_text_literal(const fr_compile_context_t *ctx,
                                             const fr_parse_expr_t *expr,
                                             uint8_t instruction_bytes[],
                                             uint16_t *offset) {
  fr_object_id_t object_id = 0;

  if (expr == NULL) {
    return FR_ERR_INVALID;
  }
  if (ctx == NULL) {
    return FR_ERR_UNSUPPORTED;
  }
  if (expr->text.length > FR_PROFILE_MAX_TEXT_LENGTH) {
    return FR_ERR_RANGE;
  }
  if (expr->text.length > 0 && expr->text.start == NULL) {
    return FR_ERR_INVALID;
  }
  if (ctx->body_texts != NULL) {
    fr_compile_body_texts_t *bt = ctx->body_texts;

    if (bt->count >= bt->capacity) {
      return FR_ERR_RANGE;
    }
    if (expr->text.length > 0) {
      memcpy(bt->storage[bt->count], expr->text.start, expr->text.length);
    }
    bt->objects[bt->count] = (fr_image_text_object_t){
        .bytes = bt->storage[bt->count],
        .length = expr->text.length,
    };
    object_id = (fr_object_id_t)bt->count;
    bt->count = (uint16_t)(bt->count + 1);
    return fr_compile_emit_push_object_id(instruction_bytes, offset, object_id);
  }
  if (ctx->runtime == NULL) {
    return FR_ERR_UNSUPPORTED;
  }
  FR_TRY(fr_text_install(ctx->runtime, (const uint8_t *)expr->text.start,
                         expr->text.length, &object_id));
  return fr_compile_emit_push_object_id(instruction_bytes, offset, object_id);
}
#endif

static fr_err_t fr_compile_emit_push_nil(uint8_t instruction_bytes[],
                                         uint16_t *offset) {
  return fr_compile_write_byte(instruction_bytes, offset, FR_OP_PUSH_NIL);
}

static fr_err_t fr_compile_emit_push_bool(uint8_t instruction_bytes[],
                                          uint16_t *offset, bool value) {
  return fr_compile_write_byte(instruction_bytes, offset,
                               value ? FR_OP_PUSH_TRUE : FR_OP_PUSH_FALSE);
}

static fr_err_t fr_compile_emit_jump_placeholder(uint8_t instruction_bytes[],
                                                 uint16_t *offset,
                                                 fr_opcode_t op,
                                                 uint16_t *out_operand_offset) {
  if (out_operand_offset == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_compile_write_byte(instruction_bytes, offset, (uint8_t)op));
  *out_operand_offset = *offset;
  return fr_compile_write_u16(instruction_bytes, offset, 0);
}

static fr_err_t fr_compile_emit_jump_target(uint8_t instruction_bytes[],
                                            uint16_t *offset, fr_opcode_t op,
                                            uint16_t target) {
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset, (uint8_t)op));
  return fr_compile_write_u16(instruction_bytes, offset, target);
}

static fr_err_t fr_compile_patch_u16(uint8_t instruction_bytes[],
                                     uint16_t operand_offset, uint16_t value) {
  if (instruction_bytes == NULL ||
      operand_offset + 1 >= FR_COMPILE_MAX_INSTRUCTION_BYTES) {
    return FR_ERR_INVALID;
  }

  instruction_bytes[operand_offset] = (uint8_t)(value & 0xFF);
  instruction_bytes[operand_offset + 1] = (uint8_t)(value >> 8);
  return FR_OK;
}

static fr_err_t fr_compile_emit_expr(const fr_compile_context_t *ctx,
                                     const fr_parse_line_t *parsed,
                                     fr_parse_expr_id_t expr_id,
                                     uint8_t instruction_bytes[],
                                     uint16_t *offset);

static fr_err_t fr_compile_emit_binop(const fr_compile_context_t *ctx,
                                      const fr_parse_line_t *parsed,
                                      const fr_parse_expr_t *expr,
                                      uint8_t instruction_bytes[],
                                      uint16_t *offset, fr_opcode_t op) {
  if (expr->child_count != 2) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0], instruction_bytes,
                              offset));
  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[1], instruction_bytes,
                              offset));
  return fr_compile_write_byte(instruction_bytes, offset, (uint8_t)op);
}

static fr_err_t fr_compile_emit_drop(uint8_t instruction_bytes[],
                                     uint16_t *offset) {
  return fr_compile_write_byte(instruction_bytes, offset, FR_OP_DROP);
}

static fr_err_t fr_compile_emit_call(const fr_compile_context_t *ctx,
                                     const fr_parse_line_t *parsed,
                                     const fr_parse_expr_t *expr,
                                     uint8_t instruction_bytes[],
                                     uint16_t *offset) {
  const fr_base_def_t *def = NULL;
  fr_base_layer_t layer = FR_BASE_LAYER_CORE;
  const fr_native_entry_t *entry = NULL;
  fr_image_ref_t ref = {0};
  fr_tagged_t tagged = 0;
  fr_slot_id_t slot_id = 0;
  fr_native_id_t native_id = 0;
  fr_code_object_id_t code_object_id = 0;

  FR_TRY(fr_compile_slot_for_name(ctx, expr->name, &slot_id));

  if (ctx != NULL && ctx->runtime != NULL) {
    FR_TRY(fr_slot_read(ctx->runtime, slot_id, &tagged));
#if FR_FEATURE_RECORDS
    {
      fr_object_id_t object_id = 0;
      fr_record_name_t ignored_name = {0};
      uint16_t ignored_field_count = 0;

      if (fr_tagged_decode_object_id(tagged, &object_id) == FR_OK &&
          fr_record_shape_view(ctx->runtime, object_id, &ignored_name,
                               &ignored_field_count) == FR_OK) {
        return FR_ERR_UNSUPPORTED;
      }
    }
#endif
    if (fr_tagged_decode_native_id(tagged, &native_id) == FR_OK) {
      FR_TRY(fr_native_get(ctx->runtime, native_id, &entry));
      if (expr->child_count != entry->arity) {
        return FR_ERR_INVALID;
      }
      for (uint8_t i = 0; i < expr->child_count; i++) {
        const fr_parse_expr_t *arg =
            fr_compile_expr_at(parsed, expr->children[i]);

        if (arg == NULL) {
          return FR_ERR_INVALID;
        }
        FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[i],
                                    instruction_bytes, offset));
      }
      return fr_compile_emit_slot_op(instruction_bytes, offset,
                                     FR_OP_CALL_NATIVE_SLOT, slot_id);
    }
    if (fr_tagged_decode_code_object_id(tagged, &code_object_id) == FR_OK) {
      fr_instruction_stream_t instructions;
      fr_instruction_header_t header;

      FR_TRY(fr_code_get_instructions(ctx->runtime, code_object_id,
                                      &instructions));
      FR_TRY(fr_instruction_read_header(&instructions, &header));
      if (expr->child_count != header.arity) {
        return FR_ERR_INVALID;
      }
      for (uint8_t i = 0; i < expr->child_count; i++) {
        const fr_parse_expr_t *arg =
            fr_compile_expr_at(parsed, expr->children[i]);

        if (arg == NULL) {
          return FR_ERR_INVALID;
        }
        FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[i],
                                    instruction_bytes, offset));
      }
      if (header.arity == 0) {
        return fr_compile_emit_slot_op(instruction_bytes, offset,
                                       FR_OP_CALL_SLOT, slot_id);
      }
      return fr_compile_emit_call_slot_arg(instruction_bytes, offset, slot_id,
                                           header.arity);
    }
    return FR_ERR_UNSUPPORTED;
  }

  FR_TRY(fr_base_slot_ref(slot_id, &ref));
  if (ref.kind == FR_IMAGE_REF_NATIVE) {
    FR_TRY(fr_base_def_for_slot(slot_id, &def, &layer));
    if (expr->child_count != def->native_arity) {
      return FR_ERR_INVALID;
    }
    for (uint8_t i = 0; i < expr->child_count; i++) {
      const fr_parse_expr_t *arg =
          fr_compile_expr_at(parsed, expr->children[i]);

      if (arg == NULL) {
        return FR_ERR_INVALID;
      }
      FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[i],
                                  instruction_bytes, offset));
    }
    return fr_compile_emit_slot_op(instruction_bytes, offset,
                                   FR_OP_CALL_NATIVE_SLOT, slot_id);
  }

  if (ref.kind == FR_IMAGE_REF_CODE_OBJECT) {
    if (expr->child_count != 0) {
      return FR_ERR_UNSUPPORTED;
    }
    return fr_compile_emit_slot_op(instruction_bytes, offset, FR_OP_CALL_SLOT,
                                   slot_id);
  }

  return FR_ERR_UNSUPPORTED;
}

static fr_err_t fr_compile_emit_if(const fr_compile_context_t *ctx,
                                   const fr_parse_line_t *parsed,
                                   const fr_parse_expr_t *expr,
                                   uint8_t instruction_bytes[],
                                   uint16_t *offset) {
  uint16_t false_target_operand = 0;
  uint16_t end_target_operand = 0;

  if (expr == NULL || expr->child_count < 2 || expr->child_count > 3) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_compile_require_source_feature(FR_COMPILE_SOURCE_CONTROL_FLOW));

  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0],
                              instruction_bytes, offset));
  FR_TRY(fr_compile_emit_jump_placeholder(
      instruction_bytes, offset, FR_OP_JUMP_IF_FALSY, &false_target_operand));
  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[1],
                              instruction_bytes, offset));
  FR_TRY(fr_compile_emit_jump_placeholder(instruction_bytes, offset, FR_OP_JUMP,
                                          &end_target_operand));

  FR_TRY(fr_compile_patch_u16(instruction_bytes, false_target_operand, *offset));
  if (expr->child_count == 3) {
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[2],
                                instruction_bytes, offset));
  } else {
    FR_TRY(fr_compile_emit_push_nil(instruction_bytes, offset));
  }
  return fr_compile_patch_u16(instruction_bytes, end_target_operand, *offset);
}

static fr_err_t fr_compile_emit_repeat(const fr_compile_context_t *ctx,
                                       const fr_parse_line_t *parsed,
                                       const fr_parse_expr_t *expr,
                                       uint8_t instruction_bytes[],
                                       uint16_t *offset) {
  const fr_parse_expr_t *count = NULL;
  uint16_t done_target_operand = 0;
  uint16_t body_offset = 0;

  if (expr == NULL || expr->child_count != 2) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_compile_require_source_feature(FR_COMPILE_SOURCE_CONTROL_FLOW));

  count = fr_compile_expr_at(parsed, expr->children[0]);
  if (count == NULL) {
    return FR_ERR_INVALID;
  }
  if (count->kind == FR_PARSE_EXPR_INT) {
    if (count->int_value < 0) {
      return FR_ERR_RANGE;
    }
  }

  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0],
                              instruction_bytes, offset));
  FR_TRY(fr_compile_emit_jump_placeholder(
      instruction_bytes, offset, FR_OP_REPEAT_BEGIN, &done_target_operand));
  body_offset = *offset;
  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[1],
                              instruction_bytes, offset));
  FR_TRY(fr_compile_emit_drop(instruction_bytes, offset));
  FR_TRY(fr_compile_emit_jump_target(instruction_bytes, offset,
                                     FR_OP_REPEAT_NEXT, body_offset));
  FR_TRY(fr_compile_patch_u16(instruction_bytes, done_target_operand, *offset));
  return fr_compile_emit_push_nil(instruction_bytes, offset);
}

static fr_err_t fr_compile_emit_while(const fr_compile_context_t *ctx,
                                      const fr_parse_line_t *parsed,
                                      const fr_parse_expr_t *expr,
                                      uint8_t instruction_bytes[],
                                      uint16_t *offset) {
  uint16_t cond_offset = 0;
  uint16_t done_target_operand = 0;

  if (expr == NULL || expr->child_count != 2) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_compile_require_source_feature(FR_COMPILE_SOURCE_CONTROL_FLOW));

  cond_offset = *offset;
  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0],
                              instruction_bytes, offset));
  FR_TRY(fr_compile_emit_jump_placeholder(
      instruction_bytes, offset, FR_OP_JUMP_IF_FALSY, &done_target_operand));
  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[1],
                              instruction_bytes, offset));
  FR_TRY(fr_compile_emit_drop(instruction_bytes, offset));
  FR_TRY(fr_compile_emit_jump_target(instruction_bytes, offset, FR_OP_JUMP,
                                     cond_offset));
  FR_TRY(fr_compile_patch_u16(instruction_bytes, done_target_operand, *offset));
  return fr_compile_emit_push_nil(instruction_bytes, offset);
}

static fr_err_t fr_compile_emit_forever(const fr_compile_context_t *ctx,
                                        const fr_parse_line_t *parsed,
                                        const fr_parse_expr_t *expr,
                                        uint8_t instruction_bytes[],
                                        uint16_t *offset) {
  uint16_t body_offset = 0;

  if (expr == NULL || expr->child_count != 1) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_compile_require_source_feature(FR_COMPILE_SOURCE_CONTROL_FLOW));

  body_offset = *offset;
  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0],
                              instruction_bytes, offset));
  FR_TRY(fr_compile_emit_drop(instruction_bytes, offset));
  FR_TRY(fr_compile_emit_jump_target(instruction_bytes, offset, FR_OP_JUMP,
                                     body_offset));
  return fr_compile_emit_push_nil(instruction_bytes, offset);
}

static fr_err_t fr_compile_emit_expr(const fr_compile_context_t *ctx,
                                     const fr_parse_line_t *parsed,
                                     fr_parse_expr_id_t expr_id,
                                     uint8_t instruction_bytes[],
                                     uint16_t *offset) {
  const fr_parse_expr_t *expr = fr_compile_expr_at(parsed, expr_id);
  fr_slot_id_t slot_id = 0;
#if FR_FEATURE_CELLS
  uint16_t cell_index = 0;
#endif
  uint8_t arg_index = 0;

  if (expr == NULL || instruction_bytes == NULL || offset == NULL) {
    return FR_ERR_INVALID;
  }

  switch (expr->kind) {
  case FR_PARSE_EXPR_NIL:
    return fr_compile_emit_push_nil(instruction_bytes, offset);
  case FR_PARSE_EXPR_FALSE:
    return fr_compile_emit_push_bool(instruction_bytes, offset, false);
  case FR_PARSE_EXPR_TRUE:
    return fr_compile_emit_push_bool(instruction_bytes, offset, true);
  case FR_PARSE_EXPR_INT:
    return fr_compile_emit_push_int(instruction_bytes, offset, expr->int_value);
  case FR_PARSE_EXPR_TEXT:
#if FR_FEATURE_TEXT
    FR_TRY(fr_compile_require_source_feature(FR_COMPILE_SOURCE_TEXT));
    return fr_compile_emit_text_literal(ctx, expr, instruction_bytes, offset);
#else
    return FR_ERR_UNSUPPORTED;
#endif
  case FR_PARSE_EXPR_NAME:
    if (fr_compile_param_for_name(ctx, expr->name, &arg_index)) {
      return fr_compile_emit_load_arg(instruction_bytes, offset, arg_index);
    }
    FR_TRY(fr_compile_slot_for_name(ctx, expr->name, &slot_id));
    return fr_compile_emit_slot_op(instruction_bytes, offset, FR_OP_LOAD_SLOT,
                                   slot_id);
#if FR_FEATURE_CELLS
  case FR_PARSE_EXPR_CELL_READ:
    FR_TRY(fr_compile_require_source_feature(FR_COMPILE_SOURCE_CELLS));
    FR_TRY(fr_compile_slot_for_name(ctx, expr->name, &slot_id));
    FR_TRY(fr_compile_cell_index(expr->int_value, &cell_index));
    return fr_compile_emit_cell_op(instruction_bytes, offset, FR_OP_LOAD_CELL,
                                   slot_id, cell_index);
  case FR_PARSE_EXPR_CELL_WRITE:
    FR_TRY(fr_compile_require_source_feature(FR_COMPILE_SOURCE_CELLS));
    if (expr->child_count != 1) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_compile_slot_for_name(ctx, expr->name, &slot_id));
    FR_TRY(fr_compile_cell_index(expr->int_value, &cell_index));
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->child, instruction_bytes,
                                offset));
    return fr_compile_emit_cell_op(instruction_bytes, offset, FR_OP_STORE_CELL,
                                   slot_id, cell_index);
#else
  case FR_PARSE_EXPR_CELL_READ:
  case FR_PARSE_EXPR_CELL_WRITE:
    return FR_ERR_UNSUPPORTED;
#endif
#if FR_FEATURE_RECORDS
  case FR_PARSE_EXPR_FIELD_READ:
    FR_TRY(fr_compile_require_source_feature(FR_COMPILE_SOURCE_RECORDS));
    if (expr->child_count != 1) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->child, instruction_bytes,
                                offset));
    return fr_compile_emit_field_op(instruction_bytes, offset,
                                    FR_OP_LOAD_FIELD, expr->name);
  case FR_PARSE_EXPR_FIELD_WRITE:
    FR_TRY(fr_compile_require_source_feature(FR_COMPILE_SOURCE_RECORDS));
    if (expr->child_count != 2) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0],
                                instruction_bytes, offset));
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[1],
                                instruction_bytes, offset));
    return fr_compile_emit_field_op(instruction_bytes, offset,
                                    FR_OP_STORE_FIELD, expr->name);
#else
  case FR_PARSE_EXPR_FIELD_READ:
  case FR_PARSE_EXPR_FIELD_WRITE:
    return FR_ERR_UNSUPPORTED;
#endif
  case FR_PARSE_EXPR_SLOT_WRITE:
    if (expr->child_count != 1) {
      return FR_ERR_INVALID;
    }
    /* Parameters are immutable; trying to `set arg to ...` is a source bug. */
    if (fr_compile_param_for_name(ctx, expr->name, &arg_index)) {
      return FR_ERR_INVALID;
    }
    /* fr_compile_slot_for_name errors if the name has never been declared,
     * which is exactly the "set on undeclared slot" rejection we want. */
    FR_TRY(fr_compile_slot_for_name(ctx, expr->name, &slot_id));
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->child, instruction_bytes,
                                offset));
    return fr_compile_emit_slot_op(instruction_bytes, offset, FR_OP_STORE_SLOT,
                                   slot_id);
  case FR_PARSE_EXPR_LT:
    return fr_compile_emit_binop(ctx, parsed, expr, instruction_bytes, offset,
                                 FR_OP_LT_INT);
  case FR_PARSE_EXPR_GT:
    return fr_compile_emit_binop(ctx, parsed, expr, instruction_bytes, offset,
                                 FR_OP_GT_INT);
  case FR_PARSE_EXPR_LE:
    return fr_compile_emit_binop(ctx, parsed, expr, instruction_bytes, offset,
                                 FR_OP_LE_INT);
  case FR_PARSE_EXPR_GE:
    return fr_compile_emit_binop(ctx, parsed, expr, instruction_bytes, offset,
                                 FR_OP_GE_INT);
  case FR_PARSE_EXPR_EQ:
    return fr_compile_emit_binop(ctx, parsed, expr, instruction_bytes, offset,
                                 FR_OP_EQ_INT);
  case FR_PARSE_EXPR_NE:
    return fr_compile_emit_binop(ctx, parsed, expr, instruction_bytes, offset,
                                 FR_OP_NE_INT);
  case FR_PARSE_EXPR_ADD:
    return fr_compile_emit_binop(ctx, parsed, expr, instruction_bytes, offset,
                                 FR_OP_ADD_INT);
  case FR_PARSE_EXPR_SUB:
    return fr_compile_emit_binop(ctx, parsed, expr, instruction_bytes, offset,
                                 FR_OP_SUB_INT);
  case FR_PARSE_EXPR_MUL:
    return fr_compile_emit_binop(ctx, parsed, expr, instruction_bytes, offset,
                                 FR_OP_MUL_INT);
  case FR_PARSE_EXPR_DIV:
    return fr_compile_emit_binop(ctx, parsed, expr, instruction_bytes, offset,
                                 FR_OP_DIV_INT);
#if FR_FEATURE_MATH
  case FR_PARSE_EXPR_MOD:
    if (expr->child_count != 2) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0],
                                instruction_bytes, offset));
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[1],
                                instruction_bytes, offset));
    return fr_compile_emit_slot_op(instruction_bytes, offset,
                                   FR_OP_CALL_NATIVE_SLOT, FR_SLOT_MOD);
#endif
  case FR_PARSE_EXPR_CALL:
    return fr_compile_emit_call(ctx, parsed, expr, instruction_bytes, offset);
  case FR_PARSE_EXPR_LIST:
    if (expr->child_count == 0) {
      return FR_ERR_INVALID;
    }
    for (uint8_t i = 0; i < expr->child_count; i++) {
      const fr_parse_expr_t *child =
          fr_compile_expr_at(parsed, expr->children[i]);

      if (child == NULL) {
        return FR_ERR_INVALID;
      }
      FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[i],
                                  instruction_bytes, offset));
      if (i + 1 < expr->child_count) {
        FR_TRY(fr_compile_emit_drop(instruction_bytes, offset));
      }
    }
    return FR_OK;
  case FR_PARSE_EXPR_IF:
    return fr_compile_emit_if(ctx, parsed, expr, instruction_bytes, offset);
  case FR_PARSE_EXPR_REPEAT:
    return fr_compile_emit_repeat(ctx, parsed, expr, instruction_bytes, offset);
  case FR_PARSE_EXPR_WHILE:
    return fr_compile_emit_while(ctx, parsed, expr, instruction_bytes, offset);
  case FR_PARSE_EXPR_FOREVER:
    return fr_compile_emit_forever(ctx, parsed, expr, instruction_bytes,
                                   offset);
  default:
    return FR_ERR_UNSUPPORTED;
  }
}

static fr_err_t fr_compile_literal_ref(const fr_compile_context_t *ctx,
                                       const fr_parse_expr_t *expr,
                                       fr_image_ref_t *out_ref) {
  fr_tagged_t tagged = 0;
  fr_slot_id_t slot_id = 0;

  if (expr == NULL || out_ref == NULL) {
    return FR_ERR_INVALID;
  }

  if (expr->kind == FR_PARSE_EXPR_NIL) {
    *out_ref = (fr_image_ref_t){FR_IMAGE_REF_LITERAL_TAGGED, FR_TAGGED_NIL, 0};
    return FR_OK;
  }
  if (expr->kind == FR_PARSE_EXPR_FALSE || expr->kind == FR_PARSE_EXPR_TRUE) {
    FR_TRY(fr_tagged_encode_bool(expr->kind == FR_PARSE_EXPR_TRUE, &tagged));
    *out_ref = (fr_image_ref_t){FR_IMAGE_REF_LITERAL_TAGGED, tagged, 0};
    return FR_OK;
  }
  if (expr->kind == FR_PARSE_EXPR_INT) {
    FR_TRY(fr_tagged_encode_int(expr->int_value, &tagged));
    *out_ref = (fr_image_ref_t){FR_IMAGE_REF_LITERAL_TAGGED, tagged, 0};
    return FR_OK;
  }
  if (expr->kind == FR_PARSE_EXPR_NAME && ctx != NULL &&
      ctx->runtime != NULL) {
    FR_TRY(fr_compile_slot_for_name(ctx, expr->name, &slot_id));
    FR_TRY(fr_slot_read(ctx->runtime, slot_id, &tagged));
    *out_ref = (fr_image_ref_t){FR_IMAGE_REF_LITERAL_TAGGED, tagged, 0};
    return FR_OK;
  }
  return FR_ERR_UNSUPPORTED;
}

#if FR_FEATURE_RECORDS
static bool fr_compile_record_name_matches_span(fr_record_name_t name,
                                                fr_parse_span_t span) {
  if (name.length != span.length) {
    return false;
  }
  if (name.length == 0) {
    return true;
  }
  if (name.bytes == NULL || span.start == NULL) {
    return false;
  }
  return memcmp(name.bytes, span.start, name.length) == 0;
}

static fr_err_t fr_compile_check_record_shape_redefinition(
    const fr_compile_context_t *ctx, const fr_parse_line_t *parsed,
    fr_slot_id_t slot_id) {
  fr_tagged_t tagged = 0;
  fr_object_id_t object_id = 0;
  fr_record_name_t shape_name = {0};
  uint16_t field_count = 0;

  if (parsed == NULL) {
    return FR_ERR_INVALID;
  }
  if (ctx == NULL || ctx->runtime == NULL) {
    return FR_OK;
  }
  FR_TRY(fr_slot_read(ctx->runtime, slot_id, &tagged));
  if (fr_tagged_is_nil(tagged)) {
    return FR_OK;
  }
  if (fr_tagged_decode_object_id(tagged, &object_id) != FR_OK) {
    return FR_ERR_TYPE;
  }
  FR_TRY(fr_record_shape_view(ctx->runtime, object_id, &shape_name,
                              &field_count));
  if (field_count != parsed->record_field_count ||
      !fr_compile_record_name_matches_span(shape_name, parsed->definition.name)) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < field_count; i++) {
    fr_record_name_t field_name = {0};

    FR_TRY(fr_record_shape_field_name(ctx->runtime, object_id, i,
                                      &field_name));
    if (!fr_compile_record_name_matches_span(field_name,
                                             parsed->record_fields[i])) {
      return FR_ERR_INVALID;
    }
  }
  return FR_OK;
}

static fr_err_t fr_compile_record_shape_line(
    const fr_compile_context_t *ctx, const fr_parse_line_t *parsed,
    fr_slot_id_t slot_id, fr_compile_overlay_update_t *out) {
  FR_TRY(fr_compile_require_source_feature(FR_COMPILE_SOURCE_RECORDS));
  if (parsed == NULL || out == NULL || parsed->record_field_count == 0 ||
      parsed->record_field_count > FR_RECORD_FIELDS_PER_SHAPE_CAPACITY) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_compile_check_record_shape_redefinition(ctx, parsed, slot_id));
  FR_TRY(fr_compile_copy_name(parsed->definition.name, out->record_name_text,
                              sizeof(out->record_name_text)));
  out->record_shape_object.name = (fr_record_name_t){
      .bytes = (const uint8_t *)out->record_name_text,
      .length = parsed->definition.name.length,
  };
  for (uint8_t i = 0; i < parsed->record_field_count; i++) {
    FR_TRY(fr_compile_copy_name(parsed->record_fields[i],
                                out->record_field_name_text[i],
                                sizeof(out->record_field_name_text[i])));
    out->record_fields[i] = (fr_record_name_t){
        .bytes = (const uint8_t *)out->record_field_name_text[i],
        .length = parsed->record_fields[i].length,
    };
  }
  out->record_shape_object.fields = out->record_fields;
  out->record_shape_object.field_count = parsed->record_field_count;
  out->slot_inits[0] = (fr_image_slot_init_t){
      slot_id, {FR_IMAGE_REF_RECORD_SHAPE_OBJECT, 0, 0}};
  out->overlay_update = (fr_overlay_update_t){
      .slot_inits = out->slot_inits,
      .slot_init_count = 1,
      .record_shape_objects = &out->record_shape_object,
      .record_shape_object_count = 1,
      .code_objects = NULL,
      .code_object_count = 0,
      .natives = NULL,
      .native_count = 0,
  };
  return FR_OK;
}

static fr_err_t fr_compile_record_field_ref(
    const fr_compile_context_t *ctx, const fr_parse_expr_t *expr,
    fr_compile_overlay_update_t *out, uint16_t *text_count,
    fr_image_ref_t *out_ref) {
  if (ctx == NULL || ctx->runtime == NULL || expr == NULL || out == NULL ||
      text_count == NULL || out_ref == NULL) {
    return FR_ERR_INVALID;
  }
  if (expr->kind == FR_PARSE_EXPR_TEXT) {
    if (*text_count >= FR_PARSE_MAX_BODY_EXPRS ||
        expr->text.length > FR_PROFILE_MAX_TEXT_LENGTH) {
      return FR_ERR_RANGE;
    }
    if (expr->text.length > 0 && expr->text.start == NULL) {
      return FR_ERR_INVALID;
    }
    if (expr->text.length > 0) {
      memcpy(out->body_text_bytes[*text_count], expr->text.start,
             expr->text.length);
    }
    out->text_objects[*text_count] = (fr_image_text_object_t){
        .bytes = out->body_text_bytes[*text_count],
        .length = expr->text.length,
    };
    *out_ref =
        (fr_image_ref_t){FR_IMAGE_REF_TEXT_OBJECT, 0, *text_count};
    *text_count = (uint16_t)(*text_count + 1);
    return FR_OK;
  }
  return fr_compile_literal_ref(ctx, expr, out_ref);
}

static fr_err_t fr_compile_record_object(
    const fr_compile_context_t *ctx, const fr_parse_line_t *parsed,
    const fr_parse_expr_t *value, fr_slot_id_t slot_id,
    fr_compile_overlay_update_t *out) {
  fr_slot_id_t shape_slot = 0;
  fr_tagged_t shape_tagged = 0;
  fr_object_id_t shape_object_id = 0;
  fr_record_name_t shape_name = {0};
  uint16_t shape_field_count = 0;
  uint16_t text_count = 0;

  if (ctx == NULL || ctx->runtime == NULL) {
    return FR_ERR_UNSUPPORTED;
  }
  if (parsed == NULL || value == NULL || out == NULL ||
      value->kind != FR_PARSE_EXPR_CALL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_compile_require_source_feature(FR_COMPILE_SOURCE_RECORDS));
  FR_TRY(fr_compile_slot_for_name(ctx, value->name, &shape_slot));
  FR_TRY(fr_slot_read(ctx->runtime, shape_slot, &shape_tagged));
  FR_TRY(fr_tagged_decode_object_id(shape_tagged, &shape_object_id));
  FR_TRY(fr_record_shape_view(ctx->runtime, shape_object_id, &shape_name,
                              &shape_field_count));
  if (value->child_count != shape_field_count ||
      value->child_count > FR_RECORD_FIELDS_PER_SHAPE_CAPACITY) {
    return FR_ERR_INVALID;
  }
  for (uint8_t i = 0; i < value->child_count; i++) {
    const fr_parse_expr_t *field_expr =
        fr_compile_expr_at(parsed, value->children[i]);
    if (field_expr == NULL) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_compile_record_field_ref(ctx, field_expr, out, &text_count,
                                       &out->record_field_refs[i]));
  }
  out->record_object = (fr_image_record_object_t){
      .shape = (fr_image_ref_t){FR_IMAGE_REF_LITERAL_TAGGED, shape_tagged, 0},
      .field_values = out->record_field_refs,
      .field_count = value->child_count,
  };
  out->slot_inits[0] = (fr_image_slot_init_t){
      slot_id, {FR_IMAGE_REF_RECORD_OBJECT, 0, 0}};
  out->overlay_update = (fr_overlay_update_t){
      .slot_inits = out->slot_inits,
      .slot_init_count = 1,
      .text_objects = text_count > 0 ? out->text_objects : NULL,
      .text_object_count = text_count,
      .record_objects = &out->record_object,
      .record_object_count = 1,
      .code_objects = NULL,
      .code_object_count = 0,
      .natives = NULL,
      .native_count = 0,
  };
  return FR_OK;
}
#endif

static fr_err_t fr_compile_function(const fr_compile_context_t *ctx,
                                    const fr_parse_line_t *parsed,
                                    const fr_parse_expr_t *function,
                                    fr_compile_overlay_update_t *out) {
  fr_compile_context_t body_ctx = {0};
  fr_compile_body_texts_t body_texts = {
      .objects = out->text_objects,
      .storage = out->body_text_bytes,
      .capacity = FR_PARSE_MAX_BODY_EXPRS,
      .count = 0,
  };
  uint8_t arity = 0;
  uint8_t header_size = FR_INSTRUCTION_MIN_HEADER_SIZE;
  uint16_t offset = 0;

  if (function == NULL || function->kind != FR_PARSE_EXPR_FUNCTION ||
      function->child_count != 1) {
    return FR_ERR_INVALID;
  }
  if (parsed == NULL ||
      (uint16_t)function->param_start + function->param_count >
          parsed->param_count ||
      function->param_count > FR_PROFILE_MAX_STACK_DEPTH) {
    return FR_ERR_INVALID;
  }
  arity = function->param_count;
  if (arity > 0) {
    header_size = FR_INSTRUCTION_ARITY_HEADER_SIZE;
  }
  offset = header_size;
  if (ctx != NULL) {
    body_ctx = *ctx;
  }
  body_ctx.params = arity > 0 ? &parsed->params[function->param_start] : NULL;
  body_ctx.param_count = arity;
  body_ctx.body_texts = &body_texts;

  out->instruction_bytes[0] = FR_INSTRUCTION_FORMAT_VERSION;
  out->instruction_bytes[1] = header_size;
  if (header_size >= FR_INSTRUCTION_ARITY_HEADER_SIZE) {
    out->instruction_bytes[2] = arity;
  }
  FR_TRY(fr_compile_emit_expr(&body_ctx, parsed, function->child,
                              out->instruction_bytes, &offset));
  FR_TRY(fr_compile_write_byte(out->instruction_bytes, &offset, FR_OP_RETURN));

  out->code_object = (fr_image_code_object_t){
      .instructions = (fr_instruction_stream_t){.bytes = out->instruction_bytes,
                                                .length = offset},
  };
  if (arity > 0) {
    uint16_t names_len = 0;
    FR_TRY(fr_compile_param_names(body_ctx.params, arity, out->param_name_text,
                                  (uint16_t)sizeof(out->param_name_text),
                                  &names_len));
    out->code_object.param_names = out->param_name_text;
    out->code_object.param_names_length = names_len;
  }

  out->slot_inits[0] = (fr_image_slot_init_t){0,
                                              {FR_IMAGE_REF_CODE_OBJECT, 0, 0}};
  out->overlay_update = (fr_overlay_update_t){
      .slot_inits = out->slot_inits,
      .slot_init_count = 1,
      .code_objects = &out->code_object,
      .code_object_count = 1,
      .text_objects = body_texts.count > 0 ? out->text_objects : NULL,
      .text_object_count = body_texts.count,
      .natives = NULL,
      .native_count = 0,
  };
  return FR_OK;
}

static fr_err_t
fr_compile_overlay_update_with_context(const fr_compile_context_t *ctx,
                                       const char *source,
                                       fr_compile_overlay_update_t *out) {
  fr_parse_line_t parsed = {0};
  const fr_parse_expr_t *value = NULL;
  fr_slot_id_t slot_id = 0;
  bool has_slot_name = false;

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  memset(out, 0, sizeof(*out));

  FR_TRY(fr_parse_line(source, &parsed));
  FR_TRY(fr_compile_definition_slot(ctx, parsed.definition.name, out, &slot_id,
                                    &has_slot_name));
  if (parsed.kind == FR_PARSE_LINE_RECORD_SHAPE) {
#if !FR_FEATURE_RECORDS
    return FR_ERR_UNSUPPORTED;
#else
    FR_TRY(fr_compile_record_shape_line(ctx, &parsed, slot_id, out));
    fr_compile_attach_slot_name(out, has_slot_name);
    return FR_OK;
#endif
  }
  value = fr_compile_expr_at(&parsed, parsed.definition.value);
  if (value == NULL) {
    return FR_ERR_INVALID;
  }

  if (value->kind == FR_PARSE_EXPR_FUNCTION) {
    FR_TRY(fr_compile_function(ctx, &parsed, value, out));
    out->slot_inits[0].slot_id = slot_id;
    fr_compile_attach_slot_name(out, has_slot_name);
    return FR_OK;
  }

#if FR_FEATURE_CELLS
  if (value->kind == FR_PARSE_EXPR_CELLS) {
    FR_TRY(fr_compile_require_source_feature(FR_COMPILE_SOURCE_CELLS));
    if (value->int_value <= 0 ||
        value->int_value > FR_PROFILE_MAX_CELL_LENGTH) {
      return FR_ERR_RANGE;
    }
    out->cell_object = (fr_image_cell_object_t){
        .length = (uint16_t)value->int_value,
        .initial_values = NULL,
    };
    out->slot_inits[0] =
        (fr_image_slot_init_t){slot_id, {FR_IMAGE_REF_CELL_OBJECT, 0, 0}};
    out->overlay_update = (fr_overlay_update_t){
        .slot_inits = out->slot_inits,
        .slot_init_count = 1,
        .cell_objects = &out->cell_object,
        .cell_object_count = 1,
        .code_objects = NULL,
        .code_object_count = 0,
        .natives = NULL,
        .native_count = 0,
    };
    fr_compile_attach_slot_name(out, has_slot_name);
    return FR_OK;
  }
#else
  if (value->kind == FR_PARSE_EXPR_CELLS) {
    return FR_ERR_UNSUPPORTED;
  }
#endif

#if FR_FEATURE_TEXT
  if (value->kind == FR_PARSE_EXPR_TEXT) {
    FR_TRY(fr_compile_require_source_feature(FR_COMPILE_SOURCE_TEXT));
    if (value->text.length > FR_PROFILE_MAX_TEXT_LENGTH) {
      return FR_ERR_RANGE;
    }
    if (value->text.length > 0 && value->text.start == NULL) {
      return FR_ERR_INVALID;
    }
    if (value->text.length > 0) {
      memcpy(out->text_bytes, value->text.start, value->text.length);
    }
    out->text_object = (fr_image_text_object_t){
        .bytes = out->text_bytes,
        .length = value->text.length,
    };
    out->slot_inits[0] =
        (fr_image_slot_init_t){slot_id, {FR_IMAGE_REF_TEXT_OBJECT, 0, 0}};
    out->overlay_update = (fr_overlay_update_t){
        .slot_inits = out->slot_inits,
        .slot_init_count = 1,
        .text_objects = &out->text_object,
        .text_object_count = 1,
        .code_objects = NULL,
        .code_object_count = 0,
        .natives = NULL,
        .native_count = 0,
    };
    fr_compile_attach_slot_name(out, has_slot_name);
    return FR_OK;
  }
#else
  if (value->kind == FR_PARSE_EXPR_TEXT) {
    return FR_ERR_UNSUPPORTED;
  }
#endif

#if FR_FEATURE_RECORDS
  if (value->kind == FR_PARSE_EXPR_CALL) {
    fr_err_t record_err =
        fr_compile_record_object(ctx, &parsed, value, slot_id, out);
    if (record_err == FR_OK) {
      fr_compile_attach_slot_name(out, has_slot_name);
      return FR_OK;
    }
    if (record_err != FR_ERR_TYPE && record_err != FR_ERR_UNSUPPORTED) {
      return record_err;
    }
  }
#else
  if (value->kind == FR_PARSE_EXPR_FIELD_READ ||
      value->kind == FR_PARSE_EXPR_FIELD_WRITE) {
    return FR_ERR_UNSUPPORTED;
  }
#endif

  FR_TRY(fr_compile_literal_ref(ctx, value, &out->slot_inits[0].ref));
  out->slot_inits[0].slot_id = slot_id;
  out->overlay_update = (fr_overlay_update_t){
      .slot_inits = out->slot_inits,
      .slot_init_count = 1,
      .code_objects = NULL,
      .code_object_count = 0,
      .natives = NULL,
      .native_count = 0,
      .slot_names = has_slot_name ? &out->slot_name : NULL,
      .slot_name_count = has_slot_name ? 1 : 0,
  };
  return FR_OK;
}

fr_err_t fr_compile_overlay_update(const char *source,
                                   fr_compile_overlay_update_t *out) {
  return fr_compile_overlay_update_with_context(NULL, source, out);
}

fr_err_t fr_compile_overlay_update_for_runtime(
    fr_runtime_t *runtime, const char *source, fr_compile_overlay_update_t *out) {
  const fr_compile_context_t ctx = {
      .runtime = runtime,
  };

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  return fr_compile_overlay_update_with_context(&ctx, source, out);
}

static bool fr_compile_expr_is_runtime_binding_value(
    const fr_parse_expr_t *expr) {
  return expr != NULL && expr->kind == FR_PARSE_EXPR_CALL;
}

fr_err_t fr_compile_value_binding_for_runtime(
    fr_runtime_t *runtime, const char *source, fr_compile_value_binding_t *out) {
  const fr_compile_context_t ctx = {
      .runtime = runtime,
  };
  fr_parse_line_t parsed = {0};
  const fr_parse_expr_t *value = NULL;
  uint16_t offset = FR_INSTRUCTION_MIN_HEADER_SIZE;

  if (runtime == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  memset(out, 0, sizeof(*out));

  FR_TRY(fr_parse_line(source, &parsed));
  if (parsed.kind != FR_PARSE_LINE_DEFINITION) {
    return FR_ERR_UNSUPPORTED;
  }
  value = fr_compile_expr_at(&parsed, parsed.definition.value);
  if (!fr_compile_expr_is_runtime_binding_value(value)) {
    return FR_ERR_UNSUPPORTED;
  }

  FR_TRY(fr_compile_definition_slot_with_name_storage(
      &ctx, parsed.definition.name, out->slot_name_text,
      (uint16_t)sizeof(out->slot_name_text), &out->slot_name, &out->slot_id,
      &out->has_slot_name));

  out->instruction_bytes[0] = FR_INSTRUCTION_FORMAT_VERSION;
  out->instruction_bytes[1] = FR_INSTRUCTION_MIN_HEADER_SIZE;
  FR_TRY(fr_compile_emit_expr(&ctx, &parsed, parsed.definition.value,
                              out->instruction_bytes, &offset));
  FR_TRY(fr_compile_emit_slot_op(out->instruction_bytes, &offset,
                                 FR_OP_STORE_SLOT, out->slot_id));
  FR_TRY(fr_compile_write_byte(out->instruction_bytes, &offset, FR_OP_RETURN));

  out->instructions = (fr_instruction_stream_t){.bytes = out->instruction_bytes,
                                                .length = offset};
  return FR_OK;
}

static fr_err_t
fr_compile_expression_with_context(const fr_compile_context_t *ctx,
                                   const char *source,
                                   fr_compile_expression_t *out) {
  fr_parse_line_t parsed = {0};
  fr_parse_expr_id_t expr_id = 0;
  uint16_t offset = FR_INSTRUCTION_MIN_HEADER_SIZE;

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  memset(out, 0, sizeof(*out));

  FR_TRY(fr_parse_expression_line(source, &parsed, &expr_id));

  out->instruction_bytes[0] = FR_INSTRUCTION_FORMAT_VERSION;
  out->instruction_bytes[1] = FR_INSTRUCTION_MIN_HEADER_SIZE;
  FR_TRY(fr_compile_emit_expr(ctx, &parsed, expr_id, out->instruction_bytes,
                              &offset));
  FR_TRY(fr_compile_write_byte(out->instruction_bytes, &offset, FR_OP_RETURN));

  out->instructions = (fr_instruction_stream_t){.bytes = out->instruction_bytes,
                                                .length = offset};
  return FR_OK;
}

fr_err_t fr_compile_expression(const char *source,
                               fr_compile_expression_t *out) {
  return fr_compile_expression_with_context(NULL, source, out);
}

fr_err_t fr_compile_expression_for_runtime(fr_runtime_t *runtime,
                                           const char *source,
                                           fr_compile_expression_t *out) {
  const fr_compile_context_t ctx = {
      .runtime = runtime,
  };

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  return fr_compile_expression_with_context(&ctx, source, out);
}
