#include "compile.h"

#include "base_defs.h"
#include "code.h"
#include "instruction.h"
#include "native.h"
#include "object.h"
#include "parse.h"
#include "platform.h"
#include "slot.h"

#include <stdint.h>
#include <string.h>

/* Function bodies stash each text literal here so apply can install the bytes
 * and patch the PUSH_OBJECT_ID operands before code install. NULL means the
 * caller is a bare expression that installs its own text now. */
typedef struct fr_compile_body_texts_t {
  fr_image_text_object_t *objects;
  uint8_t *storage;
  uint16_t *offsets;
  uint16_t object_capacity;
  uint16_t storage_capacity;
  uint16_t count;
  uint16_t used;
} fr_compile_body_texts_t;

typedef struct fr_compile_local_t {
  fr_parse_span_t name;
  uint8_t index;
} fr_compile_local_t;

typedef struct fr_compile_locals_t {
  fr_compile_local_t entries[FR_PARSE_MAX_LOCALS];
  uint8_t count;
  uint8_t next_index;
} fr_compile_locals_t;

/* An `on`/`every`/`after` statement stashes its body here as a separate code
 * object. The outer function's bytecode references it through PUSH_CODE_ID.
 * One body per overlay update for now; a second event form returns
 * FR_ERR_CAPACITY. */
typedef struct fr_compile_event_body_t {
  fr_image_code_object_t *object;
  uint8_t *bytes;
  bool used;
} fr_compile_event_body_t;

typedef struct fr_compile_context_t {
  fr_runtime_t *runtime;
  fr_diagnostic_t *diag;
  const fr_parse_span_t *params;
  uint8_t param_count;
  const fr_parse_span_t *event_outer_params;
  uint8_t event_outer_param_count;
  const fr_compile_locals_t *event_outer_locals;
  fr_slot_id_t definition_slot_id;
  uint8_t definition_arity;
  bool has_definition;
  fr_compile_body_texts_t *body_texts;
  fr_compile_locals_t *locals;
  fr_compile_event_body_t *event_body;
} fr_compile_context_t;

typedef enum fr_compile_source_feature_t {
  FR_COMPILE_SOURCE_CONTROL_FLOW,
  FR_COMPILE_SOURCE_CELLS,
  FR_COMPILE_SOURCE_TEXT,
  FR_COMPILE_SOURCE_RECORDS,
  FR_COMPILE_SOURCE_EVENTS,
} fr_compile_source_feature_t;

/* ponytail: one compile at a time; add per-task workspaces if background
 * compiles ever run concurrently. */
static fr_compile_overlay_update_t fr_compile_shared_overlay_workspace;
static bool fr_compile_shared_overlay_workspace_in_use;

fr_compile_overlay_update_t *fr_compile_overlay_workspace_acquire(void) {
  if (fr_compile_shared_overlay_workspace_in_use) {
    return NULL;
  }
  fr_compile_shared_overlay_workspace_in_use = true;
  return &fr_compile_shared_overlay_workspace;
}

void fr_compile_overlay_workspace_release(
    fr_compile_overlay_update_t *workspace) {
  if (workspace == &fr_compile_shared_overlay_workspace) {
    fr_compile_shared_overlay_workspace_in_use = false;
  }
}

static uint16_t
fr_compile_source_feature_message(fr_compile_source_feature_t feature) {
  switch (feature) {
  case FR_COMPILE_SOURCE_CONTROL_FLOW:
    return FR_DIAG_MSG_COMPILE_CONTROL_FLOW_DISABLED;
  case FR_COMPILE_SOURCE_CELLS:
    return FR_DIAG_MSG_COMPILE_CELLS_DISABLED;
  case FR_COMPILE_SOURCE_TEXT:
    return FR_DIAG_MSG_COMPILE_TEXT_DISABLED;
  case FR_COMPILE_SOURCE_RECORDS:
    return FR_DIAG_MSG_COMPILE_RECORDS_DISABLED;
  case FR_COMPILE_SOURCE_EVENTS:
    return FR_DIAG_MSG_COMPILE_EVENTS_DISABLED;
  default:
    return FR_DIAG_MSG_NONE;
  }
}

static bool fr_compile_source_feature_enabled(
    fr_compile_source_feature_t feature) {
  switch (feature) {
  case FR_COMPILE_SOURCE_CONTROL_FLOW:
    return FR_FEATURE_SOURCE_CONTROL_FLOW;
  case FR_COMPILE_SOURCE_CELLS:
    return FR_FEATURE_CELLS;
  case FR_COMPILE_SOURCE_TEXT:
    return FR_FEATURE_TEXT;
  case FR_COMPILE_SOURCE_RECORDS:
    return FR_FEATURE_RECORDS;
  case FR_COMPILE_SOURCE_EVENTS:
    return FR_FEATURE_EVENTS;
  default:
    return false;
  }
}

static void fr_compile_note_span_diagnostic(const fr_compile_context_t *ctx,
                                            fr_diag_kind_t kind,
                                            uint16_t message_id,
                                            fr_parse_span_t span) {
  if (ctx == NULL || ctx->diag == NULL ||
      ctx->diag->kind != FR_DIAG_NONE || message_id == FR_DIAG_MSG_NONE) {
    return;
  }
  ctx->diag->kind = kind;
  ctx->diag->span_start = span.start;
  ctx->diag->span_length = span.length;
  ctx->diag->message_id = message_id;
}

