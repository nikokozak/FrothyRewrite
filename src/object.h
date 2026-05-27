#pragma once

#include "tagged.h"
#include "types.h"

#include <stdbool.h>

#define FR_OBJECT_TABLE_CAPACITY                                             \
  (FR_PROFILE_OBJECT_TABLE_SIZE > 0 ? FR_PROFILE_OBJECT_TABLE_SIZE : 1)
#define FR_CELL_WORD_CAPACITY                                                \
  (FR_PROFILE_MAX_CELL_WORDS > 0 ? FR_PROFILE_MAX_CELL_WORDS : 1)
#define FR_TEXT_BYTE_CAPACITY                                                \
  (FR_PROFILE_MAX_TEXT_BYTES > 0 ? FR_PROFILE_MAX_TEXT_BYTES : 1)
#if FR_FEATURE_RECORDS
#define FR_RECORD_NAME_BYTE_CAPACITY                                         \
  (FR_PROFILE_MAX_RECORD_NAME_BYTES > 0 ? FR_PROFILE_MAX_RECORD_NAME_BYTES    \
                                        : 1)
#define FR_RECORD_SHAPE_FIELD_CAPACITY                                       \
  (FR_PROFILE_MAX_RECORD_SHAPE_FIELDS > 0                                    \
       ? FR_PROFILE_MAX_RECORD_SHAPE_FIELDS                                  \
       : 1)
#define FR_RECORD_VALUE_FIELD_CAPACITY                                       \
  (FR_PROFILE_MAX_RECORD_VALUE_FIELDS > 0                                    \
       ? FR_PROFILE_MAX_RECORD_VALUE_FIELDS                                  \
       : 1)
#define FR_RECORD_NAME_ENTRY_CAPACITY                                        \
  (FR_OBJECT_TABLE_CAPACITY + FR_RECORD_SHAPE_FIELD_CAPACITY)
#define FR_RECORD_FIELDS_PER_SHAPE_CAPACITY                                  \
  (FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE > 0                                \
       ? FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE                              \
       : 1)
#else
#define FR_RECORD_NAME_BYTE_CAPACITY 1
#define FR_RECORD_SHAPE_FIELD_CAPACITY 1
#define FR_RECORD_VALUE_FIELD_CAPACITY 1
#define FR_RECORD_NAME_ENTRY_CAPACITY 1
#define FR_RECORD_FIELDS_PER_SHAPE_CAPACITY 1
#endif

typedef enum fr_object_kind_t {
  FR_OBJECT_NONE = 0,
  FR_OBJECT_CELLS = 1,
  FR_OBJECT_TEXT = 2,
  FR_OBJECT_RECORD_SHAPE = 3,
  FR_OBJECT_RECORD = 4,
} fr_object_kind_t;

typedef struct fr_object_entry_t {
  fr_object_kind_t kind;
  uint16_t first;
  uint16_t length;
#if FR_FEATURE_RECORDS
  /*
   * Records own aux: record-shape stores its shape-name entry; record stores
   * its shape object id. Non-record object kinds must leave aux zero.
   */
  uint16_t aux;
#endif
} fr_object_entry_t;

typedef struct fr_record_name_t {
  const uint8_t *bytes;
  uint16_t length;
} fr_record_name_t;

typedef struct fr_record_name_entry_t {
  uint16_t first;
  uint16_t length;
} fr_record_name_entry_t;

typedef struct fr_object_table_t {
  fr_object_entry_t entries[FR_OBJECT_TABLE_CAPACITY];
  fr_object_entry_t base_entries[FR_OBJECT_TABLE_CAPACITY];
  fr_tagged_t cell_values[FR_CELL_WORD_CAPACITY];
  fr_tagged_t base_cell_values[FR_CELL_WORD_CAPACITY];
  uint8_t text_bytes[FR_TEXT_BYTE_CAPACITY];
  uint8_t base_text_bytes[FR_TEXT_BYTE_CAPACITY];
#if FR_FEATURE_RECORDS
  fr_record_name_entry_t record_names[FR_RECORD_NAME_ENTRY_CAPACITY];
  fr_record_name_entry_t base_record_names[FR_RECORD_NAME_ENTRY_CAPACITY];
  uint16_t record_shape_fields[FR_RECORD_SHAPE_FIELD_CAPACITY];
  uint16_t base_record_shape_fields[FR_RECORD_SHAPE_FIELD_CAPACITY];
  fr_tagged_t record_values[FR_RECORD_VALUE_FIELD_CAPACITY];
  fr_tagged_t base_record_values[FR_RECORD_VALUE_FIELD_CAPACITY];
  uint8_t record_name_bytes[FR_RECORD_NAME_BYTE_CAPACITY];
  uint8_t base_record_name_bytes[FR_RECORD_NAME_BYTE_CAPACITY];
#endif
  bool overlay[FR_OBJECT_TABLE_CAPACITY];
  uint16_t count;
  uint16_t used_cell_words;
  uint16_t used_text_bytes;
#if FR_FEATURE_RECORDS
  uint16_t used_record_names;
  uint16_t used_record_shape_fields;
  uint16_t used_record_values;
  uint16_t used_record_name_bytes;
#endif
  uint16_t base_count;
  uint16_t base_used_cell_words;
  uint16_t base_used_text_bytes;
#if FR_FEATURE_RECORDS
  uint16_t base_used_record_names;
  uint16_t base_used_record_shape_fields;
  uint16_t base_used_record_values;
  uint16_t base_used_record_name_bytes;
#endif
} fr_object_table_t;

