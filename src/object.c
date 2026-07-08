#include "object.h"

#include "runtime.h"

#include <string.h>

#if FR_FEATURE_OBJECTS
static void fr_object_zero_entries(fr_object_entry_t entries[],
                                   uint16_t start) {
  if (start >= FR_OBJECT_TABLE_CAPACITY) {
    return;
  }
  memset(&entries[start], 0,
         (FR_OBJECT_TABLE_CAPACITY - start) * sizeof(entries[0]));
}

static void fr_object_zero_cells(fr_tagged_t cells[], uint16_t start) {
  if (start >= FR_CELL_WORD_CAPACITY) {
    return;
  }
  memset(&cells[start], 0,
         (FR_CELL_WORD_CAPACITY - start) * sizeof(cells[0]));
}

static void fr_object_zero_text(uint8_t bytes[], uint16_t start) {
  if (start >= FR_TEXT_BYTE_CAPACITY) {
    return;
  }
  memset(&bytes[start], 0, FR_TEXT_BYTE_CAPACITY - start);
}

#if FR_FEATURE_RECORDS
static void fr_object_zero_record_names(fr_record_name_entry_t names[],
                                        uint16_t start) {
  if (start >= FR_RECORD_NAME_ENTRY_CAPACITY) {
    return;
  }
  memset(&names[start], 0,
         (FR_RECORD_NAME_ENTRY_CAPACITY - start) * sizeof(names[0]));
}

static void fr_object_zero_record_shape_fields(uint16_t fields[],
                                               uint16_t start) {
  if (start >= FR_RECORD_SHAPE_FIELD_CAPACITY) {
    return;
  }
  memset(&fields[start], 0,
         (FR_RECORD_SHAPE_FIELD_CAPACITY - start) * sizeof(fields[0]));
}

static void fr_object_zero_record_values(fr_tagged_t values[],
                                         uint16_t start) {
  if (start >= FR_RECORD_VALUE_FIELD_CAPACITY) {
    return;
  }
  memset(&values[start], 0,
         (FR_RECORD_VALUE_FIELD_CAPACITY - start) * sizeof(values[0]));
}

static void fr_object_zero_record_name_bytes(uint8_t bytes[], uint16_t start) {
  if (start >= FR_RECORD_NAME_BYTE_CAPACITY) {
    return;
  }
  memset(&bytes[start], 0, FR_RECORD_NAME_BYTE_CAPACITY - start);
}
#endif
#endif

void fr_object_reset(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }

#if !FR_FEATURE_OBJECTS
  memset(&runtime->objects, 0, sizeof(runtime->objects));
#else
  runtime->objects.count = 0;
  runtime->objects.used_cell_words = 0;
  runtime->objects.used_text_bytes = 0;
  runtime->objects.base_count = 0;
  runtime->objects.image_count = 0;
  runtime->objects.base_used_cell_words = 0;
  runtime->objects.base_used_text_bytes = 0;
  runtime->objects.image_used_cell_words = 0;
  runtime->objects.image_used_text_bytes = 0;
#if FR_FEATURE_RECORDS
  runtime->objects.used_record_names = 0;
  runtime->objects.used_record_shape_fields = 0;
  runtime->objects.used_record_values = 0;
  runtime->objects.used_record_name_bytes = 0;
  runtime->objects.base_used_record_names = 0;
  runtime->objects.base_used_record_shape_fields = 0;
  runtime->objects.base_used_record_values = 0;
  runtime->objects.base_used_record_name_bytes = 0;
  runtime->objects.image_used_record_names = 0;
  runtime->objects.image_used_record_shape_fields = 0;
  runtime->objects.image_used_record_values = 0;
  runtime->objects.image_used_record_name_bytes = 0;
#endif
  memset(runtime->objects.entries, 0, sizeof(runtime->objects.entries));
  memset(runtime->objects.base_entries, 0,
         sizeof(runtime->objects.base_entries));
  memset(runtime->objects.cell_values, 0, sizeof(runtime->objects.cell_values));
  memset(runtime->objects.base_cell_values, 0,
         sizeof(runtime->objects.base_cell_values));
  memset(runtime->objects.text_bytes, 0, sizeof(runtime->objects.text_bytes));
  memset(runtime->objects.base_text_bytes, 0,
         sizeof(runtime->objects.base_text_bytes));
#if FR_FEATURE_RECORDS
  memset(runtime->objects.record_names, 0,
         sizeof(runtime->objects.record_names));
  memset(runtime->objects.base_record_names, 0,
         sizeof(runtime->objects.base_record_names));
  memset(runtime->objects.record_shape_fields, 0,
         sizeof(runtime->objects.record_shape_fields));
  memset(runtime->objects.base_record_shape_fields, 0,
         sizeof(runtime->objects.base_record_shape_fields));
  memset(runtime->objects.record_values, 0,
         sizeof(runtime->objects.record_values));
  memset(runtime->objects.base_record_values, 0,
         sizeof(runtime->objects.base_record_values));
  memset(runtime->objects.record_name_bytes, 0,
         sizeof(runtime->objects.record_name_bytes));
  memset(runtime->objects.base_record_name_bytes, 0,
         sizeof(runtime->objects.base_record_name_bytes));
#endif
  memset(runtime->objects.overlay, 0, sizeof(runtime->objects.overlay));
#endif
}

