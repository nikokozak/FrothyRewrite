#include "profile.h"

#include "base_defs.h"
#include "config.h"
#include "crc.h"
#include "lib_native.h"
#include "tagged.h"

#if FR_FEATURE_SOURCE_BASE
#include "fr_source_base.h"
#endif

#ifndef FR_PROFILE_NAME
#define FR_PROFILE_NAME FR_PROFILE_HEADER
#endif

#ifndef FR_PROFILE_CONTRACT_NAME
#define FR_PROFILE_CONTRACT_NAME FR_PROFILE_NAME
#endif

#ifndef FR_PROFILE_HASH_WORD_SIZE
#define FR_PROFILE_HASH_WORD_SIZE FR_WORD_SIZE
#endif

#ifndef FR_PROFILE_HASH_MAX_SLOTS
#define FR_PROFILE_HASH_MAX_SLOTS FR_PROFILE_MAX_SLOTS
#endif

#ifndef FR_PROFILE_HASH_MAX_INSTRUCTION_BYTES
#define FR_PROFILE_HASH_MAX_INSTRUCTION_BYTES FR_PROFILE_MAX_INSTRUCTION_BYTES
#endif

#ifndef FR_PROFILE_HASH_TOTAL_IMAGE_BYTES
#define FR_PROFILE_HASH_TOTAL_IMAGE_BYTES FR_PROFILE_TOTAL_IMAGE_BYTES
#endif

#ifndef FR_PROFILE_HASH_MAX_DEFINITION_INSTRUCTION_BYTES
#define FR_PROFILE_HASH_MAX_DEFINITION_INSTRUCTION_BYTES                      \
  FR_PROFILE_MAX_DEFINITION_INSTRUCTION_BYTES
#endif

#ifndef FR_PROFILE_HASH_MAX_DEFINITION_TEXT_BYTES
#define FR_PROFILE_HASH_MAX_DEFINITION_TEXT_BYTES                             \
  FR_PROFILE_MAX_DEFINITION_TEXT_BYTES
#endif

#ifndef FR_PROFILE_HASH_MAX_DEFINITION_CODE_OBJECTS
#define FR_PROFILE_HASH_MAX_DEFINITION_CODE_OBJECTS                           \
  FR_PROFILE_MAX_DEFINITION_CODE_OBJECTS
#endif

#ifndef FR_PROFILE_HASH_MAX_OVERLAY_CODE_BYTES
#define FR_PROFILE_HASH_MAX_OVERLAY_CODE_BYTES FR_PROFILE_MAX_OVERLAY_CODE_BYTES
#endif

#ifndef FR_PROFILE_HASH_MAX_OVERLAY_TEXT_BYTES
#define FR_PROFILE_HASH_MAX_OVERLAY_TEXT_BYTES FR_PROFILE_MAX_OVERLAY_TEXT_BYTES
#endif

#ifndef FR_PROFILE_HASH_MAX_OVERLAY_PARAM_NAME_BYTES
#define FR_PROFILE_HASH_MAX_OVERLAY_PARAM_NAME_BYTES                          \
  FR_PROFILE_MAX_OVERLAY_PARAM_NAME_BYTES
#endif

#ifndef FR_PROFILE_HASH_MAX_SOURCE_RENDER_BYTES
#define FR_PROFILE_HASH_MAX_SOURCE_RENDER_BYTES FR_PROFILE_MAX_SOURCE_RENDER_BYTES
#endif

#ifndef FR_PROFILE_HASH_MAX_STACK_DEPTH
#define FR_PROFILE_HASH_MAX_STACK_DEPTH FR_PROFILE_MAX_STACK_DEPTH
#endif

#ifndef FR_PROFILE_HASH_CODE_OBJECT_TABLE_SIZE
#define FR_PROFILE_HASH_CODE_OBJECT_TABLE_SIZE FR_PROFILE_CODE_OBJECT_TABLE_SIZE
#endif

#ifndef FR_PROFILE_HASH_NATIVE_TABLE_SIZE
#define FR_PROFILE_HASH_NATIVE_TABLE_SIZE FR_PROFILE_NATIVE_TABLE_SIZE
#endif

#ifndef FR_PROFILE_HASH_MAX_HANDLES
#define FR_PROFILE_HASH_MAX_HANDLES FR_PROFILE_MAX_HANDLES
#endif

#ifndef FR_PROFILE_HASH_MAX_CALL_DEPTH
#define FR_PROFILE_HASH_MAX_CALL_DEPTH FR_PROFILE_MAX_CALL_DEPTH
#endif

