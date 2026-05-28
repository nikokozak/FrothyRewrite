#include "persist_payload.h"

#if !FR_FEATURE_PERSISTENCE
#error "persist_payload.c requires FR_FEATURE_PERSISTENCE"
#endif

#include "base_defs.h"
#include "code.h"
#include "instruction.h"
#include "object.h"
#include "slot.h"
#include "tagged.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum {
  /*
   * The 16-bit payload keeps version 0 for pre-width-change compatibility.
   * The 32-bit payload uses version 1 because int value fields are wider.
   * This is independent from the overlay update version in config.h.
   */
#if FR_WORD_SIZE == 16
  FR_PERSIST_PAYLOAD_VERSION = 0,
#else
  FR_PERSIST_PAYLOAD_VERSION = 1,
#endif
  FR_PERSIST_RECORD_CODE = 1,
  FR_PERSIST_RECORD_BIND = 2,
  FR_PERSIST_RECORD_NAME = 3,
  FR_PERSIST_RECORD_CELLS = 4,
  FR_PERSIST_RECORD_TEXT = 5,
  FR_PERSIST_RECORD_RECORD_SHAPE = 6,
  FR_PERSIST_RECORD_RECORD = 7,
  FR_PERSIST_RECORD_END = 0xff,
  FR_PERSIST_VALUE_NIL = 0,
  FR_PERSIST_VALUE_FALSE = 1,
  FR_PERSIST_VALUE_TRUE = 2,
  FR_PERSIST_VALUE_INT = 3,
  FR_PERSIST_VALUE_CODE = 4,
  FR_PERSIST_VALUE_NATIVE = 5,
  FR_PERSIST_VALUE_OBJECT = 6,
};

typedef enum fr_persist_object_record_kind_t {
  FR_PERSIST_OBJECT_NONE = 0,
  FR_PERSIST_OBJECT_CELLS = 1,
  FR_PERSIST_OBJECT_TEXT = 2,
  FR_PERSIST_OBJECT_RECORD_SHAPE = 3,
  FR_PERSIST_OBJECT_RECORD = 4,
} fr_persist_object_record_kind_t;

static const uint8_t fr_persist_payload_magic[4] = {'F', 'R', 'P', 'O'};

/* Writer/reader cursors stay uint16_t; this asserts the payload buffer fits. */
typedef char fr_persist_payload_bytes_must_fit_uint16[
    (FR_PROFILE_PERSISTENCE_BYTES <= UINT16_MAX) ? 1 : -1];

typedef struct fr_persist_writer_t {
  uint8_t *bytes;
  uint16_t used;
  uint16_t cap;
} fr_persist_writer_t;

typedef struct fr_persist_reader_t {
  const uint8_t *bytes;
  uint16_t used;
  uint16_t offset;
} fr_persist_reader_t;

typedef struct fr_persist_code_record_t {
  uint16_t local_id;
  const uint8_t *bytes;
  uint16_t length;
} fr_persist_code_record_t;

typedef struct fr_persist_bind_record_t {
  fr_slot_id_t slot_id;
  /* value_kind selects int_value for ints and value_word for compact refs. */
  uint8_t value_kind;
  fr_int_t int_value;
  uint16_t value_word;
} fr_persist_bind_record_t;

typedef struct fr_persist_cell_record_t {
  uint16_t local_id;
  uint16_t first_value;
  uint16_t length;
} fr_persist_cell_record_t;

typedef struct fr_persist_text_record_t {
  uint16_t local_id;
  const uint8_t *bytes;
  uint16_t length;
} fr_persist_text_record_t;

typedef struct fr_persist_record_shape_record_t {
  uint16_t local_id;
  fr_record_name_t name;
  uint16_t first_field;
  uint16_t field_count;
} fr_persist_record_shape_record_t;

typedef struct fr_persist_record_record_t {
  uint16_t local_id;
  uint16_t shape_local_id;
  uint16_t first_value;
  uint16_t field_count;
} fr_persist_record_record_t;

typedef struct fr_persist_name_record_t {
  fr_slot_id_t slot_id;
  char name[FR_PROFILE_MAX_NAME_BYTES + 1];
} fr_persist_name_record_t;

typedef struct fr_persist_decoded_payload_t {
  fr_persist_code_record_t code_records[FR_PROFILE_CODE_OBJECT_TABLE_SIZE];
  fr_persist_text_record_t text_records[FR_OBJECT_TABLE_CAPACITY];
  fr_persist_record_shape_record_t shape_records[FR_OBJECT_TABLE_CAPACITY];
  fr_record_name_t record_fields[FR_RECORD_SHAPE_FIELD_CAPACITY];
  fr_persist_record_record_t record_records[FR_OBJECT_TABLE_CAPACITY];
  fr_tagged_t record_values[FR_RECORD_VALUE_FIELD_CAPACITY];
  fr_persist_cell_record_t cell_records[FR_OBJECT_TABLE_CAPACITY];
  fr_tagged_t cell_values[FR_CELL_WORD_CAPACITY];
  fr_persist_object_record_kind_t object_kinds[FR_OBJECT_TABLE_CAPACITY];
  fr_persist_bind_record_t bind_records[FR_PROFILE_MAX_SLOTS];
  fr_persist_name_record_t name_records[FR_PROFILE_MAX_OVERLAY_NAMES > 0
                                             ? FR_PROFILE_MAX_OVERLAY_NAMES
                                             : 1];
  uint16_t code_count;
  uint16_t text_count;
  uint16_t shape_count;
  uint16_t record_field_count;
  uint16_t record_count;
  uint16_t record_value_count;
  uint16_t cell_count;
  uint16_t cell_value_count;
  uint16_t object_count;
  uint16_t bind_count;
  uint16_t name_count;
} fr_persist_decoded_payload_t;

static void
fr_persist_decoded_payload_reset(fr_persist_decoded_payload_t *decoded) {
  memset(&decoded->code_count, 0,
         sizeof(*decoded) -
             offsetof(fr_persist_decoded_payload_t, code_count));
}

static fr_err_t fr_persist_code_bytes_equal(
    const fr_runtime_t *runtime, fr_code_object_id_t lhs_id,
    fr_code_object_id_t rhs_id, bool *out_equal) {
  fr_instruction_stream_t lhs;
  fr_instruction_stream_t rhs;

  if (runtime == NULL || out_equal == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_code_get_instructions(runtime, lhs_id, &lhs));
  FR_TRY(fr_code_get_instructions(runtime, rhs_id, &rhs));
  if (lhs.length != rhs.length) {
    *out_equal = false;
    return FR_OK;
  }
  *out_equal = lhs.length == 0 || memcmp(lhs.bytes, rhs.bytes, lhs.length) == 0;
  return FR_OK;
}

