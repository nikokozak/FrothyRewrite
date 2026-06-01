/*
 * An image is install-time data: code-object records, native records, and slot
 * initializers that resolve to runtime tagged words.
 */

#include "image.h"

#include "code.h"
#include "crc.h"
#include "handle.h"
#include "object.h"
#include "runtime.h"
#include "slot.h"

#include <string.h>

enum {
  FR_OVERLAY_UPDATE_VERSION = FR_PROFILE_OVERLAY_UPDATE_VERSION,
  FR_OVERLAY_UPDATE_RECORD_CODE = 1,
  FR_OVERLAY_UPDATE_RECORD_BIND = 2,
  FR_OVERLAY_UPDATE_RECORD_NAME = 3,
  FR_OVERLAY_UPDATE_RECORD_TEXT = 4,
  FR_OVERLAY_UPDATE_RECORD_EVENT = 5,
  FR_OVERLAY_UPDATE_RECORD_END = 0xff,
  FR_OVERLAY_UPDATE_VALUE_NIL = 0,
  FR_OVERLAY_UPDATE_VALUE_FALSE = 1,
  FR_OVERLAY_UPDATE_VALUE_TRUE = 2,
  FR_OVERLAY_UPDATE_VALUE_INT = 3,
  FR_OVERLAY_UPDATE_VALUE_CODE = 4,
  FR_OVERLAY_UPDATE_VALUE_NATIVE = 5,
};

static const uint8_t fr_overlay_update_magic[4] = {'F', 'R', 'O', 'U'};

typedef enum fr_image_apply_mode_t {
  FR_IMAGE_APPLY_BASE,
  FR_IMAGE_APPLY_OVERLAY,
} fr_image_apply_mode_t;

typedef struct fr_image_records_t {
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
} fr_image_records_t;

typedef struct fr_overlay_update_writer_t {
  uint8_t *bytes;
  uint16_t used;
  uint16_t cap;
} fr_overlay_update_writer_t;

typedef struct fr_overlay_update_reader_t {
  const uint8_t *bytes;
  uint16_t used;
  uint16_t offset;
} fr_overlay_update_reader_t;

