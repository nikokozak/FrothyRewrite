#include "parse.h"

#include "platform.h"
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
  FR_TOKEN_PLUS,
  FR_TOKEN_MINUS,
  FR_TOKEN_STAR,
  FR_TOKEN_SLASH,
  FR_TOKEN_PERCENT,
} fr_token_kind_t;

typedef struct fr_token_t {
  fr_token_kind_t kind;
  fr_parse_span_t span;
  fr_int_t int_value;
  bool leading_space;
  bool leading_newline;
  bool text_has_escapes;
} fr_token_t;

typedef struct fr_parser_t {
  const char *cursor;
  fr_token_t token;
  fr_parse_line_t *out;
  fr_diagnostic_t *diag;
  uint8_t expr_depth;
  uint8_t call_arg_stop_depth;
  bool stop_infix_before_call;
  bool stop_before_repeat_as;
  bool inside_rescue_block;
} fr_parser_t;

typedef uint32_t fr_parse_int_magnitude_t;

#define FR_PARSE_INT_POS_LIMIT                                                \
  ((fr_parse_int_magnitude_t)FR_TAGGED_INT_MAX)
#define FR_PARSE_INT_NEG_LIMIT                                                \
  ((fr_parse_int_magnitude_t)(FR_TAGGED_INT_MAX + 1u))

static void fr_parse_note_span(fr_parser_t *parser, uint16_t message_id,
                               fr_parse_span_t span) {
  if (parser == NULL || parser->diag == NULL ||
      parser->diag->kind != FR_DIAG_NONE || message_id == FR_DIAG_MSG_NONE) {
    return;
  }
  parser->diag->kind = FR_DIAG_TOKEN;
  parser->diag->message_id = message_id;
  parser->diag->span_start = span.start;
  parser->diag->span_length = span.length;
}

static fr_err_t fr_parse_fail_span(fr_parser_t *parser, uint16_t message_id,
                                   fr_parse_span_t span, fr_err_t err) {
  fr_parse_note_span(parser, message_id, span);
  return err;
}

static fr_err_t fr_parse_fail_token(fr_parser_t *parser, uint16_t message_id,
                                    fr_err_t err) {
  return fr_parse_fail_span(parser, message_id, parser->token.span, err);
}

static bool fr_parse_is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool fr_parse_is_digit(char c) { return c >= '0' && c <= '9'; }

#if FR_FEATURE_TEXT
static bool fr_parse_is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}
#endif

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

