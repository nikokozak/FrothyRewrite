#pragma once

#ifndef FR_PROFILE_HEADER
#define FR_PROFILE_HEADER "host_small.h"
#endif

#include FR_PROFILE_HEADER

/*
 * Every profile header must declare FR_WORD_SIZE. A silent default would let a
 * new profile pick up the wrong tagged-word contract without the author
 * noticing.
 */
#ifndef FR_WORD_SIZE
#error "profile header must define FR_WORD_SIZE"
#endif

#if FR_WORD_SIZE != 32
#error "Frothy is 32-bit only; FR_WORD_SIZE must be 32"
#endif

#ifndef FR_BASE_IMAGE_INCLUDE_SYMBOLS
#define FR_BASE_IMAGE_INCLUDE_SYMBOLS 0
#endif

#ifndef FR_FEATURE_REPL
#define FR_FEATURE_REPL 1
#endif

#ifndef FR_FEATURE_COMPILER
#define FR_FEATURE_COMPILER 1
#endif

#ifndef FR_FEATURE_PERSISTENCE
#define FR_FEATURE_PERSISTENCE 1
#endif

#ifndef FR_FEATURE_INTROSPECTION
#define FR_FEATURE_INTROSPECTION FR_BASE_IMAGE_INCLUDE_SYMBOLS
#endif

#ifndef FR_FEATURE_OVERLAY_APPLY_COMMAND
#define FR_FEATURE_OVERLAY_APPLY_COMMAND 0
#endif

#ifndef FR_FEATURE_NUMERIC_SLOT_CALLS
#define FR_FEATURE_NUMERIC_SLOT_CALLS 0
#endif

#ifndef FR_FEATURE_NATIVE_SIGNATURES
#define FR_FEATURE_NATIVE_SIGNATURES 1
#endif

#ifndef FR_FEATURE_SOURCE_CONTROL_FLOW
#define FR_FEATURE_SOURCE_CONTROL_FLOW FR_FEATURE_COMPILER
#endif

#ifndef FR_FEATURE_CELLS
#define FR_FEATURE_CELLS 0
#endif

#ifndef FR_FEATURE_TEXT
#define FR_FEATURE_TEXT 0
#endif

#ifndef FR_FEATURE_RECORDS
#define FR_FEATURE_RECORDS 0
#endif

#ifndef FR_FEATURE_HANDLES
#define FR_FEATURE_HANDLES 0
#endif

#ifndef FR_FEATURE_UART
#define FR_FEATURE_UART 0
#endif

#ifndef FR_FEATURE_CONSOLE_ROUTING
#define FR_FEATURE_CONSOLE_ROUTING 0
#endif

#ifndef FR_FEATURE_RANDOM
#define FR_FEATURE_RANDOM 1
#endif

#ifndef FR_FEATURE_PWM
#define FR_FEATURE_PWM 1
#endif

#ifndef FR_FEATURE_I2C
#define FR_FEATURE_I2C 1
#endif

#ifndef FR_FEATURE_TRACE
#define FR_FEATURE_TRACE 0
#endif

#ifndef FR_FEATURE_PULSE
#define FR_FEATURE_PULSE 0
#endif

#ifndef FR_FEATURE_MATH
#define FR_FEATURE_MATH 1
#endif

#ifndef FR_FEATURE_SOURCE_BASE
#define FR_FEATURE_SOURCE_BASE 1
#endif

#ifndef FR_FEATURE_PAD
#define FR_FEATURE_PAD 0
#endif

#ifndef FR_FEATURE_EVENTS
#define FR_FEATURE_EVENTS 1
#endif

#ifndef FR_FEATURE_BYTES
#define FR_FEATURE_BYTES 0
#endif

#ifndef FR_FEATURE_BLE
#define FR_FEATURE_BLE 0
#endif

#ifndef FR_BLE_ENABLE_OBSERVER
#define FR_BLE_ENABLE_OBSERVER 0
#endif

#ifndef FR_BLE_ENABLE_BROADCASTER
#define FR_BLE_ENABLE_BROADCASTER 0
#endif

