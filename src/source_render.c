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
#include "object.h"
#include "slot.h"
#include "tagged.h"

#include <stdbool.h>

/* Single-shot scratch: see finishes before the next REPL line, the same
 * lifetime the apply/run wire buffers in repl.c rely on. */
static char fr_source_render_arena[FR_PROFILE_MAX_SOURCE_RENDER_BYTES];
static uint8_t fr_source_render_instruction_scratch[FR_PROFILE_TOTAL_IMAGE_BYTES];

fr_err_t fr_source_render_instruction_view(fr_runtime_t *runtime,
                                           fr_code_object_id_t code_object_id,
                                           fr_instruction_stream_t *out_view) {
  if (runtime == NULL || out_view == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_code_get_instructions(runtime, code_object_id, out_view));
  if (out_view->bytes != NULL) {
    return FR_OK;
  }
  if (out_view->length > sizeof(fr_source_render_instruction_scratch)) {
    return FR_ERR_CAPACITY;
  }
  FR_TRY(fr_code_read(runtime, code_object_id, 0,
                      fr_source_render_instruction_scratch, out_view->length));
  return fr_instruction_stream_init(out_view, fr_source_render_instruction_scratch,
                                    out_view->length);
}

typedef struct fr_source_frag_t {
  uint16_t start;     /* offset of a NUL-terminated string in the arena */
  bool parenthesize;  /* a binop result, wrapped when it feeds another binop */
  bool has_call;      /* an unfenced colon call; a reparse would let an
                       * operator or comma fall into its arguments, so
                       * operand and argument positions wrap it in parens */
} fr_source_frag_t;

typedef struct fr_source_render_t {
  fr_runtime_t *runtime;
  uint16_t used;
  uint8_t depth;
  fr_source_frag_t stack[FR_PROFILE_MAX_STACK_DEPTH + 1];
} fr_source_render_t;

typedef struct fr_source_name_t {
  const char *bytes;
  uint8_t length;
} fr_source_name_t;

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

static fr_err_t fr_source_put_hex_byte(fr_source_render_t *r, uint8_t byte) {
  static const char digits[] = "0123456789abcdef";

  FR_TRY(fr_source_puts(r, "\\x"));
  FR_TRY(fr_source_putc(r, digits[byte >> 4]));
  return fr_source_putc(r, digits[byte & 0x0fu]);
}

static fr_err_t fr_source_put_quoted_text(fr_source_render_t *r,
                                          const uint8_t *bytes,
                                          uint16_t length) {
  FR_TRY(fr_source_putc(r, '"'));
  for (uint16_t i = 0; i < length; i++) {
    uint8_t byte = bytes[i];

    if (byte == '"' || byte == '\\') {
      FR_TRY(fr_source_putc(r, '\\'));
      FR_TRY(fr_source_putc(r, (char)byte));
    } else if (byte == '\n') {
      FR_TRY(fr_source_puts(r, "\\n"));
    } else if (byte == '\r') {
      FR_TRY(fr_source_puts(r, "\\r"));
    } else if (byte == '\t') {
      FR_TRY(fr_source_puts(r, "\\t"));
    } else if (byte >= 0x20u && byte <= 0x7eu) {
      FR_TRY(fr_source_putc(r, (char)byte));
    } else {
      FR_TRY(fr_source_put_hex_byte(r, byte));
    }
  }
  return fr_source_putc(r, '"');
}

/* Step past the trailing NUL, then record the fragment on the stack. */
static fr_err_t fr_source_seal_frag(fr_source_render_t *r, uint16_t start,
                                    bool parenthesize, bool has_call) {
  if (r->depth >= (uint8_t)(sizeof(r->stack) / sizeof(r->stack[0]))) {
    return FR_ERR_CAPACITY;
  }
  if ((uint32_t)r->used + 1u > sizeof(fr_source_render_arena)) {
    return FR_ERR_CAPACITY;
  }
  r->used = (uint16_t)(r->used + 1u);
  r->stack[r->depth].start = start;
  r->stack[r->depth].parenthesize = parenthesize;
  r->stack[r->depth].has_call = has_call;
  r->depth = (uint8_t)(r->depth + 1u);
  return FR_OK;
}

static fr_err_t fr_source_seal(fr_source_render_t *r, uint16_t start,
                               bool parenthesize) {
  return fr_source_seal_frag(r, start, parenthesize, false);
}

static fr_err_t fr_source_put_name(fr_source_render_t *r,
                                   fr_source_name_t name) {
  if (name.bytes == NULL || name.length == 0) {
    return FR_ERR_UNSUPPORTED;
  }
  for (uint8_t i = 0; i < name.length; i++) {
    FR_TRY(fr_source_putc(r, name.bytes[i]));
  }
  return FR_OK;
}