static uint16_t fr_persist_payload_read_u16(const uint8_t *bytes) {
  return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void fr_persist_payload_write_u16(uint8_t *bytes, uint16_t value) {
  bytes[0] = (uint8_t)(value & 0xffu);
  bytes[1] = (uint8_t)(value >> 8);
}

static fr_err_t fr_persist_writer_u8(fr_persist_writer_t *writer,
                                     uint8_t value) {
  if (writer->used + 1 > writer->cap) {
    return FR_ERR_CAPACITY;
  }

  writer->bytes[writer->used++] = value;
  return FR_OK;
}

static fr_err_t fr_persist_writer_u16(fr_persist_writer_t *writer,
                                      uint16_t value) {
  if (writer->used + 2 > writer->cap) {
    return FR_ERR_CAPACITY;
  }

  fr_persist_payload_write_u16(&writer->bytes[writer->used], value);
  writer->used = (uint16_t)(writer->used + 2);
  return FR_OK;
}

#if FR_WORD_SIZE == 32
static fr_err_t fr_persist_writer_u32(fr_persist_writer_t *writer,
                                      uint32_t value) {
  if (writer->used + 4 > writer->cap) {
    return FR_ERR_CAPACITY;
  }

  fr_write_u32_le(&writer->bytes[writer->used], value);
  writer->used = (uint16_t)(writer->used + 4);
  return FR_OK;
}
#endif

static fr_err_t fr_persist_writer_int(fr_persist_writer_t *writer,
                                      fr_int_t value) {
#if FR_WORD_SIZE == 16
  return fr_persist_writer_u16(writer, (uint16_t)(int16_t)value);
#else
  return fr_persist_writer_u32(writer, (uint32_t)(int32_t)value);
#endif
}

static fr_err_t fr_persist_writer_bytes(fr_persist_writer_t *writer,
                                        const uint8_t *bytes,
                                        uint16_t length) {
  if ((bytes == NULL && length > 0) || writer->used + length > writer->cap) {
    return bytes == NULL ? FR_ERR_INVALID : FR_ERR_CAPACITY;
  }

  if (length > 0) {
    memcpy(&writer->bytes[writer->used], bytes, length);
  }
  writer->used = (uint16_t)(writer->used + length);
  return FR_OK;
}

#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
static fr_err_t fr_persist_name_length(const char *name,
                                       uint8_t *out_length) {
  uint16_t length = 0;

  if (name == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }
  while (length <= FR_PROFILE_MAX_NAME_BYTES && name[length] != '\0') {
    length += 1;
  }
  if (length == 0) {
    return FR_ERR_INVALID;
  }
  if (length > FR_PROFILE_MAX_NAME_BYTES) {
    return FR_ERR_RANGE;
  }

  *out_length = (uint8_t)length;
  return FR_OK;
}
#endif

static fr_err_t fr_persist_reader_u8(fr_persist_reader_t *reader,
                                     uint8_t *out) {
  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  if (reader->offset + 1 > reader->used) {
    return FR_ERR_CORRUPT;
  }

  *out = reader->bytes[reader->offset++];
  return FR_OK;
}

static fr_err_t fr_persist_reader_u16(fr_persist_reader_t *reader,
                                      uint16_t *out) {
  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  if (reader->offset + 2 > reader->used) {
    return FR_ERR_CORRUPT;
  }

  *out = fr_persist_payload_read_u16(&reader->bytes[reader->offset]);
  reader->offset = (uint16_t)(reader->offset + 2);
  return FR_OK;
}

#if FR_WORD_SIZE == 32
static fr_err_t fr_persist_reader_u32(fr_persist_reader_t *reader,
                                      uint32_t *out) {
  if (out == NULL) {
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

static fr_err_t fr_persist_reader_int(fr_persist_reader_t *reader,
                                      fr_int_t *out) {
  if (out == NULL) {
    return FR_ERR_INVALID;
  }
#if FR_WORD_SIZE == 16
  {
    uint16_t word = 0;
    FR_TRY(fr_persist_reader_u16(reader, &word));
    *out = (fr_int_t)(int16_t)word;
  }
#else
  {
    uint32_t word = 0;
    FR_TRY(fr_persist_reader_u32(reader, &word));
    *out = (fr_int_t)(int32_t)word;
  }
#endif
  return fr_tagged_can_encode_int(*out) ? FR_OK : FR_ERR_CORRUPT;
}

static fr_err_t fr_persist_reader_bytes(fr_persist_reader_t *reader,
                                        uint16_t length,
                                        const uint8_t **out) {
  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  if (reader->offset + length > reader->used) {
    return FR_ERR_CORRUPT;
  }

  *out = &reader->bytes[reader->offset];
  reader->offset = (uint16_t)(reader->offset + length);
  return FR_OK;
}

static fr_err_t fr_persist_encode_code_ref(
    fr_persist_writer_t *writer, const fr_runtime_t *runtime,
    fr_code_object_id_t runtime_code_id, fr_code_object_id_t runtime_code_ids[],
    uint16_t *code_count, uint16_t *out_local_id) {
  fr_instruction_stream_t instructions;

  FR_TRY(fr_code_get_instructions(runtime, runtime_code_id, &instructions));

  for (uint16_t i = 0; i < *code_count; i++) {
    bool same_code = false;

    if (runtime_code_ids[i] == runtime_code_id) {
      *out_local_id = i;
      return FR_OK;
    }
    FR_TRY(fr_persist_code_bytes_equal(runtime, runtime_code_ids[i],
                                       runtime_code_id, &same_code));
    if (same_code) {
      *out_local_id = i;
      return FR_OK;
    }
  }

  if (*code_count >= FR_PROFILE_CODE_OBJECT_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }
  FR_TRY(fr_persist_writer_u8(writer, FR_PERSIST_RECORD_CODE));
  FR_TRY(fr_persist_writer_u16(writer, *code_count));
  FR_TRY(fr_persist_writer_u16(writer, instructions.length));
  FR_TRY(fr_persist_writer_bytes(writer, instructions.bytes,
                                 instructions.length));

  runtime_code_ids[*code_count] = runtime_code_id;
  *out_local_id = *code_count;
  *code_count = (uint16_t)(*code_count + 1);
  return FR_OK;
}

static fr_err_t fr_persist_find_code_ref(
    const fr_runtime_t *runtime, fr_code_object_id_t runtime_code_id,
    const fr_code_object_id_t runtime_code_ids[], uint16_t code_count,
    uint16_t *out_local_id) {
  if (runtime == NULL || runtime_code_ids == NULL || out_local_id == NULL) {
    return FR_ERR_INVALID;
  }

  for (uint16_t i = 0; i < code_count; i++) {
    bool same_code = false;

    if (runtime_code_ids[i] == runtime_code_id) {
      *out_local_id = i;
      return FR_OK;
    }
    FR_TRY(fr_persist_code_bytes_equal(runtime, runtime_code_ids[i],
                                       runtime_code_id, &same_code));
    if (same_code) {
      *out_local_id = i;
      return FR_OK;
    }
  }

  return FR_ERR_CORRUPT;
}

static fr_err_t fr_persist_object_kind(const fr_runtime_t *runtime,
                                       fr_object_id_t runtime_object_id,
                                       fr_persist_object_record_kind_t *out) {
  const uint8_t *bytes = NULL;
  uint16_t length = 0;

  if (runtime == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_cells_length(runtime, runtime_object_id, &length) == FR_OK) {
    *out = FR_PERSIST_OBJECT_CELLS;
    return FR_OK;
  }
  if (fr_text_view(runtime, runtime_object_id, &bytes, &length) == FR_OK) {
    *out = FR_PERSIST_OBJECT_TEXT;
    return FR_OK;
  }
  if (fr_record_shape_view(runtime, runtime_object_id,
                           &(fr_record_name_t){0}, &length) == FR_OK) {
    *out = FR_PERSIST_OBJECT_RECORD_SHAPE;
    return FR_OK;
  }
  if (fr_record_view(runtime, runtime_object_id, &runtime_object_id,
                     &length) == FR_OK) {
    *out = FR_PERSIST_OBJECT_RECORD;
    return FR_OK;
  }
  return FR_ERR_TYPE;
}

#if FR_FEATURE_TEXT
static fr_err_t fr_persist_text_bytes_equal(
    const fr_runtime_t *runtime, fr_object_id_t lhs_id, fr_object_id_t rhs_id,
    bool *out_equal) {
  const uint8_t *lhs = NULL;
  const uint8_t *rhs = NULL;
  uint16_t lhs_length = 0;
  uint16_t rhs_length = 0;

  if (runtime == NULL || out_equal == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_text_view(runtime, lhs_id, &lhs, &lhs_length));
  FR_TRY(fr_text_view(runtime, rhs_id, &rhs, &rhs_length));
  if (lhs_length != rhs_length) {
    *out_equal = false;
    return FR_OK;
  }
  *out_equal =
      lhs_length == 0 || memcmp(lhs, rhs, lhs_length) == 0;
  return FR_OK;
}

static fr_err_t fr_persist_encode_text_ref(
    fr_persist_writer_t *writer, const fr_runtime_t *runtime,
    fr_object_id_t runtime_object_id, fr_object_id_t runtime_object_ids[],
    fr_persist_object_record_kind_t object_kinds[], uint16_t *object_count,
    uint16_t *out_local_id) {
  const uint8_t *bytes = NULL;
  uint16_t length = 0;

  if (writer == NULL || runtime == NULL || runtime_object_ids == NULL ||
      object_kinds == NULL || object_count == NULL || out_local_id == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_text_view(runtime, runtime_object_id, &bytes, &length));

  for (uint16_t i = 0; i < *object_count; i++) {
    bool same_text = false;

    if (object_kinds[i] != FR_PERSIST_OBJECT_TEXT) {
      continue;
    }
    if (runtime_object_ids[i] == runtime_object_id) {
      *out_local_id = i;
      return FR_OK;
    }
    FR_TRY(fr_persist_text_bytes_equal(runtime, runtime_object_ids[i],
                                       runtime_object_id, &same_text));
    if (same_text) {
      *out_local_id = i;
      return FR_OK;
    }
  }

  if (*object_count >= FR_PROFILE_OBJECT_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }
  FR_TRY(fr_persist_writer_u8(writer, FR_PERSIST_RECORD_TEXT));
  FR_TRY(fr_persist_writer_u16(writer, *object_count));
  FR_TRY(fr_persist_writer_u16(writer, length));
  FR_TRY(fr_persist_writer_bytes(writer, bytes, length));

  runtime_object_ids[*object_count] = runtime_object_id;
  object_kinds[*object_count] = FR_PERSIST_OBJECT_TEXT;
  *out_local_id = *object_count;
  *object_count = (uint16_t)(*object_count + 1);
  return FR_OK;
}
#endif

static fr_err_t fr_persist_find_object_ref(
    const fr_runtime_t *runtime, fr_object_id_t runtime_object_id,
    const fr_object_id_t runtime_object_ids[],
    const fr_persist_object_record_kind_t object_kinds[],
    uint16_t object_count, uint16_t *out_local_id);

#if FR_FEATURE_RECORDS
static bool fr_persist_record_name_same(fr_record_name_t lhs,
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

static fr_err_t fr_persist_record_shape_content_equal(
    const fr_runtime_t *runtime, fr_object_id_t lhs_id, fr_object_id_t rhs_id,
    bool *out_equal) {
  fr_record_name_t lhs_name = {0};
  fr_record_name_t rhs_name = {0};
  uint16_t lhs_count = 0;
  uint16_t rhs_count = 0;

  if (runtime == NULL || out_equal == NULL) {
    return FR_ERR_INVALID;
  }
  *out_equal = false;
  FR_TRY(fr_record_shape_view(runtime, lhs_id, &lhs_name, &lhs_count));
  FR_TRY(fr_record_shape_view(runtime, rhs_id, &rhs_name, &rhs_count));
  if (lhs_count != rhs_count ||
      !fr_persist_record_name_same(lhs_name, rhs_name)) {
    return FR_OK;
  }
  for (uint16_t i = 0; i < lhs_count; i++) {
    fr_record_name_t lhs_field = {0};
    fr_record_name_t rhs_field = {0};

    FR_TRY(fr_record_shape_field_name(runtime, lhs_id, i, &lhs_field));
    FR_TRY(fr_record_shape_field_name(runtime, rhs_id, i, &rhs_field));
    if (!fr_persist_record_name_same(lhs_field, rhs_field)) {
      return FR_OK;
    }
  }
  *out_equal = true;
  return FR_OK;
}

static fr_err_t fr_persist_find_record_shape_ref(
    const fr_runtime_t *runtime, fr_object_id_t runtime_object_id,
    const fr_object_id_t runtime_object_ids[],
    const fr_persist_object_record_kind_t object_kinds[],
    uint16_t object_count, uint16_t *out_local_id) {
  if (runtime == NULL || runtime_object_ids == NULL || object_kinds == NULL ||
      out_local_id == NULL) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < object_count; i++) {
    bool same_shape = false;

    if (object_kinds[i] != FR_PERSIST_OBJECT_RECORD_SHAPE) {
      continue;
    }
    if (runtime_object_ids[i] == runtime_object_id) {
      *out_local_id = i;
      return FR_OK;
    }
    FR_TRY(fr_persist_record_shape_content_equal(
        runtime, runtime_object_ids[i], runtime_object_id, &same_shape));
    if (same_shape) {
      *out_local_id = i;
      return FR_OK;
    }
  }
  return FR_ERR_NOT_FOUND;
}

static fr_err_t fr_persist_encode_record_shape_ref(
    fr_persist_writer_t *writer, const fr_runtime_t *runtime,
    fr_object_id_t runtime_object_id, fr_object_id_t runtime_object_ids[],
    fr_persist_object_record_kind_t object_kinds[], uint16_t *object_count,
    uint16_t *out_local_id) {
  fr_record_name_t shape_name = {0};
  uint16_t field_count = 0;
  fr_err_t find_err = FR_OK;

  if (writer == NULL || runtime == NULL || runtime_object_ids == NULL ||
      object_kinds == NULL || object_count == NULL || out_local_id == NULL) {
    return FR_ERR_INVALID;
  }
  find_err = fr_persist_find_record_shape_ref(
      runtime, runtime_object_id, runtime_object_ids, object_kinds,
      *object_count, out_local_id);
  if (find_err == FR_OK) {
    return FR_OK;
  }
  if (find_err != FR_ERR_NOT_FOUND) {
    return find_err;
  }
  if (*object_count >= FR_PROFILE_OBJECT_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }

  FR_TRY(fr_record_shape_view(runtime, runtime_object_id, &shape_name,
                              &field_count));
  FR_TRY(fr_persist_writer_u8(writer, FR_PERSIST_RECORD_RECORD_SHAPE));
  FR_TRY(fr_persist_writer_u16(writer, *object_count));
  FR_TRY(fr_persist_writer_u16(writer, shape_name.length));
  FR_TRY(fr_persist_writer_bytes(writer, shape_name.bytes, shape_name.length));
  FR_TRY(fr_persist_writer_u16(writer, field_count));
  for (uint16_t i = 0; i < field_count; i++) {
    fr_record_name_t field = {0};

    FR_TRY(fr_record_shape_field_name(runtime, runtime_object_id, i, &field));
    FR_TRY(fr_persist_writer_u16(writer, field.length));
    FR_TRY(fr_persist_writer_bytes(writer, field.bytes, field.length));
  }

  runtime_object_ids[*object_count] = runtime_object_id;
  object_kinds[*object_count] = FR_PERSIST_OBJECT_RECORD_SHAPE;
  *out_local_id = *object_count;
  *object_count = (uint16_t)(*object_count + 1);
  return FR_OK;
}

static fr_err_t fr_persist_encode_record_value(
    fr_persist_writer_t *writer, const fr_runtime_t *runtime,
    fr_tagged_t tagged, const fr_object_id_t runtime_object_ids[],
    const fr_persist_object_record_kind_t object_kinds[],
    uint16_t object_count) {
  fr_int_t raw_int = 0;
  fr_object_id_t object_id = 0;
  uint16_t local_object_id = 0;
  const uint8_t *ignored_bytes = NULL;
  uint16_t ignored_length = 0;

  if (fr_tagged_is_nil(tagged)) {
    return fr_persist_writer_u8(writer, FR_PERSIST_VALUE_NIL);
  }
  if (fr_tagged_is_false(tagged)) {
    return fr_persist_writer_u8(writer, FR_PERSIST_VALUE_FALSE);
  }
  if (fr_tagged_is_true(tagged)) {
    return fr_persist_writer_u8(writer, FR_PERSIST_VALUE_TRUE);
  }
  if (fr_tagged_decode_int(tagged, &raw_int) == FR_OK) {
    FR_TRY(fr_persist_writer_u8(writer, FR_PERSIST_VALUE_INT));
    return fr_persist_writer_int(writer, raw_int);
  }
  FR_TRY(fr_tagged_decode_object_id(tagged, &object_id));
  FR_TRY(fr_text_view(runtime, object_id, &ignored_bytes, &ignored_length));
  FR_TRY(fr_persist_find_object_ref(runtime, object_id, runtime_object_ids,
                                    object_kinds, object_count,
                                    &local_object_id));
  FR_TRY(fr_persist_writer_u8(writer, FR_PERSIST_VALUE_OBJECT));
  return fr_persist_writer_u16(writer, local_object_id);
}

static fr_err_t fr_persist_encode_record_ref(
    fr_persist_writer_t *writer, const fr_runtime_t *runtime,
    fr_object_id_t runtime_object_id, fr_object_id_t runtime_object_ids[],
    fr_persist_object_record_kind_t object_kinds[], uint16_t *object_count,
    uint16_t *out_local_id) {
  fr_object_id_t shape_object_id = 0;
  uint16_t field_count = 0;
  uint16_t shape_local_id = 0;

  if (writer == NULL || runtime == NULL || runtime_object_ids == NULL ||
      object_kinds == NULL || object_count == NULL || out_local_id == NULL) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < *object_count; i++) {
    if (object_kinds[i] == FR_PERSIST_OBJECT_RECORD &&
        runtime_object_ids[i] == runtime_object_id) {
      *out_local_id = i;
      return FR_OK;
    }
  }
  if (*object_count >= FR_PROFILE_OBJECT_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }
  FR_TRY(fr_record_view(runtime, runtime_object_id, &shape_object_id,
                        &field_count));
  FR_TRY(fr_persist_encode_record_shape_ref(
      writer, runtime, shape_object_id, runtime_object_ids, object_kinds,
      object_count, &shape_local_id));
  for (uint16_t i = 0; i < field_count; i++) {
    fr_tagged_t tagged = 0;
    fr_object_id_t object_id = 0;

    FR_TRY(fr_record_read_index(runtime, runtime_object_id, i, &tagged));
    if (fr_tagged_decode_object_id(tagged, &object_id) == FR_OK) {
      uint16_t ignored = 0;

      FR_TRY(fr_persist_encode_text_ref(writer, runtime, object_id,
                                        runtime_object_ids, object_kinds,
                                        object_count, &ignored));
    }
  }
  if (*object_count >= FR_PROFILE_OBJECT_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }
  FR_TRY(fr_persist_writer_u8(writer, FR_PERSIST_RECORD_RECORD));
  FR_TRY(fr_persist_writer_u16(writer, *object_count));
  FR_TRY(fr_persist_writer_u16(writer, shape_local_id));
  FR_TRY(fr_persist_writer_u16(writer, field_count));
  for (uint16_t i = 0; i < field_count; i++) {
    fr_tagged_t tagged = 0;

    FR_TRY(fr_record_read_index(runtime, runtime_object_id, i, &tagged));
    FR_TRY(fr_persist_encode_record_value(writer, runtime, tagged,
                                          runtime_object_ids, object_kinds,
                                          *object_count));
  }

  runtime_object_ids[*object_count] = runtime_object_id;
  object_kinds[*object_count] = FR_PERSIST_OBJECT_RECORD;
  *out_local_id = *object_count;
  *object_count = (uint16_t)(*object_count + 1);
  return FR_OK;
}
#endif

#if FR_FEATURE_CELLS
static fr_err_t fr_persist_encode_cell_value(fr_persist_writer_t *writer,
                                             const fr_runtime_t *runtime,
                                             fr_tagged_t tagged,
                                             const fr_object_id_t
                                                 runtime_object_ids[],
                                             const fr_persist_object_record_kind_t
                                                 object_kinds[],
                                             uint16_t object_count) {
  fr_int_t raw_int = 0;

  if (fr_tagged_is_nil(tagged)) {
    return fr_persist_writer_u8(writer, FR_PERSIST_VALUE_NIL);
  }
  if (fr_tagged_is_false(tagged)) {
    return fr_persist_writer_u8(writer, FR_PERSIST_VALUE_FALSE);
  }
  if (fr_tagged_is_true(tagged)) {
    return fr_persist_writer_u8(writer, FR_PERSIST_VALUE_TRUE);
  }
  if (fr_tagged_decode_int(tagged, &raw_int) == FR_OK) {
    FR_TRY(fr_persist_writer_u8(writer, FR_PERSIST_VALUE_INT));
    return fr_persist_writer_int(writer, raw_int);
  }
  {
    fr_object_id_t object_id = 0;
    uint16_t local_object_id = 0;
    fr_persist_object_record_kind_t kind = FR_PERSIST_OBJECT_NONE;

    if (fr_tagged_decode_object_id(tagged, &object_id) != FR_OK) {
      return FR_ERR_TYPE;
    }
    FR_TRY(fr_persist_object_kind(runtime, object_id, &kind));
    if (kind != FR_PERSIST_OBJECT_TEXT
#if FR_FEATURE_RECORDS
        && kind != FR_PERSIST_OBJECT_RECORD
#endif
    ) {
      return FR_ERR_TYPE;
    }
    FR_TRY(fr_persist_find_object_ref(runtime, object_id, runtime_object_ids,
                                      object_kinds, object_count,
                                      &local_object_id));
    FR_TRY(fr_persist_writer_u8(writer, FR_PERSIST_VALUE_OBJECT));
    return fr_persist_writer_u16(writer, local_object_id);
  }
}
#endif

static fr_err_t fr_persist_encode_cell_ref(
    fr_persist_writer_t *writer, const fr_runtime_t *runtime,
    fr_object_id_t runtime_object_id, fr_object_id_t runtime_object_ids[],
    fr_persist_object_record_kind_t object_kinds[], uint16_t *object_count,
    uint16_t *out_local_id) {
#if !FR_FEATURE_CELLS
  (void)writer;
  (void)runtime;
  (void)runtime_object_id;
  (void)runtime_object_ids;
  (void)object_kinds;
  (void)object_count;
  (void)out_local_id;
  return FR_ERR_UNSUPPORTED;
#else
  uint16_t length = 0;

  if (writer == NULL || runtime == NULL || runtime_object_ids == NULL ||
      object_kinds == NULL || object_count == NULL || out_local_id == NULL) {
    return FR_ERR_INVALID;
  }

  for (uint16_t i = 0; i < *object_count; i++) {
    if (object_kinds[i] == FR_PERSIST_OBJECT_CELLS &&
        runtime_object_ids[i] == runtime_object_id) {
      *out_local_id = i;
      return FR_OK;
    }
  }

  if (*object_count >= FR_PROFILE_OBJECT_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }
  FR_TRY(fr_cells_length(runtime, runtime_object_id, &length));

  for (uint16_t i = 0; i < length; i++) {
    fr_tagged_t tagged = 0;
    fr_object_id_t object_id = 0;

    FR_TRY(fr_cells_read(runtime, runtime_object_id, i, &tagged));
    if (fr_tagged_decode_object_id(tagged, &object_id) == FR_OK) {
      uint16_t ignored = 0;
      fr_persist_object_record_kind_t kind = FR_PERSIST_OBJECT_NONE;

#if !FR_FEATURE_TEXT && !FR_FEATURE_RECORDS
      (void)ignored;
#endif
      FR_TRY(fr_persist_object_kind(runtime, object_id, &kind));
      if (kind == FR_PERSIST_OBJECT_TEXT) {
#if !FR_FEATURE_TEXT
        return FR_ERR_TYPE;
#else
        FR_TRY(fr_persist_encode_text_ref(writer, runtime, object_id,
                                          runtime_object_ids, object_kinds,
                                          object_count, &ignored));
#endif
      } else if (kind == FR_PERSIST_OBJECT_RECORD) {
#if !FR_FEATURE_RECORDS
        return FR_ERR_TYPE;
#else
        FR_TRY(fr_persist_encode_record_ref(writer, runtime, object_id,
                                            runtime_object_ids, object_kinds,
                                            object_count, &ignored));
#endif
      } else {
        return FR_ERR_TYPE;
      }
    }
  }

  if (*object_count >= FR_PROFILE_OBJECT_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }
  FR_TRY(fr_persist_writer_u8(writer, FR_PERSIST_RECORD_CELLS));
  FR_TRY(fr_persist_writer_u16(writer, *object_count));
  FR_TRY(fr_persist_writer_u16(writer, length));
  for (uint16_t i = 0; i < length; i++) {
    fr_tagged_t tagged = 0;

    FR_TRY(fr_cells_read(runtime, runtime_object_id, i, &tagged));
    FR_TRY(fr_persist_encode_cell_value(writer, runtime, tagged,
                                        runtime_object_ids, object_kinds,
                                        *object_count));
  }

  runtime_object_ids[*object_count] = runtime_object_id;
  object_kinds[*object_count] = FR_PERSIST_OBJECT_CELLS;
  *out_local_id = *object_count;
  *object_count = (uint16_t)(*object_count + 1);
  return FR_OK;
#endif
}

static fr_err_t fr_persist_find_object_ref(
    const fr_runtime_t *runtime, fr_object_id_t runtime_object_id,
    const fr_object_id_t runtime_object_ids[],
    const fr_persist_object_record_kind_t object_kinds[],
    uint16_t object_count, uint16_t *out_local_id) {
  fr_persist_object_record_kind_t kind = FR_PERSIST_OBJECT_NONE;

  if (runtime == NULL || runtime_object_ids == NULL || object_kinds == NULL ||
      out_local_id == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_persist_object_kind(runtime, runtime_object_id, &kind));

  for (uint16_t i = 0; i < object_count; i++) {
    if (object_kinds[i] != kind) {
      continue;
    }
    if (runtime_object_ids[i] == runtime_object_id) {
      *out_local_id = i;
      return FR_OK;
    }
#if FR_FEATURE_TEXT
    if (kind == FR_PERSIST_OBJECT_TEXT) {
      bool same_text = false;

      FR_TRY(fr_persist_text_bytes_equal(runtime, runtime_object_ids[i],
                                         runtime_object_id, &same_text));
      if (same_text) {
        *out_local_id = i;
        return FR_OK;
      }
    }
#endif
#if FR_FEATURE_RECORDS
    if (kind == FR_PERSIST_OBJECT_RECORD_SHAPE) {
      bool same_shape = false;

      FR_TRY(fr_persist_record_shape_content_equal(
          runtime, runtime_object_ids[i], runtime_object_id, &same_shape));
      if (same_shape) {
        *out_local_id = i;
        return FR_OK;
      }
    }
#endif
  }
  return FR_ERR_CORRUPT;
}

static fr_err_t fr_persist_encode_value(
    fr_persist_writer_t *writer, const fr_runtime_t *runtime, fr_tagged_t tagged,
    fr_code_object_id_t runtime_code_ids[], uint16_t *code_count,
    const fr_object_id_t runtime_object_ids[],
    const fr_persist_object_record_kind_t object_kinds[],
    uint16_t object_count) {
  fr_int_t raw_int = 0;
  fr_code_object_id_t code_id = 0;
  fr_native_id_t native_id = 0;
  fr_object_id_t object_id = 0;
  fr_handle_ref_t handle_ref = {0};
  uint16_t local_code_id = 0;
  uint16_t local_object_id = 0;

  if (fr_tagged_is_nil(tagged)) {
    return fr_persist_writer_u8(writer, FR_PERSIST_VALUE_NIL);
  }
  if (fr_tagged_is_false(tagged)) {
    return fr_persist_writer_u8(writer, FR_PERSIST_VALUE_FALSE);
  }
  if (fr_tagged_is_true(tagged)) {
    return fr_persist_writer_u8(writer, FR_PERSIST_VALUE_TRUE);
  }
  if (fr_tagged_decode_int(tagged, &raw_int) == FR_OK) {
    FR_TRY(fr_persist_writer_u8(writer, FR_PERSIST_VALUE_INT));
    return fr_persist_writer_int(writer, raw_int);
  }
  if (fr_tagged_decode_code_object_id(tagged, &code_id) == FR_OK) {
    FR_TRY(fr_persist_find_code_ref(runtime, code_id, runtime_code_ids,
                                    *code_count, &local_code_id));
    FR_TRY(fr_persist_writer_u8(writer, FR_PERSIST_VALUE_CODE));
    return fr_persist_writer_u16(writer, local_code_id);
  }
  if (fr_tagged_decode_native_id(tagged, &native_id) == FR_OK) {
    if (native_id >= runtime->natives.base_count) {
      return FR_ERR_UNSUPPORTED;
    }
    FR_TRY(fr_persist_writer_u8(writer, FR_PERSIST_VALUE_NATIVE));
    return fr_persist_writer_u16(writer, native_id);
  }
  if (fr_tagged_decode_object_id(tagged, &object_id) == FR_OK) {
    FR_TRY(fr_persist_find_object_ref(runtime, object_id, runtime_object_ids,
                                      object_kinds, object_count,
                                      &local_object_id));
    FR_TRY(fr_persist_writer_u8(writer, FR_PERSIST_VALUE_OBJECT));
    return fr_persist_writer_u16(writer, local_object_id);
  }
  if (fr_tagged_decode_handle_ref(tagged, &handle_ref) == FR_OK) {
    return FR_ERR_VOLATILE;
  }

  return FR_ERR_UNSUPPORTED;
}

static fr_err_t fr_persist_check_no_volatile_handles(
    const fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

#if FR_FEATURE_HANDLES
  for (fr_slot_id_t slot_id = 0; slot_id < runtime->slots.count; slot_id++) {
    fr_handle_ref_t handle_ref = {0};

    if (!fr_slot_is_overlay(runtime, slot_id)) {
      continue;
    }
    if (fr_tagged_decode_handle_ref(runtime->slots.current[slot_id],
                                    &handle_ref) == FR_OK) {
      return FR_ERR_VOLATILE;
    }
  }
#endif
  return FR_OK;
}

fr_err_t fr_persist_payload_encode(const fr_runtime_t *runtime, uint8_t *bytes,
                                   uint16_t cap, uint16_t *out_length) {
  fr_persist_writer_t writer = {bytes, 0, cap};
  fr_code_object_id_t runtime_code_ids[FR_PROFILE_CODE_OBJECT_TABLE_SIZE];
  fr_object_id_t runtime_object_ids[FR_OBJECT_TABLE_CAPACITY];
  fr_persist_object_record_kind_t object_kinds[FR_OBJECT_TABLE_CAPACITY];
  uint16_t code_count = 0;
  uint16_t object_count = 0;

  if (runtime == NULL || bytes == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }

  memset(runtime_code_ids, 0, sizeof(runtime_code_ids));
  memset(runtime_object_ids, 0, sizeof(runtime_object_ids));
  memset(object_kinds, 0, sizeof(object_kinds));
  FR_TRY(fr_persist_check_no_volatile_handles(runtime));
  FR_TRY(fr_persist_writer_bytes(&writer, fr_persist_payload_magic,
                                 (uint16_t)sizeof(fr_persist_payload_magic)));
  FR_TRY(fr_persist_writer_u8(&writer, FR_PERSIST_PAYLOAD_VERSION));

  for (fr_slot_id_t slot_id = 0; slot_id < runtime->slots.count; slot_id++) {
    fr_code_object_id_t code_id = 0;
    uint16_t ignored = 0;

    if (!fr_slot_is_overlay(runtime, slot_id)) {
      continue;
    }
    if (fr_tagged_decode_code_object_id(runtime->slots.current[slot_id],
                                        &code_id) == FR_OK) {
      FR_TRY(fr_persist_encode_code_ref(&writer, runtime, code_id,
                                        runtime_code_ids, &code_count,
                                        &ignored));
    }
  }

  for (fr_slot_id_t slot_id = 0; slot_id < runtime->slots.count; slot_id++) {
    fr_object_id_t object_id = 0;
    uint16_t ignored = 0;

    if (!fr_slot_is_overlay(runtime, slot_id)) {
      continue;
    }
    if (fr_tagged_decode_object_id(runtime->slots.current[slot_id],
                                   &object_id) == FR_OK) {
      fr_persist_object_record_kind_t kind = FR_PERSIST_OBJECT_NONE;

      FR_TRY(fr_persist_object_kind(runtime, object_id, &kind));
      if (kind == FR_PERSIST_OBJECT_TEXT) {
#if !FR_FEATURE_TEXT
        return FR_ERR_UNSUPPORTED;
#else
        FR_TRY(fr_persist_encode_text_ref(&writer, runtime, object_id,
                                          runtime_object_ids, object_kinds,
                                          &object_count, &ignored));
#endif
      } else if (kind == FR_PERSIST_OBJECT_CELLS) {
        FR_TRY(fr_persist_encode_cell_ref(&writer, runtime, object_id,
                                          runtime_object_ids, object_kinds,
                                          &object_count, &ignored));
      } else if (kind == FR_PERSIST_OBJECT_RECORD_SHAPE) {
#if !FR_FEATURE_RECORDS
        return FR_ERR_UNSUPPORTED;
#else
        FR_TRY(fr_persist_encode_record_shape_ref(
            &writer, runtime, object_id, runtime_object_ids, object_kinds,
            &object_count, &ignored));
#endif
      } else if (kind == FR_PERSIST_OBJECT_RECORD) {
#if !FR_FEATURE_RECORDS
        return FR_ERR_UNSUPPORTED;
#else
        FR_TRY(fr_persist_encode_record_ref(&writer, runtime, object_id,
                                            runtime_object_ids, object_kinds,
                                            &object_count, &ignored));
#endif
      } else {
        return FR_ERR_TYPE;
      }
    }
  }

  for (fr_slot_id_t slot_id = 0; slot_id < runtime->slots.count; slot_id++) {
    if (!fr_slot_is_overlay(runtime, slot_id)) {
      continue;
    }

    FR_TRY(fr_persist_writer_u8(&writer, FR_PERSIST_RECORD_BIND));
    FR_TRY(fr_persist_writer_u16(&writer, slot_id));
    FR_TRY(fr_persist_encode_value(&writer, runtime,
                                   runtime->slots.current[slot_id],
                                   runtime_code_ids, &code_count,
                                   runtime_object_ids, object_kinds,
                                   object_count));
  }

#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  for (uint16_t i = 0; i < fr_slot_project_name_count(runtime); i++) {
    const char *name = fr_slot_project_name_at(runtime, i);
    fr_slot_id_t slot_id = 0;
    uint8_t name_length = 0;

    if (name == NULL) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_slot_id_for_name(runtime, name, &slot_id));
    FR_TRY(fr_persist_name_length(name, &name_length));

    FR_TRY(fr_persist_writer_u8(&writer, FR_PERSIST_RECORD_NAME));
    FR_TRY(fr_persist_writer_u16(&writer, slot_id));
    FR_TRY(fr_persist_writer_u8(&writer, name_length));
    FR_TRY(fr_persist_writer_bytes(&writer, (const uint8_t *)name,
                                   name_length));
  }
#endif

  FR_TRY(fr_persist_writer_u8(&writer, FR_PERSIST_RECORD_END));
  *out_length = writer.used;
  return FR_OK;
}