#ifndef FR_BLE_ENABLE_CENTRAL
#define FR_BLE_ENABLE_CENTRAL 0
#endif

#ifndef FR_BLE_ENABLE_PERIPHERAL
#define FR_BLE_ENABLE_PERIPHERAL 0
#endif

#ifndef FR_BLE_SCAN_QUEUE_COUNT
#define FR_BLE_SCAN_QUEUE_COUNT 0
#endif

#ifndef FR_BLE_SCAN_DATA_BYTES
#define FR_BLE_SCAN_DATA_BYTES 0
#endif

#ifndef FR_BLE_START_TIMEOUT_MS
#define FR_BLE_START_TIMEOUT_MS 0
#endif

#ifndef FR_BLE_STOP_TIMEOUT_MS
#define FR_BLE_STOP_TIMEOUT_MS 0
#endif

#define FR_FEATURE_OBJECTS                                                   \
  (FR_FEATURE_CELLS || FR_FEATURE_TEXT || FR_FEATURE_RECORDS)

#ifndef FR_PROFILE_OBJECT_TABLE_SIZE
#define FR_PROFILE_OBJECT_TABLE_SIZE 0
#endif

#ifndef FR_PROFILE_MAX_CELL_WORDS
#define FR_PROFILE_MAX_CELL_WORDS 0
#endif

#ifndef FR_PROFILE_MAX_CELL_LENGTH
#define FR_PROFILE_MAX_CELL_LENGTH 0
#endif

#ifndef FR_PROFILE_MAX_TEXT_BYTES
#define FR_PROFILE_MAX_TEXT_BYTES 0
#endif

#ifndef FR_PROFILE_MAX_TEXT_LENGTH
#define FR_PROFILE_MAX_TEXT_LENGTH 0
#endif

#ifndef FR_PROFILE_MAX_RECORD_NAME_BYTES
#define FR_PROFILE_MAX_RECORD_NAME_BYTES 0
#endif

#ifndef FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE
#define FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE 0
#endif

#ifndef FR_PROFILE_MAX_RECORD_SHAPE_FIELDS
#define FR_PROFILE_MAX_RECORD_SHAPE_FIELDS 0
#endif

#ifndef FR_PROFILE_MAX_RECORD_VALUE_FIELDS
#define FR_PROFILE_MAX_RECORD_VALUE_FIELDS 0
#endif

#ifndef FR_PROFILE_MAX_HANDLES
#define FR_PROFILE_MAX_HANDLES 0
#endif

#ifndef FR_PROFILE_PAD_BYTES
#define FR_PROFILE_PAD_BYTES 0
#endif

#ifndef FR_PROFILE_MAX_NAME_BYTES
#define FR_PROFILE_MAX_NAME_BYTES 32
#endif

#ifndef FR_PROFILE_PARSE_MAX_TOKEN_BYTES
#define FR_PROFILE_PARSE_MAX_TOKEN_BYTES 32
#endif

#ifndef FR_PROFILE_PARSE_MAX_EXPR_DEPTH
#define FR_PROFILE_PARSE_MAX_EXPR_DEPTH 8
#endif

#ifndef FR_PROFILE_PARSE_MAX_EXPR_NODES
#define FR_PROFILE_PARSE_MAX_EXPR_NODES 32
#endif

#ifndef FR_PROFILE_PARSE_MAX_BODY_EXPRS
#define FR_PROFILE_PARSE_MAX_BODY_EXPRS 8
#endif

#ifndef FR_PROFILE_PARSE_MAX_PARAMS
#define FR_PROFILE_PARSE_MAX_PARAMS 4
#endif

#ifndef FR_PROFILE_PARSE_MAX_LOCALS
#define FR_PROFILE_PARSE_MAX_LOCALS 4
#endif

/* Optional on-device names for live project overlay slots. */
#ifndef FR_PROFILE_MAX_OVERLAY_NAMES
#define FR_PROFILE_MAX_OVERLAY_NAMES 0
#endif

