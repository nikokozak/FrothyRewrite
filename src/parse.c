#include "parse.h"

#include "tagged.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum fr_token_kind_t {
  FR_TOKEN_EOF,
  FR_TOKEN_NAME,
  FR_TOKEN_INT,
  FR_TOKEN_TEXT,
  FR_TOKEN_LBRACKET,
  FR_TOKEN_RBRACKET,
  FR_TOKEN_LPAREN,
  FR_TOKEN_RPAREN,
  FR_TOKEN_COLON,
  FR_TOKEN_SEMICOLON,
  FR_TOKEN_COMMA,
  FR_TOKEN_ARROW,
  FR_TOKEN_LT,
  FR_TOKEN_GT,
  FR_TOKEN_LE,
  FR_TOKEN_GE,
  FR_TOKEN_EQ,
  FR_TOKEN_NE,
  FR_TOKEN_MINUS,
  FR_TOKEN_STAR,
  FR_TOKEN_SLASH,
} fr_token_kind_t;

typedef struct fr_token_t {
  fr_token_kind_t kind;
  fr_parse_span_t span;
  fr_int_t int_value;
  bool leading_space;
} fr_token_t;

typedef struct fr_parser_t {
  const char *cursor;
  fr_token_t token;
  fr_parse_line_t *out;
  uint8_t expr_depth;
} fr_parser_t;

#if FR_WORD_SIZE == 16
typedef uint16_t fr_parse_int_magnitude_t;
#else
typedef uint32_t fr_parse_int_magnitude_t;
#endif

#define FR_PARSE_INT_POS_LIMIT                                                \
  ((fr_parse_int_magnitude_t)FR_TAGGED_INT_MAX)
#define FR_PARSE_INT_NEG_LIMIT                                                \
  ((fr_parse_int_magnitude_t)(FR_TAGGED_INT_MAX + 1u))
#define FR_PARSE_INT_POS_CUTOFF                                               \
  ((fr_parse_int_magnitude_t)(FR_PARSE_INT_POS_LIMIT / 10u))
#define FR_PARSE_INT_NEG_CUTOFF                                               \
  ((fr_parse_int_magnitude_t)(FR_PARSE_INT_NEG_LIMIT / 10u))
#define FR_PARSE_INT_POS_CUTLIM ((uint8_t)(FR_PARSE_INT_POS_LIMIT % 10u))
#define FR_PARSE_INT_NEG_CUTLIM ((uint8_t)(FR_PARSE_INT_NEG_LIMIT % 10u))

static bool fr_parse_is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool fr_parse_is_digit(char c) { return c >= '0' && c <= '9'; }

static bool fr_parse_is_punctuation(char c) {
  return c == '[' || c == ']' || c == '(' || c == ')' || c == ':' ||
         c == ',' || c == ';';
}

static bool fr_parse_is_arrow_start(const char *text) {
  return text != NULL && text[0] == '-' && text[1] == '>';
}

static bool fr_parse_is_compare_op_char(char c) {
  return c == '<' || c == '>' || c == '=';
}

static bool fr_parse_minus_infix_after(fr_token_kind_t kind) {
  return kind == FR_TOKEN_INT || kind == FR_TOKEN_RBRACKET ||
         kind == FR_TOKEN_RPAREN || kind == FR_TOKEN_TEXT ||
         kind == FR_TOKEN_NAME;
}

bool fr_parse_span_equals(fr_parse_span_t span, const char *text) {
  uint16_t i = 0;

  if (span.start == NULL || text == NULL) {
    return false;
  }

  while (i < span.length && text[i] != '\0') {
    if (span.start[i] != text[i]) {
      return false;
    }
    i += 1;
  }

  return i == span.length && text[i] == '\0';
}

static bool fr_parse_skip_space(fr_parser_t *parser) {
  bool skipped = false;

  while (fr_parse_is_space(*parser->cursor)) {
    skipped = true;
    parser->cursor += 1;
  }
  return skipped;
}

static fr_err_t fr_parse_token_int(fr_parse_span_t span, fr_int_t *out_int) {
  bool negative = false;
  fr_parse_int_magnitude_t parsed = 0;
  fr_parse_int_magnitude_t cutoff = 0;
  uint8_t cutlim = 0;
  uint16_t i = 0;

  if (span.length == 0 || out_int == NULL) {
    return FR_ERR_INVALID;
  }

  if (span.start[i] == '-') {
    negative = true;
    i += 1;
  }
  if (i == span.length) {
    return FR_ERR_UNSUPPORTED;
  }
  cutoff = negative ? FR_PARSE_INT_NEG_CUTOFF : FR_PARSE_INT_POS_CUTOFF;
  cutlim = negative ? FR_PARSE_INT_NEG_CUTLIM : FR_PARSE_INT_POS_CUTLIM;

  while (i < span.length) {
    uint8_t digit = (uint8_t)(span.start[i] - '0');

    if (!fr_parse_is_digit(span.start[i])) {
      return FR_ERR_UNSUPPORTED;
    }
    if (parsed > cutoff || (parsed == cutoff && digit > cutlim)) {
      return FR_ERR_RANGE;
    }
    parsed = (fr_parse_int_magnitude_t)((parsed * 10u) + digit);
    i += 1;
  }

  if (negative) {
    if (parsed == FR_PARSE_INT_NEG_LIMIT) {
      *out_int = (fr_int_t)FR_TAGGED_INT_MIN;
    } else {
      *out_int = (fr_int_t)(-(fr_int_t)parsed);
    }
    return FR_OK;
  }

  *out_int = (fr_int_t)parsed;
  return FR_OK;
}

