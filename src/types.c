#include "types.h"

#include <stddef.h>

const char *fr_err_name(fr_err_t err) {
  switch (err) {
  case FR_ERR_RANGE:
    return "out of range";
  case FR_ERR_TYPE:
    return "wrong type";
  case FR_ERR_DOMAIN:
    return "bad value";
  case FR_ERR_CAPACITY:
    return "capacity exceeded";
  case FR_ERR_OVERFLOW:
    return "overflow";
  case FR_ERR_UNDERFLOW:
    return "underflow";
  case FR_ERR_NOT_FOUND:
    return "not found";
  case FR_ERR_INVALID:
    return "bad source";
  case FR_ERR_UNSUPPORTED:
    return "unsupported";
  case FR_ERR_INTERRUPTED:
    return "interrupted";
  case FR_ERR_CORRUPT:
    return "corrupt data";
  case FR_ERR_IO:
    return "i/o failed";
  case FR_ERR_VOLATILE:
    return "not saved";
  case FR_ERR_HANDLE:
    return "bad handle";
  case FR_ERR_NET_DISCONNECTED:
    return "no network";
  case FR_ERR_NET_TIMEOUT:
    return "timed out";
  case FR_ERR_NET_DNS:
    return "dns failed";
  case FR_ERR_NET_REFUSED:
    return "refused";
  case FR_ERR_NET_TOO_LARGE:
    return "too large";
  case FR_ERR_NET_PROTOCOL:
    return "bad protocol";
  case FR_ERR_BLE_NOT_READY:
    return "ble not ready";
  case FR_ERR_BLE_BUSY:
    return "ble busy";
  case FR_ERR_BLE_TIMEOUT:
    return "ble timed out";
  case FR_ERR_BLE_DISCONNECTED:
    return "ble disconnected";
  case FR_OK:
  default:
    return NULL;
  }
}

