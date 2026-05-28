#pragma once

#include "instruction.h"
#include "tagged.h"
#include "native.h"
#include "object.h"
#include "slot.h"
#include "types.h"

typedef enum fr_image_ref_kind_t {
  FR_IMAGE_REF_LITERAL_TAGGED,
  FR_IMAGE_REF_CODE_OBJECT,
  FR_IMAGE_REF_NATIVE,
  FR_IMAGE_REF_CELL_OBJECT,
  FR_IMAGE_REF_TEXT_OBJECT,
  FR_IMAGE_REF_RECORD_SHAPE_OBJECT,
  FR_IMAGE_REF_RECORD_OBJECT,
} fr_image_ref_kind_t;

typedef struct fr_image_ref_t {
  fr_image_ref_kind_t kind;
  fr_tagged_t literal_tagged;
  uint16_t index;
} fr_image_ref_t;

typedef struct fr_image_slot_init_t {
  fr_slot_id_t slot_id;
  fr_image_ref_t ref;
} fr_image_slot_init_t;

typedef struct fr_image_code_object_t {
  fr_instruction_stream_t instructions;
  const char *param_names;
  uint16_t param_names_length;
} fr_image_code_object_t;

typedef struct fr_image_cell_object_t {
  uint16_t length;
  const fr_tagged_t *initial_values;
} fr_image_cell_object_t;

typedef struct fr_image_text_object_t {
  const uint8_t *bytes;
  uint16_t length;
} fr_image_text_object_t;

typedef struct fr_image_record_shape_object_t {
  fr_record_name_t name;
  const fr_record_name_t *fields;
  uint16_t field_count;
} fr_image_record_shape_object_t;

typedef struct fr_image_record_object_t {
  fr_image_ref_t shape;
  const fr_image_ref_t *field_values;
  uint16_t field_count;
} fr_image_record_object_t;

typedef struct fr_image_native_t {
  fr_native_fn_t fn;
  uint8_t arity;
#if FR_FEATURE_NATIVE_SIGNATURES
  const fr_native_signature_t *signature;
#endif
} fr_image_native_t;

typedef struct fr_image_t {
  const fr_image_slot_init_t *slot_inits;
  uint16_t slot_init_count;
  const fr_image_code_object_t *code_objects;
  uint16_t code_object_count;
  const fr_image_cell_object_t *cell_objects;
  uint16_t cell_object_count;
  const fr_image_text_object_t *text_objects;
  uint16_t text_object_count;
  const fr_image_record_shape_object_t *record_shape_objects;
  uint16_t record_shape_object_count;
  const fr_image_record_object_t *record_objects;
  uint16_t record_object_count;
  const fr_image_native_t *natives;
  uint16_t native_count;
  const fr_slot_name_t *slot_names;
  uint16_t slot_name_count;
} fr_image_t;

/*
 * An overlay update is a partial record set applied over an installed base
 * image. It uses the same fields as an image record set, but it is not itself
 * an image.
 */
typedef struct fr_overlay_update_t {
  const fr_image_slot_init_t *slot_inits;
  uint16_t slot_init_count;
  const fr_image_code_object_t *code_objects;
  uint16_t code_object_count;
  const fr_image_cell_object_t *cell_objects;
  uint16_t cell_object_count;
  const fr_image_text_object_t *text_objects;
  uint16_t text_object_count;
  const fr_image_record_shape_object_t *record_shape_objects;
  uint16_t record_shape_object_count;
  const fr_image_record_object_t *record_objects;
  uint16_t record_object_count;
  const fr_image_native_t *natives;
  uint16_t native_count;
  const fr_slot_name_t *slot_names;
  uint16_t slot_name_count;
} fr_overlay_update_t;

typedef struct fr_overlay_update_decoded_t {
  fr_image_slot_init_t slot_inits[FR_PROFILE_MAX_OVERLAY_UPDATE_SLOT_INITS];
  fr_image_code_object_t
      code_objects[FR_PROFILE_MAX_OVERLAY_UPDATE_CODE_OBJECTS];
  fr_slot_name_t slot_names[FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES > 0
                                ? FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES
                                : 1];
  uint8_t instruction_bytes[FR_PROFILE_MAX_OVERLAY_UPDATE_INSTRUCTION_BYTES];
  char slot_name_text[FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES > 0
                          ? FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES
                          : 1]
                     [FR_PROFILE_MAX_NAME_BYTES + 1];
  fr_overlay_update_t update;
} fr_overlay_update_decoded_t;

fr_err_t fr_image_install(fr_runtime_t *runtime, const fr_image_t *image);

/* Apply an overlay update without clearing base slots, code objects, or natives. */
fr_err_t fr_overlay_apply(fr_runtime_t *runtime,
                          const fr_overlay_update_t *update);

fr_err_t fr_overlay_update_encode(const fr_overlay_update_t *update,
                                  uint8_t *bytes, uint16_t cap,
                                  uint16_t *out_length);
fr_err_t fr_overlay_update_decode(const uint8_t *bytes, uint16_t length,
                                  fr_overlay_update_decoded_t *out);
