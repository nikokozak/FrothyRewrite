#include "profile.h"

#include "base_defs.h"
#include "config.h"
#include "crc.h"
#include "tagged.h"

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

static void fr_profile_write_u16(uint8_t *bytes, uint16_t value) {
  bytes[0] = (uint8_t)(value & 0xffu);
  bytes[1] = (uint8_t)(value >> 8);
}

static void fr_profile_hash_u16(uint32_t *crc, uint16_t value) {
  uint8_t bytes[2];

  fr_profile_write_u16(bytes, value);
  *crc = fr_crc32_update(*crc, bytes, (uint16_t)sizeof(bytes));
}

#if FR_WORD_SIZE == 32
static void fr_profile_hash_u32(uint32_t *crc, uint32_t value) {
  uint8_t bytes[4];

  fr_write_u32_le(bytes, value);
  *crc = fr_crc32_update(*crc, bytes, (uint16_t)sizeof(bytes));
}
#endif

static void fr_profile_hash_tagged(uint32_t *crc, fr_tagged_t tagged) {
#if FR_WORD_SIZE == 16
  fr_profile_hash_u16(crc, tagged);
#else
  fr_profile_hash_u32(crc, tagged);
#endif
}

#if FR_PROFILE_HASH_FEATURE_NATIVE_SIGNATURES
#if !FR_FEATURE_NATIVE_SIGNATURES
#error "profile hash cannot include native signatures when the profile omits them"
#endif
static void
fr_profile_hash_signature(uint32_t *crc,
                          const fr_native_signature_t *signature) {
  if (signature == NULL) {
    fr_profile_hash_u16(crc, 0);
    return;
  }

  fr_profile_hash_u16(crc, (uint16_t)(signature->arg_count + 1));
  if (signature->arg_count > 0 && signature->args == NULL) {
    fr_profile_hash_u16(crc, 0xffffu);
    return;
  }
  for (uint8_t i = 0; i < signature->arg_count; i++) {
    fr_profile_hash_u16(crc, signature->args[i]);
  }
  fr_profile_hash_u16(crc, signature->result);
}
#endif

const char *fr_profile_name(void) { return FR_PROFILE_NAME; }

const char *fr_profile_contract_name(void) { return FR_PROFILE_CONTRACT_NAME; }

uint32_t fr_profile_debug_hash_for_word_size(uint16_t word_size) {
  uint32_t crc = 0xffffffffu;

  /* Compact drift fingerprint, not a cryptographic identity. */
  fr_profile_hash_u16(&crc, word_size);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_SLOTS);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_INSTRUCTION_BYTES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_STACK_DEPTH);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_CODE_OBJECT_TABLE_SIZE);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_NATIVE_TABLE_SIZE);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_HANDLES);
  fr_profile_hash_u16(&crc, FR_PROFILE_HASH_MAX_CALL_DEPTH);
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

  return ~crc;
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