static fr_err_t
fr_compile_require_source_feature(const fr_compile_context_t *ctx,
                                  fr_compile_source_feature_t feature,
                                  fr_parse_span_t span) {
  if (fr_compile_source_feature_enabled(feature)) {
    return FR_OK;
  }
  if (fr_compile_source_feature_message(feature) == FR_DIAG_MSG_NONE) {
    return FR_ERR_INVALID;
  }
  fr_compile_note_span_diagnostic(
      ctx, FR_DIAG_TOKEN, fr_compile_source_feature_message(feature), span);
  return FR_ERR_UNSUPPORTED;
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

static bool fr_compile_candidate_length(const char *candidate,
                                        uint16_t *out_length) {
  uint16_t length = 0;

  if (candidate == NULL || out_length == NULL) {
    return false;
  }
  while (candidate[length] != '\0') {
    if (length >= FR_PARSE_MAX_TOKEN_BYTES) {
      return false;
    }
    length += 1;
  }
  if (length == 0) {
    return false;
  }
  *out_length = length;
  return true;
}

static uint8_t fr_compile_min3(uint8_t a, uint8_t b, uint8_t c) {
  uint8_t m = a < b ? a : b;
  return m < c ? m : c;
}

static uint8_t fr_compile_bounded_edit_distance(fr_parse_span_t typed,
                                                const char *candidate,
                                                uint16_t candidate_len,
                                                uint8_t max_distance) {
  uint8_t previous[FR_PARSE_MAX_TOKEN_BYTES + 1];
  uint8_t current[FR_PARSE_MAX_TOKEN_BYTES + 1];
  uint8_t limit = (uint8_t)(max_distance + 1u);

  if (typed.start == NULL || typed.length > FR_PARSE_MAX_TOKEN_BYTES ||
      candidate == NULL || candidate_len > FR_PARSE_MAX_TOKEN_BYTES) {
    return limit;
  }
  if (typed.length > candidate_len) {
    if (typed.length - candidate_len > max_distance) {
      return limit;
    }
  } else if (candidate_len - typed.length > max_distance) {
    return limit;
  }

  for (uint16_t j = 0; j <= candidate_len; j++) {
    previous[j] = j > max_distance ? limit : (uint8_t)j;
  }

  for (uint16_t i = 1; i <= typed.length; i++) {
    uint8_t row_min = limit;

    current[0] = i > max_distance ? limit : (uint8_t)i;
    row_min = current[0];
    for (uint16_t j = 1; j <= candidate_len; j++) {
      uint8_t cost = typed.start[i - 1u] == candidate[j - 1u] ? 0u : 1u;
      uint8_t delete_cost =
          previous[j] >= limit ? limit : (uint8_t)(previous[j] + 1u);
      uint8_t insert_cost =
          current[j - 1u] >= limit ? limit : (uint8_t)(current[j - 1u] + 1u);
      uint8_t replace_cost =
          previous[j - 1u] >= limit
              ? limit
              : (uint8_t)(previous[j - 1u] + cost);

      current[j] = fr_compile_min3(delete_cost, insert_cost, replace_cost);
      if (current[j] < row_min) {
        row_min = current[j];
      }
    }
    if (row_min > max_distance) {
      return limit;
    }
    for (uint16_t j = 0; j <= candidate_len; j++) {
      previous[j] = current[j];
    }
  }

  return previous[candidate_len] > max_distance ? limit
                                                : previous[candidate_len];
}

typedef struct fr_compile_suggestion_scan_t {
  const char *prefix_start;
  uint16_t prefix_length;
  uint8_t prefix_count;
  const char *best_start;
  uint16_t best_length;
  uint8_t best_distance;
  uint8_t second_distance;
} fr_compile_suggestion_scan_t;

static bool fr_compile_name_is_proper_prefix(fr_parse_span_t typed,
                                             const char *candidate,
                                             uint16_t candidate_len) {
  return typed.length > 0 && candidate_len > typed.length &&
         memcmp(typed.start, candidate, typed.length) == 0;
}

static bool fr_compile_project_name_is_callable(
    const fr_compile_context_t *ctx, const char *name) {
  fr_slot_id_t slot_id = 0;
  fr_tagged_t tagged = 0;
  fr_native_id_t native_id = 0;
  fr_code_object_id_t code_object_id = 0;

  if (ctx == NULL || ctx->runtime == NULL || name == NULL) {
    return false;
  }
  if (fr_slot_id_for_name(ctx->runtime, name, &slot_id) != FR_OK ||
      fr_slot_read(ctx->runtime, slot_id, &tagged) != FR_OK) {
    return false;
  }
  return fr_tagged_decode_native_id(tagged, &native_id) == FR_OK ||
         fr_tagged_decode_code_object_id(tagged, &code_object_id) == FR_OK;
}

static void fr_compile_consider_suggestion(fr_parse_span_t typed,
                                           const char *candidate,
                                           fr_compile_suggestion_scan_t *scan) {
  enum { FR_COMPILE_SUGGEST_MAX_DISTANCE = 2 };
  uint16_t candidate_len = 0;
  uint8_t distance = 0;

  if (scan == NULL || !fr_compile_candidate_length(candidate, &candidate_len)) {
    return;
  }
  if (fr_compile_name_is_proper_prefix(typed, candidate, candidate_len)) {
    if (scan->prefix_count == 0) {
      scan->prefix_start = candidate;
      scan->prefix_length = candidate_len;
    }
    if (scan->prefix_count < 2) {
      scan->prefix_count += 1;
    }
  }
  distance = fr_compile_bounded_edit_distance(
      typed, candidate, candidate_len, FR_COMPILE_SUGGEST_MAX_DISTANCE);
  if (distance > FR_COMPILE_SUGGEST_MAX_DISTANCE) {
    return;
  }
  if (distance < scan->best_distance) {
    scan->second_distance = scan->best_distance;
    scan->best_distance = distance;
    scan->best_start = candidate;
    scan->best_length = candidate_len;
  } else if (distance < scan->second_distance) {
    scan->second_distance = distance;
  }
}

static void fr_compile_note_name_suggestion(const fr_compile_context_t *ctx,
                                            fr_parse_span_t name,
                                            bool call_position) {
  enum { FR_COMPILE_SUGGEST_MAX_DISTANCE = 2 };
  fr_compile_suggestion_scan_t scan = {
      .best_distance = FR_COMPILE_SUGGEST_MAX_DISTANCE + 1u,
      .second_distance = FR_COMPILE_SUGGEST_MAX_DISTANCE + 1u,
  };
  const char *suggestion = NULL;
  uint16_t suggestion_length = 0;

  if (ctx == NULL || ctx->diag == NULL || name.start == NULL ||
      name.length == 0 || name.length > FR_PARSE_MAX_TOKEN_BYTES) {
    return;
  }

#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  for (uint16_t i = 0; i < fr_base_def_count(); i++) {
    const fr_base_def_t *def = NULL;
    fr_base_layer_t layer = FR_BASE_LAYER_CORE;

    if (fr_base_def_at(i, &def, &layer) != FR_OK || def == NULL) {
      continue;
    }
    (void)layer;
    if (call_position && def->kind != FR_BASE_DEF_NATIVE) {
      continue;
    }
    fr_compile_consider_suggestion(name, def->name, &scan);
    fr_compile_consider_suggestion(name, def->alias, &scan);
  }
#endif

  if (ctx->runtime != NULL) {
    uint16_t project_name_count = fr_slot_project_name_count(ctx->runtime);

    for (uint16_t i = 0; i < project_name_count; i++) {
      const char *project_name = fr_slot_project_name_at(ctx->runtime, i);

      if (call_position &&
          !fr_compile_project_name_is_callable(ctx, project_name)) {
        continue;
      }
      fr_compile_consider_suggestion(name, project_name, &scan);
    }
  }

  if (scan.prefix_count > 1) {
    return;
  }
  if (scan.prefix_count == 1) {
    suggestion = scan.prefix_start;
    suggestion_length = scan.prefix_length;
  } else {
    if (scan.best_start == NULL ||
        scan.best_distance > FR_COMPILE_SUGGEST_MAX_DISTANCE ||
        scan.best_distance + 1u > scan.second_distance ||
        scan.best_distance >= name.length ||
        scan.best_length > FR_PARSE_MAX_TOKEN_BYTES) {
      return;
    }
    suggestion = scan.best_start;
    suggestion_length = scan.best_length;
  }
  if (suggestion == NULL || suggestion_length > FR_PARSE_MAX_TOKEN_BYTES) {
    return;
  }
  memcpy(ctx->diag->suggestion_text, suggestion, suggestion_length);
  ctx->diag->suggestion_text[suggestion_length] = '\0';
  ctx->diag->suggestion_start = ctx->diag->suggestion_text;
  ctx->diag->suggestion_length = suggestion_length;
}

static bool
fr_compile_event_outer_name_matches(const fr_compile_context_t *ctx,
                                    fr_parse_span_t name) {
  if (ctx == NULL) {
    return false;
  }
  if (ctx->event_outer_params != NULL) {
    for (uint8_t i = 0; i < ctx->event_outer_param_count; i++) {
      if (fr_compile_span_same(ctx->event_outer_params[i], name)) {
        return true;
      }
    }
  }
  if (ctx->event_outer_locals != NULL) {
    for (uint8_t i = ctx->event_outer_locals->count; i > 0; i--) {
      if (fr_compile_span_same(ctx->event_outer_locals->entries[i - 1u].name,
                               name)) {
        return true;
      }
    }
  }
  return false;
}

static void fr_compile_note_name_diagnostic(const fr_compile_context_t *ctx,
                                            fr_parse_span_t name,
                                            bool call_position) {
  if (ctx == NULL || ctx->diag == NULL ||
      ctx->diag->kind != FR_DIAG_NONE) {
    return;
  }
  ctx->diag->kind = FR_DIAG_NAME;
  ctx->diag->span_start = name.start;
  ctx->diag->span_length = name.length;
  if (fr_compile_event_outer_name_matches(ctx, name)) {
    ctx->diag->message_id = FR_DIAG_MSG_COMPILE_EVENT_BODY_LOCAL;
    return;
  }
  fr_compile_note_name_suggestion(ctx, name, call_position);
}

static void fr_compile_note_arity_diagnostic(const fr_compile_context_t *ctx,
                                             fr_parse_span_t name,
                                             uint16_t expected,
                                             uint16_t got) {
  if (ctx == NULL || ctx->diag == NULL ||
      ctx->diag->kind != FR_DIAG_NONE) {
    return;
  }
  ctx->diag->kind = FR_DIAG_ARITY;
  ctx->diag->span_start = name.start;
  ctx->diag->span_length = name.length;
  ctx->diag->expected = expected;
  ctx->diag->got = got;
}