static fr_err_t fr_persist_decode_value(fr_persist_reader_t *reader,
                                        fr_persist_bind_record_t *bind) {
  FR_TRY(fr_persist_reader_u8(reader, &bind->value_kind));

  switch (bind->value_kind) {
  case FR_PERSIST_VALUE_NIL:
  case FR_PERSIST_VALUE_FALSE:
  case FR_PERSIST_VALUE_TRUE:
    bind->int_value = 0;
    bind->value_word = 0;
    return FR_OK;
  case FR_PERSIST_VALUE_INT:
    bind->int_value = 0;
    bind->value_word = 0;
    return fr_persist_reader_int(reader, &bind->int_value);
  case FR_PERSIST_VALUE_CODE:
  case FR_PERSIST_VALUE_NATIVE:
    bind->int_value = 0;
    bind->value_word = 0;
    return fr_persist_reader_u16(reader, &bind->value_word);
  case FR_PERSIST_VALUE_OBJECT:
#if FR_FEATURE_OBJECTS
    bind->int_value = 0;
    bind->value_word = 0;
    return fr_persist_reader_u16(reader, &bind->value_word);
#else
    return FR_ERR_UNSUPPORTED;
#endif
  default:
    return FR_ERR_CORRUPT;
  }
}

#if FR_FEATURE_CELLS
static fr_err_t fr_persist_decode_cell_value(
    fr_persist_reader_t *reader,
    const fr_persist_object_record_kind_t object_kinds[],
    uint16_t object_count, fr_tagged_t *out_tagged) {
  uint8_t value_kind = 0;
#if FR_FEATURE_TEXT
  uint16_t value_word = 0;
#endif
  fr_int_t value_int = 0;

  if (out_tagged == NULL || object_kinds == NULL) {
    return FR_ERR_INVALID;
  }
#if !FR_FEATURE_TEXT
  (void)object_count;
#endif

  FR_TRY(fr_persist_reader_u8(reader, &value_kind));
  switch (value_kind) {
  case FR_PERSIST_VALUE_NIL:
    *out_tagged = fr_tagged_nil();
    return FR_OK;
  case FR_PERSIST_VALUE_FALSE:
    *out_tagged = fr_tagged_false();
    return FR_OK;
  case FR_PERSIST_VALUE_TRUE:
    *out_tagged = fr_tagged_true();
    return FR_OK;
  case FR_PERSIST_VALUE_INT:
    FR_TRY(fr_persist_reader_int(reader, &value_int));
    return fr_tagged_encode_int(value_int, out_tagged);
  case FR_PERSIST_VALUE_OBJECT:
#if !FR_FEATURE_TEXT
    return FR_ERR_UNSUPPORTED;
#else
    FR_TRY(fr_persist_reader_u16(reader, &value_word));
    if (value_word >= object_count) {
      return FR_ERR_CORRUPT;
    }
    if (object_kinds[value_word] != FR_PERSIST_OBJECT_TEXT
#if FR_FEATURE_RECORDS
        && object_kinds[value_word] != FR_PERSIST_OBJECT_RECORD
#endif
    ) {
      return FR_ERR_CORRUPT;
    }
    return fr_tagged_encode_object_id(value_word, out_tagged);
#endif
  default:
    return FR_ERR_CORRUPT;
  }
}
#endif