static bool fr_parse_span_looks_int(fr_parse_span_t span) {
  if (span.length == 0) {
    return false;
  }
  if (fr_parse_is_digit(span.start[0])) {
    return true;
  }
  return span.length > 1 && span.start[0] == '-' &&
         fr_parse_is_digit(span.start[1]);
}

static fr_err_t fr_parse_read_token(fr_parser_t *parser) {
  fr_parse_span_t span = {0};
  char c = '\0';
  bool leading_space = false;
  fr_token_kind_t prev_kind = parser->token.kind;

  leading_space = fr_parse_skip_space(parser);
  c = *parser->cursor;
  span.start = parser->cursor;

  if (c == '\0') {
    parser->token = (fr_token_t){
        .kind = FR_TOKEN_EOF, .span = span, .leading_space = leading_space};
    return FR_OK;
  }

  if (fr_parse_is_arrow_start(parser->cursor)) {
    parser->cursor += 2;
    span.length = 2;
    parser->token = (fr_token_t){
        .kind = FR_TOKEN_ARROW, .span = span, .leading_space = leading_space};
    return FR_OK;
  }

  if (c == '"') {
#if !FR_FEATURE_TEXT
    return FR_ERR_UNSUPPORTED;
#else
    parser->cursor += 1;
    span.start = parser->cursor;
    span.length = 0;
    while (*parser->cursor != '\0' && *parser->cursor != '"') {
      if (*parser->cursor == '\n' || *parser->cursor == '\r') {
        return FR_ERR_INVALID;
      }
      if (span.length >= FR_PROFILE_MAX_TEXT_LENGTH) {
        return FR_ERR_RANGE;
      }
      span.length += 1;
      parser->cursor += 1;
    }
    if (*parser->cursor != '"') {
      return FR_ERR_INVALID;
    }
    parser->cursor += 1;
    parser->token = (fr_token_t){
        .kind = FR_TOKEN_TEXT, .span = span, .leading_space = leading_space};
    return FR_OK;
#endif
  }

  if (fr_parse_is_compare_op_char(c)) {
    parser->cursor += 1;
    span.length = 1;
    parser->token.span = span;
    parser->token.int_value = 0;
    parser->token.leading_space = leading_space;
    if (c == '<' && *parser->cursor == '=') {
      parser->cursor += 1;
      parser->token.span.length = 2;
      parser->token.kind = FR_TOKEN_LE;
    } else if (c == '<' && *parser->cursor == '>') {
      parser->cursor += 1;
      parser->token.span.length = 2;
      parser->token.kind = FR_TOKEN_NE;
    } else if (c == '>' && *parser->cursor == '=') {
      parser->cursor += 1;
      parser->token.span.length = 2;
      parser->token.kind = FR_TOKEN_GE;
    } else if (c == '<') {
      parser->token.kind = FR_TOKEN_LT;
    } else if (c == '>') {
      parser->token.kind = FR_TOKEN_GT;
    } else {
      parser->token.kind = FR_TOKEN_EQ;
    }
    return FR_OK;
  }

  if (c == '*' || c == '/' ||
      (c == '-' &&
       (!fr_parse_is_digit(parser->cursor[1]) ||
        (!leading_space && fr_parse_minus_infix_after(prev_kind))))) {
    parser->cursor += 1;
    span.length = 1;
    parser->token.span = span;
    parser->token.int_value = 0;
    parser->token.leading_space = leading_space;
    parser->token.kind = (c == '*')   ? FR_TOKEN_STAR
                         : (c == '/') ? FR_TOKEN_SLASH
                                      : FR_TOKEN_MINUS;
    return FR_OK;
  }

  if (fr_parse_is_punctuation(c)) {
    parser->cursor += 1;
    span.length = 1;
    parser->token.span = span;
    parser->token.int_value = 0;
    parser->token.leading_space = leading_space;
    if (c == '[') {
      parser->token.kind = FR_TOKEN_LBRACKET;
    } else if (c == ']') {
      parser->token.kind = FR_TOKEN_RBRACKET;
    } else if (c == '(') {
      parser->token.kind = FR_TOKEN_LPAREN;
    } else if (c == ')') {
      parser->token.kind = FR_TOKEN_RPAREN;
    } else if (c == ':') {
      parser->token.kind = FR_TOKEN_COLON;
    } else if (c == ';') {
      parser->token.kind = FR_TOKEN_SEMICOLON;
    } else {
      parser->token.kind = FR_TOKEN_COMMA;
    }
    return FR_OK;
  }

  /* Stop at `-` only when one side is a digit, so `5-2` and `x-1` read as
   * infix subtraction while `emit-byte` and `uart.write-byte` stay one
   * name. `x-y` is a name; for subtraction between two names, write
   * `x - y` with spaces. */
  while (*parser->cursor != '\0' && !fr_parse_is_space(*parser->cursor) &&
         !fr_parse_is_punctuation(*parser->cursor) &&
         !fr_parse_is_compare_op_char(*parser->cursor) &&
         *parser->cursor != '*' && *parser->cursor != '/' &&
         !fr_parse_is_arrow_start(parser->cursor) &&
         !(*parser->cursor == '-' && span.length > 0 &&
           (fr_parse_is_digit(span.start[0]) ||
            fr_parse_is_digit(parser->cursor[1])))) {
    if (span.length >= FR_PARSE_MAX_TOKEN_BYTES) {
      return FR_ERR_RANGE;
    }
    span.length += 1;
    parser->cursor += 1;
  }

  parser->token = (fr_token_t){
      .kind = FR_TOKEN_NAME, .span = span, .leading_space = leading_space};
  if (fr_parse_span_looks_int(span)) {
    fr_err_t err = fr_parse_token_int(span, &parser->token.int_value);
    if (err == FR_OK) {
      parser->token.kind = FR_TOKEN_INT;
    } else if (err != FR_ERR_UNSUPPORTED) {
      return err;
    }
  }
  return FR_OK;
}