#ifndef FR_PROFILE_HASH_MAX_ATTEMPT_DEPTH
#define FR_PROFILE_HASH_MAX_ATTEMPT_DEPTH FR_PROFILE_MAX_ATTEMPT_DEPTH
#endif

#ifndef FR_PROFILE_HASH_REPL_LINE_BYTES
#define FR_PROFILE_HASH_REPL_LINE_BYTES FR_PROFILE_REPL_LINE_BYTES
#endif

#ifndef FR_PROFILE_HASH_PERSISTENCE_BYTES
#define FR_PROFILE_HASH_PERSISTENCE_BYTES FR_PROFILE_PERSISTENCE_BYTES
#endif

#ifndef FR_PROFILE_HASH_MAX_OVERLAY_NAMES
#define FR_PROFILE_HASH_MAX_OVERLAY_NAMES FR_PROFILE_MAX_OVERLAY_NAMES
#endif

#ifndef FR_PROFILE_HASH_MAX_OVERLAY_UPDATE_SLOT_INITS
#define FR_PROFILE_HASH_MAX_OVERLAY_UPDATE_SLOT_INITS                         \
  FR_PROFILE_MAX_OVERLAY_UPDATE_SLOT_INITS
#endif

#ifndef FR_PROFILE_HASH_MAX_OVERLAY_UPDATE_CODE_OBJECTS
#define FR_PROFILE_HASH_MAX_OVERLAY_UPDATE_CODE_OBJECTS                       \
  FR_PROFILE_MAX_OVERLAY_UPDATE_CODE_OBJECTS
#endif

#ifndef FR_PROFILE_HASH_MAX_OVERLAY_UPDATE_INSTRUCTION_BYTES
#define FR_PROFILE_HASH_MAX_OVERLAY_UPDATE_INSTRUCTION_BYTES                  \
  FR_PROFILE_MAX_OVERLAY_UPDATE_INSTRUCTION_BYTES
#endif

#ifndef FR_PROFILE_HASH_MAX_OVERLAY_UPDATE_NAMES
#define FR_PROFILE_HASH_MAX_OVERLAY_UPDATE_NAMES                              \
  FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES
#endif

#ifndef FR_PROFILE_HASH_OBJECT_TABLE_SIZE
#define FR_PROFILE_HASH_OBJECT_TABLE_SIZE FR_PROFILE_OBJECT_TABLE_SIZE
#endif

#ifndef FR_PROFILE_HASH_MAX_CELL_WORDS
#define FR_PROFILE_HASH_MAX_CELL_WORDS FR_PROFILE_MAX_CELL_WORDS
#endif

#ifndef FR_PROFILE_HASH_MAX_CELL_LENGTH
#define FR_PROFILE_HASH_MAX_CELL_LENGTH FR_PROFILE_MAX_CELL_LENGTH
#endif

#ifndef FR_PROFILE_HASH_MAX_TEXT_BYTES
#define FR_PROFILE_HASH_MAX_TEXT_BYTES FR_PROFILE_MAX_TEXT_BYTES
#endif

#ifndef FR_PROFILE_HASH_MAX_TEXT_LENGTH
#define FR_PROFILE_HASH_MAX_TEXT_LENGTH FR_PROFILE_MAX_TEXT_LENGTH
#endif

#ifndef FR_PROFILE_HASH_MAX_RECORD_NAME_BYTES
#define FR_PROFILE_HASH_MAX_RECORD_NAME_BYTES FR_PROFILE_MAX_RECORD_NAME_BYTES
#endif

#ifndef FR_PROFILE_HASH_MAX_RECORD_FIELDS_PER_SHAPE
#define FR_PROFILE_HASH_MAX_RECORD_FIELDS_PER_SHAPE                           \
  FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE
#endif

#ifndef FR_PROFILE_HASH_MAX_RECORD_SHAPE_FIELDS
#define FR_PROFILE_HASH_MAX_RECORD_SHAPE_FIELDS                               \
  FR_PROFILE_MAX_RECORD_SHAPE_FIELDS
#endif

#ifndef FR_PROFILE_HASH_MAX_RECORD_VALUE_FIELDS
#define FR_PROFILE_HASH_MAX_RECORD_VALUE_FIELDS                               \
  FR_PROFILE_MAX_RECORD_VALUE_FIELDS
#endif

#ifndef FR_PROFILE_HASH_PAD_BYTES
#define FR_PROFILE_HASH_PAD_BYTES FR_PROFILE_PAD_BYTES
#endif

#ifndef FR_PROFILE_HASH_OVERLAY_UPDATE_VERSION
#define FR_PROFILE_HASH_OVERLAY_UPDATE_VERSION FR_PROFILE_OVERLAY_UPDATE_VERSION
#endif