#if FR_FEATURE_RECORDS
static fr_err_t fr_persist_decode_record_name(fr_persist_reader_t *reader,
                                              fr_record_name_t *out_name) {
  uint16_t length = 0;
  const uint8_t *bytes = NULL;

  if (reader == NULL || out_name == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_persist_reader_u16(reader, &length));
  if (length == 0 || length > FR_PROFILE_MAX_NAME_BYTES) {
    return FR_ERR_CORRUPT;
  }
  FR_TRY(fr_persist_reader_bytes(reader, length, &bytes));
  for (uint16_t i = 0; i < length; i++) {
    if (bytes[i] == '\0' || bytes[i] == '.') {
      return FR_ERR_CORRUPT;
    }
  }
  *out_name = (fr_record_name_t){.bytes = bytes, .length = length};
  return FR_OK;
}

static fr_err_t fr_persist_find_shape_record(
    const fr_persist_record_shape_record_t shape_records[],
    uint16_t shape_count, uint16_t local_id,
    const fr_persist_record_shape_record_t **out_shape) {
  if (shape_records == NULL || out_shape == NULL) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < shape_count; i++) {
    if (shape_records[i].local_id == local_id) {
      *out_shape = &shape_records[i];
      return FR_OK;
    }
  }
  return FR_ERR_CORRUPT;
}