static bool fr_parse_comment_after(fr_token_kind_t kind) {
  return kind == FR_TOKEN_EOF || kind == FR_TOKEN_LBRACKET ||
         kind == FR_TOKEN_LPAREN || kind == FR_TOKEN_COLON ||
         kind == FR_TOKEN_COMMA || kind == FR_TOKEN_SEMICOLON ||
         kind == FR_TOKEN_ARROW;
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

static fr_err_t fr_parse_skip_space_and_comments(fr_parser_t *parser,
                                                 fr_token_kind_t prev_kind,
                                                 bool *out_skipped,
                                                 bool *out_crossed_newline) {
  bool skipped = false;
  bool crossed_newline = false;

  for (;;) {
    while (fr_parse_is_space(*parser->cursor)) {
      skipped = true;
      if (*parser->cursor == '\n' || *parser->cursor == '\r') {
        crossed_newline = true;
      }
      parser->cursor += 1;
    }

    if ((skipped || fr_parse_comment_after(prev_kind)) &&
        parser->cursor[0] == '-' && parser->cursor[1] == '-') {
      skipped = true;
      parser->cursor += 2;
      while (*parser->cursor != '\0' && *parser->cursor != '\n' &&
             *parser->cursor != '\r') {
        parser->cursor += 1;
      }
      continue;
    }

    if ((skipped || fr_parse_comment_after(prev_kind)) &&
        parser->cursor[0] == '-' && parser->cursor[1] == '*') {
      const char *comment_start = parser->cursor;

      skipped = true;
      parser->cursor += 2;
      while (parser->cursor[0] != '\0' &&
             !(parser->cursor[0] == '*' && parser->cursor[1] == '-')) {
        if (*parser->cursor == '\n' || *parser->cursor == '\r') {
          crossed_newline = true;
        }
        parser->cursor += 1;
      }
      if (parser->cursor[0] == '\0') {
        return fr_parse_fail_span(
            parser, FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN,
            (fr_parse_span_t){.start = comment_start, .length = 2},
            FR_ERR_INVALID);
      }
      parser->cursor += 2;
      continue;
    }

    break;
  }
  *out_skipped = skipped;
  *out_crossed_newline = crossed_newline;
  return FR_OK;
}

/* `1s` multiplies into milliseconds; `ms`, `us`, and `ns` pass through
 * unchanged so a literal can name the unit its API takes (each API
 * documents one). The suffix is a compile-time multiply into the same
 * int — there is no duration type. */
static bool fr_parse_duration_suffix(fr_parse_span_t span, uint16_t i,
                                     fr_parse_int_magnitude_t *out_multiplier,
                                     uint16_t *out_length) {
  uint16_t rest = (uint16_t)(span.length - i);

  if (rest == 1 && span.start[i] == 's') {
    *out_multiplier = 1000;
    *out_length = 1;
    return true;
  }
  if (rest == 2 && span.start[i] == 'm' && span.start[i + 1] == 's') {
    *out_multiplier = 1;
    *out_length = 2;
    return true;
  }
  if (rest == 2 && (span.start[i] == 'u' || span.start[i] == 'n') &&
      span.start[i + 1] == 's') {
    *out_multiplier = 1;
    *out_length = 2;
    return true;
  }
  return false;
}

static fr_err_t fr_parse_token_int(fr_parse_span_t span, fr_int_t *out_int) {
  bool negative = false;
  fr_parse_int_magnitude_t parsed = 0;
  fr_parse_int_magnitude_t limit = 0;
  fr_parse_int_magnitude_t cutoff = 0;
  fr_parse_int_magnitude_t multiplier = 1;
  bool seen_digit = false;
  uint8_t cutlim = 0;
  uint8_t base = 10;
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

  if (span.start[i] == '0' && i + 1 < span.length) {
    char prefix = span.start[i + 1];
    if (prefix == 'x' || prefix == 'X') {
      base = 16;
      i += 2;
    } else if (prefix == 'b' || prefix == 'B') {
      base = 2;
      i += 2;
    }
  }
  if (i == span.length) {
    /* Bare `0x` / `0b` with no digits — committed to int by the prefix, so
     * report a hard error rather than falling back to name. */
    return FR_ERR_INVALID;
  }

  limit = negative ? FR_PARSE_INT_NEG_LIMIT : FR_PARSE_INT_POS_LIMIT;
  cutoff = (fr_parse_int_magnitude_t)(limit / base);
  cutlim = (uint8_t)(limit % base);

  while (i < span.length) {
    char c = span.start[i];
    uint8_t digit = 0;

    /* `_` groups digits in any base: `1_000_000`, `0b1000_0000`. It must
     * sit between digits, so a malformed span falls back to being a name
     * the same way any other non-integer digit-led token does. */
    if (c == '_') {
      if (!seen_digit || span.start[i - 1] == '_' ||
          i + 1u >= span.length) {
        return base == 10 ? FR_ERR_UNSUPPORTED : FR_ERR_INVALID;
      }
      i += 1;
      continue;
    }
    if (base == 10) {
      if (!fr_parse_is_digit(c)) {
        uint16_t suffix_length = 0;

        if (seen_digit && span.start[i - 1] != '_' &&
            fr_parse_duration_suffix(span, i, &multiplier, &suffix_length)) {
          i = (uint16_t)(i + suffix_length);
          break;
        }
        return FR_ERR_UNSUPPORTED;
      }
      digit = (uint8_t)(c - '0');
    } else if (base == 16) {
      if (c >= '0' && c <= '9') {
        digit = (uint8_t)(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = (uint8_t)(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = (uint8_t)(c - 'A' + 10);
      } else {
        return FR_ERR_INVALID;
      }
    } else {
      if (c == '0' || c == '1') {
        digit = (uint8_t)(c - '0');
      } else {
        return FR_ERR_INVALID;
      }
    }

    if (parsed > cutoff || (parsed == cutoff && digit > cutlim)) {
      return FR_ERR_RANGE;
    }
    parsed = (fr_parse_int_magnitude_t)((parsed * base) + digit);
    seen_digit = true;
    i += 1;
  }

  if (multiplier > 1) {
    if (parsed > limit / multiplier) {
      return FR_ERR_RANGE;
    }
    parsed = (fr_parse_int_magnitude_t)(parsed * multiplier);
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
  bool leading_newline = false;
  fr_token_kind_t prev_kind = parser->token.kind;

  parser->token.text_has_escapes = false;
  FR_TRY(fr_parse_skip_space_and_comments(parser, prev_kind, &leading_space,
                                          &leading_newline));
  c = *parser->cursor;
  span.start = parser->cursor;

  if (c == '\0') {
    parser->token = (fr_token_t){.kind = FR_TOKEN_EOF,
                                .span = span,
                                .leading_space = leading_space,
                                .leading_newline = leading_newline};
    return FR_OK;
  }

  if (fr_parse_is_arrow_start(parser->cursor)) {
    parser->cursor += 2;
    span.length = 2;
    parser->token = (fr_token_t){.kind = FR_TOKEN_ARROW,
                                .span = span,
                                .leading_space = leading_space,
                                .leading_newline = leading_newline};
    return FR_OK;
  }

  if (c == '"') {
#if !FR_FEATURE_TEXT
    return fr_parse_fail_span(parser, FR_DIAG_MSG_PARSE_TEXT_DISABLED,
                              (fr_parse_span_t){.start = span.start,
                                                .length = 1},
                              FR_ERR_UNSUPPORTED);
#else
    bool has_escape = false;
    uint16_t decoded_length = 0;
    const char *literal_start = span.start;

    parser->cursor += 1;
    span.start = parser->cursor;
    while (*parser->cursor != '\0' && *parser->cursor != '"') {
      if (*parser->cursor == '\n' || *parser->cursor == '\r') {
        return fr_parse_fail_span(
            parser, FR_DIAG_MSG_PARSE_UNTERMINATED_TEXT,
            (fr_parse_span_t){.start = literal_start, .length = 1},
            FR_ERR_INVALID);
      }
      if (decoded_length >= FR_PROFILE_MAX_TEXT_LENGTH) {
        return fr_parse_fail_span(
            parser, FR_DIAG_MSG_PARSE_TEXT_TOO_LONG,
            (fr_parse_span_t){.start = literal_start, .length = 1},
            FR_ERR_RANGE);
      }
      if (*parser->cursor == '\\') {
        char escape = '\0';
        const char *escape_start = parser->cursor;

        has_escape = true;
        parser->cursor += 1;
        escape = *parser->cursor;
        if (escape == 'n' || escape == 'r' || escape == 't' ||
            escape == '"' || escape == '\\') {
          parser->cursor += 1;
        } else if (escape == 'x') {
          if (parser->cursor[1] == '\0' || parser->cursor[2] == '\0' ||
              !fr_parse_is_hex_digit(parser->cursor[1]) ||
              !fr_parse_is_hex_digit(parser->cursor[2])) {
            return fr_parse_fail_span(
                parser, FR_DIAG_MSG_PARSE_BAD_ESCAPE,
                (fr_parse_span_t){.start = escape_start, .length = 2},
                FR_ERR_INVALID);
          }
          parser->cursor += 3;
        } else {
          return fr_parse_fail_span(
              parser, FR_DIAG_MSG_PARSE_BAD_ESCAPE,
              (fr_parse_span_t){.start = escape_start, .length = 2},
              FR_ERR_INVALID);
        }
      } else {
        parser->cursor += 1;
      }
      decoded_length += 1;
    }
    if (*parser->cursor != '"') {
      return fr_parse_fail_span(
          parser, FR_DIAG_MSG_PARSE_UNTERMINATED_TEXT,
          (fr_parse_span_t){.start = literal_start, .length = 1},
          FR_ERR_INVALID);
    }
    if ((uint32_t)(parser->cursor - span.start) > UINT16_MAX) {
      return fr_parse_fail_span(
          parser, FR_DIAG_MSG_PARSE_TEXT_TOO_LONG,
          (fr_parse_span_t){.start = literal_start, .length = 1},
          FR_ERR_RANGE);
    }
    span.length = (uint16_t)(parser->cursor - span.start);
    parser->cursor += 1;
    parser->token = (fr_token_t){
        .kind = FR_TOKEN_TEXT,
        .span = span,
        .leading_space = leading_space,
        .leading_newline = leading_newline,
        .text_has_escapes = has_escape};
    return FR_OK;
#endif
  }

  if (fr_parse_is_compare_op_char(c)) {
    parser->cursor += 1;
    span.length = 1;
    parser->token.span = span;
    parser->token.int_value = 0;
    parser->token.leading_space = leading_space;
    parser->token.leading_newline = leading_newline;
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

  /* `+` is always infix — Frothy has no `+5` positive-literal form, so the
   * `-` special-case logic doesn't apply here. */
  if (c == '+') {
    parser->cursor += 1;
    span.length = 1;
    parser->token.span = span;
    parser->token.int_value = 0;
    parser->token.leading_space = leading_space;
    parser->token.leading_newline = leading_newline;
    parser->token.kind = FR_TOKEN_PLUS;
    return FR_OK;
  }

  if (c == '*' || c == '/' || c == '%' ||
      (c == '-' &&
       (!fr_parse_is_digit(parser->cursor[1]) ||
        (!leading_space && fr_parse_minus_infix_after(prev_kind))))) {
    parser->cursor += 1;
    span.length = 1;
    parser->token.span = span;
    parser->token.int_value = 0;
    parser->token.leading_space = leading_space;
    parser->token.leading_newline = leading_newline;
    parser->token.kind = (c == '*')   ? FR_TOKEN_STAR
                         : (c == '/') ? FR_TOKEN_SLASH
                         : (c == '%') ? FR_TOKEN_PERCENT
                                      : FR_TOKEN_MINUS;
    return FR_OK;
  }

  if (fr_parse_is_punctuation(c)) {
    parser->cursor += 1;
    span.length = 1;
    parser->token.span = span;
    parser->token.int_value = 0;
    parser->token.leading_space = leading_space;
    parser->token.leading_newline = leading_newline;
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
   * `x - y` with spaces. `+` always stops the name — no identifier uses
   * it, and Frothy has no positive-literal form to confuse it with. */
  while (*parser->cursor != '\0' && !fr_parse_is_space(*parser->cursor) &&
         !fr_parse_is_punctuation(*parser->cursor) &&
         !fr_parse_is_compare_op_char(*parser->cursor) &&
         *parser->cursor != '*' && *parser->cursor != '/' &&
         *parser->cursor != '%' && *parser->cursor != '+' &&
         !fr_parse_is_arrow_start(parser->cursor) &&
         !(*parser->cursor == '-' && span.length > 0 &&
           (fr_parse_is_digit(span.start[0]) ||
            fr_parse_is_digit(parser->cursor[1])))) {
    if (span.length >= FR_PARSE_MAX_TOKEN_BYTES) {
      return fr_parse_fail_span(parser, FR_DIAG_MSG_PARSE_TOKEN_TOO_LONG, span,
                                FR_ERR_RANGE);
    }
    span.length += 1;
    parser->cursor += 1;
  }

  parser->token = (fr_token_t){.kind = FR_TOKEN_NAME,
                              .span = span,
                              .leading_space = leading_space,
                              .leading_newline = leading_newline};
  if (fr_parse_span_looks_int(span)) {
    fr_err_t err = fr_parse_token_int(span, &parser->token.int_value);
    if (err == FR_OK) {
      parser->token.kind = FR_TOKEN_INT;
    } else if (err != FR_ERR_UNSUPPORTED) {
      if (err == FR_ERR_RANGE) {
        fr_parse_note_span(parser, FR_DIAG_MSG_PARSE_INTEGER_RANGE, span);
      } else {
        fr_parse_note_span(parser, FR_DIAG_MSG_PARSE_BAD_INTEGER, span);
      }
      return err;
    }
  }
  return FR_OK;
}

static fr_err_t fr_parse_advance(fr_parser_t *parser) {
  return fr_parse_read_token(parser);
}

static uint16_t fr_parse_expected_message(fr_token_kind_t kind) {
  switch (kind) {
  case FR_TOKEN_LBRACKET:
    return FR_DIAG_MSG_PARSE_EXPECTED_BLOCK_START;
  case FR_TOKEN_RBRACKET:
    return FR_DIAG_MSG_PARSE_EXPECTED_BLOCK_END;
  case FR_TOKEN_RPAREN:
    return FR_DIAG_MSG_PARSE_EXPECTED_GROUP_END;
  default:
    return FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN;
  }
}

static fr_err_t fr_parse_expect(fr_parser_t *parser, fr_token_kind_t kind) {
  if (parser->token.kind != kind) {
    return fr_parse_fail_token(parser, fr_parse_expected_message(kind),
                               FR_ERR_INVALID);
  }
  return fr_parse_advance(parser);
}

static fr_err_t fr_parse_expect_word(fr_parser_t *parser, const char *word) {
  if (parser->token.kind != FR_TOKEN_NAME ||
      !fr_parse_span_equals(parser->token.span, word)) {
    uint16_t message_id = FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN;

    if (word != NULL && word[0] == 'i' && word[1] == 's' &&
        word[2] == '\0') {
      message_id = FR_DIAG_MSG_PARSE_EXPECTED_IS;
    } else if (word != NULL && word[0] == 't' && word[1] == 'o' &&
               word[2] == '\0') {
      message_id = FR_DIAG_MSG_PARSE_EXPECTED_TO;
    }
    return fr_parse_fail_token(parser, message_id, FR_ERR_INVALID);
  }
  return fr_parse_advance(parser);
}

static bool fr_parse_infix_rhs_starts_call(const fr_parser_t *parser) {
  fr_parser_t lookahead;

  if (parser == NULL) {
    return false;
  }
  lookahead = *parser;
  lookahead.diag = NULL;
  if (fr_parse_advance(&lookahead) != FR_OK ||
      lookahead.token.kind != FR_TOKEN_NAME) {
    return false;
  }
  if (fr_parse_advance(&lookahead) != FR_OK) {
    return false;
  }
  return lookahead.token.kind == FR_TOKEN_COLON;
}

/* In a call argument, an infix operator whose right side starts another
 * call ends the argument: a call's arguments end where the next call
 * begins. The rule is the same for every operator; parentheses opt back
 * in by raising expr_depth. */
static bool fr_parse_call_arg_ends_before_call(const fr_parser_t *parser) {
  return parser->stop_infix_before_call &&
         parser->expr_depth == parser->call_arg_stop_depth &&
         fr_parse_infix_rhs_starts_call(parser);
}

static fr_err_t fr_parse_finish_line(fr_parser_t *parser) {
  if (parser->token.kind == FR_TOKEN_SEMICOLON) {
    FR_TRY(fr_parse_advance(parser));
  }
  if (parser->token.kind != FR_TOKEN_EOF) {
    if (parser->token.kind == FR_TOKEN_RBRACKET) {
      return fr_parse_fail_token(parser,
                                 FR_DIAG_MSG_PARSE_UNEXPECTED_BLOCK_END,
                                 FR_ERR_INVALID);
    }
    if (parser->token.kind == FR_TOKEN_RPAREN) {
      return fr_parse_fail_token(parser,
                                 FR_DIAG_MSG_PARSE_UNEXPECTED_GROUP_END,
                                 FR_ERR_INVALID);
    }
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN,
                               FR_ERR_INVALID);
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

static bool fr_parse_is_event_keyword(fr_parse_span_t name) {
  return fr_parse_span_equals(name, "on") ||
         fr_parse_span_equals(name, "every") ||
         fr_parse_span_equals(name, "after") ||
         fr_parse_span_equals(name, "cancel") ||
         fr_parse_span_equals(name, "rising") ||
         fr_parse_span_equals(name, "falling") ||
         fr_parse_span_equals(name, "changes") ||
         fr_parse_span_equals(name, "debounce");
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
         fr_parse_span_equals(name, "and") ||
         fr_parse_span_equals(name, "or") ||
         fr_parse_span_equals(name, "not") ||
         fr_parse_span_equals(name, "when") ||
         fr_parse_span_equals(name, "unless") ||
         fr_parse_span_equals(name, "repeat") ||
         fr_parse_span_equals(name, "while") ||
         fr_parse_span_equals(name, "forever") ||
         fr_parse_span_equals(name, "cells") ||
         fr_parse_span_equals(name, "record") ||
         fr_parse_span_equals(name, "set") ||
         fr_parse_span_equals(name, "to") ||
         fr_parse_span_equals(name, "here") ||
         fr_parse_is_event_keyword(name);
}

static bool fr_parse_is_reserved_is_definition_name(fr_parse_span_t name) {
  return fr_parse_span_equals(name, "is") ||
         fr_parse_span_equals(name, "true") ||
         fr_parse_span_equals(name, "false") ||
         fr_parse_span_equals(name, "and") ||
         fr_parse_span_equals(name, "or") ||
         fr_parse_span_equals(name, "not") ||
         fr_parse_is_event_keyword(name);
}

static bool fr_parse_token_starts_word_argument(const fr_token_t *token) {
  if (token == NULL) {
    return false;
  }
  if (token->kind == FR_TOKEN_NAME) {
    return !fr_parse_is_reserved_parameter(token->span);
  }
  return token->kind == FR_TOKEN_INT || token->kind == FR_TOKEN_TEXT ||
         token->kind == FR_TOKEN_LPAREN;
}

static fr_parse_span_t fr_parse_word_argument_diagnostic_span(
    const fr_token_t *token) {
  if (token != NULL && token->kind == FR_TOKEN_TEXT &&
      token->span.start != NULL) {
    return (fr_parse_span_t){.start = token->span.start - 1, .length = 1};
  }
  return token != NULL ? token->span : (fr_parse_span_t){0};
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

static fr_err_t fr_parse_check_field_name_with_diagnostic(
    fr_parser_t *parser, fr_parse_span_t name) {
  fr_err_t err = fr_parse_check_field_name(name);

  if (err == FR_ERR_RANGE) {
    fr_parse_note_span(parser, FR_DIAG_MSG_PARSE_TOKEN_TOO_LONG, name);
  } else if (err == FR_ERR_INVALID) {
    fr_parse_note_span(parser, FR_DIAG_MSG_PARSE_BAD_FIELD, name);
  }
  return err;
}
#endif

static fr_err_t fr_parse_add_param(fr_parser_t *parser, fr_parse_span_t name,
                                   uint8_t param_start) {
  if (parser == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_parse_is_reserved_parameter(name)) {
    return fr_parse_fail_span(parser, FR_DIAG_MSG_PARSE_RESERVED_PARAMETER,
                              name, FR_ERR_INVALID);
  }
  if (parser->out->param_count >= FR_PARSE_MAX_PARAMS) {
    return FR_ERR_CAPACITY;
  }
  for (uint8_t i = param_start; i < parser->out->param_count; i++) {
    if (fr_parse_span_same(parser->out->params[i], name)) {
      return fr_parse_fail_span(parser, FR_DIAG_MSG_PARSE_DUPLICATE_PARAMETER,
                                name, FR_ERR_INVALID);
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

  if (parser->token.kind != FR_TOKEN_ARROW ||
      parser->token.leading_newline) {
    *out_id = base_id;
    return FR_OK;
  }
#if !FR_FEATURE_RECORDS
  (void)field;
  return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_RECORDS_DISABLED,
                             FR_ERR_UNSUPPORTED);
#else
  FR_TRY(fr_parse_advance(parser));
  if (parser->token.kind != FR_TOKEN_NAME) {
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_EXPECTED_FIELD,
                               FR_ERR_INVALID);
  }
  field = parser->token.span;
  FR_TRY(fr_parse_check_field_name_with_diagnostic(parser, field));
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

static fr_err_t fr_parse_detached_bracket_block(fr_parser_t *parser,
                                                fr_parse_expr_id_t *out_id) {
  bool saved_inside_rescue_block = parser->inside_rescue_block;
  fr_err_t err = FR_OK;

  parser->inside_rescue_block = false;
  err = fr_parse_bracket_block(parser, out_id);
  parser->inside_rescue_block = saved_inside_rescue_block;
  return err;
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
        return fr_parse_fail_token(parser,
                                   FR_DIAG_MSG_PARSE_EXPECTED_PARAMETER,
                                   FR_ERR_INVALID);
      }
      FR_TRY(fr_parse_add_param(parser, parser->token.span, param_start));
      param_count += 1;
      FR_TRY(fr_parse_advance(parser));

      if (parser->token.kind != FR_TOKEN_COMMA) {
        break;
      }
      FR_TRY(fr_parse_advance(parser));
      if (parser->token.kind == FR_TOKEN_LBRACKET) {
        return fr_parse_fail_token(parser,
                                   FR_DIAG_MSG_PARSE_EXPECTED_PARAMETER,
                                   FR_ERR_INVALID);
      }
    }
  }
  FR_TRY(fr_parse_detached_bracket_block(parser, &body));

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

static fr_err_t fr_parse_call_argument(fr_parser_t *parser,
                                       fr_parse_expr_id_t *out_id) {
  bool saved_stop_infix_before_call = false;
  uint8_t saved_call_arg_stop_depth = 0;
  fr_err_t err = FR_OK;

  if (parser == NULL || out_id == NULL) {
    return FR_ERR_INVALID;
  }

  saved_stop_infix_before_call = parser->stop_infix_before_call;
  saved_call_arg_stop_depth = parser->call_arg_stop_depth;
  parser->stop_infix_before_call = true;
  parser->call_arg_stop_depth = (uint8_t)(parser->expr_depth + 1u);
  err = fr_parse_expression(parser, out_id);
  parser->stop_infix_before_call = saved_stop_infix_before_call;
  parser->call_arg_stop_depth = saved_call_arg_stop_depth;
  return err;
}

static fr_err_t fr_parse_name_or_call(fr_parser_t *parser,
                                      fr_parse_expr_id_t *out_id) {
  fr_parse_span_t name = parser->token.span;
  fr_parse_expr_id_t base_id = 0;

  FR_TRY(fr_parse_advance(parser));
  if (parser->token.kind == FR_TOKEN_LBRACKET &&
      !parser->token.leading_space) {
#if !FR_FEATURE_CELLS
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_CELLS_DISABLED,
                               FR_ERR_UNSUPPORTED);
#else
    fr_parse_expr_id_t index = 0;

    FR_TRY(fr_parse_advance(parser));
    FR_TRY(fr_parse_expression(parser, &index));
    FR_TRY(fr_parse_expect(parser, FR_TOKEN_RBRACKET));
    FR_TRY(fr_parse_add_expr(parser,
                             (fr_parse_expr_t){.kind = FR_PARSE_EXPR_CELL_READ,
                                               .name = name,
                                               .child = index,
                                               .children = {index},
                                               .child_count = 1},
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
           parser->token.kind != FR_TOKEN_SEMICOLON &&
           !(call.child_count == 0 && parser->token.leading_newline)) {
      if (parser->stop_before_repeat_as &&
          fr_parse_span_equals(parser->token.span, "as")) {
        break;
      }
      if (parser->token.kind == FR_TOKEN_COMMA ||
          call.child_count >= FR_PARSE_MAX_BODY_EXPRS) {
        return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN,
                                   FR_ERR_INVALID);
      }

      FR_TRY(fr_parse_call_argument(parser, &call.children[call.child_count]));
      if (call.child_count == 0) {
        call.child = call.children[0];
      }
      call.child_count += 1;

      if (parser->token.kind == FR_TOKEN_COMMA) {
        FR_TRY(fr_parse_advance(parser));
        if (parser->token.kind == FR_TOKEN_EOF ||
            parser->token.kind == FR_TOKEN_RBRACKET ||
            parser->token.kind == FR_TOKEN_SEMICOLON) {
          return fr_parse_fail_token(parser,
                                     FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN,
                                     FR_ERR_INVALID);
        }
      } else {
        break;
      }
    }
    FR_TRY(fr_parse_add_expr(parser, call, &base_id));
    return fr_parse_field_postfix(parser, base_id, out_id);
  }

  if (parser->token.leading_space && parser->stop_before_repeat_as &&
      fr_parse_span_equals(parser->token.span, "as")) {
    FR_TRY(fr_parse_add_expr(
        parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_NAME, .name = name},
        &base_id));
    return fr_parse_field_postfix(parser, base_id, out_id);
  }

  if (parser->token.leading_space && !parser->token.leading_newline &&
      fr_parse_token_starts_word_argument(&parser->token)) {
    return fr_parse_fail_span(
        parser, FR_DIAG_MSG_PARSE_EXPECTED_COLON_BEFORE_ARGUMENT,
        fr_parse_word_argument_diagnostic_span(&parser->token), FR_ERR_INVALID);
  }

  FR_TRY(fr_parse_add_expr(
      parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_NAME, .name = name},
      &base_id));
  return fr_parse_field_postfix(parser, base_id, out_id);
}

#if FR_FEATURE_CELLS
/* `cells: 3` asks for a fixed row of cells. The length stays a literal
 * whole number because the row is reserved in the image at definition
 * time, not allocated at runtime. */
static fr_err_t fr_parse_cells(fr_parser_t *parser,
                               fr_parse_expr_id_t *out_id) {
  fr_int_t length = 0;

  FR_TRY(fr_parse_advance(parser));
  if (parser->token.kind != FR_TOKEN_COLON) {
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_CELLS_COLON,
                               FR_ERR_INVALID);
  }
  FR_TRY(fr_parse_advance(parser));
  if (parser->token.kind != FR_TOKEN_INT) {
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN,
                               FR_ERR_INVALID);
  }
  length = parser->token.int_value;
  if (length <= 0) {
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_INTEGER_RANGE,
                               FR_ERR_RANGE);
  }
  FR_TRY(fr_parse_advance(parser));
  return fr_parse_add_expr(parser,
                           (fr_parse_expr_t){.kind = FR_PARSE_EXPR_CELLS,
                                             .int_value = length},
                           out_id);
}
#endif

static fr_err_t fr_parse_set(fr_parser_t *parser, fr_parse_expr_id_t *out_id) {
  const fr_parse_expr_t *target = NULL;
  fr_parse_expr_id_t target_id = 0;
  fr_parse_expr_id_t value = 0;

  FR_TRY(fr_parse_advance(parser));
  FR_TRY(fr_parse_expression(parser, &target_id));
  FR_TRY(fr_parse_expect_word(parser, "to"));
  FR_TRY(fr_parse_expression(parser, &value));
  if (target_id >= parser->out->expr_count) {
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN,
                               FR_ERR_INVALID);
  }
  target = &parser->out->exprs[target_id];
  if (target->kind == FR_PARSE_EXPR_CELL_READ) {
#if !FR_FEATURE_CELLS
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_CELLS_DISABLED,
                               FR_ERR_UNSUPPORTED);
#else
    return fr_parse_add_expr(
        parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_CELL_WRITE,
                                  .name = target->name,
                                  .child = target->child,
                                  .children = {target->child, value},
                                  .child_count = 2},
        out_id);
