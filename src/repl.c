#include "repl.h"

#if !FR_FEATURE_REPL
#error "repl.c requires FR_FEATURE_REPL"
#endif

#include "base_defs.h"
#include "code.h"
#if FR_FEATURE_COMPILER
#include "compile.h"
#endif
#include "event.h"
#include "handle.h"
#include "instruction.h"
#include "native.h"
#include "object.h"
#if FR_FEATURE_PERSISTENCE
#include "persist.h"
#include "persist_payload.h"
#endif
#include "platform.h"
#include "profile.h"
#include "slot.h"
#include "source_render.h"
#include "tagged.h"
#include "vm.h"

#include <string.h>

typedef struct fr_repl_writer_t {
  void *ctx;
  fr_err_t (*write)(void *ctx, const char *text);
} fr_repl_writer_t;

typedef struct fr_repl_buffer_writer_t {
  char *out;
  uint16_t out_cap;
  uint16_t used;
} fr_repl_buffer_writer_t;

typedef enum fr_repl_command_kind_t {
  FR_REPL_COMMAND_NONE,
  FR_REPL_COMMAND_BLANK,
  FR_REPL_COMMAND_STATUS,
  FR_REPL_COMMAND_WORDS,
  FR_REPL_COMMAND_EVENTS,
  FR_REPL_COMMAND_SEE,
  FR_REPL_COMMAND_CLEAR,
  FR_REPL_COMMAND_APPLY,
  FR_REPL_COMMAND_RUN,
  FR_REPL_COMMAND_INSTALL_LIBRARY,
  FR_REPL_COMMAND_INSTALL_USER,
  FR_REPL_COMMAND_WIPE_USER,
  FR_REPL_COMMAND_MEM,
} fr_repl_command_kind_t;

typedef struct fr_repl_command_t {
  fr_repl_command_kind_t kind;
  const char *arg;
  uint16_t arg_len;
} fr_repl_command_t;

#if FR_FEATURE_OVERLAY_APPLY_COMMAND
/* Single-line decode scratch; apply and run commands finish before next input. */
static uint8_t fr_repl_wire_bytes[FR_REPL_APPLY_BYTES];
static fr_overlay_update_decoded_t fr_repl_apply_decoded;
#endif

static bool fr_repl_is_space(char ch) { return ch == ' ' || ch == '\t'; }

static bool fr_repl_is_digit(char ch) { return ch >= '0' && ch <= '9'; }

static bool fr_repl_is_name_start(char ch) {
  return ch != '\0' && !fr_repl_is_space(ch) && ch != '(' && ch != ')' &&
         ch != '[' && ch != ']' && ch != ',' && ch != ':' && ch != ';' &&
         ch != '+' && ch != '-' && ch != '*' && ch != '/' && ch != '%' &&
         ch != '<' && ch != '>' && ch != '=' && ch != '"';
}

static bool fr_repl_span_equals(const char *start, uint16_t length,
                                const char *expected) {
  size_t expected_len = 0;

  expected_len = strlen(expected);
  return expected_len == length && memcmp(start, expected, length) == 0;
}

static bool fr_repl_line_starts_definition(const char *line) {
  const char *first = line;
  const char *first_end = NULL;
  const char *second = NULL;
  const char *second_end = NULL;
  uint16_t first_len = 0;
  uint16_t second_len = 0;

  if (line == NULL) {
    return false;
  }
  while (fr_repl_is_space(*first)) {
    first += 1;
  }
  first_end = first;
  while (*first_end != '\0' && !fr_repl_is_space(*first_end)) {
    first_end += 1;
  }
  if ((uint32_t)(first_end - first) > UINT16_MAX) {
    return false;
  }
  first_len = (uint16_t)(first_end - first);
  if (fr_repl_span_equals(first, first_len, "record")) {
    return true;
  }

  second = first_end;
  while (fr_repl_is_space(*second)) {
    second += 1;
  }
  second_end = second;
  while (*second_end != '\0' && !fr_repl_is_space(*second_end)) {
    second_end += 1;
  }
  if ((uint32_t)(second_end - second) > UINT16_MAX) {
    return false;
  }
  second_len = (uint16_t)(second_end - second);
  if (fr_repl_span_equals(first, first_len, "to")) {
    return fr_repl_is_name_start(*second);
  }
  return fr_repl_span_equals(second, second_len, "is");
}

/* `line` is the NUL-terminated REPL line buffer. NONE also covers exact
 * no-argument command names followed by source tokens, such as `status is 1`.
 */
static fr_err_t fr_repl_parse_recognized_command(
    const char *line, fr_repl_command_t *out) {
  const char *start = line;
  const char *end = NULL;
  const char *token_end = NULL;
  const char *arg = NULL;
  uint16_t token_len = 0;

  if (line == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }

  out->kind = FR_REPL_COMMAND_NONE;
  out->arg = NULL;
  out->arg_len = 0;

  while (fr_repl_is_space(*start)) {
    start += 1;
  }
  end = start + strlen(start);
  while (end > start && fr_repl_is_space(end[-1])) {
    end -= 1;
  }
  if (start == end) {
    out->kind = FR_REPL_COMMAND_BLANK;
    return FR_OK;
  }

  token_end = start;
  while (token_end < end && !fr_repl_is_space(*token_end)) {
    token_end += 1;
  }
  if ((uint32_t)(token_end - start) > UINT16_MAX) {
    return FR_ERR_RANGE;
  }
  token_len = (uint16_t)(token_end - start);

  arg = token_end;
  while (arg < end && fr_repl_is_space(*arg)) {
    arg += 1;
  }

  if (fr_repl_span_equals(start, token_len, "status")) {
    if (arg == end) {
      out->kind = FR_REPL_COMMAND_STATUS;
    }
    return FR_OK;
  }
  if (fr_repl_span_equals(start, token_len, "words")) {
    if (arg == end) {
      out->kind = FR_REPL_COMMAND_WORDS;
    }
    return FR_OK;
  }
  if (fr_repl_span_equals(start, token_len, "events")) {
    if (arg == end) {
      out->kind = FR_REPL_COMMAND_EVENTS;
    }
    return FR_OK;
  }
  if (fr_repl_span_equals(start, token_len, "clear")) {
    if (arg == end) {
      out->kind = FR_REPL_COMMAND_CLEAR;
    }
    return FR_OK;
  }
  if (fr_repl_span_equals(start, token_len, "see")) {
    const char *arg_end = arg;
    const char *tail = NULL;

    if (arg == end) {
      return FR_ERR_INVALID;
    }
    while (arg_end < end && !fr_repl_is_space(*arg_end)) {
      arg_end += 1;
    }
    tail = arg_end;
    while (tail < end && fr_repl_is_space(*tail)) {
      tail += 1;
    }
    if (tail != end || (uint32_t)(arg_end - arg) > UINT16_MAX) {
      return FR_ERR_INVALID;
    }
    out->kind = FR_REPL_COMMAND_SEE;
    out->arg = arg;
    out->arg_len = (uint16_t)(arg_end - arg);
    return FR_OK;
  }
  if (fr_repl_span_equals(start, token_len, "apply")) {
    if (arg == end || (uint32_t)(end - arg) > UINT16_MAX) {
      return FR_ERR_INVALID;
    }
    out->kind = FR_REPL_COMMAND_APPLY;
    out->arg = arg;
    out->arg_len = (uint16_t)(end - arg);
    return FR_OK;
  }
  if (fr_repl_span_equals(start, token_len, "run")) {
    if (arg == end || (uint32_t)(end - arg) > UINT16_MAX) {
      return FR_ERR_INVALID;
    }
    out->kind = FR_REPL_COMMAND_RUN;
    out->arg = arg;
    out->arg_len = (uint16_t)(end - arg);
    return FR_OK;
  }
  if (fr_repl_span_equals(start, token_len, "install-library")) {
    if (arg == end) {
      out->kind = FR_REPL_COMMAND_INSTALL_LIBRARY;
    }
    return FR_OK;
  }
  if (fr_repl_span_equals(start, token_len, "install-user")) {
    if (arg == end) {
      out->kind = FR_REPL_COMMAND_INSTALL_USER;
    }
    return FR_OK;
  }
  if (fr_repl_span_equals(start, token_len, "wipe-user")) {
    if (arg == end) {
      out->kind = FR_REPL_COMMAND_WIPE_USER;
    }
    return FR_OK;
  }
  if (fr_repl_span_equals(start, token_len, "mem")) {
    out->kind = FR_REPL_COMMAND_MEM;
    if (arg == end) {
      return FR_OK;
    }
    if ((uint32_t)(end - arg) > UINT16_MAX) {
      return FR_ERR_RANGE;
    }
    /* The dispatcher matches arg against known topic names; anything else
     * (unknown topic, or a known topic with trailing junk that prevents an
     * exact match) returns FR_ERR_DOMAIN per SPEC D2. */
    out->arg = arg;
    out->arg_len = (uint16_t)(end - arg);
    return FR_OK;
  }

  return FR_OK;
}

static fr_err_t fr_repl_writer_write(const fr_repl_writer_t *writer,
                                     const char *text) {
  if (writer == NULL || writer->write == NULL || text == NULL) {
    return FR_ERR_INVALID;
  }
  return writer->write(writer->ctx, text);
}

static fr_err_t fr_repl_buffer_writer_write(void *ctx, const char *text) {
  fr_repl_buffer_writer_t *writer = (fr_repl_buffer_writer_t *)ctx;
  uint16_t text_len = 0;

  if (writer == NULL || writer->out == NULL || text == NULL ||
      writer->out_cap == 0) {
    return FR_ERR_INVALID;
  }

  text_len = (uint16_t)strlen(text);
  if ((uint32_t)writer->used + text_len + 1 > writer->out_cap) {
    return FR_ERR_RANGE;
  }
  memcpy(&writer->out[writer->used], text, text_len);
  writer->used = (uint16_t)(writer->used + text_len);
  writer->out[writer->used] = '\0';
  return FR_OK;
}