#ifndef FR_PROFILE_TOTAL_IMAGE_BYTES
#ifdef FR_PROFILE_MAX_INSTRUCTION_BYTES
#define FR_PROFILE_TOTAL_IMAGE_BYTES FR_PROFILE_MAX_INSTRUCTION_BYTES
#else
#define FR_PROFILE_TOTAL_IMAGE_BYTES 128
#endif
#endif

#ifndef FR_PROFILE_MAX_INSTRUCTION_BYTES
/* Transitional spelling: retire after the flash-image storage split. */
#define FR_PROFILE_MAX_INSTRUCTION_BYTES FR_PROFILE_TOTAL_IMAGE_BYTES
#endif

#ifndef FR_PROFILE_MAX_DEFINITION_INSTRUCTION_BYTES
#define FR_PROFILE_MAX_DEFINITION_INSTRUCTION_BYTES FR_PROFILE_TOTAL_IMAGE_BYTES
#endif

#ifndef FR_PROFILE_MAX_DEFINITION_TEXT_BYTES
#define FR_PROFILE_MAX_DEFINITION_TEXT_BYTES                                  \
  (FR_PROFILE_MAX_TEXT_LENGTH > 0 ? FR_PROFILE_MAX_TEXT_LENGTH : 1)
#endif

#ifndef FR_PROFILE_MAX_DEFINITION_CODE_OBJECTS
#define FR_PROFILE_MAX_DEFINITION_CODE_OBJECTS 2
#endif

#ifndef FR_PROFILE_MAX_OVERLAY_CODE_BYTES
#define FR_PROFILE_MAX_OVERLAY_CODE_BYTES                                     \
  FR_PROFILE_MAX_DEFINITION_INSTRUCTION_BYTES
#endif

#ifndef FR_PROFILE_MAX_OVERLAY_TEXT_BYTES
#define FR_PROFILE_MAX_OVERLAY_TEXT_BYTES FR_PROFILE_MAX_DEFINITION_TEXT_BYTES
#endif

#ifndef FR_PROFILE_MAX_OVERLAY_PARAM_NAME_BYTES
#define FR_PROFILE_MAX_OVERLAY_PARAM_NAME_BYTES                               \
  (FR_PROFILE_PARSE_MAX_PARAMS * (FR_PROFILE_PARSE_MAX_TOKEN_BYTES + 1))
#endif

#ifndef FR_PROFILE_MAX_SOURCE_RENDER_BYTES
#define FR_PROFILE_MAX_SOURCE_RENDER_BYTES                                    \
  (FR_PROFILE_MAX_DEFINITION_INSTRUCTION_BYTES * 4u)
#endif

#ifndef FR_PROFILE_MAX_OVERLAY_UPDATE_SLOT_INITS
#define FR_PROFILE_MAX_OVERLAY_UPDATE_SLOT_INITS FR_PROFILE_MAX_SLOTS
#endif

#ifndef FR_PROFILE_MAX_OVERLAY_UPDATE_CODE_OBJECTS
#define FR_PROFILE_MAX_OVERLAY_UPDATE_CODE_OBJECTS                           \
  FR_PROFILE_CODE_OBJECT_TABLE_SIZE
#endif

#ifndef FR_PROFILE_MAX_OVERLAY_UPDATE_INSTRUCTION_BYTES
#define FR_PROFILE_MAX_OVERLAY_UPDATE_INSTRUCTION_BYTES                       \
  FR_PROFILE_MAX_OVERLAY_CODE_BYTES
#endif

#ifndef FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES
#define FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES FR_PROFILE_MAX_OVERLAY_NAMES
#endif

#ifndef FR_PROFILE_OVERLAY_UPDATE_VERSION
/*
 * Overlay update version tracks the serial apply/run payload format and is
 * reserved for format evolution. Width identity lives in the profile hash
 * (see FR_PROFILE_HASH_WORD_SIZE in profile.c), so this counter must not be
 * derived from FR_WORD_SIZE -- cross-width rejection is the hash's job.
 * Separate from the persistence payload version in persist_payload.c; bump
 * each only when its own wire format changes.
 *
 * v2 added a text-object record so function bodies can carry text literals.
 * v3 added the event-binding record. Frothy is pre-release: readers accept the
 * current version only, and any older payload returns the version-mismatch
 * error.
 */
