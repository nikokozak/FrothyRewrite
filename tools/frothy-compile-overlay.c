#include "base_image.h"
#include "compile.h"
#include "image.h"
#include "native.h"
#include "profile.h"
#include "repl.h"
#include "slot.h"
#include "tagged.h"
#include "types.h"

#if FR_FEATURE_TEXT
#include "parse.h"
#endif

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static fr_runtime_t runtime;
static fr_compile_overlay_update_t pending;
typedef enum pending_kind_t {
  PENDING_NONE,
  PENDING_UPDATE,
  PENDING_CLEAR,
} pending_kind_t;
static pending_kind_t pending_kind = PENDING_NONE;

#ifndef FR_HOST_TINY_NAMES_MODE
#define FR_HOST_TINY_NAMES_MODE 0
#endif

static void write_hex_byte(uint8_t byte) {
  static const char digits[] = "0123456789abcdef";

  (void)putchar(digits[byte >> 4]);
  (void)putchar(digits[byte & 0x0fu]);
}

static void trim_line(char *line) {
  size_t length = strlen(line);

  while (length > 0 &&
         (line[length - 1] == '\n' || line[length - 1] == '\r')) {
    length -= 1;
    line[length] = '\0';
  }
}

static bool line_complete(const char *line) {
  size_t length = strlen(line);

  return length == 0 || line[length - 1] == '\n' || line[length - 1] == '\r';
}

static bool source_has_colon_call_shape(const char *source) {
  return source != NULL && strchr(source, ':') != NULL;
}

static void discard_line(void) {
  int ch = 0;

  do {
    ch = getchar();
  } while (ch != '\n' && ch != EOF);
}

static int print_err(fr_err_t err) {
  printf("err %u\n", (unsigned)err);
  fflush(stdout);
  return 0;
}

static int print_err_apply_capacity(uint16_t cap, uint16_t required) {
  printf("err %u apply_bytes=%u required=%u\n", (unsigned)FR_ERR_CAPACITY,
         (unsigned)cap, (unsigned)required);
  fflush(stdout);
  return 0;
}

static int print_ok(void) {
  fputs("ok\n", stdout);
  fflush(stdout);
  return 0;
}

static int handle_commit(void) {
  if (pending_kind == PENDING_NONE) {
    return print_err(FR_ERR_INVALID);
  }

  if (pending_kind == PENDING_CLEAR) {
    fr_err_t err = fr_runtime_clear_project(&runtime);

    pending_kind = PENDING_NONE;
    if (err != FR_OK) {
      return print_err(err);
    }
    return print_ok();
  }

  fr_err_t err = fr_overlay_apply(&runtime, &pending.overlay_update);
  pending_kind = PENDING_NONE;
  if (err != FR_OK) {
    return print_err(err);
  }

  return print_ok();
}

static int handle_drop(void) {
  pending_kind = PENDING_NONE;
  return print_ok();
}

static int handle_target(void) {
  printf("target profile=%s profile_hash=%08" PRIx32
         " max_slots=%u host_names=%u word_size=%u int_min=%" PRId32
         " int_max=%" PRId32 " apply_bytes=%u\n",
         fr_profile_contract_name(), fr_profile_hash(),
         (unsigned)FR_PROFILE_MAX_SLOTS,
         (unsigned)FR_PROFILE_MAX_OVERLAY_NAMES, (unsigned)FR_WORD_SIZE,
         (int32_t)FR_TAGGED_INT_MIN, (int32_t)FR_TAGGED_INT_MAX,
         (unsigned)FR_REPL_APPLY_BYTES);
  fflush(stdout);
  return 0;
}

#if FR_HOST_TINY_NAMES_MODE
static bool is_space(char ch) { return ch == ' ' || ch == '\t'; }

static bool is_digit(char ch) { return ch >= '0' && ch <= '9'; }

static int print_send_slot_call(fr_slot_id_t slot_id) {
  printf("send %u:\n", (unsigned)slot_id);
  fflush(stdout);
  return 0;
}

static int print_send_source(const char *source) {
  printf("send %s\n", source);
  fflush(stdout);
  return 0;
}