static fr_err_t fr_parse_advance(fr_parser_t *parser) {
  return fr_parse_read_token(parser);
}

static fr_err_t fr_parse_expect(fr_parser_t *parser, fr_token_kind_t kind) {
  if (parser->token.kind != kind) {
    return FR_ERR_INVALID;
  }
  return fr_parse_advance(parser);
}

static fr_err_t fr_parse_expect_word(fr_parser_t *parser, const char *word) {
  if (parser->token.kind != FR_TOKEN_NAME ||
      !fr_parse_span_equals(parser->token.span, word)) {
    return FR_ERR_INVALID;
  }
  return fr_parse_advance(parser);
}

static fr_err_t fr_parse_finish_line(fr_parser_t *parser) {
  if (parser->token.kind == FR_TOKEN_SEMICOLON) {
    FR_TRY(fr_parse_advance(parser));
  }
  if (parser->token.kind != FR_TOKEN_EOF) {
    return FR_ERR_INVALID;
  }
  return FR_OK;
}

static fr_err_t fr_parse_add_expr(fr_parser_t *parser, fr_parse_expr_t expr,
                                  fr_parse_expr_id_t *out_id) {
  if (parser->out->expr_count >= FR_PARSE_MAX_EXPR_NODES) {
    return FR_ERR_CAPACITY;
  }
  *out_id = parser->out->expr_count;
  parser->out->exprs[parser->out->expr_count] = expr;
  parser->out->expr_count += 1;
  return FR_OK;
}

static bool fr_parse_span_same(fr_parse_span_t lhs, fr_parse_span_t rhs) {
  if (lhs.length != rhs.length || lhs.start == NULL || rhs.start == NULL) {
    return false;
  }
  for (uint16_t i = 0; i < lhs.length; i++) {
    if (lhs.start[i] != rhs.start[i]) {
      return false;
    }
  }
  return true;
}

static bool fr_parse_is_reserved_parameter(fr_parse_span_t name) {
  return fr_parse_span_equals(name, "nil") ||
         fr_parse_span_equals(name, "true") ||
         fr_parse_span_equals(name, "false") ||
         fr_parse_span_equals(name, "fn") ||
         fr_parse_span_equals(name, "with") ||
         fr_parse_span_equals(name, "is") ||
         fr_parse_span_equals(name, "if") ||
         fr_parse_span_equals(name, "else") ||
         fr_parse_span_equals(name, "when") ||
         fr_parse_span_equals(name, "unless") ||
         fr_parse_span_equals(name, "repeat") ||
         fr_parse_span_equals(name, "while") ||
         fr_parse_span_equals(name, "forever") ||
         fr_parse_span_equals(name, "cells") ||
         fr_parse_span_equals(name, "record") ||
         fr_parse_span_equals(name, "set") ||
         fr_parse_span_equals(name, "to");
}

#if FR_FEATURE_RECORDS
static fr_err_t fr_parse_check_field_name(fr_parse_span_t name) {
  if (name.start == NULL || name.length == 0 ||
      name.length > FR_PROFILE_MAX_NAME_BYTES) {
    return FR_ERR_RANGE;
  }
  for (uint16_t i = 0; i < name.length; i++) {
    if (name.start[i] == '.') {
      return FR_ERR_INVALID;
    }
  }
  return FR_OK;
}
#endif

static fr_err_t fr_parse_add_param(fr_parser_t *parser, fr_parse_span_t name,
                                   uint8_t param_start) {
  if (parser == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_parse_is_reserved_parameter(name)) {
    return FR_ERR_INVALID;
  }
  if (parser->out->param_count >= FR_PARSE_MAX_PARAMS) {
    return FR_ERR_CAPACITY;
  }
  for (uint8_t i = param_start; i < parser->out->param_count; i++) {
    if (fr_parse_span_same(parser->out->params[i], name)) {
      return FR_ERR_INVALID;
    }
  }

  parser->out->params[parser->out->param_count] = name;
  parser->out->param_count += 1;
  return FR_OK;
}

