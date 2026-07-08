#include "persist_payload.h"

#if !FR_FEATURE_PERSISTENCE
#error "persist_payload.c requires FR_FEATURE_PERSISTENCE"
#endif

#include "base_defs.h"
#include "code.h"
#include "event.h"
#include "image.h"
#include "instruction.h"
#include "object.h"
#include "platform.h"
#include "slot.h"
#include "tagged.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum {
  FR_PERSIST_RECORD_CODE = 1,
  FR_PERSIST_RECORD_BIND = 2,
  FR_PERSIST_RECORD_NAME = 3,
  FR_PERSIST_RECORD_CELLS = 4,
  FR_PERSIST_RECORD_TEXT = 5,
  FR_PERSIST_RECORD_RECORD_SHAPE = 6,
  FR_PERSIST_RECORD_RECORD = 7,
  FR_PERSIST_RECORD_EVENT = 8,
  FR_PERSIST_RECORD_END = 0xff,
  FR_PERSIST_VALUE_NIL = 0,
  FR_PERSIST_VALUE_FALSE = 1,
  FR_PERSIST_VALUE_TRUE = 2,
  FR_PERSIST_VALUE_INT = 3,
  FR_PERSIST_VALUE_CODE = 4,
  FR_PERSIST_VALUE_NATIVE = 5,
  FR_PERSIST_VALUE_OBJECT = 6,
  FR_PERSIST_TIER_LIBRARY = 1,
  FR_PERSIST_TIER_USER = 2,
};

typedef enum fr_persist_object_record_kind_t {
  FR_PERSIST_OBJECT_NONE = 0,
  FR_PERSIST_OBJECT_CELLS = 1,
  FR_PERSIST_OBJECT_TEXT = 2,
  FR_PERSIST_OBJECT_RECORD_SHAPE = 3,
  FR_PERSIST_OBJECT_RECORD = 4,
} fr_persist_object_record_kind_t;

static const uint8_t fr_persist_payload_magic[4] = {'F', 'R', 'P', 'O'};

/* fr_persist_slot_tier holds the per-slot stamp the encoder emits on BIND
 * records. install-library by itself does not rewrite this table; only
 * stamp_overlay (called from the REPL overlay-apply path) writes new entries
 * with the current session install_tier, so flipping the tier mid-session
 * never relabels previously-installed slots. Per-slot stamping stays
 * file-static for the prototype; the session tier itself is per-runtime
 * (runtime->install_tier) per SPEC D3. */
static uint8_t fr_persist_slot_tier[FR_PROFILE_MAX_SLOTS];

/* D6 boot two-call sequence: the L2 pass must follow a successful L1 pass on
 * the same payload. Code and object ids are already final image ids, so no
 * restore-side id map carries between passes. */
static bool fr_persist_boot_library_valid;

/* Clear the per-slot tier stamps for a fresh image. The table is module-global
 * session state, not per-runtime, so installing a new base image must wipe it
 * or stamps from a prior runtime leak across (restore re-derives tiers from the
 * payload's BIND records, and the REPL re-stamps on each apply). */
void fr_persist_session_reset(void) {
  memset(fr_persist_slot_tier, 0, sizeof(fr_persist_slot_tier));
}

void fr_persist_session_install_tier_stamp_slot(const fr_runtime_t *runtime,
                                                 fr_slot_id_t slot_id) {
  if (runtime == NULL) {
    return;
  }
  if (slot_id < FR_PROFILE_MAX_SLOTS) {
    fr_persist_slot_tier[slot_id] = (uint8_t)runtime->install_tier;
  }
}

void fr_persist_session_install_tier_stamp_overlay(
    const fr_runtime_t *runtime, const fr_overlay_update_t *update) {
  if (runtime == NULL || update == NULL) {
    return;
  }
  for (uint16_t i = 0; i < update->slot_init_count; i++) {
    fr_persist_session_install_tier_stamp_slot(runtime,
                                                update->slot_inits[i].slot_id);
  }
}

/* Drop every user-tier overlay binding from the runtime; library-tier
 * stamps stay. A slot with no stamp (default 0) encodes as USER, so the
 * non-library check covers both explicitly-USER and unstamped slots. The
 * encoder reads runtime->slots, so a subsequent fr_persist_save writes
 * only the library binds. Overlay-name entries that point at a no-longer-
 * overlaid slot get compacted out so `words` and fr_slot_id_for_name no
 * longer surface the wiped names. */
void fr_persist_session_wipe_user_tier(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }
  for (fr_slot_id_t slot_id = 0; slot_id < runtime->slots.count; slot_id++) {
    if (runtime->slots.base_tier[slot_id] == FR_INSTALL_TIER_USER) {
      if (runtime->slots.library_base_present[slot_id]) {
        runtime->slots.base[slot_id] = runtime->slots.library_base[slot_id];
        runtime->slots.current[slot_id] = runtime->slots.library_base[slot_id];
        runtime->slots.base_tier[slot_id] = FR_INSTALL_TIER_LIBRARY;
        fr_persist_slot_tier[slot_id] = FR_PERSIST_TIER_LIBRARY;
      } else {
        runtime->slots.base[slot_id] = fr_tagged_nil();
        runtime->slots.current[slot_id] = fr_tagged_nil();
        runtime->slots.base_tier[slot_id] = 0;
        fr_persist_slot_tier[slot_id] = 0;
      }
      runtime->slots.overlay[slot_id] = false;
    }
    if (fr_slot_is_overlay(runtime, slot_id) &&
        fr_persist_slot_tier[slot_id] != FR_PERSIST_TIER_LIBRARY) {
      (void)fr_slot_restore(runtime, slot_id);
    }
  }
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  {
    uint16_t dst = 0;
    for (uint16_t src = 0; src < runtime->slots.overlay_name_count; src++) {
      fr_slot_id_t s = runtime->slots.overlay_name_slots[src];

      if (fr_slot_is_overlay(runtime, s) ||
          (s < FR_PROFILE_MAX_SLOTS &&
           runtime->slots.base_tier[s] == FR_INSTALL_TIER_LIBRARY)) {
        if (dst != src) {
          memcpy(runtime->slots.overlay_names[dst],
                 runtime->slots.overlay_names[src],
                 (size_t)FR_PROFILE_MAX_NAME_BYTES + 1);
          runtime->slots.overlay_name_slots[dst] = s;
        }
        dst = (uint16_t)(dst + 1);
      }
    }
    for (uint16_t i = dst; i < runtime->slots.overlay_name_count; i++) {
      runtime->slots.overlay_names[i][0] = '\0';
      runtime->slots.overlay_name_slots[i] = 0;
    }
    runtime->slots.overlay_name_count = dst;
  }
#endif
  /* Wiping user slots frees the tail of the table; reclaim it so the next
   * definition is assigned the lowest free id. Without this, slots.count keeps
   * counting the wiped slots and the first post-wipe definition is assigned an
   * id the install validator then rejects (bad source). */
  fr_slot_reclaim_free_tail(runtime);
}

/* SPEC D10: install-library on receipt drops L1 binds from the runtime so
 * re-installing replaces removed library words. Mirror of
 * fr_persist_session_wipe_user_tier with the tier check inverted, and the
 * slot stamp cleared on wipe so the slot doesn't keep its L1 tag (a later
 * install-library record will stamp it back to L1; an install-user record
 * lands as USER). The name table compacts the same way. */
void fr_persist_session_wipe_library_tier(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }
  for (fr_slot_id_t slot_id = 0; slot_id < runtime->slots.count; slot_id++) {
    if (runtime->slots.base_tier[slot_id] == FR_INSTALL_TIER_LIBRARY) {
      runtime->slots.base[slot_id] = fr_tagged_nil();
      runtime->slots.current[slot_id] = fr_tagged_nil();
      runtime->slots.base_tier[slot_id] = 0;
      runtime->slots.library_base[slot_id] = fr_tagged_nil();
      runtime->slots.library_base_present[slot_id] = false;
      fr_persist_slot_tier[slot_id] = 0;
      runtime->slots.overlay[slot_id] = false;
    } else if (runtime->slots.library_base_present[slot_id]) {
      runtime->slots.library_base[slot_id] = fr_tagged_nil();
      runtime->slots.library_base_present[slot_id] = false;
    }
    if (fr_slot_is_overlay(runtime, slot_id) &&
        fr_persist_slot_tier[slot_id] == FR_PERSIST_TIER_LIBRARY) {
      (void)fr_slot_restore(runtime, slot_id);
      fr_persist_slot_tier[slot_id] = 0;
    }
  }
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  {
    uint16_t dst = 0;
    for (uint16_t src = 0; src < runtime->slots.overlay_name_count; src++) {
      fr_slot_id_t s = runtime->slots.overlay_name_slots[src];

      if (fr_slot_is_overlay(runtime, s) ||
          (s < FR_PROFILE_MAX_SLOTS && runtime->slots.base_tier[s] != 0)) {
        if (dst != src) {
          memcpy(runtime->slots.overlay_names[dst],
                 runtime->slots.overlay_names[src],
                 (size_t)FR_PROFILE_MAX_NAME_BYTES + 1);
          runtime->slots.overlay_name_slots[dst] = s;
        }
        dst = (uint16_t)(dst + 1);
      }
    }
    for (uint16_t i = dst; i < runtime->slots.overlay_name_count; i++) {
      runtime->slots.overlay_names[i][0] = '\0';
      runtime->slots.overlay_name_slots[i] = 0;
    }
    runtime->slots.overlay_name_count = dst;
  }
#endif
  fr_slot_reclaim_free_tail(runtime);
}

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
  /* Tier byte from the BIND wire record. FR_PERSIST_TIER_LIBRARY or
     FR_PERSIST_TIER_USER; any other value is a corrupt-record reject. */
  uint8_t tier;
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
  bool code_present[FR_PROFILE_CODE_OBJECT_TABLE_SIZE];
  fr_persist_text_record_t text_records[FR_OBJECT_TABLE_CAPACITY];
  fr_persist_record_shape_record_t shape_records[FR_OBJECT_TABLE_CAPACITY];
  fr_record_name_t record_fields[FR_RECORD_SHAPE_FIELD_CAPACITY];
  fr_persist_record_record_t record_records[FR_OBJECT_TABLE_CAPACITY];
  fr_tagged_t record_values[FR_RECORD_VALUE_FIELD_CAPACITY];
  fr_persist_cell_record_t cell_records[FR_OBJECT_TABLE_CAPACITY];
  fr_tagged_t cell_values[FR_CELL_WORD_CAPACITY];
  fr_persist_object_record_kind_t object_kinds[FR_OBJECT_TABLE_CAPACITY];
  bool object_present[FR_OBJECT_TABLE_CAPACITY];
  fr_persist_bind_record_t bind_records[FR_PROFILE_MAX_SLOTS];
  fr_persist_name_record_t name_records[FR_PROFILE_MAX_OVERLAY_NAMES > 0
                                             ? FR_PROFILE_MAX_OVERLAY_NAMES
                                             : 1];
  fr_image_event_binding_t event_records[FR_EVENT_BINDING_COUNT];
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
  uint16_t event_count;
} fr_persist_decoded_payload_t;