static fr_err_t fr_source_push_text(fr_source_render_t *r, const char *text) {
  uint16_t start = r->used;

  FR_TRY(fr_source_puts(r, text));
  return fr_source_seal(r, start, false);
}

static fr_err_t fr_source_push_name(fr_source_render_t *r,
                                    fr_source_name_t name) {
  uint16_t start = r->used;

  FR_TRY(fr_source_put_name(r, name));
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
  bool rhs_splits = false;
  bool fence_lhs = false;
  bool fence_rhs = false;

  if (r->depth < 2) {
    return FR_ERR_UNSUPPORTED;
  }
  rhs = r->stack[r->depth - 1];
  lhs = r->stack[r->depth - 2];
  r->depth = (uint8_t)(r->depth - 2);

  /* When the right side begins with a call, a reparse in argument position
   * splits at this operator, which is exactly what an unfenced call on the
   * left needs. Otherwise a left-side call would swallow the operator into
   * its arguments and has to be fenced. */
  rhs_splits = rhs.has_call && !rhs.parenthesize;
  fence_lhs = lhs.parenthesize || (lhs.has_call && !rhs_splits);
  fence_rhs = rhs.parenthesize;

  FR_TRY(fr_source_emit_operand(
      r, (fr_source_frag_t){.start = lhs.start, .parenthesize = fence_lhs}));
  FR_TRY(fr_source_putc(r, ' '));
  FR_TRY(fr_source_puts(r, symbol));
  FR_TRY(fr_source_putc(r, ' '));
  FR_TRY(fr_source_emit_operand(
      r, (fr_source_frag_t){.start = rhs.start, .parenthesize = fence_rhs}));
  return fr_source_seal_frag(r, start, true, rhs_splits);
}

static fr_err_t fr_source_reduce_call(fr_source_render_t *r,
                                      fr_source_name_t name,
                                      uint8_t arg_count) {
  uint16_t start = r->used;
  uint8_t base = 0;

  /* A zero-arg call reads as a bare name, indistinguishable from a load, so
   * leave it to the fallback rather than emit ambiguous source. */
  if (arg_count == 0 || r->depth < arg_count) {
    return FR_ERR_UNSUPPORTED;
  }
  base = (uint8_t)(r->depth - arg_count);

  FR_TRY(fr_source_put_name(r, name));
  FR_TRY(fr_source_puts(r, ": "));
  for (uint8_t i = 0; i < arg_count; i++) {
    fr_source_frag_t arg = r->stack[base + i];
    /* An argument with an unfenced call inside is wrapped in parens: a
     * trailing comma or operator would otherwise fall into the inner
     * call's arguments on reparse. A plain call as the last argument is
     * the one safe raw spelling (`f: g: b`). */
    bool fence = arg.has_call && (i + 1u < arg_count || arg.parenthesize);

    if (i > 0) {
      FR_TRY(fr_source_puts(r, ", "));
    }
    FR_TRY(fr_source_emit_operand(
        r, (fr_source_frag_t){.start = arg.start, .parenthesize = fence}));
  }
  r->depth = base;
  return fr_source_seal_frag(r, start, false, true);
}

static fr_err_t fr_source_write_slot_write(fr_source_render_t *r,
                                           fr_source_name_t name,
                                           fr_source_frag_t value) {
  FR_TRY(fr_source_puts(r, "set "));
  FR_TRY(fr_source_put_name(r, name));
  FR_TRY(fr_source_puts(r, " to "));
  return fr_source_puts(r, &fr_source_render_arena[value.start]);
}

#if FR_FEATURE_CELLS
static fr_err_t fr_source_write_cell_read(fr_source_render_t *r,
                                          fr_source_name_t name,
                                          fr_source_frag_t index) {
  FR_TRY(fr_source_put_name(r, name));
  FR_TRY(fr_source_putc(r, '['));
  FR_TRY(fr_source_puts(r, &fr_source_render_arena[index.start]));
  return fr_source_putc(r, ']');
}

static fr_err_t fr_source_write_cell_write(fr_source_render_t *r,
                                           fr_source_name_t name,
                                           fr_source_frag_t index,
                                           fr_source_frag_t value) {
  FR_TRY(fr_source_puts(r, "set "));
  FR_TRY(fr_source_write_cell_read(r, name, index));
  FR_TRY(fr_source_puts(r, " to "));
  return fr_source_puts(r, &fr_source_render_arena[value.start]);
}
#endif

