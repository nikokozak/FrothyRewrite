#pragma once

#include "code.h"
#include "handle.h"
#include "native.h"
#include "object.h"
#include "pad.h"
#include "tagged.h"
#include "types.h"

#include <stdbool.h>

typedef struct fr_runtime_limits_t {
  uint16_t max_slots;
  uint16_t max_instruction_bytes;
  uint16_t max_code_objects;
  uint16_t max_natives;
  uint16_t max_handles;
  uint16_t max_objects;
  uint16_t max_cell_words;
  uint16_t max_text_bytes;
  uint16_t max_record_name_bytes;
  uint16_t max_record_fields_per_shape;
  uint16_t max_record_shape_fields;
  uint16_t max_record_value_fields;
  uint16_t max_pad_bytes;
} fr_runtime_limits_t;

/* Base is the installed reset tagged word; current is the live tagged word. */
typedef struct fr_slot_table_t {
  fr_tagged_t current[FR_PROFILE_MAX_SLOTS];
  fr_tagged_t base[FR_PROFILE_MAX_SLOTS];
  bool overlay[FR_PROFILE_MAX_SLOTS];
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  /*
   * Overlay is the clearable runtime layer; project names are the public view
   * of that layer in profiles that keep live names on-device.
   */
  char overlay_names[FR_PROFILE_MAX_OVERLAY_NAMES]
                    [FR_PROFILE_MAX_NAME_BYTES + 1];
  fr_slot_id_t overlay_name_slots[FR_PROFILE_MAX_OVERLAY_NAMES];
  uint16_t overlay_name_count;
#endif
  uint16_t base_count;
  uint16_t count;
} fr_slot_table_t;

struct fr_runtime_t {
  fr_slot_table_t slots;
  fr_code_table_t code;
  fr_native_table_t natives;
  fr_object_table_t objects;
#if FR_FEATURE_HANDLES
  fr_handle_table_t handles;
#endif
#if FR_FEATURE_PAD
  fr_pad_t pad;
#endif
  bool interrupted;
};

fr_err_t fr_runtime_init(fr_runtime_t *runtime);
fr_err_t fr_runtime_reset(fr_runtime_t *runtime);
fr_err_t fr_runtime_clear_project(fr_runtime_t *runtime);
void fr_runtime_interrupt(fr_runtime_t *runtime);
void fr_runtime_clear_interrupt(fr_runtime_t *runtime);
bool fr_runtime_is_interrupted(const fr_runtime_t *runtime);
fr_runtime_limits_t fr_runtime_get_limits(void);