static fr_err_t fr_repl_io_writer_write(void *ctx, const char *text) {
  const fr_repl_io_t *io = (const fr_repl_io_t *)ctx;

  if (io == NULL || io->write_text == NULL) {
    return FR_ERR_INVALID;
  }
  return io->write_text(text);
}

static fr_err_t fr_repl_write_u16(char *out, uint16_t out_cap, uint16_t value) {
  char digits[5];
  uint8_t count = 0;
  uint16_t used = 0;

  if (out == NULL || out_cap == 0) {
    return FR_ERR_INVALID;
  }

  do {
    digits[count] = (char)('0' + (value % 10));
    value = (uint16_t)(value / 10);
    count += 1;
  } while (value > 0);

  if ((uint16_t)(count + 1) > out_cap) {
    return FR_ERR_RANGE;
  }
  while (count > 0) {
    count -= 1;
    out[used] = digits[count];
    used += 1;
  }
  out[used] = '\0';
  return FR_OK;
}

static fr_err_t fr_repl_write_u32(char *out, uint16_t out_cap, uint32_t value) {
  char digits[10];
  uint8_t count = 0;
  uint16_t used = 0;

  if (out == NULL || out_cap == 0) {
    return FR_ERR_INVALID;
  }

  do {
    digits[count] = (char)('0' + (value % 10u));
    value = value / 10u;
    count += 1;
  } while (value > 0);

  if ((uint16_t)(count + 1) > out_cap) {
    return FR_ERR_RANGE;
  }
  while (count > 0) {
    count -= 1;
    out[used] = digits[count];
    used += 1;
  }
  out[used] = '\0';
  return FR_OK;
}

static fr_err_t fr_repl_append(char *out, uint16_t out_cap, uint16_t *used,
                               const char *text) {
  uint16_t text_len = 0;

  if (out == NULL || used == NULL || text == NULL) {
    return FR_ERR_INVALID;
  }
  text_len = (uint16_t)strlen(text);
  if ((uint32_t)*used + text_len + 1 > out_cap) {
    return FR_ERR_RANGE;
  }

  memcpy(&out[*used], text, text_len);
  *used = (uint16_t)(*used + text_len);
  out[*used] = '\0';
  return FR_OK;
}

static fr_err_t fr_repl_append_u16(char *out, uint16_t out_cap,
                                   uint16_t *used, uint16_t value) {
  if (out == NULL || used == NULL || *used >= out_cap) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_repl_write_u16(&out[*used], (uint16_t)(out_cap - *used), value));
  *used = (uint16_t)strlen(out);
  return FR_OK;
}

static fr_err_t fr_repl_append_u32(char *out, uint16_t out_cap,
                                   uint16_t *used, uint32_t value) {
  if (out == NULL || used == NULL || *used >= out_cap) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_repl_write_u32(&out[*used], (uint16_t)(out_cap - *used), value));
  *used = (uint16_t)strlen(out);
  return FR_OK;
}

static fr_err_t fr_repl_append_int(char *out, uint16_t out_cap,
                                   uint16_t *used, fr_int_t value) {
  if (value < 0) {
    uint32_t magnitude = (uint32_t)(-(int64_t)value);

    FR_TRY(fr_repl_append(out, out_cap, used, "-"));
    return fr_repl_append_u32(out, out_cap, used, magnitude);
  }
  return fr_repl_append_u32(out, out_cap, used, (uint32_t)value);
}

static fr_err_t fr_repl_append_char(char *out, uint16_t out_cap,
                                    uint16_t *used, char ch) {
  if (out == NULL || used == NULL) {
    return FR_ERR_INVALID;
  }
  if ((uint32_t)*used + 2 > out_cap) {
    return FR_ERR_RANGE;
  }
  out[*used] = ch;
  *used = (uint16_t)(*used + 1);
  out[*used] = '\0';
  return FR_OK;
}

static fr_err_t fr_repl_append_hex_byte(char *out, uint16_t out_cap,
                                        uint16_t *used, uint8_t byte) {
  static const char digits[] = "0123456789abcdef";

  FR_TRY(fr_repl_append(out, out_cap, used, "\\x"));
  FR_TRY(fr_repl_append_char(out, out_cap, used, digits[byte >> 4]));
  return fr_repl_append_char(out, out_cap, used, digits[byte & 0x0fu]);
}

/* Buffered quoted-text formatter, used by see/records. The eval-result
 * display uses a streaming variant (fr_repl_writer_write_quoted_text);
 * if you change escape rules here, change them there too. */
static fr_err_t fr_repl_append_quoted_text(char *out, uint16_t out_cap,
                                           uint16_t *used,
                                           const uint8_t *bytes,
                                           uint16_t length) {
  FR_TRY(fr_repl_append_char(out, out_cap, used, '"'));
  for (uint16_t i = 0; i < length; i++) {
    uint8_t byte = bytes[i];

    if (byte == '"' || byte == '\\') {
      FR_TRY(fr_repl_append_char(out, out_cap, used, '\\'));
      FR_TRY(fr_repl_append_char(out, out_cap, used, (char)byte));
    } else if (byte == '\n') {
      FR_TRY(fr_repl_append(out, out_cap, used, "\\n"));
    } else if (byte == '\r') {
      FR_TRY(fr_repl_append(out, out_cap, used, "\\r"));
    } else if (byte == '\t') {
      FR_TRY(fr_repl_append(out, out_cap, used, "\\t"));
    } else if (byte >= 0x20u && byte <= 0x7eu) {
      FR_TRY(fr_repl_append_char(out, out_cap, used, (char)byte));
    } else {
      FR_TRY(fr_repl_append_hex_byte(out, out_cap, used, byte));
    }
  }
  return fr_repl_append_char(out, out_cap, used, '"');
}

#if FR_FEATURE_RECORDS
static fr_err_t fr_repl_append_record_name(char *out, uint16_t out_cap,
                                           uint16_t *used,
                                           fr_record_name_t name) {
  if (name.bytes == NULL || name.length == 0) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < name.length; i++) {
    FR_TRY(fr_repl_append_char(out, out_cap, used, (char)name.bytes[i]));
  }
  return FR_OK;
}

static fr_err_t fr_repl_append_record_shape(fr_runtime_t *runtime, char *out,
                                            uint16_t out_cap, uint16_t *used,
                                            fr_object_id_t shape_object_id,
                                            const char *prefix) {
  fr_record_name_t name = {0};
  uint16_t field_count = 0;

  FR_TRY(fr_record_shape_view(runtime, shape_object_id, &name, &field_count));
  FR_TRY(fr_repl_append(out, out_cap, used, prefix));
  FR_TRY(fr_repl_append_record_name(out, out_cap, used, name));
  FR_TRY(fr_repl_append(out, out_cap, used, " [ "));
  for (uint16_t i = 0; i < field_count; i++) {
    fr_record_name_t field = {0};

    if (i > 0) {
      FR_TRY(fr_repl_append(out, out_cap, used, ", "));
    }
    FR_TRY(fr_record_shape_field_name(runtime, shape_object_id, i, &field));
    FR_TRY(fr_repl_append_record_name(out, out_cap, used, field));
  }
  return fr_repl_append(out, out_cap, used, " ]");
}
#endif

static fr_err_t fr_repl_writer_write_hex_u32(const fr_repl_writer_t *writer,
                                             uint32_t value) {
  static const char digits[] = "0123456789abcdef";
  char out[9];

  if (writer == NULL) {
    return FR_ERR_INVALID;
  }
  for (uint8_t i = 0; i < 8; i++) {
    uint8_t shift = (uint8_t)((7u - i) * 4u);

    out[i] = digits[(value >> shift) & 0x0fu];
  }
  out[8] = '\0';
  return fr_repl_writer_write(writer, out);
}

static fr_err_t fr_repl_write_status(const fr_repl_writer_t *writer) {
  if (writer == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_repl_writer_write(writer, "frothy status v1 profile="));
  FR_TRY(fr_repl_writer_write(writer, fr_profile_contract_name()));
  FR_TRY(fr_repl_writer_write(writer, " profile_hash="));
  FR_TRY(fr_repl_writer_write_hex_u32(writer, fr_profile_hash()));
  FR_TRY(fr_repl_writer_write(writer, " compiler="));
  FR_TRY(fr_repl_writer_write(writer, fr_profile_compiler_mode()));
  FR_TRY(fr_repl_writer_write(writer, " names="));
  FR_TRY(fr_repl_writer_write(writer, fr_profile_names_mode()));
  FR_TRY(fr_repl_writer_write(writer, " storage="));
  FR_TRY(fr_repl_writer_write(writer, fr_profile_storage_mode()));
  FR_TRY(fr_repl_writer_write(writer, " interrupt="));
  FR_TRY(fr_repl_writer_write(writer, fr_profile_interrupt_mode()));
  FR_TRY(fr_repl_writer_write(writer, " word_size="));
  FR_TRY(fr_repl_writer_write(writer, "32"));
  FR_TRY(fr_repl_writer_write(writer, " int_min="));
  {
    char int_min[14];
    uint16_t used = 0;

    int_min[0] = '\0';
    FR_TRY(fr_repl_append_int(int_min, (uint16_t)sizeof(int_min), &used,
                              (fr_int_t)FR_TAGGED_INT_MIN));
    FR_TRY(fr_repl_writer_write(writer, int_min));
  }
  FR_TRY(fr_repl_writer_write(writer, " int_max="));
  {
    char int_max[14];
    uint16_t used = 0;

    int_max[0] = '\0';
    FR_TRY(fr_repl_append_int(int_max, (uint16_t)sizeof(int_max), &used,
                              (fr_int_t)FR_TAGGED_INT_MAX));
    FR_TRY(fr_repl_writer_write(writer, int_max));
  }
  FR_TRY(fr_repl_writer_write(writer, " apply_bytes="));
  {
    char apply_bytes[6];

    FR_TRY(fr_repl_write_u16(apply_bytes, (uint16_t)sizeof(apply_bytes),
                             FR_REPL_APPLY_BYTES));
    FR_TRY(fr_repl_writer_write(writer, apply_bytes));
  }
  return fr_repl_writer_write(writer, "\nok\n");
}

