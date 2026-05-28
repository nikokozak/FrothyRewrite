/*
 * Rebuilds Frothy source by simulating the operand stack with text fragments:
 * leaves push text, binops/calls/if pop operands and push the combined form.
 * It reads our compiler's emission, not a stored AST. Shapes it can't place
 * return FR_ERR_UNSUPPORTED so see falls back to the bytecode listing.
 */

#include "source_render.h"

#if FR_FEATURE_INTROSPECTION

#include "base_defs.h"
#include "code.h"
#include "instruction.h"
#include "native.h"
#include "slot.h"
#include "tagged.h"

#include <stdbool.h>

/* Single-shot scratch: see finishes before the next REPL line, the same
 * lifetime the apply/run wire buffers in repl.c rely on. */
static char fr_source_render_arena[FR_PROFILE_SOURCE_RENDER_BYTES];

typedef struct fr_source_frag_t {
  uint16_t start;     /* offset of a NUL-terminated string in the arena */
  bool parenthesize;  /* a binop result, wrapped when it feeds another binop */
} fr_source_frag_t;

typedef struct fr_source_render_t {
  fr_runtime_t *runtime;
  uint16_t used;
  uint8_t depth;
  fr_source_frag_t stack[FR_PROFILE_MAX_STACK_DEPTH + 1];
} fr_source_render_t;

static fr_err_t fr_source_putc(fr_source_render_t *r, char ch) {
  if ((uint32_t)r->used + 2u > sizeof(fr_source_render_arena)) {
    return FR_ERR_CAPACITY;
  }
  fr_source_render_arena[r->used] = ch;
  r->used = (uint16_t)(r->used + 1u);
  fr_source_render_arena[r->used] = '\0';
  return FR_OK;
}

static fr_err_t fr_source_puts(fr_source_render_t *r, const char *text) {
  while (*text != '\0') {
    FR_TRY(fr_source_putc(r, *text));
    text += 1;
  }
  return FR_OK;
}

static fr_err_t fr_source_put_int(fr_source_render_t *r, fr_int_t value) {
  char digits[10];
  uint8_t count = 0;
  uint32_t magnitude = 0;

  if (value < 0) {
    FR_TRY(fr_source_putc(r, '-'));
    magnitude = (uint32_t)(-(int32_t)(value + 1)) + 1u;
  } else {
    magnitude = (uint32_t)(int32_t)value;
  }
  do {
    digits[count] = (char)('0' + (magnitude % 10u));
    count += 1;
    magnitude /= 10u;
  } while (magnitude > 0);
  while (count > 0) {
    count -= 1;
    FR_TRY(fr_source_putc(r, digits[count]));
  }
  return FR_OK;
}

/* Step past the trailing NUL, then record the fragment on the stack. */
static fr_err_t fr_source_seal(fr_source_render_t *r, uint16_t start,
                               bool parenthesize) {
  if (r->depth >= (uint8_t)(sizeof(r->stack) / sizeof(r->stack[0]))) {
    return FR_ERR_CAPACITY;
  }
  if ((uint32_t)r->used + 1u > sizeof(fr_source_render_arena)) {
    return FR_ERR_CAPACITY;
  }
  r->used = (uint16_t)(r->used + 1u);
  r->stack[r->depth].start = start;
  r->stack[r->depth].parenthesize = parenthesize;
  r->depth = (uint8_t)(r->depth + 1u);
  return FR_OK;
}

static fr_err_t fr_source_push_text(fr_source_render_t *r, const char *text) {
  uint16_t start = r->used;

  FR_TRY(fr_source_puts(r, text));
  return fr_source_seal(r, start, false);
}

static fr_err_t fr_source_push_int(fr_source_render_t *r, fr_int_t value) {
  uint16_t start = r->used;

  FR_TRY(fr_source_put_int(r, value));
  return fr_source_seal(r, start, false);
}