#ifndef FR_PROFILE_HASH_BASE_IMAGE_INCLUDE_SYMBOLS
#define FR_PROFILE_HASH_BASE_IMAGE_INCLUDE_SYMBOLS                            \
  FR_BASE_IMAGE_INCLUDE_SYMBOLS
#endif

#ifndef FR_PROFILE_HASH_FEATURE_COMPILER
#define FR_PROFILE_HASH_FEATURE_COMPILER FR_FEATURE_COMPILER
#endif

#ifndef FR_PROFILE_HASH_FEATURE_OVERLAY_APPLY_COMMAND
#define FR_PROFILE_HASH_FEATURE_OVERLAY_APPLY_COMMAND                         \
  FR_FEATURE_OVERLAY_APPLY_COMMAND
#endif

#ifndef FR_PROFILE_HASH_FEATURE_NUMERIC_SLOT_CALLS
#define FR_PROFILE_HASH_FEATURE_NUMERIC_SLOT_CALLS                            \
  FR_FEATURE_NUMERIC_SLOT_CALLS
#endif

#ifndef FR_PROFILE_HASH_FEATURE_NATIVE_SIGNATURES
#define FR_PROFILE_HASH_FEATURE_NATIVE_SIGNATURES FR_FEATURE_NATIVE_SIGNATURES
#endif

#ifndef FR_PROFILE_HASH_FEATURE_CELLS
#define FR_PROFILE_HASH_FEATURE_CELLS FR_FEATURE_CELLS
#endif

#ifndef FR_PROFILE_HASH_FEATURE_TEXT
#define FR_PROFILE_HASH_FEATURE_TEXT FR_FEATURE_TEXT
#endif

#ifndef FR_PROFILE_HASH_FEATURE_RECORDS
#define FR_PROFILE_HASH_FEATURE_RECORDS FR_FEATURE_RECORDS
#endif

#ifndef FR_PROFILE_HASH_FEATURE_HANDLES
#define FR_PROFILE_HASH_FEATURE_HANDLES FR_FEATURE_HANDLES
#endif

#ifndef FR_PROFILE_HASH_FEATURE_PAD
#define FR_PROFILE_HASH_FEATURE_PAD FR_FEATURE_PAD
#endif

#ifndef FR_PROFILE_HASH_FEATURE_BLE
#define FR_PROFILE_HASH_FEATURE_BLE FR_FEATURE_BLE
#endif

#ifndef FR_PROFILE_HASH_BLE_ENABLE_OBSERVER
#define FR_PROFILE_HASH_BLE_ENABLE_OBSERVER FR_BLE_ENABLE_OBSERVER
#endif

#ifndef FR_PROFILE_HASH_BLE_ENABLE_BROADCASTER
#define FR_PROFILE_HASH_BLE_ENABLE_BROADCASTER FR_BLE_ENABLE_BROADCASTER
#endif

#ifndef FR_PROFILE_HASH_BLE_ENABLE_CENTRAL
#define FR_PROFILE_HASH_BLE_ENABLE_CENTRAL FR_BLE_ENABLE_CENTRAL
#endif

#ifndef FR_PROFILE_HASH_BLE_ENABLE_PERIPHERAL
#define FR_PROFILE_HASH_BLE_ENABLE_PERIPHERAL FR_BLE_ENABLE_PERIPHERAL
#endif

#ifndef FR_PROFILE_HASH_BLE_SCAN_QUEUE_COUNT
#define FR_PROFILE_HASH_BLE_SCAN_QUEUE_COUNT FR_BLE_SCAN_QUEUE_COUNT
#endif

#ifndef FR_PROFILE_HASH_BLE_SCAN_DATA_BYTES
#define FR_PROFILE_HASH_BLE_SCAN_DATA_BYTES FR_BLE_SCAN_DATA_BYTES
#endif

#ifndef FR_PROFILE_HASH_BLE_START_TIMEOUT_MS
#define FR_PROFILE_HASH_BLE_START_TIMEOUT_MS FR_BLE_START_TIMEOUT_MS
#endif

#ifndef FR_PROFILE_HASH_BLE_STOP_TIMEOUT_MS
#define FR_PROFILE_HASH_BLE_STOP_TIMEOUT_MS FR_BLE_STOP_TIMEOUT_MS
#endif

static void fr_profile_write_u16(uint8_t *bytes, uint16_t value) {
  bytes[0] = (uint8_t)(value & 0xffu);
  bytes[1] = (uint8_t)(value >> 8);
}