void fr_object_mark_base(fr_runtime_t *runtime) {
#if !FR_FEATURE_OBJECTS
  (void)runtime;
#else
  if (runtime == NULL) {
    return;
  }

  runtime->objects.base_count = runtime->objects.count;
  runtime->objects.image_count = runtime->objects.count;
  runtime->objects.base_used_cell_words = runtime->objects.used_cell_words;
  runtime->objects.base_used_text_bytes = runtime->objects.used_text_bytes;
  runtime->objects.image_used_cell_words = runtime->objects.used_cell_words;
  runtime->objects.image_used_text_bytes = runtime->objects.used_text_bytes;
#if FR_FEATURE_RECORDS
  runtime->objects.base_used_record_names =
      runtime->objects.used_record_names;
  runtime->objects.base_used_record_shape_fields =
      runtime->objects.used_record_shape_fields;
  runtime->objects.base_used_record_values =
      runtime->objects.used_record_values;
  runtime->objects.base_used_record_name_bytes =
      runtime->objects.used_record_name_bytes;
  runtime->objects.image_used_record_names =
      runtime->objects.used_record_names;
  runtime->objects.image_used_record_shape_fields =
      runtime->objects.used_record_shape_fields;
  runtime->objects.image_used_record_values =
      runtime->objects.used_record_values;
  runtime->objects.image_used_record_name_bytes =
      runtime->objects.used_record_name_bytes;
#endif
  memcpy(runtime->objects.base_entries, runtime->objects.entries,
         runtime->objects.count * sizeof(runtime->objects.entries[0]));
  memcpy(runtime->objects.base_cell_values, runtime->objects.cell_values,
         runtime->objects.used_cell_words *
             sizeof(runtime->objects.cell_values[0]));
  memcpy(runtime->objects.base_text_bytes, runtime->objects.text_bytes,
         runtime->objects.used_text_bytes *
             sizeof(runtime->objects.text_bytes[0]));
#if FR_FEATURE_RECORDS
  memcpy(runtime->objects.base_record_names, runtime->objects.record_names,
         runtime->objects.used_record_names *
             sizeof(runtime->objects.record_names[0]));
  memcpy(runtime->objects.base_record_shape_fields,
         runtime->objects.record_shape_fields,
         runtime->objects.used_record_shape_fields *
             sizeof(runtime->objects.record_shape_fields[0]));
  memcpy(runtime->objects.base_record_values, runtime->objects.record_values,
         runtime->objects.used_record_values *
             sizeof(runtime->objects.record_values[0]));
  memcpy(runtime->objects.base_record_name_bytes,
         runtime->objects.record_name_bytes,
         runtime->objects.used_record_name_bytes *
             sizeof(runtime->objects.record_name_bytes[0]));
#endif
  for (fr_object_id_t object_id = 0; object_id < runtime->objects.count;
       object_id++) {
    runtime->objects.overlay[object_id] = false;
  }
#endif
}

void fr_object_mark_image(fr_runtime_t *runtime) {
#if !FR_FEATURE_OBJECTS
  (void)runtime;
#else
  if (runtime == NULL) {
    return;
  }

  runtime->objects.image_count = runtime->objects.count;
  runtime->objects.image_used_cell_words = runtime->objects.used_cell_words;
  runtime->objects.image_used_text_bytes = runtime->objects.used_text_bytes;
#if FR_FEATURE_RECORDS
  runtime->objects.image_used_record_names =
      runtime->objects.used_record_names;
  runtime->objects.image_used_record_shape_fields =
      runtime->objects.used_record_shape_fields;
  runtime->objects.image_used_record_values =
      runtime->objects.used_record_values;
  runtime->objects.image_used_record_name_bytes =
      runtime->objects.used_record_name_bytes;
#endif
  for (fr_object_id_t object_id = 0; object_id < runtime->objects.count;
       object_id++) {
    runtime->objects.overlay[object_id] = false;
  }
#endif
}

void fr_object_restore_base(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }

#if !FR_FEATURE_OBJECTS
  memset(&runtime->objects, 0, sizeof(runtime->objects));
#else
  runtime->objects.count = runtime->objects.base_count;
  runtime->objects.image_count = runtime->objects.base_count;
  runtime->objects.used_cell_words = runtime->objects.base_used_cell_words;
  runtime->objects.used_text_bytes = runtime->objects.base_used_text_bytes;
  runtime->objects.image_used_cell_words =
      runtime->objects.base_used_cell_words;
  runtime->objects.image_used_text_bytes =
      runtime->objects.base_used_text_bytes;
#if FR_FEATURE_RECORDS
  runtime->objects.used_record_names = runtime->objects.base_used_record_names;
  runtime->objects.used_record_shape_fields =
      runtime->objects.base_used_record_shape_fields;
  runtime->objects.used_record_values =
      runtime->objects.base_used_record_values;
  runtime->objects.used_record_name_bytes =
      runtime->objects.base_used_record_name_bytes;
  runtime->objects.image_used_record_names =
      runtime->objects.base_used_record_names;
  runtime->objects.image_used_record_shape_fields =
      runtime->objects.base_used_record_shape_fields;
  runtime->objects.image_used_record_values =
      runtime->objects.base_used_record_values;
  runtime->objects.image_used_record_name_bytes =
      runtime->objects.base_used_record_name_bytes;
#endif
  memcpy(runtime->objects.entries, runtime->objects.base_entries,
         runtime->objects.base_count * sizeof(runtime->objects.entries[0]));
  memcpy(runtime->objects.cell_values, runtime->objects.base_cell_values,
         runtime->objects.base_used_cell_words *
             sizeof(runtime->objects.cell_values[0]));
  memcpy(runtime->objects.text_bytes, runtime->objects.base_text_bytes,
         runtime->objects.base_used_text_bytes *
             sizeof(runtime->objects.text_bytes[0]));
#if FR_FEATURE_RECORDS
  memcpy(runtime->objects.record_names, runtime->objects.base_record_names,
         runtime->objects.base_used_record_names *
             sizeof(runtime->objects.record_names[0]));
  memcpy(runtime->objects.record_shape_fields,
         runtime->objects.base_record_shape_fields,
         runtime->objects.base_used_record_shape_fields *
             sizeof(runtime->objects.record_shape_fields[0]));
  memcpy(runtime->objects.record_values, runtime->objects.base_record_values,
         runtime->objects.base_used_record_values *
             sizeof(runtime->objects.record_values[0]));
  memcpy(runtime->objects.record_name_bytes,
         runtime->objects.base_record_name_bytes,
         runtime->objects.base_used_record_name_bytes *
             sizeof(runtime->objects.record_name_bytes[0]));
#endif
  fr_object_zero_entries(runtime->objects.entries, runtime->objects.count);
  fr_object_zero_cells(runtime->objects.cell_values,
                       runtime->objects.used_cell_words);
  fr_object_zero_text(runtime->objects.text_bytes,
                      runtime->objects.used_text_bytes);