static bool parse_zero_arg_call(const char *line, char *name, size_t name_cap) {
  const char *cursor = line;
  const char *name_start = NULL;
  size_t name_len = 0;

  while (is_space(*cursor)) {
    cursor += 1;
  }
  name_start = cursor;
  while (*cursor != '\0' && *cursor != ':' && !is_space(*cursor)) {
    cursor += 1;
  }
  if (*cursor != ':' || cursor == name_start) {
    return false;
  }

  name_len = (size_t)(cursor - name_start);
  if (name_len + 1 > name_cap) {
    return false;
  }
  cursor += 1;
  while (is_space(*cursor)) {
    cursor += 1;
  }
  if (*cursor != '\0') {
    return false;
  }

  memcpy(name, name_start, name_len);
  name[name_len] = '\0';
  return true;
}

static bool parse_bare_word(const char *line, char *name, size_t name_cap) {
  const char *cursor = line;
  const char *name_start = NULL;
  size_t name_len = 0;

  while (is_space(*cursor)) {
    cursor += 1;
  }
  name_start = cursor;
  while (*cursor != '\0' && !is_space(*cursor)) {
    cursor += 1;
  }
  name_len = (size_t)(cursor - name_start);
  if (name_len == 0 || name_len + 1 > name_cap) {
    return false;
  }
  while (is_space(*cursor)) {
    cursor += 1;
  }
  if (*cursor != '\0') {
    return false;
  }

  memcpy(name, name_start, name_len);
  name[name_len] = '\0';
  return true;
}

static int handle_named_slot_call(const char *source, bool *out_handled) {
  char name[FR_PROFILE_MAX_NAME_BYTES + 1];
  fr_slot_id_t slot_id = 0;
  fr_err_t err = FR_OK;

  *out_handled = false;
  if (!parse_zero_arg_call(source, name, sizeof(name))) {
    return 0;
  }
  err = fr_slot_id_for_name(&runtime, name, &slot_id);
  if (err == FR_OK) {
    *out_handled = true;
    return print_send_slot_call(slot_id);
  }
  if (err != FR_ERR_NOT_FOUND) {
    *out_handled = true;
    return print_err(err);
  }
  if (!is_digit(name[0])) {
    *out_handled = true;
    return print_err(FR_ERR_NOT_FOUND);
  }
  *out_handled = true;
  return print_send_source(source);
}

static int handle_bare_zero_arg_command(const char *source, bool *out_handled) {
  char name[FR_PROFILE_MAX_NAME_BYTES + 1];
  const fr_native_entry_t *entry = NULL;
  fr_slot_id_t slot_id = 0;
  fr_native_id_t native_id = 0;
  fr_tagged_t tagged = 0;
  fr_err_t err = FR_OK;

  *out_handled = false;
  if (!parse_bare_word(source, name, sizeof(name))) {
    return 0;
  }

  err = fr_slot_id_for_name(&runtime, name, &slot_id);
  if (err == FR_ERR_NOT_FOUND) {
    return 0;
  }
  if (err != FR_OK) {
    *out_handled = true;
    return print_err(err);
  }

  err = fr_slot_read(&runtime, slot_id, &tagged);
  if (err != FR_OK) {
    *out_handled = true;
    return print_err(err);
  }
  if (fr_tagged_decode_native_id(tagged, &native_id) != FR_OK) {
    return 0;
  }
  err = fr_native_get(&runtime, native_id, &entry);
  if (err != FR_OK) {
    *out_handled = true;
    return print_err(err);
  }
  if (entry->arity != 0) {
    return 0;
  }

  *out_handled = true;
  return print_send_slot_call(slot_id);
}
#endif

static int print_send_run(const fr_instruction_stream_t *instructions) {
  const uint16_t device_prefix_len = (uint16_t)(sizeof("run ") - 1);

  if (instructions == NULL || instructions->bytes == NULL) {
    return print_err(FR_ERR_INVALID);
  }
  if (instructions->length > FR_REPL_APPLY_BYTES) {
    return print_err(FR_ERR_RANGE);
  }
  if ((uint32_t)device_prefix_len + ((uint32_t)instructions->length * 2u) + 1u >
      FR_PROFILE_REPL_LINE_BYTES) {
    return print_err(FR_ERR_RANGE);
  }

  fputs("send run ", stdout);
  for (uint16_t i = 0; i < instructions->length; i++) {
    write_hex_byte(instructions->bytes[i]);
  }
  fputc('\n', stdout);
  fflush(stdout);
  return 0;
}