static fr_err_t fr_persist_decode_record_value(
    fr_persist_reader_t *reader,
    const fr_persist_object_record_kind_t object_kinds[],
    uint16_t object_count, fr_tagged_t *out_tagged) {
  uint8_t value_kind = 0;
  uint16_t value_word = 0;
  fr_int_t value_int = 0;

  if (reader == NULL || object_kinds == NULL || out_tagged == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_persist_reader_u8(reader, &value_kind));
  switch (value_kind) {
  case FR_PERSIST_VALUE_NIL:
    *out_tagged = fr_tagged_nil();
    return FR_OK;
  case FR_PERSIST_VALUE_FALSE:
    *out_tagged = fr_tagged_false();
    return FR_OK;
  case FR_PERSIST_VALUE_TRUE:
    *out_tagged = fr_tagged_true();
    return FR_OK;
  case FR_PERSIST_VALUE_INT:
    FR_TRY(fr_persist_reader_int(reader, &value_int));
    return fr_tagged_encode_int(value_int, out_tagged);
  case FR_PERSIST_VALUE_OBJECT:
    FR_TRY(fr_persist_reader_u16(reader, &value_word));
    if (value_word >= object_count ||
        object_kinds[value_word] != FR_PERSIST_OBJECT_TEXT) {
      return FR_ERR_CORRUPT;
    }
    return fr_tagged_encode_object_id(value_word, out_tagged);
  default:
    return FR_ERR_CORRUPT;
  }
}
#endif