static fr_err_t fr_source_emit_operand(fr_source_render_t *r,
                                       fr_source_frag_t frag) {
  if (frag.parenthesize) {
    FR_TRY(fr_source_putc(r, '('));
  }
  FR_TRY(fr_source_puts(r, &fr_source_render_arena[frag.start]));
  if (frag.parenthesize) {
    FR_TRY(fr_source_putc(r, ')'));
  }
  return FR_OK;
}

static fr_err_t fr_source_reduce_binop(fr_source_render_t *r,
                                       const char *symbol) {
  fr_source_frag_t lhs;
  fr_source_frag_t rhs;
  uint16_t start = r->used;

  if (r->depth < 2) {
    return FR_ERR_UNSUPPORTED;
  }
  rhs = r->stack[r->depth - 1];
  lhs = r->stack[r->depth - 2];
  r->depth = (uint8_t)(r->depth - 2);

  FR_TRY(fr_source_emit_operand(r, lhs));
  FR_TRY(fr_source_putc(r, ' '));
  FR_TRY(fr_source_puts(r, symbol));
  FR_TRY(fr_source_putc(r, ' '));
  FR_TRY(fr_source_emit_operand(r, rhs));
  return fr_source_seal(r, start, true);
}

static fr_err_t fr_source_reduce_call(fr_source_render_t *r, const char *name,
                                      uint8_t arg_count) {
  uint16_t start = r->used;
  uint8_t base = 0;

  /* A zero-arg call reads as a bare name, indistinguishable from a load, so
   * leave it to the fallback rather than emit ambiguous source. */
  if (arg_count == 0 || r->depth < arg_count) {
    return FR_ERR_UNSUPPORTED;
  }
  base = (uint8_t)(r->depth - arg_count);

  FR_TRY(fr_source_puts(r, name));
  FR_TRY(fr_source_puts(r, ": "));
  for (uint8_t i = 0; i < arg_count; i++) {
    if (i > 0) {
      FR_TRY(fr_source_puts(r, ", "));
    }
    FR_TRY(fr_source_puts(r, &fr_source_render_arena[r->stack[base + i].start]));
  }
  r->depth = base;
  return fr_source_seal(r, start, false);
}

static const char *fr_source_slot_name(fr_source_render_t *r,
                                       fr_slot_id_t slot_id) {
  const char *name = fr_base_slot_name(slot_id);

  if (name != NULL) {
    return name;
  }
  return fr_slot_name(r->runtime, slot_id);
}

static fr_err_t fr_source_native_arity(fr_source_render_t *r,
                                       fr_slot_id_t slot_id,
                                       uint8_t *out_arity) {
  fr_tagged_t tagged = 0;
  fr_native_id_t native_id = 0;
  const fr_native_entry_t *entry = NULL;

  FR_TRY(fr_slot_read(r->runtime, slot_id, &tagged));
  if (fr_tagged_decode_native_id(tagged, &native_id) != FR_OK) {
    return FR_ERR_UNSUPPORTED;
  }
  FR_TRY(fr_native_get(r->runtime, native_id, &entry));
  *out_arity = entry->arity;
  return FR_OK;
}

/* Names are packed one per arg, each NUL-terminated. */
static uint8_t fr_source_param_count(const char *names, uint16_t names_len) {
  uint8_t count = 0;

  for (uint16_t pos = 0; pos < names_len; pos++) {
    if (names[pos] == '\0') {
      count = (uint8_t)(count + 1u);
    }
  }
  return count;
}

static fr_err_t fr_source_param_name_at(const char *names, uint16_t names_len,
                                        uint8_t index, const char **out_name) {
  uint16_t pos = 0;

  for (uint8_t seen = 0; seen < index; seen++) {
    while (pos < names_len && names[pos] != '\0') {
      pos += 1;
    }
    if (pos >= names_len) {
      return FR_ERR_UNSUPPORTED;
    }
    pos += 1;
  }
  if (pos >= names_len) {
    return FR_ERR_UNSUPPORTED;
  }
  *out_name = &names[pos];
  return FR_OK;
}