const char *fr_diag_message(uint16_t message_id) {
  static const char *const messages[] = {
      [FR_DIAG_MSG_NONE] = NULL,
      [FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN] = "unexpected token",
      [FR_DIAG_MSG_PARSE_EXPECTED_IS] =
          "expected 'is' before the value of a definition",
      [FR_DIAG_MSG_PARSE_EXPECTED_COLON_BEFORE_ARGUMENT] =
          "expected ':' before the argument to a word",
      [FR_DIAG_MSG_PARSE_EXPECTED_WORD_NAME] = "expected a word name",
      [FR_DIAG_MSG_PARSE_RESERVED_NAME] =
          "reserved word cannot be used as a name",
      [FR_DIAG_MSG_PARSE_RESERVED_PARAMETER] =
          "reserved word cannot be used as a parameter",
      [FR_DIAG_MSG_PARSE_DUPLICATE_PARAMETER] =
          "parameter name is already used",
      [FR_DIAG_MSG_PARSE_EXPECTED_PARAMETER] = "expected a parameter name",
      [FR_DIAG_MSG_PARSE_EXPECTED_BLOCK_START] =
          "expected '[' to start the block",
      [FR_DIAG_MSG_PARSE_EXPECTED_BLOCK_END] =
          "expected ']' to close the block",
      [FR_DIAG_MSG_PARSE_UNEXPECTED_BLOCK_END] = "unexpected ']'",
      [FR_DIAG_MSG_PARSE_EXPECTED_GROUP_END] =
          "expected ')' to close the group",
      [FR_DIAG_MSG_PARSE_UNEXPECTED_GROUP_END] = "unexpected ')'",
      [FR_DIAG_MSG_PARSE_EMPTY_BLOCK] = "block needs an expression",
      [FR_DIAG_MSG_PARSE_UNTERMINATED_TEXT] = "unterminated text literal",
      [FR_DIAG_MSG_PARSE_BAD_ESCAPE] = "malformed text escape",
      [FR_DIAG_MSG_PARSE_TEXT_TOO_LONG] = "text literal is too long",
      [FR_DIAG_MSG_PARSE_TOKEN_TOO_LONG] = "token is too long",
      [FR_DIAG_MSG_PARSE_BAD_INTEGER] = "malformed integer literal",
      [FR_DIAG_MSG_PARSE_INTEGER_RANGE] = "integer literal is out of range",
      [FR_DIAG_MSG_PARSE_TEXT_DISABLED] =
          "text is not enabled in this build",
      [FR_DIAG_MSG_PARSE_CELLS_DISABLED] =
          "cells are not enabled in this build",
      [FR_DIAG_MSG_PARSE_RECORDS_DISABLED] =
          "records are not enabled in this build",
      [FR_DIAG_MSG_PARSE_EMPTY_RECORD] = "record needs at least one field",
      [FR_DIAG_MSG_PARSE_EXPECTED_FIELD] = "expected a field name",
      [FR_DIAG_MSG_PARSE_DUPLICATE_FIELD] = "field name is already used",
      [FR_DIAG_MSG_PARSE_BAD_FIELD] = "field name cannot contain '.'",
      [FR_DIAG_MSG_PARSE_EXPECTED_TO] = "expected 'to' before the value",
      [FR_DIAG_MSG_PARSE_EXPECTED_EVENT_EDGE] = "expected an event edge",
      [FR_DIAG_MSG_PARSE_TOO_DEEP] = "expression is too deeply nested",
      [FR_DIAG_MSG_COMPILE_EVENT_BODY_LOCAL] =
          "event bodies can't use the caller's locals -- lift it to a global",
      [FR_DIAG_MSG_COMPILE_CONTROL_FLOW_DISABLED] =
          "control flow is not enabled in this build",
      [FR_DIAG_MSG_COMPILE_CELLS_DISABLED] =
          "cells are not enabled in this build",
      [FR_DIAG_MSG_COMPILE_TEXT_DISABLED] =
          "text is not enabled in this build",
      [FR_DIAG_MSG_COMPILE_RECORDS_DISABLED] =
          "records are not enabled in this build",
      [FR_DIAG_MSG_COMPILE_EVENTS_DISABLED] =
          "events are not enabled in this build",
      [FR_DIAG_MSG_COMPILE_PARAM_SHADOW] =
          "parameter shadows an existing name",
      [FR_DIAG_MSG_COMPILE_RECORD_NAME_NOT_SHAPE] =
          "record name is already used by another value",
      [FR_DIAG_MSG_RUNTIME_STACK_OVERFLOW] = "stack overflow",
      [FR_DIAG_MSG_RUNTIME_STACK_UNDERFLOW] = "stack underflow",
      [FR_DIAG_MSG_RUNTIME_INTEGER_OVERFLOW] = "integer overflow",
  };

  if (message_id >= (uint16_t)(sizeof(messages) / sizeof(messages[0]))) {
    return NULL;
  }
  return messages[message_id];
}

const char *fr_diag_value_kind_name(uint16_t value_kind) {
  switch ((fr_diag_value_kind_t)value_kind) {
  case FR_DIAG_VALUE_INT:
    return "int";
  case FR_DIAG_VALUE_BOOL:
    return "bool";
  case FR_DIAG_VALUE_NIL:
    return "nil";
  case FR_DIAG_VALUE_SPECIAL:
    return "special";
  case FR_DIAG_VALUE_SLOT:
    return "slot";
  case FR_DIAG_VALUE_FUNCTION:
    return "function";
  case FR_DIAG_VALUE_NATIVE:
    return "native";
  case FR_DIAG_VALUE_OBJECT:
    return "object";
  case FR_DIAG_VALUE_HANDLE:
    return "handle";
  case FR_DIAG_VALUE_BYTES:
    return "bytes";
  case FR_DIAG_VALUE_RESERVED:
    return "reserved";
  case FR_DIAG_VALUE_ANY:
    return "anything";
  case FR_DIAG_VALUE_TEXT:
    return "text";
  case FR_DIAG_VALUE_TEXT_OR_BYTES:
    return "text or bytes";
  case FR_DIAG_VALUE_NONE:
  default:
    return NULL;
  }
}
