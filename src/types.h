#pragma once

#include "config.h"

#include <stddef.h>
#include <stdint.h>

typedef struct fr_runtime_t fr_runtime_t;

typedef enum fr_err_t {
  FR_OK = 0,
  FR_ERR_RANGE,
  FR_ERR_TYPE,
  FR_ERR_DOMAIN,
  FR_ERR_CAPACITY,
  FR_ERR_OVERFLOW,
  FR_ERR_UNDERFLOW,
  FR_ERR_NOT_FOUND,
  FR_ERR_INVALID,
  FR_ERR_UNSUPPORTED,
  FR_ERR_INTERRUPTED,
  FR_ERR_CORRUPT,
  FR_ERR_IO,
  FR_ERR_VOLATILE,
  FR_ERR_HANDLE,
  FR_ERR_NET_DISCONNECTED,
  FR_ERR_NET_TIMEOUT,
  FR_ERR_NET_DNS,
  FR_ERR_NET_REFUSED,
  FR_ERR_NET_TOO_LARGE,
  FR_ERR_NET_PROTOCOL,
  FR_ERR_BLE_NOT_READY,
  FR_ERR_BLE_BUSY,
  FR_ERR_BLE_TIMEOUT,
  FR_ERR_BLE_DISCONNECTED,
} fr_err_t;

const char *fr_err_name(fr_err_t err);

typedef enum fr_diag_message_id_t {
  FR_DIAG_MSG_NONE = 0,
  /* Parser messages. Keep ids stable; later diagnostic tranches can append. */
  FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN,
  FR_DIAG_MSG_PARSE_EXPECTED_IS,
  FR_DIAG_MSG_PARSE_EXPECTED_COLON_BEFORE_ARGUMENT,
  FR_DIAG_MSG_PARSE_EXPECTED_WORD_NAME,
  FR_DIAG_MSG_PARSE_RESERVED_NAME,
  FR_DIAG_MSG_PARSE_RESERVED_PARAMETER,
  FR_DIAG_MSG_PARSE_DUPLICATE_PARAMETER,
  FR_DIAG_MSG_PARSE_EXPECTED_PARAMETER,
  FR_DIAG_MSG_PARSE_EXPECTED_BLOCK_START,
  FR_DIAG_MSG_PARSE_EXPECTED_BLOCK_END,
  FR_DIAG_MSG_PARSE_UNEXPECTED_BLOCK_END,
  FR_DIAG_MSG_PARSE_EXPECTED_GROUP_END,
  FR_DIAG_MSG_PARSE_UNEXPECTED_GROUP_END,
  FR_DIAG_MSG_PARSE_EMPTY_BLOCK,
  FR_DIAG_MSG_PARSE_UNTERMINATED_TEXT,
  FR_DIAG_MSG_PARSE_BAD_ESCAPE,
  FR_DIAG_MSG_PARSE_TEXT_TOO_LONG,
  FR_DIAG_MSG_PARSE_TOKEN_TOO_LONG,
  FR_DIAG_MSG_PARSE_BAD_INTEGER,
  FR_DIAG_MSG_PARSE_INTEGER_RANGE,
  FR_DIAG_MSG_PARSE_TEXT_DISABLED,
  FR_DIAG_MSG_PARSE_CELLS_DISABLED,
  FR_DIAG_MSG_PARSE_RECORDS_DISABLED,
  FR_DIAG_MSG_PARSE_EMPTY_RECORD,
  FR_DIAG_MSG_PARSE_EXPECTED_FIELD,
  FR_DIAG_MSG_PARSE_DUPLICATE_FIELD,
  FR_DIAG_MSG_PARSE_BAD_FIELD,
  FR_DIAG_MSG_PARSE_EXPECTED_TO,
  FR_DIAG_MSG_PARSE_EXPECTED_EVENT_EDGE,
  FR_DIAG_MSG_PARSE_TOO_DEEP,
  /* Compile messages. Appended after E2 parser ids. */
  FR_DIAG_MSG_COMPILE_EVENT_BODY_LOCAL,
  FR_DIAG_MSG_COMPILE_CONTROL_FLOW_DISABLED,
  FR_DIAG_MSG_COMPILE_CELLS_DISABLED,
  FR_DIAG_MSG_COMPILE_TEXT_DISABLED,
  FR_DIAG_MSG_COMPILE_RECORDS_DISABLED,
  FR_DIAG_MSG_COMPILE_EVENTS_DISABLED,
  FR_DIAG_MSG_COMPILE_PARAM_SHADOW,
  FR_DIAG_MSG_COMPILE_RECORD_NAME_NOT_SHAPE,
  /* Runtime messages. Appended after E3 compile ids. */
  FR_DIAG_MSG_RUNTIME_CELL_INDEX_OOB,
  FR_DIAG_MSG_RUNTIME_CALL_DEPTH,
  FR_DIAG_MSG_RUNTIME_STACK_OVERFLOW,
  FR_DIAG_MSG_RUNTIME_STACK_UNDERFLOW,
  FR_DIAG_MSG_RUNTIME_TOO_FEW_ARGS,
  FR_DIAG_MSG_RUNTIME_INTEGER_OVERFLOW,
  FR_DIAG_MSG_RUNTIME_SLOT_UNPERSISTABLE,
} fr_diag_message_id_t;