static fr_err_t fr_persist_decode_payload(
    const uint8_t *payload, uint16_t payload_length,
    fr_persist_decoded_payload_t *decoded) {
  fr_persist_reader_t reader = {payload, payload_length, 0};
  const uint8_t *magic = NULL;
  uint8_t version = 0;
  uint8_t record = 0;
  fr_persist_code_record_t *code_records = NULL;
  fr_persist_text_record_t *text_records = NULL;
  fr_persist_record_shape_record_t *shape_records = NULL;
  fr_record_name_t *record_fields = NULL;
  fr_persist_record_record_t *record_records = NULL;
  fr_tagged_t *record_values = NULL;
  fr_persist_cell_record_t *cell_records = NULL;
  fr_tagged_t *cell_values = NULL;
  fr_persist_object_record_kind_t *object_kinds = NULL;
  fr_persist_bind_record_t *bind_records = NULL;
  fr_persist_name_record_t *name_records = NULL;
  uint16_t *out_code_count = NULL;
  uint16_t *out_text_count = NULL;
  uint16_t *out_shape_count = NULL;
  uint16_t *out_record_field_count = NULL;
  uint16_t *out_record_count = NULL;
  uint16_t *out_record_value_count = NULL;
  uint16_t *out_cell_count = NULL;
  uint16_t *out_cell_value_count = NULL;
  uint16_t *out_object_count = NULL;
  uint16_t *out_bind_count = NULL;
  uint16_t *out_name_count = NULL;

  if (payload == NULL || decoded == NULL) {
    return FR_ERR_INVALID;
  }

  fr_persist_decoded_payload_reset(decoded);
  code_records = decoded->code_records;
  text_records = decoded->text_records;
  shape_records = decoded->shape_records;
  record_fields = decoded->record_fields;
  record_records = decoded->record_records;
  record_values = decoded->record_values;
  cell_records = decoded->cell_records;
  cell_values = decoded->cell_values;
  object_kinds = decoded->object_kinds;
  bind_records = decoded->bind_records;
  name_records = decoded->name_records;
  out_code_count = &decoded->code_count;
  out_text_count = &decoded->text_count;
  out_shape_count = &decoded->shape_count;
  out_record_field_count = &decoded->record_field_count;
  out_record_count = &decoded->record_count;
  out_record_value_count = &decoded->record_value_count;
  out_cell_count = &decoded->cell_count;
  out_cell_value_count = &decoded->cell_value_count;
  out_object_count = &decoded->object_count;
  out_bind_count = &decoded->bind_count;
  out_name_count = &decoded->name_count;
#if !FR_FEATURE_TEXT
  (void)text_records;
  (void)out_text_count;
#endif
#if !FR_FEATURE_RECORDS
  (void)shape_records;
  (void)record_fields;
  (void)record_records;
  (void)record_values;
  (void)out_shape_count;
  (void)out_record_field_count;
  (void)out_record_count;
  (void)out_record_value_count;
#endif
#if !FR_FEATURE_CELLS
  (void)cell_records;
  (void)cell_values;
  (void)out_cell_count;
  (void)out_cell_value_count;
#endif
#if !FR_FEATURE_OBJECTS
  (void)object_kinds;
  (void)out_object_count;
#endif
#if FR_PROFILE_MAX_OVERLAY_NAMES == 0
  (void)name_records;
  (void)out_name_count;
#endif
  FR_TRY(fr_persist_reader_bytes(&reader,
                                 (uint16_t)sizeof(fr_persist_payload_magic),
                                 &magic));
  if (memcmp(magic, fr_persist_payload_magic,
             sizeof(fr_persist_payload_magic)) != 0) {
    return FR_ERR_CORRUPT;
  }
  FR_TRY(fr_persist_reader_u8(&reader, &version));
  if (version != FR_PERSIST_PAYLOAD_VERSION) {
    return FR_ERR_CORRUPT;
  }

  while (true) {
    FR_TRY(fr_persist_reader_u8(&reader, &record));
    if (record == FR_PERSIST_RECORD_END) {
      return reader.offset == reader.used ? FR_OK : FR_ERR_CORRUPT;
    }
    if (record == FR_PERSIST_RECORD_CODE) {
      fr_persist_code_record_t *code = NULL;
      fr_instruction_stream_t instructions;

      if (*out_code_count >= FR_PROFILE_CODE_OBJECT_TABLE_SIZE) {
        return FR_ERR_CAPACITY;
      }
      code = &code_records[*out_code_count];
      FR_TRY(fr_persist_reader_u16(&reader, &code->local_id));
      if (code->local_id != *out_code_count) {
        return FR_ERR_CORRUPT;
      }
      FR_TRY(fr_persist_reader_u16(&reader, &code->length));
      FR_TRY(fr_persist_reader_bytes(&reader, code->length, &code->bytes));
      FR_TRY(fr_instruction_stream_init(&instructions, code->bytes,
                                        code->length));
      *out_code_count = (uint16_t)(*out_code_count + 1);
    } else if (record == FR_PERSIST_RECORD_TEXT) {
#if !FR_FEATURE_TEXT
      return FR_ERR_UNSUPPORTED;
#else
      fr_persist_text_record_t *text = NULL;

      if (*out_text_count >= FR_PROFILE_OBJECT_TABLE_SIZE ||
          *out_object_count >= FR_PROFILE_OBJECT_TABLE_SIZE) {
        return FR_ERR_CAPACITY;
      }
      text = &text_records[*out_text_count];
      FR_TRY(fr_persist_reader_u16(&reader, &text->local_id));
      if (text->local_id != *out_object_count) {
        return FR_ERR_CORRUPT;
      }
      FR_TRY(fr_persist_reader_u16(&reader, &text->length));
      if (text->length > FR_PROFILE_MAX_TEXT_LENGTH) {
        return FR_ERR_CORRUPT;
      }
      FR_TRY(fr_persist_reader_bytes(&reader, text->length, &text->bytes));
      object_kinds[*out_object_count] = FR_PERSIST_OBJECT_TEXT;
      *out_text_count = (uint16_t)(*out_text_count + 1);
      *out_object_count = (uint16_t)(*out_object_count + 1);
#endif
    } else if (record == FR_PERSIST_RECORD_RECORD_SHAPE) {
#if !FR_FEATURE_RECORDS
      return FR_ERR_UNSUPPORTED;
#else
      fr_persist_record_shape_record_t *shape = NULL;

      if (*out_shape_count >= FR_PROFILE_OBJECT_TABLE_SIZE ||
          *out_object_count >= FR_PROFILE_OBJECT_TABLE_SIZE) {
        return FR_ERR_CAPACITY;
      }
      shape = &shape_records[*out_shape_count];
      FR_TRY(fr_persist_reader_u16(&reader, &shape->local_id));
      if (shape->local_id != *out_object_count) {
        return FR_ERR_CORRUPT;
      }
      FR_TRY(fr_persist_decode_record_name(&reader, &shape->name));
      FR_TRY(fr_persist_reader_u16(&reader, &shape->field_count));
      if (shape->field_count == 0 ||
          shape->field_count > FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE) {
        return FR_ERR_CORRUPT;
      }
      if ((uint32_t)*out_record_field_count + shape->field_count >
          FR_RECORD_SHAPE_FIELD_CAPACITY) {
        return FR_ERR_CAPACITY;
      }
      shape->first_field = *out_record_field_count;
      for (uint16_t i = 0; i < shape->field_count; i++) {
        fr_record_name_t field = {0};

        FR_TRY(fr_persist_decode_record_name(&reader, &field));
        for (uint16_t j = 0; j < i; j++) {
          if (fr_persist_record_name_same(
                  field, record_fields[shape->first_field + j])) {
            return FR_ERR_CORRUPT;
          }
        }
        record_fields[*out_record_field_count] = field;
        *out_record_field_count =
            (uint16_t)(*out_record_field_count + 1);
      }
      object_kinds[*out_object_count] = FR_PERSIST_OBJECT_RECORD_SHAPE;
      *out_shape_count = (uint16_t)(*out_shape_count + 1);
      *out_object_count = (uint16_t)(*out_object_count + 1);
#endif
    } else if (record == FR_PERSIST_RECORD_RECORD) {
#if !FR_FEATURE_RECORDS
      return FR_ERR_UNSUPPORTED;
#else
      fr_persist_record_record_t *record_object = NULL;
      const fr_persist_record_shape_record_t *shape = NULL;

      if (*out_record_count >= FR_PROFILE_OBJECT_TABLE_SIZE ||
          *out_object_count >= FR_PROFILE_OBJECT_TABLE_SIZE) {
        return FR_ERR_CAPACITY;
      }
      record_object = &record_records[*out_record_count];
      FR_TRY(fr_persist_reader_u16(&reader, &record_object->local_id));
      if (record_object->local_id != *out_object_count) {
        return FR_ERR_CORRUPT;
      }
      FR_TRY(fr_persist_reader_u16(&reader,
                                   &record_object->shape_local_id));
      if (record_object->shape_local_id >= *out_object_count ||
          object_kinds[record_object->shape_local_id] !=
              FR_PERSIST_OBJECT_RECORD_SHAPE) {
        return FR_ERR_CORRUPT;
      }
      FR_TRY(fr_persist_find_shape_record(shape_records, *out_shape_count,
                                          record_object->shape_local_id,
                                          &shape));
      FR_TRY(fr_persist_reader_u16(&reader, &record_object->field_count));
      if (record_object->field_count != shape->field_count) {
        return FR_ERR_CORRUPT;
      }
      if ((uint32_t)*out_record_value_count + record_object->field_count >
          FR_RECORD_VALUE_FIELD_CAPACITY) {
        return FR_ERR_CAPACITY;
      }
      record_object->first_value = *out_record_value_count;
      for (uint16_t i = 0; i < record_object->field_count; i++) {
        fr_tagged_t tagged = 0;

        FR_TRY(fr_persist_decode_record_value(&reader, object_kinds,
                                              *out_object_count, &tagged));
        record_values[*out_record_value_count] = tagged;
        *out_record_value_count =
            (uint16_t)(*out_record_value_count + 1);
      }
      object_kinds[*out_object_count] = FR_PERSIST_OBJECT_RECORD;
      *out_record_count = (uint16_t)(*out_record_count + 1);
      *out_object_count = (uint16_t)(*out_object_count + 1);
#endif
    } else if (record == FR_PERSIST_RECORD_CELLS) {
#if !FR_FEATURE_CELLS
      return FR_ERR_UNSUPPORTED;
#else
      fr_persist_cell_record_t *cell = NULL;

      if (*out_cell_count >= FR_PROFILE_OBJECT_TABLE_SIZE ||
          *out_object_count >= FR_PROFILE_OBJECT_TABLE_SIZE) {
        return FR_ERR_CAPACITY;
      }
      cell = &cell_records[*out_cell_count];
      FR_TRY(fr_persist_reader_u16(&reader, &cell->local_id));
      if (cell->local_id != *out_object_count) {
        return FR_ERR_CORRUPT;
      }
      FR_TRY(fr_persist_reader_u16(&reader, &cell->length));
      if (cell->length == 0 || cell->length > FR_PROFILE_MAX_CELL_LENGTH) {
        return FR_ERR_CORRUPT;
      }
      if ((uint32_t)*out_cell_value_count + cell->length >
          FR_PROFILE_MAX_CELL_WORDS) {
        return FR_ERR_CAPACITY;
      }
      cell->first_value = *out_cell_value_count;
      for (uint16_t i = 0; i < cell->length; i++) {
        fr_tagged_t tagged = 0;

        FR_TRY(fr_persist_decode_cell_value(&reader, object_kinds,
                                            *out_object_count, &tagged));
        cell_values[*out_cell_value_count] = tagged;
        *out_cell_value_count = (uint16_t)(*out_cell_value_count + 1);
      }
      object_kinds[*out_object_count] = FR_PERSIST_OBJECT_CELLS;
      *out_cell_count = (uint16_t)(*out_cell_count + 1);
      *out_object_count = (uint16_t)(*out_object_count + 1);
#endif
    } else if (record == FR_PERSIST_RECORD_BIND) {
      fr_persist_bind_record_t *bind = NULL;

      if (*out_bind_count >= FR_PROFILE_MAX_SLOTS) {
        return FR_ERR_CAPACITY;
      }
      bind = &bind_records[*out_bind_count];
      FR_TRY(fr_persist_reader_u16(&reader, &bind->slot_id));
      if (bind->slot_id >= FR_PROFILE_MAX_SLOTS) {
        return FR_ERR_CORRUPT;
      }
      FR_TRY(fr_persist_decode_value(&reader, bind));
      *out_bind_count = (uint16_t)(*out_bind_count + 1);
    }
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
    else if (record == FR_PERSIST_RECORD_NAME) {
      fr_persist_name_record_t *name = NULL;
      const uint8_t *name_bytes = NULL;
      uint8_t name_length = 0;

      if (*out_name_count >= FR_PROFILE_MAX_OVERLAY_NAMES) {
        return FR_ERR_CAPACITY;
      }
      name = &name_records[*out_name_count];
      FR_TRY(fr_persist_reader_u16(&reader, &name->slot_id));
      if (name->slot_id >= FR_PROFILE_MAX_SLOTS) {
        return FR_ERR_CORRUPT;
      }
      FR_TRY(fr_persist_reader_u8(&reader, &name_length));
      if (name_length == 0 || name_length > FR_PROFILE_MAX_NAME_BYTES) {
        return FR_ERR_CORRUPT;
      }
      FR_TRY(fr_persist_reader_bytes(&reader, name_length, &name_bytes));
      for (uint8_t i = 0; i < name_length; i++) {
        if (name_bytes[i] == '\0') {
          return FR_ERR_CORRUPT;
        }
        name->name[i] = (char)name_bytes[i];
      }
      name->name[name_length] = '\0';
      *out_name_count = (uint16_t)(*out_name_count + 1);
    }
#else
    else if (record == FR_PERSIST_RECORD_NAME) {
      return FR_ERR_UNSUPPORTED;
    }
#endif
    else {
      return FR_ERR_CORRUPT;
    }
  }
}