static void fr_profile_hash_u16(uint32_t *crc, uint16_t value) {
  uint8_t bytes[2];

  fr_profile_write_u16(bytes, value);
  *crc = fr_crc32_update(*crc, bytes, (uint16_t)sizeof(bytes));
}

static void fr_profile_hash_u32(uint32_t *crc, uint32_t value) {
  uint8_t bytes[4];

  fr_write_u32_le(bytes, value);
  *crc = fr_crc32_update(*crc, bytes, (uint16_t)sizeof(bytes));
}

static void fr_profile_hash_tagged(uint32_t *crc, fr_tagged_t tagged) {
  fr_profile_hash_u32(crc, tagged);
}

#if FR_PROFILE_HASH_FEATURE_NATIVE_SIGNATURES
#if !FR_FEATURE_NATIVE_SIGNATURES
#error "profile hash cannot include native signatures when the profile omits them"
#endif
/* Display-only fields (.params[i].name, .help) must not enter this hash.
 * If they did, every help-text edit would invalidate every saved image. */
static void
fr_profile_hash_signature(uint32_t *crc,
                          const fr_native_signature_t *signature) {
  if (signature == NULL) {
    fr_profile_hash_u16(crc, 0);
    return;
  }

  fr_profile_hash_u16(crc, (uint16_t)(signature->arg_count + 1));
  if (signature->arg_count > 0 && signature->params == NULL) {
    fr_profile_hash_u16(crc, 0xffffu);
    return;
  }
  for (uint8_t i = 0; i < signature->arg_count; i++) {
    fr_profile_hash_u16(crc, signature->params[i].type);
  }
  fr_profile_hash_u16(crc, signature->result);
}
#endif

const char *fr_profile_name(void) { return FR_PROFILE_NAME; }

const char *fr_profile_contract_name(void) { return FR_PROFILE_CONTRACT_NAME; }

static uint32_t fr_profile_hash_body(uint16_t word_size,
                                     const char *source_bytes,
                                     uint16_t source_length,
                                     const fr_lib_native_def_t *lib_natives,
                                     uint16_t lib_natives_count) {
  uint32_t crc = 0xffffffffu;
  (void)source_bytes;
  (void)source_length;

  /* Compact drift fingerprint, not a cryptographic identity. */
  fr_profile_hash_u16(&crc, word_size);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_SLOTS);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_INSTRUCTION_BYTES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_TOTAL_IMAGE_BYTES);
  fr_profile_hash_u16(&crc,
                      FR_PROFILE_HASH_MAX_DEFINITION_INSTRUCTION_BYTES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_DEFINITION_TEXT_BYTES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_DEFINITION_CODE_OBJECTS);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_OVERLAY_CODE_BYTES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_OVERLAY_TEXT_BYTES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_OVERLAY_PARAM_NAME_BYTES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_SOURCE_RENDER_BYTES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_STACK_DEPTH);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_CODE_OBJECT_TABLE_SIZE);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_NATIVE_TABLE_SIZE);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_HANDLES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_CALL_DEPTH);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_ATTEMPT_DEPTH);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_REPL_LINE_BYTES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_PERSISTENCE_BYTES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_OVERLAY_NAMES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_OVERLAY_UPDATE_SLOT_INITS);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_OVERLAY_UPDATE_CODE_OBJECTS);
  fr_profile_hash_u16(&crc,
                      FR_PROFILE_HASH_MAX_OVERLAY_UPDATE_INSTRUCTION_BYTES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_OVERLAY_UPDATE_NAMES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_OBJECT_TABLE_SIZE);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_CELL_WORDS);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_CELL_LENGTH);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_TEXT_BYTES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_TEXT_LENGTH);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_RECORD_NAME_BYTES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_RECORD_FIELDS_PER_SHAPE);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_RECORD_SHAPE_FIELDS);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_RECORD_VALUE_FIELDS);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_PAD_BYTES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_OVERLAY_UPDATE_VERSION);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_BASE_IMAGE_INCLUDE_SYMBOLS);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_FEATURE_COMPILER);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_FEATURE_OVERLAY_APPLY_COMMAND);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_FEATURE_NUMERIC_SLOT_CALLS);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_FEATURE_NATIVE_SIGNATURES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_FEATURE_CELLS);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_FEATURE_TEXT);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_FEATURE_RECORDS);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_FEATURE_HANDLES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_FEATURE_PAD);

  /* BLE-disabled profiles keep their pre-BLE persisted-image identity. */