static fr_err_t fr_repl_write_tagged_value(fr_runtime_t *runtime, char *out,
                                           uint16_t out_cap,
                                           fr_tagged_t tagged, bool inspect) {
  const fr_native_entry_t *entry = NULL;
  uint16_t used = 0;
  fr_int_t int_value = 0;
  fr_slot_id_t slot_id = 0;
  fr_code_object_id_t code_object_id = 0;
  fr_native_id_t native_id = 0;
  fr_object_id_t object_id = 0;
  fr_handle_ref_t handle_ref = {0};

  if (out == NULL || out_cap == 0 || (inspect && runtime == NULL)) {
    return FR_ERR_INVALID;
  }
  out[0] = '\0';

  if (fr_tagged_is_nil(tagged)) {
    return fr_repl_append(out, out_cap, &used, "nil");
  }
  if (fr_tagged_is_false(tagged)) {
    return fr_repl_append(out, out_cap, &used, "false");
  }
  if (fr_tagged_is_true(tagged)) {
    return fr_repl_append(out, out_cap, &used, "true");
  }
  if (fr_tagged_decode_int(tagged, &int_value) == FR_OK) {
    return fr_repl_append_int(out, out_cap, &used, int_value);
  }
  if (fr_tagged_decode_slot_id(tagged, &slot_id) == FR_OK) {
    FR_TRY(fr_repl_append(out, out_cap, &used, "slot "));
    return fr_repl_append_u16(out, out_cap, &used, slot_id);
  }
  if (fr_tagged_decode_code_object_id(tagged, &code_object_id) == FR_OK) {
    if (inspect) {
      (void)code_object_id;
      return fr_repl_append(out, out_cap, &used, "code");
    }
    FR_TRY(fr_repl_append(out, out_cap, &used, "code "));
    return fr_repl_append_u16(out, out_cap, &used, code_object_id);
  }
  if (fr_tagged_decode_native_id(tagged, &native_id) == FR_OK) {
    if (inspect) {
      FR_TRY(fr_native_get(runtime, native_id, &entry));
      FR_TRY(fr_repl_append(out, out_cap, &used, "native arity "));
      return fr_repl_append_u16(out, out_cap, &used, entry->arity);
    }
    FR_TRY(fr_repl_append(out, out_cap, &used, "native "));
    return fr_repl_append_u16(out, out_cap, &used, native_id);
  }
  if (fr_tagged_decode_handle_ref(tagged, &handle_ref) == FR_OK) {
    fr_handle_kind_t handle_kind = FR_HANDLE_KIND_NONE;
    fr_err_t err = FR_OK;

    if (runtime == NULL) {
      return FR_ERR_INVALID;
    }
    err = fr_handle_lookup(runtime, handle_ref, FR_HANDLE_KIND_NONE,
                           &handle_kind, NULL);
    if (err == FR_ERR_HANDLE) {
      return fr_repl_append(out, out_cap, &used,
                            inspect ? "volatile closed" : "handle closed");
    }
    FR_TRY(err);
    FR_TRY(fr_repl_append(out, out_cap, &used,
                          inspect ? "volatile " : "handle "));
    return fr_repl_append(out, out_cap, &used,
                          fr_handle_kind_name(handle_kind));
  }
#if FR_FEATURE_BYTES
  {
    fr_bytes_ref_t bytes_ref = {0};
    if (fr_tagged_decode_bytes_ref(tagged, &bytes_ref) == FR_OK) {
      const uint8_t *bytes_data = NULL;
      uint16_t bytes_length = 0;

      if (runtime != NULL &&
          fr_bytes_view(runtime, bytes_ref, &bytes_data, &bytes_length) ==
              FR_OK) {
        FR_TRY(fr_repl_append(out, out_cap, &used, "bytes "));
        return fr_repl_append_u16(out, out_cap, &used, bytes_length);
      }
      return fr_repl_append(out, out_cap, &used, "bytes stale");
    }
  }
#endif
  if (fr_tagged_decode_object_id(tagged, &object_id) == FR_OK) {
    uint16_t cell_length = 0;
    const uint8_t *text_bytes = NULL;
    uint16_t text_length = 0;
#if FR_FEATURE_RECORDS
    fr_record_name_t shape_name = {0};
    fr_object_id_t shape_object_id = 0;
    uint16_t field_count = 0;
#endif

    if (fr_text_view(runtime, object_id, &text_bytes, &text_length) == FR_OK) {
      if (inspect) {
        FR_TRY(fr_repl_append(out, out_cap, &used, "text "));
        return fr_repl_append_u16(out, out_cap, &used, text_length);
      }
      return fr_repl_append_quoted_text(out, out_cap, &used, text_bytes,
                                        text_length);
    }
#if FR_FEATURE_RECORDS
    if (fr_record_shape_view(runtime, object_id, &shape_name, &field_count) ==
        FR_OK) {
      return fr_repl_append_record_shape(runtime, out, out_cap, &used,
                                         object_id,
                                         inspect ? "record-shape "
                                                 : "record ");
    }
    if (fr_record_view(runtime, object_id, &shape_object_id, &field_count) ==
        FR_OK) {
      FR_TRY(fr_record_shape_view(runtime, shape_object_id, &shape_name,
                                  &field_count));
      if (inspect) {
        return fr_repl_append_record_shape(runtime, out, out_cap, &used,
                                           shape_object_id, "record ");
      }
      FR_TRY(fr_repl_append_record_name(out, out_cap, &used, shape_name));
      FR_TRY(fr_repl_append(out, out_cap, &used, ": "));
      for (uint16_t i = 0; i < field_count; i++) {
        fr_tagged_t field_value = 0;
        char value_out[FR_REPL_OUTPUT_BYTES];

        if (i > 0) {
          FR_TRY(fr_repl_append(out, out_cap, &used, ", "));
        }
        FR_TRY(fr_record_read_index(runtime, object_id, i, &field_value));
        FR_TRY(fr_repl_write_tagged_value(runtime, value_out,
                                          (uint16_t)sizeof(value_out),
                                          field_value, false));
        FR_TRY(fr_repl_append(out, out_cap, &used, value_out));
      }
      return FR_OK;
    }
#endif
    if (fr_cells_length(runtime, object_id, &cell_length) == FR_OK) {
      FR_TRY(fr_repl_append(out, out_cap, &used, "cells "));
      return fr_repl_append_u16(out, out_cap, &used, cell_length);
    }
    FR_TRY(fr_repl_append(out, out_cap, &used, "object "));
    return fr_repl_append_u16(out, out_cap, &used, object_id);
  }
  return FR_ERR_UNSUPPORTED;
}

/* Eval-result display streams text because text may exceed
 * FR_REPL_OUTPUT_BYTES; non-text tagged values still go through the fixed
 * formatter above. Text escape rules MUST stay in sync with
 * fr_repl_append_quoted_text — that one still serves see/records. */
enum {
  FR_REPL_TEXT_CHUNK_BYTES = 64,
  /* Longest per-byte escape is \xNN. */
  FR_REPL_MAX_TEXT_ESCAPE_BYTES = 4,
};

static fr_err_t
fr_repl_writer_write_quoted_text(const fr_repl_writer_t *writer,
                                 const uint8_t *bytes, uint16_t length) {
  char chunk[FR_REPL_TEXT_CHUNK_BYTES];
  uint16_t used = 0;

  if (writer == NULL || (length > 0 && bytes == NULL)) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_repl_writer_write(writer, "\""));
  for (uint16_t i = 0; i < length; i++) {
    uint8_t byte = bytes[i];

    if (used + FR_REPL_MAX_TEXT_ESCAPE_BYTES + 1 > sizeof(chunk)) {
      chunk[used] = '\0';
      FR_TRY(fr_repl_writer_write(writer, chunk));
      used = 0;
    }
    if (byte == '"' || byte == '\\') {
      chunk[used++] = '\\';
      chunk[used++] = (char)byte;
    } else if (byte == '\n') {
      chunk[used++] = '\\';
      chunk[used++] = 'n';
    } else if (byte == '\r') {
      chunk[used++] = '\\';
      chunk[used++] = 'r';
    } else if (byte == '\t') {
      chunk[used++] = '\\';
      chunk[used++] = 't';
    } else if (byte >= 0x20u && byte <= 0x7eu) {
      chunk[used++] = (char)byte;
    } else {
      static const char hex_digits[] = "0123456789abcdef";

      chunk[used++] = '\\';
      chunk[used++] = 'x';
      chunk[used++] = hex_digits[byte >> 4];
      chunk[used++] = hex_digits[byte & 0x0fu];
    }
  }
  if (used > 0) {
    chunk[used] = '\0';
    FR_TRY(fr_repl_writer_write(writer, chunk));
  }
  return fr_repl_writer_write(writer, "\"");
}

static fr_err_t
fr_repl_writer_write_tagged_value(const fr_repl_writer_t *writer,
                                  fr_runtime_t *runtime, fr_tagged_t tagged) {
  fr_object_id_t object_id = 0;
  const uint8_t *text_bytes = NULL;
  uint16_t text_length = 0;
  char buf[FR_REPL_OUTPUT_BYTES];

  if (writer == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_tagged_decode_object_id(tagged, &object_id) == FR_OK &&
      fr_text_view(runtime, object_id, &text_bytes, &text_length) == FR_OK) {
    return fr_repl_writer_write_quoted_text(writer, text_bytes, text_length);
  }
  FR_TRY(fr_repl_write_tagged_value(runtime, buf, (uint16_t)sizeof(buf),
                                    tagged, false));
  return fr_repl_writer_write(writer, buf);
}

/* value + "\nok\n" — always displays the value, including nil. Used after
 * bare-word lookup, where reading a slot bound to nil should still print
 * "nil" rather than swallow the result. */