#define FR_PROFILE_OVERLAY_UPDATE_VERSION 3
#endif

/* Separate from FR_PROFILE_PARSE_MAX_BODY_EXPRS: this is the number of
 * max-length text literals one overlay compile/decode scratch can carry. */
#ifndef FR_PROFILE_MAX_OVERLAY_UPDATE_TEXT_OBJECTS
#define FR_PROFILE_MAX_OVERLAY_UPDATE_TEXT_OBJECTS 8
#endif

#ifndef FR_PROFILE_MAX_OVERLAY_UPDATE_TEXT_BYTES
#define FR_PROFILE_MAX_OVERLAY_UPDATE_TEXT_BYTES FR_PROFILE_MAX_OVERLAY_TEXT_BYTES
#endif

/* Mirrors FR_EVENT_BINDING_COUNT in runtime.h; the overlay decoder caps event
   records at the runtime table size. config.h cannot include runtime.h, so the
   value is repeated here and pinned by a static check in image.c. */
#ifndef FR_PROFILE_MAX_OVERLAY_UPDATE_EVENT_BINDINGS
#if FR_FEATURE_EVENTS
#define FR_PROFILE_MAX_OVERLAY_UPDATE_EVENT_BINDINGS 16
#else
#define FR_PROFILE_MAX_OVERLAY_UPDATE_EVENT_BINDINGS 0
#endif
#endif

#ifndef FR_PROFILE_MAX_ATTEMPT_DEPTH
#define FR_PROFILE_MAX_ATTEMPT_DEPTH 4
#endif

#ifndef FR_PROFILE_REPL_LINE_BYTES
#define FR_PROFILE_REPL_LINE_BYTES 128
#endif

/* Shared cache for code-object param names, NUL-separated. Sized off the code
 * table because most words have zero or one param; the renderer reads it. When
 * the cache fills, install drops names for the overflowing object (the renderer
 * falls back), so this is a budget, not a hard limit on legal source. */
#ifndef FR_PROFILE_MAX_PARAM_NAME_BYTES
#define FR_PROFILE_MAX_PARAM_NAME_BYTES (FR_PROFILE_CODE_OBJECT_TABLE_SIZE * 8)
#endif

/* Scratch the source renderer assembles a body into before writing it. Each
 * reduction copies its child fragments forward, so size off the byte budget;
 * on overflow the renderer falls back to the bytecode listing. */
#ifndef FR_PROFILE_SOURCE_RENDER_BYTES
#define FR_PROFILE_SOURCE_RENDER_BYTES FR_PROFILE_MAX_SOURCE_RENDER_BYTES
#endif

#if FR_PROFILE_MAX_NAME_BYTES == 0
#error "FR_PROFILE_MAX_NAME_BYTES must be greater than zero"
#endif

#if FR_PROFILE_PARSE_MAX_TOKEN_BYTES == 0
#error "FR_PROFILE_PARSE_MAX_TOKEN_BYTES must be greater than zero"
#endif

#if FR_PROFILE_PARSE_MAX_EXPR_DEPTH == 0 ||                                   \
    FR_PROFILE_PARSE_MAX_EXPR_DEPTH > 64
#error "FR_PROFILE_PARSE_MAX_EXPR_DEPTH must be between 1 and 64"
#endif

#if FR_PROFILE_PARSE_MAX_EXPR_NODES == 0 ||                                   \
    FR_PROFILE_PARSE_MAX_EXPR_NODES > 255
#error "FR_PROFILE_PARSE_MAX_EXPR_NODES must fit the uint8_t expression id"
#endif

#if FR_PROFILE_PARSE_MAX_BODY_EXPRS == 0 ||                                    \
    FR_PROFILE_PARSE_MAX_BODY_EXPRS > 255