static fr_err_t fr_source_render_span(fr_source_render_t *r,
                                      const fr_instruction_stream_t *view,
                                      const char *names, uint16_t names_len,
                                      fr_code_offset_t ip, fr_code_offset_t end);

/* if/else leaves one value; assemble it from the already-rendered fragments.
 * The condition prints raw — a comparison reads fine without parens here. */
static fr_err_t fr_source_reduce_if(fr_source_render_t *r,
                                    fr_source_frag_t cond, uint16_t then_start,
                                    uint16_t else_start, bool has_else) {
  uint16_t start = r->used;

  FR_TRY(fr_source_puts(r, "if "));
  FR_TRY(fr_source_puts(r, &fr_source_render_arena[cond.start]));
  FR_TRY(fr_source_puts(r, " [ "));
  FR_TRY(fr_source_puts(r, &fr_source_render_arena[then_start]));
  FR_TRY(fr_source_puts(r, " ]"));
  if (has_else) {
    FR_TRY(fr_source_puts(r, " else [ "));
    FR_TRY(fr_source_puts(r, &fr_source_render_arena[else_start]));
    FR_TRY(fr_source_puts(r, " ]"));
  }
  return fr_source_seal(r, start, false);
}

/* A branch body must reduce to exactly one fragment; hand back its arena
 * offset and pop it so the caller can splice it into the if form. */
static fr_err_t fr_source_render_branch(fr_source_render_t *r,
                                        const fr_instruction_stream_t *view,
                                        const char *names, uint16_t names_len,
                                        fr_code_offset_t ip,
                                        fr_code_offset_t end,
                                        uint16_t *out_start) {
  uint8_t base = r->depth;

  FR_TRY(fr_source_render_span(r, view, names, names_len, ip, end));
  if (r->depth != (uint8_t)(base + 1u)) {
    return FR_ERR_UNSUPPORTED;
  }
  *out_start = r->stack[base].start;
  r->depth = base;
  return FR_OK;
}

static fr_err_t fr_source_render_if(fr_source_render_t *r,
                                    const fr_instruction_stream_t *view,
                                    const char *names, uint16_t names_len,
                                    fr_code_offset_t ip,
                                    fr_code_offset_t *out_ip) {
  fr_code_offset_t false_target = 0;
  fr_code_offset_t end_target = 0;
  fr_code_offset_t jump_ip = 0;
  fr_source_frag_t cond;
  uint16_t then_start = 0;
  uint16_t else_start = 0;
  bool has_else = true;

  /* cond JUMP_IF_FALSY(F) <then> JUMP(E) F:<else> E:  — the JUMP sits at F-3. */
  if (r->depth < 1) {
    return FR_ERR_UNSUPPORTED;
  }
  FR_TRY(fr_instruction_read_jump_operand(view, ip, &false_target));
  if (false_target < (fr_code_offset_t)(ip + 6u)) {
    return FR_ERR_UNSUPPORTED;
  }
  jump_ip = (fr_code_offset_t)(false_target - 3u);
  if ((fr_opcode_t)view->bytes[jump_ip] != FR_OP_JUMP) {
    return FR_ERR_UNSUPPORTED;
  }
  FR_TRY(fr_instruction_read_jump_operand(view, jump_ip, &end_target));
  if (end_target < false_target) {
    return FR_ERR_UNSUPPORTED;
  }

  cond = r->stack[r->depth - 1];
  r->depth = (uint8_t)(r->depth - 1u);

  FR_TRY(fr_source_render_branch(r, view, names, names_len,
                                 (fr_code_offset_t)(ip + 3u), jump_ip,
                                 &then_start));
  /* A missing else compiles to a lone PUSH_NIL; drop the clause to match. */
  if (end_target == (fr_code_offset_t)(false_target + 1u) &&
      (fr_opcode_t)view->bytes[false_target] == FR_OP_PUSH_NIL) {
    has_else = false;
  } else {
    FR_TRY(fr_source_render_branch(r, view, names, names_len, false_target,
                                   end_target, &else_start));
  }
  FR_TRY(fr_source_reduce_if(r, cond, then_start, else_start, has_else));
  *out_ip = end_target;
  return FR_OK;
}