void fr_object_reset(fr_runtime_t *runtime);
void fr_object_mark_base(fr_runtime_t *runtime);
void fr_object_restore_base(fr_runtime_t *runtime);

bool fr_cells_value_allowed(const fr_runtime_t *runtime, fr_tagged_t tagged);
fr_err_t fr_cells_check_install(const fr_runtime_t *runtime, uint16_t length,
                                const fr_tagged_t initial_values[]);
fr_err_t fr_cells_install(fr_runtime_t *runtime, uint16_t length,
                          const fr_tagged_t initial_values[],
                          fr_object_id_t *out_object_id);
fr_err_t fr_cells_length(const fr_runtime_t *runtime,
                         fr_object_id_t object_id, uint16_t *out_length);
fr_err_t fr_cells_read(const fr_runtime_t *runtime, fr_object_id_t object_id,
                       uint16_t index, fr_tagged_t *out_tagged);
fr_err_t fr_cells_write(fr_runtime_t *runtime, fr_object_id_t object_id,
                        uint16_t index, fr_tagged_t tagged);
/*
 * Text objects store exactly length bytes. The bytes are not NUL-terminated,
 * and callers must keep all display or comparison code length-based.
 */
fr_err_t fr_text_find(const fr_runtime_t *runtime, const uint8_t *bytes,
                      uint16_t length, fr_object_id_t first_object_id,
                      fr_object_id_t *out_object_id);
fr_err_t fr_text_check_install(const fr_runtime_t *runtime,
                               const uint8_t *bytes, uint16_t length);
fr_err_t fr_text_install(fr_runtime_t *runtime, const uint8_t *bytes,
                         uint16_t length, fr_object_id_t *out_object_id);
fr_err_t fr_text_install_since(fr_runtime_t *runtime, const uint8_t *bytes,
                               uint16_t length,
                               fr_object_id_t first_object_id,
                               fr_object_id_t *out_object_id);
fr_err_t fr_text_view(const fr_runtime_t *runtime, fr_object_id_t object_id,
                      const uint8_t **out_bytes, uint16_t *out_length);
bool fr_record_field_value_allowed(const fr_runtime_t *runtime,
                                   fr_tagged_t tagged);
fr_err_t fr_record_shape_find(const fr_runtime_t *runtime,
                              fr_record_name_t name,
                              const fr_record_name_t fields[],
                              uint16_t field_count,
                              fr_object_id_t first_object_id,
                              fr_object_id_t *out_object_id);
fr_err_t fr_record_shape_install(fr_runtime_t *runtime,
                                 fr_record_name_t name,
                                 const fr_record_name_t fields[],
                                 uint16_t field_count,
                                 fr_object_id_t *out_object_id);
fr_err_t fr_record_shape_install_since(fr_runtime_t *runtime,
                                       fr_record_name_t name,
                                       const fr_record_name_t fields[],
                                       uint16_t field_count,
                                       fr_object_id_t first_object_id,
                                       fr_object_id_t *out_object_id);
fr_err_t fr_record_shape_view(const fr_runtime_t *runtime,
                              fr_object_id_t object_id,
                              fr_record_name_t *out_name,
                              uint16_t *out_field_count);
fr_err_t fr_record_shape_field_name(const fr_runtime_t *runtime,
                                    fr_object_id_t shape_object_id,
                                    uint16_t field_index,
                                    fr_record_name_t *out_field);
fr_err_t fr_record_shape_field_index(const fr_runtime_t *runtime,
                                     fr_object_id_t shape_object_id,
                                     fr_record_name_t field,
                                     uint16_t *out_index);
fr_err_t fr_record_install(fr_runtime_t *runtime,
                           fr_object_id_t shape_object_id,
                           const fr_tagged_t field_values[],
                           uint16_t field_count,
                           fr_object_id_t *out_object_id);
fr_err_t fr_record_view(const fr_runtime_t *runtime, fr_object_id_t object_id,
                        fr_object_id_t *out_shape_object_id,
                        uint16_t *out_field_count);
fr_err_t fr_record_read_index(const fr_runtime_t *runtime,
                              fr_object_id_t object_id, uint16_t field_index,
                              fr_tagged_t *out_tagged);
fr_err_t fr_record_read_field(const fr_runtime_t *runtime,
                              fr_object_id_t object_id,
                              fr_record_name_t field,
                              fr_tagged_t *out_tagged);
fr_err_t fr_record_write_field(fr_runtime_t *runtime,
                               fr_object_id_t object_id,
                               fr_record_name_t field,
                               fr_tagged_t tagged);
fr_object_id_t fr_object_count(const fr_runtime_t *runtime);
bool fr_object_is_overlay(const fr_runtime_t *runtime,
                          fr_object_id_t object_id);