#endif
  }
  if (target->kind == FR_PARSE_EXPR_FIELD_READ) {
#if !FR_FEATURE_RECORDS
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_RECORDS_DISABLED,
                               FR_ERR_UNSUPPORTED);
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
  /* Scope belongs to compile, so a bare-name write stays neutral here:
   * compile resolves it as local, immutable parameter, then slot. */
  if (target->kind == FR_PARSE_EXPR_NAME) {
    return fr_parse_add_expr(
        parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_NAME_WRITE,
                                  .name = target->name,
                                  .child = value,
                                  .child_count = 1},
        out_id);
  }
  return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN,
                             FR_ERR_INVALID);
}

/* `name is value` in statement position declares a local. The current
 * token must be the name; the caller has already decided this statement
 * is a binding. `here name is value` spells the same statement with the
 * scope written out. */
static fr_err_t fr_parse_local_bind(fr_parser_t *parser,
                                    fr_parse_expr_id_t *out_id) {
  fr_parse_span_t name = {0};
  fr_parse_expr_id_t value = 0;

  if (parser->token.kind != FR_TOKEN_NAME ||
      fr_parse_is_reserved_parameter(parser->token.span)) {
    if (parser->token.kind == FR_TOKEN_NAME) {
      return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_RESERVED_NAME,
                                 FR_ERR_INVALID);
    }
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_EXPECTED_WORD_NAME,
                               FR_ERR_INVALID);
  }
  name = parser->token.span;
  FR_TRY(fr_parse_advance(parser));
  FR_TRY(fr_parse_expect_word(parser, "is"));
  FR_TRY(fr_parse_expression(parser, &value));
  return fr_parse_add_expr(
      parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_LOCAL_BIND,
                                .name = name,
                                .child = value,
                                .child_count = 1},
      out_id);
}