static fr_err_t
fr_repl_writer_write_tagged_response(const fr_repl_writer_t *writer,
                                     fr_runtime_t *runtime,
                                     fr_tagged_t tagged) {
  FR_TRY(fr_repl_writer_write_tagged_value(writer, runtime, tagged));
  return fr_repl_writer_write(writer, "\nok\n");
}

/* Short-circuit nil to "ok\n"; otherwise value + "\nok\n". Used after
 * expression eval, where a nil result usually means "no value to show". */
static fr_err_t
fr_repl_writer_write_eval_response(const fr_repl_writer_t *writer,
                                   fr_runtime_t *runtime, fr_tagged_t tagged) {
  if (fr_tagged_is_nil(tagged)) {
    return fr_repl_writer_write(writer, "ok\n");
  }
  return fr_repl_writer_write_tagged_response(writer, runtime, tagged);
}

static void fr_repl_truncate(char *out, uint16_t *used, uint16_t keep) {
  if (out == NULL || used == NULL) {
    return;
  }
  *used = keep;
  out[*used] = '\0';
}

static fr_err_t fr_repl_append_span(char *out, uint16_t out_cap,
                                    uint16_t *used, const char *start,
                                    uint16_t length) {
  if (start == NULL) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < length; i++) {
    FR_TRY(fr_repl_append_char(out, out_cap, used, start[i]));
  }
  return FR_OK;
}

static bool fr_repl_diagnostic_span_column(const char *line,
                                           const fr_diagnostic_t *diag,
                                           uint16_t *out_column) {
  size_t line_len = 0;
  fr_addr_t line_start = 0;
  fr_addr_t line_end = 0;
  fr_addr_t span_start = 0;

  if (line == NULL || diag == NULL || diag->span_start == NULL ||
      out_column == NULL) {
    return false;
  }
  line_len = strlen(line);
  if (line_len > UINT16_MAX) {
    return false;
  }
  line_start = (fr_addr_t)line;
  line_end = line_start + (fr_addr_t)line_len;
  span_start = (fr_addr_t)diag->span_start;
  if (span_start < line_start || span_start > line_end) {
    return false;
  }
  if (span_start == line_end && diag->span_length != 0) {
    return false;
  }
  if (span_start < line_end && diag->span_length == 0) {
    return false;
  }
  *out_column = (uint16_t)(span_start - line_start);
  return true;
}

static fr_err_t fr_repl_append_caret_line(char *out, uint16_t out_cap,
                                          uint16_t *used, uint16_t column,
                                          uint16_t length) {
  for (uint16_t i = 0; i < column; i++) {
    FR_TRY(fr_repl_append_char(out, out_cap, used, ' '));
  }
  for (uint16_t i = 0; i < length; i++) {
    FR_TRY(fr_repl_append_char(out, out_cap, used, '^'));
  }
  return fr_repl_append_char(out, out_cap, used, '\n');
}

static fr_err_t fr_repl_append_arity_line(char *out, uint16_t out_cap,
                                          uint16_t *used,
                                          const fr_diagnostic_t *diag) {
  FR_TRY(fr_repl_append(out, out_cap, used, "expected "));
  FR_TRY(fr_repl_append_u16(out, out_cap, used, diag->expected));
  if (diag->expected == 1) {
    FR_TRY(fr_repl_append(out, out_cap, used, " argument, got "));
  } else {
    FR_TRY(fr_repl_append(out, out_cap, used, " arguments, got "));
  }
  FR_TRY(fr_repl_append_u16(out, out_cap, used, diag->got));
  return fr_repl_append_char(out, out_cap, used, '\n');
}

static fr_err_t fr_repl_append_note_line(char *out, uint16_t out_cap,
                                         uint16_t *used, const char *message) {
  FR_TRY(fr_repl_append(out, out_cap, used, "note: "));
  FR_TRY(fr_repl_append(out, out_cap, used, message));
  return fr_repl_append_char(out, out_cap, used, '\n');
}

static fr_err_t fr_repl_append_suggestion_line(char *out, uint16_t out_cap,
                                               uint16_t *used,
                                               const fr_diagnostic_t *diag) {
  FR_TRY(fr_repl_append(out, out_cap, used, "help: did you mean \""));
  FR_TRY(fr_repl_append_span(out, out_cap, used, diag->suggestion_start,
                             diag->suggestion_length));
  return fr_repl_append(out, out_cap, used, "\"?\n");
}

static fr_err_t fr_repl_write_error(char *out, uint16_t out_cap, fr_err_t err,
                                    const char *line,
                                    const fr_diagnostic_t *diag) {
  const char *name = fr_err_name(err);
  const char *message = NULL;
  uint16_t used = 0;
  uint16_t base_used = 0;
  bool source_visible = line == NULL;

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  if (name == NULL) {
    FR_TRY(fr_repl_append(out, out_cap, &used, "err "));
    FR_TRY(fr_repl_append_u16(out, out_cap, &used, (uint16_t)err));
    return fr_repl_append(out, out_cap, &used, "\n");
  }
  FR_TRY(fr_repl_append(out, out_cap, &used, "error: "));
  FR_TRY(fr_repl_append(out, out_cap, &used, name));
  FR_TRY(fr_repl_append(out, out_cap, &used, " ("));
  FR_TRY(fr_repl_append_u16(out, out_cap, &used, (uint16_t)err));
  FR_TRY(fr_repl_append(out, out_cap, &used, ")\n"));
  base_used = used;

  if (diag == NULL || diag->span_start == NULL) {
    return FR_OK;
  }

  if (diag->kind == FR_DIAG_ARITY) {
    if (fr_repl_append_arity_line(out, out_cap, &used, diag) != FR_OK) {
      fr_repl_truncate(out, &used, base_used);
      return FR_OK;
    }
  } else if (diag->kind == FR_DIAG_NAME) {
    if (fr_repl_append(out, out_cap, &used, "name: ") != FR_OK ||
        fr_repl_append_span(out, out_cap, &used, diag->span_start,
                            diag->span_length) != FR_OK ||
        fr_repl_append_char(out, out_cap, &used, '\n') != FR_OK) {
      fr_repl_truncate(out, &used, base_used);
      return FR_OK;
    }
  } else if (diag->message_id != FR_DIAG_MSG_NONE) {
    message = fr_diag_message(diag->message_id);
    if (message == NULL) {
      return FR_OK;
    }
    if (fr_repl_append(out, out_cap, &used, message) != FR_OK ||
        fr_repl_append_char(out, out_cap, &used, '\n') != FR_OK) {
      fr_repl_truncate(out, &used, base_used);
      return FR_OK;
    }
  } else {
    return FR_OK;
  }

  if (line != NULL) {
    uint16_t detail_used = used;
    uint16_t source_used = used;
    uint16_t column = 0;
    uint16_t caret_length = diag->span_length;
    bool has_caret = fr_repl_diagnostic_span_column(line, diag, &column);

    if (fr_repl_append(out, out_cap, &used, line) != FR_OK ||
        fr_repl_append_char(out, out_cap, &used, '\n') != FR_OK) {
      fr_repl_truncate(out, &used, detail_used);
      return FR_OK;
    }
    source_visible = true;
    source_used = used;
    if (caret_length == 0) {
      caret_length = 1;
    }
    if (has_caret &&
        fr_repl_append_caret_line(out, out_cap, &used, column, caret_length) !=
            FR_OK) {
      fr_repl_truncate(out, &used, source_used);
    }
  }

  if (source_visible && diag->kind == FR_DIAG_NAME &&
      diag->message_id != FR_DIAG_MSG_NONE) {
    uint16_t note_used = used;

    message = fr_diag_message(diag->message_id);
    if (message != NULL &&
        fr_repl_append_note_line(out, out_cap, &used, message) != FR_OK) {
      fr_repl_truncate(out, &used, note_used);
    }
  }

  if (source_visible && diag->suggestion_start != NULL) {
    uint16_t help_used = used;

    if (fr_repl_append_suggestion_line(out, out_cap, &used, diag) != FR_OK) {
      fr_repl_truncate(out, &used, help_used);
    }
  }

  return FR_OK;
}

#if FR_FEATURE_PERSISTENCE
static void fr_repl_write_startup_error(fr_err_t err) {
  char response[FR_REPL_OUTPUT_BYTES];

  if (fr_repl_write_error(response, (uint16_t)sizeof(response), err, NULL,
                          NULL) == FR_OK) {
    (void)fr_platform_write_text(response);
  }
}
#endif

fr_err_t fr_repl_startup_restore_and_boot(fr_runtime_t *runtime) {
#if FR_FEATURE_PERSISTENCE
  fr_tagged_t out = fr_tagged_nil();
  fr_err_t l1_err = FR_OK;
  fr_err_t l2_err = FR_OK;
  fr_err_t boot_err = FR_OK;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  /* SPEC D6 boot order: L1 records (library tier) before L2 (user tier).
   * Two explicit calls — L1 first writes a freshly-reset runtime with the
   * library layer; L2 layers user definitions on top. An L1 miss
   * (no payload yet) is normal on first boot. An L1 error other than
   * FR_ERR_NOT_FOUND surfaces via fr_repl_write_startup_error; the L2 call
   * still runs so a corrupt L1 doesn't strand a saved user tier. */
  l1_err = fr_persist_restore_library(runtime);
  if (l1_err != FR_OK && l1_err != FR_ERR_NOT_FOUND) {
    fr_repl_write_startup_error(l1_err);
  }
  l2_err = fr_persist_restore_user(runtime);
  if (l2_err != FR_OK && l2_err != FR_ERR_NOT_FOUND) {
    fr_repl_write_startup_error(l2_err);
  }
  if (l1_err != FR_OK && l2_err != FR_OK) {
    /* Nothing installed from NVS; skip boot dispatch. */
    return FR_OK;
  }

  boot_err = fr_vm_run_boot(runtime, &out);
  if (boot_err == FR_ERR_INTERRUPTED) {
    fr_runtime_clear_interrupt(runtime);
    fr_repl_write_startup_error(boot_err);
    return FR_OK;
  }
  if (boot_err != FR_OK) {
    fr_repl_write_startup_error(boot_err);
  }
#else
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
#endif
  return FR_OK;
}