static fr_err_t fr_source_slot_name(fr_source_render_t *r,
                                    fr_slot_id_t slot_id,
                                    fr_source_name_t *out_name) {
  if (r == NULL || out_name == NULL) {
    return FR_ERR_INVALID;
  }
  return fr_slot_name_view(r->runtime, slot_id, &out_name->bytes,
                           &out_name->length);
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

/* Restored code installs with no stored parameter names (persist payload
 * stays param-name-less). Render canonical `argN` into the caller buffer
 * when that case shows up; the VM never sees these names. canon[7] holds
 * "arg" + up to three decimal digits (index is uint8_t) + NUL. */
static fr_err_t fr_source_param_name_at(const char *names, uint16_t names_len,
                                        uint8_t index, char canon[7],
                                        const char **out_name) {
  uint16_t pos = 0;

  if (names_len == 0) {
    char digits[3];
    uint8_t digit_count = 0;
    uint8_t magnitude = index;
    uint8_t out_pos = 0;

    do {
      digits[digit_count] = (char)('0' + (magnitude % 10u));
      digit_count = (uint8_t)(digit_count + 1u);
      magnitude /= 10u;
    } while (magnitude > 0);
    canon[out_pos++] = 'a';
    canon[out_pos++] = 'r';
    canon[out_pos++] = 'g';
    while (digit_count > 0) {
      digit_count = (uint8_t)(digit_count - 1u);
      canon[out_pos++] = digits[digit_count];
    }
    canon[out_pos] = '\0';
    *out_name = canon;
    return FR_OK;
  }

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

static void fr_source_local_name(uint8_t index, char canon[9]) {
  char digits[3];
  uint8_t digit_count = 0;
  uint8_t magnitude = index;
  uint8_t out_pos = 0;

  do {
    digits[digit_count] = (char)('0' + (magnitude % 10u));
    digit_count = (uint8_t)(digit_count + 1u);
    magnitude /= 10u;
  } while (magnitude > 0);

  canon[out_pos++] = 'l';
  canon[out_pos++] = 'o';
  canon[out_pos++] = 'c';
  canon[out_pos++] = 'a';
  canon[out_pos++] = 'l';
  while (digit_count > 0) {
    digit_count = (uint8_t)(digit_count - 1u);
    canon[out_pos++] = digits[digit_count];
  }
  canon[out_pos] = '\0';
}

static fr_err_t fr_source_render_span(fr_source_render_t *r,
                                      const fr_instruction_stream_t *view,
                                      const char *names, uint16_t names_len,
                                      fr_code_offset_t ip, fr_code_offset_t end);

/* A while closes its body with a JUMP back to the condition; if/else jumps
 * forward past the else. The JUMP sits at false_target-3 for both shapes. */
static bool fr_source_is_while(const fr_instruction_stream_t *view,
                               fr_code_offset_t ip,
                               fr_code_offset_t false_target) {
  fr_code_offset_t jump_ip = 0;
  fr_code_offset_t back_target = 0;

  if (false_target < (fr_code_offset_t)(ip + 6u)) {
    return false;
  }
  jump_ip = (fr_code_offset_t)(false_target - 3u);
  if ((fr_opcode_t)view->bytes[jump_ip] != FR_OP_JUMP) {
    return false;
  }
  if (fr_instruction_read_jump_operand(view, jump_ip, &back_target) != FR_OK) {
    return false;
  }
  return back_target <= ip;
}

static fr_err_t fr_source_reduce_while(fr_source_render_t *r,
                                       fr_source_frag_t cond,
                                       uint16_t body_start) {
  uint16_t start = r->used;

  FR_TRY(fr_source_puts(r, "while "));
  FR_TRY(fr_source_puts(r, &fr_source_render_arena[cond.start]));
  FR_TRY(fr_source_puts(r, " [ "));
  FR_TRY(fr_source_puts(r, &fr_source_render_arena[body_start]));
  FR_TRY(fr_source_puts(r, " ]"));
  return fr_source_seal(r, start, false);
}

static fr_err_t fr_source_reduce_repeat(fr_source_render_t *r,
                                        fr_source_frag_t count,
                                        const char *index_name,
                                        uint16_t body_start) {
  uint16_t start = r->used;

  FR_TRY(fr_source_puts(r, "repeat "));
  FR_TRY(fr_source_puts(r, &fr_source_render_arena[count.start]));
  if (index_name != NULL) {
    FR_TRY(fr_source_puts(r, " as "));
    FR_TRY(fr_source_puts(r, index_name));
  }
  FR_TRY(fr_source_puts(r, " [ "));
  FR_TRY(fr_source_puts(r, &fr_source_render_arena[body_start]));
  FR_TRY(fr_source_puts(r, " ]"));
  return fr_source_seal(r, start, false);
}

/* A multi-statement body leaves one fragment per `;`-separated statement on
 * the stack; splice them back into `a ; b ; c` source form. */
static fr_err_t fr_source_join_statements(fr_source_render_t *r, uint8_t base) {
  uint16_t start = r->used;

  for (uint8_t i = base; i < r->depth; i++) {
    if (i > base) {
      FR_TRY(fr_source_puts(r, " ; "));
    }
    FR_TRY(fr_source_puts(r, &fr_source_render_arena[r->stack[i].start]));
  }
  r->depth = base;
  return fr_source_seal(r, start, false);
}

/* When the else branch is a single if-expression, render the chained `else if`
 * spelling instead of wrapping it in another bracket pair. Only a top-level
 * `; ` (outside every `[ ]`) means the else body holds multiple statements; a
 * `; ` inside an arm's brackets belongs to that arm and shouldn't drop us out
 * of the chained form. */
static bool fr_source_else_chains(const char *text) {
  uint8_t depth = 0;

  if (text[0] != 'i' || text[1] != 'f' || text[2] != ' ') {
    return false;
  }
  for (const char *p = text + 3; *p; p++) {
    if (*p == '[') {
      depth = (uint8_t)(depth + 1u);
    } else if (*p == ']') {
      if (depth > 0) {
        depth = (uint8_t)(depth - 1u);
      }
    } else if (depth == 0 && p[0] == ' ' && p[1] == ';' && p[2] == ' ') {
      return false;
    }
  }
  return true;
}

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
    const char *else_text = &fr_source_render_arena[else_start];
    if (fr_source_else_chains(else_text)) {
      FR_TRY(fr_source_puts(r, " else "));
      FR_TRY(fr_source_puts(r, else_text));
    } else {
      FR_TRY(fr_source_puts(r, " else [ "));
      FR_TRY(fr_source_puts(r, else_text));
      FR_TRY(fr_source_puts(r, " ]"));
    }
  }
  return fr_source_seal(r, start, false);
}