static fr_err_t fr_source_render_span(fr_source_render_t *r,
                                      const fr_instruction_stream_t *view,
                                      const char *names, uint16_t names_len,
                                      fr_code_offset_t ip, fr_code_offset_t end) {
  while (ip < end) {
    switch ((fr_opcode_t)view->bytes[ip]) {
    case FR_OP_PUSH_NIL:
      FR_TRY(fr_source_push_text(r, "nil"));
      ip = (fr_code_offset_t)(ip + 1u);
      break;
    case FR_OP_PUSH_FALSE:
      FR_TRY(fr_source_push_text(r, "false"));
      ip = (fr_code_offset_t)(ip + 1u);
      break;
    case FR_OP_PUSH_TRUE:
      FR_TRY(fr_source_push_text(r, "true"));
      ip = (fr_code_offset_t)(ip + 1u);
      break;
    case FR_OP_PUSH_INT: {
      fr_int_t value = 0;

      FR_TRY(fr_instruction_read_int_operand(view, ip, &value));
      FR_TRY(fr_source_push_int(r, value));
      ip = (fr_code_offset_t)(ip + FR_INSTRUCTION_PUSH_INT_SIZE);
      break;
    }
    case FR_OP_LOAD_ARG: {
      uint8_t arg_index = 0;
      const char *name = NULL;

      FR_TRY(fr_instruction_read_arg_operand(view, ip, &arg_index));
      FR_TRY(fr_source_param_name_at(names, names_len, arg_index, &name));
      FR_TRY(fr_source_push_text(r, name));
      ip = (fr_code_offset_t)(ip + 2u);
      break;
    }
    case FR_OP_LOAD_SLOT: {
      fr_slot_id_t slot_id = 0;
      const char *name = NULL;

      FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
      name = fr_source_slot_name(r, slot_id);
      if (name == NULL) {
        return FR_ERR_UNSUPPORTED;
      }
      FR_TRY(fr_source_push_text(r, name));
      ip = (fr_code_offset_t)(ip + 3u);
      break;
    }
    case FR_OP_LT_INT:
      FR_TRY(fr_source_reduce_binop(r, "<"));
      ip = (fr_code_offset_t)(ip + 1u);
      break;
    case FR_OP_GT_INT:
      FR_TRY(fr_source_reduce_binop(r, ">"));
      ip = (fr_code_offset_t)(ip + 1u);
      break;
    case FR_OP_LE_INT:
      FR_TRY(fr_source_reduce_binop(r, "<="));
      ip = (fr_code_offset_t)(ip + 1u);
      break;
    case FR_OP_GE_INT:
      FR_TRY(fr_source_reduce_binop(r, ">="));
      ip = (fr_code_offset_t)(ip + 1u);
      break;
    case FR_OP_EQ_INT:
      FR_TRY(fr_source_reduce_binop(r, "="));
      ip = (fr_code_offset_t)(ip + 1u);
      break;
    case FR_OP_NE_INT:
      FR_TRY(fr_source_reduce_binop(r, "<>"));
      ip = (fr_code_offset_t)(ip + 1u);
      break;
    case FR_OP_ADD_INT:
      FR_TRY(fr_source_reduce_binop(r, "+"));
      ip = (fr_code_offset_t)(ip + 1u);
      break;
    case FR_OP_SUB_INT:
      FR_TRY(fr_source_reduce_binop(r, "-"));
      ip = (fr_code_offset_t)(ip + 1u);
      break;
    case FR_OP_MUL_INT:
      FR_TRY(fr_source_reduce_binop(r, "*"));
      ip = (fr_code_offset_t)(ip + 1u);
      break;
    case FR_OP_DIV_INT:
      FR_TRY(fr_source_reduce_binop(r, "/"));
      ip = (fr_code_offset_t)(ip + 1u);
      break;
    case FR_OP_CALL_NATIVE_SLOT: {
      fr_slot_id_t slot_id = 0;
      uint8_t arity = 0;
      const char *name = NULL;

      FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
      name = fr_source_slot_name(r, slot_id);
      if (name == NULL) {
        return FR_ERR_UNSUPPORTED;
      }
      FR_TRY(fr_source_native_arity(r, slot_id, &arity));
      FR_TRY(fr_source_reduce_call(r, name, arity));
      ip = (fr_code_offset_t)(ip + 3u);
      break;
    }
    case FR_OP_CALL_SLOT_ARG: {
      fr_slot_id_t slot_id = 0;
      uint8_t arg_count = 0;
      const char *name = NULL;

      FR_TRY(fr_instruction_read_call_slot_arg_operands(view, ip, &slot_id,
                                                        &arg_count));
      name = fr_source_slot_name(r, slot_id);
      if (name == NULL) {
        return FR_ERR_UNSUPPORTED;
      }
      FR_TRY(fr_source_reduce_call(r, name, arg_count));
      ip = (fr_code_offset_t)(ip + 4u);
      break;
    }
    case FR_OP_JUMP_IF_FALSY:
      FR_TRY(fr_source_render_if(r, view, names, names_len, ip, &ip));
      break;
    default:
      return FR_ERR_UNSUPPORTED;
    }
  }
  return FR_OK;
}