#if FR_PROFILE_HASH_FEATURE_BLE
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_FEATURE_BLE);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_BLE_ENABLE_OBSERVER);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_BLE_ENABLE_BROADCASTER);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_BLE_ENABLE_CENTRAL);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_BLE_ENABLE_PERIPHERAL);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_BLE_SCAN_QUEUE_COUNT);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_BLE_SCAN_DATA_BYTES);
  fr_profile_hash_u32(&crc, FR_PROFILE_HASH_BLE_START_TIMEOUT_MS);
  fr_profile_hash_u32(&crc, FR_PROFILE_HASH_BLE_STOP_TIMEOUT_MS);
#endif

  fr_profile_hash_u16(&crc, fr_base_def_count());
  for (uint16_t i = 0; i < fr_base_def_count(); i++) {
    const fr_base_def_t *def = NULL;
    fr_base_layer_t layer = FR_BASE_LAYER_CORE;

    if (fr_base_def_at(i, &def, &layer) != FR_OK || def == NULL) {
      fr_profile_hash_u16(&crc, 0xffffu);
      continue;
    }

    fr_profile_hash_u16(&crc, layer);
    fr_profile_hash_u16(&crc, def->slot_id);
    fr_profile_hash_u16(&crc, def->kind);
    fr_profile_hash_tagged(&crc, def->literal_tagged);
    fr_profile_hash_u16(&crc, def->native_arity);
#if FR_PROFILE_HASH_FEATURE_NATIVE_SIGNATURES
    fr_profile_hash_signature(&crc, def->native_signature);
#endif
  }

#if FR_FEATURE_SOURCE_BASE
  fr_profile_hash_u16(&crc, source_length);
  for (uint16_t i = 0; i < source_length; i++) {
    fr_profile_hash_u16(&crc, (uint16_t)(uint8_t)source_bytes[i]);
  }
#endif

  fr_profile_hash_u16(&crc, lib_natives_count);
  for (uint16_t i = 0; i < lib_natives_count; i++) {
    const fr_lib_native_def_t *def = NULL;
    const char *name = NULL;

    if (lib_natives == NULL) {
      fr_profile_hash_u16(&crc, 0xffffu);
      continue;
    }

    def = &lib_natives[i];
    name = def->name;
    if (name == NULL) {
      fr_profile_hash_u16(&crc, 0xffffu);
    } else {
      while (*name != '\0') {
        fr_profile_hash_u16(&crc, (uint16_t)(uint8_t)*name);
        name++;
      }
      fr_profile_hash_u16(&crc, 0);
    }
    fr_profile_hash_u16(&crc, def->arity);
  }

  return ~crc;
}

uint32_t fr_profile_debug_hash_for_word_size(uint16_t word_size) {
#if FR_FEATURE_SOURCE_BASE
  return fr_profile_hash_body(word_size, fr_source_base_bytes,
                              fr_source_base_bytes_len, fr_lib_natives,
                              fr_lib_natives_count);
#else
  return fr_profile_hash_body(word_size, NULL, 0, fr_lib_natives,
                              fr_lib_natives_count);
#endif
}

#if FR_FEATURE_SOURCE_BASE
uint32_t fr_profile_debug_hash_for_source(uint16_t word_size, const char *bytes,
                                          uint16_t length) {
  return fr_profile_hash_body(word_size, bytes, length, fr_lib_natives,
                              fr_lib_natives_count);
}
#endif

uint32_t
fr_profile_debug_hash_for_lib_natives(const fr_lib_native_def_t *defs,
                                      uint16_t count) {
#if FR_FEATURE_SOURCE_BASE
  return fr_profile_hash_body(FR_PROFILE_HASH_WORD_SIZE, fr_source_base_bytes,
                              fr_source_base_bytes_len, defs, count);
#else
  return fr_profile_hash_body(FR_PROFILE_HASH_WORD_SIZE, NULL, 0, defs, count);
#endif
}

uint32_t fr_profile_hash(void) {
  return fr_profile_debug_hash_for_word_size(FR_PROFILE_HASH_WORD_SIZE);
}

const char *fr_profile_compiler_mode(void) {
#if FR_FEATURE_COMPILER
  return "device";
#elif FR_FEATURE_OVERLAY_APPLY_COMMAND
  return "host-required";
#else
  return "unknown";
#endif
}

const char *fr_profile_names_mode(void) {
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS || FR_PROFILE_MAX_OVERLAY_NAMES > 0
  return "device";
#elif FR_FEATURE_NUMERIC_SLOT_CALLS
  return "host";
#else
  return "none";
#endif
}

const char *fr_profile_storage_mode(void) {
#if FR_FEATURE_PERSISTENCE
  return "eeprom";
#else
  return "volatile";
#endif
}

const char *fr_profile_interrupt_mode(void) { return "cooperative"; }
