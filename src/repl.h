#pragma once

#include "runtime.h"

#if FR_FEATURE_REPL

enum {
  FR_REPL_LINE_BYTES = FR_PROFILE_REPL_LINE_BYTES,
  FR_REPL_APPLY_PREFIX_BYTES = sizeof("apply ") - 1,
  FR_REPL_APPLY_BYTES =
      (FR_REPL_LINE_BYTES - FR_REPL_APPLY_PREFIX_BYTES - 1) / 2,
  FR_REPL_OUTPUT_BYTES = 64,
};

typedef fr_err_t (*fr_repl_read_line_fn)(char *line, uint16_t cap,
                                         bool *out_eof);
typedef fr_err_t (*fr_repl_write_text_fn)(const char *text);

typedef struct fr_repl_io_t {
  fr_repl_read_line_fn read_line;
  fr_repl_write_text_fn write_text;
} fr_repl_io_t;

fr_err_t fr_repl_eval_line(fr_runtime_t *runtime, const char *line, char *out,
                           uint16_t out_cap);
fr_err_t fr_repl_startup_restore_and_boot(fr_runtime_t *runtime);
fr_err_t fr_repl_run(fr_runtime_t *runtime, const fr_repl_io_t *io);
fr_err_t fr_repl_run_platform(fr_runtime_t *runtime);

#endif