#error "FR_PROFILE_PARSE_MAX_BODY_EXPRS must fit the uint8_t child count"
#endif

#if FR_PROFILE_PARSE_MAX_PARAMS > 255
#error "FR_PROFILE_PARSE_MAX_PARAMS must fit the one-byte arity field"
#endif

#if FR_PROFILE_PARSE_MAX_LOCALS > 255
#error "FR_PROFILE_PARSE_MAX_LOCALS must fit the one-byte local field"
#endif

#if FR_PROFILE_PARSE_MAX_PARAMS > FR_PROFILE_MAX_STACK_DEPTH
#error "FR_PROFILE_PARSE_MAX_PARAMS cannot exceed FR_PROFILE_MAX_STACK_DEPTH"
#endif

#if FR_PROFILE_PARSE_MAX_LOCALS > FR_PROFILE_MAX_STACK_DEPTH
#error "FR_PROFILE_PARSE_MAX_LOCALS cannot exceed FR_PROFILE_MAX_STACK_DEPTH"
#endif

#if FR_PROFILE_PARSE_MAX_PARAMS + FR_PROFILE_PARSE_MAX_LOCALS >                \
    FR_PROFILE_MAX_STACK_DEPTH
#error "FR_PROFILE_PARSE_MAX_PARAMS plus FR_PROFILE_PARSE_MAX_LOCALS cannot exceed FR_PROFILE_MAX_STACK_DEPTH"
#endif

#if FR_PROFILE_MAX_ATTEMPT_DEPTH == 0 || FR_PROFILE_MAX_ATTEMPT_DEPTH > 255
#error "FR_PROFILE_MAX_ATTEMPT_DEPTH must be between 1 and 255"
#endif

#if FR_PROFILE_REPL_LINE_BYTES == 0
#error "FR_PROFILE_REPL_LINE_BYTES must be greater than zero"
#endif

#if FR_FEATURE_OVERLAY_APPLY_COMMAND && FR_PROFILE_REPL_LINE_BYTES < 9
#error "FR_PROFILE_REPL_LINE_BYTES must fit 'apply ', one byte of hex, and a terminator"
#endif

#if FR_PROFILE_MAX_OVERLAY_NAMES > FR_PROFILE_MAX_SLOTS
#error "FR_PROFILE_MAX_OVERLAY_NAMES cannot exceed FR_PROFILE_MAX_SLOTS"
#endif

#if FR_PROFILE_MAX_DEFINITION_INSTRUCTION_BYTES >                             \
    FR_PROFILE_MAX_OVERLAY_CODE_BYTES
#error "FR_PROFILE_MAX_DEFINITION_INSTRUCTION_BYTES cannot exceed FR_PROFILE_MAX_OVERLAY_CODE_BYTES"
#endif

#if FR_PROFILE_MAX_OVERLAY_CODE_BYTES > FR_PROFILE_TOTAL_IMAGE_BYTES
#error "FR_PROFILE_MAX_OVERLAY_CODE_BYTES cannot exceed FR_PROFILE_TOTAL_IMAGE_BYTES"
#endif

#if FR_PROFILE_MAX_DEFINITION_TEXT_BYTES == 0
#error "FR_PROFILE_MAX_DEFINITION_TEXT_BYTES must be greater than zero"
#endif

#if FR_PROFILE_MAX_DEFINITION_CODE_OBJECTS < 2
#error "FR_PROFILE_MAX_DEFINITION_CODE_OBJECTS must fit outer and event body code objects"
#endif

#if FR_PROFILE_MAX_OVERLAY_PARAM_NAME_BYTES == 0
#error "FR_PROFILE_MAX_OVERLAY_PARAM_NAME_BYTES must be greater than zero"
#endif

#if FR_PROFILE_MAX_OVERLAY_UPDATE_SLOT_INITS > FR_PROFILE_MAX_SLOTS
#error "FR_PROFILE_MAX_OVERLAY_UPDATE_SLOT_INITS cannot exceed FR_PROFILE_MAX_SLOTS"
#endif