static fr_err_t fr_persist_bind_value(const fr_runtime_t *runtime,
                                      const fr_persist_bind_record_t *bind,
                                      const fr_code_object_id_t code_map[],
                                      uint16_t code_count,
                                      const fr_object_id_t object_map[],
                                      uint16_t object_count,
                                      fr_tagged_t *out_tagged) {
  switch (bind->value_kind) {
  case FR_PERSIST_VALUE_NIL:
    *out_tagged = fr_tagged_nil();
    return FR_OK;
  case FR_PERSIST_VALUE_FALSE:
    *out_tagged = fr_tagged_false();
    return FR_OK;
  case FR_PERSIST_VALUE_TRUE:
    *out_tagged = fr_tagged_true();
    return FR_OK;
  case FR_PERSIST_VALUE_INT:
    return fr_tagged_encode_int(bind->int_value, out_tagged);
  case FR_PERSIST_VALUE_CODE:
    if (bind->value_word >= code_count) {
      return FR_ERR_CORRUPT;
    }
    return fr_tagged_encode_code_object_id(code_map[bind->value_word],
                                           out_tagged);
  case FR_PERSIST_VALUE_NATIVE:
    if (bind->value_word >= runtime->natives.base_count) {
      return FR_ERR_CORRUPT;
    }
    return fr_tagged_encode_native_id(bind->value_word, out_tagged);
  case FR_PERSIST_VALUE_OBJECT:
    if (bind->value_word >= object_count) {
      return FR_ERR_CORRUPT;
    }
    return fr_tagged_encode_object_id(object_map[bind->value_word],
                                      out_tagged);
  default:
    return FR_ERR_CORRUPT;
  }
}