static void
fr_persist_decoded_payload_reset(fr_persist_decoded_payload_t *decoded) {
  memset(decoded, 0, sizeof(*decoded));
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

static fr_err_t fr_persist_writer_u32(fr_persist_writer_t *writer,
                                      uint32_t value) {
  if (writer->used + 4 > writer->cap) {
    return FR_ERR_CAPACITY;
  }

  fr_write_u32_le(&writer->bytes[writer->used], value);
  writer->used = (uint16_t)(writer->used + 4);
  return FR_OK;
}

static fr_err_t fr_persist_writer_int(fr_persist_writer_t *writer,
                                      fr_int_t value) {
  return fr_persist_writer_u32(writer, (uint32_t)(int32_t)value);
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

static fr_err_t fr_persist_reader_int(fr_persist_reader_t *reader,
                                      fr_int_t *out) {
  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  {
    uint32_t word = 0;
    FR_TRY(fr_persist_reader_u32(reader, &word));
    *out = (fr_int_t)(int32_t)word;
  }
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

#if FR_FEATURE_TEXT
static fr_err_t fr_persist_encode_text_ref(
    fr_persist_writer_t *writer, const fr_runtime_t *runtime,
    fr_object_id_t runtime_object_id, fr_object_id_t runtime_object_ids[],
    fr_persist_object_record_kind_t object_kinds[], uint16_t *object_count,
    uint16_t *out_local_id);
#endif

static fr_err_t fr_persist_encode_code_ref(
    fr_persist_writer_t *writer, const fr_runtime_t *runtime,
    fr_code_object_id_t runtime_code_id, fr_code_object_id_t runtime_code_ids[],
    bool code_visiting[], uint16_t *code_count,
    fr_object_id_t runtime_object_ids[],
    fr_persist_object_record_kind_t object_kinds[], uint16_t *object_count,
    uint16_t *out_local_id) {
  fr_instruction_stream_t instructions;
  /* Save/finalize scratch holds one whole-image code object, which can be
   * larger than a single device-compiled definition; size off the image cap. */
  uint8_t patched[FR_PROFILE_TOTAL_IMAGE_BYTES];
  fr_instruction_stream_t view;
  fr_instruction_header_t header = {0};
  fr_code_offset_t ip = 0;
  char scratch[64];

  if (runtime_code_id >= FR_PROFILE_CODE_OBJECT_TABLE_SIZE) {
    return FR_ERR_CORRUPT;
  }
  FR_TRY(fr_code_get_instructions(runtime, runtime_code_id, &instructions));

  for (uint16_t i = 0; i < *code_count; i++) {
    bool same_code = false;

    if (runtime_code_ids[i] >= runtime->code.count) {
      continue;
    }
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
  if (code_visiting[runtime_code_id]) {
    return FR_ERR_INVALID;
  }
  code_visiting[runtime_code_id] = true;

  if (instructions.length > (uint16_t)sizeof(patched)) {
    return FR_ERR_CAPACITY;
  }
  memcpy(patched, instructions.bytes, instructions.length);
  FR_TRY(fr_instruction_stream_init(&view, patched, instructions.length));
  FR_TRY(fr_instruction_read_header(&view, &header));
  ip = (fr_code_offset_t)header.header_size;
  while (ip < view.length) {
    fr_code_offset_t next_ip = 0;

#if FR_FEATURE_TEXT
    if ((fr_opcode_t)patched[ip] == FR_OP_PUSH_OBJECT_ID) {
      fr_object_id_t runtime_id = 0;
      uint16_t local_text_id = 0;

      FR_TRY(fr_instruction_read_object_id_operand(&view, ip, &runtime_id));
      FR_TRY(fr_persist_encode_text_ref(writer, runtime, runtime_id,
                                        runtime_object_ids, object_kinds,
                                        object_count, &local_text_id));
      patched[ip + 1] = (uint8_t)(local_text_id & 0xffu);
      patched[ip + 2] = (uint8_t)(local_text_id >> 8);
    }
#endif
    if ((fr_opcode_t)patched[ip] == FR_OP_PUSH_CODE_ID) {
      fr_code_object_id_t inner_runtime_id = 0;
      uint16_t inner_local_id = 0;

      FR_TRY(fr_instruction_read_code_id_operand(&view, ip, &inner_runtime_id));
      /* Encode the inner body first so it gets a smaller final id; the
       * record we are about to write then carries that final id. Mount
       * validates the id order and does not patch these bytes. */
      FR_TRY(fr_persist_encode_code_ref(writer, runtime, inner_runtime_id,
                                        runtime_code_ids, code_visiting,
                                        code_count,
                                        runtime_object_ids, object_kinds,
                                        object_count, &inner_local_id));
      patched[ip + 1] = (uint8_t)(inner_local_id & 0xffu);
      patched[ip + 2] = (uint8_t)(inner_local_id >> 8);
    }
    FR_TRY(fr_instruction_disassemble_at(&view, ip, scratch,
                                         (uint16_t)sizeof(scratch), NULL,
                                         &next_ip));
    if (next_ip <= ip) {
      return FR_ERR_CORRUPT;
    }
    ip = next_ip;
  }
#if !FR_FEATURE_TEXT
  (void)runtime_object_ids;
  (void)object_kinds;
  (void)object_count;
#endif

  FR_TRY(fr_persist_writer_u8(writer, FR_PERSIST_RECORD_CODE));
  FR_TRY(fr_persist_writer_u16(writer, *code_count));
  FR_TRY(fr_persist_writer_u16(writer, instructions.length));
  FR_TRY(fr_persist_writer_bytes(writer, patched, instructions.length));

  runtime_code_ids[*code_count] = runtime_code_id;
  *out_local_id = *code_count;
  *code_count = (uint16_t)(*code_count + 1);
  code_visiting[runtime_code_id] = false;
  return FR_OK;
}

static fr_err_t fr_persist_find_code_ref(
    const fr_runtime_t *runtime, fr_code_object_id_t runtime_code_id,
    const fr_code_object_id_t runtime_code_ids[], uint16_t code_count,
    uint16_t *out_local_id) {
  if (runtime == NULL || runtime_code_ids == NULL || out_local_id == NULL) {
    return FR_ERR_INVALID;
  }

  for (uint16_t i = runtime->code.base_image_count; i < code_count; i++) {
    bool same_code = false;

    if (runtime_code_ids[i] >= runtime->code.count) {
      continue;
    }
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
    if (runtime_object_ids[i] >= runtime->objects.count) {
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
    if (runtime_object_ids[i] >= runtime->objects.count) {
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
        runtime_object_ids[i] < runtime->objects.count &&
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
        runtime_object_ids[i] < runtime->objects.count &&
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
    if (runtime_object_ids[i] >= runtime->objects.count) {
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
  fr_bytes_ref_t bytes_ref = {0};
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
  if (fr_tagged_decode_bytes_ref(tagged, &bytes_ref) == FR_OK) {
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
#if FR_FEATURE_BYTES
  for (fr_slot_id_t slot_id = 0; slot_id < runtime->slots.count; slot_id++) {
    fr_bytes_ref_t bytes_ref = {0};

    if (!fr_slot_is_overlay(runtime, slot_id)) {
      continue;
    }
    if (fr_tagged_decode_bytes_ref(runtime->slots.current[slot_id],
                                   &bytes_ref) == FR_OK) {
      return FR_ERR_VOLATILE;
    }
  }
#endif
  return FR_OK;
}

static uint8_t fr_persist_slot_encode_tier(const fr_runtime_t *runtime,
                                           fr_slot_id_t slot_id) {
  uint8_t tier = 0;

  if (runtime == NULL || slot_id >= FR_PROFILE_MAX_SLOTS) {
    return (uint8_t)FR_PERSIST_TIER_USER;
  }
  if (fr_slot_is_overlay(runtime, slot_id)) {
    tier = fr_persist_slot_tier[slot_id];
  } else if (runtime->slots.base_tier[slot_id] == FR_INSTALL_TIER_LIBRARY) {
    tier = (uint8_t)FR_PERSIST_TIER_LIBRARY;
  } else if (runtime->slots.base_tier[slot_id] == FR_INSTALL_TIER_USER) {
    tier = (uint8_t)FR_PERSIST_TIER_USER;
  }
  if (tier != FR_PERSIST_TIER_LIBRARY && tier != FR_PERSIST_TIER_USER) {
    tier = (uint8_t)FR_PERSIST_TIER_USER;
  }
  return tier;
}

static bool fr_persist_slot_should_encode(const fr_runtime_t *runtime,
                                          fr_slot_id_t slot_id,
                                          uint8_t tier_filter) {
  uint8_t tier = 0;

  if (runtime == NULL || slot_id >= FR_PROFILE_MAX_SLOTS) {
    return false;
  }
  if (!fr_slot_is_overlay(runtime, slot_id) &&
      runtime->slots.base_tier[slot_id] == 0) {
    return false;
  }
  tier = fr_persist_slot_encode_tier(runtime, slot_id);
  return tier_filter == 0 || tier == tier_filter;
}

/* Finalize/remap coverage for payload v5:
 * 1. PUSH_CODE_ID operands in code bytes become final code ids.
 * 2. PUSH_OBJECT_ID operands become final object ids.
 * 3. Slot-init / BIND CODE, NATIVE, and OBJECT values become final ids.
 * 4. Record shape refs and record field value refs become final object ids.
 * 5. Event body refs become final code ids.
 * 6. Tagged words stored in cells become final object ids.
 *
 * tier_filter == 0 walks every overlay slot (the historical full-encode).
 * tier_filter == FR_PERSIST_TIER_USER skips slots stamped L1; that's the save
 * path which trusts NVS for L1 (D5) and only writes L2 from the runtime.
 * library_prefix is an optional pre-encoded byte span pasted immediately
 * after magic+version; save passes the L1 records it extracted from the
 * existing payload so they survive the round-trip byte-for-byte. The
 * prefix carries its own CODE/OBJECT records with local ids 0..K-1; the
 * caller passes K via code_count_initial / object_count_initial so the
 * L2 encode writes new CODE/OBJECT records with local ids continuing from
 * the prefix's last id, which is what the decoder's strict local-id sequence
 * check at fr_persist_decode_payload requires. */
static fr_err_t fr_persist_payload_finalize_encode(
    const fr_runtime_t *runtime, uint8_t tier_filter,
    uint16_t code_count_initial, uint16_t object_count_initial,
    const uint8_t *library_prefix, uint16_t library_prefix_length,
    uint8_t *bytes, uint16_t cap, uint16_t *out_length) {
  fr_persist_writer_t writer = {bytes, 0, cap};
  fr_code_object_id_t runtime_code_ids[FR_PROFILE_CODE_OBJECT_TABLE_SIZE];
  bool code_visiting[FR_PROFILE_CODE_OBJECT_TABLE_SIZE];
  fr_object_id_t runtime_object_ids[FR_OBJECT_TABLE_CAPACITY];
  fr_persist_object_record_kind_t object_kinds[FR_OBJECT_TABLE_CAPACITY];
  uint16_t code_count = code_count_initial;
  uint16_t object_count = object_count_initial;

  if (runtime == NULL || bytes == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }
  if (library_prefix == NULL && library_prefix_length > 0) {
    return FR_ERR_INVALID;
  }

  for (uint16_t i = 0; i < FR_PROFILE_CODE_OBJECT_TABLE_SIZE; i++) {
    runtime_code_ids[i] = FR_PROFILE_CODE_OBJECT_TABLE_SIZE;
  }
  memset(code_visiting, 0, sizeof(code_visiting));
  for (uint16_t i = 0; i < FR_OBJECT_TABLE_CAPACITY; i++) {
    runtime_object_ids[i] = FR_OBJECT_TABLE_CAPACITY;
  }
  memset(object_kinds, 0, sizeof(object_kinds));
  for (fr_code_object_id_t i = 0;
       i < code_count && i < runtime->code.count &&
       i < FR_PROFILE_CODE_OBJECT_TABLE_SIZE;
       i++) {
    runtime_code_ids[i] = i;
  }
  for (fr_object_id_t i = 0;
       i < object_count && i < runtime->objects.count &&
       i < FR_OBJECT_TABLE_CAPACITY;
       i++) {
    runtime_object_ids[i] = i;
    (void)fr_persist_object_kind(runtime, i, &object_kinds[i]);
  }
  FR_TRY(fr_persist_check_no_volatile_handles(runtime));
  FR_TRY(fr_persist_writer_bytes(&writer, fr_persist_payload_magic,
                                 (uint16_t)sizeof(fr_persist_payload_magic)));
  FR_TRY(fr_persist_writer_u8(&writer, FR_PERSIST_PAYLOAD_VERSION));
  if (library_prefix_length > 0) {
    FR_TRY(fr_persist_writer_bytes(&writer, library_prefix,
                                   library_prefix_length));
  }

  for (fr_slot_id_t slot_id = 0; slot_id < runtime->slots.count; slot_id++) {
    fr_code_object_id_t code_id = 0;
    uint16_t ignored = 0;

    if (!fr_persist_slot_should_encode(runtime, slot_id, tier_filter)) {
      continue;
    }
    if (fr_tagged_decode_code_object_id(runtime->slots.current[slot_id],
                                        &code_id) == FR_OK) {
      FR_TRY(fr_persist_encode_code_ref(&writer, runtime, code_id,
                                        runtime_code_ids, code_visiting,
                                        &code_count,
                                        runtime_object_ids, object_kinds,
                                        &object_count, &ignored));
    }
  }

  for (fr_slot_id_t slot_id = 0; slot_id < runtime->slots.count; slot_id++) {
    fr_object_id_t object_id = 0;
    uint16_t ignored = 0;

    if (!fr_persist_slot_should_encode(runtime, slot_id, tier_filter)) {
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
    if (!fr_persist_slot_should_encode(runtime, slot_id, tier_filter)) {
      continue;
    }

    FR_TRY(fr_persist_writer_u8(&writer, FR_PERSIST_RECORD_BIND));
    FR_TRY(fr_persist_writer_u16(&writer, slot_id));
    {
      uint8_t tier = fr_persist_slot_encode_tier(runtime, slot_id);

      FR_TRY(fr_persist_writer_u8(&writer, tier));
    }
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
    if (!fr_persist_slot_should_encode(runtime, slot_id, tier_filter)) {
      continue;
    }
    FR_TRY(fr_persist_name_length(name, &name_length));

    FR_TRY(fr_persist_writer_u8(&writer, FR_PERSIST_RECORD_NAME));
    FR_TRY(fr_persist_writer_u16(&writer, slot_id));
    FR_TRY(fr_persist_writer_u8(&writer, name_length));
    FR_TRY(fr_persist_writer_bytes(&writer, (const uint8_t *)name,
                                   name_length));
  }
#endif

#if FR_FEATURE_EVENTS
  /* Events are always L2 per D5; the boot L1 pass and a hypothetical
   * library-only encode (tier_filter == LIBRARY) skip them. */
  if (tier_filter != (uint8_t)FR_PERSIST_TIER_LIBRARY) {
    for (uint16_t i = 0; i < FR_EVENT_BINDING_COUNT; i++) {
      const fr_event_binding_t *entry = &runtime->events.entries[i];
      uint16_t local_code_id = 0;

      if (entry->kind == FR_EVENT_KIND_NONE) {
        continue;
      }
      FR_TRY(fr_persist_encode_code_ref(&writer, runtime, entry->body,
                                        runtime_code_ids, code_visiting,
                                        &code_count,
                                        runtime_object_ids, object_kinds,
                                        &object_count, &local_code_id));
      FR_TRY(fr_persist_writer_u8(&writer, FR_PERSIST_RECORD_EVENT));
      FR_TRY(fr_persist_writer_u8(&writer, (uint8_t)entry->kind));
      FR_TRY(fr_persist_writer_u16(&writer, entry->source));
      FR_TRY(fr_persist_writer_u16(&writer, entry->debounce_ms));
      FR_TRY(fr_persist_writer_u16(&writer, local_code_id));
    }
  }
#endif

  FR_TRY(fr_persist_writer_u8(&writer, FR_PERSIST_RECORD_END));
  *out_length = writer.used;
  return FR_OK;
}

fr_err_t fr_persist_payload_encode(const fr_runtime_t *runtime, uint8_t *bytes,
                                   uint16_t cap, uint16_t *out_length) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  return fr_persist_payload_finalize_encode(
      runtime, 0, runtime->code.base_image_count, runtime->objects.base_count,
      NULL, 0, bytes, cap, out_length);
}

/* Skip a tagged value body inside a BIND record. Mirrors the decoder's
 * fr_persist_decode_value byte layout (kind byte plus a kind-dependent body)
 * without rebuilding the value — the extract walker only needs the byte
 * span, not the runtime tagged_t. */
static fr_err_t
fr_persist_payload_skip_value(fr_persist_reader_t *reader) {
  uint8_t value_kind = 0;
  fr_int_t scratch_int = 0;
  uint16_t scratch_u16 = 0;

  FR_TRY(fr_persist_reader_u8(reader, &value_kind));
  switch (value_kind) {
  case FR_PERSIST_VALUE_NIL:
  case FR_PERSIST_VALUE_FALSE:
  case FR_PERSIST_VALUE_TRUE:
    return FR_OK;
  case FR_PERSIST_VALUE_INT:
    return fr_persist_reader_int(reader, &scratch_int);
  case FR_PERSIST_VALUE_NATIVE:
  case FR_PERSIST_VALUE_CODE:
  case FR_PERSIST_VALUE_OBJECT:
    return fr_persist_reader_u16(reader, &scratch_u16);
  default:
    return FR_ERR_CORRUPT;
  }
}

#if FR_FEATURE_RECORDS
/* Skip a record_name (u16 length + bytes) used inside RECORD_SHAPE bodies. */
static fr_err_t
fr_persist_payload_skip_record_name(fr_persist_reader_t *reader) {
  uint16_t length = 0;
  const uint8_t *bytes = NULL;

  FR_TRY(fr_persist_reader_u16(reader, &length));
  if (length == 0 || length > FR_PROFILE_MAX_NAME_BYTES) {
    return FR_ERR_CORRUPT;
  }
  return fr_persist_reader_bytes(reader, length, &bytes);
}
#endif

/* Advance the reader past one whole record. Returns the tag and the lookup
 * key the extract walker needs (local_id for resource records; slot_id for
 * BIND / NAME; zero for END / EVENT). count_prefix_records and the source-
 * order extract walker both use this so they parse the wire format the same
 * way and a record that one understands the other does too. */
static fr_err_t fr_persist_payload_advance_record(
    fr_persist_reader_t *reader, uint8_t *out_tag, uint16_t *out_key) {
  uint8_t tag = 0;
  uint16_t local_id = 0;
  uint16_t length = 0;
  uint16_t slot_id = 0;
  uint8_t tier = 0;
  uint8_t name_length = 0;
  const uint8_t *bytes = NULL;

  if (reader == NULL || out_tag == NULL || out_key == NULL) {
    return FR_ERR_INVALID;
  }
  *out_key = 0;

  FR_TRY(fr_persist_reader_u8(reader, &tag));
  *out_tag = tag;

  if (tag == FR_PERSIST_RECORD_END) {
    return FR_OK;
  }
  if (tag == FR_PERSIST_RECORD_CODE || tag == FR_PERSIST_RECORD_TEXT) {
    FR_TRY(fr_persist_reader_u16(reader, &local_id));
    FR_TRY(fr_persist_reader_u16(reader, &length));
    FR_TRY(fr_persist_reader_bytes(reader, length, &bytes));
    *out_key = local_id;
    return FR_OK;
  }
  if (tag == FR_PERSIST_RECORD_BIND) {
    FR_TRY(fr_persist_reader_u16(reader, &slot_id));
    FR_TRY(fr_persist_reader_u8(reader, &tier));
    if (tier != FR_PERSIST_TIER_LIBRARY && tier != FR_PERSIST_TIER_USER) {
      return FR_ERR_CORRUPT;
    }
    FR_TRY(fr_persist_payload_skip_value(reader));
    *out_key = slot_id;
    return FR_OK;
  }
  if (tag == FR_PERSIST_RECORD_NAME) {
    FR_TRY(fr_persist_reader_u16(reader, &slot_id));
    FR_TRY(fr_persist_reader_u8(reader, &name_length));
    if (name_length == 0 || name_length > FR_PROFILE_MAX_NAME_BYTES) {
      return FR_ERR_CORRUPT;
    }
    FR_TRY(fr_persist_reader_bytes(reader, name_length, &bytes));
    *out_key = slot_id;
    return FR_OK;
  }
#if FR_FEATURE_RECORDS
  if (tag == FR_PERSIST_RECORD_RECORD_SHAPE) {
    uint16_t field_count = 0;

    FR_TRY(fr_persist_reader_u16(reader, &local_id));
    FR_TRY(fr_persist_payload_skip_record_name(reader));
    FR_TRY(fr_persist_reader_u16(reader, &field_count));
    for (uint16_t i = 0; i < field_count; i++) {
      FR_TRY(fr_persist_payload_skip_record_name(reader));
    }
    *out_key = local_id;
    return FR_OK;
  }
  if (tag == FR_PERSIST_RECORD_RECORD) {
    uint16_t shape_local_id = 0;
    uint16_t field_count = 0;

    FR_TRY(fr_persist_reader_u16(reader, &local_id));
    FR_TRY(fr_persist_reader_u16(reader, &shape_local_id));
    FR_TRY(fr_persist_reader_u16(reader, &field_count));
    for (uint16_t i = 0; i < field_count; i++) {
      FR_TRY(fr_persist_payload_skip_value(reader));
    }
    *out_key = local_id;
    return FR_OK;
  }
#endif
#if FR_FEATURE_CELLS
  if (tag == FR_PERSIST_RECORD_CELLS) {
    FR_TRY(fr_persist_reader_u16(reader, &local_id));
    FR_TRY(fr_persist_reader_u16(reader, &length));
    for (uint16_t i = 0; i < length; i++) {
      FR_TRY(fr_persist_payload_skip_value(reader));
    }
    *out_key = local_id;
    return FR_OK;
  }
#endif
#if FR_FEATURE_EVENTS
  if (tag == FR_PERSIST_RECORD_EVENT) {
    uint8_t kind = 0;
    uint16_t scratch = 0;

    FR_TRY(fr_persist_reader_u8(reader, &kind));
    FR_TRY(fr_persist_reader_u16(reader, &scratch));
    FR_TRY(fr_persist_reader_u16(reader, &scratch));
    FR_TRY(fr_persist_reader_u16(reader, &scratch));
    return FR_OK;
  }
#endif
  return FR_ERR_CORRUPT;
}

/* Walk a prefix span (a slice of an encoded payload, starting at the first
 * record byte after magic+version) and return the next final code/object ids
 * after the preserved L1 records. */
static fr_err_t fr_persist_payload_next_prefix_ids(
    const uint8_t *prefix, uint16_t prefix_length, uint16_t *out_code_count,
    uint16_t *out_object_count) {
  fr_persist_reader_t reader = {prefix, prefix_length, 0};

  if (out_code_count == NULL || out_object_count == NULL) {
    return FR_ERR_INVALID;
  }
  *out_code_count = 0;
  *out_object_count = 0;
  if (prefix == NULL || prefix_length == 0) {
    return FR_OK;
  }

  while (reader.offset < reader.used) {
    uint8_t tag = 0;
    uint16_t key = 0;

    FR_TRY(fr_persist_payload_advance_record(&reader, &tag, &key));
    if (tag == FR_PERSIST_RECORD_END) {
      return FR_ERR_CORRUPT;
    }
    if (tag == FR_PERSIST_RECORD_CODE) {
      if (key >= *out_code_count) {
        *out_code_count = (uint16_t)(key + 1u);
      }
    } else if (tag == FR_PERSIST_RECORD_TEXT
#if FR_FEATURE_RECORDS
               || tag == FR_PERSIST_RECORD_RECORD_SHAPE ||
               tag == FR_PERSIST_RECORD_RECORD
#endif
#if FR_FEATURE_CELLS
               || tag == FR_PERSIST_RECORD_CELLS
#endif
    ) {
      if (key >= *out_object_count) {
        *out_object_count = (uint16_t)(key + 1u);
      }
    } else if (tag == FR_PERSIST_RECORD_BIND ||
               tag == FR_PERSIST_RECORD_NAME) {
      /* no count change */
    } else {
      return FR_ERR_CORRUPT;
    }
  }
  return FR_OK;
}

/* D5: save encodes only L2 from runtime and prefixes the existing payload's
 * L1 records (CODE / TEXT / BIND / NAME — the closure of records L1 BINDs
 * reference) verbatim. Callers (fr_persist_save) extract the prefix via
 * fr_persist_payload_extract_library_records. */
fr_err_t fr_persist_payload_save_encode(
    const fr_runtime_t *runtime, const uint8_t *library_prefix,
    uint16_t library_prefix_length, uint8_t *bytes, uint16_t cap,
    uint16_t *out_length) {
  uint16_t prefix_code_count = 0;
  uint16_t prefix_object_count = 0;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_persist_payload_next_prefix_ids(
      library_prefix, library_prefix_length, &prefix_code_count,
      &prefix_object_count));
  if (prefix_code_count < runtime->code.base_image_count) {
    prefix_code_count = runtime->code.base_image_count;
  }
  if (prefix_object_count < runtime->objects.base_count) {
    prefix_object_count = runtime->objects.base_count;
  }
  return fr_persist_payload_finalize_encode(
      runtime, (uint8_t)FR_PERSIST_TIER_USER, prefix_code_count,
      prefix_object_count, library_prefix, library_prefix_length, bytes, cap,
      out_length);
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
    const bool object_present[], uint16_t object_count,
    fr_tagged_t *out_tagged) {
  uint8_t value_kind = 0;
#if FR_FEATURE_TEXT
  uint16_t value_word = 0;
#endif
  fr_int_t value_int = 0;

  if (out_tagged == NULL || object_kinds == NULL || object_present == NULL) {
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
    if (value_word >= object_count || !object_present[value_word]) {
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

#if FR_FEATURE_TEXT
static fr_err_t fr_persist_find_text_record(
    const fr_persist_text_record_t text_records[], uint16_t text_count,
    uint16_t local_id, const fr_persist_text_record_t **out_text) {
  if (text_records == NULL || out_text == NULL) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < text_count; i++) {
    if (text_records[i].local_id == local_id) {
      *out_text = &text_records[i];
      return FR_OK;
    }
  }
  return FR_ERR_CORRUPT;
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

static fr_err_t fr_persist_find_record_record(
    const fr_persist_record_record_t record_records[], uint16_t record_count,
    uint16_t local_id, const fr_persist_record_record_t **out_record) {
  if (record_records == NULL || out_record == NULL) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < record_count; i++) {
    if (record_records[i].local_id == local_id) {
      *out_record = &record_records[i];
      return FR_OK;
    }
  }
  return FR_ERR_CORRUPT;
}

static fr_err_t fr_persist_decode_record_value(
    fr_persist_reader_t *reader,
    const fr_persist_object_record_kind_t object_kinds[],
    const bool object_present[], uint16_t object_count,
    fr_tagged_t *out_tagged) {
  uint8_t value_kind = 0;
  uint16_t value_word = 0;
  fr_int_t value_int = 0;

  if (reader == NULL || object_kinds == NULL || object_present == NULL ||
      out_tagged == NULL) {
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
    if (value_word >= object_count || !object_present[value_word] ||
        object_kinds[value_word] != FR_PERSIST_OBJECT_TEXT) {
      return FR_ERR_CORRUPT;
    }
    return fr_tagged_encode_object_id(value_word, out_tagged);
  default:
    return FR_ERR_CORRUPT;
  }
}
#endif

#if FR_FEATURE_CELLS
static fr_err_t fr_persist_find_cell_record(
    const fr_persist_cell_record_t cell_records[], uint16_t cell_count,
    uint16_t local_id, const fr_persist_cell_record_t **out_cell) {
  if (cell_records == NULL || out_cell == NULL) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < cell_count; i++) {
    if (cell_records[i].local_id == local_id) {
      *out_cell = &cell_records[i];
      return FR_OK;
    }
  }
  return FR_ERR_CORRUPT;
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
  bool *code_present = NULL;
  bool *object_present = NULL;
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
  code_present = decoded->code_present;
  object_present = decoded->object_present;
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
  (void)object_present;
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
      fr_instruction_header_t header = {0};
      uint16_t final_id = 0;

      FR_TRY(fr_persist_reader_u16(&reader, &final_id));
      if (final_id >= FR_PROFILE_CODE_OBJECT_TABLE_SIZE ||
          code_present[final_id]) {
        return FR_ERR_CORRUPT;
      }
      code = &code_records[final_id];
      code->local_id = final_id;
      FR_TRY(fr_persist_reader_u16(&reader, &code->length));
      FR_TRY(fr_persist_reader_bytes(&reader, code->length, &code->bytes));
      FR_TRY(fr_instruction_stream_init(&instructions, code->bytes,
                                        code->length));
      FR_TRY(fr_instruction_read_header(&instructions, &header));
      code_present[final_id] = true;
      if (final_id >= *out_code_count) {
        *out_code_count = (uint16_t)(final_id + 1u);
      }
    } else if (record == FR_PERSIST_RECORD_TEXT) {
#if !FR_FEATURE_TEXT
      return FR_ERR_UNSUPPORTED;
#else
      fr_persist_text_record_t *text = NULL;
      uint16_t final_id = 0;

      if (*out_text_count >= FR_PROFILE_OBJECT_TABLE_SIZE) {
        return FR_ERR_CAPACITY;
      }
      text = &text_records[*out_text_count];
      FR_TRY(fr_persist_reader_u16(&reader, &final_id));
      if (final_id >= FR_OBJECT_TABLE_CAPACITY ||
          object_present[final_id]) {
        return FR_ERR_CORRUPT;
      }
      text->local_id = final_id;
      FR_TRY(fr_persist_reader_u16(&reader, &text->length));
      if (text->length > FR_PROFILE_MAX_TEXT_LENGTH) {
        return FR_ERR_CORRUPT;
      }
      FR_TRY(fr_persist_reader_bytes(&reader, text->length, &text->bytes));
      object_kinds[final_id] = FR_PERSIST_OBJECT_TEXT;
      object_present[final_id] = true;
      *out_text_count = (uint16_t)(*out_text_count + 1);
      if (final_id >= *out_object_count) {
        *out_object_count = (uint16_t)(final_id + 1u);
      }
#endif
    } else if (record == FR_PERSIST_RECORD_RECORD_SHAPE) {
#if !FR_FEATURE_RECORDS
      return FR_ERR_UNSUPPORTED;
#else
      fr_persist_record_shape_record_t *shape = NULL;
      uint16_t final_id = 0;

      if (*out_shape_count >= FR_PROFILE_OBJECT_TABLE_SIZE) {
        return FR_ERR_CAPACITY;
      }
      shape = &shape_records[*out_shape_count];
      FR_TRY(fr_persist_reader_u16(&reader, &final_id));
      if (final_id >= FR_OBJECT_TABLE_CAPACITY ||
          object_present[final_id]) {
        return FR_ERR_CORRUPT;
      }
      shape->local_id = final_id;
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
      object_kinds[final_id] = FR_PERSIST_OBJECT_RECORD_SHAPE;
      object_present[final_id] = true;
      *out_shape_count = (uint16_t)(*out_shape_count + 1);
      if (final_id >= *out_object_count) {
        *out_object_count = (uint16_t)(final_id + 1u);
      }
#endif
    } else if (record == FR_PERSIST_RECORD_RECORD) {
#if !FR_FEATURE_RECORDS
      return FR_ERR_UNSUPPORTED;
#else
      fr_persist_record_record_t *record_object = NULL;
      const fr_persist_record_shape_record_t *shape = NULL;
      uint16_t final_id = 0;

      if (*out_record_count >= FR_PROFILE_OBJECT_TABLE_SIZE) {
        return FR_ERR_CAPACITY;
      }
      record_object = &record_records[*out_record_count];
      FR_TRY(fr_persist_reader_u16(&reader, &final_id));
      if (final_id >= FR_OBJECT_TABLE_CAPACITY ||
          object_present[final_id]) {
        return FR_ERR_CORRUPT;
      }
      record_object->local_id = final_id;
      FR_TRY(fr_persist_reader_u16(&reader,
                                   &record_object->shape_local_id));
      if (record_object->shape_local_id >= *out_object_count ||
          !object_present[record_object->shape_local_id] ||
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
                                              object_present,
                                              *out_object_count, &tagged));
        record_values[*out_record_value_count] = tagged;
        *out_record_value_count =
            (uint16_t)(*out_record_value_count + 1);
      }
      object_kinds[final_id] = FR_PERSIST_OBJECT_RECORD;
      object_present[final_id] = true;
      *out_record_count = (uint16_t)(*out_record_count + 1);
      if (final_id >= *out_object_count) {
        *out_object_count = (uint16_t)(final_id + 1u);
      }
#endif
    } else if (record == FR_PERSIST_RECORD_CELLS) {
#if !FR_FEATURE_CELLS
      return FR_ERR_UNSUPPORTED;
#else
      fr_persist_cell_record_t *cell = NULL;
      uint16_t final_id = 0;

      if (*out_cell_count >= FR_PROFILE_OBJECT_TABLE_SIZE) {
        return FR_ERR_CAPACITY;
      }
      cell = &cell_records[*out_cell_count];
      FR_TRY(fr_persist_reader_u16(&reader, &final_id));
      if (final_id >= FR_OBJECT_TABLE_CAPACITY ||
          object_present[final_id]) {
        return FR_ERR_CORRUPT;
      }
      cell->local_id = final_id;
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
                                            object_present,
                                            *out_object_count, &tagged));
        cell_values[*out_cell_value_count] = tagged;
        *out_cell_value_count = (uint16_t)(*out_cell_value_count + 1);
      }
      object_kinds[final_id] = FR_PERSIST_OBJECT_CELLS;
      object_present[final_id] = true;
      *out_cell_count = (uint16_t)(*out_cell_count + 1);
      if (final_id >= *out_object_count) {
        *out_object_count = (uint16_t)(final_id + 1u);
      }
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
      FR_TRY(fr_persist_reader_u8(&reader, &bind->tier));
      if (bind->tier != FR_PERSIST_TIER_LIBRARY &&
          bind->tier != FR_PERSIST_TIER_USER) {
        return FR_ERR_CORRUPT;
      }
      FR_TRY(fr_persist_decode_value(&reader, bind));
      *out_bind_count = (uint16_t)(*out_bind_count + 1);
    } else if (record == FR_PERSIST_RECORD_EVENT) {
#if !FR_FEATURE_EVENTS
      return FR_ERR_UNSUPPORTED;
#else
      fr_image_event_binding_t *binding = NULL;
      uint8_t kind = 0;
      uint16_t source = 0;
      uint16_t debounce_ms = 0;
      uint16_t body = 0;

      if (decoded->event_count >= FR_EVENT_BINDING_COUNT) {
        return FR_ERR_CAPACITY;
      }
      FR_TRY(fr_persist_reader_u8(&reader, &kind));
      if (kind == FR_EVENT_KIND_NONE || kind > FR_EVENT_KIND_WIFI_RECONNECTED) {
        return FR_ERR_CORRUPT;
      }
      FR_TRY(fr_persist_reader_u16(&reader, &source));
      FR_TRY(fr_persist_reader_u16(&reader, &debounce_ms));
      FR_TRY(fr_persist_reader_u16(&reader, &body));
      /* Save writes the body's CODE record before this EVENT record via
       * fr_persist_encode_code_ref, so the local id has been seen by decode. */
      if (body >= *out_code_count || !code_present[body]) {
        return FR_ERR_CORRUPT;
      }
      binding = &decoded->event_records[decoded->event_count];
      binding->kind = (fr_event_kind_t)kind;
      binding->source = source;
      binding->debounce_ms = debounce_ms;
      binding->body = body;
      decoded->event_count = (uint16_t)(decoded->event_count + 1);
#endif
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
                                      uint16_t code_count,
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
    return fr_tagged_encode_code_object_id(bind->value_word, out_tagged);
  case FR_PERSIST_VALUE_NATIVE:
    if (bind->value_word >= runtime->natives.base_count) {
      return FR_ERR_CORRUPT;
    }
    return fr_tagged_encode_native_id(bind->value_word, out_tagged);
  case FR_PERSIST_VALUE_OBJECT:
    if (bind->value_word >= object_count) {
      return FR_ERR_CORRUPT;
    }
    return fr_tagged_encode_object_id(bind->value_word, out_tagged);
  default:
    return FR_ERR_CORRUPT;
  }
}

static fr_err_t fr_persist_check_restore_capacity(
    const fr_runtime_t *runtime,
    const fr_persist_decoded_payload_t *decoded) {
  uint32_t used_cell_words = 0;
  uint32_t used_text_bytes = 0;
  uint32_t used_record_names = 0;
  uint32_t used_record_shape_fields = 0;
  uint32_t used_record_values = 0;
  uint32_t used_record_name_bytes = 0;
  const fr_persist_code_record_t *code_records = NULL;
  const fr_persist_text_record_t *text_records = NULL;
  const fr_persist_record_shape_record_t *shape_records = NULL;
  const fr_record_name_t *record_fields = NULL;
  const fr_persist_record_record_t *record_records = NULL;
  const fr_persist_cell_record_t *cell_records = NULL;
  const bool *code_present = NULL;
  uint16_t code_count = 0;
  uint16_t text_count = 0;
  uint16_t shape_count = 0;
  uint16_t record_field_count = 0;
  uint16_t record_count = 0;
  uint16_t cell_count = 0;
  uint16_t object_count = 0;

  if (runtime == NULL || decoded == NULL) {
    return FR_ERR_INVALID;
  }
  code_records = decoded->code_records;
  code_present = decoded->code_present;
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
  object_count = decoded->object_count;
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

  if (code_count > FR_PROFILE_CODE_OBJECT_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }

  for (uint16_t i = 0; i < code_count; i++) {
    if (!code_present[i]) {
      continue;
    }
    if (code_records[i].length > FR_PROFILE_TOTAL_IMAGE_BYTES) {
      return FR_ERR_CAPACITY;
    }
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
  if (object_count > FR_PROFILE_OBJECT_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }
  return FR_OK;
}

static fr_err_t fr_persist_check_bind_record(
    const fr_runtime_t *runtime, const fr_persist_bind_record_t *bind,
    const bool code_present[], uint16_t code_count,
    const bool object_present[], uint16_t object_count) {
  fr_tagged_t ignored = 0;

#if !FR_FEATURE_OBJECTS
  (void)object_count;
#endif
  if (runtime == NULL || bind == NULL || code_present == NULL ||
      object_present == NULL) {
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
    if (bind->value_word < runtime->code.base_image_count) {
      return bind->value_word < runtime->code.count ? FR_OK : FR_ERR_CORRUPT;
    }
    return bind->value_word < code_count && code_present[bind->value_word]
               ? FR_OK
               : FR_ERR_CORRUPT;
  case FR_PERSIST_VALUE_NATIVE:
    return bind->value_word < runtime->natives.base_count ? FR_OK
                                                          : FR_ERR_CORRUPT;
  case FR_PERSIST_VALUE_OBJECT:
#if FR_FEATURE_OBJECTS
    if (bind->value_word < runtime->objects.base_count) {
      return bind->value_word < runtime->objects.count ? FR_OK
                                                       : FR_ERR_CORRUPT;
    }
    return bind->value_word < object_count && object_present[bind->value_word]
               ? FR_OK
               : FR_ERR_CORRUPT;
#else
    return FR_ERR_UNSUPPORTED;
#endif
  default:
    return FR_ERR_CORRUPT;
  }
}

static fr_err_t fr_persist_check_final_image_refs(
    const fr_runtime_t *runtime,
    const fr_persist_decoded_payload_t *decoded) {
  if (runtime == NULL || decoded == NULL) {
    return FR_ERR_INVALID;
  }
  if (decoded->code_count > 0 &&
      decoded->code_count < runtime->code.base_image_count) {
    return FR_ERR_CORRUPT;
  }
  for (uint16_t i = 0;
       i < runtime->code.base_image_count && i < decoded->code_count; i++) {
    if (decoded->code_present[i]) {
      return FR_ERR_CORRUPT;
    }
  }
  for (uint16_t i = runtime->code.base_image_count; i < decoded->code_count;
       i++) {
    if (!decoded->code_present[i]) {
      return FR_ERR_CORRUPT;
    }
  }
  if (decoded->object_count > 0 &&
      decoded->object_count < runtime->objects.base_count) {
    return FR_ERR_CORRUPT;
  }
  for (uint16_t i = 0;
       i < runtime->objects.base_count && i < decoded->object_count; i++) {
    if (decoded->object_present[i]) {
      return FR_ERR_CORRUPT;
    }
  }
  for (uint16_t i = runtime->objects.base_count; i < decoded->object_count;
       i++) {
    if (!decoded->object_present[i]) {
      return FR_ERR_CORRUPT;
    }
  }

  for (uint16_t i = runtime->code.base_image_count; i < decoded->code_count;
       i++) {
    const fr_persist_code_record_t *code = &decoded->code_records[i];
    fr_instruction_stream_t view;
    fr_instruction_header_t header = {0};
    fr_code_offset_t ip = 0;
    char scratch[64];

    if (!decoded->code_present[i]) {
      continue;
    }
    FR_TRY(fr_instruction_stream_init(&view, code->bytes, code->length));
    FR_TRY(fr_instruction_read_header(&view, &header));
    ip = (fr_code_offset_t)header.header_size;
    while (ip < view.length) {
      fr_code_offset_t next_ip = 0;
      fr_opcode_t op = (fr_opcode_t)code->bytes[ip];

      if (op == FR_OP_PUSH_CODE_ID) {
        fr_code_object_id_t ref = 0;

        FR_TRY(fr_instruction_read_code_id_operand(&view, ip, &ref));
        if (ref < runtime->code.base_image_count) {
          if (ref >= runtime->code.count) {
            return FR_ERR_CORRUPT;
          }
        } else if (ref >= i || !decoded->code_present[ref]) {
          return FR_ERR_CORRUPT;
        }
      }
#if FR_FEATURE_OBJECTS
      if (op == FR_OP_PUSH_OBJECT_ID) {
        fr_object_id_t ref = 0;

        FR_TRY(fr_instruction_read_object_id_operand(&view, ip, &ref));
        if (ref < runtime->objects.base_count) {
          const uint8_t *ignored_bytes = NULL;
          uint16_t ignored_length = 0;

          FR_TRY(fr_text_view(runtime, ref, &ignored_bytes, &ignored_length));
        } else if (ref >= decoded->object_count ||
                   !decoded->object_present[ref] ||
                   decoded->object_kinds[ref] != FR_PERSIST_OBJECT_TEXT) {
          return FR_ERR_CORRUPT;
        }
        /* Object records are all validated and installed before any code is
         * mounted, so PUSH_OBJECT_ID has no code-style deps-before ordering. */
      }
#endif
      FR_TRY(fr_instruction_disassemble_at(&view, ip, scratch,
                                           (uint16_t)sizeof(scratch), NULL,
                                           &next_ip));
      if (next_ip <= ip) {
        return FR_ERR_CORRUPT;
      }
      ip = next_ip;
    }
  }
  return FR_OK;
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

static uint16_t fr_persist_append_str(char *buf, uint16_t cap, uint16_t used,
                                      const char *s) {
  while (s != NULL && *s != '\0' && used + 1 < cap) {
    buf[used++] = *s++;
  }
  return used;
}

static uint16_t fr_persist_append_u16(char *buf, uint16_t cap, uint16_t used,
                                      uint16_t value) {
  char digits[8];
  uint8_t count = 0;

  if (value == 0) {
    if (used + 1 < cap) {
      buf[used++] = '0';
    }
    return used;
  }
  while (value > 0 && count < (uint8_t)sizeof(digits)) {
    digits[count++] = (char)('0' + value % 10);
    value = (uint16_t)(value / 10);
  }
  while (count > 0 && used + 1 < cap) {
    buf[used++] = digits[--count];
  }
  return used;
}

/* SPEC D6 paragraph 4: an L1 bind failure during boot writes one line via
 * the same channel fr_repl_write_startup_error uses, then the loop continues.
 * The format is fixed by the SPEC: warning: L1 skip slot=<id> err=<name>\n. */
static void fr_persist_write_l1_skip_warning(fr_slot_id_t slot_id,
                                             fr_err_t err) {
  char buf[80];
  uint16_t used = 0;
  const char *name = fr_err_name(err);

  used = fr_persist_append_str(buf, (uint16_t)sizeof(buf), used,
                               "warning: L1 skip slot=");
  used = fr_persist_append_u16(buf, (uint16_t)sizeof(buf), used,
                               (uint16_t)slot_id);
  used = fr_persist_append_str(buf, (uint16_t)sizeof(buf), used, " err=");
  used = fr_persist_append_str(buf, (uint16_t)sizeof(buf), used,
                               name != NULL ? name : "unknown");
  used = fr_persist_append_str(buf, (uint16_t)sizeof(buf), used, "\n");
  if (used < (uint16_t)sizeof(buf)) {
    buf[used] = '\0';
  } else {
    buf[sizeof(buf) - 1] = '\0';
  }
  (void)fr_platform_write_text(buf);
}

/* Walks decoded bind records once per tier in tier-id order (L1 then L2)
 * when tier_filter == 0; otherwise restricts the walk to the named tier.
 * skip_on_failure=true means a single bind's bind_value/slot_write error
 * is logged via the L1 warning channel and the walk continues — that is the
 * D6 boot L1 contract. With skip_on_failure=false the first failure aborts
 * the walk, which is what the FULL and L2 restore paths want. */
static fr_err_t fr_persist_apply_binds_tiered(
    fr_runtime_t *runtime, const fr_persist_decoded_payload_t *decoded,
    uint8_t tier_filter, bool skip_on_failure, bool mount_as_base) {
  const fr_persist_bind_record_t *bind_records = decoded->bind_records;
  uint16_t bind_count = decoded->bind_count;
  uint16_t code_count = decoded->code_count;
  uint16_t object_count = decoded->object_count;
  uint8_t tier_lo =
      tier_filter == 0 ? (uint8_t)FR_PERSIST_TIER_LIBRARY : tier_filter;
  uint8_t tier_hi =
      tier_filter == 0 ? (uint8_t)FR_PERSIST_TIER_USER : tier_filter;

  for (uint8_t pass_tier = tier_lo; pass_tier <= tier_hi; pass_tier++) {
    for (uint16_t i = 0; i < bind_count; i++) {
      fr_tagged_t tagged = 0;
      fr_err_t err = FR_OK;

      if (bind_records[i].tier != pass_tier) {
        continue;
      }
      err = fr_persist_bind_value(runtime, &bind_records[i], code_count,
                                  object_count, &tagged);
      if (err == FR_OK) {
        if (mount_as_base) {
          fr_install_tier_t tier =
              bind_records[i].tier == FR_PERSIST_TIER_LIBRARY
                  ? FR_INSTALL_TIER_LIBRARY
                  : FR_INSTALL_TIER_USER;

          err = fr_slot_set_mounted_base(runtime, bind_records[i].slot_id,
                                         tagged, tier);
        } else {
          err = fr_slot_write(runtime, bind_records[i].slot_id, tagged);
        }
      }
      if (err != FR_OK) {
        if (skip_on_failure) {
          fr_persist_write_l1_skip_warning(bind_records[i].slot_id, err);
          continue;
        }
        return err;
      }
      if (bind_records[i].slot_id < FR_PROFILE_MAX_SLOTS) {
        fr_persist_slot_tier[bind_records[i].slot_id] = bind_records[i].tier;
      }
    }
  }
  return FR_OK;
}

/* Tier-agnostic resource install: texts, codes, shapes, records, cells. Codes
 * are not deduped — they always append — and final ids must match installed
 * ids because mount does not patch immutable image bytes.
 * Objects dedupe by content via fr_text_install_since's find path, but the
 * encoder also dedupes by content so within a single payload no two records
 * carry the same bytes; the dedup never collapses two distinct local ids.
 *
 * Optional code_filter / object_filter masks limit the install to closure
 * subsets — used by the L2-only public restore (D5 acceptance #6) so L1
 * resources already installed by the boot pipeline do not get reinstalled.
 * NULL means "install every record" (FULL restore + boot L1 pass). */
static fr_err_t
fr_persist_install_resources(fr_runtime_t *runtime,
                             const fr_persist_decoded_payload_t *decoded,
                             const bool *code_filter,
                             const bool *object_filter) {
  const fr_persist_code_record_t *code_records = decoded->code_records;
  const fr_persist_text_record_t *text_records = decoded->text_records;
  const fr_persist_record_shape_record_t *shape_records =
      decoded->shape_records;
  const fr_record_name_t *record_fields = decoded->record_fields;
  const fr_persist_record_record_t *record_records = decoded->record_records;
  const fr_tagged_t *record_values = decoded->record_values;
  const fr_persist_cell_record_t *cell_records = decoded->cell_records;
  const fr_tagged_t *cell_values = decoded->cell_values;
  const fr_persist_object_record_kind_t *object_kinds = decoded->object_kinds;
  uint16_t code_count = decoded->code_count;
  uint16_t text_count = decoded->text_count;
  uint16_t shape_count = decoded->shape_count;
  uint16_t record_count = decoded->record_count;
  uint16_t cell_count = decoded->cell_count;
  uint16_t object_count = decoded->object_count;
  fr_tagged_t cell_install_values[FR_CELL_WORD_CAPACITY];

#if !FR_FEATURE_TEXT
  (void)text_records;
  (void)text_count;
#endif
#if !FR_FEATURE_RECORDS
  (void)shape_records;
  (void)record_fields;
  (void)record_records;
  (void)record_values;
  (void)shape_count;
  (void)record_count;
#endif
#if !FR_FEATURE_CELLS
  (void)cell_records;
  (void)cell_values;
  (void)cell_count;
  (void)cell_install_values;
#endif

  for (uint16_t object_id = runtime->objects.base_count;
       object_id < object_count; object_id++) {
    fr_object_id_t installed_id = 0;

    if (!decoded->object_present[object_id]) {
      continue;
    }
    if (object_filter != NULL && !object_filter[object_id]) {
      continue;
    }

    if (object_kinds[object_id] == FR_PERSIST_OBJECT_TEXT) {
#if !FR_FEATURE_TEXT
      return FR_ERR_UNSUPPORTED;
#else
      const fr_persist_text_record_t *text = NULL;

      FR_TRY(fr_persist_find_text_record(text_records, text_count, object_id,
                                         &text));
      FR_TRY(fr_text_install_since(runtime, text->bytes, text->length,
                                   runtime->objects.base_count,
                                   &installed_id));
#endif
    } else if (object_kinds[object_id] == FR_PERSIST_OBJECT_RECORD_SHAPE) {
#if !FR_FEATURE_RECORDS
      return FR_ERR_UNSUPPORTED;
#else
      const fr_persist_record_shape_record_t *shape = NULL;
      fr_record_name_t install_fields[FR_RECORD_FIELDS_PER_SHAPE_CAPACITY];

      FR_TRY(fr_persist_find_shape_record(shape_records, shape_count,
                                          object_id, &shape));
      for (uint16_t j = 0; j < shape->field_count; j++) {
        install_fields[j] = record_fields[shape->first_field + j];
      }
      FR_TRY(fr_record_shape_install_since(
          runtime, shape->name, install_fields, shape->field_count,
          runtime->objects.base_count, &installed_id));
#endif
    } else if (object_kinds[object_id] == FR_PERSIST_OBJECT_RECORD) {
#if !FR_FEATURE_RECORDS
      return FR_ERR_UNSUPPORTED;
#else
      const fr_persist_record_record_t *record = NULL;
      fr_tagged_t record_install_values[FR_RECORD_FIELDS_PER_SHAPE_CAPACITY];

      FR_TRY(fr_persist_find_record_record(record_records, record_count,
                                           object_id, &record));
      if (record->field_count > FR_RECORD_FIELDS_PER_SHAPE_CAPACITY) {
        return FR_ERR_CORRUPT;
      }
      for (uint16_t j = 0; j < record->field_count; j++) {
        fr_tagged_t stored = record_values[record->first_value + j];
        fr_object_id_t ref = 0;

        if (fr_tagged_decode_object_id(stored, &ref) == FR_OK) {
          if (ref >= object_count || !decoded->object_present[ref] ||
              object_kinds[ref] != FR_PERSIST_OBJECT_TEXT) {
            return FR_ERR_CORRUPT;
          }
        }
        if (!fr_record_field_value_allowed(runtime, stored)) {
          return FR_ERR_CORRUPT;
        }
        record_install_values[j] = stored;
      }
      FR_TRY(fr_record_install(runtime, record->shape_local_id,
                               record_install_values, record->field_count,
                               &installed_id));
#endif
    } else if (object_kinds[object_id] == FR_PERSIST_OBJECT_CELLS) {
#if !FR_FEATURE_CELLS
      return FR_ERR_UNSUPPORTED;
#else
      const fr_persist_cell_record_t *cell = NULL;

      FR_TRY(
          fr_persist_find_cell_record(cell_records, cell_count, object_id,
                                      &cell));
      for (uint16_t j = 0; j < cell->length; j++) {
        fr_tagged_t stored = cell_values[cell->first_value + j];
        fr_object_id_t ref = 0;

        if (fr_tagged_decode_object_id(stored, &ref) == FR_OK) {
          if (ref >= object_count || !decoded->object_present[ref] ||
              (object_kinds[ref] != FR_PERSIST_OBJECT_TEXT
#if FR_FEATURE_RECORDS
               && object_kinds[ref] != FR_PERSIST_OBJECT_RECORD
#endif
               )) {
            return FR_ERR_CORRUPT;
          }
        }
        if (!fr_cells_value_allowed(runtime, stored)) {
          return FR_ERR_CORRUPT;
        }
        cell_install_values[j] = stored;
      }
      FR_TRY(fr_cells_install(runtime, cell->length, cell_install_values,
                              &installed_id));
#endif
    } else {
      return FR_ERR_CORRUPT;
    }

    if (installed_id != object_id) {
      return FR_ERR_CORRUPT;
    }
  }

  for (uint16_t i = runtime->code.base_image_count; i < code_count; i++) {
    fr_instruction_stream_t instructions;
    fr_code_object_id_t mounted_id = 0;

    if (!decoded->code_present[i]) {
      continue;
    }
    if (code_filter != NULL && !code_filter[i]) {
      continue;
    }
    FR_TRY(fr_instruction_stream_init(&instructions, code_records[i].bytes,
                                      code_records[i].length));
    FR_TRY(fr_code_mount_image(runtime, &instructions, NULL, 0, &mounted_id));
    if (mounted_id != i) {
      return FR_ERR_CORRUPT;
    }
  }
  return FR_OK;
}

#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
/* Tier of the bind record that owns this slot in this payload. Names carry
 * no tier on the wire (P1), so we read it from the matching bind. Returns 0
 * if no bind matches — the validate step rules that out, so this only happens
 * on a programming error in the caller. */
static uint8_t
fr_persist_name_slot_tier(const fr_persist_decoded_payload_t *decoded,
                          fr_slot_id_t slot_id) {
  for (uint16_t i = 0; i < decoded->bind_count; i++) {
    if (decoded->bind_records[i].slot_id == slot_id) {
      return decoded->bind_records[i].tier;
    }
  }
  return 0;
}
#endif

/* Applies overlay name bindings filtered by the bind tier of each name's
 * slot. tier_filter == 0 applies every name (FULL restore). The two-call
 * boot path uses FR_PERSIST_TIER_LIBRARY on the L1 pass so library words
 * resolve by name before the L2 pass runs, and FR_PERSIST_TIER_USER on the
 * L2 pass to land the remaining names. */
static fr_err_t
fr_persist_apply_names(fr_runtime_t *runtime,
                       const fr_persist_decoded_payload_t *decoded,
                       uint8_t tier_filter) {
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  for (uint16_t i = 0; i < decoded->name_count; i++) {
    if (tier_filter != 0) {
      uint8_t name_tier = fr_persist_name_slot_tier(
          decoded, decoded->name_records[i].slot_id);
      if (name_tier != tier_filter) {
        continue;
      }
    }
    FR_TRY(fr_slot_bind_project_name(runtime, decoded->name_records[i].name,
                                     decoded->name_records[i].slot_id));
  }
#else
  (void)runtime;
  (void)decoded;
  (void)tier_filter;
#endif
  return FR_OK;
}

static fr_err_t
fr_persist_apply_events(fr_runtime_t *runtime,
                        const fr_persist_decoded_payload_t *decoded) {
#if FR_FEATURE_EVENTS
  /* fr_runtime_reset cleared the binding table at the start of the L1 pass;
   * re-register each decoded record in payload order. fr_event_register stages
   * platform install before writing the table (event.c:88-103), so a failed
   * install leaves the slot empty and the restore aborts. */
  for (uint16_t i = 0; i < decoded->event_count; i++) {
    const fr_image_event_binding_t *binding = &decoded->event_records[i];

    FR_TRY(fr_event_register(runtime, binding->kind, binding->source,
                             binding->debounce_ms, binding->body));
  }
#else
  (void)runtime;
  (void)decoded;
#endif
  return FR_OK;
}

/* Shared decode-and-validate step. Sets up the decoded payload plus the
 * per-bind / per-name checks the install path relies on. Splitting it out
 * keeps the three public entry points (FULL restore, L1 boot pass, L2 boot
 * pass) honest about doing the same pre-flight before they diverge. */
static fr_err_t
fr_persist_payload_decode_and_validate(const fr_runtime_t *runtime,
                                       const uint8_t *bytes, uint16_t length,
                                       fr_persist_decoded_payload_t *decoded) {
  if (runtime == NULL || bytes == NULL || decoded == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_persist_decode_payload(bytes, length, decoded));
  FR_TRY(fr_persist_check_restore_capacity(runtime, decoded));
  FR_TRY(fr_persist_check_final_image_refs(runtime, decoded));
  for (uint16_t i = 0; i < decoded->bind_count; i++) {
    FR_TRY(fr_persist_check_bind_record(runtime, &decoded->bind_records[i],
                                        decoded->code_present,
                                        decoded->code_count,
                                        decoded->object_present,
                                        decoded->object_count));
  }
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  {
    fr_slot_id_t slot_count_after_writes = fr_persist_slot_count_after_restore(
        runtime, decoded->bind_records, decoded->bind_count);

    FR_TRY(fr_persist_check_name_records(decoded->name_records,
                                         decoded->name_count,
                                         slot_count_after_writes));
  }
#endif
  return FR_OK;
}

fr_err_t fr_persist_payload_restore(fr_runtime_t *runtime, const uint8_t *bytes,
                                    uint16_t length) {
  fr_persist_decoded_payload_t decoded;

  FR_TRY(fr_persist_payload_decode_and_validate(runtime, bytes, length,
                                                &decoded));
  FR_TRY(fr_runtime_reset(runtime));
  fr_code_restore_base(runtime);
  fr_object_restore_base(runtime);
  FR_TRY(fr_persist_install_resources(runtime, &decoded, NULL, NULL));
  fr_object_mark_image(runtime);
  /* D6 single-call path applies both tiers in tier order so user definitions
   * see library slots already in place. tier_filter=0 selects both passes;
   * skip_on_failure=false matches the strict FULL-restore contract. */
  FR_TRY(fr_persist_apply_binds_tiered(runtime, &decoded, 0, false, true));
  FR_TRY(fr_persist_apply_names(runtime, &decoded, 0));
  FR_TRY(fr_persist_apply_events(runtime, &decoded));
  return FR_OK;
}

/* D6 boot L1 pass: reset the runtime, install all resources the payload
 * references (codes, texts, shapes, records, cells), then apply only L1
 * binds and the names whose slot is L1. A per-bind failure here writes
 * the SPEC-shaped warning and the walk continues; binding L1 names in
 * this pass is what makes a library word resolve via fr_slot_id_for_name
 * before the L2 pass runs. */
fr_err_t fr_persist_payload_restore_library(fr_runtime_t *runtime,
                                            const uint8_t *bytes,
                                            uint16_t length) {
  fr_persist_decoded_payload_t decoded;

  fr_persist_boot_library_valid = false;
  FR_TRY(fr_persist_payload_decode_and_validate(runtime, bytes, length,
                                                &decoded));
  FR_TRY(fr_runtime_reset(runtime));
  fr_code_restore_base(runtime);
  fr_object_restore_base(runtime);
  FR_TRY(fr_persist_install_resources(runtime, &decoded, NULL, NULL));
  fr_object_mark_image(runtime);
  FR_TRY(fr_persist_apply_binds_tiered(runtime, &decoded,
                                       (uint8_t)FR_PERSIST_TIER_LIBRARY, true,
                                       true));
  FR_TRY(fr_persist_apply_names(runtime, &decoded,
                                (uint8_t)FR_PERSIST_TIER_LIBRARY));
  fr_persist_boot_library_valid = true;
  return FR_OK;
}

/* Save's L1 preservation step (D5). Decodes the existing NVS payload to work
 * out which records belong to the L1 closure (every CODE / object / BIND /
 * NAME the L1 tier reaches), then walks the source bytes a second time and
 * memcpys each L1-qualifying record into dst in its original on-wire order.
 *
 * Source-order verbatim emission is the contract: D5 says "L1 bytes in NVS
 * are preserved unchanged across save." The install path emits a TEXT
 * referenced by a CODE body before the CODE that pushes it, and nested CODE
 * before its parent — re-emitting CODE-then-TEXT (or any other category
 * sort) would change those bytes. Walking source order keeps the prefix
 * byte-identical to the L1 region of the input payload.
 *
 * The L1 closure widens through three relations:
 *   1. L1 BIND -> its value's CODE / object id.
 *   2. CODE body's PUSH_CODE_ID / PUSH_OBJECT_ID operands -> nested
 *      CODE / object ids. The widen runs until no new ids land.
 *   3. RECORD object -> its SHAPE; RECORD / CELL object value fields ->
 *      any other object ids those fields encode. Each pass widens until
 *      stable so an L1 record that points at another record that points at
 *      a text still reaches the text.
 *
 * Object closure no longer narrows to TEXT — an L1 BIND of a CELLS / RECORD
 * / RECORD_SHAPE object reaches the matching object record and the second-
 * pass walker emits it verbatim. */
fr_err_t fr_persist_payload_extract_library_records(
    const uint8_t *src, uint16_t src_length, uint8_t *dst, uint16_t dst_cap,
    uint16_t *out_length) {
  fr_persist_decoded_payload_t decoded;
  fr_persist_writer_t writer = {dst, 0, dst_cap};
  bool code_in_l1[FR_PROFILE_CODE_OBJECT_TABLE_SIZE];
  bool object_in_l1[FR_OBJECT_TABLE_CAPACITY];
  bool slot_in_l1[FR_PROFILE_MAX_SLOTS];
  bool closure_changed = true;
  fr_persist_reader_t scan = {src, src_length, 0};
  const uint8_t *magic_scratch = NULL;
  uint8_t version_scratch = 0;

  if (src == NULL || dst == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }
  memset(code_in_l1, 0, sizeof(code_in_l1));
  memset(object_in_l1, 0, sizeof(object_in_l1));
  memset(slot_in_l1, 0, sizeof(slot_in_l1));

  FR_TRY(fr_persist_decode_payload(src, src_length, &decoded));

  for (uint16_t i = 0; i < decoded.bind_count; i++) {
    const fr_persist_bind_record_t *bind = &decoded.bind_records[i];

    if (bind->tier != FR_PERSIST_TIER_LIBRARY) {
      continue;
    }
    if (bind->slot_id < FR_PROFILE_MAX_SLOTS) {
      slot_in_l1[bind->slot_id] = true;
    }
    if (bind->value_kind == FR_PERSIST_VALUE_CODE) {
      if (bind->value_word >= decoded.code_count ||
          !decoded.code_present[bind->value_word]) {
        return FR_ERR_CORRUPT;
      }
      code_in_l1[bind->value_word] = true;
    } else if (bind->value_kind == FR_PERSIST_VALUE_OBJECT) {
      if (bind->value_word >= decoded.object_count ||
          !decoded.object_present[bind->value_word]) {
        return FR_ERR_CORRUPT;
      }
      object_in_l1[bind->value_word] = true;
    }
  }

  while (closure_changed) {
    closure_changed = false;
    for (uint16_t i = 0; i < decoded.code_count; i++) {
      const fr_persist_code_record_t *code = &decoded.code_records[i];
      fr_instruction_stream_t view;
      fr_instruction_header_t header = {0};
      fr_code_offset_t ip = 0;
      char scratch[64];

      if (!code_in_l1[i]) {
        continue;
      }
      if (!decoded.code_present[i]) {
        return FR_ERR_CORRUPT;
      }
      FR_TRY(fr_instruction_stream_init(&view, code->bytes, code->length));
      FR_TRY(fr_instruction_read_header(&view, &header));
      ip = (fr_code_offset_t)header.header_size;
      while (ip < view.length) {
        fr_code_offset_t next_ip = 0;
        fr_opcode_t op = (fr_opcode_t)code->bytes[ip];

        if (op == FR_OP_PUSH_CODE_ID) {
          uint16_t ref = (uint16_t)code->bytes[ip + 1] |
                         ((uint16_t)code->bytes[ip + 2] << 8);
          if (ref >= decoded.code_count || !decoded.code_present[ref]) {
            return FR_ERR_CORRUPT;
          }
          if (!code_in_l1[ref]) {
            code_in_l1[ref] = true;
            closure_changed = true;
          }
        }
#if FR_FEATURE_OBJECTS
        if (op == FR_OP_PUSH_OBJECT_ID) {
          uint16_t ref = (uint16_t)code->bytes[ip + 1] |
                         ((uint16_t)code->bytes[ip + 2] << 8);
          if (ref >= decoded.object_count || !decoded.object_present[ref]) {
            return FR_ERR_CORRUPT;
          }
          if (!object_in_l1[ref]) {
            object_in_l1[ref] = true;
            closure_changed = true;
          }
        }
#endif
        FR_TRY(fr_instruction_disassemble_at(&view, ip, scratch,
                                             (uint16_t)sizeof(scratch), NULL,
                                             &next_ip));
        if (next_ip <= ip) {
          return FR_ERR_CORRUPT;
        }
        ip = next_ip;
      }
    }
#if FR_FEATURE_RECORDS
    /* A RECORD object reaches its SHAPE plus any object refs its field
     * values carry — those refs sit in decoded.record_values as tagged_t. */
    for (uint16_t i = 0; i < decoded.record_count; i++) {
      const fr_persist_record_record_t *rec = &decoded.record_records[i];

      if (rec->local_id >= FR_OBJECT_TABLE_CAPACITY ||
          !object_in_l1[rec->local_id]) {
        continue;
      }
      if (rec->shape_local_id >= FR_OBJECT_TABLE_CAPACITY ||
          !decoded.object_present[rec->shape_local_id]) {
        return FR_ERR_CORRUPT;
      }
      if (!object_in_l1[rec->shape_local_id]) {
        object_in_l1[rec->shape_local_id] = true;
        closure_changed = true;
      }
      for (uint16_t j = 0; j < rec->field_count; j++) {
        fr_tagged_t val = decoded.record_values[rec->first_value + j];
        fr_object_id_t obj = 0;

        if (fr_tagged_decode_object_id(val, &obj) != FR_OK) {
          continue;
        }
        if ((uint16_t)obj >= FR_OBJECT_TABLE_CAPACITY ||
            !decoded.object_present[(uint16_t)obj]) {
          return FR_ERR_CORRUPT;
        }
        if (!object_in_l1[(uint16_t)obj]) {
          object_in_l1[(uint16_t)obj] = true;
          closure_changed = true;
        }
      }
    }
#endif
#if FR_FEATURE_CELLS
    /* A CELLS object reaches any object refs its value slots carry. */
    for (uint16_t i = 0; i < decoded.cell_count; i++) {
      const fr_persist_cell_record_t *cell = &decoded.cell_records[i];

      if (cell->local_id >= FR_OBJECT_TABLE_CAPACITY ||
          !object_in_l1[cell->local_id]) {
        continue;
      }
      for (uint16_t j = 0; j < cell->length; j++) {
        fr_tagged_t val = decoded.cell_values[cell->first_value + j];
        fr_object_id_t obj = 0;

        if (fr_tagged_decode_object_id(val, &obj) != FR_OK) {
          continue;
        }
        if ((uint16_t)obj >= FR_OBJECT_TABLE_CAPACITY ||
            !decoded.object_present[(uint16_t)obj]) {
          return FR_ERR_CORRUPT;
        }
        if (!object_in_l1[(uint16_t)obj]) {
          object_in_l1[(uint16_t)obj] = true;
          closure_changed = true;
        }
      }
    }
#endif
  }

  /* Source-order walk: advance through src record by record and memcpy any
   * record that the closure marked L1. Events never qualify — D5 makes
   * events L2-only at the encoder. */
  FR_TRY(fr_persist_reader_bytes(&scan,
                                 (uint16_t)sizeof(fr_persist_payload_magic),
                                 &magic_scratch));
  FR_TRY(fr_persist_reader_u8(&scan, &version_scratch));
  while (true) {
    uint16_t record_start = scan.offset;
    uint8_t tag = 0;
    uint16_t key = 0;
    uint16_t record_length = 0;
    bool qualifies = false;

    FR_TRY(fr_persist_payload_advance_record(&scan, &tag, &key));
    if (tag == FR_PERSIST_RECORD_END) {
      break;
    }
    record_length = (uint16_t)(scan.offset - record_start);

    switch (tag) {
    case FR_PERSIST_RECORD_CODE:
      qualifies =
          key < FR_PROFILE_CODE_OBJECT_TABLE_SIZE && code_in_l1[key];
      break;
    case FR_PERSIST_RECORD_TEXT:
#if FR_FEATURE_RECORDS
    case FR_PERSIST_RECORD_RECORD_SHAPE:
    case FR_PERSIST_RECORD_RECORD:
#endif
#if FR_FEATURE_CELLS
    case FR_PERSIST_RECORD_CELLS:
#endif
      qualifies = key < FR_OBJECT_TABLE_CAPACITY && object_in_l1[key];
      break;
    case FR_PERSIST_RECORD_BIND:
    case FR_PERSIST_RECORD_NAME:
      qualifies = key < FR_PROFILE_MAX_SLOTS && slot_in_l1[key];
      break;
#if FR_FEATURE_EVENTS
    case FR_PERSIST_RECORD_EVENT:
      qualifies = false;
      break;
#endif
    default:
      return FR_ERR_CORRUPT;
    }
    if (qualifies) {
      FR_TRY(fr_persist_writer_bytes(&writer, src + record_start,
                                     record_length));
    }
  }

  *out_length = writer.used;
  return FR_OK;
}

/* SPEC D10 + D5: install-library on receipt drops L1 records from NVS
 * while preserving L2 bytes byte-for-byte. Mirror of
 * fr_persist_payload_extract_library_records with the opposite polarity —
 * walk src record by record and copy each one that the L1 closure did NOT
 * mark. Events always qualify (D5 makes events L2-only at the encoder).
 *
 * The result is a complete payload (magic + version + L2 records + END):
 * the install-library caller writes it directly to NVS without further
 * wrapping. */
fr_err_t fr_persist_payload_strip_library_records(
    const uint8_t *src, uint16_t src_length, uint8_t *dst, uint16_t dst_cap,
    uint16_t *out_length) {
  fr_persist_decoded_payload_t decoded;
  fr_persist_writer_t writer = {dst, 0, dst_cap};
  bool code_in_l1[FR_PROFILE_CODE_OBJECT_TABLE_SIZE];
  bool object_in_l1[FR_OBJECT_TABLE_CAPACITY];
  bool slot_in_l1[FR_PROFILE_MAX_SLOTS];
  bool closure_changed = true;
  fr_persist_reader_t scan = {src, src_length, 0};
  const uint8_t *magic_scratch = NULL;
  uint8_t version_scratch = 0;

  if (src == NULL || dst == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }
  memset(code_in_l1, 0, sizeof(code_in_l1));
  memset(object_in_l1, 0, sizeof(object_in_l1));
  memset(slot_in_l1, 0, sizeof(slot_in_l1));

  FR_TRY(fr_persist_decode_payload(src, src_length, &decoded));

  for (uint16_t i = 0; i < decoded.bind_count; i++) {
    const fr_persist_bind_record_t *bind = &decoded.bind_records[i];

    if (bind->tier != FR_PERSIST_TIER_LIBRARY) {
      continue;
    }
    if (bind->slot_id < FR_PROFILE_MAX_SLOTS) {
      slot_in_l1[bind->slot_id] = true;
    }
    if (bind->value_kind == FR_PERSIST_VALUE_CODE) {
      if (bind->value_word >= decoded.code_count ||
          !decoded.code_present[bind->value_word]) {
        return FR_ERR_CORRUPT;
      }
      code_in_l1[bind->value_word] = true;
    } else if (bind->value_kind == FR_PERSIST_VALUE_OBJECT) {
      if (bind->value_word >= decoded.object_count ||
          !decoded.object_present[bind->value_word]) {
        return FR_ERR_CORRUPT;
      }
      object_in_l1[bind->value_word] = true;
    }
  }

  while (closure_changed) {
    closure_changed = false;
    for (uint16_t i = 0; i < decoded.code_count; i++) {
      const fr_persist_code_record_t *code = &decoded.code_records[i];
      fr_instruction_stream_t view;
      fr_instruction_header_t header = {0};
      fr_code_offset_t ip = 0;
      char scratch[64];

      if (!code_in_l1[i]) {
        continue;
      }
      if (!decoded.code_present[i]) {
        return FR_ERR_CORRUPT;
      }
      FR_TRY(fr_instruction_stream_init(&view, code->bytes, code->length));
      FR_TRY(fr_instruction_read_header(&view, &header));
      ip = (fr_code_offset_t)header.header_size;
      while (ip < view.length) {
        fr_code_offset_t next_ip = 0;
        fr_opcode_t op = (fr_opcode_t)code->bytes[ip];

        if (op == FR_OP_PUSH_CODE_ID) {
          uint16_t ref = (uint16_t)code->bytes[ip + 1] |
                         ((uint16_t)code->bytes[ip + 2] << 8);
          if (ref >= decoded.code_count || !decoded.code_present[ref]) {
            return FR_ERR_CORRUPT;
          }
          if (!code_in_l1[ref]) {
            code_in_l1[ref] = true;
            closure_changed = true;
          }
        }
#if FR_FEATURE_OBJECTS
        if (op == FR_OP_PUSH_OBJECT_ID) {
          uint16_t ref = (uint16_t)code->bytes[ip + 1] |
                         ((uint16_t)code->bytes[ip + 2] << 8);
          if (ref >= decoded.object_count || !decoded.object_present[ref]) {
            return FR_ERR_CORRUPT;
          }
          if (!object_in_l1[ref]) {
            object_in_l1[ref] = true;
            closure_changed = true;
          }
        }
#endif
        FR_TRY(fr_instruction_disassemble_at(&view, ip, scratch,
                                             (uint16_t)sizeof(scratch), NULL,
                                             &next_ip));
        if (next_ip <= ip) {
          return FR_ERR_CORRUPT;
        }
        ip = next_ip;
      }
    }
#if FR_FEATURE_RECORDS
    for (uint16_t i = 0; i < decoded.record_count; i++) {
      const fr_persist_record_record_t *rec = &decoded.record_records[i];

      if (rec->local_id >= FR_OBJECT_TABLE_CAPACITY ||
          !object_in_l1[rec->local_id]) {
        continue;
      }
      if (rec->shape_local_id >= FR_OBJECT_TABLE_CAPACITY ||
          !decoded.object_present[rec->shape_local_id]) {
        return FR_ERR_CORRUPT;
      }
      if (!object_in_l1[rec->shape_local_id]) {
        object_in_l1[rec->shape_local_id] = true;
        closure_changed = true;
      }
      for (uint16_t j = 0; j < rec->field_count; j++) {
        fr_tagged_t val = decoded.record_values[rec->first_value + j];
        fr_object_id_t obj = 0;

        if (fr_tagged_decode_object_id(val, &obj) != FR_OK) {
          continue;
        }
        if ((uint16_t)obj >= FR_OBJECT_TABLE_CAPACITY ||
            !decoded.object_present[(uint16_t)obj]) {
          return FR_ERR_CORRUPT;
        }
        if (!object_in_l1[(uint16_t)obj]) {
          object_in_l1[(uint16_t)obj] = true;
          closure_changed = true;
        }
      }
    }
#endif
#if FR_FEATURE_CELLS
    for (uint16_t i = 0; i < decoded.cell_count; i++) {
      const fr_persist_cell_record_t *cell = &decoded.cell_records[i];

      if (cell->local_id >= FR_OBJECT_TABLE_CAPACITY ||
          !object_in_l1[cell->local_id]) {
        continue;
      }
      for (uint16_t j = 0; j < cell->length; j++) {
        fr_tagged_t val = decoded.cell_values[cell->first_value + j];
        fr_object_id_t obj = 0;

        if (fr_tagged_decode_object_id(val, &obj) != FR_OK) {
          continue;
        }
        if ((uint16_t)obj >= FR_OBJECT_TABLE_CAPACITY ||
            !decoded.object_present[(uint16_t)obj]) {
          return FR_ERR_CORRUPT;
        }
        if (!object_in_l1[(uint16_t)obj]) {
          object_in_l1[(uint16_t)obj] = true;
          closure_changed = true;
        }
      }
    }
#endif
  }

  FR_TRY(fr_persist_writer_bytes(&writer, fr_persist_payload_magic,
                                 (uint16_t)sizeof(fr_persist_payload_magic)));
  FR_TRY(fr_persist_writer_u8(&writer, FR_PERSIST_PAYLOAD_VERSION));

  FR_TRY(fr_persist_reader_bytes(&scan,
                                 (uint16_t)sizeof(fr_persist_payload_magic),
                                 &magic_scratch));
  FR_TRY(fr_persist_reader_u8(&scan, &version_scratch));
  while (true) {
    uint16_t record_start = scan.offset;
    uint8_t tag = 0;
    uint16_t key = 0;
    uint16_t record_length = 0;
    bool drop = false;

    FR_TRY(fr_persist_payload_advance_record(&scan, &tag, &key));
    if (tag == FR_PERSIST_RECORD_END) {
      break;
    }
    record_length = (uint16_t)(scan.offset - record_start);

    switch (tag) {
    case FR_PERSIST_RECORD_CODE:
      drop = key < FR_PROFILE_CODE_OBJECT_TABLE_SIZE && code_in_l1[key];
      break;
    case FR_PERSIST_RECORD_TEXT:
#if FR_FEATURE_RECORDS
    case FR_PERSIST_RECORD_RECORD_SHAPE:
    case FR_PERSIST_RECORD_RECORD:
#endif
#if FR_FEATURE_CELLS
    case FR_PERSIST_RECORD_CELLS:
#endif
      drop = key < FR_OBJECT_TABLE_CAPACITY && object_in_l1[key];
      break;
    case FR_PERSIST_RECORD_BIND:
    case FR_PERSIST_RECORD_NAME:
      drop = key < FR_PROFILE_MAX_SLOTS && slot_in_l1[key];
      break;
#if FR_FEATURE_EVENTS
    case FR_PERSIST_RECORD_EVENT:
      drop = false;
      break;
#endif
    default:
      return FR_ERR_CORRUPT;
    }
    if (!drop) {
      FR_TRY(fr_persist_writer_bytes(&writer, src + record_start,
                                     record_length));
    }
  }

  FR_TRY(fr_persist_writer_u8(&writer, FR_PERSIST_RECORD_END));

  *out_length = writer.used;
  return FR_OK;
}

/* D6 boot L2 pass: must follow a successful library pass on the same
 * payload. Applies only L2 binds onto the runtime the L1 pass left behind,
 * then installs names and events. Returns FR_ERR_NOT_FOUND if no L1 pass
 * started this sequence. */
fr_err_t fr_persist_payload_restore_user_after_library(fr_runtime_t *runtime,
                                                       const uint8_t *bytes,
                                                       uint16_t length) {
  fr_persist_decoded_payload_t decoded;

  if (runtime == NULL || bytes == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_persist_boot_library_valid) {
    return FR_ERR_NOT_FOUND;
  }
  /* The L1 pass already passed the validate checks against the same payload;
   * decode again (the decoded record arrays live on this call's stack), then
   * apply only L2 binds plus names and events. */
  FR_TRY(fr_persist_decode_payload(bytes, length, &decoded));
  FR_TRY(fr_persist_apply_binds_tiered(
      runtime, &decoded, (uint8_t)FR_PERSIST_TIER_USER, false, true));
  FR_TRY(fr_persist_apply_names(runtime, &decoded,
                                (uint8_t)FR_PERSIST_TIER_USER));
  FR_TRY(fr_persist_apply_events(runtime, &decoded));
  fr_persist_boot_library_valid = false;
  return FR_OK;
}

/* D5 acceptance #6: the public `restore` word brings L2 back from NVS while
 * leaving L1 binds in place. Code is the exception in storage terms: it is
 * rewound to the base-image boundary, then remounted from the payload so
 * preserved library slots still point at executable ids and repeated restore
 * calls cannot grow or drift the image id range. Binds, names, and events are
 * still applied for the user tier only. */
fr_err_t fr_persist_payload_restore_user_only(fr_runtime_t *runtime,
                                              const uint8_t *bytes,
                                              uint16_t length) {
  fr_persist_decoded_payload_t decoded;

  FR_TRY(fr_persist_payload_decode_and_validate(runtime, bytes, length,
                                                &decoded));
  fr_persist_session_wipe_user_tier(runtime);
  fr_code_restore_base(runtime);
  fr_object_restore_base(runtime);
  FR_TRY(fr_persist_install_resources(runtime, &decoded, NULL, NULL));
  fr_object_mark_image(runtime);
  FR_TRY(fr_persist_apply_binds_tiered(runtime, &decoded,
                                       (uint8_t)FR_PERSIST_TIER_USER, false,
                                       true));
  FR_TRY(fr_persist_apply_names(runtime, &decoded,
                                (uint8_t)FR_PERSIST_TIER_USER));
  FR_TRY(fr_persist_apply_events(runtime, &decoded));
  return FR_OK;
}