#if FR_FEATURE_INTROSPECTION
static fr_err_t fr_repl_write_word(const fr_repl_writer_t *writer,
                                   bool *wrote_word, const char *name) {
  if (writer == NULL || wrote_word == NULL || name == NULL) {
    return FR_ERR_INVALID;
  }
  if (*wrote_word) {
    FR_TRY(fr_repl_writer_write(writer, " "));
  }
  FR_TRY(fr_repl_writer_write(writer, name));
  *wrote_word = true;
  return FR_OK;
}

static fr_err_t fr_repl_write_words(fr_runtime_t *runtime,
                                    const fr_repl_writer_t *writer) {
  uint16_t base_word_count = fr_base_slot_count();
  uint16_t overlay_word_count = fr_slot_project_name_count(runtime);
  bool wrote_word = false;

  if (runtime == NULL || writer == NULL) {
    return FR_ERR_INVALID;
  }
  if (base_word_count == 0 && overlay_word_count == 0) {
    return FR_ERR_UNSUPPORTED;
  }

  for (uint16_t i = 0; i < base_word_count; i++) {
    const char *name = fr_base_slot_name_at(i);

    if (name == NULL) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_repl_write_word(writer, &wrote_word, name));
  }
#if FR_FEATURE_SOURCE_BASE
  for (uint16_t i = 0; i < fr_base_source_record_count(); i++) {
    fr_slot_id_t slot_id = 0;
    const char *name = NULL;

    if (fr_base_source_record_slot_id_at(i, &slot_id) != FR_OK) {
      return FR_ERR_INVALID;
    }
    name = fr_base_source_slot_name(slot_id);
    if (name == NULL) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_repl_write_word(writer, &wrote_word, name));
  }
#endif
  for (uint16_t i = 0; i < overlay_word_count; i++) {
    const char *name = fr_slot_project_name_at(runtime, i);

    if (name == NULL) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_repl_write_word(writer, &wrote_word, name));
  }
  return fr_repl_writer_write(writer, "\nok\n");
}
#endif

#if FR_FEATURE_EVENTS
static const char *fr_repl_event_kind_name(fr_event_kind_t kind) {
  /* `on falling` / `on rising` / `on changes` / `every` / `after` —
   * matches the source spellings the parser accepts so a user can
   * round-trip what `events` prints back into source if needed. */
  switch (kind) {
  case FR_EVENT_KIND_GPIO_RISING:  return "on rising";
  case FR_EVENT_KIND_GPIO_FALLING: return "on falling";
  case FR_EVENT_KIND_GPIO_CHANGES: return "on changes";
  case FR_EVENT_KIND_EVERY:        return "every";
  case FR_EVENT_KIND_AFTER:        return "after";
  default:                         return NULL;
  }
}

/* Format: one line per active binding, then optional overflow line,
 * then `ok\n`. Empty binding table prints just `ok\n` — zero is a
 * valid count, so callers parse "lines before ok" as the table.
 *
 * Per-binding line: `<kind-name> <source>[ debounce <ms>]\n`
 *   - kind-name: source spelling (`on rising`, `every`, ...) so the
 *     line round-trips back to parseable source if needed
 *   - source: pin number for GPIO, ms interval for timers (uint16
 *     each — source is uint16_t in the binding struct)
 *
 * Overflow line emits a distinct keyword when truncated so any
 * parser can tell saturation from a real count:
 *   - `overflow N\n` when the count fits in uint16
 *   - `overflow_saturated 65535\n` when the count exceeds uint16
 * The split keyword keeps the value field a clean integer either way. */
static fr_err_t fr_repl_write_events(fr_runtime_t *runtime,
                                     const fr_repl_writer_t *writer) {
  char number[12];

  if (runtime == NULL || writer == NULL) {
    return FR_ERR_INVALID;
  }

  for (uint16_t i = 0; i < FR_EVENT_BINDING_COUNT; i++) {
    const fr_event_binding_t *entry = &runtime->events.entries[i];
    const char *kind_name = NULL;

    if (entry->kind == FR_EVENT_KIND_NONE) {
      continue;
    }
    kind_name = fr_repl_event_kind_name(entry->kind);
    if (kind_name == NULL) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_repl_writer_write(writer, kind_name));
    FR_TRY(fr_repl_writer_write(writer, " "));
    FR_TRY(fr_repl_write_u16(number, (uint16_t)sizeof(number),
                             entry->source));
    FR_TRY(fr_repl_writer_write(writer, number));
    if (entry->debounce_ms > 0) {
      FR_TRY(fr_repl_writer_write(writer, " debounce "));
      FR_TRY(fr_repl_write_u16(number, (uint16_t)sizeof(number),
                               entry->debounce_ms));
      FR_TRY(fr_repl_writer_write(writer, number));
    }
    FR_TRY(fr_repl_writer_write(writer, "\n"));
  }
  if (runtime->events.overflow_count > 0) {
    uint32_t raw = runtime->events.overflow_count;
    bool saturated = raw > UINT16_MAX;
    uint16_t shown = saturated ? UINT16_MAX : (uint16_t)raw;

    FR_TRY(fr_repl_writer_write(
        writer, saturated ? "overflow_saturated " : "overflow "));
    FR_TRY(fr_repl_write_u16(number, (uint16_t)sizeof(number), shown));
    FR_TRY(fr_repl_writer_write(writer, number));
    FR_TRY(fr_repl_writer_write(writer, "\n"));
  }
  return fr_repl_writer_write(writer, "ok\n");
}
#endif

static fr_err_t fr_repl_write_mem_pair(const fr_repl_writer_t *writer,
                                       const char *key, uint32_t value) {
  char number[11];

  FR_TRY(fr_repl_writer_write(writer, key));
  FR_TRY(fr_repl_writer_write(writer, " "));
  FR_TRY(fr_repl_write_u32(number, (uint16_t)sizeof(number), value));
  FR_TRY(fr_repl_writer_write(writer, number));
  return fr_repl_writer_write(writer, "\n");
}

static fr_err_t fr_repl_write_mem_heap(const fr_repl_writer_t *writer) {
  uint32_t heap_free = 0;
  uint32_t heap_largest = 0;

  FR_TRY(fr_platform_heap_free(&heap_free));
  FR_TRY(fr_platform_heap_largest(&heap_largest));
  FR_TRY(fr_repl_write_mem_pair(writer, "heap.free", heap_free));
  return fr_repl_write_mem_pair(writer, "heap.largest", heap_largest);
}

static fr_err_t fr_repl_write_mem_slots(fr_runtime_t *runtime,
                                        const fr_repl_writer_t *writer) {
  FR_TRY(fr_repl_write_mem_pair(writer, "slots.used", runtime->slots.count));
  return fr_repl_write_mem_pair(writer, "slots.total", FR_PROFILE_MAX_SLOTS);
}

static fr_err_t fr_repl_write_mem_objects(fr_runtime_t *runtime,
                                          const fr_repl_writer_t *writer) {
  FR_TRY(fr_repl_write_mem_pair(writer, "objects.entries.used",
                                runtime->objects.count));
  FR_TRY(fr_repl_write_mem_pair(writer, "objects.entries.total",
                                FR_OBJECT_TABLE_CAPACITY));
  FR_TRY(fr_repl_write_mem_pair(writer, "objects.cells.used",
                                runtime->objects.used_cell_words));
  FR_TRY(fr_repl_write_mem_pair(writer, "objects.cells.total",
                                FR_CELL_WORD_CAPACITY));
  FR_TRY(fr_repl_write_mem_pair(writer, "objects.text.used",
                                runtime->objects.used_text_bytes));
  FR_TRY(fr_repl_write_mem_pair(writer, "objects.text.total",
                                FR_TEXT_BYTE_CAPACITY));
#if FR_FEATURE_RECORDS
  FR_TRY(fr_repl_write_mem_pair(writer, "objects.record_names.used",
                                runtime->objects.used_record_names));
  FR_TRY(fr_repl_write_mem_pair(writer, "objects.record_names.total",
                                FR_RECORD_NAME_ENTRY_CAPACITY));
  FR_TRY(fr_repl_write_mem_pair(writer, "objects.record_shape_fields.used",
                                runtime->objects.used_record_shape_fields));
  FR_TRY(fr_repl_write_mem_pair(writer, "objects.record_shape_fields.total",
                                FR_RECORD_SHAPE_FIELD_CAPACITY));
  FR_TRY(fr_repl_write_mem_pair(writer, "objects.record_values.used",
                                runtime->objects.used_record_values));
  FR_TRY(fr_repl_write_mem_pair(writer, "objects.record_values.total",
                                FR_RECORD_VALUE_FIELD_CAPACITY));
  FR_TRY(fr_repl_write_mem_pair(writer, "objects.record_name_bytes.used",
                                runtime->objects.used_record_name_bytes));
  FR_TRY(fr_repl_write_mem_pair(writer, "objects.record_name_bytes.total",
                                FR_RECORD_NAME_BYTE_CAPACITY));
#endif
  return FR_OK;
}

static fr_err_t fr_repl_write_mem_events(fr_runtime_t *runtime,
                                         const fr_repl_writer_t *writer) {
  uint16_t used = 0;

  for (uint16_t i = 0; i < FR_EVENT_BINDING_COUNT; i++) {
    if (runtime->events.entries[i].kind != FR_EVENT_KIND_NONE) {
      used = (uint16_t)(used + 1);
    }
  }
  FR_TRY(fr_repl_write_mem_pair(writer, "events.used", used));
  return fr_repl_write_mem_pair(writer, "events.total", FR_EVENT_BINDING_COUNT);
}