static fr_err_t fr_persist_check_restore_capacity(
    const fr_runtime_t *runtime,
    const fr_persist_decoded_payload_t *decoded) {
  uint32_t used_instruction_bytes = 0;
  uint32_t used_cell_words = 0;
  uint32_t used_text_bytes = 0;
  uint32_t used_record_names = 0;
  uint32_t used_record_shape_fields = 0;
  uint32_t used_record_values = 0;
  uint32_t used_record_name_bytes = 0;
  uint16_t unique_text_count = 0;
  const fr_persist_code_record_t *code_records = NULL;
  const fr_persist_text_record_t *text_records = NULL;
  const fr_persist_record_shape_record_t *shape_records = NULL;
  const fr_record_name_t *record_fields = NULL;
  const fr_persist_record_record_t *record_records = NULL;
  const fr_persist_cell_record_t *cell_records = NULL;
  uint16_t code_count = 0;
  uint16_t text_count = 0;
  uint16_t shape_count = 0;
  uint16_t record_field_count = 0;
  uint16_t record_count = 0;
  uint16_t cell_count = 0;

  if (runtime == NULL || decoded == NULL) {
    return FR_ERR_INVALID;
  }
  code_records = decoded->code_records;
  text_records = decoded->text_records;
  shape_records = decoded->shape_records;
  record_fields = decoded->record_fields;
  record_records = decoded->record_records;
  cell_records = decoded->cell_records;
  code_count = decoded->code_count;
  text_count = decoded->text_count;
  shape_count = decoded->shape_count;
  record_field_count = decoded->record_field_count;
  record_count = decoded->record_count;
  cell_count = decoded->cell_count;
#if !FR_FEATURE_TEXT
  (void)text_records;
  (void)text_count;
#endif
#if !FR_FEATURE_RECORDS
  (void)shape_records;
  (void)record_fields;
  (void)record_records;
  (void)shape_count;
  (void)record_field_count;
  (void)record_count;
#endif
#if !FR_FEATURE_CELLS
  (void)cell_records;
  (void)cell_count;
#endif

  if ((uint32_t)runtime->code.base_count + code_count >
      FR_PROFILE_CODE_OBJECT_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }

  used_instruction_bytes = runtime->code.base_used_instruction_bytes;
  for (uint16_t i = 0; i < code_count; i++) {
    if (used_instruction_bytes + code_records[i].length >
        FR_PROFILE_MAX_INSTRUCTION_BYTES) {
      return FR_ERR_CAPACITY;
    }
    used_instruction_bytes += code_records[i].length;
  }

#if FR_FEATURE_TEXT
  used_text_bytes = runtime->objects.base_used_text_bytes;
  for (uint16_t i = 0; i < text_count; i++) {
    bool duplicate = false;

    if (text_records[i].length > FR_PROFILE_MAX_TEXT_LENGTH ||
        (text_records[i].bytes == NULL && text_records[i].length > 0)) {
      return FR_ERR_CORRUPT;
    }
    for (uint16_t j = 0; j < i; j++) {
      if (text_records[j].length == text_records[i].length &&
          (text_records[i].length == 0 ||
           memcmp(text_records[j].bytes, text_records[i].bytes,
                  text_records[i].length) == 0)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    if (used_text_bytes + text_records[i].length >
        FR_PROFILE_MAX_TEXT_BYTES) {
      return FR_ERR_CAPACITY;
    }
    used_text_bytes += text_records[i].length;
    unique_text_count = (uint16_t)(unique_text_count + 1);
  }
#else
  if (text_count > 0) {
    return FR_ERR_UNSUPPORTED;
  }
  (void)used_text_bytes;
#endif

#if FR_FEATURE_RECORDS
  used_record_names = runtime->objects.base_used_record_names;
  used_record_shape_fields = runtime->objects.base_used_record_shape_fields;
  used_record_values = runtime->objects.base_used_record_values;
  used_record_name_bytes = runtime->objects.base_used_record_name_bytes;
  if (record_field_count > FR_RECORD_SHAPE_FIELD_CAPACITY) {
    return FR_ERR_CAPACITY;
  }
  for (uint16_t i = 0; i < shape_count; i++) {
    const fr_persist_record_shape_record_t *shape = &shape_records[i];
    uint32_t shape_name_bytes = shape->name.length;

    if (shape->field_count == 0 ||
        shape->field_count > FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE ||
        (uint32_t)shape->first_field + shape->field_count >
            record_field_count ||
        shape->name.length == 0 ||
        shape->name.length > FR_PROFILE_MAX_NAME_BYTES ||
        shape->name.bytes == NULL) {
      return FR_ERR_CORRUPT;
    }
    used_record_names += (uint32_t)1 + shape->field_count;
    used_record_shape_fields += shape->field_count;
    for (uint16_t j = 0; j < shape->field_count; j++) {
      fr_record_name_t field = record_fields[shape->first_field + j];

      if (field.length == 0 || field.length > FR_PROFILE_MAX_NAME_BYTES ||
          field.bytes == NULL) {
        return FR_ERR_CORRUPT;
      }
      shape_name_bytes += field.length;
    }
    used_record_name_bytes += shape_name_bytes;
  }
  for (uint16_t i = 0; i < record_count; i++) {
    const fr_persist_record_shape_record_t *shape = NULL;

    FR_TRY(fr_persist_find_shape_record(shape_records, shape_count,
                                        record_records[i].shape_local_id,
                                        &shape));
    if (record_records[i].field_count != shape->field_count ||
        (uint32_t)record_records[i].first_value +
                record_records[i].field_count >
            FR_RECORD_VALUE_FIELD_CAPACITY) {
      return FR_ERR_CORRUPT;
    }
    used_record_values += record_records[i].field_count;
  }
  if (used_record_names > FR_RECORD_NAME_ENTRY_CAPACITY ||
      used_record_shape_fields > FR_PROFILE_MAX_RECORD_SHAPE_FIELDS ||
      used_record_values > FR_PROFILE_MAX_RECORD_VALUE_FIELDS ||
      used_record_name_bytes > FR_PROFILE_MAX_RECORD_NAME_BYTES) {
    return FR_ERR_CAPACITY;
  }
#else
  if (shape_count > 0 || record_count > 0 || record_field_count > 0) {
    return FR_ERR_UNSUPPORTED;
  }
  (void)used_record_names;
  (void)used_record_shape_fields;
  (void)used_record_values;
  (void)used_record_name_bytes;
#endif

  used_cell_words = runtime->objects.base_used_cell_words;
  for (uint16_t i = 0; i < cell_count; i++) {
    if (cell_records[i].length == 0 ||
        cell_records[i].length > FR_PROFILE_MAX_CELL_LENGTH) {
      return FR_ERR_CORRUPT;
    }
    if (used_cell_words + cell_records[i].length >
        FR_PROFILE_MAX_CELL_WORDS) {
      return FR_ERR_CAPACITY;
    }
    used_cell_words += cell_records[i].length;
  }
  if ((uint32_t)runtime->objects.base_count + unique_text_count + shape_count +
          record_count + cell_count >
      FR_PROFILE_OBJECT_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }
  return FR_OK;
}

static fr_err_t fr_persist_check_bind_record(
    const fr_runtime_t *runtime, const fr_persist_bind_record_t *bind,
    uint16_t code_count, uint16_t object_count) {
  fr_tagged_t ignored = 0;

#if !FR_FEATURE_OBJECTS
  (void)object_count;
#endif
  if (runtime == NULL || bind == NULL) {
    return FR_ERR_INVALID;
  }

  switch (bind->value_kind) {
  case FR_PERSIST_VALUE_NIL:
  case FR_PERSIST_VALUE_FALSE:
  case FR_PERSIST_VALUE_TRUE:
    return FR_OK;
  case FR_PERSIST_VALUE_INT:
    return fr_tagged_encode_int(bind->int_value, &ignored);
  case FR_PERSIST_VALUE_CODE:
    return bind->value_word < code_count ? FR_OK : FR_ERR_CORRUPT;
  case FR_PERSIST_VALUE_NATIVE:
    return bind->value_word < runtime->natives.base_count ? FR_OK
                                                          : FR_ERR_CORRUPT;
  case FR_PERSIST_VALUE_OBJECT:
#if FR_FEATURE_OBJECTS
    return bind->value_word < object_count ? FR_OK : FR_ERR_CORRUPT;
#else
    return FR_ERR_UNSUPPORTED;
#endif
  default:
    return FR_ERR_CORRUPT;
  }
}

#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
static fr_slot_id_t fr_persist_slot_count_after_restore(
    const fr_runtime_t *runtime, const fr_persist_bind_record_t bind_records[],
    uint16_t bind_count) {
  fr_slot_id_t slot_count = runtime->slots.base_count;

  for (uint16_t i = 0; i < bind_count; i++) {
    if (bind_records[i].slot_id >= slot_count) {
      slot_count = (fr_slot_id_t)(bind_records[i].slot_id + 1);
    }
  }
  return slot_count;
}

static fr_err_t fr_persist_check_name_records(
    const fr_persist_name_record_t name_records[], uint16_t name_count,
    fr_slot_id_t slot_count_after_writes) {
  fr_slot_id_t base_slot_id = 0;
  fr_slot_id_t first_project_id = fr_slot_first_project_id();

  if (name_records == NULL && name_count > 0) {
    return FR_ERR_INVALID;
  }
  if (name_count > FR_PROFILE_MAX_OVERLAY_NAMES) {
    return FR_ERR_CAPACITY;
  }

  for (uint16_t i = 0; i < name_count; i++) {
    if (name_records[i].slot_id < first_project_id ||
        name_records[i].slot_id >= slot_count_after_writes) {
      return FR_ERR_CORRUPT;
    }
    if (fr_base_slot_id_for_name(name_records[i].name, &base_slot_id) ==
        FR_OK) {
      return FR_ERR_CORRUPT;
    }
    for (uint16_t j = 0; j < i; j++) {
      if (strcmp(name_records[i].name, name_records[j].name) == 0 ||
          name_records[i].slot_id == name_records[j].slot_id) {
        return FR_ERR_CORRUPT;
      }
    }
  }
  return FR_OK;
}
#endif

fr_err_t fr_persist_payload_restore(fr_runtime_t *runtime, const uint8_t *bytes,
                                    uint16_t length) {
  fr_persist_decoded_payload_t decoded;
  fr_tagged_t cell_install_values[FR_CELL_WORD_CAPACITY];
  const fr_persist_code_record_t *code_records = decoded.code_records;
  const fr_persist_text_record_t *text_records = decoded.text_records;
  const fr_persist_record_shape_record_t *shape_records =
      decoded.shape_records;
  const fr_record_name_t *record_fields = decoded.record_fields;
  const fr_persist_record_record_t *record_records = decoded.record_records;
  const fr_tagged_t *record_values = decoded.record_values;
  const fr_persist_cell_record_t *cell_records = decoded.cell_records;
  const fr_tagged_t *cell_values = decoded.cell_values;
  const fr_persist_object_record_kind_t *object_kinds = decoded.object_kinds;
  const fr_persist_bind_record_t *bind_records = decoded.bind_records;
  const fr_persist_name_record_t *name_records = decoded.name_records;
  fr_code_object_id_t code_map[FR_PROFILE_CODE_OBJECT_TABLE_SIZE];
  fr_object_id_t object_map[FR_OBJECT_TABLE_CAPACITY];
  uint16_t code_count = 0;
  uint16_t text_count = 0;
  uint16_t shape_count = 0;
  uint16_t record_count = 0;
  uint16_t record_value_count = 0;
  uint16_t cell_count = 0;
  uint16_t cell_value_count = 0;
  uint16_t object_count = 0;
  uint16_t bind_count = 0;
  uint16_t name_count = 0;
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  fr_slot_id_t slot_count_after_writes = 0;
#endif

  if (runtime == NULL || bytes == NULL) {
    return FR_ERR_INVALID;
  }
#if !FR_FEATURE_RECORDS
  (void)shape_records;
  (void)record_fields;
  (void)record_records;
  (void)record_values;
#endif

  memset(object_map, 0, sizeof(object_map));
  FR_TRY(fr_persist_decode_payload(bytes, length, &decoded));
  code_count = decoded.code_count;
  text_count = decoded.text_count;
  shape_count = decoded.shape_count;
  record_count = decoded.record_count;
  record_value_count = decoded.record_value_count;
  cell_count = decoded.cell_count;
  cell_value_count = decoded.cell_value_count;
  object_count = decoded.object_count;
  bind_count = decoded.bind_count;
  name_count = decoded.name_count;
  (void)cell_value_count;
  (void)record_value_count;
#if FR_PROFILE_MAX_OVERLAY_NAMES == 0
  (void)name_records;
  (void)name_count;
#endif
  FR_TRY(fr_persist_check_restore_capacity(runtime, &decoded));
  for (uint16_t i = 0; i < bind_count; i++) {
    FR_TRY(fr_persist_check_bind_record(runtime, &bind_records[i], code_count,
                                        object_count));
  }
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  slot_count_after_writes = fr_persist_slot_count_after_restore(
      runtime, bind_records, bind_count);
  FR_TRY(fr_persist_check_name_records(name_records, name_count,
                                      slot_count_after_writes));
#endif

  FR_TRY(fr_runtime_reset(runtime));
  for (uint16_t i = 0; i < code_count; i++) {
    fr_instruction_stream_t instructions;

    FR_TRY(fr_instruction_stream_init(&instructions, code_records[i].bytes,
                                      code_records[i].length));
    FR_TRY(fr_code_install(runtime, &instructions, NULL, 0, &code_map[i]));
  }
  for (uint16_t i = 0; i < text_count; i++) {
    FR_TRY(fr_text_install_since(runtime, text_records[i].bytes,
                                 text_records[i].length,
                                 runtime->objects.base_count,
                                 &object_map[text_records[i].local_id]));
  }
  /*
   * Restore object refs in dependency order: record fields may point at text,
   * records point at shapes, and cells may point at records.
   */
#if FR_FEATURE_RECORDS
  fr_tagged_t record_install_values[FR_RECORD_FIELDS_PER_SHAPE_CAPACITY];

  for (uint16_t i = 0; i < shape_count; i++) {
    fr_record_name_t install_fields[FR_RECORD_FIELDS_PER_SHAPE_CAPACITY];

    for (uint16_t j = 0; j < shape_records[i].field_count; j++) {
      install_fields[j] = record_fields[shape_records[i].first_field + j];
    }
    FR_TRY(fr_record_shape_install_since(
        runtime, shape_records[i].name, install_fields,
        shape_records[i].field_count, runtime->objects.base_count,
        &object_map[shape_records[i].local_id]));
  }
  for (uint16_t i = 0; i < record_count; i++) {
    if (record_records[i].field_count > FR_RECORD_FIELDS_PER_SHAPE_CAPACITY) {
      return FR_ERR_CORRUPT;
    }
    for (uint16_t j = 0; j < record_records[i].field_count; j++) {
      fr_tagged_t stored = record_values[record_records[i].first_value + j];
      fr_object_id_t local_object_id = 0;

      if (fr_tagged_decode_object_id(stored, &local_object_id) == FR_OK) {
        if (local_object_id >= object_count ||
            object_kinds[local_object_id] != FR_PERSIST_OBJECT_TEXT) {
          return FR_ERR_CORRUPT;
        }
        FR_TRY(fr_tagged_encode_object_id(object_map[local_object_id],
                                          &stored));
      }
      if (!fr_record_field_value_allowed(runtime, stored)) {
        return FR_ERR_CORRUPT;
      }
      record_install_values[j] = stored;
    }
    FR_TRY(fr_record_install(
        runtime, object_map[record_records[i].shape_local_id],
        record_install_values, record_records[i].field_count,
        &object_map[record_records[i].local_id]));
  }
#else
  if (shape_count > 0 || record_count > 0) {
    return FR_ERR_UNSUPPORTED;
  }
#endif
  for (uint16_t i = 0; i < cell_count; i++) {
    for (uint16_t j = 0; j < cell_records[i].length; j++) {
      fr_tagged_t stored = cell_values[cell_records[i].first_value + j];
      fr_object_id_t local_object_id = 0;

      if (fr_tagged_decode_object_id(stored, &local_object_id) == FR_OK) {
        if (local_object_id >= object_count ||
            (object_kinds[local_object_id] != FR_PERSIST_OBJECT_TEXT
#if FR_FEATURE_RECORDS
             && object_kinds[local_object_id] != FR_PERSIST_OBJECT_RECORD
#endif
             )) {
          return FR_ERR_CORRUPT;
        }
        FR_TRY(fr_tagged_encode_object_id(object_map[local_object_id],
                                          &stored));
      }
      if (!fr_cells_value_allowed(runtime, stored)) {
        return FR_ERR_CORRUPT;
      }
      cell_install_values[j] = stored;
    }
    FR_TRY(fr_cells_install(runtime, cell_records[i].length,
                            cell_install_values,
                            &object_map[cell_records[i].local_id]));
  }
  for (uint16_t i = 0; i < bind_count; i++) {
    fr_tagged_t tagged = 0;

    FR_TRY(fr_persist_bind_value(runtime, &bind_records[i], code_map,
                                 code_count, object_map, object_count,
                                 &tagged));
    FR_TRY(fr_slot_write(runtime, bind_records[i].slot_id, tagged));
  }
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  for (uint16_t i = 0; i < name_count; i++) {
    FR_TRY(fr_slot_bind_project_name(runtime, name_records[i].name,
                                     name_records[i].slot_id));
  }
#endif

  return FR_OK;
}