#if FR_FEATURE_RECORDS
  fr_object_zero_record_names(runtime->objects.record_names,
                              runtime->objects.used_record_names);
  fr_object_zero_record_shape_fields(
      runtime->objects.record_shape_fields,
      runtime->objects.used_record_shape_fields);
  fr_object_zero_record_values(runtime->objects.record_values,
                               runtime->objects.used_record_values);
  fr_object_zero_record_name_bytes(runtime->objects.record_name_bytes,
                                   runtime->objects.used_record_name_bytes);
#endif
  memset(runtime->objects.overlay, 0, sizeof(runtime->objects.overlay));
#endif
}

void fr_object_clear_overlay(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }

#if !FR_FEATURE_OBJECTS
  memset(&runtime->objects, 0, sizeof(runtime->objects));
#else
  memcpy(runtime->objects.entries, runtime->objects.base_entries,
         runtime->objects.base_count * sizeof(runtime->objects.entries[0]));
  memcpy(runtime->objects.cell_values, runtime->objects.base_cell_values,
         runtime->objects.base_used_cell_words *
             sizeof(runtime->objects.cell_values[0]));
  memcpy(runtime->objects.text_bytes, runtime->objects.base_text_bytes,
         runtime->objects.base_used_text_bytes *
             sizeof(runtime->objects.text_bytes[0]));
#if FR_FEATURE_RECORDS
  memcpy(runtime->objects.record_names, runtime->objects.base_record_names,
         runtime->objects.base_used_record_names *
             sizeof(runtime->objects.record_names[0]));
  memcpy(runtime->objects.record_shape_fields,
         runtime->objects.base_record_shape_fields,
         runtime->objects.base_used_record_shape_fields *
             sizeof(runtime->objects.record_shape_fields[0]));
  memcpy(runtime->objects.record_values, runtime->objects.base_record_values,
         runtime->objects.base_used_record_values *
             sizeof(runtime->objects.record_values[0]));
  memcpy(runtime->objects.record_name_bytes,
         runtime->objects.base_record_name_bytes,
         runtime->objects.base_used_record_name_bytes *
             sizeof(runtime->objects.record_name_bytes[0]));
#endif
  runtime->objects.count = runtime->objects.image_count;
  runtime->objects.used_cell_words = runtime->objects.image_used_cell_words;
  runtime->objects.used_text_bytes = runtime->objects.image_used_text_bytes;
#if FR_FEATURE_RECORDS
  runtime->objects.used_record_names =
      runtime->objects.image_used_record_names;
  runtime->objects.used_record_shape_fields =
      runtime->objects.image_used_record_shape_fields;
  runtime->objects.used_record_values =
      runtime->objects.image_used_record_values;
  runtime->objects.used_record_name_bytes =
      runtime->objects.image_used_record_name_bytes;
#endif
  fr_object_zero_entries(runtime->objects.entries, runtime->objects.count);
  fr_object_zero_cells(runtime->objects.cell_values,
                       runtime->objects.used_cell_words);
  fr_object_zero_text(runtime->objects.text_bytes,
                      runtime->objects.used_text_bytes);
#if FR_FEATURE_RECORDS
  fr_object_zero_record_names(runtime->objects.record_names,
                              runtime->objects.used_record_names);
  fr_object_zero_record_shape_fields(
      runtime->objects.record_shape_fields,
      runtime->objects.used_record_shape_fields);
  fr_object_zero_record_values(runtime->objects.record_values,
                               runtime->objects.used_record_values);
  fr_object_zero_record_name_bytes(runtime->objects.record_name_bytes,
                                   runtime->objects.used_record_name_bytes);
#endif
  memset(runtime->objects.overlay, 0,
         runtime->objects.image_count * sizeof(runtime->objects.overlay[0]));
  if (runtime->objects.image_count < FR_OBJECT_TABLE_CAPACITY) {
    memset(&runtime->objects.overlay[runtime->objects.image_count], 0,
           (FR_OBJECT_TABLE_CAPACITY - runtime->objects.image_count) *
               sizeof(runtime->objects.overlay[0]));
  }
#endif
}

static fr_err_t fr_object_entry_of_kind(const fr_runtime_t *runtime,
                                        fr_object_id_t object_id,
                                        fr_object_kind_t kind,
                                        const fr_object_entry_t **out_entry) {
#if !FR_FEATURE_OBJECTS
  (void)runtime;
  (void)object_id;
  (void)kind;
  (void)out_entry;
  return FR_ERR_UNSUPPORTED;
#else
  const fr_object_entry_t *entry = NULL;

  if (runtime == NULL || out_entry == NULL) {
    return FR_ERR_INVALID;
  }
  if (object_id >= runtime->objects.count) {
    return FR_ERR_NOT_FOUND;
  }
  entry = &runtime->objects.entries[object_id];
  if (entry->kind != kind) {
    return FR_ERR_TYPE;
  }
  *out_entry = entry;
  return FR_OK;
#endif
}

bool fr_cells_value_allowed(const fr_runtime_t *runtime, fr_tagged_t tagged) {
  fr_int_t ignored_int = 0;
  fr_object_id_t object_id = 0;
  const uint8_t *ignored_bytes = NULL;
  uint16_t ignored_length = 0;
#if FR_FEATURE_RECORDS
  fr_object_id_t ignored_shape = 0;
#endif

  if (fr_tagged_is_nil(tagged) || fr_tagged_is_bool(tagged)) {
    return true;
  }
  if (fr_tagged_decode_int(tagged, &ignored_int) == FR_OK) {
    return true;
  }
  if (fr_tagged_decode_object_id(tagged, &object_id) != FR_OK) {
    return false;
  }
#if FR_FEATURE_TEXT
  if (fr_text_view(runtime, object_id, &ignored_bytes, &ignored_length) ==
      FR_OK) {
    return true;
  }
#if FR_FEATURE_RECORDS
  return fr_record_view(runtime, object_id, &ignored_shape, &ignored_length) ==
         FR_OK;
#else
  return false;
#endif
#else
  (void)ignored_bytes;
  (void)ignored_length;
#if FR_FEATURE_RECORDS
  return fr_record_view(runtime, object_id, &ignored_shape, &ignored_length) ==
         FR_OK;
#else
  (void)runtime;
  (void)object_id;
  return false;
#endif
#endif
}