const char *fr_diag_message(uint16_t message_id);

typedef enum fr_diag_kind_t {
  FR_DIAG_NONE = 0,
  FR_DIAG_NAME,
  FR_DIAG_TYPE,
  FR_DIAG_ARITY,
  FR_DIAG_LIMIT,
  FR_DIAG_TOKEN,
  FR_DIAG_NOTE,
} fr_diag_kind_t;

typedef int32_t fr_int_t;

typedef enum fr_diag_value_kind_t {
  FR_DIAG_VALUE_NONE = 0,
  FR_DIAG_VALUE_INT,
  FR_DIAG_VALUE_BOOL,
  FR_DIAG_VALUE_NIL,
  FR_DIAG_VALUE_SPECIAL,
  FR_DIAG_VALUE_SLOT,
  FR_DIAG_VALUE_FUNCTION,
  FR_DIAG_VALUE_NATIVE,
  FR_DIAG_VALUE_OBJECT,
  FR_DIAG_VALUE_HANDLE,
  FR_DIAG_VALUE_BYTES,
  FR_DIAG_VALUE_RESERVED,
  FR_DIAG_VALUE_ANY,
  FR_DIAG_VALUE_TEXT,
  FR_DIAG_VALUE_TEXT_OR_BYTES,
} fr_diag_value_kind_t;

const char *fr_diag_value_kind_name(uint16_t value_kind);

typedef enum fr_diag_actual_state_t {
  FR_DIAG_ACTUAL_NONE = 0,
  FR_DIAG_ACTUAL_VALUE,
  FR_DIAG_ACTUAL_REDACTED,
} fr_diag_actual_state_t;

typedef struct fr_diagnostic_t {
  fr_diag_kind_t kind;
  const char *span_start;
  uint16_t span_length;
  uint16_t message_id;
  fr_int_t expected;
  fr_int_t got;
  uint32_t actual;
  fr_diag_actual_state_t actual_state;
  uint16_t index;
  /* May point into suggestion_text; do not copy a populated diagnostic. */
  const char *context_name;
  const char *suggestion_start;
  uint16_t suggestion_length;
  char suggestion_text[FR_PROFILE_PARSE_MAX_TOKEN_BYTES + 1];
} fr_diagnostic_t;

typedef uint16_t fr_slot_id_t;
typedef uint16_t fr_code_object_id_t;
typedef uint16_t fr_native_id_t;
typedef uint16_t fr_object_id_t;
typedef uint8_t fr_handle_id_t;
typedef uint8_t fr_handle_generation_t;
typedef uint8_t fr_handle_kind_t;
typedef uint16_t fr_code_offset_t;
typedef uintptr_t fr_addr_t;

typedef struct fr_handle_ref_t {
  fr_handle_id_t id;
  fr_handle_generation_t generation;
} fr_handle_ref_t;

typedef uint8_t fr_bytes_id_t;
typedef uint8_t fr_bytes_generation_t;

typedef struct fr_bytes_ref_t {
  fr_bytes_id_t id;
  fr_bytes_generation_t generation;
} fr_bytes_ref_t;

/* T12L-7 D3: session-scoped install tier on the runtime. Wire-byte values
   match FR_PERSIST_TIER_* so the persist encoder can cast directly. */
typedef enum fr_install_tier_t {
  FR_INSTALL_TIER_LIBRARY = 1,
  FR_INSTALL_TIER_USER = 2,
} fr_install_tier_t;

#define FR_TRY(expr)                                                           \
  do {                                                                         \
    fr_err_t fr_try_err = (expr);                                              \
    if (fr_try_err != FR_OK) {                                                 \
      return fr_try_err;                                                       \
    }                                                                          \
  } while (0)