static fr_err_t fr_repl_write_mem(fr_runtime_t *runtime, const char *arg,
                                  uint16_t arg_len,
                                  const fr_repl_writer_t *writer) {
  if (runtime == NULL || writer == NULL) {
    return FR_ERR_INVALID;
  }

  if (arg_len == 0) {
    FR_TRY(fr_repl_write_mem_heap(writer));
    FR_TRY(fr_repl_write_mem_slots(runtime, writer));
    FR_TRY(fr_repl_write_mem_objects(runtime, writer));
    FR_TRY(fr_repl_write_mem_events(runtime, writer));
    return fr_repl_writer_write(writer, "ok\n");
  }
  if (fr_repl_span_equals(arg, arg_len, "heap")) {
    FR_TRY(fr_repl_write_mem_heap(writer));
    return fr_repl_writer_write(writer, "ok\n");
  }
  if (fr_repl_span_equals(arg, arg_len, "slots")) {
    FR_TRY(fr_repl_write_mem_slots(runtime, writer));
    return fr_repl_writer_write(writer, "ok\n");
  }
  if (fr_repl_span_equals(arg, arg_len, "objects")) {
    FR_TRY(fr_repl_write_mem_objects(runtime, writer));
    return fr_repl_writer_write(writer, "ok\n");
  }
  if (fr_repl_span_equals(arg, arg_len, "events")) {
    FR_TRY(fr_repl_write_mem_events(runtime, writer));
    return fr_repl_writer_write(writer, "ok\n");
  }
  return FR_ERR_DOMAIN;
}

#if FR_FEATURE_INTROSPECTION || FR_FEATURE_NUMERIC_SLOT_CALLS
static fr_err_t fr_repl_parse_slot_id(const char *start, uint16_t length,
                                      fr_slot_id_t *out_slot_id) {
  uint32_t value = 0;

  if (start == NULL || out_slot_id == NULL || length == 0) {
    return FR_ERR_INVALID;
  }

  for (uint16_t i = 0; i < length; i++) {
    if (!fr_repl_is_digit(start[i])) {
      return FR_ERR_INVALID;
    }
    value = (value * 10u) + (uint32_t)(start[i] - '0');
    if (value > UINT16_MAX) {
      return FR_ERR_RANGE;
    }
  }

  *out_slot_id = (fr_slot_id_t)value;
  return FR_OK;
}
#endif

#if FR_FEATURE_INTROSPECTION
static const char *fr_repl_base_layer_name(fr_base_layer_t layer) {
  switch (layer) {
  case FR_BASE_LAYER_CORE:
    return "core";
  case FR_BASE_LAYER_TARGET:
    return "target";
  case FR_BASE_LAYER_BOARD:
    return "board";
  case FR_BASE_LAYER_SOURCE:
    return "source";
  case FR_BASE_LAYER_PERSISTENCE:
    return "persistence";
  }

  return NULL;
}

static fr_err_t fr_repl_write_see_tagged(fr_runtime_t *runtime, char *out,
                                         uint16_t out_cap,
                                         fr_tagged_t tagged) {
  return fr_repl_write_tagged_value(runtime, out, out_cap, tagged, true);
}

#if FR_FEATURE_NATIVE_SIGNATURES
static fr_err_t fr_repl_write_value_kind(const fr_repl_writer_t *writer,
                                         fr_native_value_kind_t kind) {
  switch (kind) {
  case FR_NATIVE_VALUE_ANY:
    return fr_repl_writer_write(writer, "any");
  case FR_NATIVE_VALUE_INT:
    return fr_repl_writer_write(writer, "int");
  case FR_NATIVE_VALUE_HANDLE:
    return fr_repl_writer_write(writer, "handle");
  case FR_NATIVE_VALUE_NIL:
    return fr_repl_writer_write(writer, "nil");
  case FR_NATIVE_VALUE_TEXT:
    return fr_repl_writer_write(writer, "text");
  case FR_NATIVE_VALUE_TEXT_OR_BYTES:
    return fr_repl_writer_write(writer, "text|bytes");
  }
  return FR_ERR_INVALID;
}

static bool
fr_repl_signature_is_helped(const fr_native_signature_t *signature) {
  if (signature == NULL || signature->help == NULL) {
    return false;
  }
  for (uint8_t i = 0; i < signature->arg_count; i++) {
    if (signature->params == NULL || signature->params[i].name == NULL) {
      return false;
    }
  }
  return true;
}

static fr_err_t
fr_repl_write_native_signature(const fr_repl_writer_t *writer,
                               const char *func_name,
                               const fr_native_signature_t *signature) {
  FR_TRY(fr_repl_writer_write(writer, func_name));
  FR_TRY(fr_repl_writer_write(writer, "("));
  for (uint8_t i = 0; i < signature->arg_count; i++) {
    if (i > 0) {
      FR_TRY(fr_repl_writer_write(writer, ", "));
    }
    FR_TRY(fr_repl_writer_write(writer, signature->params[i].name));
    FR_TRY(fr_repl_writer_write(writer, ": "));
    FR_TRY(fr_repl_write_value_kind(writer, signature->params[i].type));
  }
  FR_TRY(fr_repl_writer_write(writer, ") -> "));
  FR_TRY(fr_repl_write_value_kind(writer, signature->result));
  FR_TRY(fr_repl_writer_write(writer, "\n"));
  FR_TRY(fr_repl_writer_write(writer, signature->help));
  return fr_repl_writer_write(writer, "\nok\n");
}
#endif

static fr_err_t fr_repl_see_command_slot(fr_runtime_t *runtime,
                                         const char *arg, uint16_t arg_len,
                                         fr_slot_id_t *out_slot_id) {
  bool is_numeric = true;
  char name[FR_PROFILE_MAX_NAME_BYTES + 1];

  if (runtime == NULL || arg == NULL || out_slot_id == NULL ||
      arg_len == 0) {
    return FR_ERR_INVALID;
  }

  for (uint16_t i = 0; i < arg_len; i++) {
    if (!fr_repl_is_digit(arg[i])) {
      is_numeric = false;
    }
  }
  if (is_numeric) {
    return fr_repl_parse_slot_id(arg, arg_len, out_slot_id);
  }

  if ((uint32_t)arg_len + 1 > sizeof(name)) {
    return FR_ERR_RANGE;
  }
  memcpy(name, arg, arg_len);
  name[arg_len] = '\0';
  return fr_slot_id_for_name(runtime, name, out_slot_id);
}

static fr_err_t fr_repl_write_code_listing(fr_runtime_t *runtime,
                                           fr_code_object_id_t code_object_id,
                                           const char *word_name,
                                           const fr_repl_writer_t *writer) {
  fr_instruction_stream_t view;
  fr_instruction_header_t header;
  fr_code_offset_t ip = 0;
  char line[FR_PROFILE_MAX_NAME_BYTES + 32];
  fr_err_t render_err =
      fr_source_render_code(runtime, code_object_id, word_name, writer->write,
                            writer->ctx);

  if (render_err != FR_ERR_UNSUPPORTED) {
    return render_err;
  }

  /* The renderer couldn't rebuild this body. Show the bytecode so see still
   * answers, marked so the reader knows it's the fallback form. */
  FR_TRY(fr_repl_writer_write(writer,
                              ";; source reconstruction unavailable\n"));
  FR_TRY(fr_code_get_instructions(runtime, code_object_id, &view));
  FR_TRY(fr_instruction_read_header(&view, &header));
  if (view.length == header.header_size) {
    return FR_ERR_INVALID;
  }

  ip = header.header_size;
  while (ip < view.length) {
    uint16_t line_len = 0;
    fr_code_offset_t next_ip = 0;

    FR_TRY(fr_instruction_disassemble_at(&view, ip, line, sizeof(line),
                                         &line_len, &next_ip));
    (void)line_len;
    FR_TRY(fr_repl_writer_write(writer, line));
    FR_TRY(fr_repl_writer_write(writer, "\n"));
    ip = next_ip;
  }

  return FR_OK;
}

static fr_err_t fr_repl_eval_see_arg(fr_runtime_t *runtime, const char *arg,
                                     uint16_t arg_len,
                                     const fr_repl_writer_t *writer) {
  fr_slot_id_t slot_id = 0;
  fr_tagged_t tagged = 0;
  fr_code_object_id_t code_object_id = 0;
  fr_base_layer_t layer = FR_BASE_LAYER_CORE;
  const char *base_name = NULL;
  const char *layer_name = NULL;
  const char *slot_name = NULL;
  const char *word_name = NULL;
  char response[FR_REPL_OUTPUT_BYTES];
  uint16_t used = 0;

  FR_TRY(fr_repl_see_command_slot(runtime, arg, arg_len, &slot_id));
  if (slot_id >= fr_slot_count(runtime)) {
    return FR_ERR_NOT_FOUND;
  }

  response[0] = '\0';
  FR_TRY(fr_slot_read(runtime, slot_id, &tagged));
#if FR_FEATURE_NATIVE_SIGNATURES
  {
    fr_native_id_t native_id = 0;

    if (fr_tagged_decode_native_id(tagged, &native_id) == FR_OK) {
      const fr_native_entry_t *entry = NULL;
      const char *func_name = fr_base_slot_name(slot_id);

      FR_TRY(fr_native_get(runtime, native_id, &entry));
      if (func_name != NULL &&
          fr_repl_signature_is_helped(entry->signature)) {
        return fr_repl_write_native_signature(writer, func_name,
                                              entry->signature);
      }
    }
  }
#endif
  base_name = fr_base_slot_name(slot_id);
  slot_name = fr_slot_name(runtime, slot_id);
  if (fr_slot_is_overlay(runtime, slot_id) ||
      (base_name == NULL && slot_name != NULL)) {
    /* A project name is overlay state even when the stored value is nil. */
    FR_TRY(fr_repl_append(response, (uint16_t)sizeof(response), &used,
                          "overlay "));
    word_name = slot_name != NULL ? slot_name : base_name;
  } else if (fr_base_slot_layer(slot_id, &layer) == FR_OK) {
    layer_name = fr_repl_base_layer_name(layer);
    if (layer_name == NULL) {
      return FR_ERR_INVALID;
    }
    FR_TRY(fr_repl_append(response, (uint16_t)sizeof(response), &used,
                          "base "));
    FR_TRY(fr_repl_append(response, (uint16_t)sizeof(response), &used,
                          layer_name));
    FR_TRY(fr_repl_append(response, (uint16_t)sizeof(response), &used, " "));
    word_name = base_name != NULL ? base_name : slot_name;
  } else {
    return FR_ERR_NOT_FOUND;
  }
  FR_TRY(fr_repl_write_see_tagged(
      runtime, &response[used], (uint16_t)(sizeof(response) - used), tagged));
  used = (uint16_t)strlen(response);
  FR_TRY(fr_repl_append(response, (uint16_t)sizeof(response), &used, "\n"));
  FR_TRY(fr_repl_writer_write(writer, response));

  if (fr_tagged_decode_code_object_id(tagged, &code_object_id) == FR_OK) {
    FR_TRY(fr_repl_write_code_listing(runtime, code_object_id, word_name,
                                      writer));
  }

  return fr_repl_writer_write(writer, "ok\n");
}
#endif