static fr_err_t fr_compile_expr_slot_for_name(const fr_compile_context_t *ctx,
                                              fr_parse_span_t name,
                                              fr_slot_id_t *out_slot_id,
                                              bool call_position) {
  fr_err_t err = fr_compile_slot_for_name(ctx, name, out_slot_id);

  if (err == FR_ERR_NOT_FOUND) {
    fr_compile_note_name_diagnostic(ctx, name, call_position);
  }
  return err;
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

static fr_parse_span_t
fr_compile_expr_diagnostic_span(const fr_parse_line_t *parsed,
                                const fr_parse_expr_t *expr) {
  if (expr == NULL) {
    return (fr_parse_span_t){0};
  }
  if (expr->name.start != NULL) {
    return expr->name;
  }
  if (expr->text.start != NULL) {
    return expr->text;
  }
  if (expr->child_count > 0) {
    return fr_compile_expr_diagnostic_span(
        parsed, fr_compile_expr_at(parsed, expr->children[0]));
  }
  return (fr_parse_span_t){0};
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

/* Walk back-to-front so a later `here` with the same name shadows the
 * earlier binding. Slot indices live on each entry, not the array
 * position, so a block exit can drop visible entries without disturbing
 * the slot a later `here` will get. */
static bool fr_compile_local_for_name(const fr_compile_context_t *ctx,
                                      fr_parse_span_t name,
                                      uint8_t *out_local_index) {
  if (ctx == NULL || ctx->locals == NULL || out_local_index == NULL) {
    return false;
  }
  for (uint8_t i = ctx->locals->count; i > 0; i--) {
    if (fr_compile_span_same(ctx->locals->entries[i - 1].name, name)) {
      *out_local_index = ctx->locals->entries[i - 1].index;
      return true;
    }
  }
  return false;
}

static fr_err_t fr_compile_add_local(const fr_compile_context_t *ctx,
                                     fr_parse_span_t name,
                                     uint8_t *out_local_index) {
  uint8_t shadow_arg = 0;

  if (ctx == NULL || ctx->locals == NULL || out_local_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_compile_param_for_name(ctx, name, &shadow_arg)) {
    (void)shadow_arg;
    fr_compile_note_span_diagnostic(
        ctx, FR_DIAG_TOKEN, FR_DIAG_MSG_COMPILE_PARAM_SHADOW, name);
    return FR_ERR_INVALID;
  }
  if (ctx->locals->next_index >= FR_PARSE_MAX_LOCALS ||
      ctx->locals->count >= FR_PARSE_MAX_LOCALS) {
    return FR_ERR_CAPACITY;
  }
  ctx->locals->entries[ctx->locals->count].name = name;
  ctx->locals->entries[ctx->locals->count].index = ctx->locals->next_index;
  *out_local_index = ctx->locals->next_index;
  ctx->locals->count = (uint8_t)(ctx->locals->count + 1);
  ctx->locals->next_index = (uint8_t)(ctx->locals->next_index + 1);
  return FR_OK;
}

static uint8_t fr_compile_count_local_binds(const fr_parse_line_t *parsed) {
  uint8_t count = 0;

  if (parsed == NULL) {
    return 0;
  }
  for (uint8_t i = 0; i < parsed->expr_count; i++) {
    if (parsed->exprs[i].kind == FR_PARSE_EXPR_LOCAL_BIND ||
        (parsed->exprs[i].kind == FR_PARSE_EXPR_REPEAT &&
         parsed->exprs[i].name.start != NULL)) {
      count = (uint8_t)(count + 1);
    }
  }
  return count;
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

static fr_err_t fr_compile_write_u32(uint8_t instruction_bytes[],
                                     uint16_t *offset, uint32_t word) {
  if (*offset + 4u > FR_COMPILE_MAX_INSTRUCTION_BYTES) {
    return FR_ERR_CAPACITY;
  }
  fr_write_u32_le(&instruction_bytes[*offset], word);
  *offset = (uint16_t)(*offset + 4u);
  return FR_OK;
}

static fr_err_t fr_compile_write_int_operand(uint8_t instruction_bytes[],
                                             uint16_t *offset,
                                             fr_int_t int_operand) {
  return fr_compile_write_u32(instruction_bytes, offset,
                              (uint32_t)(int32_t)int_operand);
}

static fr_err_t fr_compile_emit_slot_op(uint8_t instruction_bytes[],
                                        uint16_t *offset, fr_opcode_t op,
                                        fr_slot_id_t slot_id) {
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset, (uint8_t)op));
  return fr_compile_write_u16(instruction_bytes, offset, slot_id);
}

static fr_err_t fr_compile_read_code_header(fr_runtime_t *runtime,
                                            fr_code_object_id_t code_object_id,
                                            fr_instruction_header_t *header) {
  fr_instruction_stream_t instructions;

  FR_TRY(fr_code_get_instructions(runtime, code_object_id, &instructions));
  if (instructions.bytes != NULL) {
    return fr_instruction_read_header(&instructions, header);
  }

  {
    uint8_t header_bytes[FR_INSTRUCTION_MAX_HEADER_SIZE];
    uint8_t header_size = 0;
    fr_instruction_stream_t header_view;

    FR_TRY(fr_code_read_u8(runtime, code_object_id, 1, &header_size));
    if (header_size < FR_INSTRUCTION_MIN_HEADER_SIZE ||
        header_size > FR_INSTRUCTION_MAX_HEADER_SIZE) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_code_read(runtime, code_object_id, 0, header_bytes, header_size));
    FR_TRY(fr_instruction_stream_init(&header_view, header_bytes, header_size));
    return fr_instruction_read_header(&header_view, header);
  }
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

static bool fr_compile_current_definition_for_slot(
    const fr_compile_context_t *ctx, fr_slot_id_t slot_id, uint8_t *out_arity) {
  if (ctx == NULL || out_arity == NULL || !ctx->has_definition ||
      ctx->definition_slot_id != slot_id) {
    return false;
  }
  *out_arity = ctx->definition_arity;
  return true;
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

static fr_err_t fr_compile_emit_dynamic_cell_op(uint8_t instruction_bytes[],
                                                uint16_t *offset,
                                                fr_opcode_t op,
                                                fr_slot_id_t slot_id) {
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset, (uint8_t)op));
  return fr_compile_write_u16(instruction_bytes, offset, slot_id);
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

static fr_err_t fr_compile_emit_push_code_id(uint8_t instruction_bytes[],
                                             uint16_t *offset,
                                             uint16_t local_code_index) {
  FR_TRY(
      fr_compile_write_byte(instruction_bytes, offset, FR_OP_PUSH_CODE_ID));
  return fr_compile_write_u16(instruction_bytes, offset, local_code_index);
}

#if FR_FEATURE_TEXT
static fr_err_t fr_compile_emit_push_object_id(uint8_t instruction_bytes[],
                                               uint16_t *offset,
                                               fr_object_id_t object_id) {
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset,
                               FR_OP_PUSH_OBJECT_ID));
  return fr_compile_write_u16(instruction_bytes, offset, (uint16_t)object_id);
}

static int8_t fr_compile_hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return (int8_t)(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return (int8_t)(c - 'a' + 10);
  }
  if (c >= 'A' && c <= 'F') {
    return (int8_t)(c - 'A' + 10);
  }
  return -1;
}

static fr_err_t fr_compile_copy_text_literal(const fr_parse_expr_t *expr,
                                             uint8_t out[], uint16_t out_cap,
                                             uint16_t *out_length) {
  uint16_t used = 0;

  if (expr == NULL || out == NULL || out_length == NULL ||
      expr->kind != FR_PARSE_EXPR_TEXT) {
    return FR_ERR_INVALID;
  }
  if (expr->text.length > 0 && expr->text.start == NULL) {
    return FR_ERR_INVALID;
  }
  if (!expr->text_has_escapes) {
    if (expr->text.length > out_cap) {
      return FR_ERR_RANGE;
    }
    if (expr->text.length > 0) {
      memcpy(out, expr->text.start, expr->text.length);
    }
    *out_length = expr->text.length;
    return FR_OK;
  }

  for (uint16_t i = 0; i < expr->text.length; i++) {
    uint8_t byte = (uint8_t)expr->text.start[i];

    if (used >= out_cap) {
      return FR_ERR_RANGE;
    }
    if (byte != '\\') {
      out[used] = byte;
      used = (uint16_t)(used + 1);
      continue;
    }

    i += 1;
    if (i >= expr->text.length) {
      return FR_ERR_INVALID;
    }
    switch (expr->text.start[i]) {
    case 'n': out[used] = '\n'; break;
    case 'r': out[used] = '\r'; break;
    case 't': out[used] = '\t'; break;
    case '"': out[used] = '"'; break;
    case '\\': out[used] = '\\'; break;
    case 'x': {
      int8_t high = 0;
      int8_t low = 0;

      if (i + 2 >= expr->text.length) {
        return FR_ERR_INVALID;
      }
      high = fr_compile_hex_value(expr->text.start[i + 1]);
      low = fr_compile_hex_value(expr->text.start[i + 2]);
      if (high < 0 || low < 0) {
        return FR_ERR_INVALID;
      }
      out[used] = (uint8_t)((high << 4) | low);
      i = (uint16_t)(i + 2);
      break;
    }
    default:
      return FR_ERR_INVALID;
    }
    used = (uint16_t)(used + 1);
  }

  *out_length = used;
  return FR_OK;
}