fr_err_t fr_cells_check_install(const fr_runtime_t *runtime, uint16_t length,
                                const fr_tagged_t initial_values[]) {
#if !FR_FEATURE_CELLS
  (void)runtime;
  (void)length;
  (void)initial_values;
  return FR_ERR_UNSUPPORTED;
#else
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (length == 0 || length > FR_PROFILE_MAX_CELL_LENGTH) {
    return FR_ERR_RANGE;
  }
  if (runtime->objects.count >= FR_PROFILE_OBJECT_TABLE_SIZE ||
      (uint32_t)runtime->objects.used_cell_words + length >
          FR_PROFILE_MAX_CELL_WORDS) {
    return FR_ERR_CAPACITY;
  }
  if (initial_values != NULL) {
    for (uint16_t i = 0; i < length; i++) {
      if (!fr_cells_value_allowed(runtime, initial_values[i])) {
        return FR_ERR_TYPE;
      }
    }
  }
  return FR_OK;
#endif
}

fr_err_t fr_cells_install(fr_runtime_t *runtime, uint16_t length,
                          const fr_tagged_t initial_values[],
                          fr_object_id_t *out_object_id) {
#if !FR_FEATURE_CELLS
  (void)runtime;
  (void)length;
  (void)initial_values;
  (void)out_object_id;
  return FR_ERR_UNSUPPORTED;
#else
  fr_object_entry_t *entry = NULL;
  uint16_t first_cell = 0;

  if (out_object_id == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_cells_check_install(runtime, length, initial_values));

  *out_object_id = runtime->objects.count;
  first_cell = runtime->objects.used_cell_words;
  entry = &runtime->objects.entries[*out_object_id];
  *entry = (fr_object_entry_t){
      .kind = FR_OBJECT_CELLS,
      .first = first_cell,
      .length = length,
  };

  for (uint16_t i = 0; i < length; i++) {
    runtime->objects.cell_values[first_cell + i] =
        initial_values == NULL ? fr_tagged_nil() : initial_values[i];
  }
  runtime->objects.overlay[*out_object_id] = true;
  runtime->objects.count = (uint16_t)(runtime->objects.count + 1);
  runtime->objects.used_cell_words =
      (uint16_t)(runtime->objects.used_cell_words + length);
  return FR_OK;
#endif
}

fr_err_t fr_cells_length(const fr_runtime_t *runtime,
                         fr_object_id_t object_id, uint16_t *out_length) {
  const fr_object_entry_t *entry = NULL;

  if (out_length == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_object_entry_of_kind(runtime, object_id, FR_OBJECT_CELLS, &entry));
  *out_length = entry->length;
  return FR_OK;
}

fr_err_t fr_cells_read(const fr_runtime_t *runtime, fr_object_id_t object_id,
                       uint16_t index, fr_tagged_t *out_tagged) {
  const fr_object_entry_t *entry = NULL;

  if (out_tagged == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_object_entry_of_kind(runtime, object_id, FR_OBJECT_CELLS, &entry));
  if (index >= entry->length) {
    return FR_ERR_RANGE;
  }

  *out_tagged = runtime->objects.cell_values[entry->first + index];
  return FR_OK;
}

fr_err_t fr_cells_write(fr_runtime_t *runtime, fr_object_id_t object_id,
                        uint16_t index, fr_tagged_t tagged) {
  const fr_object_entry_t *entry = NULL;
  fr_bytes_ref_t bytes_ref = {0};

  FR_TRY(fr_object_entry_of_kind(runtime, object_id, FR_OBJECT_CELLS, &entry));
  if (index >= entry->length) {
    return FR_ERR_RANGE;
  }
  if (fr_tagged_decode_bytes_ref(tagged, &bytes_ref) == FR_OK) {
    return FR_ERR_VOLATILE;
  }
  if (!fr_cells_value_allowed(runtime, tagged)) {
    return FR_ERR_TYPE;
  }

  runtime->objects.cell_values[entry->first + index] = tagged;
  runtime->objects.overlay[object_id] = true;
  return FR_OK;
}

#if FR_FEATURE_TEXT
static fr_err_t fr_text_check_input(const uint8_t *bytes, uint16_t length) {
  if (length > 0 && bytes == NULL) {
    return FR_ERR_INVALID;
  }
  if (length > FR_PROFILE_MAX_TEXT_LENGTH) {
    return FR_ERR_RANGE;
  }
  return FR_OK;
}
#endif

fr_err_t fr_text_find(const fr_runtime_t *runtime, const uint8_t *bytes,
                      uint16_t length, fr_object_id_t first_object_id,
                      fr_object_id_t *out_object_id) {
#if !FR_FEATURE_TEXT
  (void)runtime;
  (void)bytes;
  (void)length;
  (void)first_object_id;
  (void)out_object_id;
  return FR_ERR_UNSUPPORTED;
#else
  if (runtime == NULL || out_object_id == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_text_check_input(bytes, length));

  for (fr_object_id_t object_id = first_object_id;
       object_id < runtime->objects.count; object_id++) {
    const fr_object_entry_t *entry = &runtime->objects.entries[object_id];

    if (entry->kind != FR_OBJECT_TEXT || entry->length != length) {
      continue;
    }
    if (length == 0 ||
        memcmp(&runtime->objects.text_bytes[entry->first], bytes, length) ==
            0) {
      *out_object_id = object_id;
      return FR_OK;
    }
  }
  return FR_ERR_NOT_FOUND;
#endif
}

fr_err_t fr_text_check_install(const fr_runtime_t *runtime,
                               const uint8_t *bytes, uint16_t length) {
#if !FR_FEATURE_TEXT
  (void)runtime;
  (void)bytes;
  (void)length;
  return FR_ERR_UNSUPPORTED;
#else
  fr_object_id_t ignored = 0;
  fr_err_t find_err = FR_OK;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_text_check_input(bytes, length));

  find_err = fr_text_find(runtime, bytes, length, 0, &ignored);
  if (find_err == FR_OK) {
    return FR_OK;
  }
  if (find_err != FR_ERR_NOT_FOUND) {
    return find_err;
  }
  if (runtime->objects.count >= FR_PROFILE_OBJECT_TABLE_SIZE ||
      (uint32_t)runtime->objects.used_text_bytes + length >
          FR_PROFILE_MAX_TEXT_BYTES) {
    return FR_ERR_CAPACITY;
  }
  return FR_OK;