static uint16_t fr_overlay_update_read_u16(const uint8_t *bytes) {
  return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void fr_overlay_update_write_u16(uint8_t *bytes, uint16_t value) {
  bytes[0] = (uint8_t)(value & 0xffu);
  bytes[1] = (uint8_t)(value >> 8);
}

static fr_err_t fr_overlay_update_writer_u8(fr_overlay_update_writer_t *writer,
                                            uint8_t value) {
  if (writer == NULL || writer->bytes == NULL) {
    return FR_ERR_INVALID;
  }
  if (writer->used + 1 > writer->cap) {
    return FR_ERR_CAPACITY;
  }

  writer->bytes[writer->used++] = value;
  return FR_OK;
}

static fr_err_t fr_overlay_update_writer_u16(fr_overlay_update_writer_t *writer,
                                             uint16_t value) {
  if (writer == NULL || writer->bytes == NULL) {
    return FR_ERR_INVALID;
  }
  if (writer->used + 2 > writer->cap) {
    return FR_ERR_CAPACITY;
  }

  fr_overlay_update_write_u16(&writer->bytes[writer->used], value);
  writer->used = (uint16_t)(writer->used + 2);
  return FR_OK;
}

static fr_err_t fr_overlay_update_writer_u32(fr_overlay_update_writer_t *writer,
                                             uint32_t value) {
  if (writer == NULL || writer->bytes == NULL) {
    return FR_ERR_INVALID;
  }
  if (writer->used + 4 > writer->cap) {
    return FR_ERR_CAPACITY;
  }

  fr_write_u32_le(&writer->bytes[writer->used], value);
  writer->used = (uint16_t)(writer->used + 4);
  return FR_OK;
}

static fr_err_t fr_overlay_update_writer_bytes(
    fr_overlay_update_writer_t *writer, const uint8_t *bytes, uint16_t length) {
  if (writer == NULL || writer->bytes == NULL ||
      (bytes == NULL && length > 0)) {
    return FR_ERR_INVALID;
  }
  if (writer->used + length > writer->cap) {
    return FR_ERR_CAPACITY;
  }

  if (length > 0) {
    memcpy(&writer->bytes[writer->used], bytes, length);
  }
  writer->used = (uint16_t)(writer->used + length);
  return FR_OK;
}

static fr_err_t fr_overlay_update_reader_u8(fr_overlay_update_reader_t *reader,
                                            uint8_t *out) {
  if (reader == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (reader->offset + 1 > reader->used) {
    return FR_ERR_CORRUPT;
  }

  *out = reader->bytes[reader->offset++];
  return FR_OK;
}

static fr_err_t fr_overlay_update_reader_u16(fr_overlay_update_reader_t *reader,
                                             uint16_t *out) {
  if (reader == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (reader->offset + 2 > reader->used) {
    return FR_ERR_CORRUPT;
  }

  *out = fr_overlay_update_read_u16(&reader->bytes[reader->offset]);
  reader->offset = (uint16_t)(reader->offset + 2);
  return FR_OK;
}

#if FR_WORD_SIZE == 32
static fr_err_t fr_overlay_update_reader_u32(fr_overlay_update_reader_t *reader,
                                             uint32_t *out) {
  if (reader == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (reader->offset + 4 > reader->used) {
    return FR_ERR_CORRUPT;
  }

  *out = fr_read_u32_le(&reader->bytes[reader->offset]);
  reader->offset = (uint16_t)(reader->offset + 4);
  return FR_OK;
}
#endif

static fr_err_t
fr_overlay_update_writer_int(fr_overlay_update_writer_t *writer,
                             fr_int_t value) {
#if FR_WORD_SIZE == 16
  return fr_overlay_update_writer_u16(writer, (uint16_t)(int16_t)value);
#else
  return fr_overlay_update_writer_u32(writer, (uint32_t)(int32_t)value);
#endif
}

static fr_err_t fr_overlay_update_reader_int(fr_overlay_update_reader_t *reader,
                                             fr_int_t *out) {
  if (out == NULL) {
    return FR_ERR_INVALID;
  }
#if FR_WORD_SIZE == 16
  {
    uint16_t word = 0;
    FR_TRY(fr_overlay_update_reader_u16(reader, &word));
    *out = (fr_int_t)(int16_t)word;
  }
#else
  {
    uint32_t word = 0;
    FR_TRY(fr_overlay_update_reader_u32(reader, &word));
    *out = (fr_int_t)(int32_t)word;
  }
#endif
  return fr_tagged_can_encode_int(*out) ? FR_OK : FR_ERR_CORRUPT;
}

static fr_err_t fr_overlay_update_reader_bytes(
    fr_overlay_update_reader_t *reader, uint16_t length,
    const uint8_t **out) {
  if (reader == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (reader->offset + length > reader->used) {
    return FR_ERR_CORRUPT;
  }

  *out = &reader->bytes[reader->offset];
  reader->offset = (uint16_t)(reader->offset + length);
  return FR_OK;
}

static fr_image_records_t fr_image_records_from_image(const fr_image_t *image) {
  return (fr_image_records_t){
      .slot_inits = image->slot_inits,
      .slot_init_count = image->slot_init_count,
      .code_objects = image->code_objects,
      .code_object_count = image->code_object_count,
      .cell_objects = image->cell_objects,
      .cell_object_count = image->cell_object_count,
      .text_objects = image->text_objects,
      .text_object_count = image->text_object_count,
      .record_shape_objects = image->record_shape_objects,
      .record_shape_object_count = image->record_shape_object_count,
      .record_objects = image->record_objects,
      .record_object_count = image->record_object_count,
      .natives = image->natives,
      .native_count = image->native_count,
      .slot_names = image->slot_names,
      .slot_name_count = image->slot_name_count,
  };
}

static fr_image_records_t
fr_image_records_from_overlay(const fr_overlay_update_t *update) {
  return (fr_image_records_t){
      .slot_inits = update->slot_inits,
      .slot_init_count = update->slot_init_count,
      .code_objects = update->code_objects,
      .code_object_count = update->code_object_count,
      .cell_objects = update->cell_objects,
      .cell_object_count = update->cell_object_count,
      .text_objects = update->text_objects,
      .text_object_count = update->text_object_count,
      .record_shape_objects = update->record_shape_objects,
      .record_shape_object_count = update->record_shape_object_count,
      .record_objects = update->record_objects,
      .record_object_count = update->record_object_count,
      .natives = update->natives,
      .native_count = update->native_count,
      .slot_names = update->slot_names,
      .slot_name_count = update->slot_name_count,
  };
}

static fr_err_t fr_image_check_tables(const fr_image_records_t *records) {
  if (records->slot_inits == NULL && records->slot_init_count > 0) {
    return FR_ERR_INVALID;
  }
  if (records->code_objects == NULL && records->code_object_count > 0) {
    return FR_ERR_INVALID;
  }
  if (records->cell_objects == NULL && records->cell_object_count > 0) {
    return FR_ERR_INVALID;
  }
  if (records->text_objects == NULL && records->text_object_count > 0) {
    return FR_ERR_INVALID;
  }
  if (records->record_shape_objects == NULL &&
      records->record_shape_object_count > 0) {
    return FR_ERR_INVALID;
  }
  if (records->record_objects == NULL && records->record_object_count > 0) {
    return FR_ERR_INVALID;
  }
#if !FR_FEATURE_CELLS
  if (records->cell_object_count > 0) {
    return FR_ERR_UNSUPPORTED;
  }
#endif
#if !FR_FEATURE_TEXT
  if (records->text_object_count > 0) {
    return FR_ERR_UNSUPPORTED;
  }
#endif
#if !FR_FEATURE_RECORDS
  if (records->record_shape_object_count > 0 ||
      records->record_object_count > 0) {
    return FR_ERR_UNSUPPORTED;
  }
#endif
  if (records->natives == NULL && records->native_count > 0) {
    return FR_ERR_INVALID;
  }
  if (records->slot_names == NULL && records->slot_name_count > 0) {
    return FR_ERR_INVALID;
  }
  if (records->code_object_count > FR_PROFILE_CODE_OBJECT_TABLE_SIZE ||
      (uint32_t)records->cell_object_count + records->text_object_count +
              records->record_shape_object_count + records->record_object_count >
          FR_PROFILE_OBJECT_TABLE_SIZE ||
      records->native_count > FR_PROFILE_NATIVE_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }
  return FR_OK;
}

static fr_err_t fr_image_check_ref(const fr_image_records_t *records,
                                   fr_image_ref_t ref) {
  fr_handle_ref_t handle_ref = {0};

  switch (ref.kind) {
  case FR_IMAGE_REF_LITERAL_TAGGED:
    if (fr_tagged_decode_handle_ref(ref.literal_tagged, &handle_ref) ==
        FR_OK) {
      return FR_ERR_VOLATILE;
    }
    return fr_tagged_is_valid(ref.literal_tagged) ? FR_OK : FR_ERR_INVALID;
  case FR_IMAGE_REF_CODE_OBJECT:
    return ref.index < records->code_object_count ? FR_OK : FR_ERR_NOT_FOUND;
  case FR_IMAGE_REF_NATIVE:
    return ref.index < records->native_count ? FR_OK : FR_ERR_NOT_FOUND;
  case FR_IMAGE_REF_CELL_OBJECT:
    return ref.index < records->cell_object_count ? FR_OK : FR_ERR_NOT_FOUND;
  case FR_IMAGE_REF_TEXT_OBJECT:
    return ref.index < records->text_object_count ? FR_OK : FR_ERR_NOT_FOUND;
  case FR_IMAGE_REF_RECORD_SHAPE_OBJECT:
    return ref.index < records->record_shape_object_count ? FR_OK
                                                          : FR_ERR_NOT_FOUND;
  case FR_IMAGE_REF_RECORD_OBJECT:
    return ref.index < records->record_object_count ? FR_OK
                                                    : FR_ERR_NOT_FOUND;
  default:
    return FR_ERR_INVALID;
  }
}

static fr_err_t
fr_image_check_native(const fr_image_native_t *native) {
#if FR_FEATURE_NATIVE_SIGNATURES
  const fr_native_signature_t *signature = NULL;
#endif

  if (native == NULL || native->fn == NULL) {
    return FR_ERR_INVALID;
  }
  if (native->arity > FR_PROFILE_MAX_STACK_DEPTH) {
    return FR_ERR_INVALID;
  }

#if FR_FEATURE_NATIVE_SIGNATURES
  signature = native->signature;
  if (signature == NULL) {
    return FR_OK;
  }
  if (signature->arg_count != native->arity) {
    return FR_ERR_INVALID;
  }
  if (signature->arg_count > 0 && signature->params == NULL) {
    return FR_ERR_INVALID;
  }
#endif
  return FR_OK;
}

#if FR_FEATURE_TEXT
static bool fr_image_text_record_same(const fr_image_text_object_t *lhs,
                                      const fr_image_text_object_t *rhs) {
  if (lhs == NULL || rhs == NULL || lhs->length != rhs->length) {
    return false;
  }
  if (lhs->length == 0) {
    return true;
  }
  if (lhs->bytes == NULL || rhs->bytes == NULL) {
    return false;
  }
  return memcmp(lhs->bytes, rhs->bytes, lhs->length) == 0;
}

static fr_err_t fr_image_count_new_text(const fr_runtime_t *runtime,
                                        const fr_image_records_t *records,
                                        uint16_t *out_new_objects,
                                        uint16_t *out_new_bytes) {
  uint32_t new_bytes = 0;
  uint16_t new_objects = 0;

  if (runtime == NULL || records == NULL || out_new_objects == NULL ||
      out_new_bytes == NULL) {
    return FR_ERR_INVALID;
  }

  for (uint16_t i = 0; i < records->text_object_count; i++) {
    fr_object_id_t ignored = 0;
    fr_err_t find_err = FR_OK;
    bool duplicate_in_records = false;

    find_err = fr_text_find(runtime, records->text_objects[i].bytes,
                            records->text_objects[i].length, 0, &ignored);
    if (find_err == FR_OK) {
      continue;
    }
    if (find_err != FR_ERR_NOT_FOUND) {
      return find_err;
    }

    for (uint16_t j = 0; j < i; j++) {
      if (fr_image_text_record_same(&records->text_objects[j],
                                    &records->text_objects[i])) {
        duplicate_in_records = true;
        break;
      }
    }
    if (duplicate_in_records) {
      continue;
    }

    if (new_objects == UINT16_MAX ||
        new_bytes + records->text_objects[i].length > UINT16_MAX) {
      return FR_ERR_CAPACITY;
    }
    new_objects = (uint16_t)(new_objects + 1);
    new_bytes += records->text_objects[i].length;
  }

  *out_new_objects = new_objects;
  *out_new_bytes = (uint16_t)new_bytes;
  return FR_OK;
}
#endif

#if FR_FEATURE_RECORDS
static bool fr_image_record_name_same(fr_record_name_t lhs,
                                      fr_record_name_t rhs) {
  if (lhs.length != rhs.length) {
    return false;
  }
  if (lhs.length == 0) {
    return true;
  }
  if (lhs.bytes == NULL || rhs.bytes == NULL) {
    return false;
  }
  return memcmp(lhs.bytes, rhs.bytes, lhs.length) == 0;
}

static bool fr_image_record_shape_same(
    const fr_image_record_shape_object_t *lhs,
    const fr_image_record_shape_object_t *rhs) {
  if (lhs == NULL || rhs == NULL || lhs->field_count != rhs->field_count ||
      !fr_image_record_name_same(lhs->name, rhs->name)) {
    return false;
  }
  if ((lhs->fields == NULL || rhs->fields == NULL) && lhs->field_count > 0) {
    return false;
  }
  for (uint16_t i = 0; i < lhs->field_count; i++) {
    if (!fr_image_record_name_same(lhs->fields[i], rhs->fields[i])) {
      return false;
    }
  }
  return true;
}

static fr_err_t fr_image_count_new_record_shapes(
    const fr_runtime_t *runtime, const fr_image_records_t *records,
    uint16_t *out_new_objects, uint16_t *out_new_name_entries,
    uint16_t *out_new_shape_fields, uint16_t *out_new_name_bytes) {
  uint16_t new_objects = 0;
  uint16_t new_name_entries = 0;
  uint16_t new_shape_fields = 0;
  uint16_t new_name_bytes = 0;

  if (runtime == NULL || records == NULL || out_new_objects == NULL ||
      out_new_name_entries == NULL || out_new_shape_fields == NULL ||
      out_new_name_bytes == NULL) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < records->record_shape_object_count; i++) {
    const fr_image_record_shape_object_t *shape =
        &records->record_shape_objects[i];
    fr_object_id_t ignored = 0;
    fr_err_t find_err = FR_OK;
    bool duplicate_in_records = false;
    uint32_t name_bytes = shape->name.length;

    find_err = fr_record_shape_find(runtime, shape->name, shape->fields,
                                    shape->field_count, 0, &ignored);
    if (find_err == FR_OK) {
      continue;
    }
    if (find_err != FR_ERR_NOT_FOUND) {
      return find_err;
    }
    for (uint16_t j = 0; j < i; j++) {
      if (fr_image_record_shape_same(&records->record_shape_objects[j],
                                     shape)) {
        duplicate_in_records = true;
        break;
      }
    }
    if (duplicate_in_records) {
      continue;
    }
    for (uint16_t j = 0; j < shape->field_count; j++) {
      name_bytes += shape->fields[j].length;
    }
    if ((uint32_t)new_name_bytes + name_bytes > UINT16_MAX ||
        (uint32_t)new_name_entries + 1u + shape->field_count > UINT16_MAX ||
        (uint32_t)new_shape_fields + shape->field_count > UINT16_MAX) {
      return FR_ERR_CAPACITY;
    }
    new_objects = (uint16_t)(new_objects + 1);
    new_name_entries =
        (uint16_t)(new_name_entries + 1u + shape->field_count);
    new_shape_fields = (uint16_t)(new_shape_fields + shape->field_count);
    new_name_bytes = (uint16_t)(new_name_bytes + name_bytes);
  }

  *out_new_objects = new_objects;
  *out_new_name_entries = new_name_entries;
  *out_new_shape_fields = new_shape_fields;
  *out_new_name_bytes = new_name_bytes;
  return FR_OK;
}

static fr_err_t fr_image_record_field_ref_allowed(
    const fr_runtime_t *runtime, const fr_image_records_t *records,
    fr_image_ref_t ref) {
  fr_int_t ignored_int = 0;
  fr_object_id_t object_id = 0;
  const uint8_t *ignored_bytes = NULL;
  uint16_t ignored_length = 0;

  FR_TRY(fr_image_check_ref(records, ref));
  if (ref.kind == FR_IMAGE_REF_TEXT_OBJECT) {
    return FR_OK;
  }
  if (ref.kind != FR_IMAGE_REF_LITERAL_TAGGED) {
    return FR_ERR_TYPE;
  }
  if (fr_tagged_is_nil(ref.literal_tagged) ||
      fr_tagged_is_bool(ref.literal_tagged)) {
    return FR_OK;
  }
  if (fr_tagged_decode_int(ref.literal_tagged, &ignored_int) == FR_OK) {
    return FR_OK;
  }
  if (fr_tagged_decode_object_id(ref.literal_tagged, &object_id) == FR_OK &&
      fr_text_view(runtime, object_id, &ignored_bytes, &ignored_length) ==
          FR_OK) {
    return FR_OK;
  }
  return FR_ERR_TYPE;
}
#endif

static fr_err_t fr_image_check_apply(const fr_runtime_t *runtime,
                                     const fr_image_records_t *records,
                                     fr_image_apply_mode_t mode) {
  uint32_t used_instruction_bytes = 0;
  uint32_t used_cell_words = 0;
  uint32_t used_record_values = 0;
  uint16_t new_text_objects = 0;
  uint16_t new_text_bytes = 0;
  uint16_t new_record_shape_objects = 0;
  uint16_t new_record_name_entries = 0;
  uint16_t new_record_shape_fields = 0;
  uint16_t new_record_name_bytes = 0;
  fr_slot_id_t slot_count_after_writes = 0;

  if (runtime == NULL || records == NULL) {
    return FR_ERR_INVALID;
  }
  if (mode == FR_IMAGE_APPLY_BASE && records->slot_name_count > 0) {
    return FR_ERR_INVALID;
  }
#if !FR_FEATURE_CELLS
  if (records->cell_object_count > 0) {
    return FR_ERR_UNSUPPORTED;
  }
#endif
#if !FR_FEATURE_TEXT
  if (records->text_object_count > 0) {
    return FR_ERR_UNSUPPORTED;
  }
#else
  FR_TRY(fr_image_count_new_text(runtime, records, &new_text_objects,
                                 &new_text_bytes));
#endif
#if !FR_FEATURE_RECORDS
  if (records->record_shape_object_count > 0 ||
      records->record_object_count > 0) {
    return FR_ERR_UNSUPPORTED;
  }
#else
  FR_TRY(fr_image_count_new_record_shapes(
      runtime, records, &new_record_shape_objects, &new_record_name_entries,
      &new_record_shape_fields, &new_record_name_bytes));
#endif
  if ((uint32_t)runtime->code.count + records->code_object_count >
          FR_PROFILE_CODE_OBJECT_TABLE_SIZE ||
      (uint32_t)runtime->objects.count + records->cell_object_count +
              new_text_objects + new_record_shape_objects +
              records->record_object_count >
          FR_PROFILE_OBJECT_TABLE_SIZE ||
      (uint32_t)runtime->natives.count + records->native_count >
          FR_PROFILE_NATIVE_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }

  used_instruction_bytes = runtime->code.used_instruction_bytes;
  for (uint16_t i = 0; i < records->code_object_count; i++) {
    const fr_instruction_stream_t *instructions =
        &records->code_objects[i].instructions;

    if (instructions->length == 0 || instructions->bytes == NULL) {
      return FR_ERR_INVALID;
    }
    if (used_instruction_bytes + instructions->length >
        FR_PROFILE_MAX_INSTRUCTION_BYTES) {
      return FR_ERR_CAPACITY;
    }
    used_instruction_bytes += instructions->length;
  }

#if FR_FEATURE_CELLS
  used_cell_words = runtime->objects.used_cell_words;
  for (uint16_t i = 0; i < records->cell_object_count; i++) {
    FR_TRY(fr_cells_check_install(runtime, records->cell_objects[i].length,
                                  records->cell_objects[i].initial_values));
    if (used_cell_words + records->cell_objects[i].length >
        FR_PROFILE_MAX_CELL_WORDS) {
      return FR_ERR_CAPACITY;
    }
    used_cell_words += records->cell_objects[i].length;
  }
#else
  (void)used_cell_words;
#endif

#if FR_FEATURE_TEXT
  if ((uint32_t)runtime->objects.used_text_bytes + new_text_bytes >
      FR_PROFILE_MAX_TEXT_BYTES) {
    return FR_ERR_CAPACITY;
  }
#else
  (void)new_text_objects;
  (void)new_text_bytes;
#endif

#if FR_FEATURE_RECORDS
  if ((uint32_t)runtime->objects.used_record_names +
              new_record_name_entries >
          FR_RECORD_NAME_ENTRY_CAPACITY ||
      (uint32_t)runtime->objects.used_record_shape_fields +
              new_record_shape_fields >
          FR_PROFILE_MAX_RECORD_SHAPE_FIELDS ||
      (uint32_t)runtime->objects.used_record_name_bytes +
              new_record_name_bytes >
          FR_PROFILE_MAX_RECORD_NAME_BYTES) {
    return FR_ERR_CAPACITY;
  }
  used_record_values = runtime->objects.used_record_values;
  for (uint16_t i = 0; i < records->record_shape_object_count; i++) {
    const fr_image_record_shape_object_t *shape =
        &records->record_shape_objects[i];
    fr_object_id_t ignored = 0;
    fr_err_t err = fr_record_shape_find(runtime, shape->name, shape->fields,
                                        shape->field_count, 0, &ignored);
    if (err != FR_OK && err != FR_ERR_NOT_FOUND) {
      return err;
    }
  }
  for (uint16_t i = 0; i < records->record_object_count; i++) {
    const fr_image_record_object_t *record = &records->record_objects[i];
    uint16_t shape_field_count = 0;

    FR_TRY(fr_image_check_ref(records, record->shape));
    if (record->field_values == NULL || record->field_count == 0 ||
        record->field_count > FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE) {
      return FR_ERR_INVALID;
    }
    if (record->shape.kind == FR_IMAGE_REF_RECORD_SHAPE_OBJECT) {
      if (record->shape.index >= records->record_shape_object_count) {
        return FR_ERR_NOT_FOUND;
      }
      shape_field_count =
          records->record_shape_objects[record->shape.index].field_count;
    } else if (record->shape.kind == FR_IMAGE_REF_LITERAL_TAGGED) {
      fr_object_id_t shape_object_id = 0;
      fr_record_name_t ignored_name = {0};

      FR_TRY(fr_tagged_decode_object_id(record->shape.literal_tagged,
                                        &shape_object_id));
      FR_TRY(fr_record_shape_view(runtime, shape_object_id, &ignored_name,
                                  &shape_field_count));
    } else {
      return FR_ERR_TYPE;
    }
    if (record->field_count != shape_field_count) {
      return FR_ERR_INVALID;
    }
    if (used_record_values + record->field_count >
        FR_PROFILE_MAX_RECORD_VALUE_FIELDS) {
      return FR_ERR_CAPACITY;
    }
    used_record_values += record->field_count;
    for (uint16_t j = 0; j < record->field_count; j++) {
      FR_TRY(fr_image_record_field_ref_allowed(runtime, records,
                                               record->field_values[j]));
    }
  }
#else
  (void)used_record_values;
  (void)new_record_shape_objects;
  (void)new_record_name_entries;
  (void)new_record_shape_fields;
  (void)new_record_name_bytes;
#endif

  for (uint16_t i = 0; i < records->native_count; i++) {
    FR_TRY(fr_image_check_native(&records->natives[i]));
  }

  slot_count_after_writes = fr_slot_count(runtime);
  for (uint16_t i = 0; i < records->slot_init_count; i++) {
    if (records->slot_inits[i].slot_id >= FR_PROFILE_MAX_SLOTS) {
      return FR_ERR_RANGE;
    }
    if (records->slot_inits[i].slot_id >= slot_count_after_writes) {
      slot_count_after_writes =
          (fr_slot_id_t)(records->slot_inits[i].slot_id + 1);
    }
    FR_TRY(fr_image_check_ref(records, records->slot_inits[i].ref));
  }

  if (mode == FR_IMAGE_APPLY_OVERLAY) {
    FR_TRY(fr_slot_validate_project_names(
        runtime, records->slot_names, records->slot_name_count,
        slot_count_after_writes));
  }

  return FR_OK;
}

static fr_err_t fr_image_resolve_ref(const fr_image_records_t *records,
                                     const fr_code_object_id_t code_ids[],
                                     const fr_object_id_t cell_ids[],
                                     const fr_object_id_t text_ids[],
                                     const fr_object_id_t record_shape_ids[],
                                     const fr_object_id_t record_ids[],
                                     const fr_native_id_t native_ids[],
                                     fr_image_ref_t ref,
                                     fr_tagged_t *out_tagged) {
  switch (ref.kind) {
  case FR_IMAGE_REF_LITERAL_TAGGED:
    if (!fr_tagged_is_valid(ref.literal_tagged)) {
      return FR_ERR_INVALID;
    }
    *out_tagged = ref.literal_tagged;
    return FR_OK;
  case FR_IMAGE_REF_CODE_OBJECT:
    if (ref.index >= records->code_object_count) {
      return FR_ERR_NOT_FOUND;
    }
    return fr_tagged_encode_code_object_id(code_ids[ref.index], out_tagged);
  case FR_IMAGE_REF_NATIVE:
    if (ref.index >= records->native_count) {
      return FR_ERR_NOT_FOUND;
    }
    return fr_tagged_encode_native_id(native_ids[ref.index], out_tagged);
  case FR_IMAGE_REF_CELL_OBJECT:
    if (ref.index >= records->cell_object_count) {
      return FR_ERR_NOT_FOUND;
    }
    return fr_tagged_encode_object_id(cell_ids[ref.index], out_tagged);
  case FR_IMAGE_REF_TEXT_OBJECT:
    if (ref.index >= records->text_object_count) {
      return FR_ERR_NOT_FOUND;
    }
    return fr_tagged_encode_object_id(text_ids[ref.index], out_tagged);
  case FR_IMAGE_REF_RECORD_SHAPE_OBJECT:
    if (ref.index >= records->record_shape_object_count) {
      return FR_ERR_NOT_FOUND;
    }
    return fr_tagged_encode_object_id(record_shape_ids[ref.index],
                                      out_tagged);
  case FR_IMAGE_REF_RECORD_OBJECT:
    if (ref.index >= records->record_object_count) {
      return FR_ERR_NOT_FOUND;
    }
    return fr_tagged_encode_object_id(record_ids[ref.index], out_tagged);
  default:
    return FR_ERR_INVALID;
  }
}

#if FR_FEATURE_TEXT
/* The caller owns writable storage for these instruction bytes (compile output
 * or decoded scratch) and passes the writable pointer in explicitly. We read
 * the operand through the instruction reader before writing the runtime id. */
static fr_err_t fr_image_patch_code_text_refs(
    uint8_t *bytes, uint16_t length, const fr_object_id_t text_ids[],
    uint16_t text_object_count) {
  fr_instruction_stream_t view;
  fr_instruction_header_t header = {0};
  fr_code_offset_t ip = 0;
  char scratch[64];

  if (bytes == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_instruction_stream_init(&view, bytes, length));
  FR_TRY(fr_instruction_read_header(&view, &header));
  ip = (fr_code_offset_t)header.header_size;
  while (ip < view.length) {
    fr_code_offset_t next_ip = 0;

    if ((fr_opcode_t)bytes[ip] == FR_OP_PUSH_OBJECT_ID) {
      fr_object_id_t local_index = 0;

      FR_TRY(fr_instruction_read_object_id_operand(&view, ip, &local_index));
      if (local_index >= text_object_count) {
        return FR_ERR_CORRUPT;
      }
      bytes[ip + 1] = (uint8_t)(text_ids[local_index] & 0xffu);
      bytes[ip + 2] = (uint8_t)(text_ids[local_index] >> 8);
    }
    FR_TRY(fr_instruction_disassemble_at(&view, ip, scratch,
                                         (uint16_t)sizeof(scratch), NULL,
                                         &next_ip));
    if (next_ip <= ip) {
      return FR_ERR_CORRUPT;
    }
    ip = next_ip;
  }
  return FR_OK;
}
#endif

/* PUSH_CODE_ID carries an overlay-local code index at compile time. We rewrite
 * it to the runtime code id before fr_code_install copies the bytes, mirroring
 * the text patch path. */
static fr_err_t fr_image_patch_code_id_refs(uint8_t *bytes, uint16_t length,
                                            const fr_code_object_id_t code_ids[],
                                            uint16_t code_object_count) {
  fr_instruction_stream_t view;
  fr_instruction_header_t header = {0};
  fr_code_offset_t ip = 0;
  char scratch[64];

  if (bytes == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_instruction_stream_init(&view, bytes, length));
  FR_TRY(fr_instruction_read_header(&view, &header));
  ip = (fr_code_offset_t)header.header_size;
  while (ip < view.length) {
    fr_code_offset_t next_ip = 0;

    if ((fr_opcode_t)bytes[ip] == FR_OP_PUSH_CODE_ID) {
      fr_code_object_id_t local_index = 0;

      FR_TRY(fr_instruction_read_code_id_operand(&view, ip, &local_index));
      if (local_index >= code_object_count) {
        return FR_ERR_CORRUPT;
      }
      bytes[ip + 1] = (uint8_t)(code_ids[local_index] & 0xffu);
      bytes[ip + 2] = (uint8_t)(code_ids[local_index] >> 8);
    }
    FR_TRY(fr_instruction_disassemble_at(&view, ip, scratch,
                                         (uint16_t)sizeof(scratch), NULL,
                                         &next_ip));
    if (next_ip <= ip) {
      return FR_ERR_CORRUPT;
    }
    ip = next_ip;
  }
  return FR_OK;
}

static fr_err_t fr_image_apply_to_runtime(fr_runtime_t *runtime,
                                          const fr_image_records_t *records,
                                          fr_image_apply_mode_t mode) {
  fr_code_object_id_t code_ids[FR_PROFILE_CODE_OBJECT_TABLE_SIZE];
  fr_object_id_t cell_ids[FR_OBJECT_TABLE_CAPACITY];
  fr_object_id_t text_ids[FR_OBJECT_TABLE_CAPACITY];
  fr_object_id_t record_shape_ids[FR_OBJECT_TABLE_CAPACITY];
  fr_object_id_t record_ids[FR_OBJECT_TABLE_CAPACITY];
  fr_native_id_t native_ids[FR_PROFILE_NATIVE_TABLE_SIZE];

#if FR_FEATURE_TEXT
  /* Text install runs first so PUSH_OBJECT_ID operands can be patched from
   * local indices to runtime ids before code install copies the bytes. */
  for (uint16_t i = 0; i < records->text_object_count; i++) {
    FR_TRY(fr_text_install(runtime, records->text_objects[i].bytes,
                           records->text_objects[i].length, &text_ids[i]));
  }
  if (records->text_object_count > 0) {
    for (uint16_t i = 0; i < records->code_object_count; i++) {
      /* Records views are const, but the compile output and decoded overlay
       * arenas under them are writable. Cast once at the call site. */
      uint8_t *writable = (uint8_t *)records->code_objects[i].instructions.bytes;

      FR_TRY(fr_image_patch_code_text_refs(
          writable, records->code_objects[i].instructions.length, text_ids,
          records->text_object_count));
    }
  }
#else
  (void)text_ids;
#endif
  /* fr_code_install assigns ids sequentially from runtime->code.count, so we
   * can pre-compute the ids and patch PUSH_CODE_ID operands before install
   * copies the bytes. */
  for (uint16_t i = 0; i < records->code_object_count; i++) {
    code_ids[i] = (fr_code_object_id_t)(runtime->code.count + i);
  }
  if (records->code_object_count > 1) {
    for (uint16_t i = 0; i < records->code_object_count; i++) {
      uint8_t *writable = (uint8_t *)records->code_objects[i].instructions.bytes;
      FR_TRY(fr_image_patch_code_id_refs(
          writable, records->code_objects[i].instructions.length, code_ids,
          records->code_object_count));
    }
  }
  for (uint16_t i = 0; i < records->code_object_count; i++) {
    fr_code_object_id_t installed_id = 0;
    FR_TRY(fr_code_install(runtime, &records->code_objects[i].instructions,
                           records->code_objects[i].param_names,
                           records->code_objects[i].param_names_length,
                           &installed_id));
    if (installed_id != code_ids[i]) {
      return FR_ERR_CORRUPT;
    }
  }
#if FR_FEATURE_RECORDS
  for (uint16_t i = 0; i < records->record_shape_object_count; i++) {
    FR_TRY(fr_record_shape_install(
        runtime, records->record_shape_objects[i].name,
        records->record_shape_objects[i].fields,
        records->record_shape_objects[i].field_count, &record_shape_ids[i]));
  }
  for (uint16_t i = 0; i < records->record_object_count; i++) {
    fr_tagged_t shape_tagged = 0;
    fr_object_id_t shape_object_id = 0;
    fr_tagged_t field_values[FR_RECORD_FIELDS_PER_SHAPE_CAPACITY];

    FR_TRY(fr_image_resolve_ref(records, code_ids, cell_ids, text_ids,
                                record_shape_ids, record_ids, native_ids,
                                records->record_objects[i].shape,
                                &shape_tagged));
    FR_TRY(fr_tagged_decode_object_id(shape_tagged, &shape_object_id));
    if (records->record_objects[i].field_count >
        FR_RECORD_FIELDS_PER_SHAPE_CAPACITY) {
      return FR_ERR_RANGE;
    }
    for (uint16_t j = 0; j < records->record_objects[i].field_count; j++) {
      FR_TRY(fr_image_resolve_ref(
          records, code_ids, cell_ids, text_ids, record_shape_ids, record_ids,
          native_ids, records->record_objects[i].field_values[j],
          &field_values[j]));
    }
    FR_TRY(fr_record_install(runtime, shape_object_id, field_values,
                             records->record_objects[i].field_count,
                             &record_ids[i]));
  }
#else
  (void)record_shape_ids;
  (void)record_ids;
#endif
#if FR_FEATURE_CELLS
  for (uint16_t i = 0; i < records->cell_object_count; i++) {
    FR_TRY(fr_cells_install(runtime, records->cell_objects[i].length,
                            records->cell_objects[i].initial_values,
                            &cell_ids[i]));
  }
#else
  (void)cell_ids;
#endif
  for (uint16_t i = 0; i < records->native_count; i++) {
    const fr_native_signature_t *signature = NULL;

#if FR_FEATURE_NATIVE_SIGNATURES
    signature = records->natives[i].signature;
#endif
    FR_TRY(fr_native_install(runtime, records->natives[i].fn,
                             records->natives[i].arity, signature,
                             &native_ids[i]));
  }
  for (uint16_t i = 0; i < records->slot_init_count; i++) {
    fr_tagged_t tagged = 0;

    FR_TRY(fr_image_resolve_ref(records, code_ids, cell_ids, text_ids,
                                record_shape_ids, record_ids, native_ids,
                                records->slot_inits[i].ref, &tagged));
    if (mode == FR_IMAGE_APPLY_BASE) {
      FR_TRY(fr_slot_set_base(runtime, records->slot_inits[i].slot_id, tagged));
    } else {
      FR_TRY(fr_slot_write(runtime, records->slot_inits[i].slot_id, tagged));
    }
  }
  if (mode == FR_IMAGE_APPLY_OVERLAY) {
    for (uint16_t i = 0; i < records->slot_name_count; i++) {
      FR_TRY(fr_slot_bind_project_name(runtime, records->slot_names[i].name,
                                       records->slot_names[i].slot_id));
    }
  }

  return FR_OK;
}

fr_err_t fr_image_install(fr_runtime_t *runtime, const fr_image_t *image) {
  fr_runtime_t next;
  fr_image_records_t records;

  if (runtime == NULL || image == NULL) {
    return FR_ERR_INVALID;
  }
  records = fr_image_records_from_image(image);
  FR_TRY(fr_image_check_tables(&records));
  FR_TRY(fr_runtime_init(&next));
  FR_TRY(fr_image_check_apply(&next, &records, FR_IMAGE_APPLY_BASE));
  FR_TRY(fr_image_apply_to_runtime(&next, &records, FR_IMAGE_APPLY_BASE));
  fr_code_mark_base(&next);
  fr_native_mark_base(&next);
  fr_object_mark_base(&next);

  *runtime = next;
  return FR_OK;
}

fr_err_t fr_overlay_apply(fr_runtime_t *runtime,
                          const fr_overlay_update_t *update) {
  fr_image_records_t records;

  if (runtime == NULL || update == NULL) {
    return FR_ERR_INVALID;
  }
  records = fr_image_records_from_overlay(update);
  FR_TRY(fr_image_check_tables(&records));
  FR_TRY(fr_image_check_apply(runtime, &records, FR_IMAGE_APPLY_OVERLAY));

  return fr_image_apply_to_runtime(runtime, &records, FR_IMAGE_APPLY_OVERLAY);
}

fr_err_t fr_overlay_apply_base(fr_runtime_t *runtime,
                               const fr_overlay_update_t *update) {
  fr_image_records_t records;

  if (runtime == NULL || update == NULL) {
    return FR_ERR_INVALID;
  }
  records = fr_image_records_from_overlay(update);
  records.slot_names = NULL;
  records.slot_name_count = 0;
  FR_TRY(fr_image_check_tables(&records));
  FR_TRY(fr_image_check_apply(runtime, &records, FR_IMAGE_APPLY_BASE));

  return fr_image_apply_to_runtime(runtime, &records, FR_IMAGE_APPLY_BASE);
}

static fr_err_t fr_overlay_update_encode_ref(
    fr_overlay_update_writer_t *writer, fr_image_ref_t ref) {
  fr_int_t raw_int = 0;
  fr_native_id_t native_id = 0;
  fr_handle_ref_t handle_ref = {0};

  switch (ref.kind) {
  case FR_IMAGE_REF_LITERAL_TAGGED:
    if (fr_tagged_decode_handle_ref(ref.literal_tagged, &handle_ref) ==
        FR_OK) {
      return FR_ERR_VOLATILE;
    }
    if (fr_tagged_is_nil(ref.literal_tagged)) {
      return fr_overlay_update_writer_u8(writer, FR_OVERLAY_UPDATE_VALUE_NIL);
    }
    if (fr_tagged_is_false(ref.literal_tagged)) {
      return fr_overlay_update_writer_u8(writer, FR_OVERLAY_UPDATE_VALUE_FALSE);
    }
    if (fr_tagged_is_true(ref.literal_tagged)) {
      return fr_overlay_update_writer_u8(writer, FR_OVERLAY_UPDATE_VALUE_TRUE);
    }
    if (fr_tagged_decode_int(ref.literal_tagged, &raw_int) == FR_OK) {
      FR_TRY(fr_overlay_update_writer_u8(writer, FR_OVERLAY_UPDATE_VALUE_INT));
      return fr_overlay_update_writer_int(writer, raw_int);
    }
    if (fr_tagged_decode_native_id(ref.literal_tagged, &native_id) == FR_OK) {
      FR_TRY(
          fr_overlay_update_writer_u8(writer, FR_OVERLAY_UPDATE_VALUE_NATIVE));
      return fr_overlay_update_writer_u16(writer, native_id);
    }
    return FR_ERR_UNSUPPORTED;
  case FR_IMAGE_REF_CODE_OBJECT:
    FR_TRY(fr_overlay_update_writer_u8(writer, FR_OVERLAY_UPDATE_VALUE_CODE));
    return fr_overlay_update_writer_u16(writer, ref.index);
  case FR_IMAGE_REF_NATIVE:
    return FR_ERR_UNSUPPORTED;
  case FR_IMAGE_REF_CELL_OBJECT:
    return FR_ERR_UNSUPPORTED;
  case FR_IMAGE_REF_TEXT_OBJECT:
    return FR_ERR_UNSUPPORTED;
  case FR_IMAGE_REF_RECORD_SHAPE_OBJECT:
  case FR_IMAGE_REF_RECORD_OBJECT:
    return FR_ERR_UNSUPPORTED;
  default:
    return FR_ERR_INVALID;
  }
}

static fr_err_t fr_overlay_update_decode_ref(
    fr_overlay_update_reader_t *reader, uint16_t code_object_count,
    fr_image_ref_t *out_ref) {
  uint8_t value_kind = 0;
  uint16_t value_word = 0;
  fr_int_t value_int = 0;
  fr_tagged_t tagged = 0;

  if (out_ref == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_overlay_update_reader_u8(reader, &value_kind));
  switch (value_kind) {
  case FR_OVERLAY_UPDATE_VALUE_NIL:
    *out_ref =
        (fr_image_ref_t){FR_IMAGE_REF_LITERAL_TAGGED, fr_tagged_nil(), 0};
    return FR_OK;
  case FR_OVERLAY_UPDATE_VALUE_FALSE:
    *out_ref =
        (fr_image_ref_t){FR_IMAGE_REF_LITERAL_TAGGED, fr_tagged_false(), 0};
    return FR_OK;
  case FR_OVERLAY_UPDATE_VALUE_TRUE:
    *out_ref =
        (fr_image_ref_t){FR_IMAGE_REF_LITERAL_TAGGED, fr_tagged_true(), 0};
    return FR_OK;
  case FR_OVERLAY_UPDATE_VALUE_INT:
    FR_TRY(fr_overlay_update_reader_int(reader, &value_int));
    FR_TRY(fr_tagged_encode_int(value_int, &tagged));
    *out_ref = (fr_image_ref_t){FR_IMAGE_REF_LITERAL_TAGGED, tagged, 0};
    return FR_OK;
  case FR_OVERLAY_UPDATE_VALUE_CODE:
    FR_TRY(fr_overlay_update_reader_u16(reader, &value_word));
    if (value_word >= code_object_count) {
      return FR_ERR_CORRUPT;
    }
    *out_ref = (fr_image_ref_t){FR_IMAGE_REF_CODE_OBJECT, 0, value_word};
    return FR_OK;
  case FR_OVERLAY_UPDATE_VALUE_NATIVE:
    FR_TRY(fr_overlay_update_reader_u16(reader, &value_word));
    FR_TRY(fr_tagged_encode_native_id(value_word, &tagged));
    *out_ref = (fr_image_ref_t){FR_IMAGE_REF_LITERAL_TAGGED, tagged, 0};
    return FR_OK;
  default:
    return FR_ERR_CORRUPT;
  }
}

fr_err_t fr_overlay_update_encode(const fr_overlay_update_t *update,
                                  uint8_t *bytes, uint16_t cap,
                                  uint16_t *out_length) {
  fr_overlay_update_writer_t writer = {bytes, 0, cap};
  fr_image_records_t records;

  if (update == NULL || bytes == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }
  records = fr_image_records_from_overlay(update);
  FR_TRY(fr_image_check_tables(&records));
  if (records.native_count > 0) {
    return FR_ERR_UNSUPPORTED;
  }
  if (records.cell_object_count > 0) {
    return FR_ERR_UNSUPPORTED;
  }
  if (records.record_shape_object_count > 0 || records.record_object_count > 0) {
    return FR_ERR_UNSUPPORTED;
  }
  if (records.slot_init_count > FR_PROFILE_MAX_OVERLAY_UPDATE_SLOT_INITS ||
      records.code_object_count > FR_PROFILE_MAX_OVERLAY_UPDATE_CODE_OBJECTS ||
      records.text_object_count > FR_PROFILE_MAX_OVERLAY_UPDATE_TEXT_OBJECTS ||
      records.slot_name_count > FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES) {
    return FR_ERR_CAPACITY;
  }

  FR_TRY(fr_overlay_update_writer_bytes(
      &writer, fr_overlay_update_magic,
      (uint16_t)sizeof(fr_overlay_update_magic)));
  FR_TRY(fr_overlay_update_writer_u8(&writer, FR_OVERLAY_UPDATE_VERSION));

  for (uint16_t i = 0; i < records.code_object_count; i++) {
    const fr_instruction_stream_t *instructions =
        &records.code_objects[i].instructions;
    uint32_t instruction_bytes_after =
        (uint32_t)records.code_objects[i].instructions.length;

    if (instructions->length == 0 || instructions->bytes == NULL) {
      return FR_ERR_INVALID;
    }
    for (uint16_t j = 0; j < i; j++) {
      instruction_bytes_after += records.code_objects[j].instructions.length;
    }

    if (instruction_bytes_after >
        FR_PROFILE_MAX_OVERLAY_UPDATE_INSTRUCTION_BYTES) {
      return FR_ERR_CAPACITY;
    }
    FR_TRY(fr_overlay_update_writer_u8(&writer,
                                       FR_OVERLAY_UPDATE_RECORD_CODE));
    FR_TRY(fr_overlay_update_writer_u16(&writer, i));
    FR_TRY(fr_overlay_update_writer_u16(&writer, instructions->length));
    FR_TRY(fr_overlay_update_writer_bytes(&writer, instructions->bytes,
                                          instructions->length));
  }

#if FR_FEATURE_TEXT
  for (uint16_t i = 0; i < records.text_object_count; i++) {
    const fr_image_text_object_t *text = &records.text_objects[i];

    if (text->length > FR_PROFILE_MAX_TEXT_LENGTH) {
      return FR_ERR_RANGE;
    }
    if (text->length > 0 && text->bytes == NULL) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_overlay_update_writer_u8(&writer,
                                       FR_OVERLAY_UPDATE_RECORD_TEXT));
    FR_TRY(fr_overlay_update_writer_u16(&writer, i));
    FR_TRY(fr_overlay_update_writer_u16(&writer, text->length));
    if (text->length > 0) {
      FR_TRY(fr_overlay_update_writer_bytes(&writer, text->bytes, text->length));
    }
  }
#endif

  for (uint16_t i = 0; i < records.slot_init_count; i++) {
    if (records.slot_inits[i].slot_id >= FR_PROFILE_MAX_SLOTS) {
      return FR_ERR_RANGE;
    }
    FR_TRY(fr_image_check_ref(&records, records.slot_inits[i].ref));
    FR_TRY(fr_overlay_update_writer_u8(&writer,
                                       FR_OVERLAY_UPDATE_RECORD_BIND));
    FR_TRY(fr_overlay_update_writer_u16(&writer, records.slot_inits[i].slot_id));
    FR_TRY(fr_overlay_update_encode_ref(&writer, records.slot_inits[i].ref));
  }

  for (uint16_t i = 0; i < records.slot_name_count; i++) {
    uint16_t name_len = 0;

    if (records.slot_names[i].slot_id >= FR_PROFILE_MAX_SLOTS ||
        records.slot_names[i].name == NULL) {
      return FR_ERR_INVALID;
    }
    name_len = (uint16_t)strlen(records.slot_names[i].name);
    if (name_len == 0 || name_len > FR_PROFILE_MAX_NAME_BYTES ||
        name_len > UINT8_MAX) {
      return FR_ERR_RANGE;
    }
    FR_TRY(
        fr_overlay_update_writer_u8(&writer, FR_OVERLAY_UPDATE_RECORD_NAME));
    FR_TRY(fr_overlay_update_writer_u16(&writer, records.slot_names[i].slot_id));
    FR_TRY(fr_overlay_update_writer_u8(&writer, (uint8_t)name_len));
    FR_TRY(fr_overlay_update_writer_bytes(
        &writer, (const uint8_t *)records.slot_names[i].name, name_len));
  }

  FR_TRY(fr_overlay_update_writer_u8(&writer, FR_OVERLAY_UPDATE_RECORD_END));
  FR_TRY(fr_overlay_update_writer_u32(&writer, fr_crc32(bytes, writer.used)));
  *out_length = writer.used;
  return FR_OK;
}

static fr_err_t fr_overlay_update_decode_code(
    fr_overlay_update_reader_t *reader, fr_overlay_update_decoded_t *out,
    uint16_t *instruction_bytes_used) {
  fr_image_code_object_t *code = NULL;
  fr_instruction_stream_t instructions;
  const uint8_t *instruction_bytes = NULL;
  uint16_t local_id = 0;
  uint16_t length = 0;

  if (reader == NULL || out == NULL || instruction_bytes_used == NULL) {
    return FR_ERR_INVALID;
  }
  if (out->update.code_object_count >=
      FR_PROFILE_MAX_OVERLAY_UPDATE_CODE_OBJECTS) {
    return FR_ERR_CAPACITY;
  }

  FR_TRY(fr_overlay_update_reader_u16(reader, &local_id));
  if (local_id != out->update.code_object_count) {
    return FR_ERR_CORRUPT;
  }
  FR_TRY(fr_overlay_update_reader_u16(reader, &length));
  if (length == 0) {
    return FR_ERR_CORRUPT;
  }
  if ((uint32_t)*instruction_bytes_used + length >
      FR_PROFILE_MAX_OVERLAY_UPDATE_INSTRUCTION_BYTES) {
    return FR_ERR_CAPACITY;
  }
  FR_TRY(fr_overlay_update_reader_bytes(reader, length, &instruction_bytes));
  FR_TRY(fr_instruction_stream_init(&instructions, instruction_bytes, length));

  memcpy(&out->instruction_bytes[*instruction_bytes_used], instruction_bytes,
         length);
  code = &out->code_objects[out->update.code_object_count];
  code->instructions =
      (fr_instruction_stream_t){.bytes =
                                    &out->instruction_bytes
                                         [*instruction_bytes_used],
                                .length = length};
  /* Persisted code carries no param names yet; the renderer falls back. */
  code->param_names = NULL;
  code->param_names_length = 0;
  *instruction_bytes_used = (uint16_t)(*instruction_bytes_used + length);
  out->update.code_object_count =
      (uint16_t)(out->update.code_object_count + 1);
  return FR_OK;
}

static fr_err_t
fr_overlay_update_decode_bind(fr_overlay_update_reader_t *reader,
                              fr_overlay_update_decoded_t *out) {
  fr_image_slot_init_t *slot_init = NULL;
  uint16_t slot_id = 0;

  if (reader == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (out->update.slot_init_count >=
      FR_PROFILE_MAX_OVERLAY_UPDATE_SLOT_INITS) {
    return FR_ERR_CAPACITY;
  }

  FR_TRY(fr_overlay_update_reader_u16(reader, &slot_id));
  if (slot_id >= FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_CORRUPT;
  }
  slot_init = &out->slot_inits[out->update.slot_init_count];
  slot_init->slot_id = (fr_slot_id_t)slot_id;
  FR_TRY(fr_overlay_update_decode_ref(
      reader, out->update.code_object_count, &slot_init->ref));
  out->update.slot_init_count =
      (uint16_t)(out->update.slot_init_count + 1);
  return FR_OK;
}

static fr_err_t
fr_overlay_update_decode_name(fr_overlay_update_reader_t *reader,
                              fr_overlay_update_decoded_t *out) {
#if FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES == 0
  (void)reader;
  (void)out;
  return FR_ERR_UNSUPPORTED;
#else
  fr_slot_name_t *slot_name = NULL;
  const uint8_t *name = NULL;
  uint16_t slot_id = 0;
  uint8_t name_len = 0;

  if (reader == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (out->update.slot_name_count >= FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES) {
    return FR_ERR_CAPACITY;
  }

  FR_TRY(fr_overlay_update_reader_u16(reader, &slot_id));
  if (slot_id >= FR_PROFILE_MAX_SLOTS) {
    return FR_ERR_CORRUPT;
  }
  FR_TRY(fr_overlay_update_reader_u8(reader, &name_len));
  if (name_len == 0 || name_len > FR_PROFILE_MAX_NAME_BYTES) {
    return FR_ERR_CORRUPT;
  }
  FR_TRY(fr_overlay_update_reader_bytes(reader, name_len, &name));
  for (uint8_t i = 0; i < name_len; i++) {
    if (name[i] == '\0') {
      return FR_ERR_CORRUPT;
    }
  }

  memcpy(out->slot_name_text[out->update.slot_name_count], name, name_len);
  out->slot_name_text[out->update.slot_name_count][name_len] = '\0';
  slot_name = &out->slot_names[out->update.slot_name_count];
  slot_name->slot_id = (fr_slot_id_t)slot_id;
  slot_name->name = out->slot_name_text[out->update.slot_name_count];
  out->update.slot_name_count =
      (uint16_t)(out->update.slot_name_count + 1);
  return FR_OK;
#endif
}

#if FR_FEATURE_TEXT
static fr_err_t
fr_overlay_update_decode_text(fr_overlay_update_reader_t *reader,
                              fr_overlay_update_decoded_t *out,
                              uint16_t *text_bytes_used) {
  const uint8_t *text_bytes = NULL;
  uint16_t local_id = 0;
  uint16_t length = 0;

  if (reader == NULL || out == NULL || text_bytes_used == NULL) {
    return FR_ERR_INVALID;
  }
  if (out->update.text_object_count >=
      FR_PROFILE_MAX_OVERLAY_UPDATE_TEXT_OBJECTS) {
    return FR_ERR_CAPACITY;
  }

  FR_TRY(fr_overlay_update_reader_u16(reader, &local_id));
  if (local_id != out->update.text_object_count) {
    return FR_ERR_CORRUPT;
  }
  FR_TRY(fr_overlay_update_reader_u16(reader, &length));
  if (length > FR_PROFILE_MAX_TEXT_LENGTH) {
    return FR_ERR_CORRUPT;
  }
  if ((uint32_t)*text_bytes_used + length >
      FR_PROFILE_MAX_OVERLAY_UPDATE_TEXT_BYTES) {
    return FR_ERR_CAPACITY;
  }
  if (length > 0) {
    FR_TRY(fr_overlay_update_reader_bytes(reader, length, &text_bytes));
    memcpy(&out->text_bytes[*text_bytes_used], text_bytes, length);
  }

  out->text_objects[out->update.text_object_count] = (fr_image_text_object_t){
      .bytes = length > 0 ? &out->text_bytes[*text_bytes_used] : NULL,
      .length = length,
  };
  *text_bytes_used = (uint16_t)(*text_bytes_used + length);
  out->update.text_object_count =
      (uint16_t)(out->update.text_object_count + 1);
  return FR_OK;
}
#endif

fr_err_t fr_overlay_update_decode(const uint8_t *bytes, uint16_t length,
                                  fr_overlay_update_decoded_t *out) {
  fr_overlay_update_reader_t reader = {bytes, length, 0};
  const uint8_t *magic = NULL;
  uint32_t stored_crc = 0;
  uint16_t instruction_bytes_used = 0;
#if FR_FEATURE_TEXT
  uint16_t text_bytes_used = 0;
#endif
  uint8_t version = 0;
  uint8_t record = 0;

  if (bytes == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (length < 4) {
    return FR_ERR_CORRUPT;
  }
  stored_crc = fr_read_u32_le(&bytes[length - 4u]);
  if (fr_crc32(bytes, (uint16_t)(length - 4u)) != stored_crc) {
    return FR_ERR_CORRUPT;
  }

  reader.used = (uint16_t)(length - 4u);
  memset(out, 0, sizeof(*out));
  out->update.slot_inits = out->slot_inits;
  out->update.code_objects = out->code_objects;
#if FR_FEATURE_TEXT
  out->update.text_objects = out->text_objects;
#endif
  out->update.slot_names = out->slot_names;

  FR_TRY(fr_overlay_update_reader_bytes(
      &reader, (uint16_t)sizeof(fr_overlay_update_magic), &magic));
  if (memcmp(magic, fr_overlay_update_magic,
             sizeof(fr_overlay_update_magic)) != 0) {
    return FR_ERR_CORRUPT;
  }
  FR_TRY(fr_overlay_update_reader_u8(&reader, &version));
  if (version != FR_OVERLAY_UPDATE_VERSION) {
    return FR_ERR_CORRUPT;
  }

  while (true) {
    FR_TRY(fr_overlay_update_reader_u8(&reader, &record));
    if (record == FR_OVERLAY_UPDATE_RECORD_END) {
      return reader.offset == reader.used ? FR_OK : FR_ERR_CORRUPT;
    }
    if (record == FR_OVERLAY_UPDATE_RECORD_CODE) {
      FR_TRY(fr_overlay_update_decode_code(&reader, out,
                                           &instruction_bytes_used));
    } else if (record == FR_OVERLAY_UPDATE_RECORD_BIND) {
      FR_TRY(fr_overlay_update_decode_bind(&reader, out));
    } else if (record == FR_OVERLAY_UPDATE_RECORD_NAME) {
      FR_TRY(fr_overlay_update_decode_name(&reader, out));
#if FR_FEATURE_TEXT
    } else if (record == FR_OVERLAY_UPDATE_RECORD_TEXT) {
      FR_TRY(fr_overlay_update_decode_text(&reader, out, &text_bytes_used));
#endif
    } else {
      return FR_ERR_CORRUPT;
    }
  }
}