static fr_err_t fr_parse_here(fr_parser_t *parser,
                              fr_parse_expr_id_t *out_id) {
  FR_TRY(fr_parse_advance(parser));
  return fr_parse_local_bind(parser, out_id);
}

/* A statement that reads `name is ...` is a local declaration, not an
 * expression. Reserved names fall through so their own parses can give a
 * sharper error. */
static bool fr_parse_statement_starts_local_bind(fr_parser_t *parser) {
  fr_parser_t lookahead;

  if (parser->token.kind != FR_TOKEN_NAME ||
      fr_parse_is_reserved_parameter(parser->token.span)) {
    return false;
  }
  lookahead = *parser;
  lookahead.diag = NULL;
  if (fr_parse_advance(&lookahead) != FR_OK) {
    return false;
  }
  return lookahead.token.kind == FR_TOKEN_NAME &&
         fr_parse_span_equals(lookahead.token.span, "is");
}

static fr_err_t fr_parse_if(fr_parser_t *parser, fr_parse_expr_id_t *out_id) {
  fr_parse_expr_t if_expr = {.kind = FR_PARSE_EXPR_IF};

  FR_TRY(fr_parse_advance(parser));
  FR_TRY(fr_parse_expression(parser, &if_expr.children[0]));
  FR_TRY(fr_parse_bracket_block(parser, &if_expr.children[1]));
  if_expr.child = if_expr.children[0];
  if_expr.child_count = 2;

  /* `else if` chains into the same node as alternating cond/body pairs;
   * a plain `else [ ... ]` appends one final body. Recursive `else [ if ... ]`
   * still parses through the inner bracket block. */
  while (parser->token.kind == FR_TOKEN_NAME &&
         fr_parse_span_equals(parser->token.span, "else")) {
    FR_TRY(fr_parse_advance(parser));
    if (parser->token.kind == FR_TOKEN_NAME &&
        fr_parse_span_equals(parser->token.span, "if")) {
      if (if_expr.child_count + 2 > FR_PARSE_MAX_BODY_EXPRS) {
        return FR_ERR_CAPACITY;
      }
      FR_TRY(fr_parse_advance(parser));
      FR_TRY(fr_parse_expression(parser,
                                 &if_expr.children[if_expr.child_count]));
      FR_TRY(fr_parse_bracket_block(
          parser, &if_expr.children[if_expr.child_count + 1]));
      if_expr.child_count = (uint8_t)(if_expr.child_count + 2);
      continue;
    }
    if (if_expr.child_count + 1 > FR_PARSE_MAX_BODY_EXPRS) {
      return FR_ERR_CAPACITY;
    }
    FR_TRY(fr_parse_bracket_block(parser,
                                  &if_expr.children[if_expr.child_count]));
    if_expr.child_count = (uint8_t)(if_expr.child_count + 1);
    break;
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
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN,
                               FR_ERR_INVALID);
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
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN,
                               FR_ERR_INVALID);
  }
  unless_expr.children[1] = nil_id;
  unless_expr.child = unless_expr.children[0];
  unless_expr.child_count = 3;
  return fr_parse_add_expr(parser, unless_expr, out_id);
}