static fr_err_t fr_compile_body_texts_add(fr_compile_body_texts_t *bt,
                                          const fr_parse_expr_t *expr,
                                          fr_object_id_t *out_object_id) {
  uint16_t offset = 0;
  uint16_t copied_length = 0;

  if (bt == NULL || expr == NULL || out_object_id == NULL ||
      bt->objects == NULL || bt->storage == NULL || bt->offsets == NULL ||
      bt->storage_capacity == 0) {
    return FR_ERR_INVALID;
  }
  if (bt->count >= bt->object_capacity || bt->used > bt->storage_capacity) {
    return FR_ERR_RANGE;
  }

  offset = bt->used;
  FR_TRY(fr_compile_copy_text_literal(
      expr, &bt->storage[offset], (uint16_t)(bt->storage_capacity - offset),
      &copied_length));
  bt->offsets[bt->count] = offset;
  bt->objects[bt->count] = (fr_image_text_object_t){
      .bytes = copied_length > 0 ? &bt->storage[offset] : NULL,
      .length = copied_length,
  };
  *out_object_id = (fr_object_id_t)bt->count;
  bt->count = (uint16_t)(bt->count + 1);
  bt->used = (uint16_t)(bt->used + copied_length);
  return FR_OK;
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
  if (!expr->text_has_escapes &&
      expr->text.length > FR_PROFILE_MAX_TEXT_LENGTH) {
    return FR_ERR_RANGE;
  }
  if (expr->text.length > 0 && expr->text.start == NULL) {
    return FR_ERR_INVALID;
  }
  if (ctx->body_texts != NULL) {
    fr_compile_body_texts_t *bt = ctx->body_texts;

    FR_TRY(fr_compile_body_texts_add(bt, expr, &object_id));
    return fr_compile_emit_push_object_id(instruction_bytes, offset, object_id);
  }
  if (ctx->runtime == NULL) {
    return FR_ERR_UNSUPPORTED;
  }
  if (expr->text_has_escapes) {
    uint8_t decoded[FR_PROFILE_MAX_TEXT_LENGTH > 0
                        ? FR_PROFILE_MAX_TEXT_LENGTH
                        : 1];
    uint16_t decoded_length = 0;

    FR_TRY(fr_compile_copy_text_literal(
        expr, decoded, (uint16_t)sizeof(decoded), &decoded_length));
    FR_TRY(fr_text_install(ctx->runtime, decoded, decoded_length, &object_id));
  } else {
    FR_TRY(fr_text_install(ctx->runtime, (const uint8_t *)expr->text.start,
                           expr->text.length, &object_id));
  }
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
  uint8_t definition_arity = 0;

  FR_TRY(fr_compile_expr_slot_for_name(ctx, expr->name, &slot_id, true));

  if (fr_compile_current_definition_for_slot(ctx, slot_id,
                                             &definition_arity)) {
    if (expr->child_count != definition_arity) {
      fr_compile_note_arity_diagnostic(ctx, expr->name, definition_arity,
                                       expr->child_count);
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
    if (definition_arity == 0) {
      return fr_compile_emit_slot_op(instruction_bytes, offset,
                                     FR_OP_CALL_SLOT, slot_id);
    }
    return fr_compile_emit_call_slot_arg(instruction_bytes, offset, slot_id,
                                         definition_arity);
  }

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
        fr_compile_note_arity_diagnostic(ctx, expr->name, entry->arity,
                                         expr->child_count);
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
      fr_instruction_header_t header;

      FR_TRY(fr_compile_read_code_header(ctx->runtime, code_object_id,
                                         &header));
      if (expr->child_count != header.arity) {
        fr_compile_note_arity_diagnostic(ctx, expr->name, header.arity,
                                         expr->child_count);
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
      fr_compile_note_arity_diagnostic(ctx, expr->name, def->native_arity,
                                       expr->child_count);
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
  /* Children alternate cond/body for each arm; an odd trailing child is the
   * final `else` body. Each arm emits cond → JUMP_IF_FALSY(next-arm) →
   * body → JUMP(end). Bare `if` (no else) still falls through to PUSH_NIL,
   * preserving the renderer's existing shape match for the 2-child case. */
  uint16_t end_operands[FR_PARSE_MAX_BODY_EXPRS / 2];
  uint8_t end_operand_count = 0;
  uint8_t arm_count = 0;
  bool has_final_else = false;

  if (expr == NULL || expr->child_count < 2 ||
      expr->child_count > FR_PARSE_MAX_BODY_EXPRS) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_compile_require_source_feature(
      ctx, FR_COMPILE_SOURCE_CONTROL_FLOW,
      fr_compile_expr_diagnostic_span(parsed, expr)));

  arm_count = (uint8_t)(expr->child_count / 2u);
  has_final_else = (expr->child_count % 2u) == 1u;

  for (uint8_t arm = 0; arm < arm_count; arm++) {
    uint16_t false_target_operand = 0;
    uint8_t cond_index = (uint8_t)(arm * 2u);

    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[cond_index],
                                instruction_bytes, offset));
    FR_TRY(fr_compile_emit_jump_placeholder(
        instruction_bytes, offset, FR_OP_JUMP_IF_FALSY, &false_target_operand));
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[cond_index + 1u],
                                instruction_bytes, offset));
    FR_TRY(fr_compile_emit_jump_placeholder(
        instruction_bytes, offset, FR_OP_JUMP,
        &end_operands[end_operand_count]));
    end_operand_count = (uint8_t)(end_operand_count + 1u);
    FR_TRY(
        fr_compile_patch_u16(instruction_bytes, false_target_operand, *offset));
  }

  if (has_final_else) {
    FR_TRY(fr_compile_emit_expr(ctx, parsed,
                                expr->children[expr->child_count - 1u],
                                instruction_bytes, offset));
  } else {
    FR_TRY(fr_compile_emit_push_nil(instruction_bytes, offset));
  }
  for (uint8_t i = 0; i < end_operand_count; i++) {
    FR_TRY(fr_compile_patch_u16(instruction_bytes, end_operands[i], *offset));
  }
  return FR_OK;
}

static fr_err_t fr_compile_emit_bool_result(const fr_compile_context_t *ctx,
                                            const fr_parse_line_t *parsed,
                                            fr_parse_expr_id_t expr_id,
                                            uint8_t instruction_bytes[],
                                            uint16_t *offset, bool negate) {
  uint16_t false_target_operand = 0;
  uint16_t end_operand = 0;

  FR_TRY(fr_compile_require_source_feature(
      ctx, FR_COMPILE_SOURCE_CONTROL_FLOW,
      fr_compile_expr_diagnostic_span(parsed,
                                      fr_compile_expr_at(parsed, expr_id))));
  FR_TRY(
      fr_compile_emit_expr(ctx, parsed, expr_id, instruction_bytes, offset));
  FR_TRY(fr_compile_emit_jump_placeholder(
      instruction_bytes, offset, FR_OP_JUMP_IF_FALSY, &false_target_operand));
  FR_TRY(fr_compile_emit_push_bool(instruction_bytes, offset, !negate));
  FR_TRY(fr_compile_emit_jump_placeholder(instruction_bytes, offset, FR_OP_JUMP,
                                          &end_operand));
  FR_TRY(
      fr_compile_patch_u16(instruction_bytes, false_target_operand, *offset));
  FR_TRY(fr_compile_emit_push_bool(instruction_bytes, offset, negate));
  return fr_compile_patch_u16(instruction_bytes, end_operand, *offset);
}