static fr_err_t fr_source_reduce_attempt(fr_source_render_t *r,
                                         uint16_t body_start,
                                         uint16_t fallback_start) {
  uint16_t start = r->used;

  FR_TRY(fr_source_puts(r, "attempt [ "));
  FR_TRY(fr_source_puts(r, &fr_source_render_arena[body_start]));
  FR_TRY(fr_source_puts(r, " ] rescue [ "));
  FR_TRY(fr_source_puts(r, &fr_source_render_arena[fallback_start]));
  FR_TRY(fr_source_puts(r, " ]"));
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

/* When bytes scratch is enabled the compiler emits one FR_OP_BYTES_RESET byte
 * at each loop back-edge, between the body's trailing DROP and the
 * REPEAT_NEXT/JUMP (src/compile.c). */
#if FR_FEATURE_BYTES
#define FR_SOURCE_LOOP_RESET 1u
#else
#define FR_SOURCE_LOOP_RESET 0u
#endif

/* cond JUMP_IF_FALSY(done) <body> DROP [BYTES_RESET] JUMP(cond) done:PUSH_NIL —
 * the loop's value is that nil, so skip it and let the while text stand in its
 * place. */
static fr_err_t fr_source_render_while(fr_source_render_t *r,
                                       const fr_instruction_stream_t *view,
                                       const char *names, uint16_t names_len,
                                       fr_code_offset_t ip,
                                       fr_code_offset_t false_target,
                                       fr_code_offset_t *out_ip) {
  fr_code_offset_t drop_ip =
      (fr_code_offset_t)(false_target - 4u - FR_SOURCE_LOOP_RESET);
  fr_source_frag_t cond;
  uint16_t body_start = 0;

  if (r->depth < 1) {
    return FR_ERR_UNSUPPORTED;
  }
  if ((fr_opcode_t)view->bytes[drop_ip] != FR_OP_DROP ||
      (fr_opcode_t)view->bytes[false_target] != FR_OP_PUSH_NIL) {
    return FR_ERR_UNSUPPORTED;
  }
  cond = r->stack[r->depth - 1];
  r->depth = (uint8_t)(r->depth - 1u);

  FR_TRY(fr_source_render_branch(r, view, names, names_len,
                                 (fr_code_offset_t)(ip + 3u), drop_ip,
                                 &body_start));
  FR_TRY(fr_source_reduce_while(r, cond, body_start));
  *out_ip = (fr_code_offset_t)(false_target + 1u);
  return FR_OK;
}

/* count REPEAT_BEGIN(done) <body> DROP [BYTES_RESET] REPEAT_NEXT(body)
 * done:PUSH_NIL — same shape as while, with the count already on the stack as
 * the operand. */
static fr_err_t fr_source_render_repeat(fr_source_render_t *r,
                                        const fr_instruction_stream_t *view,
                                        const char *names, uint16_t names_len,
                                        fr_code_offset_t ip,
                                        fr_code_offset_t *out_ip) {
  fr_code_offset_t done_target = 0;
  fr_code_offset_t drop_ip = 0;
  fr_code_offset_t next_ip = 0;
  fr_code_offset_t next_target = 0;
  fr_code_offset_t body_ip = 0;
  fr_source_frag_t count;
  uint16_t body_start = 0;
  uint8_t index_local = 0;
  uint8_t next_index_local = 0;
  uint8_t begin_width = 3;
  uint8_t next_width = 3;
  char index_name[9];
  bool has_index = (fr_opcode_t)view->bytes[ip] == FR_OP_REPEAT_BEGIN_AS;

  if (r->depth < 1) {
    return FR_ERR_UNSUPPORTED;
  }
  if (has_index) {
    begin_width = 4;
    next_width = 4;
    FR_TRY(fr_instruction_read_jump_local_operands(view, ip, &done_target,
                                                   &index_local));
    fr_source_local_name(index_local, index_name);
  } else {
    index_name[0] = '\0';
    FR_TRY(fr_instruction_read_jump_operand(view, ip, &done_target));
  }
  body_ip = (fr_code_offset_t)(ip + begin_width);
  if (done_target <
      (fr_code_offset_t)(ip + begin_width + next_width + 2u)) {
    return FR_ERR_UNSUPPORTED;
  }
  drop_ip = (fr_code_offset_t)(done_target - next_width - 1u -
                               FR_SOURCE_LOOP_RESET);
  next_ip = (fr_code_offset_t)(done_target - next_width);
  if ((fr_opcode_t)view->bytes[drop_ip] != FR_OP_DROP ||
      (fr_opcode_t)view->bytes[done_target] != FR_OP_PUSH_NIL) {
    return FR_ERR_UNSUPPORTED;
  }
  /* REPEAT_NEXT must loop back to the body start, else this is some other
   * shape that happens to share the tail opcodes. */
  if (has_index) {
    if ((fr_opcode_t)view->bytes[next_ip] != FR_OP_REPEAT_NEXT_AS) {
      return FR_ERR_UNSUPPORTED;
    }
    FR_TRY(fr_instruction_read_jump_local_operands(view, next_ip, &next_target,
                                                   &next_index_local));
    if (next_index_local != index_local) {
      return FR_ERR_UNSUPPORTED;
    }
  } else {
    if ((fr_opcode_t)view->bytes[next_ip] != FR_OP_REPEAT_NEXT) {
      return FR_ERR_UNSUPPORTED;
    }
    FR_TRY(fr_instruction_read_jump_operand(view, next_ip, &next_target));
  }
  if (next_target != body_ip) {
    return FR_ERR_UNSUPPORTED;
  }
  count = r->stack[r->depth - 1];
  r->depth = (uint8_t)(r->depth - 1u);

  FR_TRY(fr_source_render_branch(r, view, names, names_len, body_ip, drop_ip,
                                 &body_start));
  FR_TRY(fr_source_reduce_repeat(r, count, has_index ? index_name : NULL,
                                 body_start));
  *out_ip = (fr_code_offset_t)(done_target + 1u);
  return FR_OK;
}

static fr_err_t fr_source_render_attempt(fr_source_render_t *r,
                                         const fr_instruction_stream_t *view,
                                         const char *names, uint16_t names_len,
                                         fr_code_offset_t ip,
                                         fr_code_offset_t end,
                                         fr_code_offset_t *out_ip) {
  fr_code_offset_t fallback_target = 0;
  fr_code_offset_t attempt_end_ip = 0;
  fr_code_offset_t end_target = 0;
  uint16_t body_start = 0;
  uint16_t fallback_start = 0;

  FR_TRY(fr_instruction_read_jump_operand(view, ip, &fallback_target));
  if (fallback_target < (fr_code_offset_t)(ip + 6u) ||
      fallback_target > end) {
    return FR_ERR_UNSUPPORTED;
  }
  attempt_end_ip = (fr_code_offset_t)(fallback_target - 3u);
  if ((fr_opcode_t)view->bytes[attempt_end_ip] != FR_OP_ATTEMPT_END) {
    return FR_ERR_UNSUPPORTED;
  }
  FR_TRY(fr_instruction_read_jump_operand(view, attempt_end_ip, &end_target));
  if (end_target < fallback_target || end_target > end) {
    return FR_ERR_UNSUPPORTED;
  }

  FR_TRY(fr_source_render_branch(r, view, names, names_len,
                                 (fr_code_offset_t)(ip + 3u), attempt_end_ip,
                                 &body_start));
  FR_TRY(fr_source_render_branch(r, view, names, names_len, fallback_target,
                                 end_target, &fallback_start));
  FR_TRY(fr_source_reduce_attempt(r, body_start, fallback_start));
  *out_ip = end_target;
  return FR_OK;
}

static fr_err_t fr_source_render_span(fr_source_render_t *r,
                                      const fr_instruction_stream_t *view,
                                      const char *names, uint16_t names_len,
                                      fr_code_offset_t ip, fr_code_offset_t end) {
  uint8_t base = r->depth;

  while (ip < end) {
    switch ((fr_opcode_t)view->bytes[ip]) {
    case FR_OP_DROP:
      /* Statement separator: the finished statement stays on the stack for
       * the join below. Control-flow constructs eat their own internal DROPs,
       * so any DROP reaching here sits between two body statements. */
      ip = (fr_code_offset_t)(ip + 1u);
      break;
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
    case FR_OP_ERROR_CODE:
      FR_TRY(fr_source_push_text(r, "error.code"));
      ip = (fr_code_offset_t)(ip + 1u);
      break;
    case FR_OP_ERROR_NAME:
      FR_TRY(fr_source_push_text(r, "error.name"));
      ip = (fr_code_offset_t)(ip + 1u);
      break;
    case FR_OP_PUSH_INT: {
      fr_int_t value = 0;

      FR_TRY(fr_instruction_read_int_operand(view, ip, &value));
      FR_TRY(fr_source_push_int(r, value));
      ip = (fr_code_offset_t)(ip + FR_INSTRUCTION_PUSH_INT_SIZE);
      break;
    }
    case FR_OP_PUSH_OBJECT_ID: {
      fr_object_id_t object_id = 0;
      const uint8_t *bytes = NULL;
      uint16_t length = 0;
      uint16_t start = r->used;

      FR_TRY(fr_instruction_read_object_id_operand(view, ip, &object_id));
      FR_TRY(fr_text_view(r->runtime, object_id, &bytes, &length));
      FR_TRY(fr_source_put_quoted_text(r, bytes, length));
      FR_TRY(fr_source_seal(r, start, false));
      ip = (fr_code_offset_t)(ip + FR_INSTRUCTION_PUSH_OBJECT_ID_SIZE);
      break;
    }
    case FR_OP_LOAD_ARG: {
      uint8_t arg_index = 0;
      char canon[7];
      const char *name = NULL;

      FR_TRY(fr_instruction_read_arg_operand(view, ip, &arg_index));
      FR_TRY(
          fr_source_param_name_at(names, names_len, arg_index, canon, &name));
      FR_TRY(fr_source_push_text(r, name));
      ip = (fr_code_offset_t)(ip + 2u);
      break;
    }
    case FR_OP_LOAD_SLOT: {
      fr_slot_id_t slot_id = 0;
      fr_source_name_t name = {0};

      FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
      FR_TRY(fr_source_slot_name(r, slot_id, &name));
      FR_TRY(fr_source_push_name(r, name));
      ip = (fr_code_offset_t)(ip + 3u);
      break;
    }
    case FR_OP_STORE_SLOT: {
      fr_slot_id_t slot_id = 0;
      fr_source_frag_t value;
      fr_source_name_t name = {0};
      uint16_t start = r->used;

      FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
      FR_TRY(fr_source_slot_name(r, slot_id, &name));
      if (r->depth < 1) {
        return FR_ERR_UNSUPPORTED;
      }
      value = r->stack[r->depth - 1];
      r->depth = (uint8_t)(r->depth - 1u);
      FR_TRY(fr_source_write_slot_write(r, name, value));
      FR_TRY(fr_source_seal(r, start, false));
      ip = (fr_code_offset_t)(ip + 3u);
      break;
    }
#if FR_FEATURE_CELLS
    case FR_OP_LOAD_CELL: {
      fr_slot_id_t slot_id = 0;
      uint16_t index_value = 0;
      fr_source_frag_t index;
      fr_source_name_t name = {0};
      uint16_t start = 0;

      FR_TRY(fr_instruction_read_cell_operands(view, ip, &slot_id,
                                               &index_value));
      FR_TRY(fr_source_slot_name(r, slot_id, &name));
      FR_TRY(fr_source_push_int(r, (fr_int_t)index_value));
      index = r->stack[r->depth - 1];
      r->depth = (uint8_t)(r->depth - 1u);
      start = r->used;
      FR_TRY(fr_source_write_cell_read(r, name, index));
      FR_TRY(fr_source_seal(r, start, false));
      ip = (fr_code_offset_t)(ip + 5u);
      break;
    }
    case FR_OP_STORE_CELL: {
      fr_slot_id_t slot_id = 0;
      uint16_t index_value = 0;
      fr_source_frag_t index;
      fr_source_frag_t value;
      fr_source_name_t name = {0};
      uint16_t start = 0;

      FR_TRY(fr_instruction_read_cell_operands(view, ip, &slot_id,
                                               &index_value));
      FR_TRY(fr_source_slot_name(r, slot_id, &name));
      if (r->depth < 1) {
        return FR_ERR_UNSUPPORTED;
      }
      value = r->stack[r->depth - 1];
      r->depth = (uint8_t)(r->depth - 1u);
      FR_TRY(fr_source_push_int(r, (fr_int_t)index_value));
      index = r->stack[r->depth - 1];
      r->depth = (uint8_t)(r->depth - 1u);
      start = r->used;
      FR_TRY(fr_source_write_cell_write(r, name, index, value));
      FR_TRY(fr_source_seal(r, start, false));
      ip = (fr_code_offset_t)(ip + 5u);
      break;
    }
    case FR_OP_LOAD_CELL_DYNAMIC: {
      fr_slot_id_t slot_id = 0;
      fr_source_frag_t index;
      fr_source_name_t name = {0};
      uint16_t start = r->used;

      FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
      FR_TRY(fr_source_slot_name(r, slot_id, &name));
      if (r->depth < 1) {
        return FR_ERR_UNSUPPORTED;
      }
      index = r->stack[r->depth - 1];
      r->depth = (uint8_t)(r->depth - 1u);
      FR_TRY(fr_source_write_cell_read(r, name, index));
      FR_TRY(fr_source_seal(r, start, false));
      ip = (fr_code_offset_t)(ip + 3u);
      break;
    }
    case FR_OP_STORE_CELL_DYNAMIC: {
      fr_slot_id_t slot_id = 0;
      fr_source_frag_t index;
      fr_source_frag_t value;
      fr_source_name_t name = {0};
      uint16_t start = r->used;

      FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
      FR_TRY(fr_source_slot_name(r, slot_id, &name));
      if (r->depth < 2) {
        return FR_ERR_UNSUPPORTED;
      }
      index = r->stack[r->depth - 1];
      value = r->stack[r->depth - 2];
      r->depth = (uint8_t)(r->depth - 2u);
      FR_TRY(fr_source_write_cell_write(r, name, index, value));
      FR_TRY(fr_source_seal(r, start, false));
      ip = (fr_code_offset_t)(ip + 3u);
      break;
    }
#endif
    case FR_OP_LOAD_LOCAL: {
      uint8_t local_index = 0;
      char canon[9];

      FR_TRY(fr_instruction_read_local_operand(view, ip, &local_index));
      fr_source_local_name(local_index, canon);
      FR_TRY(fr_source_push_text(r, canon));
      ip = (fr_code_offset_t)(ip + 2u);
      break;
    }
    case FR_OP_STORE_LOCAL: {
      uint8_t local_index = 0;
      fr_source_frag_t value;
      uint16_t start = r->used;
      char canon[9];

      FR_TRY(fr_instruction_read_local_operand(view, ip, &local_index));
      if (r->depth < 1) {
        return FR_ERR_UNSUPPORTED;
      }
      value = r->stack[r->depth - 1];
      r->depth = (uint8_t)(r->depth - 1);

      fr_source_local_name(local_index, canon);
      FR_TRY(fr_source_puts(r, canon));
      FR_TRY(fr_source_puts(r, " is "));
      FR_TRY(fr_source_puts(r, &fr_source_render_arena[value.start]));
      FR_TRY(fr_source_seal(r, start, false));
      ip = (fr_code_offset_t)(ip + 2u);
      break;
    }
    case FR_OP_SET_LOCAL: {
      uint8_t local_index = 0;
      fr_source_frag_t value;
      uint16_t start = r->used;
      char canon[9];

      FR_TRY(fr_instruction_read_local_operand(view, ip, &local_index));
      if (r->depth < 1) {
        return FR_ERR_UNSUPPORTED;
      }
      value = r->stack[r->depth - 1];
      r->depth = (uint8_t)(r->depth - 1);

      fr_source_local_name(local_index, canon);
      FR_TRY(fr_source_puts(r, "set "));
      FR_TRY(fr_source_puts(r, canon));
      FR_TRY(fr_source_puts(r, " to "));
      FR_TRY(fr_source_puts(r, &fr_source_render_arena[value.start]));
      FR_TRY(fr_source_seal(r, start, false));
      ip = (fr_code_offset_t)(ip + 2u);
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
    case FR_OP_CALL_SLOT: {
      fr_slot_id_t slot_id = 0;
      fr_source_name_t name = {0};
      uint16_t start = r->used;

      /* A zero-arg call to a source-defined word: the opcode (not LOAD_SLOT)
       * marks it a call, so the trailing colon is unambiguous. T7b renders
       * only the tail call-then-return shape; any other zero-arg CALL_SLOT
       * position falls back. */
      if ((fr_code_offset_t)(ip + 3u) != end) {
        return FR_ERR_UNSUPPORTED;
      }
      FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
      FR_TRY(fr_source_slot_name(r, slot_id, &name));
      FR_TRY(fr_source_put_name(r, name));
      FR_TRY(fr_source_putc(r, ':'));
      FR_TRY(fr_source_seal(r, start, false));
      ip = (fr_code_offset_t)(ip + 3u);
      break;
    }
    case FR_OP_CALL_NATIVE_SLOT: {
      fr_slot_id_t slot_id = 0;
      uint8_t arity = 0;
      fr_source_name_t name = {0};

      FR_TRY(fr_instruction_read_slot_operand(view, ip, &slot_id));
      FR_TRY(fr_source_slot_name(r, slot_id, &name));
      FR_TRY(fr_source_native_arity(r, slot_id, &arity));
#if FR_FEATURE_MATH
      if (slot_id == FR_SLOT_MOD && arity == 2) {
        FR_TRY(fr_source_reduce_binop(r, "%"));
      } else
#endif
      {
        FR_TRY(fr_source_reduce_call(r, name, arity));
      }
      ip = (fr_code_offset_t)(ip + 3u);
      break;
    }
    case FR_OP_CALL_SLOT_ARG: {
      fr_slot_id_t slot_id = 0;
      uint8_t arg_count = 0;
      fr_source_name_t name = {0};

      FR_TRY(fr_instruction_read_call_slot_arg_operands(view, ip, &slot_id,
                                                        &arg_count));
      FR_TRY(fr_source_slot_name(r, slot_id, &name));
      FR_TRY(fr_source_reduce_call(r, name, arg_count));
      ip = (fr_code_offset_t)(ip + 4u);
      break;
    }
    case FR_OP_JUMP_IF_FALSY: {
      fr_code_offset_t false_target = 0;

      FR_TRY(fr_instruction_read_jump_operand(view, ip, &false_target));
      if (fr_source_is_while(view, ip, false_target)) {
        FR_TRY(fr_source_render_while(r, view, names, names_len, ip,
                                      false_target, &ip));
      } else {
        FR_TRY(fr_source_render_if(r, view, names, names_len, ip, &ip));
      }
      break;
    }
    case FR_OP_REPEAT_BEGIN:
    case FR_OP_REPEAT_BEGIN_AS:
      FR_TRY(fr_source_render_repeat(r, view, names, names_len, ip, &ip));
      break;
    case FR_OP_ATTEMPT_BEGIN:
      FR_TRY(fr_source_render_attempt(r, view, names, names_len, ip, end, &ip));
      break;
    default:
      return FR_ERR_UNSUPPORTED;
    }
  }
  if (r->depth > (uint8_t)(base + 1u)) {
    FR_TRY(fr_source_join_statements(r, base));
  }
  return FR_OK;
}

static fr_err_t fr_source_build(fr_source_render_t *r,
                                fr_code_object_id_t code_object_id,
                                fr_instruction_header_t *out_header,
                                const char **out_names,
                                uint16_t *out_names_len) {
  fr_instruction_stream_t view;

  FR_TRY(fr_source_render_instruction_view(r->runtime, code_object_id, &view));
  FR_TRY(fr_instruction_read_header(&view, out_header));
  FR_TRY(fr_code_get_param_names(r->runtime, code_object_id, out_names,
                                 out_names_len));
  if (out_header->arity > 0 && *out_names_len > 0 &&
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
      char canon[7];
      const char *name = NULL;

      if (i > 0) {
        FR_TRY(write(ctx, ", "));
      }
      FR_TRY(fr_source_param_name_at(names, names_len, i, canon, &name));
      FR_TRY(write(ctx, name));
    }
  }
  FR_TRY(write(ctx, " [ "));
  FR_TRY(write(ctx, &fr_source_render_arena[r.stack[0].start]));
  return write(ctx, " ]\n");
}

#endif