#endif
}

fr_err_t fr_text_install_since(fr_runtime_t *runtime, const uint8_t *bytes,
                               uint16_t length,
                               fr_object_id_t first_object_id,
                               fr_object_id_t *out_object_id) {
#if !FR_FEATURE_TEXT
  (void)runtime;
  (void)bytes;
  (void)length;
  (void)first_object_id;
  (void)out_object_id;
  return FR_ERR_UNSUPPORTED;
#else
  fr_object_entry_t *entry = NULL;
  fr_err_t find_err = FR_OK;
  fr_object_id_t existing_id = 0;
  uint16_t first_byte = 0;

  if (runtime == NULL || out_object_id == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_text_check_input(bytes, length));
  find_err = fr_text_find(runtime, bytes, length, first_object_id,
                          &existing_id);
  if (find_err == FR_OK) {
    *out_object_id = existing_id;
    return FR_OK;
  }
  if (find_err != FR_ERR_NOT_FOUND) {
    return find_err;
  }
  if (runtime->objects.count >= FR_PROFILE_OBJECT_TABLE_SIZE ||
      (uint32_t)runtime->objects.used_text_bytes + length >
          FR_PROFILE_MAX_TEXT_BYTES) {
    return FR_ERR_CAPACITY;
  }

  *out_object_id = runtime->objects.count;
  first_byte = runtime->objects.used_text_bytes;
  entry = &runtime->objects.entries[*out_object_id];
  *entry = (fr_object_entry_t){
      .kind = FR_OBJECT_TEXT,
      .first = first_byte,
      .length = length,
  };
  if (length > 0) {
    memcpy(&runtime->objects.text_bytes[first_byte], bytes, length);
  }
  runtime->objects.overlay[*out_object_id] = true;
  runtime->objects.count = (uint16_t)(runtime->objects.count + 1);
  runtime->objects.used_text_bytes =
      (uint16_t)(runtime->objects.used_text_bytes + length);
  return FR_OK;
#endif
}

fr_err_t fr_text_install(fr_runtime_t *runtime, const uint8_t *bytes,
                         uint16_t length, fr_object_id_t *out_object_id) {
  return fr_text_install_since(runtime, bytes, length, 0, out_object_id);
}

fr_err_t fr_text_view(const fr_runtime_t *runtime, fr_object_id_t object_id,
                      const uint8_t **out_bytes, uint16_t *out_length) {
#if !FR_FEATURE_TEXT
  (void)runtime;
  (void)object_id;
  (void)out_bytes;
  (void)out_length;
  return FR_ERR_UNSUPPORTED;
#else
  const fr_object_entry_t *entry = NULL;

  if (out_bytes == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_object_entry_of_kind(runtime, object_id, FR_OBJECT_TEXT, &entry));
  *out_bytes = &runtime->objects.text_bytes[entry->first];
  *out_length = entry->length;
  return FR_OK;
#endif
}

#if FR_FEATURE_RECORDS
static bool fr_record_name_same(fr_record_name_t lhs, fr_record_name_t rhs) {
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

static fr_err_t fr_record_check_name(fr_record_name_t name) {
  if (name.length == 0 || name.length > FR_PROFILE_MAX_NAME_BYTES) {
    return FR_ERR_RANGE;
  }
  if (name.bytes == NULL) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < name.length; i++) {
    if (name.bytes[i] == '\0' || name.bytes[i] == '.') {
      return FR_ERR_INVALID;
    }
  }
  return FR_OK;
}

static fr_err_t fr_record_name_entry_view(const fr_runtime_t *runtime,
                                          uint16_t name_entry,
                                          fr_record_name_t *out_name) {
  const fr_record_name_entry_t *entry = NULL;

  if (runtime == NULL || out_name == NULL) {
    return FR_ERR_INVALID;
  }
  if (name_entry >= runtime->objects.used_record_names) {
    return FR_ERR_CORRUPT;
  }
  entry = &runtime->objects.record_names[name_entry];
  if ((uint32_t)entry->first + entry->length >
      runtime->objects.used_record_name_bytes) {
    return FR_ERR_CORRUPT;
  }
  *out_name = (fr_record_name_t){
      .bytes = &runtime->objects.record_name_bytes[entry->first],
      .length = entry->length,
  };
  return FR_OK;
}

static fr_err_t fr_record_store_name(fr_runtime_t *runtime,
                                     fr_record_name_t name,
                                     uint16_t *out_name_entry) {
  uint16_t entry_index = 0;
  uint16_t first_byte = 0;

  if (runtime == NULL || out_name_entry == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_record_check_name(name));
  if (runtime->objects.used_record_names >= FR_RECORD_NAME_ENTRY_CAPACITY ||
      (uint32_t)runtime->objects.used_record_name_bytes + name.length >
          FR_PROFILE_MAX_RECORD_NAME_BYTES) {
    return FR_ERR_CAPACITY;
  }

  entry_index = runtime->objects.used_record_names;
  first_byte = runtime->objects.used_record_name_bytes;
  runtime->objects.record_names[entry_index] =
      (fr_record_name_entry_t){.first = first_byte, .length = name.length};
  memcpy(&runtime->objects.record_name_bytes[first_byte], name.bytes,
         name.length);
  runtime->objects.used_record_names =
      (uint16_t)(runtime->objects.used_record_names + 1);
  runtime->objects.used_record_name_bytes =
      (uint16_t)(runtime->objects.used_record_name_bytes + name.length);
  *out_name_entry = entry_index;
  return FR_OK;
}