static fr_err_t fr_parse_repeat(fr_parser_t *parser,
                                fr_parse_expr_id_t *out_id) {
  fr_parse_expr_t repeat = {.kind = FR_PARSE_EXPR_REPEAT};
  bool saved_stop_before_repeat_as = false;
  fr_err_t err = FR_OK;

  FR_TRY(fr_parse_advance(parser));
  saved_stop_before_repeat_as = parser->stop_before_repeat_as;
  parser->stop_before_repeat_as = true;
  err = fr_parse_expression(parser, &repeat.children[0]);
  parser->stop_before_repeat_as = saved_stop_before_repeat_as;
  FR_TRY(err);
  if (parser->token.kind == FR_TOKEN_NAME &&
      fr_parse_span_equals(parser->token.span, "as")) {
    FR_TRY(fr_parse_advance(parser));
    if (parser->token.kind != FR_TOKEN_NAME ||
        fr_parse_is_reserved_parameter(parser->token.span)) {
      if (parser->token.kind == FR_TOKEN_NAME) {
        return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_RESERVED_NAME,
                                   FR_ERR_INVALID);
      }
      return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_EXPECTED_WORD_NAME,
                                 FR_ERR_INVALID);
    }
    repeat.name = parser->token.span;
    FR_TRY(fr_parse_advance(parser));
  }
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

static fr_err_t fr_parse_attempt(fr_parser_t *parser,
                                 fr_parse_expr_id_t *out_id) {
  fr_parse_expr_t attempt = {.kind = FR_PARSE_EXPR_ATTEMPT};
  bool saved_inside_rescue_block = false;
  fr_err_t err = FR_OK;

  FR_TRY(fr_parse_advance(parser));
  FR_TRY(fr_parse_bracket_block(parser, &attempt.children[0]));
  if (parser->token.kind != FR_TOKEN_NAME ||
      !fr_parse_span_equals(parser->token.span, "rescue")) {
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN,
                               FR_ERR_INVALID);
  }
  FR_TRY(fr_parse_advance(parser));
  saved_inside_rescue_block = parser->inside_rescue_block;
  parser->inside_rescue_block = true;
  err = fr_parse_bracket_block(parser, &attempt.children[1]);
  parser->inside_rescue_block = saved_inside_rescue_block;
  FR_TRY(err);

  attempt.child = attempt.children[0];
  attempt.child_count = 2;
  return fr_parse_add_expr(parser, attempt, out_id);
}

/* `every <ms-expr> [body]` and `after <ms-expr> [body]` share one shape:
 * (ms, body) with int_value carrying the matching fr_event_kind_t value.
 * The keyword string drives the dispatch. */
static fr_err_t fr_parse_timer_event(fr_parser_t *parser,
                                     fr_parse_expr_id_t *out_id) {
  fr_parse_expr_t reg = {.kind = FR_PARSE_EXPR_EVENT_REGISTER};

  reg.int_value = fr_parse_span_equals(parser->token.span, "every")
                      ? FR_EVENT_KIND_EVERY
                      : FR_EVENT_KIND_AFTER;
  FR_TRY(fr_parse_advance(parser));
  FR_TRY(fr_parse_expression(parser, &reg.children[0]));
  FR_TRY(fr_parse_detached_bracket_block(parser, &reg.children[1]));
  reg.child = reg.children[0];
  reg.child_count = 2;
  return fr_parse_add_expr(parser, reg, out_id);
}

/* `on <pin-expr> <edge> [debounce <ms-expr>] [body]` — child layout is
 * (pin, body) or (pin, body, debounce); int_value carries the GPIO edge
 * as the matching fr_event_kind_t value so the compile slice can lift it
 * straight into the registration call. `on wifi.disconnected [body]` and
 * `on wifi.reconnected [body]` (D19) carry the wifi kind in int_value and
 * use a single-child layout (body), no pin or debounce. */