#if FR_PROFILE_MAX_OVERLAY_UPDATE_CODE_OBJECTS >                              \
    FR_PROFILE_CODE_OBJECT_TABLE_SIZE
#error "FR_PROFILE_MAX_OVERLAY_UPDATE_CODE_OBJECTS cannot exceed FR_PROFILE_CODE_OBJECT_TABLE_SIZE"
#endif

#if FR_PROFILE_MAX_OVERLAY_UPDATE_INSTRUCTION_BYTES >                         \
    FR_PROFILE_MAX_OVERLAY_CODE_BYTES
#error "FR_PROFILE_MAX_OVERLAY_UPDATE_INSTRUCTION_BYTES cannot exceed FR_PROFILE_MAX_OVERLAY_CODE_BYTES"
#endif

#if FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES > FR_PROFILE_MAX_OVERLAY_NAMES
#error "FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES cannot exceed FR_PROFILE_MAX_OVERLAY_NAMES"
#endif

#if FR_FEATURE_OBJECTS && FR_PROFILE_OBJECT_TABLE_SIZE == 0
#error "object features require FR_PROFILE_OBJECT_TABLE_SIZE"
#endif

#if !FR_FEATURE_OBJECTS && FR_PROFILE_OBJECT_TABLE_SIZE > 0
#error "FR_PROFILE_OBJECT_TABLE_SIZE requires an object feature"
#endif

#if FR_FEATURE_CELLS && FR_PROFILE_MAX_CELL_WORDS == 0
#error "FR_FEATURE_CELLS requires FR_PROFILE_MAX_CELL_WORDS"
#endif

#if FR_FEATURE_CELLS && FR_PROFILE_MAX_CELL_LENGTH == 0
#error "FR_FEATURE_CELLS requires FR_PROFILE_MAX_CELL_LENGTH"
#endif

#if !FR_FEATURE_CELLS &&                                                     \
    (FR_PROFILE_MAX_CELL_WORDS > 0 || FR_PROFILE_MAX_CELL_LENGTH > 0)
#error "cell profile limits require FR_FEATURE_CELLS"
#endif

#if FR_PROFILE_MAX_CELL_LENGTH > FR_PROFILE_MAX_CELL_WORDS
#error "FR_PROFILE_MAX_CELL_LENGTH cannot exceed FR_PROFILE_MAX_CELL_WORDS"
#endif

#if FR_FEATURE_TEXT && FR_PROFILE_MAX_TEXT_BYTES == 0
#error "FR_FEATURE_TEXT requires FR_PROFILE_MAX_TEXT_BYTES"
#endif

#if FR_FEATURE_TEXT && FR_PROFILE_MAX_TEXT_LENGTH == 0
#error "FR_FEATURE_TEXT requires FR_PROFILE_MAX_TEXT_LENGTH"
#endif

#if !FR_FEATURE_TEXT &&                                                      \
    (FR_PROFILE_MAX_TEXT_BYTES > 0 || FR_PROFILE_MAX_TEXT_LENGTH > 0)
#error "text profile limits require FR_FEATURE_TEXT"
#endif

#if FR_PROFILE_MAX_TEXT_LENGTH > FR_PROFILE_MAX_TEXT_BYTES
#error "FR_PROFILE_MAX_TEXT_LENGTH cannot exceed FR_PROFILE_MAX_TEXT_BYTES"
#endif

#if FR_FEATURE_RECORDS && !FR_FEATURE_TEXT
#error "FR_FEATURE_RECORDS requires FR_FEATURE_TEXT in this tranche"
#endif

#if FR_FEATURE_RECORDS && FR_PROFILE_MAX_RECORD_NAME_BYTES == 0
#error "FR_FEATURE_RECORDS requires FR_PROFILE_MAX_RECORD_NAME_BYTES"
#endif

#if FR_FEATURE_RECORDS && FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE == 0
#error "FR_FEATURE_RECORDS requires FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE"
#endif

