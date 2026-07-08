#pragma once

#include "types.h"

#include <stdbool.h>

#define FR_PARSE_MAX_TOKEN_BYTES FR_PROFILE_PARSE_MAX_TOKEN_BYTES
#define FR_PARSE_MAX_EXPR_DEPTH FR_PROFILE_PARSE_MAX_EXPR_DEPTH
#define FR_PARSE_MAX_EXPR_NODES FR_PROFILE_PARSE_MAX_EXPR_NODES
#define FR_PARSE_MAX_BODY_EXPRS FR_PROFILE_PARSE_MAX_BODY_EXPRS
#define FR_PARSE_MAX_PARAMS FR_PROFILE_PARSE_MAX_PARAMS
#define FR_PARSE_MAX_LOCALS FR_PROFILE_PARSE_MAX_LOCALS

#define FR_PARSE_MAX_RECORD_FIELDS                                           \
  (FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE > 0                                \
       ? FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE                              \
       : 1)

typedef struct fr_parse_span_t {
  const char *start;
  uint16_t length;
} fr_parse_span_t;

typedef uint8_t fr_parse_expr_id_t;

typedef enum fr_parse_expr_kind_t {
  FR_PARSE_EXPR_NIL,
  FR_PARSE_EXPR_FALSE,
  FR_PARSE_EXPR_TRUE,
  FR_PARSE_EXPR_INT,
  FR_PARSE_EXPR_TEXT,
  FR_PARSE_EXPR_NAME,
  FR_PARSE_EXPR_CALL,
  FR_PARSE_EXPR_FUNCTION,
  FR_PARSE_EXPR_LIST,
  FR_PARSE_EXPR_IF,
  FR_PARSE_EXPR_REPEAT,
  FR_PARSE_EXPR_WHILE,
  FR_PARSE_EXPR_FOREVER,
  FR_PARSE_EXPR_CELLS,
  FR_PARSE_EXPR_CELL_READ,
  FR_PARSE_EXPR_CELL_WRITE,
  FR_PARSE_EXPR_FIELD_READ,
  FR_PARSE_EXPR_FIELD_WRITE,
  FR_PARSE_EXPR_NAME_WRITE,
  FR_PARSE_EXPR_LOCAL_BIND,
  FR_PARSE_EXPR_LT,
  FR_PARSE_EXPR_GT,
  FR_PARSE_EXPR_LE,
  FR_PARSE_EXPR_GE,
  FR_PARSE_EXPR_EQ,
  FR_PARSE_EXPR_NE,
  FR_PARSE_EXPR_AND,
  FR_PARSE_EXPR_OR,
  FR_PARSE_EXPR_NOT,
  FR_PARSE_EXPR_ADD,
  FR_PARSE_EXPR_SUB,
  FR_PARSE_EXPR_MUL,
  FR_PARSE_EXPR_DIV,
  FR_PARSE_EXPR_MOD,
  FR_PARSE_EXPR_EVENT_REGISTER,
  FR_PARSE_EXPR_EVENT_CANCEL,
} fr_parse_expr_kind_t;

typedef struct fr_parse_expr_t {
  fr_parse_expr_kind_t kind;
  fr_parse_span_t name;
  fr_parse_span_t text;
  fr_int_t int_value;
  fr_parse_expr_id_t child;
  fr_parse_expr_id_t children[FR_PARSE_MAX_BODY_EXPRS];
  uint8_t param_start;
  uint8_t param_count;
  uint8_t child_count;
  bool text_has_escapes;
} fr_parse_expr_t;

typedef struct fr_parse_definition_t {
  fr_parse_span_t name;
  fr_parse_expr_id_t value;
} fr_parse_definition_t;

typedef enum fr_parse_line_kind_t {
  FR_PARSE_LINE_DEFINITION = 0,
  FR_PARSE_LINE_RECORD_SHAPE = 1,
} fr_parse_line_kind_t;

typedef struct fr_parse_line_t {
  fr_parse_expr_t exprs[FR_PARSE_MAX_EXPR_NODES];
  fr_parse_span_t params[FR_PARSE_MAX_PARAMS];
  fr_parse_span_t record_fields[FR_PARSE_MAX_RECORD_FIELDS];
  fr_parse_line_kind_t kind;
  uint8_t expr_count;
  uint8_t param_count;
  uint8_t record_field_count;
  fr_parse_definition_t definition;
} fr_parse_line_t;

bool fr_parse_span_equals(fr_parse_span_t span, const char *text);
fr_err_t fr_parse_expression_line(const char *source, fr_parse_line_t *out,
                                  fr_parse_expr_id_t *out_expr);
fr_err_t fr_parse_expression_line_with_diagnostic(
    const char *source, fr_parse_line_t *out, fr_parse_expr_id_t *out_expr,
    fr_diagnostic_t *diag);
fr_err_t fr_parse_line(const char *source, fr_parse_line_t *out);
fr_err_t fr_parse_line_with_diagnostic(const char *source, fr_parse_line_t *out,
                                       fr_diagnostic_t *diag);