static fr_err_t fr_parse_on(fr_parser_t *parser, fr_parse_expr_id_t *out_id) {
  fr_parse_expr_t reg = {.kind = FR_PARSE_EXPR_EVENT_REGISTER};
  fr_int_t edge = 0;

  FR_TRY(fr_parse_advance(parser));
  if (parser->token.kind == FR_TOKEN_NAME &&
      fr_parse_span_equals(parser->token.span, "wifi.disconnected")) {
    reg.int_value = FR_EVENT_KIND_WIFI_DISCONNECTED;
    FR_TRY(fr_parse_advance(parser));
    FR_TRY(fr_parse_detached_bracket_block(parser, &reg.children[0]));
    reg.child = reg.children[0];
    reg.child_count = 1;
    return fr_parse_add_expr(parser, reg, out_id);
  }
  if (parser->token.kind == FR_TOKEN_NAME &&
      fr_parse_span_equals(parser->token.span, "wifi.reconnected")) {
    reg.int_value = FR_EVENT_KIND_WIFI_RECONNECTED;
    FR_TRY(fr_parse_advance(parser));
    FR_TRY(fr_parse_detached_bracket_block(parser, &reg.children[0]));
    reg.child = reg.children[0];
    reg.child_count = 1;
    return fr_parse_add_expr(parser, reg, out_id);
  }
  FR_TRY(fr_parse_expression(parser, &reg.children[0]));
  if (parser->token.kind != FR_TOKEN_NAME) {
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_EXPECTED_EVENT_EDGE,
                               FR_ERR_INVALID);
  }
  if (fr_parse_span_equals(parser->token.span, "rising")) {
    edge = FR_EVENT_KIND_GPIO_RISING;
  } else if (fr_parse_span_equals(parser->token.span, "falling")) {
    edge = FR_EVENT_KIND_GPIO_FALLING;
  } else if (fr_parse_span_equals(parser->token.span, "changes")) {
    edge = FR_EVENT_KIND_GPIO_CHANGES;
  } else {
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_EXPECTED_EVENT_EDGE,
                               FR_ERR_INVALID);
  }
  FR_TRY(fr_parse_advance(parser));
  reg.int_value = edge;
  reg.child_count = 2;
  if (parser->token.kind == FR_TOKEN_NAME &&
      fr_parse_span_equals(parser->token.span, "debounce")) {
    FR_TRY(fr_parse_advance(parser));
    FR_TRY(fr_parse_expression(parser, &reg.children[2]));
    reg.child_count = 3;
  }
  FR_TRY(fr_parse_detached_bracket_block(parser, &reg.children[1]));
  reg.child = reg.children[0];
  return fr_parse_add_expr(parser, reg, out_id);
}

/* `cancel <pin-expr>` removes a GPIO binding by pin; `cancel every <ms-expr>`
 * and `cancel after <ms-expr>` remove timer bindings by (kind, ms). int_value
 * carries the matching fr_event_kind_t. The runtime matcher checks only "is
 * GPIO + same source" for GPIO cancels, so any GPIO kind works as the
 * sentinel; FR_EVENT_KIND_GPIO_CHANGES reads as "any edge on this pin." */