#if FR_FEATURE_RECORDS && FR_PROFILE_MAX_RECORD_SHAPE_FIELDS == 0
#error "FR_FEATURE_RECORDS requires FR_PROFILE_MAX_RECORD_SHAPE_FIELDS"
#endif

#if FR_FEATURE_RECORDS && FR_PROFILE_MAX_RECORD_VALUE_FIELDS == 0
#error "FR_FEATURE_RECORDS requires FR_PROFILE_MAX_RECORD_VALUE_FIELDS"
#endif

#if !FR_FEATURE_RECORDS &&                                                   \
    (FR_PROFILE_MAX_RECORD_NAME_BYTES > 0 ||                                  \
     FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE > 0 ||                            \
     FR_PROFILE_MAX_RECORD_SHAPE_FIELDS > 0 ||                                \
     FR_PROFILE_MAX_RECORD_VALUE_FIELDS > 0)
#error "record profile limits require FR_FEATURE_RECORDS"
#endif

#if FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE >                                  \
    FR_PROFILE_MAX_RECORD_SHAPE_FIELDS
#error "FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE cannot exceed FR_PROFILE_MAX_RECORD_SHAPE_FIELDS"
#endif

#if FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE >                                  \
    FR_PROFILE_MAX_RECORD_VALUE_FIELDS
#error "FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE cannot exceed FR_PROFILE_MAX_RECORD_VALUE_FIELDS"
#endif

#if FR_FEATURE_HANDLES && FR_PROFILE_MAX_HANDLES == 0
#error "FR_FEATURE_HANDLES requires FR_PROFILE_MAX_HANDLES"
#endif

#if FR_PROFILE_MAX_HANDLES > 16
#error "FR_PROFILE_MAX_HANDLES cannot exceed the 16-entry handle tag band"
#endif

#if !FR_FEATURE_HANDLES && FR_PROFILE_MAX_HANDLES > 0
#error "FR_PROFILE_MAX_HANDLES requires FR_FEATURE_HANDLES"
#endif

#if FR_FEATURE_UART && !FR_FEATURE_HANDLES
#error "FR_FEATURE_UART requires FR_FEATURE_HANDLES"
#endif

#if FR_FEATURE_UART && !FR_FEATURE_COMPILER
#error "FR_FEATURE_UART requires FR_FEATURE_COMPILER in this tranche"
#endif

#if FR_FEATURE_CONSOLE_ROUTING && !FR_FEATURE_REPL
#error "FR_FEATURE_CONSOLE_ROUTING requires FR_FEATURE_REPL"
#endif

#if FR_FEATURE_SOURCE_BASE && !FR_FEATURE_COMPILER
#error "FR_FEATURE_SOURCE_BASE requires FR_FEATURE_COMPILER"
#endif

#if FR_FEATURE_I2C && !FR_FEATURE_HANDLES
#error "FR_FEATURE_I2C requires FR_FEATURE_HANDLES"
#endif

#if FR_FEATURE_I2C && !FR_FEATURE_TEXT
#error "FR_FEATURE_I2C requires FR_FEATURE_TEXT"
#endif

#if FR_FEATURE_TRACE && !FR_FEATURE_HANDLES
#error "FR_FEATURE_TRACE requires FR_FEATURE_HANDLES"
#endif

#if FR_FEATURE_TRACE && !FR_FEATURE_REPL
#error "FR_FEATURE_TRACE requires FR_FEATURE_REPL"
#endif

#if FR_FEATURE_PULSE && !FR_FEATURE_HANDLES
#error "FR_FEATURE_PULSE requires FR_FEATURE_HANDLES"
#endif

#if FR_FEATURE_PULSE && !FR_FEATURE_REPL
#error "FR_FEATURE_PULSE requires FR_FEATURE_REPL"
#endif

#if FR_FEATURE_BLE && !FR_FEATURE_BYTES
#error "FR_FEATURE_BLE requires FR_FEATURE_BYTES"
#endif