#if FR_FEATURE_TEXT
static bool line_has_text_literal(const fr_parse_line_t *line) {
  for (uint8_t i = 0; i < line->expr_count; i++) {
    if (line->exprs[i].kind == FR_PARSE_EXPR_TEXT) {
      return true;
    }
  }
  return false;
}
#endif

static int handle_expression(const char *source) {
  fr_compile_expression_t expression;
  fr_err_t err = FR_OK;

  if (!source_has_colon_call_shape(source)) {
    fputs("pass\n", stdout);
    fflush(stdout);
    return 0;
  }

#if FR_FEATURE_TEXT
  {
    fr_parse_line_t line = {0};
    fr_parse_expr_id_t root = 0;

    if (fr_parse_expression_line(source, &line, &root) == FR_OK &&
        line_has_text_literal(&line)) {
      fputs("pass\n", stdout);
      fflush(stdout);
      return 0;
    }
  }
#endif

  err = fr_compile_expression_for_runtime(&runtime, source, &expression);

  if (err == FR_OK) {
    return print_send_run(&expression.instructions);
  }
  return print_err(err);
}

static int handle_source(const char *source) {
  uint8_t bytes[FR_REPL_APPLY_BYTES];
  uint8_t measure_bytes[FR_REPL_APPLY_BYTES + 32u];
  uint16_t length = 0;
  uint16_t measured_length = 0;
  fr_err_t err = FR_OK;
  const fr_overlay_update_t *device_update = NULL;
#if FR_HOST_TINY_NAMES_MODE
  bool handled = false;
  fr_overlay_update_t device_update_without_names;
#endif

  if (pending_kind != PENDING_NONE) {
    return print_err(FR_ERR_INVALID);
  }

  if (strcmp(source, "clear") == 0) {
    pending_kind = PENDING_CLEAR;
    fputs("clear\n", stdout);
    fflush(stdout);
    return 0;
  }

#if FR_HOST_TINY_NAMES_MODE
  if (handle_named_slot_call(source, &handled) != 0 || handled) {
    return 0;
  }
  if (handle_bare_zero_arg_command(source, &handled) != 0 || handled) {
    return 0;
  }
#endif

  err = fr_compile_overlay_update_for_runtime(&runtime, source, &pending);
  if (err == FR_ERR_INVALID) {
    return handle_expression(source);
  }
  if (err != FR_OK) {
    return print_err(err);
  }

  device_update = &pending.overlay_update;
#if FR_HOST_TINY_NAMES_MODE
  device_update_without_names = pending.overlay_update;
  device_update_without_names.slot_names = NULL;
  device_update_without_names.slot_name_count = 0;
  device_update = &device_update_without_names;
#endif

  err = fr_overlay_update_encode(device_update, bytes, (uint16_t)sizeof(bytes),
                                 &length);
  if (err != FR_OK) {
    pending_kind = PENDING_NONE;
    if (err == FR_ERR_CAPACITY &&
        fr_overlay_update_encode(device_update, measure_bytes,
                                 (uint16_t)sizeof(measure_bytes),
                                 &measured_length) == FR_OK &&
        measured_length > sizeof(bytes)) {
      return print_err_apply_capacity((uint16_t)sizeof(bytes),
                                      measured_length);
    }
    return print_err(err);
  }

  pending_kind = PENDING_UPDATE;
  fputs("apply ", stdout);
  for (uint16_t i = 0; i < length; i++) {
    write_hex_byte(bytes[i]);
  }
  fputc('\n', stdout);
  fflush(stdout);
  return 0;
}

int main(void) {
  char line[FR_PROFILE_REPL_LINE_BYTES];
  fr_err_t err = fr_base_image_install(&runtime);

  if (err != FR_OK) {
    return (int)err;
  }

  while (fgets(line, sizeof(line), stdin) != NULL) {
    if (!line_complete(line)) {
      discard_line();
      (void)print_err(FR_ERR_RANGE);
      continue;
    }

    trim_line(line);
    if (strcmp(line, "@commit") == 0) {
      (void)handle_commit();
    } else if (strcmp(line, "@drop") == 0) {
      (void)handle_drop();
    } else if (strcmp(line, "@target") == 0) {
      (void)handle_target();
    } else {
      (void)handle_source(line);
    }
  }

  return 0;
}