#if FR_FEATURE_OVERLAY_APPLY_COMMAND
static fr_err_t fr_repl_hex_value(char ch, uint8_t *out) {
  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  if (ch >= '0' && ch <= '9') {
    *out = (uint8_t)(ch - '0');
    return FR_OK;
  }
  if (ch >= 'a' && ch <= 'f') {
    *out = (uint8_t)(ch - 'a' + 10);
    return FR_OK;
  }
  if (ch >= 'A' && ch <= 'F') {
    *out = (uint8_t)(ch - 'A' + 10);
    return FR_OK;
  }
  return FR_ERR_INVALID;
}

static fr_err_t fr_repl_decode_hex_bytes(const char *text, uint8_t bytes[],
                                         uint16_t cap,
                                         uint16_t *out_length) {
  uint16_t used = 0;
  bool have_high = false;
  uint8_t high = 0;

  if (text == NULL || bytes == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }

  while (*text != '\0') {
    uint8_t value = 0;

    if (fr_repl_is_space(*text)) {
      text += 1;
      continue;
    }
    FR_TRY(fr_repl_hex_value(*text, &value));
    if (have_high) {
      if (used >= cap) {
        return FR_ERR_CAPACITY;
      }
      bytes[used] = (uint8_t)((high << 4) | value);
      used = (uint16_t)(used + 1);
      have_high = false;
    } else {
      high = value;
      have_high = true;
    }
    text += 1;
  }

  if (have_high || used == 0) {
    return FR_ERR_INVALID;
  }
  *out_length = used;
  return FR_OK;
}

static fr_err_t fr_repl_eval_apply_arg(fr_runtime_t *runtime, const char *arg,
                                       const fr_repl_writer_t *writer) {
  uint16_t byte_count = 0;

  if (runtime == NULL || arg == NULL || writer == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_repl_decode_hex_bytes(arg, fr_repl_wire_bytes,
                                  (uint16_t)sizeof(fr_repl_wire_bytes),
                                  &byte_count));
  FR_TRY(fr_overlay_update_decode(fr_repl_wire_bytes, byte_count,
                                  &fr_repl_apply_decoded));
  FR_TRY(fr_overlay_apply(runtime, &fr_repl_apply_decoded.update));
#if FR_FEATURE_PERSISTENCE
  fr_persist_session_install_tier_stamp_overlay(runtime,
                                                &fr_repl_apply_decoded.update);
#endif
  return fr_repl_writer_write(writer, "ok\n");
}

static fr_err_t fr_repl_eval_run_arg(fr_runtime_t *runtime, const char *arg,
                                     const fr_repl_writer_t *writer) {
  fr_instruction_stream_t instructions = {0};
  fr_tagged_t result = 0;
  uint16_t byte_count = 0;

  if (runtime == NULL || arg == NULL || writer == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_repl_decode_hex_bytes(arg, fr_repl_wire_bytes,
                                  (uint16_t)sizeof(fr_repl_wire_bytes),
                                  &byte_count));
  FR_TRY(fr_instruction_stream_init(&instructions, fr_repl_wire_bytes,
                                    byte_count));
  FR_TRY(fr_vm_run_instruction_stream(runtime, &instructions, &result));
  return fr_repl_writer_write_eval_response(writer, runtime, result);
}
#endif

static fr_err_t fr_repl_zero_arg_call_slot(fr_runtime_t *runtime,
                                           const char *line,
                                           fr_slot_id_t *out_slot_id) {
  char name[FR_PROFILE_MAX_NAME_BYTES + 1];
  const char *cursor = line;
  const char *name_start = NULL;
#if FR_FEATURE_NUMERIC_SLOT_CALLS
  bool is_numeric = true;
#endif
  uint16_t name_len = 0;

  if (runtime == NULL || line == NULL || out_slot_id == NULL) {
    return FR_ERR_INVALID;
  }
  while (fr_repl_is_space(*cursor)) {
    cursor += 1;
  }
  name_start = cursor;
  while (*cursor != '\0' && *cursor != ':' && !fr_repl_is_space(*cursor)) {
    cursor += 1;
  }
  if (*cursor != ':' || cursor == name_start) {
    return FR_ERR_NOT_FOUND;
  }
  name_len = (uint16_t)(cursor - name_start);
  if ((uint32_t)name_len + 1 > sizeof(name)) {
    return FR_ERR_RANGE;
  }
  cursor += 1;
  while (fr_repl_is_space(*cursor)) {
    cursor += 1;
  }
  if (*cursor != '\0') {
    return FR_ERR_NOT_FOUND;
  }
#if FR_FEATURE_NUMERIC_SLOT_CALLS
  for (uint16_t i = 0; i < name_len; i++) {
    if (!fr_repl_is_digit(name_start[i])) {
      is_numeric = false;
      break;
    }
  }
  if (is_numeric) {
    return fr_repl_parse_slot_id(name_start, name_len, out_slot_id);
  }
#endif
  memcpy(name, name_start, name_len);
  name[name_len] = '\0';
  return fr_slot_id_for_name(runtime, name, out_slot_id);
}

static fr_err_t fr_repl_eval_zero_arg_call(fr_runtime_t *runtime,
                                           const char *line,
                                           fr_tagged_t *out,
                                           bool *out_matched) {
  const fr_native_entry_t *entry = NULL;
  fr_code_object_id_t code_object_id = 0;
  fr_native_id_t native_id = 0;
  fr_slot_id_t slot_id = 0;
  fr_tagged_t tagged = 0;
  fr_err_t err = FR_OK;

  if (out_matched == NULL) {
    return FR_ERR_INVALID;
  }
  *out_matched = false;

  err = fr_repl_zero_arg_call_slot(runtime, line, &slot_id);
  if (err == FR_ERR_NOT_FOUND) {
    return FR_OK;
  }
  FR_TRY(err);
  *out_matched = true;

  if (slot_id == FR_SLOT_BOOT) {
    FR_TRY(fr_vm_run_boot(runtime, out));
    *out = fr_tagged_nil();
    return FR_OK;
  }

  FR_TRY(fr_slot_read(runtime, slot_id, &tagged));
  if (fr_tagged_decode_code_object_id(tagged, &code_object_id) == FR_OK) {
    (void)code_object_id;
    return fr_vm_run_slot(runtime, slot_id, out);
  }
  if (fr_tagged_decode_native_id(tagged, &native_id) != FR_OK) {
    return FR_ERR_UNSUPPORTED;
  }
  FR_TRY(fr_native_get(runtime, native_id, &entry));
  if (entry->arity != 0) {
    return FR_ERR_INVALID;
  }
  return fr_native_call(runtime, entry, NULL, 0, out);
}

static fr_err_t fr_repl_eval_bare_word(fr_runtime_t *runtime, const char *line,
                                       fr_tagged_t *out, bool *out_matched,
                                       bool *out_ran_command) {
  const fr_native_entry_t *entry = NULL;
  fr_native_id_t native_id = 0;
  fr_slot_id_t slot_id = 0;
  fr_tagged_t tagged = 0;
  fr_err_t err = FR_OK;

  if (out == NULL || out_matched == NULL || out_ran_command == NULL) {
    return FR_ERR_INVALID;
  }
  *out_matched = false;
  *out_ran_command = false;

  err = fr_slot_id_for_name(runtime, line, &slot_id);
  if (err == FR_ERR_NOT_FOUND) {
    return FR_OK;
  }
  FR_TRY(err);
  *out_matched = true;

  FR_TRY(fr_slot_read(runtime, slot_id, &tagged));

  if (fr_tagged_decode_native_id(tagged, &native_id) == FR_OK) {
    FR_TRY(fr_native_get(runtime, native_id, &entry));
    if (entry->arity == 0) {
      *out_ran_command = true;
      return fr_native_call(runtime, entry, NULL, 0, out);
    }
  }

  *out = tagged;
  return FR_OK;
}

#if FR_FEATURE_COMPILER
static fr_err_t
fr_repl_eval_value_binding(fr_runtime_t *runtime,
                           const fr_compile_value_binding_t *binding,
                           fr_tagged_t *out) {
  if (runtime == NULL || binding == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_vm_run_instruction_stream(runtime, &binding->instructions, out));
  if (binding->has_slot_name) {
    FR_TRY(fr_slot_bind_project_name(runtime, binding->slot_name.name,
                                     binding->slot_name.slot_id));
  }
  return FR_OK;
}
#endif