static fr_err_t fr_parse_expression(fr_parser_t *parser,
                                    fr_parse_expr_id_t *out_id);
static fr_err_t fr_parse_expression_inner(fr_parser_t *parser,
                                          fr_parse_expr_id_t *out_id);
static fr_err_t fr_parse_statement_list(fr_parser_t *parser,
                                        fr_parse_expr_id_t *out_id);
static fr_err_t fr_parse_field_postfix(fr_parser_t *parser,
                                       fr_parse_expr_id_t base_id,
                                       fr_parse_expr_id_t *out_id) {
  fr_parse_span_t field = {0};

  if (parser->token.kind != FR_TOKEN_ARROW) {
    *out_id = base_id;
    return FR_OK;
  }
#if !FR_FEATURE_RECORDS
  (void)field;
  return FR_ERR_UNSUPPORTED;
#else
  FR_TRY(fr_parse_advance(parser));
  if (parser->token.kind != FR_TOKEN_NAME) {
    return FR_ERR_INVALID;
  }
  field = parser->token.span;
  FR_TRY(fr_parse_check_field_name(field));
  FR_TRY(fr_parse_advance(parser));
  return fr_parse_add_expr(
      parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_FIELD_READ,
                                .name = field,
                                .child = base_id,
                                .child_count = 1},
      out_id);
#endif
}

static fr_err_t fr_parse_bracket_block(fr_parser_t *parser,
                                       fr_parse_expr_id_t *out_id) {
  FR_TRY(fr_parse_expect(parser, FR_TOKEN_LBRACKET));
  FR_TRY(fr_parse_statement_list(parser, out_id));
  return fr_parse_expect(parser, FR_TOKEN_RBRACKET);
}

static fr_err_t fr_parse_function_value(fr_parser_t *parser,
                                        fr_parse_expr_id_t *out_id) {
  uint8_t param_start = parser->out->param_count;
  uint8_t param_count = 0;
  fr_parse_expr_id_t body = 0;

  if (parser->token.kind == FR_TOKEN_NAME &&
      fr_parse_span_equals(parser->token.span, "with")) {
    FR_TRY(fr_parse_advance(parser));
    while (true) {
      if (parser->token.kind != FR_TOKEN_NAME) {
        return FR_ERR_INVALID;
      }
      FR_TRY(fr_parse_add_param(parser, parser->token.span, param_start));
      param_count += 1;
      FR_TRY(fr_parse_advance(parser));

      if (parser->token.kind != FR_TOKEN_COMMA) {
        break;
      }
      FR_TRY(fr_parse_advance(parser));
      if (parser->token.kind == FR_TOKEN_LBRACKET) {
        return FR_ERR_INVALID;
      }
    }
  }
  FR_TRY(fr_parse_bracket_block(parser, &body));

  return fr_parse_add_expr(parser,
                           (fr_parse_expr_t){.kind = FR_PARSE_EXPR_FUNCTION,
                                             .child = body,
                                             .param_start = param_start,
                                             .param_count = param_count,
                                             .child_count = 1},
                           out_id);
}

static fr_err_t fr_parse_function(fr_parser_t *parser,
                                  fr_parse_expr_id_t *out_id) {
  FR_TRY(fr_parse_advance(parser));
  return fr_parse_function_value(parser, out_id);
}

static fr_err_t fr_parse_name_or_call(fr_parser_t *parser,
                                      fr_parse_expr_id_t *out_id) {
  fr_parse_span_t name = parser->token.span;
  fr_parse_expr_id_t base_id = 0;

  FR_TRY(fr_parse_advance(parser));
  if (parser->token.kind == FR_TOKEN_LBRACKET &&
      !parser->token.leading_space) {
#if !FR_FEATURE_CELLS
    return FR_ERR_UNSUPPORTED;
#else
    fr_int_t index = 0;

    FR_TRY(fr_parse_advance(parser));
    if (parser->token.kind != FR_TOKEN_INT) {
      return FR_ERR_INVALID;
    }
    index = parser->token.int_value;
    if (index < 0) {
      return FR_ERR_RANGE;
    }
    FR_TRY(fr_parse_advance(parser));
    FR_TRY(fr_parse_expect(parser, FR_TOKEN_RBRACKET));
    FR_TRY(fr_parse_add_expr(parser,
                             (fr_parse_expr_t){.kind = FR_PARSE_EXPR_CELL_READ,
                                               .name = name,
                                               .int_value = index},
                             &base_id));
    return fr_parse_field_postfix(parser, base_id, out_id);
#endif
  }
  if (parser->token.kind == FR_TOKEN_COLON) {
    fr_parse_expr_t call = {.kind = FR_PARSE_EXPR_CALL, .name = name};

    FR_TRY(fr_parse_advance(parser));
    while (parser->token.kind != FR_TOKEN_EOF &&
           parser->token.kind != FR_TOKEN_RBRACKET &&
           parser->token.kind != FR_TOKEN_LBRACKET &&
           parser->token.kind != FR_TOKEN_SEMICOLON) {
      if (parser->token.kind == FR_TOKEN_COMMA ||
          call.child_count >= FR_PARSE_MAX_BODY_EXPRS) {
        return FR_ERR_INVALID;
      }

      FR_TRY(fr_parse_expression(parser, &call.children[call.child_count]));
      if (call.child_count == 0) {
        call.child = call.children[0];
      }
      call.child_count += 1;

      if (parser->token.kind == FR_TOKEN_COMMA) {
        FR_TRY(fr_parse_advance(parser));
        if (parser->token.kind == FR_TOKEN_EOF ||
            parser->token.kind == FR_TOKEN_RBRACKET ||
            parser->token.kind == FR_TOKEN_SEMICOLON) {
          return FR_ERR_INVALID;
        }
      } else {
        break;
      }
    }
    FR_TRY(fr_parse_add_expr(parser, call, &base_id));
    return fr_parse_field_postfix(parser, base_id, out_id);
  }

  FR_TRY(fr_parse_add_expr(
      parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_NAME, .name = name},
      &base_id));
  return fr_parse_field_postfix(parser, base_id, out_id);
}