static fr_err_t fr_compile_emit_and(const fr_compile_context_t *ctx,
                                    const fr_parse_line_t *parsed,
                                    const fr_parse_expr_t *expr,
                                    uint8_t instruction_bytes[],
                                    uint16_t *offset) {
  uint16_t false_target_operand = 0;
  uint16_t end_operand = 0;

  if (expr == NULL || expr->child_count != 2) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_compile_require_source_feature(
      ctx, FR_COMPILE_SOURCE_CONTROL_FLOW,
      fr_compile_expr_diagnostic_span(parsed, expr)));
  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0],
                              instruction_bytes, offset));
  FR_TRY(fr_compile_emit_jump_placeholder(
      instruction_bytes, offset, FR_OP_JUMP_IF_FALSY, &false_target_operand));
  FR_TRY(fr_compile_emit_bool_result(ctx, parsed, expr->children[1],
                                     instruction_bytes, offset, false));
  FR_TRY(fr_compile_emit_jump_placeholder(instruction_bytes, offset, FR_OP_JUMP,
                                          &end_operand));
  FR_TRY(
      fr_compile_patch_u16(instruction_bytes, false_target_operand, *offset));
  FR_TRY(fr_compile_emit_push_bool(instruction_bytes, offset, false));
  return fr_compile_patch_u16(instruction_bytes, end_operand, *offset);
}

static fr_err_t fr_compile_emit_or(const fr_compile_context_t *ctx,
                                   const fr_parse_line_t *parsed,
                                   const fr_parse_expr_t *expr,
                                   uint8_t instruction_bytes[],
                                   uint16_t *offset) {
  uint16_t rhs_target_operand = 0;
  uint16_t end_operand = 0;

  if (expr == NULL || expr->child_count != 2) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_compile_require_source_feature(
      ctx, FR_COMPILE_SOURCE_CONTROL_FLOW,
      fr_compile_expr_diagnostic_span(parsed, expr)));
  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0],
                              instruction_bytes, offset));
  FR_TRY(fr_compile_emit_jump_placeholder(
      instruction_bytes, offset, FR_OP_JUMP_IF_FALSY, &rhs_target_operand));
  FR_TRY(fr_compile_emit_push_bool(instruction_bytes, offset, true));
  FR_TRY(fr_compile_emit_jump_placeholder(instruction_bytes, offset, FR_OP_JUMP,
                                          &end_operand));
  FR_TRY(fr_compile_patch_u16(instruction_bytes, rhs_target_operand, *offset));
  FR_TRY(fr_compile_emit_bool_result(ctx, parsed, expr->children[1],
                                     instruction_bytes, offset, false));
  return fr_compile_patch_u16(instruction_bytes, end_operand, *offset);
}

static fr_err_t fr_compile_emit_repeat(const fr_compile_context_t *ctx,
                                       const fr_parse_line_t *parsed,
                                       const fr_parse_expr_t *expr,
                                       uint8_t instruction_bytes[],
                                       uint16_t *offset) {
  const fr_parse_expr_t *count = NULL;
  uint16_t done_target_operand = 0;
  uint16_t body_offset = 0;
  uint8_t saved_locals_count = 0;
  uint8_t index_local = 0;
  bool has_index = false;

  if (expr == NULL || expr->child_count != 2) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_compile_require_source_feature(
      ctx, FR_COMPILE_SOURCE_CONTROL_FLOW,
      fr_compile_expr_diagnostic_span(parsed, expr)));

  count = fr_compile_expr_at(parsed, expr->children[0]);
  if (count == NULL) {
    return FR_ERR_INVALID;
  }
  if (count->kind == FR_PARSE_EXPR_INT) {
    if (count->int_value < 0) {
      return FR_ERR_RANGE;
    }
  }

  has_index = expr->name.start != NULL;
  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0],
                              instruction_bytes, offset));
  if (has_index) {
    if (ctx == NULL || ctx->locals == NULL) {
      return FR_ERR_INVALID;
    }
    saved_locals_count = ctx->locals->count;
    FR_TRY(fr_compile_add_local(ctx, expr->name, &index_local));
    FR_TRY(fr_compile_write_byte(instruction_bytes, offset,
                                 FR_OP_REPEAT_BEGIN_AS));
    done_target_operand = *offset;
    FR_TRY(fr_compile_write_u16(instruction_bytes, offset, 0));
    FR_TRY(fr_compile_write_byte(instruction_bytes, offset, index_local));
  } else {
    FR_TRY(fr_compile_emit_jump_placeholder(
        instruction_bytes, offset, FR_OP_REPEAT_BEGIN, &done_target_operand));
  }
  body_offset = *offset;
  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[1],
                              instruction_bytes, offset));
  FR_TRY(fr_compile_emit_drop(instruction_bytes, offset));
#if FR_FEATURE_BYTES
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset, FR_OP_BYTES_RESET));
#endif
  if (has_index) {
    FR_TRY(fr_compile_write_byte(instruction_bytes, offset,
                                 FR_OP_REPEAT_NEXT_AS));
    FR_TRY(fr_compile_write_u16(instruction_bytes, offset, body_offset));
    FR_TRY(fr_compile_write_byte(instruction_bytes, offset, index_local));
    ctx->locals->count = saved_locals_count;
  } else {
    FR_TRY(fr_compile_emit_jump_target(instruction_bytes, offset,
                                       FR_OP_REPEAT_NEXT, body_offset));
  }
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
  FR_TRY(fr_compile_require_source_feature(
      ctx, FR_COMPILE_SOURCE_CONTROL_FLOW,
      fr_compile_expr_diagnostic_span(parsed, expr)));

  cond_offset = *offset;
  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0],
                              instruction_bytes, offset));
  FR_TRY(fr_compile_emit_jump_placeholder(
      instruction_bytes, offset, FR_OP_JUMP_IF_FALSY, &done_target_operand));
  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[1],
                              instruction_bytes, offset));
  FR_TRY(fr_compile_emit_drop(instruction_bytes, offset));
#if FR_FEATURE_BYTES
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset, FR_OP_BYTES_RESET));
#endif
  FR_TRY(fr_compile_emit_jump_target(instruction_bytes, offset, FR_OP_JUMP,
                                     cond_offset));
  FR_TRY(fr_compile_patch_u16(instruction_bytes, done_target_operand, *offset));
  return fr_compile_emit_push_nil(instruction_bytes, offset);
}

/* `on <pin> <edge> [debounce <ms>] [body]`, `every <ms> [body]`, and
 * `after <ms> [body]` emit the body as a sibling code object, then push
 * (kind, source, debounce, body-code-id) and call the event-register
 * native. Timers carry no debounce, so child_count is fixed at 2 for
 * EVERY/AFTER. The body code id is patched in at install time through
 * PUSH_CODE_ID. */