static fr_err_t
fr_record_check_shape_input(fr_record_name_t name,
                            const fr_record_name_t fields[],
                            uint16_t field_count) {
  if (fields == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_record_check_name(name));
  if (field_count == 0 ||
      field_count > FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE) {
    return FR_ERR_RANGE;
  }
  for (uint16_t i = 0; i < field_count; i++) {
    FR_TRY(fr_record_check_name(fields[i]));
    for (uint16_t j = 0; j < i; j++) {
      if (fr_record_name_same(fields[i], fields[j])) {
        return FR_ERR_INVALID;
      }
    }
  }
  return FR_OK;
}

static fr_err_t fr_record_shape_same(const fr_runtime_t *runtime,
                                     fr_object_id_t object_id,
                                     fr_record_name_t name,
                                     const fr_record_name_t fields[],
                                     uint16_t field_count,
                                     bool *out_same) {
  fr_record_name_t stored_name = {0};
  uint16_t stored_field_count = 0;

  if (runtime == NULL || fields == NULL || out_same == NULL) {
    return FR_ERR_INVALID;
  }
  *out_same = false;
  if (fr_record_shape_view(runtime, object_id, &stored_name,
                           &stored_field_count) != FR_OK ||
      stored_field_count != field_count ||
      !fr_record_name_same(stored_name, name)) {
    return FR_OK;
  }
  for (uint16_t i = 0; i < field_count; i++) {
    fr_record_name_t stored_field = {0};

    FR_TRY(fr_record_shape_field_name(runtime, object_id, i, &stored_field));
    if (!fr_record_name_same(stored_field, fields[i])) {
      return FR_OK;
    }
  }
  *out_same = true;
  return FR_OK;
}
#endif

bool fr_record_field_value_allowed(const fr_runtime_t *runtime,
                                   fr_tagged_t tagged) {
#if !FR_FEATURE_RECORDS
  (void)runtime;
  (void)tagged;
  return false;
#else
  fr_int_t ignored_int = 0;
  fr_object_id_t object_id = 0;
  const uint8_t *ignored_bytes = NULL;
  uint16_t ignored_length = 0;

  if (fr_tagged_is_nil(tagged) || fr_tagged_is_bool(tagged)) {
    return true;
  }
  if (fr_tagged_decode_int(tagged, &ignored_int) == FR_OK) {
    return true;
  }
  if (fr_tagged_decode_object_id(tagged, &object_id) != FR_OK) {
    return false;
  }
  return fr_text_view(runtime, object_id, &ignored_bytes, &ignored_length) ==
         FR_OK;
#endif
}

fr_err_t fr_record_shape_find(const fr_runtime_t *runtime,
                              fr_record_name_t name,
                              const fr_record_name_t fields[],
                              uint16_t field_count,
                              fr_object_id_t first_object_id,
                              fr_object_id_t *out_object_id) {
#if !FR_FEATURE_RECORDS
  (void)runtime;
  (void)name;
  (void)fields;
  (void)field_count;
  (void)first_object_id;
  (void)out_object_id;
  return FR_ERR_UNSUPPORTED;
#else
  if (runtime == NULL || out_object_id == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_record_check_shape_input(name, fields, field_count));
  for (fr_object_id_t object_id = first_object_id;
       object_id < runtime->objects.count; object_id++) {
    bool same = false;
    if (runtime->objects.entries[object_id].kind != FR_OBJECT_RECORD_SHAPE) {
      continue;
    }
    FR_TRY(fr_record_shape_same(runtime, object_id, name, fields, field_count,
                                &same));
    if (same) {
      *out_object_id = object_id;
      return FR_OK;
    }
  }
  return FR_ERR_NOT_FOUND;
#endif
}

fr_err_t fr_record_shape_install_since(fr_runtime_t *runtime,
                                       fr_record_name_t name,
                                       const fr_record_name_t fields[],
                                       uint16_t field_count,
                                       fr_object_id_t first_object_id,
                                       fr_object_id_t *out_object_id) {
#if !FR_FEATURE_RECORDS
  (void)runtime;
  (void)name;
  (void)fields;
  (void)field_count;
  (void)first_object_id;
  (void)out_object_id;
  return FR_ERR_UNSUPPORTED;
#else
  fr_object_entry_t *entry = NULL;
  fr_err_t find_err = FR_OK;
  fr_object_id_t existing_id = 0;
  uint16_t shape_name_entry = 0;
  uint16_t first_field = 0;

  if (runtime == NULL || out_object_id == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_record_check_shape_input(name, fields, field_count));
  find_err =
      fr_record_shape_find(runtime, name, fields, field_count, first_object_id,
                           &existing_id);
  if (find_err == FR_OK) {
    *out_object_id = existing_id;
    return FR_OK;
  }
  if (find_err != FR_ERR_NOT_FOUND) {
    return find_err;
  }
  if (runtime->objects.count >= FR_PROFILE_OBJECT_TABLE_SIZE ||
      runtime->objects.used_record_shape_fields + field_count >
          FR_PROFILE_MAX_RECORD_SHAPE_FIELDS ||
      runtime->objects.used_record_names + 1u + field_count >
          FR_RECORD_NAME_ENTRY_CAPACITY) {
    return FR_ERR_CAPACITY;
  }
  {
    uint32_t name_bytes = name.length;
    for (uint16_t i = 0; i < field_count; i++) {
      name_bytes += fields[i].length;
    }
    if ((uint32_t)runtime->objects.used_record_name_bytes + name_bytes >
        FR_PROFILE_MAX_RECORD_NAME_BYTES) {
      return FR_ERR_CAPACITY;
    }
  }

  FR_TRY(fr_record_store_name(runtime, name, &shape_name_entry));
  first_field = runtime->objects.used_record_shape_fields;
  for (uint16_t i = 0; i < field_count; i++) {
    uint16_t field_name_entry = 0;

    FR_TRY(fr_record_store_name(runtime, fields[i], &field_name_entry));
    runtime->objects.record_shape_fields[first_field + i] = field_name_entry;
  }
  runtime->objects.used_record_shape_fields =
      (uint16_t)(runtime->objects.used_record_shape_fields + field_count);

  *out_object_id = runtime->objects.count;
  entry = &runtime->objects.entries[*out_object_id];
  *entry = (fr_object_entry_t){
      .kind = FR_OBJECT_RECORD_SHAPE,
      .first = first_field,
      .length = field_count,
      .aux = shape_name_entry,
  };
  runtime->objects.overlay[*out_object_id] = true;
  runtime->objects.count = (uint16_t)(runtime->objects.count + 1);
  return FR_OK;
#endif
}