static fr_err_t fr_parse_cancel(fr_parser_t *parser,
                                fr_parse_expr_id_t *out_id) {
  fr_parse_expr_t reg = {.kind = FR_PARSE_EXPR_EVENT_CANCEL};

  FR_TRY(fr_parse_advance(parser));
  if (parser->token.kind == FR_TOKEN_NAME &&
      fr_parse_span_equals(parser->token.span, "wifi.disconnected")) {
    reg.int_value = FR_EVENT_KIND_WIFI_DISCONNECTED;
    FR_TRY(fr_parse_advance(parser));
    reg.child_count = 0;
    return fr_parse_add_expr(parser, reg, out_id);
  }
  if (parser->token.kind == FR_TOKEN_NAME &&
      fr_parse_span_equals(parser->token.span, "wifi.reconnected")) {
    reg.int_value = FR_EVENT_KIND_WIFI_RECONNECTED;
    FR_TRY(fr_parse_advance(parser));
    reg.child_count = 0;
    return fr_parse_add_expr(parser, reg, out_id);
  }
  if (parser->token.kind == FR_TOKEN_NAME &&
      fr_parse_span_equals(parser->token.span, "every")) {
    reg.int_value = FR_EVENT_KIND_EVERY;
    FR_TRY(fr_parse_advance(parser));
  } else if (parser->token.kind == FR_TOKEN_NAME &&
             fr_parse_span_equals(parser->token.span, "after")) {
    reg.int_value = FR_EVENT_KIND_AFTER;
    FR_TRY(fr_parse_advance(parser));
  } else {
    reg.int_value = FR_EVENT_KIND_GPIO_CHANGES;
  }
  FR_TRY(fr_parse_expression(parser, &reg.children[0]));
  reg.child = reg.children[0];
  reg.child_count = 1;
  return fr_parse_add_expr(parser, reg, out_id);
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

static bool fr_parse_name_token_is(fr_parser_t *parser, const char *word) {
  return parser->token.kind == FR_TOKEN_NAME &&
         fr_parse_span_equals(parser->token.span, word);
}

static bool fr_parse_name_followed_by_block(fr_parser_t *parser) {
  fr_parser_t lookahead;

  if (parser == NULL || parser->token.kind != FR_TOKEN_NAME) {
    return false;
  }
  lookahead = *parser;
  lookahead.diag = NULL;
  if (fr_parse_advance(&lookahead) != FR_OK) {
    return false;
  }
  return lookahead.token.kind == FR_TOKEN_LBRACKET;
}

static fr_err_t fr_parse_multiplicative(fr_parser_t *parser,
                                        fr_parse_expr_id_t *out_id) {
  fr_parse_expr_id_t lhs = 0;

  FR_TRY(fr_parse_expression_inner(parser, &lhs));
  while (parser->token.kind == FR_TOKEN_STAR ||
         parser->token.kind == FR_TOKEN_SLASH ||
         parser->token.kind == FR_TOKEN_PERCENT) {
    fr_parse_expr_kind_t op_kind = parser->token.kind == FR_TOKEN_STAR
                                       ? FR_PARSE_EXPR_MUL
                                   : parser->token.kind == FR_TOKEN_SLASH
                                       ? FR_PARSE_EXPR_DIV
                                       : FR_PARSE_EXPR_MOD;
    fr_parse_expr_id_t rhs = 0;
    fr_parse_expr_t binop = {.kind = op_kind, .child_count = 2};

    if (parser->token.leading_newline) {
      break;
    }
    if (fr_parse_call_arg_ends_before_call(parser)) {
      break;
    }
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
  while (parser->token.kind == FR_TOKEN_PLUS ||
         parser->token.kind == FR_TOKEN_MINUS) {
    fr_parse_expr_id_t rhs = 0;
    fr_parse_expr_t binop = {
        .kind = parser->token.kind == FR_TOKEN_PLUS ? FR_PARSE_EXPR_ADD
                                                    : FR_PARSE_EXPR_SUB,
        .child_count = 2};

    if (parser->token.leading_newline) {
      break;
    }
    if (fr_parse_call_arg_ends_before_call(parser)) {
      break;
    }
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

/* Comparison does not associate: `1 < 2 < 3` is a parse error that asks
 * for `and`, never a bool compared against an int. */
static fr_err_t fr_parse_comparison(fr_parser_t *parser,
                                    fr_parse_expr_id_t *out_id) {
  fr_parse_expr_id_t lhs = 0;
  fr_parse_expr_kind_t op_kind = FR_PARSE_EXPR_LT;

  FR_TRY(fr_parse_additive(parser, &lhs));
  if (fr_parse_compare_token_kind(parser->token.kind, &op_kind) &&
      !parser->token.leading_newline &&
      !fr_parse_call_arg_ends_before_call(parser)) {
    fr_parse_expr_id_t rhs = 0;
    fr_parse_expr_t binop = {.kind = op_kind, .child_count = 2};

    FR_TRY(fr_parse_advance(parser));
    FR_TRY(fr_parse_additive(parser, &rhs));
    binop.children[0] = lhs;
    binop.children[1] = rhs;
    binop.child = lhs;
    FR_TRY(fr_parse_add_expr(parser, binop, &lhs));
    if (fr_parse_compare_token_kind(parser->token.kind, &op_kind) &&
        !parser->token.leading_newline) {
      return fr_parse_fail_token(parser,
                                 FR_DIAG_MSG_PARSE_CHAINED_COMPARISON,
                                 FR_ERR_INVALID);
    }
  }
  *out_id = lhs;
  return FR_OK;
}

/* `not` sits between comparison and `and`: `not x = 1` negates the
 * comparison, and `not a and b` reads as `(not a) and b`. */
static fr_err_t fr_parse_not_level(fr_parser_t *parser,
                                   fr_parse_expr_id_t *out_id) {
  fr_parse_expr_id_t child = 0;

  if (fr_parse_name_token_is(parser, "not")) {
    FR_TRY(fr_parse_advance(parser));
    FR_TRY(fr_parse_not_level(parser, &child));
    return fr_parse_add_expr(
        parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_NOT,
                                  .child = child,
                                  .children = {child},
                                  .child_count = 1},
        out_id);
  }
  return fr_parse_comparison(parser, out_id);
}

static fr_err_t fr_parse_and(fr_parser_t *parser, fr_parse_expr_id_t *out_id) {
  fr_parse_expr_id_t lhs = 0;

  FR_TRY(fr_parse_not_level(parser, &lhs));
  while (fr_parse_name_token_is(parser, "and") &&
         !parser->token.leading_newline &&
         !fr_parse_call_arg_ends_before_call(parser)) {
    fr_parse_expr_id_t rhs = 0;
    fr_parse_expr_t binop = {.kind = FR_PARSE_EXPR_AND, .child_count = 2};

    FR_TRY(fr_parse_advance(parser));
    FR_TRY(fr_parse_not_level(parser, &rhs));
    binop.children[0] = lhs;
    binop.children[1] = rhs;
    binop.child = lhs;
    FR_TRY(fr_parse_add_expr(parser, binop, &lhs));
  }
  *out_id = lhs;
  return FR_OK;
}

static fr_err_t fr_parse_or(fr_parser_t *parser, fr_parse_expr_id_t *out_id) {
  fr_parse_expr_id_t lhs = 0;

  FR_TRY(fr_parse_and(parser, &lhs));
  while (fr_parse_name_token_is(parser, "or") &&
         !parser->token.leading_newline &&
         !fr_parse_call_arg_ends_before_call(parser)) {
    fr_parse_expr_id_t rhs = 0;
    fr_parse_expr_t binop = {.kind = FR_PARSE_EXPR_OR, .child_count = 2};

    FR_TRY(fr_parse_advance(parser));
    FR_TRY(fr_parse_and(parser, &rhs));
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
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_TOO_DEEP,
                               FR_ERR_OVERFLOW);
  }

  parser->expr_depth += 1;
  err = fr_parse_or(parser, out_id);
  parser->expr_depth -= 1;
  return err;
}

static fr_err_t fr_parse_expression_inner(fr_parser_t *parser,
                                          fr_parse_expr_id_t *out_id) {
  if (out_id == NULL) {
    return FR_ERR_INVALID;
  }

  if (parser->token.kind == FR_TOKEN_LPAREN) {
    FR_TRY(fr_parse_advance(parser));
    FR_TRY(fr_parse_expression(parser, out_id));
    return fr_parse_expect(parser, FR_TOKEN_RPAREN);
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
    bool text_has_escapes = parser->token.text_has_escapes;

    FR_TRY(fr_parse_advance(parser));
    return fr_parse_add_expr(
        parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_TEXT,
                                  .text = text,
                                  .text_has_escapes = text_has_escapes},
        out_id);
  }

  if (parser->token.kind == FR_TOKEN_NAME) {
    if (parser->inside_rescue_block &&
        fr_parse_span_equals(parser->token.span, "error.code")) {
      FR_TRY(fr_parse_advance(parser));
      return fr_parse_add_expr(
          parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_ERROR_CODE},
          out_id);
    }
    if (parser->inside_rescue_block &&
        fr_parse_span_equals(parser->token.span, "error.name")) {
      FR_TRY(fr_parse_advance(parser));
      return fr_parse_add_expr(
          parser, (fr_parse_expr_t){.kind = FR_PARSE_EXPR_ERROR_NAME},
          out_id);
    }
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
    if (fr_parse_span_equals(parser->token.span, "not")) {
      /* `not` lives between comparison and `and`; inside arithmetic it
       * needs parentheses, so reaching it here is an error. */
      return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN,
                                 FR_ERR_INVALID);
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
    if (fr_parse_span_equals(parser->token.span, "attempt") &&
        fr_parse_name_followed_by_block(parser)) {
      return fr_parse_attempt(parser, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "on")) {
      return fr_parse_on(parser, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "every") ||
        fr_parse_span_equals(parser->token.span, "after")) {
      return fr_parse_timer_event(parser, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "cancel")) {
      return fr_parse_cancel(parser, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "cells")) {
#if !FR_FEATURE_CELLS
      return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_CELLS_DISABLED,
                                 FR_ERR_UNSUPPORTED);
#else
      return fr_parse_cells(parser, out_id);
#endif
    }
    if (fr_parse_span_equals(parser->token.span, "set")) {
      return fr_parse_set(parser, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "here")) {
      return fr_parse_here(parser, out_id);
    }
    if (fr_parse_span_equals(parser->token.span, "is")) {
      return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN,
                                 FR_ERR_INVALID);
    }
    return fr_parse_name_or_call(parser, out_id);
  }

  if (parser->token.kind == FR_TOKEN_RBRACKET) {
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_UNEXPECTED_BLOCK_END,
                               FR_ERR_INVALID);
  }
  if (parser->token.kind == FR_TOKEN_RPAREN) {
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_UNEXPECTED_GROUP_END,
                               FR_ERR_INVALID);
  }
  return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN,
                             FR_ERR_INVALID);
}

static fr_err_t fr_parse_statement_list(fr_parser_t *parser,
                                        fr_parse_expr_id_t *out_id) {
  fr_parse_expr_t list = {.kind = FR_PARSE_EXPR_LIST};

  if (parser == NULL || out_id == NULL) {
    return FR_ERR_INVALID;
  }
  if (parser->token.kind == FR_TOKEN_RBRACKET ||
      parser->token.kind == FR_TOKEN_EOF) {
    return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_EMPTY_BLOCK,
                               FR_ERR_INVALID);
  }

  while (parser->token.kind != FR_TOKEN_EOF &&
         parser->token.kind != FR_TOKEN_RBRACKET) {
    if (list.child_count >= FR_PARSE_MAX_BODY_EXPRS) {
      return FR_ERR_CAPACITY;
    }
    if (fr_parse_statement_starts_local_bind(parser)) {
      FR_TRY(fr_parse_local_bind(parser, &list.children[list.child_count]));
    } else {
      FR_TRY(fr_parse_expression(parser, &list.children[list.child_count]));
    }
    list.child_count += 1;
    if (parser->token.kind == FR_TOKEN_SEMICOLON) {
      FR_TRY(fr_parse_advance(parser));
      if (parser->token.kind == FR_TOKEN_RBRACKET) {
        break;
      }
      if (parser->token.kind == FR_TOKEN_EOF) {
        return fr_parse_fail_token(parser, FR_DIAG_MSG_PARSE_UNEXPECTED_TOKEN,
                                   FR_ERR_INVALID);
      }
    } else if (!parser->token.leading_newline) {
      break;
    }
  }

  return fr_parse_add_expr(parser, list, out_id);
}