static fr_err_t
fr_compile_emit_event_register(const fr_compile_context_t *ctx,
                               const fr_parse_line_t *parsed,
                               const fr_parse_expr_t *expr,
                               uint8_t instruction_bytes[], uint16_t *offset) {
  fr_compile_event_body_t *body = NULL;
  fr_compile_context_t body_ctx = {0};
  fr_compile_locals_t body_locals = {0};
  uint16_t body_offset = FR_INSTRUCTION_LOCALS_HEADER_SIZE;
  bool is_gpio_kind = false;
  bool is_timer_kind = false;
  bool is_wifi_kind = false;
  uint8_t body_child = 1;

  if (expr == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_compile_require_source_feature(
      ctx, FR_COMPILE_SOURCE_EVENTS,
      fr_compile_expr_diagnostic_span(parsed, expr)));
  is_gpio_kind = expr->int_value >= FR_EVENT_KIND_GPIO_RISING &&
                 expr->int_value <= FR_EVENT_KIND_GPIO_CHANGES;
  is_timer_kind = expr->int_value == FR_EVENT_KIND_EVERY ||
                  expr->int_value == FR_EVENT_KIND_AFTER;
  is_wifi_kind = expr->int_value == FR_EVENT_KIND_WIFI_DISCONNECTED ||
                 expr->int_value == FR_EVENT_KIND_WIFI_RECONNECTED;
  if (!is_gpio_kind && !is_timer_kind && !is_wifi_kind) {
    return FR_ERR_INVALID;
  }
  if (is_gpio_kind && expr->child_count != 2 && expr->child_count != 3) {
    return FR_ERR_INVALID;
  }
  if (is_timer_kind && expr->child_count != 2) {
    return FR_ERR_INVALID;
  }
  /* D19: wifi binds have one child — the body. Source and debounce are 0. */
  if (is_wifi_kind && expr->child_count != 1) {
    return FR_ERR_INVALID;
  }
  if (is_wifi_kind) {
    body_child = 0;
  }
  if (ctx == NULL || ctx->event_body == NULL) {
    return FR_ERR_UNSUPPORTED;
  }
  body = ctx->event_body;
  if (body->used) {
    return FR_ERR_CAPACITY;
  }

  /* Reserve the full locals header so a body that uses `here` runs with a
   * matching local_count. The byte at offset 3 is patched after emit. */
  body->bytes[0] = FR_INSTRUCTION_FORMAT_VERSION;
  body->bytes[1] = FR_INSTRUCTION_LOCALS_HEADER_SIZE;
  body->bytes[2] = 0;
  body->bytes[3] = 0;
  body_ctx = *ctx;
  body_ctx.params = NULL;
  body_ctx.param_count = 0;
  body_ctx.event_outer_params = ctx->params;
  body_ctx.event_outer_param_count = ctx->param_count;
  body_ctx.event_outer_locals = ctx->locals;
  body_ctx.body_texts = ctx->body_texts;
  body_ctx.locals = &body_locals;
  body_ctx.event_body = NULL;
  FR_TRY(fr_compile_emit_expr(&body_ctx, parsed, expr->children[body_child],
                              body->bytes, &body_offset));
  FR_TRY(fr_compile_write_byte(body->bytes, &body_offset, FR_OP_RETURN));
  /* next_index is the high-water-mark; LIST emit restores `count` on close
   * so the body needs to size locals to what was ever assigned. */
  body->bytes[3] = body_locals.next_index;
  body->object->instructions =
      (fr_instruction_stream_t){.bytes = body->bytes, .length = body_offset};
  body->object->param_names = NULL;
  body->object->param_names_length = 0;
  body->used = true;

  FR_TRY(fr_compile_emit_push_int(instruction_bytes, offset, expr->int_value));
  if (is_wifi_kind) {
    FR_TRY(fr_compile_emit_push_int(instruction_bytes, offset, 0));
  } else {
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0],
                                instruction_bytes, offset));
  }
  if (expr->child_count == 3) {
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[2],
                                instruction_bytes, offset));
  } else {
    FR_TRY(fr_compile_emit_push_int(instruction_bytes, offset, 0));
  }
  FR_TRY(fr_compile_emit_push_code_id(instruction_bytes, offset, 1));
  return fr_compile_emit_slot_op(instruction_bytes, offset,
                                 FR_OP_CALL_NATIVE_SLOT, FR_SLOT_EVENT_REGISTER);
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
  FR_TRY(fr_compile_require_source_feature(
      ctx, FR_COMPILE_SOURCE_CONTROL_FLOW,
      fr_compile_expr_diagnostic_span(parsed, expr)));

  body_offset = *offset;
  FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0],
                              instruction_bytes, offset));
  FR_TRY(fr_compile_emit_drop(instruction_bytes, offset));
#if FR_FEATURE_BYTES
  FR_TRY(fr_compile_write_byte(instruction_bytes, offset, FR_OP_BYTES_RESET));
#endif
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
  uint8_t local_index = 0;

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
    FR_TRY(fr_compile_require_source_feature(
        ctx, FR_COMPILE_SOURCE_TEXT,
        fr_compile_expr_diagnostic_span(parsed, expr)));
    return fr_compile_emit_text_literal(ctx, expr, instruction_bytes, offset);
#else
    return FR_ERR_UNSUPPORTED;
#endif
  case FR_PARSE_EXPR_NAME: {
    uint8_t local_index = 0;
    if (fr_compile_local_for_name(ctx, expr->name, &local_index)) {
      FR_TRY(fr_compile_write_byte(instruction_bytes, offset,
                                   FR_OP_LOAD_LOCAL));
      return fr_compile_write_byte(instruction_bytes, offset, local_index);
    }
    if (fr_compile_param_for_name(ctx, expr->name, &arg_index)) {
      return fr_compile_emit_load_arg(instruction_bytes, offset, arg_index);
    }
    FR_TRY(fr_compile_expr_slot_for_name(ctx, expr->name, &slot_id, false));
    return fr_compile_emit_slot_op(instruction_bytes, offset, FR_OP_LOAD_SLOT,
                                   slot_id);
  }
  case FR_PARSE_EXPR_LOCAL_BIND: {
    uint8_t local_index = 0;
    if (expr->child_count != 1) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->child, instruction_bytes,
                                offset));
    FR_TRY(fr_compile_add_local(ctx, expr->name, &local_index));
    FR_TRY(fr_compile_write_byte(instruction_bytes, offset, FR_OP_STORE_LOCAL));
    return fr_compile_write_byte(instruction_bytes, offset, local_index);
  }
#if FR_FEATURE_CELLS
  case FR_PARSE_EXPR_CELL_READ:
    FR_TRY(fr_compile_require_source_feature(
        ctx, FR_COMPILE_SOURCE_CELLS,
        fr_compile_expr_diagnostic_span(parsed, expr)));
    if (expr->child_count != 1) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_compile_expr_slot_for_name(ctx, expr->name, &slot_id, false));
    {
      const fr_parse_expr_t *index =
          fr_compile_expr_at(parsed, expr->children[0]);
      if (index == NULL) {
        return FR_ERR_INVALID;
      }
      if (index->kind == FR_PARSE_EXPR_INT) {
        FR_TRY(fr_compile_cell_index(index->int_value, &cell_index));
        return fr_compile_emit_cell_op(instruction_bytes, offset,
                                       FR_OP_LOAD_CELL, slot_id, cell_index);
      }
    }
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0],
                                instruction_bytes, offset));
    return fr_compile_emit_dynamic_cell_op(
        instruction_bytes, offset, FR_OP_LOAD_CELL_DYNAMIC, slot_id);
  case FR_PARSE_EXPR_CELL_WRITE:
    FR_TRY(fr_compile_require_source_feature(
        ctx, FR_COMPILE_SOURCE_CELLS,
        fr_compile_expr_diagnostic_span(parsed, expr)));
    if (expr->child_count != 2) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_compile_expr_slot_for_name(ctx, expr->name, &slot_id, false));
    {
      const fr_parse_expr_t *index =
          fr_compile_expr_at(parsed, expr->children[0]);
      if (index == NULL) {
        return FR_ERR_INVALID;
      }
      if (index->kind == FR_PARSE_EXPR_INT) {
        FR_TRY(fr_compile_cell_index(index->int_value, &cell_index));
        FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[1],
                                    instruction_bytes, offset));
        return fr_compile_emit_cell_op(instruction_bytes, offset,
                                       FR_OP_STORE_CELL, slot_id, cell_index);
      }
    }
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[1],
                                instruction_bytes, offset));
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0],
                                instruction_bytes, offset));
    return fr_compile_emit_dynamic_cell_op(
        instruction_bytes, offset, FR_OP_STORE_CELL_DYNAMIC, slot_id);
#else
  case FR_PARSE_EXPR_CELL_READ:
  case FR_PARSE_EXPR_CELL_WRITE:
    return FR_ERR_UNSUPPORTED;