static fr_err_t fr_repl_eval_line_to_writer_inner(fr_runtime_t *runtime,
                                                  const char *line,
                                                  const fr_repl_writer_t *writer,
                                                  fr_diagnostic_t *diag) {
  fr_repl_command_t command = {0};
  fr_tagged_t result = 0;
  bool matched = false;
  bool ran_command = false;

  if (runtime == NULL || line == NULL || writer == NULL) {
    return FR_ERR_INVALID;
  }

  fr_runtime_clear_interrupt(runtime);

  FR_TRY(fr_repl_parse_recognized_command(line, &command));

  if (command.kind == FR_REPL_COMMAND_BLANK) {
    return fr_repl_writer_write(writer, "ok\n");
  }

  if (command.kind == FR_REPL_COMMAND_STATUS) {
    return fr_repl_write_status(writer);
  }

  if (command.kind == FR_REPL_COMMAND_SEE) {
#if FR_FEATURE_INTROSPECTION
    return fr_repl_eval_see_arg(runtime, command.arg, command.arg_len, writer);
#else
    return FR_ERR_UNSUPPORTED;
#endif
  }

  if (command.kind == FR_REPL_COMMAND_APPLY) {
#if FR_FEATURE_OVERLAY_APPLY_COMMAND
    return fr_repl_eval_apply_arg(runtime, command.arg, writer);
#else
    return FR_ERR_UNSUPPORTED;
#endif
  }

  if (command.kind == FR_REPL_COMMAND_RUN) {
#if FR_FEATURE_OVERLAY_APPLY_COMMAND
    return fr_repl_eval_run_arg(runtime, command.arg, writer);
#else
    return FR_ERR_UNSUPPORTED;
#endif
  }

  if (command.kind == FR_REPL_COMMAND_CLEAR) {
    FR_TRY(fr_runtime_clear_project(runtime));
    return fr_repl_writer_write(writer, "ok\n");
  }

  if (command.kind == FR_REPL_COMMAND_INSTALL_LIBRARY) {
#if FR_FEATURE_PERSISTENCE
    FR_TRY(fr_persist_install_library(runtime));
#endif
    runtime->install_tier = FR_INSTALL_TIER_LIBRARY;
    return fr_repl_writer_write(writer, "ok\n");
  }

  if (command.kind == FR_REPL_COMMAND_INSTALL_USER) {
    runtime->install_tier = FR_INSTALL_TIER_USER;
    return fr_repl_writer_write(writer, "ok\n");
  }

  if (command.kind == FR_REPL_COMMAND_WIPE_USER) {
#if FR_FEATURE_PERSISTENCE
    FR_TRY(fr_persist_wipe_user(runtime));
    return fr_repl_writer_write(writer, "ok\n");
#else
    return FR_ERR_UNSUPPORTED;
#endif
  }

  if (command.kind == FR_REPL_COMMAND_WORDS) {
#if FR_FEATURE_INTROSPECTION
    return fr_repl_write_words(runtime, writer);
#else
    return FR_ERR_UNSUPPORTED;
#endif
  }

  if (command.kind == FR_REPL_COMMAND_EVENTS) {
#if FR_FEATURE_EVENTS
    return fr_repl_write_events(runtime, writer);
#else
    return FR_ERR_UNSUPPORTED;
#endif
  }

  if (command.kind == FR_REPL_COMMAND_MEM) {
    return fr_repl_write_mem(runtime, command.arg, command.arg_len, writer);
  }

  FR_TRY(fr_repl_eval_zero_arg_call(runtime, line, &result, &matched));
  if (matched) {
    return fr_repl_writer_write_eval_response(writer, runtime, result);
  }

  FR_TRY(fr_repl_eval_bare_word(runtime, line, &result, &matched,
                                &ran_command));
  if (matched) {
    if (ran_command) {
      return fr_repl_writer_write(writer, "ok\n");
    }
    return fr_repl_writer_write_tagged_response(writer, runtime, result);
  }

#if FR_FEATURE_COMPILER
  {
    /* T15-hwfix: static rather than automatic — fr_compile_overlay_update_t
     * embeds profile-sized text buffers (FR_PROFILE_MAX_TEXT_LENGTH + 8 body
     * buffers); on esp32_plain that's ~36KB which overflows the REPL task
     * stack. The REPL processes one line at a time on a single task, so
     * static storage is safe. */
    static fr_compile_overlay_update_t compiled;
    memset(&compiled, 0, sizeof(compiled));
    if (diag != NULL) {
      *diag = (fr_diagnostic_t){0};
    }
    fr_err_t err = fr_compile_overlay_update_for_runtime_with_diagnostic(
        runtime, line, &compiled, diag);

    if (err == FR_OK) {
      FR_TRY(fr_overlay_apply(runtime, &compiled.overlay_update));
#if FR_FEATURE_PERSISTENCE
      fr_persist_session_install_tier_stamp_overlay(runtime,
                                                    &compiled.overlay_update);
      if (runtime->install_tier == FR_INSTALL_TIER_LIBRARY) {
        FR_TRY(fr_persist_save_full(runtime));
      }
#endif
      return fr_repl_writer_write(writer, "ok\n");
    }
    if (err == FR_ERR_UNSUPPORTED) {
      fr_compile_value_binding_t binding = {0};
      fr_err_t bind_err = FR_OK;

      if (diag != NULL) {
        *diag = (fr_diagnostic_t){0};
      }
      bind_err = fr_compile_value_binding_for_runtime_with_diagnostic(
          runtime, line, &binding, diag);

      if (bind_err == FR_OK) {
        FR_TRY(fr_repl_eval_value_binding(runtime, &binding, &result));
#if FR_FEATURE_PERSISTENCE
        fr_persist_session_install_tier_stamp_slot(runtime, binding.slot_id);
        if (runtime->install_tier == FR_INSTALL_TIER_LIBRARY) {
          FR_TRY(fr_persist_save_full(runtime));
        }
#endif
        return fr_repl_writer_write(writer, "ok\n");
      }
      if (bind_err != FR_ERR_UNSUPPORTED) {
        return bind_err;
      }
    }
    if (err == FR_ERR_INVALID && fr_repl_line_starts_definition(line)) {
      return err;
    }
    if (err != FR_ERR_INVALID) {
      return err;
    }
  }

  {
    fr_compile_expression_t expression = {0};

    if (diag != NULL) {
      *diag = (fr_diagnostic_t){0};
    }
    FR_TRY(fr_compile_expression_for_runtime_with_diagnostic(
        runtime, line, &expression, diag));
    FR_TRY(fr_vm_run_instruction_stream(runtime, &expression.instructions,
                                        &result));
    return fr_repl_writer_write_eval_response(writer, runtime, result);
  }
#else
  return FR_ERR_UNSUPPORTED;
#endif
}

static fr_err_t fr_repl_eval_line_to_writer(fr_runtime_t *runtime,
                                            const char *line,
                                            const fr_repl_writer_t *writer,
                                            fr_diagnostic_t *diag) {
  fr_err_t err = FR_OK;

  if (diag != NULL) {
    *diag = (fr_diagnostic_t){0};
  }
  err = fr_repl_eval_line_to_writer_inner(runtime, line, writer, diag);
#if FR_FEATURE_BYTES
  fr_bytes_reset_if_outermost(runtime);
#endif
  return err;
}

fr_err_t fr_repl_eval_line(fr_runtime_t *runtime, const char *line, char *out,
                           uint16_t out_cap) {
  fr_repl_buffer_writer_t buffer = {
      .out = out,
      .out_cap = out_cap,
  };
  const fr_repl_writer_t writer = {
      .ctx = &buffer,
      .write = fr_repl_buffer_writer_write,
  };
  fr_diagnostic_t diag = {0};

  if (runtime == NULL || line == NULL || out == NULL || out_cap == 0) {
    return FR_ERR_INVALID;
  }
  out[0] = '\0';
  return fr_repl_eval_line_to_writer(runtime, line, &writer, &diag);
}

/* Idle-servicing body: drain queued interrupt/timer candidates and run any
 * pending handler bodies, so events fire at the prompt instead of only while a
 * program runs. The platform calls this while read_line waits for input.
 *
 * A handler fault surfaces as an async error line (so it is not lost, the way a
 * fault during a running program surfaces on the response); Ctrl-C is expected
 * and stays silent. After an interrupt, runtime->interrupted remains set until
 * the next eval clears it, so idle dispatch pauses until the user acts — an
 * acceptable consequence of having just interrupted. Always returns OK: a fault
 * here must never break the read loop. */
static fr_err_t fr_repl_service_events(void *ctx) {
  fr_runtime_t *runtime = (fr_runtime_t *)ctx;
  fr_err_t err;

  if (fr_event_drain(runtime) != FR_OK) {
    return FR_OK;
  }
  fr_event_report_overflow(runtime);
  err = fr_event_dispatch(runtime);
  if (err != FR_OK && err != FR_ERR_INTERRUPTED) {
    char response[FR_REPL_OUTPUT_BYTES];
    if (fr_repl_write_error(response, (uint16_t)sizeof(response), err, NULL,
                            NULL) == FR_OK) {
      (void)fr_platform_write_text("! ");
      (void)fr_platform_write_text(response);
    }
  }
  return FR_OK;
}

fr_err_t fr_repl_run(fr_runtime_t *runtime, const fr_repl_io_t *io) {
  char line[FR_REPL_LINE_BYTES];
  const fr_repl_writer_t writer = {
      .ctx = (void *)io,
      .write = fr_repl_io_writer_write,
  };
  bool eof = false;

  if (runtime == NULL || io == NULL || io->read_line == NULL ||
      io->write_text == NULL) {
    return FR_ERR_INVALID;
  }

  fr_platform_set_idle_handler(fr_repl_service_events, runtime);

  /* T12L-7 D3: each fresh REPL session starts in user-install mode. */
  runtime->install_tier = FR_INSTALL_TIER_USER;

  while (true) {
    FR_TRY(io->write_text("> "));
    FR_TRY(io->read_line(line, (uint16_t)sizeof(line), &eof));
    if (eof) {
      return FR_OK;
    }

    fr_diagnostic_t diag = {0};
    fr_err_t err = fr_repl_eval_line_to_writer(runtime, line, &writer, &diag);
    if (err != FR_OK) {
      char response[FR_REPL_OUTPUT_BYTES];

      FR_TRY(fr_repl_write_error(response, (uint16_t)sizeof(response), err,
                                 line, &diag));
      FR_TRY(io->write_text(response));
    }
  }
}

fr_err_t fr_repl_run_platform(fr_runtime_t *runtime) {
  const fr_repl_io_t io = {
      .read_line = fr_platform_read_line,
      .write_text = fr_platform_write_text,
  };

  return fr_repl_run(runtime, &io);
}