#if FR_FEATURE_CELLS
static fr_err_t fr_parse_cells(fr_parser_t *parser,
                               fr_parse_expr_id_t *out_id) {
  fr_int_t length = 0;

  FR_TRY(fr_parse_advance(parser));
  FR_TRY(fr_parse_expect(parser, FR_TOKEN_LPAREN));
  if (parser->token.kind != FR_TOKEN_INT) {
    return FR_ERR_INVALID;
  }
  length = parser->token.int_value;
  if (length <= 0) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_parse_advance(parser));
  FR_TRY(fr_parse_expect(parser, FR_TOKEN_RPAREN));
  return fr_parse_add_expr(parser,
                           (fr_parse_expr_t){.kind = FR_PARSE_EXPR_CELLS,
                                             .int_value = length},
                           out_id);
}
#endif

#if FR_FEATURE_CELLS || FR_FEATURE_RECORDS
static fr_err_t fr_parse_set(fr_parser_t *parser, fr_parse_expr_id_t *out_id) {
  const fr_parse_expr_t *target = NULL;
  fr_parse_expr_id_t target_id = 0;
  fr_parse_expr_id_t value = 0;

  FR_TRY(fr_parse_advance(parser));
  FR_TRY(fr_parse_expression(parser, &target_id));
  FR_TRY(fr_parse_expect_word(parser, "to"));
  FR_TRY(fr_parse_expression(parser, &value));
  if (target_id >= parser->out->expr_count) {
    return FR_ERR_INVALID;
  }
  target = &parser->out->exprs[target_id];
  if (target->kind == FR_PARSE_EXPR_CELL_READ) {
#if !FR_FEATURE_CELLS
    return FR_ERR_UNSUPPORTED;
#else
    return fr_parse_add_expr(
        parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_CELL_WRITE,
                                  .name = target->name,
                                  .int_value = target->int_value,
                                  .child = value,
                                  .child_count = 1},
        out_id);
#endif
  }
  if (target->kind == FR_PARSE_EXPR_FIELD_READ) {
#if !FR_FEATURE_RECORDS
    return FR_ERR_UNSUPPORTED;
#else
    return fr_parse_add_expr(
        parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_FIELD_WRITE,
                                  .name = target->name,
                                  .child = target->child,
                                  .children = {target->child, value},
                                  .child_count = 2},
        out_id);
#endif
  }
  return FR_ERR_INVALID;
}
#endif

static fr_err_t fr_parse_if(fr_parser_t *parser, fr_parse_expr_id_t *out_id) {
  fr_parse_expr_t if_expr = {.kind = FR_PARSE_EXPR_IF};

  FR_TRY(fr_parse_advance(parser));
  FR_TRY(fr_parse_expression(parser, &if_expr.children[0]));
  FR_TRY(fr_parse_bracket_block(parser, &if_expr.children[1]));
  if_expr.child = if_expr.children[0];
  if_expr.child_count = 2;

  if (parser->token.kind == FR_TOKEN_NAME &&
      fr_parse_span_equals(parser->token.span, "else")) {
    FR_TRY(fr_parse_advance(parser));
    FR_TRY(fr_parse_bracket_block(parser, &if_expr.children[2]));
    if_expr.child_count = 3;
  }

  return fr_parse_add_expr(parser, if_expr, out_id);
}

static fr_err_t fr_parse_when(fr_parser_t *parser, fr_parse_expr_id_t *out_id) {
  fr_parse_expr_t when_expr = {.kind = FR_PARSE_EXPR_IF};

  FR_TRY(fr_parse_advance(parser));
  FR_TRY(fr_parse_expression(parser, &when_expr.children[0]));
  FR_TRY(fr_parse_bracket_block(parser, &when_expr.children[1]));
  if (parser->token.kind == FR_TOKEN_NAME &&
      fr_parse_span_equals(parser->token.span, "else")) {
    return FR_ERR_INVALID;
  }
  when_expr.child = when_expr.children[0];
  when_expr.child_count = 2;
  return fr_parse_add_expr(parser, when_expr, out_id);
}

