#pragma once

#include "image.h"
#include "parse.h"

#include <stdbool.h>

typedef char fr_compile_assert_overlay_text_cap_matches_parse_body
    [(FR_PROFILE_MAX_OVERLAY_UPDATE_TEXT_OBJECTS == FR_PARSE_MAX_BODY_EXPRS)
         ? 1
         : -1];

enum {
  FR_COMPILE_MAX_INSTRUCTION_BYTES = FR_PROFILE_MAX_INSTRUCTION_BYTES,
  /* One function's packed param names: every arg at max token length plus its
   * NUL. The runtime pool is a shared cache, but this scratch holds one
   * signature, so it must fit the largest legal one or compile would reject it. */
  FR_COMPILE_MAX_PARAM_NAME_BYTES =
      FR_PARSE_MAX_PARAMS * (FR_PARSE_MAX_TOKEN_BYTES + 1),
};

typedef struct fr_compile_overlay_update_t {
  fr_image_slot_init_t slot_inits[1];
  fr_slot_name_t slot_name;
  fr_image_code_object_t code_object;
  /* Event-body code object emitted alongside an `on` statement. The outer
   * function gets index 0 in the overlay update; the event body, when
   * present, gets index 1. PUSH_CODE_ID resolves through that ordering. */
  fr_image_code_object_t event_body_object;
  fr_image_code_object_t code_objects_storage[2];
  fr_image_cell_object_t cell_object;
  fr_image_text_object_t text_object;
  fr_image_text_object_t text_objects[FR_PARSE_MAX_BODY_EXPRS];
  fr_image_record_shape_object_t record_shape_object;
  fr_image_record_object_t record_object;
  fr_record_name_t record_fields[FR_RECORD_FIELDS_PER_SHAPE_CAPACITY];
  fr_image_ref_t record_field_refs[FR_RECORD_FIELDS_PER_SHAPE_CAPACITY];
  uint8_t instruction_bytes[FR_COMPILE_MAX_INSTRUCTION_BYTES];
  uint8_t event_body_bytes[FR_COMPILE_MAX_INSTRUCTION_BYTES];
  char param_name_text[FR_COMPILE_MAX_PARAM_NAME_BYTES];
  uint8_t text_bytes[FR_PROFILE_MAX_TEXT_LENGTH > 0
                         ? FR_PROFILE_MAX_TEXT_LENGTH
                         : 1];
  /* Scratch for text bytes collected from one line, either record-field
   * defaults or function-body literals. Both paths feed text_objects[]. */
  uint8_t body_text_bytes[FR_PARSE_MAX_BODY_EXPRS]
                         [FR_PROFILE_MAX_TEXT_LENGTH > 0
                              ? FR_PROFILE_MAX_TEXT_LENGTH
                              : 1];
  char record_name_text[FR_PROFILE_MAX_NAME_BYTES + 1];
  char record_field_name_text[FR_RECORD_FIELDS_PER_SHAPE_CAPACITY]
                             [FR_PROFILE_MAX_NAME_BYTES + 1];
  char slot_name_text[FR_PROFILE_MAX_NAME_BYTES + 1];
  fr_overlay_update_t overlay_update;
} fr_compile_overlay_update_t;

typedef struct fr_compile_expression_t {
  uint8_t instruction_bytes[FR_COMPILE_MAX_INSTRUCTION_BYTES];
  fr_instruction_stream_t instructions;
} fr_compile_expression_t;

typedef struct fr_compile_value_binding_t {
  fr_slot_id_t slot_id;
  fr_slot_name_t slot_name;
  bool has_slot_name;
  char slot_name_text[FR_PROFILE_MAX_NAME_BYTES + 1];
  uint8_t instruction_bytes[FR_COMPILE_MAX_INSTRUCTION_BYTES];
  fr_instruction_stream_t instructions;
} fr_compile_value_binding_t;

/* Compile one source line into records that update the runtime overlay. */
fr_err_t fr_compile_overlay_update(const char *source,
                                   fr_compile_overlay_update_t *out);
fr_err_t fr_compile_overlay_update_for_runtime(fr_runtime_t *runtime,
                                               const char *source,
                                               fr_compile_overlay_update_t *out);
/* Compile one runtime-only binding, such as a call result assigned by `is`. */
fr_err_t fr_compile_value_binding_for_runtime(
    fr_runtime_t *runtime, const char *source, fr_compile_value_binding_t *out);
/* Compile one source expression into a temporary instruction stream. */
fr_err_t fr_compile_expression(const char *source,
                               fr_compile_expression_t *out);
fr_err_t fr_compile_expression_for_runtime(fr_runtime_t *runtime,
                                           const char *source,
                                           fr_compile_expression_t *out);