fr_err_t fr_record_shape_install(fr_runtime_t *runtime, fr_record_name_t name,
                                 const fr_record_name_t fields[],
                                 uint16_t field_count,
                                 fr_object_id_t *out_object_id) {
  return fr_record_shape_install_since(runtime, name, fields, field_count, 0,
                                       out_object_id);
}

fr_err_t fr_record_shape_view(const fr_runtime_t *runtime,
                              fr_object_id_t object_id,
                              fr_record_name_t *out_name,
                              uint16_t *out_field_count) {
#if !FR_FEATURE_RECORDS
  (void)runtime;
  (void)object_id;
  (void)out_name;
  (void)out_field_count;
  return FR_ERR_UNSUPPORTED;
#else
  const fr_object_entry_t *entry = NULL;

  if (out_name == NULL || out_field_count == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_object_entry_of_kind(runtime, object_id, FR_OBJECT_RECORD_SHAPE,
                                 &entry));
  FR_TRY(fr_record_name_entry_view(runtime, entry->aux, out_name));
  *out_field_count = entry->length;
  return FR_OK;
#endif
}

fr_err_t fr_record_shape_field_name(const fr_runtime_t *runtime,
                                    fr_object_id_t shape_object_id,
                                    uint16_t field_index,
                                    fr_record_name_t *out_field) {
#if !FR_FEATURE_RECORDS
  (void)runtime;
  (void)shape_object_id;
  (void)field_index;
  (void)out_field;
  return FR_ERR_UNSUPPORTED;
#else
  const fr_object_entry_t *entry = NULL;
  uint16_t name_entry = 0;

  if (out_field == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_object_entry_of_kind(runtime, shape_object_id,
                                 FR_OBJECT_RECORD_SHAPE, &entry));
  if (field_index >= entry->length) {
    return FR_ERR_RANGE;
  }
  name_entry = runtime->objects.record_shape_fields[entry->first + field_index];
  return fr_record_name_entry_view(runtime, name_entry, out_field);
#endif
}

fr_err_t fr_record_shape_field_index(const fr_runtime_t *runtime,
                                     fr_object_id_t shape_object_id,
                                     fr_record_name_t field,
                                     uint16_t *out_index) {
#if !FR_FEATURE_RECORDS
  (void)runtime;
  (void)shape_object_id;
  (void)field;
  (void)out_index;
  return FR_ERR_UNSUPPORTED;
#else
  uint16_t field_count = 0;
  fr_record_name_t ignored_name = {0};

  if (out_index == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_record_check_name(field));
  FR_TRY(fr_record_shape_view(runtime, shape_object_id, &ignored_name,
                              &field_count));
  for (uint16_t i = 0; i < field_count; i++) {
    fr_record_name_t stored = {0};

    FR_TRY(fr_record_shape_field_name(runtime, shape_object_id, i, &stored));
    if (fr_record_name_same(stored, field)) {
      *out_index = i;
      return FR_OK;
    }
  }
  return FR_ERR_NOT_FOUND;
#endif
}

fr_err_t fr_record_install(fr_runtime_t *runtime,
                           fr_object_id_t shape_object_id,
                           const fr_tagged_t field_values[],
                           uint16_t field_count,
                           fr_object_id_t *out_object_id) {
#if !FR_FEATURE_RECORDS
  (void)runtime;
  (void)shape_object_id;
  (void)field_values;
  (void)field_count;
  (void)out_object_id;
  return FR_ERR_UNSUPPORTED;
#else
  fr_object_entry_t *entry = NULL;
  fr_record_name_t ignored_name = {0};
  uint16_t shape_field_count = 0;
  uint16_t first_field = 0;

  if (runtime == NULL || out_object_id == NULL ||
      (field_values == NULL && field_count > 0)) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_record_shape_view(runtime, shape_object_id, &ignored_name,
                              &shape_field_count));
  if (field_count != shape_field_count) {
    return FR_ERR_INVALID;
  }
  if (runtime->objects.count >= FR_PROFILE_OBJECT_TABLE_SIZE ||
      (uint32_t)runtime->objects.used_record_values + field_count >
          FR_PROFILE_MAX_RECORD_VALUE_FIELDS) {
    return FR_ERR_CAPACITY;
  }
  for (uint16_t i = 0; i < field_count; i++) {
    if (!fr_record_field_value_allowed(runtime, field_values[i])) {
      return FR_ERR_TYPE;
    }
  }

  *out_object_id = runtime->objects.count;
  first_field = runtime->objects.used_record_values;
  entry = &runtime->objects.entries[*out_object_id];
  *entry = (fr_object_entry_t){
      .kind = FR_OBJECT_RECORD,
      .first = first_field,
      .length = field_count,
      .aux = shape_object_id,
  };
  for (uint16_t i = 0; i < field_count; i++) {
    runtime->objects.record_values[first_field + i] = field_values[i];
  }
  runtime->objects.overlay[*out_object_id] = true;
  runtime->objects.count = (uint16_t)(runtime->objects.count + 1);
  runtime->objects.used_record_values =
      (uint16_t)(runtime->objects.used_record_values + field_count);
  return FR_OK;
#endif
}

fr_err_t fr_record_view(const fr_runtime_t *runtime, fr_object_id_t object_id,
                        fr_object_id_t *out_shape_object_id,
                        uint16_t *out_field_count) {
#if !FR_FEATURE_RECORDS
  (void)runtime;
  (void)object_id;
  (void)out_shape_object_id;
  (void)out_field_count;
  return FR_ERR_UNSUPPORTED;
#else
  const fr_object_entry_t *entry = NULL;

  if (out_shape_object_id == NULL || out_field_count == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_object_entry_of_kind(runtime, object_id, FR_OBJECT_RECORD,
                                 &entry));
  *out_shape_object_id = entry->aux;
  *out_field_count = entry->length;
  return FR_OK;
#endif
}