#if FR_FEATURE_BLE && !FR_FEATURE_REPL
#error "the first BLE profile requires FR_FEATURE_REPL"
#endif

#if !FR_FEATURE_BLE &&                                                     \
    (FR_BLE_ENABLE_OBSERVER || FR_BLE_ENABLE_BROADCASTER ||                \
     FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL)
#error "BLE role gates require FR_FEATURE_BLE"
#endif

#if FR_FEATURE_BLE &&                                                       \
    !(FR_BLE_ENABLE_OBSERVER || FR_BLE_ENABLE_BROADCASTER ||                \
      FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL)
#error "FR_FEATURE_BLE requires at least one BLE role"
#endif

#if (FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL) &&                 \
    !FR_FEATURE_HANDLES
#error "connected BLE roles require FR_FEATURE_HANDLES"
#endif

#if FR_BLE_ENABLE_CENTRAL && !FR_BLE_ENABLE_OBSERVER
#error "BLE central requires observer"
#endif

#if FR_BLE_ENABLE_PERIPHERAL && !FR_BLE_ENABLE_BROADCASTER
#error "BLE peripheral requires broadcaster"
#endif

#if FR_BLE_ENABLE_OBSERVER &&                                              \
    (FR_BLE_SCAN_QUEUE_COUNT < 1 || FR_BLE_SCAN_QUEUE_COUNT > 255)
#error "BLE scan queue count must fit one byte"
#endif

#if FR_BLE_ENABLE_OBSERVER && FR_BLE_SCAN_DATA_BYTES != 31
#error "legacy BLE scan reports require exactly 31 payload bytes"
#endif

#if !FR_BLE_ENABLE_OBSERVER &&                                             \
    (FR_BLE_SCAN_QUEUE_COUNT != 0 || FR_BLE_SCAN_DATA_BYTES != 0)
#error "BLE scan limits require the observer role"
#endif

#if FR_FEATURE_BLE &&                                                      \
    (FR_BLE_START_TIMEOUT_MS < 1 || FR_BLE_STOP_TIMEOUT_MS < 1)
#error "BLE lifecycle timeouts must be positive"
#endif

#if !FR_FEATURE_BLE &&                                                     \
    (FR_BLE_START_TIMEOUT_MS != 0 || FR_BLE_STOP_TIMEOUT_MS != 0)
#error "BLE lifecycle timeouts require FR_FEATURE_BLE"
#endif

#if FR_FEATURE_PAD && FR_PROFILE_PAD_BYTES == 0
#error "FR_FEATURE_PAD requires FR_PROFILE_PAD_BYTES"
#endif

#if FR_FEATURE_PAD && !FR_FEATURE_REPL
#error "FR_FEATURE_PAD requires FR_FEATURE_REPL in this tranche"
#endif

#if !FR_FEATURE_PAD && FR_PROFILE_PAD_BYTES > 0
#error "FR_PROFILE_PAD_BYTES requires FR_FEATURE_PAD"
#endif

#if FR_FEATURE_PERSISTENCE
#ifndef FR_PROFILE_PERSISTENCE_BYTES
#error "FR_FEATURE_PERSISTENCE requires FR_PROFILE_PERSISTENCE_BYTES"
#endif
#else
#undef FR_PROFILE_PERSISTENCE_BYTES
#define FR_PROFILE_PERSISTENCE_BYTES 0
#endif

#if (FR_FEATURE_COMPILER || FR_FEATURE_INTROSPECTION) &&                       \
    !FR_BASE_IMAGE_INCLUDE_SYMBOLS
#error "current compiler/introspection require base image symbols"
#endif

#if FR_FEATURE_OVERLAY_APPLY_COMMAND && !FR_FEATURE_REPL
#error "FR_FEATURE_OVERLAY_APPLY_COMMAND requires FR_FEATURE_REPL"
#endif

#if FR_FEATURE_NUMERIC_SLOT_CALLS && !FR_FEATURE_REPL
#error "FR_FEATURE_NUMERIC_SLOT_CALLS requires FR_FEATURE_REPL"
#endif