static fr_err_t fr_source_build(fr_source_render_t *r,
                                fr_code_object_id_t code_object_id,
                                fr_instruction_header_t *out_header,
                                const char **out_names,
                                uint16_t *out_names_len) {
  fr_instruction_stream_t view;

  FR_TRY(fr_code_get_instructions(r->runtime, code_object_id, &view));
  FR_TRY(fr_instruction_read_header(&view, out_header));
  FR_TRY(fr_code_get_param_names(r->runtime, code_object_id, out_names,
                                 out_names_len));
  if (out_header->arity > 0 &&
      fr_source_param_count(*out_names, *out_names_len) != out_header->arity) {
    return FR_ERR_UNSUPPORTED;
  }
  /* The body is one expression followed by a trailing RETURN. */
  if (view.length <= out_header->header_size ||
      (fr_opcode_t)view.bytes[view.length - 1u] != FR_OP_RETURN) {
    return FR_ERR_UNSUPPORTED;
  }

  FR_TRY(fr_source_render_span(r, &view, *out_names, *out_names_len,
                               out_header->header_size,
                               (fr_code_offset_t)(view.length - 1u)));
  if (r->depth != 1) {
    return FR_ERR_UNSUPPORTED;
  }
  return FR_OK;
}

fr_err_t fr_source_render_code(fr_runtime_t *runtime,
                               fr_code_object_id_t code_object_id,
                               const char *word_name,
                               fr_err_t (*write)(void *ctx, const char *text),
                               void *ctx) {
  fr_source_render_t r = {0};
  fr_instruction_header_t header;
  const char *names = NULL;
  uint16_t names_len = 0;

  if (runtime == NULL || word_name == NULL || write == NULL) {
    return FR_ERR_UNSUPPORTED;
  }
  r.runtime = runtime;

  if (fr_source_build(&r, code_object_id, &header, &names, &names_len) !=
      FR_OK) {
    return FR_ERR_UNSUPPORTED;
  }

  FR_TRY(write(ctx, "to "));
  FR_TRY(write(ctx, word_name));
  if (header.arity > 0) {
    FR_TRY(write(ctx, " with "));
    for (uint8_t i = 0; i < header.arity; i++) {
      const char *name = NULL;

      if (i > 0) {
        FR_TRY(write(ctx, ", "));
      }
      FR_TRY(fr_source_param_name_at(names, names_len, i, &name));
      FR_TRY(write(ctx, name));
    }
  }
  FR_TRY(write(ctx, " [ "));
  FR_TRY(write(ctx, &fr_source_render_arena[r.stack[0].start]));
  return write(ctx, " ]\n");
}

#endif