#endif
#if FR_FEATURE_RECORDS
  case FR_PARSE_EXPR_FIELD_READ:
    FR_TRY(fr_compile_require_source_feature(
        ctx, FR_COMPILE_SOURCE_RECORDS,
        fr_compile_expr_diagnostic_span(parsed, expr)));
    if (expr->child_count != 1) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->child, instruction_bytes,
                                offset));
    return fr_compile_emit_field_op(instruction_bytes, offset,
                                    FR_OP_LOAD_FIELD, expr->name);
  case FR_PARSE_EXPR_FIELD_WRITE:
    FR_TRY(fr_compile_require_source_feature(
        ctx, FR_COMPILE_SOURCE_RECORDS,
        fr_compile_expr_diagnostic_span(parsed, expr)));
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
  case FR_PARSE_EXPR_NAME_WRITE:
    if (expr->child_count != 1) {
      return FR_ERR_INVALID;
    }
    if (fr_compile_local_for_name(ctx, expr->name, &local_index)) {
      FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->child, instruction_bytes,
                                  offset));
      FR_TRY(fr_compile_write_byte(instruction_bytes, offset, FR_OP_SET_LOCAL));
      return fr_compile_write_byte(instruction_bytes, offset, local_index);
    }
    /* Parameters are immutable; trying to `set arg to ...` is a source bug. */
    if (fr_compile_param_for_name(ctx, expr->name, &arg_index)) {
      return FR_ERR_INVALID;
    }
    /* fr_compile_expr_slot_for_name errors if the name has never been declared,
     * which is exactly the "set on undeclared slot" rejection we want. */
    FR_TRY(fr_compile_expr_slot_for_name(ctx, expr->name, &slot_id, false));
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
  case FR_PARSE_EXPR_AND:
    return fr_compile_emit_and(ctx, parsed, expr, instruction_bytes, offset);
  case FR_PARSE_EXPR_OR:
    return fr_compile_emit_or(ctx, parsed, expr, instruction_bytes, offset);
  case FR_PARSE_EXPR_NOT:
    if (expr->child_count != 1) {
      return FR_ERR_INVALID;
    }
    return fr_compile_emit_bool_result(ctx, parsed, expr->child,
                                       instruction_bytes, offset, true);
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
  case FR_PARSE_EXPR_LIST: {
    /* A bracket statement list is a block: locals declared inside go out
     * of scope at the closing `]`. */
    uint8_t saved_locals_count =
        (ctx != NULL && ctx->locals != NULL) ? ctx->locals->count : 0;

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
    if (ctx != NULL && ctx->locals != NULL) {
      ctx->locals->count = saved_locals_count;
    }
    return FR_OK;
  }
  case FR_PARSE_EXPR_IF:
    return fr_compile_emit_if(ctx, parsed, expr, instruction_bytes, offset);
  case FR_PARSE_EXPR_REPEAT:
    return fr_compile_emit_repeat(ctx, parsed, expr, instruction_bytes, offset);
  case FR_PARSE_EXPR_WHILE:
    return fr_compile_emit_while(ctx, parsed, expr, instruction_bytes, offset);
  case FR_PARSE_EXPR_FOREVER:
    return fr_compile_emit_forever(ctx, parsed, expr, instruction_bytes,
                                   offset);
  case FR_PARSE_EXPR_EVENT_REGISTER:
    return fr_compile_emit_event_register(ctx, parsed, expr, instruction_bytes,
                                          offset);
  case FR_PARSE_EXPR_EVENT_CANCEL: {
    bool cancel_is_wifi =
        expr->int_value == FR_EVENT_KIND_WIFI_DISCONNECTED ||
        expr->int_value == FR_EVENT_KIND_WIFI_RECONNECTED;
    if (cancel_is_wifi) {
      if (expr->child_count != 0) {
        return FR_ERR_INVALID;
      }
    } else if (expr->child_count != 1) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_compile_require_source_feature(
        ctx, FR_COMPILE_SOURCE_EVENTS,
        fr_compile_expr_diagnostic_span(parsed, expr)));
    FR_TRY(fr_compile_emit_push_int(instruction_bytes, offset, expr->int_value));
    if (cancel_is_wifi) {
      FR_TRY(fr_compile_emit_push_int(instruction_bytes, offset, 0));
    } else {
      FR_TRY(fr_compile_emit_expr(ctx, parsed, expr->children[0],
                                  instruction_bytes, offset));
    }
    return fr_compile_emit_slot_op(instruction_bytes, offset,
                                   FR_OP_CALL_NATIVE_SLOT,
                                   FR_SLOT_EVENT_CANCEL);
  }
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
    FR_TRY(fr_compile_expr_slot_for_name(ctx, expr->name, &slot_id, false));
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
    fr_compile_note_span_diagnostic(
        ctx, FR_DIAG_TYPE, FR_DIAG_MSG_COMPILE_RECORD_NAME_NOT_SHAPE,
        parsed->definition.name);
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
  if (parsed == NULL || out == NULL || parsed->record_field_count == 0 ||
      parsed->record_field_count > FR_RECORD_FIELDS_PER_SHAPE_CAPACITY) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_compile_require_source_feature(
      ctx, FR_COMPILE_SOURCE_RECORDS, parsed->definition.name));
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
    fr_compile_body_texts_t texts = {
        .objects = out->text_objects,
        .storage = out->definition_text_bytes,
        .offsets = out->definition_text_offsets,
        .object_capacity = FR_PROFILE_MAX_OVERLAY_UPDATE_TEXT_OBJECTS,
        .storage_capacity = FR_PROFILE_MAX_DEFINITION_TEXT_BYTES,
        .count = *text_count,
        .used = 0,
    };
    fr_object_id_t object_id = 0;

    for (uint16_t i = 0; i < *text_count; i++) {
      texts.used = (uint16_t)(texts.used + out->text_objects[i].length);
    }

    if (!expr->text_has_escapes &&
        expr->text.length > FR_PROFILE_MAX_TEXT_LENGTH) {
      return FR_ERR_RANGE;
    }
    FR_TRY(fr_compile_body_texts_add(&texts, expr, &object_id));
    *out_ref =
        (fr_image_ref_t){FR_IMAGE_REF_TEXT_OBJECT, 0, object_id};
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
  FR_TRY(fr_compile_require_source_feature(
      ctx, FR_COMPILE_SOURCE_RECORDS,
      fr_compile_expr_diagnostic_span(parsed, value)));
  FR_TRY(fr_compile_expr_slot_for_name(ctx, value->name, &shape_slot, false));
  FR_TRY(fr_slot_read(ctx->runtime, shape_slot, &shape_tagged));
  FR_TRY(fr_tagged_decode_object_id(shape_tagged, &shape_object_id));
  FR_TRY(fr_record_shape_view(ctx->runtime, shape_object_id, &shape_name,
                              &shape_field_count));
  if (value->child_count != shape_field_count ||
      value->child_count > FR_RECORD_FIELDS_PER_SHAPE_CAPACITY) {
    fr_compile_note_arity_diagnostic(ctx, value->name, shape_field_count,
                                     value->child_count);
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
                                    fr_slot_id_t definition_slot_id,
                                    fr_compile_overlay_update_t *out) {
  fr_compile_context_t body_ctx = {0};
  fr_compile_body_texts_t body_texts = {
      .objects = out->text_objects,
      .storage = out->definition_text_bytes,
      .offsets = out->definition_text_offsets,
      .object_capacity = FR_PROFILE_MAX_OVERLAY_UPDATE_TEXT_OBJECTS,
      .storage_capacity = FR_PROFILE_MAX_DEFINITION_TEXT_BYTES,
      .count = 0,
      .used = 0,
  };
  fr_compile_locals_t locals = {0};
  fr_compile_event_body_t event_body = {
      .object = &out->event_body_object,
      .bytes = out->event_body_bytes,
      .used = false,
  };
  uint8_t arity = 0;
  uint8_t local_count = 0;
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
  local_count = fr_compile_count_local_binds(parsed);
  if (local_count > FR_PARSE_MAX_LOCALS) {
    return FR_ERR_CAPACITY;
  }
  if ((uint16_t)arity + local_count > FR_PROFILE_MAX_STACK_DEPTH) {
    return FR_ERR_INVALID;
  }
  if (local_count > 0) {
    header_size = FR_INSTRUCTION_LOCALS_HEADER_SIZE;
  } else if (arity > 0) {
    header_size = FR_INSTRUCTION_ARITY_HEADER_SIZE;
  }
  offset = header_size;
  if (ctx != NULL) {
    body_ctx = *ctx;
  }
  body_ctx.params = arity > 0 ? &parsed->params[function->param_start] : NULL;
  body_ctx.param_count = arity;
  body_ctx.definition_slot_id = definition_slot_id;
  body_ctx.definition_arity = arity;
  body_ctx.has_definition = true;
  body_ctx.body_texts = &body_texts;
  body_ctx.locals = &locals;
  body_ctx.event_body = &event_body;

  out->instruction_bytes[0] = FR_INSTRUCTION_FORMAT_VERSION;
  out->instruction_bytes[1] = header_size;
  if (header_size >= FR_INSTRUCTION_ARITY_HEADER_SIZE) {
    out->instruction_bytes[2] = arity;
  }
  if (header_size >= FR_INSTRUCTION_LOCALS_HEADER_SIZE) {
    out->instruction_bytes[3] = local_count;
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
  /* The `on`/`every`/`after` body rides in code_objects[1]; outer at [0]
   * keeps PUSH_CODE_ID's patch-time index aligned with the slot init's
   * REF_CODE_OBJECT index. */
  if (event_body.used) {
    out->code_objects_storage[0] = out->code_object;
    out->code_objects_storage[1] = out->event_body_object;
    out->overlay_update.code_objects = out->code_objects_storage;
    out->overlay_update.code_object_count = 2;
  }
  return FR_OK;
}

/* A fresh function name must resolve while its body compiles. The compiled
 * overlay update still owns the real install, so this binding is always
 * rolled back before compile returns. */
static fr_err_t fr_compile_bind_temporary_definition_slot(
    const fr_compile_context_t *ctx, const fr_slot_name_t *slot_name,
    bool has_slot_name) {
  if (!has_slot_name) {
    return FR_OK;
  }
  if (ctx == NULL || ctx->runtime == NULL || slot_name == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_slot_write(ctx->runtime, slot_name->slot_id, fr_tagged_nil()));
  return fr_slot_bind_project_name(ctx->runtime, slot_name->name,
                                   slot_name->slot_id);
}

static fr_err_t fr_compile_rollback_temporary_definition_slot(
    const fr_compile_context_t *ctx, const fr_slot_name_t *slot_name,
    bool has_slot_name) {
  if (!has_slot_name) {
    return FR_OK;
  }
  if (ctx == NULL || ctx->runtime == NULL || slot_name == NULL) {
    return FR_ERR_INVALID;
  }

  return fr_slot_rollback_project_name(ctx->runtime, slot_name->name,
                                       slot_name->slot_id);
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

  FR_TRY(fr_parse_line_with_diagnostic(source, &parsed,
                                       ctx != NULL ? ctx->diag : NULL));
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
    fr_err_t err = FR_OK;
    fr_err_t rollback_err = FR_OK;

    err = fr_compile_bind_temporary_definition_slot(ctx, &out->slot_name,
                                                    has_slot_name);
    if (err != FR_OK) {
      (void)fr_compile_rollback_temporary_definition_slot(
          ctx, &out->slot_name, has_slot_name);
      return err;
    }
    err = fr_compile_function(ctx, &parsed, value, slot_id, out);
    rollback_err = fr_compile_rollback_temporary_definition_slot(
        ctx, &out->slot_name, has_slot_name);
    if (err != FR_OK) {
      return err;
    }
    if (rollback_err != FR_OK) {
      return rollback_err;
    }
    out->slot_inits[0].slot_id = slot_id;
    fr_compile_attach_slot_name(out, has_slot_name);
    return FR_OK;
  }

#if FR_FEATURE_CELLS
  if (value->kind == FR_PARSE_EXPR_CELLS) {
    FR_TRY(fr_compile_require_source_feature(
        ctx, FR_COMPILE_SOURCE_CELLS, parsed.definition.name));
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
    uint16_t copied_length = 0;

    FR_TRY(fr_compile_require_source_feature(
        ctx, FR_COMPILE_SOURCE_TEXT,
        fr_compile_expr_diagnostic_span(&parsed, value)));
    if (!value->text_has_escapes &&
        value->text.length > FR_PROFILE_MAX_TEXT_LENGTH) {
      return FR_ERR_RANGE;
    }
    FR_TRY(fr_compile_copy_text_literal(value, out->text_bytes,
                                        FR_PROFILE_MAX_TEXT_LENGTH,
                                        &copied_length));
    out->text_object = (fr_image_text_object_t){
        .bytes = out->text_bytes,
        .length = copied_length,
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
  return fr_compile_overlay_update_for_runtime_with_diagnostic(runtime, source,
                                                               out, NULL);
}

fr_err_t fr_compile_overlay_update_for_runtime_with_diagnostic(
    fr_runtime_t *runtime, const char *source, fr_compile_overlay_update_t *out,
    fr_diagnostic_t *diag) {
  const fr_compile_context_t ctx = {
      .runtime = runtime,
      .diag = diag,
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
  return fr_compile_value_binding_for_runtime_with_diagnostic(runtime, source,
                                                              out, NULL);
}

fr_err_t fr_compile_value_binding_for_runtime_with_diagnostic(
    fr_runtime_t *runtime, const char *source, fr_compile_value_binding_t *out,
    fr_diagnostic_t *diag) {
  fr_compile_context_t ctx = {
      .runtime = runtime,
      .diag = diag,
  };
  fr_compile_locals_t locals = {0};
  fr_parse_line_t parsed = {0};
  const fr_parse_expr_t *value = NULL;
  uint8_t local_count = 0;
  uint8_t header_size = FR_INSTRUCTION_MIN_HEADER_SIZE;
  uint16_t offset = 0;

  if (runtime == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  memset(out, 0, sizeof(*out));

  FR_TRY(fr_parse_line_with_diagnostic(source, &parsed, ctx.diag));
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

  local_count = fr_compile_count_local_binds(&parsed);
  if (local_count > FR_PARSE_MAX_LOCALS) {
    return FR_ERR_CAPACITY;
  }
  if (local_count > FR_PROFILE_MAX_STACK_DEPTH) {
    return FR_ERR_INVALID;
  }
  if (local_count > 0) {
    header_size = FR_INSTRUCTION_LOCALS_HEADER_SIZE;
  }
  offset = header_size;
  ctx.locals = &locals;

  out->instruction_bytes[0] = FR_INSTRUCTION_FORMAT_VERSION;
  out->instruction_bytes[1] = header_size;
  if (header_size >= FR_INSTRUCTION_ARITY_HEADER_SIZE) {
    out->instruction_bytes[2] = 0;
  }
  if (header_size >= FR_INSTRUCTION_LOCALS_HEADER_SIZE) {
    out->instruction_bytes[3] = local_count;
  }
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
  fr_compile_context_t expr_ctx = {0};
  fr_compile_locals_t locals = {0};
  uint8_t local_count = 0;
  uint8_t header_size = FR_INSTRUCTION_MIN_HEADER_SIZE;
  uint16_t offset = 0;

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  memset(out, 0, sizeof(*out));

  FR_TRY(fr_parse_expression_line_with_diagnostic(
      source, &parsed, &expr_id, ctx != NULL ? ctx->diag : NULL));

  local_count = fr_compile_count_local_binds(&parsed);
  if (local_count > FR_PARSE_MAX_LOCALS) {
    return FR_ERR_CAPACITY;
  }
  if (local_count > FR_PROFILE_MAX_STACK_DEPTH) {
    return FR_ERR_INVALID;
  }
  if (local_count > 0) {
    header_size = FR_INSTRUCTION_LOCALS_HEADER_SIZE;
  }
  offset = header_size;
  if (ctx != NULL) {
    expr_ctx = *ctx;
  }
  expr_ctx.locals = &locals;

  out->instruction_bytes[0] = FR_INSTRUCTION_FORMAT_VERSION;
  out->instruction_bytes[1] = header_size;
  if (header_size >= FR_INSTRUCTION_ARITY_HEADER_SIZE) {
    out->instruction_bytes[2] = 0;
  }
  if (header_size >= FR_INSTRUCTION_LOCALS_HEADER_SIZE) {
    out->instruction_bytes[3] = local_count;
  }
  FR_TRY(fr_compile_emit_expr(&expr_ctx, &parsed, expr_id,
                              out->instruction_bytes, &offset));
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
  return fr_compile_expression_for_runtime_with_diagnostic(runtime, source, out,
                                                           NULL);
}

fr_err_t fr_compile_expression_for_runtime_with_diagnostic(
    fr_runtime_t *runtime, const char *source, fr_compile_expression_t *out,
    fr_diagnostic_t *diag) {
  const fr_compile_context_t ctx = {
      .runtime = runtime,
      .diag = diag,
  };

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  return fr_compile_expression_with_context(&ctx, source, out);
}