fr_err_t fr_parse_expression_line_with_diagnostic(
    const char *source, fr_parse_line_t *out, fr_parse_expr_id_t *out_expr,
    fr_diagnostic_t *diag) {
  fr_parser_t parser = {0};
  fr_parse_expr_t list = {.kind = FR_PARSE_EXPR_LIST};

  if (source == NULL || out == NULL || out_expr == NULL) {
    return FR_ERR_INVALID;
  }

  *out = (fr_parse_line_t){0};
  parser.cursor = source;
  parser.out = out;
  parser.diag = diag;

  FR_TRY(fr_parse_advance(&parser));
  FR_TRY(fr_parse_expression(&parser, &list.children[0]));
  list.child_count = 1;

  /* A semicolon followed by another expression collapses into a LIST so
   * `here name is value; rest` works at the top level the same way it does
   * inside a `[ ]` block. A trailing `;` with no follow-up stays a single
   * expression — finish_line eats it. */
  while (parser.token.kind == FR_TOKEN_SEMICOLON) {
    FR_TRY(fr_parse_advance(&parser));
    if (parser.token.kind == FR_TOKEN_EOF) {
      break;
    }
    if (list.child_count >= FR_PARSE_MAX_BODY_EXPRS) {
      return FR_ERR_CAPACITY;
    }
    FR_TRY(fr_parse_expression(&parser, &list.children[list.child_count]));
    list.child_count = (uint8_t)(list.child_count + 1u);
  }

  if (list.child_count == 1) {
    *out_expr = list.children[0];
  } else {
    FR_TRY(fr_parse_add_expr(&parser, list, out_expr));
  }
  return fr_parse_finish_line(&parser);
}

fr_err_t fr_parse_expression_line(const char *source, fr_parse_line_t *out,
                                  fr_parse_expr_id_t *out_expr) {
  return fr_parse_expression_line_with_diagnostic(source, out, out_expr, NULL);
}

fr_err_t fr_parse_line_with_diagnostic(const char *source, fr_parse_line_t *out,
                                       fr_diagnostic_t *diag) {
  fr_parser_t parser = {0};

  if (source == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }

  *out = (fr_parse_line_t){0};
  parser.cursor = source;
  parser.out = out;
  parser.diag = diag;

  FR_TRY(fr_parse_advance(&parser));
  if (parser.token.kind == FR_TOKEN_NAME &&
      fr_parse_span_equals(parser.token.span, "record")) {
#if !FR_FEATURE_RECORDS
    return fr_parse_fail_token(&parser, FR_DIAG_MSG_PARSE_RECORDS_DISABLED,
                               FR_ERR_UNSUPPORTED);
#else
    FR_TRY(fr_parse_advance(&parser));
    if (parser.token.kind != FR_TOKEN_NAME) {
      return fr_parse_fail_token(&parser, FR_DIAG_MSG_PARSE_EXPECTED_WORD_NAME,
                                 FR_ERR_INVALID);
    }
    out->kind = FR_PARSE_LINE_RECORD_SHAPE;
    out->definition.name = parser.token.span;
    FR_TRY(fr_parse_check_field_name_with_diagnostic(&parser,
                                                     out->definition.name));
    FR_TRY(fr_parse_advance(&parser));
    FR_TRY(fr_parse_expect(&parser, FR_TOKEN_LBRACKET));
    if (parser.token.kind == FR_TOKEN_RBRACKET) {
      return fr_parse_fail_token(&parser, FR_DIAG_MSG_PARSE_EMPTY_RECORD,
                                 FR_ERR_RANGE);
    }
    while (parser.token.kind != FR_TOKEN_RBRACKET) {
      if (parser.token.kind != FR_TOKEN_NAME ||
          out->record_field_count >= FR_PARSE_MAX_RECORD_FIELDS) {
        return fr_parse_fail_token(&parser, FR_DIAG_MSG_PARSE_EXPECTED_FIELD,
                                   FR_ERR_INVALID);
      }
      FR_TRY(fr_parse_check_field_name_with_diagnostic(&parser,
                                                       parser.token.span));
      for (uint8_t i = 0; i < out->record_field_count; i++) {
        if (fr_parse_span_same(out->record_fields[i], parser.token.span)) {
          return fr_parse_fail_token(&parser,
                                     FR_DIAG_MSG_PARSE_DUPLICATE_FIELD,
                                     FR_ERR_INVALID);
        }
      }
      out->record_fields[out->record_field_count] = parser.token.span;
      out->record_field_count += 1;
      FR_TRY(fr_parse_advance(&parser));
      if (parser.token.kind == FR_TOKEN_COMMA) {
        FR_TRY(fr_parse_advance(&parser));
        if (parser.token.kind == FR_TOKEN_RBRACKET) {
          return fr_parse_fail_token(&parser,
                                     FR_DIAG_MSG_PARSE_EXPECTED_FIELD,
                                     FR_ERR_INVALID);
        }
      } else if (parser.token.kind != FR_TOKEN_RBRACKET) {
        return fr_parse_fail_token(&parser, FR_DIAG_MSG_PARSE_EXPECTED_BLOCK_END,
                                   FR_ERR_INVALID);
      }
    }
    FR_TRY(fr_parse_expect(&parser, FR_TOKEN_RBRACKET));
    return fr_parse_finish_line(&parser);
#endif
  }
  if (parser.token.kind == FR_TOKEN_NAME &&
      fr_parse_span_equals(parser.token.span, "to")) {
    FR_TRY(fr_parse_advance(&parser));
    if (parser.token.kind != FR_TOKEN_NAME) {
      return fr_parse_fail_token(&parser, FR_DIAG_MSG_PARSE_EXPECTED_WORD_NAME,
                                 FR_ERR_INVALID);
    }
    if (fr_parse_span_equals(parser.token.span, "is") ||
        fr_parse_span_equals(parser.token.span, "to") ||
        fr_parse_span_equals(parser.token.span, "with") ||
        fr_parse_span_equals(parser.token.span, "true") ||
        fr_parse_span_equals(parser.token.span, "false") ||
        fr_parse_span_equals(parser.token.span, "and") ||
        fr_parse_span_equals(parser.token.span, "or") ||
        fr_parse_span_equals(parser.token.span, "not") ||
        fr_parse_is_event_keyword(parser.token.span)) {
      return fr_parse_fail_token(&parser, FR_DIAG_MSG_PARSE_RESERVED_NAME,
                                 FR_ERR_INVALID);
    }
    out->definition.name = parser.token.span;
    FR_TRY(fr_parse_advance(&parser));
    FR_TRY(fr_parse_function_value(&parser, &out->definition.value));
    return fr_parse_finish_line(&parser);
  }
  if (parser.token.kind != FR_TOKEN_NAME) {
    return FR_ERR_INVALID;
  }
  if (fr_parse_is_reserved_is_definition_name(parser.token.span)) {
    fr_parser_t lookahead = parser;

    lookahead.diag = NULL;
    if (fr_parse_advance(&lookahead) == FR_OK &&
        lookahead.token.kind == FR_TOKEN_NAME &&
        fr_parse_span_equals(lookahead.token.span, "is")) {
      return fr_parse_fail_token(&parser, FR_DIAG_MSG_PARSE_RESERVED_NAME,
                                 FR_ERR_INVALID);
    }
    return FR_ERR_INVALID;
  }
  out->definition.name = parser.token.span;
  FR_TRY(fr_parse_advance(&parser));
  if (parser.token.kind != FR_TOKEN_NAME ||
      !fr_parse_span_equals(parser.token.span, "is")) {
    if (parser.token.leading_space && !parser.token.leading_newline &&
        fr_parse_token_starts_word_argument(&parser.token)) {
      return fr_parse_fail_span(
          &parser, FR_DIAG_MSG_PARSE_EXPECTED_COLON_BEFORE_ARGUMENT,
          fr_parse_word_argument_diagnostic_span(&parser.token),
          FR_ERR_INVALID);
    }
    if (parser.token.kind == FR_TOKEN_RBRACKET) {
      return fr_parse_fail_token(&parser,
                                 FR_DIAG_MSG_PARSE_UNEXPECTED_BLOCK_END,
                                 FR_ERR_INVALID);
    }
    if (parser.token.kind == FR_TOKEN_RPAREN) {
      return fr_parse_fail_token(&parser,
                                 FR_DIAG_MSG_PARSE_UNEXPECTED_GROUP_END,
                                 FR_ERR_INVALID);
    }
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_parse_expect_word(&parser, "is"));
  FR_TRY(fr_parse_expression(&parser, &out->definition.value));

  return fr_parse_finish_line(&parser);
}

fr_err_t fr_parse_line(const char *source, fr_parse_line_t *out) {
  return fr_parse_line_with_diagnostic(source, out, NULL);
}