/* `unless X [body]` rides the if emitter by parking the body in the else slot
 * and synthesizing a nil for the (unused) then arm. */
static fr_err_t fr_parse_unless(fr_parser_t *parser,
                                fr_parse_expr_id_t *out_id) {
  fr_parse_expr_t unless_expr = {.kind = FR_PARSE_EXPR_IF};
  fr_parse_expr_id_t nil_id = 0;

  FR_TRY(fr_parse_advance(parser));
  FR_TRY(fr_parse_expression(parser, &unless_expr.children[0]));
  FR_TRY(fr_parse_add_expr(
      parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_NIL}, &nil_id));
  FR_TRY(fr_parse_bracket_block(parser, &unless_expr.children[2]));
  if (parser->token.kind == FR_TOKEN_NAME &&
      fr_parse_span_equals(parser->token.span, "else")) {
    return FR_ERR_INVALID;
  }
  unless_expr.children[1] = nil_id;
  unless_expr.child = unless_expr.children[0];
  unless_expr.child_count = 3;
  return fr_parse_add_expr(parser, unless_expr, out_id);
}

static fr_err_t fr_parse_repeat(fr_parser_t *parser,
                                fr_parse_expr_id_t *out_id) {
  fr_parse_expr_t repeat = {.kind = FR_PARSE_EXPR_REPEAT};

  FR_TRY(fr_parse_advance(parser));
  FR_TRY(fr_parse_expression(parser, &repeat.children[0]));
  FR_TRY(fr_parse_bracket_block(parser, &repeat.children[1]));
  repeat.child = repeat.children[0];
  repeat.child_count = 2;
  return fr_parse_add_expr(parser, repeat, out_id);
}

static fr_err_t fr_parse_while(fr_parser_t *parser,
                               fr_parse_expr_id_t *out_id) {
  fr_parse_expr_t while_expr = {.kind = FR_PARSE_EXPR_WHILE};

  FR_TRY(fr_parse_advance(parser));
  FR_TRY(fr_parse_expression(parser, &while_expr.children[0]));
  FR_TRY(fr_parse_bracket_block(parser, &while_expr.children[1]));
  while_expr.child = while_expr.children[0];
  while_expr.child_count = 2;
  return fr_parse_add_expr(parser, while_expr, out_id);
}

static fr_err_t fr_parse_forever(fr_parser_t *parser,
                                 fr_parse_expr_id_t *out_id) {
  fr_parse_expr_t forever = {.kind = FR_PARSE_EXPR_FOREVER};

  FR_TRY(fr_parse_advance(parser));
  FR_TRY(fr_parse_bracket_block(parser, &forever.children[0]));
  forever.child = forever.children[0];
  forever.child_count = 1;
  return fr_parse_add_expr(parser, forever, out_id);
}

static bool fr_parse_compare_token_kind(fr_token_kind_t kind,
                                        fr_parse_expr_kind_t *out_expr_kind) {
  switch (kind) {
  case FR_TOKEN_LT: *out_expr_kind = FR_PARSE_EXPR_LT; return true;
  case FR_TOKEN_GT: *out_expr_kind = FR_PARSE_EXPR_GT; return true;
  case FR_TOKEN_LE: *out_expr_kind = FR_PARSE_EXPR_LE; return true;
  case FR_TOKEN_GE: *out_expr_kind = FR_PARSE_EXPR_GE; return true;
  case FR_TOKEN_EQ: *out_expr_kind = FR_PARSE_EXPR_EQ; return true;
  case FR_TOKEN_NE: *out_expr_kind = FR_PARSE_EXPR_NE; return true;
  default: return false;
  }
}

static fr_err_t fr_parse_multiplicative(fr_parser_t *parser,
                                        fr_parse_expr_id_t *out_id) {
  fr_parse_expr_id_t lhs = 0;

  FR_TRY(fr_parse_expression_inner(parser, &lhs));
  while (parser->token.kind == FR_TOKEN_STAR ||
         parser->token.kind == FR_TOKEN_SLASH) {
    fr_parse_expr_kind_t op_kind = parser->token.kind == FR_TOKEN_STAR
                                       ? FR_PARSE_EXPR_MUL
                                       : FR_PARSE_EXPR_DIV;
    fr_parse_expr_id_t rhs = 0;
    fr_parse_expr_t binop = {.kind = op_kind, .child_count = 2};

    FR_TRY(fr_parse_advance(parser));
    FR_TRY(fr_parse_expression_inner(parser, &rhs));
    binop.children[0] = lhs;
    binop.children[1] = rhs;
    binop.child = lhs;
    FR_TRY(fr_parse_add_expr(parser, binop, &lhs));
  }
  *out_id = lhs;
  return FR_OK;
}