fr_err_t fr_record_read_index(const fr_runtime_t *runtime,
                              fr_object_id_t object_id, uint16_t field_index,
                              fr_tagged_t *out_tagged) {
#if !FR_FEATURE_RECORDS
  (void)runtime;
  (void)object_id;
  (void)field_index;
  (void)out_tagged;
  return FR_ERR_UNSUPPORTED;
#else
  const fr_object_entry_t *entry = NULL;

  if (out_tagged == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_object_entry_of_kind(runtime, object_id, FR_OBJECT_RECORD,
                                 &entry));
  if (field_index >= entry->length) {
    return FR_ERR_RANGE;
  }
  *out_tagged = runtime->objects.record_values[entry->first + field_index];
  return FR_OK;
#endif
}

fr_err_t fr_record_read_field(const fr_runtime_t *runtime,
                              fr_object_id_t object_id,
                              fr_record_name_t field,
                              fr_tagged_t *out_tagged) {
#if !FR_FEATURE_RECORDS
  (void)runtime;
  (void)object_id;
  (void)field;
  (void)out_tagged;
  return FR_ERR_UNSUPPORTED;
#else
  fr_object_id_t shape_object_id = 0;
  uint16_t field_count = 0;
  uint16_t field_index = 0;

  FR_TRY(fr_record_view(runtime, object_id, &shape_object_id, &field_count));
  (void)field_count;
  FR_TRY(fr_record_shape_field_index(runtime, shape_object_id, field,
                                     &field_index));
  return fr_record_read_index(runtime, object_id, field_index, out_tagged);
#endif
}

fr_err_t fr_record_write_field(fr_runtime_t *runtime,
                               fr_object_id_t object_id,
                               fr_record_name_t field,
                               fr_tagged_t tagged) {
#if !FR_FEATURE_RECORDS
  (void)runtime;
  (void)object_id;
  (void)field;
  (void)tagged;
  return FR_ERR_UNSUPPORTED;
#else
  const fr_object_entry_t *entry = NULL;
  fr_object_id_t shape_object_id = 0;
  uint16_t field_count = 0;
  uint16_t field_index = 0;
  fr_bytes_ref_t bytes_ref = {0};

  FR_TRY(fr_object_entry_of_kind(runtime, object_id, FR_OBJECT_RECORD,
                                 &entry));
  shape_object_id = entry->aux;
  field_count = entry->length;
  (void)field_count;
  FR_TRY(fr_record_shape_field_index(runtime, shape_object_id, field,
                                     &field_index));
  if (fr_tagged_decode_bytes_ref(tagged, &bytes_ref) == FR_OK) {
    return FR_ERR_VOLATILE;
  }
  if (!fr_record_field_value_allowed(runtime, tagged)) {
    return FR_ERR_TYPE;
  }
  runtime->objects.record_values[entry->first + field_index] = tagged;
  runtime->objects.overlay[object_id] = true;
  return FR_OK;
#endif
}

fr_object_id_t fr_object_count(const fr_runtime_t *runtime) {
#if !FR_FEATURE_OBJECTS
  (void)runtime;
  return 0;
#else
  return runtime == NULL ? 0 : runtime->objects.count;
#endif
}

bool fr_object_is_overlay(const fr_runtime_t *runtime,
                          fr_object_id_t object_id) {
#if !FR_FEATURE_OBJECTS
  (void)runtime;
  (void)object_id;
  return false;
#else
  if (runtime == NULL || object_id >= runtime->objects.count) {
    return false;
  }
  return runtime->objects.overlay[object_id];
#endif
}

fr_err_t fr_bytes_install(fr_runtime_t *runtime, const uint8_t *bytes,
                          uint16_t length, fr_tagged_t *out_tagged) {
#if FR_FEATURE_BYTES
  if (runtime == NULL || out_tagged == NULL) {
    return FR_ERR_INVALID;
  }
  if (length > 0 && bytes == NULL) {
    return FR_ERR_INVALID;
  }
  if ((uint32_t)runtime->bytes.arena_used + length >
      FR_PROFILE_BYTES_ARENA_BYTES) {
    return FR_ERR_CAPACITY;
  }

  for (fr_bytes_id_t i = 0; i < FR_PROFILE_BYTES_COUNT; i++) {
    fr_bytes_entry_t *entry = &runtime->bytes.entries[i];

    if (entry->in_use || entry->retired) {
      continue;
    }
    if (entry->generation == FR_TAGGED_BYTES_MAX_GENERATION) {
      entry->retired = true;
      continue;
    }

    entry->generation = (fr_bytes_generation_t)(entry->generation + 1u);
    entry->offset = runtime->bytes.arena_used;
    entry->length = length;
    entry->in_use = true;
    if (length > 0) {
      memcpy(&runtime->bytes.arena[runtime->bytes.arena_used], bytes, length);
    }
    runtime->bytes.arena_used =
        (uint16_t)(runtime->bytes.arena_used + length);

    fr_bytes_ref_t ref = {.id = i, .generation = entry->generation};
    return fr_tagged_encode_bytes_ref(ref, out_tagged);
  }
  return FR_ERR_CAPACITY;
#else
  (void)runtime;
  (void)bytes;
  (void)length;
  (void)out_tagged;
  return FR_ERR_UNSUPPORTED;
#endif
}

fr_err_t fr_bytes_view(const fr_runtime_t *runtime, fr_bytes_ref_t ref,
                       const uint8_t **out_bytes, uint16_t *out_length) {
#if FR_FEATURE_BYTES
  const fr_bytes_entry_t *entry = NULL;

  if (runtime == NULL || out_bytes == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }
  if (ref.id >= FR_PROFILE_BYTES_COUNT) {
    return FR_ERR_VOLATILE;
  }
  entry = &runtime->bytes.entries[ref.id];
  if (!entry->in_use || entry->generation != ref.generation) {
    return FR_ERR_VOLATILE;
  }
  *out_bytes = &runtime->bytes.arena[entry->offset];
  *out_length = entry->length;
  return FR_OK;
#else
  (void)runtime;
  (void)ref;
  (void)out_bytes;
  (void)out_length;
  return FR_ERR_UNSUPPORTED;
#endif
}
