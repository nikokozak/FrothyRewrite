#pragma once

#include "code.h"
#include "handle.h"
#include "native.h"
#include "object.h"
#include "pad.h"
#include "platform.h"
#include "tagged.h"
#include "types.h"

#include <stdbool.h>

typedef struct fr_runtime_limits_t {
  uint16_t max_slots;
  uint16_t max_instruction_bytes;
  uint16_t max_definition_instruction_bytes;
  uint16_t max_definition_text_bytes;
  uint16_t max_source_render_bytes;
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
  uint16_t max_attempt_depth;
} fr_runtime_limits_t;

typedef enum fr_slot_name_storage_kind_t {
  FR_SLOT_NAME_STORAGE_OVERLAY_RAM = 0,
  FR_SLOT_NAME_STORAGE_IMAGE = 1,
  FR_SLOT_NAME_STORAGE_PERSIST_IMAGE = 2,
} fr_slot_name_storage_kind_t;

#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
typedef struct fr_slot_project_name_entry_t {
  fr_slot_name_storage_kind_t storage_kind;
  fr_slot_id_t slot_id;
  const char *bytes;
  uint8_t length;
} fr_slot_project_name_entry_t;
#endif

/* Base is the installed reset tagged word; current is the live tagged word. */
typedef struct fr_slot_table_t {
  fr_tagged_t current[FR_PROFILE_MAX_SLOTS];
  fr_tagged_t base[FR_PROFILE_MAX_SLOTS];
  fr_tagged_t library_base[FR_PROFILE_MAX_SLOTS];
  bool overlay[FR_PROFILE_MAX_SLOTS];
  bool library_base_present[FR_PROFILE_MAX_SLOTS];
  fr_install_tier_t base_tier[FR_PROFILE_MAX_SLOTS];
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  /*
   * Overlay is the clearable runtime layer; project names are the public view
   * of that layer in profiles that keep live names on-device.
   */
  fr_slot_project_name_entry_t overlay_names[FR_PROFILE_MAX_OVERLAY_NAMES];
  char overlay_name_bytes[FR_PROFILE_MAX_OVERLAY_NAMES]
                         [FR_PROFILE_MAX_NAME_BYTES + 1];
  uint16_t overlay_name_count;
#endif
  uint16_t base_count;
  uint16_t count;
} fr_slot_table_t;

/* T11 fixes the live-binding capacity at 16 (spec §10). Matches the
 * platform queue depth so a full drain can land without losing
 * candidates inside the runtime. */
#define FR_EVENT_BINDING_COUNT 16

typedef struct fr_event_binding_t {
  /* kind == FR_EVENT_KIND_NONE marks the slot inactive. */
  fr_event_kind_t kind;
  bool pending;
  /* has_fired gates the debounce check; last_fire_ms = 0 is a valid candidate
   * timestamp so it cannot double as a never-fired sentinel. */
  bool has_fired;
  uint16_t source;
  uint16_t debounce_ms;
  uint16_t generation;
  fr_code_object_id_t body;
  uint32_t registered_at_ms;
  uint32_t last_fire_ms;
} fr_event_binding_t;

typedef struct fr_event_table_t {
  fr_event_binding_t entries[FR_EVENT_BINDING_COUNT];
  uint8_t active_count;
  uint32_t overflow_count;
  uint32_t overflow_reported_count;
} fr_event_table_t;

#if FR_FEATURE_BYTES && FR_PROFILE_BYTES_COUNT == 0
#error "FR_FEATURE_BYTES requires FR_PROFILE_BYTES_COUNT"
#endif

#if FR_FEATURE_BYTES && FR_PROFILE_BYTES_ARENA_BYTES == 0
#error "FR_FEATURE_BYTES requires FR_PROFILE_BYTES_ARENA_BYTES"
#endif

#if !FR_FEATURE_BYTES &&                                                     \
    (FR_PROFILE_BYTES_COUNT > 0 || FR_PROFILE_BYTES_ARENA_BYTES > 0)
#error "bytes profile limits require FR_FEATURE_BYTES"
#endif

#if FR_FEATURE_NET && !FR_FEATURE_BYTES
#error "FR_FEATURE_NET requires FR_FEATURE_BYTES"
#endif

#if FR_FEATURE_I2C && !FR_FEATURE_BYTES
#error "FR_FEATURE_I2C requires FR_FEATURE_BYTES"
#endif

#if FR_FEATURE_BYTES
#define FR_BYTES_TABLE_CAPACITY                                              \
  (FR_PROFILE_BYTES_COUNT > 0 ? FR_PROFILE_BYTES_COUNT : 1)

typedef struct fr_bytes_entry_t {
  uint16_t offset;
  uint16_t length;
  uint8_t generation;
  bool in_use;
  bool retired;
} fr_bytes_entry_t;

typedef struct fr_bytes_t {
  fr_bytes_entry_t entries[FR_BYTES_TABLE_CAPACITY];
  uint8_t arena[FR_PROFILE_BYTES_ARENA_BYTES > 0
                    ? FR_PROFILE_BYTES_ARENA_BYTES
                    : 1];
  uint16_t arena_used;
  uint16_t eval_depth;
} fr_bytes_t;
#endif

#if FR_FEATURE_NET
/* D5: per-TCP-handle state. The handle table's platform_index for a TCP
 * handle indexes this array. Target-specific OS state (the lwip fd on
 * ESP-IDF) lives in the platform layer (D17); the kernel-visible flag here
 * latches when the platform observes wifi_down on this socket so later
 * operations on the same handle return FR_ERR_NET_DISCONNECTED until the
 * user closes it explicitly (D12). */
typedef struct fr_tcp_handle_state_t {
  bool failed;
} fr_tcp_handle_state_t;
#endif

struct fr_runtime_t {
  fr_slot_table_t slots;
  fr_code_table_t code;
  fr_native_table_t natives;
  fr_object_table_t objects;
#if FR_FEATURE_HANDLES
  fr_handle_table_t handles;
#endif
#if FR_FEATURE_BYTES
  fr_bytes_t bytes;
#endif
#if FR_FEATURE_PAD
  fr_pad_t pad;
#endif
  fr_event_table_t events;
#if FR_FEATURE_NET
  fr_tcp_handle_state_t tcp_handles[FR_TCP_HANDLE_COUNT];
#endif
  bool interrupted;
  /* Set while fr_event_dispatch is running a body so the VM step loop does
     not re-enter dispatch from inside a handler (spec §5: no preemption). */
  bool dispatching_event;
  /* T12L-7 D3: session-scoped install tier. fr_repl_run resets to USER on
     entry; install-library / install-user REPL commands flip it; the persist
     encoder reads it when stamping new overlay records. */
  fr_install_tier_t install_tier;
  fr_err_t rescue_error;
  bool rescue_error_active;
  /* Borrowed for one REPL eval so VM/native faults can fill the same
     diagnostic object parse and compile already use. */
  fr_diagnostic_t *diag;
};

fr_err_t fr_runtime_init(fr_runtime_t *runtime);
fr_err_t fr_runtime_reset(fr_runtime_t *runtime);
fr_err_t fr_runtime_clear_project(fr_runtime_t *runtime);
void fr_runtime_interrupt(fr_runtime_t *runtime);
void fr_runtime_clear_interrupt(fr_runtime_t *runtime);
bool fr_runtime_is_interrupted(const fr_runtime_t *runtime);
fr_runtime_limits_t fr_runtime_get_limits(void);

#if FR_FEATURE_BYTES
void fr_bytes_init(fr_runtime_t *runtime);
void fr_bytes_reset_if_outermost(fr_runtime_t *runtime);
#endif