static fr_err_t fr_parse_additive(fr_parser_t *parser,
                                  fr_parse_expr_id_t *out_id) {
  fr_parse_expr_id_t lhs = 0;

  FR_TRY(fr_parse_multiplicative(parser, &lhs));
  while (parser->token.kind == FR_TOKEN_MINUS) {
    fr_parse_expr_id_t rhs = 0;
    fr_parse_expr_t binop = {.kind = FR_PARSE_EXPR_SUB, .child_count = 2};

    FR_TRY(fr_parse_advance(parser));
    FR_TRY(fr_parse_multiplicative(parser, &rhs));
    binop.children[0] = lhs;
    binop.children[1] = rhs;
    binop.child = lhs;
    FR_TRY(fr_parse_add_expr(parser, binop, &lhs));
  }
  *out_id = lhs;
  return FR_OK;
}

static fr_err_t fr_parse_comparison(fr_parser_t *parser,
                                    fr_parse_expr_id_t *out_id) {
  fr_parse_expr_id_t lhs = 0;
  fr_parse_expr_kind_t op_kind = FR_PARSE_EXPR_LT;

  FR_TRY(fr_parse_additive(parser, &lhs));
  while (fr_parse_compare_token_kind(parser->token.kind, &op_kind)) {
    fr_parse_expr_id_t rhs = 0;
    fr_parse_expr_t binop = {.kind = op_kind, .child_count = 2};

    FR_TRY(fr_parse_advance(parser));
    FR_TRY(fr_parse_additive(parser, &rhs));
    binop.children[0] = lhs;
    binop.children[1] = rhs;
    binop.child = lhs;
    FR_TRY(fr_parse_add_expr(parser, binop, &lhs));
  }
  *out_id = lhs;
  return FR_OK;
}

static fr_err_t fr_parse_expression(fr_parser_t *parser,
                                    fr_parse_expr_id_t *out_id) {
  fr_err_t err = FR_OK;

  if (parser == NULL) {
    return FR_ERR_INVALID;
  }
  if (parser->expr_depth >= FR_PARSE_MAX_EXPR_DEPTH) {
    return FR_ERR_OVERFLOW;
  }

  parser->expr_depth += 1;
  err = fr_parse_comparison(parser, out_id);
  parser->expr_depth -= 1;
  return err;
}

static fr_err_t fr_parse_expression_inner(fr_parser_t *parser,
                                          fr_parse_expr_id_t *out_id) {
  if (out_id == NULL) {
    return FR_ERR_INVALID;
  }

  if (parser->token.kind == FR_TOKEN_INT) {
    fr_int_t int_value = parser->token.int_value;

    FR_TRY(fr_parse_advance(parser));
    return fr_parse_add_expr(
        parser,
        (fr_parse_expr_t){.kind = FR_PARSE_EXPR_INT, .int_value = int_value},
        out_id);
  }

  if (parser->token.kind == FR_TOKEN_TEXT) {
    fr_parse_span_t text = parser->token.span;

    FR_TRY(fr_parse_advance(parser));
    return fr_parse_add_expr(
        parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_TEXT, .text = text},
        out_id);
  }

  if (parser->token.kind == FR_TOKEN_NAME) {
    if (fr_parse_span_equals(parser->token.span, "nil")) {
      FR_TRY(fr_parse_advance(parser));
      return fr_parse_add_expr(
          parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_NIL}, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "false")) {
      FR_TRY(fr_parse_advance(parser));
      return fr_parse_add_expr(
          parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_FALSE}, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "true")) {
      FR_TRY(fr_parse_advance(parser));
      return fr_parse_add_expr(
          parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_TRUE}, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "fn")) {
      return fr_parse_function(parser, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "if")) {
      return fr_parse_if(parser, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "when")) {
      return fr_parse_when(parser, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "unless")) {
      return fr_parse_unless(parser, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "repeat")) {
      return fr_parse_repeat(parser, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "while")) {
      return fr_parse_while(parser, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "forever")) {
      return fr_parse_forever(parser, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "cells")) {
#if !FR_FEATURE_CELLS
      return FR_ERR_UNSUPPORTED;
#else
      return fr_parse_cells(parser, out_id);
#endif
    }
    if (fr_parse_span_equals(parser->token.span, "set")) {
#if !FR_FEATURE_CELLS && !FR_FEATURE_RECORDS
      return FR_ERR_UNSUPPORTED;
#else
      return fr_parse_set(parser, out_id);
#endif
    }
    if (fr_parse_span_equals(parser->token.span, "is")) {
      return FR_ERR_INVALID;
    }
    return fr_parse_name_or_call(parser, out_id);
  }

  return FR_ERR_INVALID;
}

static fr_err_t fr_parse_statement_list(fr_parser_t *parser,
                                        fr_parse_expr_id_t *out_id) {
  fr_parse_expr_t list = {.kind = FR_PARSE_EXPR_LIST};

  if (parser == NULL || out_id == NULL) {
    return FR_ERR_INVALID;
  }
  if (parser->token.kind == FR_TOKEN_RBRACKET ||
      parser->token.kind == FR_TOKEN_EOF) {
    return FR_ERR_INVALID;
  }

  while (parser->token.kind != FR_TOKEN_EOF &&
         parser->token.kind != FR_TOKEN_RBRACKET) {
    if (list.child_count >= FR_PARSE_MAX_BODY_EXPRS) {
      return FR_ERR_CAPACITY;
    }
    FR_TRY(fr_parse_expression(parser, &list.children[list.child_count]));
    list.child_count += 1;
    if (parser->token.kind == FR_TOKEN_SEMICOLON) {
      FR_TRY(fr_parse_advance(parser));
      if (parser->token.kind == FR_TOKEN_RBRACKET) {
        break;
      }
      if (parser->token.kind == FR_TOKEN_EOF) {
        return FR_ERR_INVALID;
      }
    } else {
      break;
    }
  }

  return fr_parse_add_expr(parser, list, out_id);
}

fr_err_t fr_parse_expression_line(const char *source, fr_parse_line_t *out,
                                  fr_parse_expr_id_t *out_expr) {
  fr_parser_t parser = {0};

  if (source == NULL || out == NULL || out_expr == NULL) {
    return FR_ERR_INVALID;
  }

  *out = (fr_parse_line_t){0};
  parser.cursor = source;
  parser.out = out;

  FR_TRY(fr_parse_advance(&parser));
  FR_TRY(fr_parse_expression(&parser, out_expr));
  return fr_parse_finish_line(&parser);
}

fr_err_t fr_parse_line(const char *source, fr_parse_line_t *out) {
  fr_parser_t parser = {0};

  if (source == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }

  *out = (fr_parse_line_t){0};
  parser.cursor = source;
  parser.out = out;

  FR_TRY(fr_parse_advance(&parser));
  if (parser.token.kind == FR_TOKEN_NAME &&
      fr_parse_span_equals(parser.token.span, "record")) {
#if !FR_FEATURE_RECORDS
    return FR_ERR_UNSUPPORTED;
#else
    FR_TRY(fr_parse_advance(&parser));
    if (parser.token.kind != FR_TOKEN_NAME) {
      return FR_ERR_INVALID;
    }
    out->kind = FR_PARSE_LINE_RECORD_SHAPE;
    out->definition.name = parser.token.span;
    FR_TRY(fr_parse_check_field_name(out->definition.name));
    FR_TRY(fr_parse_advance(&parser));
    FR_TRY(fr_parse_expect(&parser, FR_TOKEN_LBRACKET));
    if (parser.token.kind == FR_TOKEN_RBRACKET) {
      return FR_ERR_RANGE;
    }
    while (parser.token.kind != FR_TOKEN_RBRACKET) {
      if (parser.token.kind != FR_TOKEN_NAME ||
          out->record_field_count >= FR_PARSE_MAX_RECORD_FIELDS) {
        return FR_ERR_INVALID;
      }
      FR_TRY(fr_parse_check_field_name(parser.token.span));
      for (uint8_t i = 0; i < out->record_field_count; i++) {
        if (fr_parse_span_same(out->record_fields[i], parser.token.span)) {
          return FR_ERR_INVALID;
        }
      }
      out->record_fields[out->record_field_count] = parser.token.span;
      out->record_field_count += 1;
      FR_TRY(fr_parse_advance(&parser));
      if (parser.token.kind == FR_TOKEN_COMMA) {
        FR_TRY(fr_parse_advance(&parser));
        if (parser.token.kind == FR_TOKEN_RBRACKET) {
          return FR_ERR_INVALID;
        }
      } else if (parser.token.kind != FR_TOKEN_RBRACKET) {
        return FR_ERR_INVALID;
      }
    }
    FR_TRY(fr_parse_expect(&parser, FR_TOKEN_RBRACKET));
    return fr_parse_finish_line(&parser);
#endif
  }
  if (parser.token.kind == FR_TOKEN_NAME &&
      fr_parse_span_equals(parser.token.span, "to")) {
    FR_TRY(fr_parse_advance(&parser));
    if (parser.token.kind != FR_TOKEN_NAME ||
        fr_parse_span_equals(parser.token.span, "is") ||
        fr_parse_span_equals(parser.token.span, "to") ||
        fr_parse_span_equals(parser.token.span, "with") ||
        fr_parse_span_equals(parser.token.span, "true") ||
        fr_parse_span_equals(parser.token.span, "false")) {
      return FR_ERR_INVALID;
    }
    out->definition.name = parser.token.span;
    FR_TRY(fr_parse_advance(&parser));
    FR_TRY(fr_parse_function_value(&parser, &out->definition.value));
    return fr_parse_finish_line(&parser);
  }
  if (parser.token.kind != FR_TOKEN_NAME ||
      fr_parse_span_equals(parser.token.span, "is") ||
      fr_parse_span_equals(parser.token.span, "true") ||
      fr_parse_span_equals(parser.token.span, "false")) {
    return FR_ERR_INVALID;
  }
  out->definition.name = parser.token.span;
  FR_TRY(fr_parse_advance(&parser));
  FR_TRY(fr_parse_expect_word(&parser, "is"));
  FR_TRY(fr_parse_expression(&parser, &out->definition.value));

  return fr_parse_finish_line(&parser);
}
