#include "froth.h"
#include "compile.h"
#include "base_image.h"
#include "repl.h"
#include "vm.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
  FUZZ_MAX_LINES = 8,
  FUZZ_LINE_BYTES = 192,
  FUZZ_MAX_BYTES = FR_PROFILE_MAX_INSTRUCTION_BYTES,
  FUZZ_CHILD_TIMEOUT_MS = 250,
  FUZZ_CORRUPTION_MIN_REJECT_PERCENT = 15,
};

typedef enum fuzz_mode_t {
  FUZZ_MODE_SOURCE,
  FUZZ_MODE_CORRUPTION,
} fuzz_mode_t;

typedef enum fuzz_seed_kind_t {
  FUZZ_SEED_EXPRESSION,
  FUZZ_SEED_OVERLAY,
} fuzz_seed_kind_t;

typedef enum fuzz_observation_kind_t {
  FUZZ_OBS_VALUE,
  FUZZ_OBS_TIMEOUT,
  FUZZ_OBS_SIGNAL,
  FUZZ_OBS_EXIT,
  FUZZ_OBS_IO,
} fuzz_observation_kind_t;

typedef struct fuzz_rng_t {
  uint32_t state;
} fuzz_rng_t;

typedef struct fuzz_source_case_t {
  uint8_t line_count;
  char lines[FUZZ_MAX_LINES][FUZZ_LINE_BYTES];
} fuzz_source_case_t;

typedef struct fuzz_seed_case_t {
  fuzz_seed_kind_t kind;
  uint8_t setup_count;
  char setup[FUZZ_MAX_LINES][FUZZ_LINE_BYTES];
  char source[FUZZ_LINE_BYTES];
  char run_line[FUZZ_LINE_BYTES];
} fuzz_seed_case_t;

typedef struct fuzz_mutant_t {
  uint8_t bytes[FUZZ_MAX_BYTES];
  uint16_t length;
} fuzz_mutant_t;

typedef struct fuzz_observation_t {
  fuzz_observation_kind_t kind;
  fr_err_t err;
  fr_tagged_t tagged;
  char out[FR_REPL_OUTPUT_BYTES];
} fuzz_observation_t;

typedef struct fuzz_child_run_t {
  const fr_runtime_t *base;
  const fuzz_seed_case_t *seed;
  const fuzz_mutant_t *mutant;
} fuzz_child_run_t;

typedef fr_err_t (*fuzz_child_fn_t)(void *ctx, fuzz_observation_t *out);

static uint32_t fuzz_rng_next(fuzz_rng_t *rng) {
  uint32_t x = rng->state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng->state = x == 0 ? 0x6d2b79f5u : x;
  return rng->state;
}

static uint32_t fuzz_rng_below(fuzz_rng_t *rng, uint32_t cap) {
  if (cap == 0) {
    return 0;
  }
  return fuzz_rng_next(rng) % cap;
}

static int32_t fuzz_int_value(fuzz_rng_t *rng) {
  static const int32_t values[] = {
      0, 1, -1, 2, -2, 3, 7, 42, -42,
      FR_TAGGED_INT_MAX, FR_TAGGED_INT_MIN,
      FR_TAGGED_INT_MAX - 1, FR_TAGGED_INT_MIN + 1,
  };
  return values[fuzz_rng_below(rng, (uint32_t)(sizeof(values) / sizeof(values[0])))];
}

static void fuzz_name(char prefix, uint32_t index, char *out, uint16_t cap) {
  (void)snprintf(out, cap, "%c%" PRIu32, prefix, index % 100000u);
}

static void fuzz_hex_print_bytes(const uint8_t *bytes, uint16_t length) {
  static const char hex[] = "0123456789abcdef";
  for (uint16_t i = 0; i < length; i++) {
    putchar(hex[bytes[i] >> 4]);
    putchar(hex[bytes[i] & 0x0f]);
  }
}

static void fuzz_hex_print_text(const char *text) {
  fuzz_hex_print_bytes((const uint8_t *)text, (uint16_t)strlen(text));
}

static void fuzz_write_u16(uint8_t *bytes, uint16_t value) {
  bytes[0] = (uint8_t)(value & 0xffu);
  bytes[1] = (uint8_t)(value >> 8);
}

static void fuzz_write_i32(uint8_t *bytes, int32_t value) {
  uint32_t raw = (uint32_t)value;
  bytes[0] = (uint8_t)(raw & 0xffu);
  bytes[1] = (uint8_t)((raw >> 8) & 0xffu);
  bytes[2] = (uint8_t)((raw >> 16) & 0xffu);
  bytes[3] = (uint8_t)((raw >> 24) & 0xffu);
}

static void fuzz_observation_print(const fuzz_observation_t *obs) {
  switch (obs->kind) {
  case FUZZ_OBS_VALUE:
    printf("value err=%d tagged=%08" PRIx32 " out=", (int)obs->err,
           (uint32_t)obs->tagged);
    fuzz_hex_print_text(obs->out);
    break;
  case FUZZ_OBS_TIMEOUT:
    printf("timeout err=%d tagged=00000000 out=", (int)obs->err);
    break;
  case FUZZ_OBS_SIGNAL:
    printf("signal err=%d tagged=00000000 out=", (int)obs->err);
    break;
  case FUZZ_OBS_EXIT:
    printf("exit err=%d tagged=00000000 out=", (int)obs->err);
    break;
  case FUZZ_OBS_IO:
  default:
    printf("io err=%d tagged=00000000 out=", (int)obs->err);
    break;
  }
}

static fr_err_t fuzz_eval_line(fr_runtime_t *runtime, const char *line,
                               char *out, uint16_t out_cap) {
  fr_err_t err = fr_repl_eval_line(runtime, line, out, out_cap);
  if (err != FR_OK) {
    out[0] = '\0';
  }
  return err;
}

static fr_err_t fuzz_apply_setup(fr_runtime_t *runtime,
                                 const fuzz_seed_case_t *seed) {
  char out[FR_REPL_OUTPUT_BYTES];

  for (uint8_t i = 0; i < seed->setup_count; i++) {
    FR_TRY(fuzz_eval_line(runtime, seed->setup[i], out, (uint16_t)sizeof(out)));
  }
  return FR_OK;
}

static fr_err_t fuzz_run_source_case(void *ctx, fuzz_observation_t *obs) {
  const fuzz_source_case_t *source_case = (const fuzz_source_case_t *)ctx;
  fr_runtime_t runtime;

  FR_TRY(fr_base_image_install(&runtime));
  memset(obs, 0, sizeof(*obs));
  obs->kind = FUZZ_OBS_VALUE;
  for (uint8_t i = 0; i < source_case->line_count; i++) {
    obs->err =
        fuzz_eval_line(&runtime, source_case->lines[i], obs->out,
                       (uint16_t)sizeof(obs->out));
    if (obs->err != FR_OK) {
      return FR_OK;
    }
  }
  return FR_OK;
}

static fr_err_t fuzz_run_expression_mutant(void *ctx, fuzz_observation_t *obs) {
  const fuzz_child_run_t *run = (const fuzz_child_run_t *)ctx;
  fr_runtime_t runtime = *run->base;
  fr_instruction_stream_t view = {
      .bytes = run->mutant->bytes,
      .length = run->mutant->length,
  };

  memset(obs, 0, sizeof(*obs));
  obs->kind = FUZZ_OBS_VALUE;
  FR_TRY(fuzz_apply_setup(&runtime, run->seed));
  obs->err = fr_vm_run_instruction_stream(&runtime, &view, &obs->tagged);
  return FR_OK;
}

static fr_err_t fuzz_run_overlay_mutant(void *ctx, fuzz_observation_t *obs) {
  const fuzz_child_run_t *run = (const fuzz_child_run_t *)ctx;
  fr_runtime_t runtime = *run->base;
  fr_compile_overlay_update_t update;

  memset(obs, 0, sizeof(*obs));
  obs->kind = FUZZ_OBS_VALUE;
  FR_TRY(fuzz_apply_setup(&runtime, run->seed));
  obs->err =
      fr_compile_overlay_update_for_runtime(&runtime, run->seed->source, &update);
  if (obs->err != FR_OK) {
    return FR_OK;
  }
  if (update.overlay_update.code_object_count == 0 ||
      update.overlay_update.code_objects == NULL) {
    obs->err = FR_ERR_INVALID;
    return FR_OK;
  }
  {
    fr_image_code_object_t *code_object =
        (fr_image_code_object_t *)update.overlay_update.code_objects;
    memcpy((uint8_t *)code_object[0].instructions.bytes, run->mutant->bytes,
           run->mutant->length);
    code_object[0].instructions.length = run->mutant->length;
  }
  obs->err = fr_overlay_apply(&runtime, &update.overlay_update);
  if (obs->err != FR_OK) {
    return FR_OK;
  }
  obs->err =
      fuzz_eval_line(&runtime, run->seed->run_line, obs->out,
                     (uint16_t)sizeof(obs->out));
  return FR_OK;
}

static fuzz_observation_t fuzz_run_child(fuzz_child_fn_t fn, void *ctx) {
  fuzz_observation_t obs = {
      .kind = FUZZ_OBS_IO,
      .err = FR_ERR_IO,
  };
  int pipefd[2];
  pid_t pid;

  if (pipe(pipefd) != 0) {
    return obs;
  }

  pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return obs;
  }

  if (pid == 0) {
    fuzz_observation_t child_obs;
    fr_err_t err;

    close(pipefd[0]);
    memset(&child_obs, 0, sizeof(child_obs));
    child_obs.kind = FUZZ_OBS_VALUE;
    err = fn(ctx, &child_obs);
    if (err != FR_OK) {
      child_obs.kind = FUZZ_OBS_VALUE;
      child_obs.err = err;
    }
    if (write(pipefd[1], &child_obs, sizeof(child_obs)) !=
        (ssize_t)sizeof(child_obs)) {
      _exit(3);
    }
    close(pipefd[1]);
    _exit(0);
  }

  close(pipefd[1]);
  for (uint16_t waited = 0; waited < FUZZ_CHILD_TIMEOUT_MS; waited++) {
    int status = 0;
    pid_t done = waitpid(pid, &status, WNOHANG);

    if (done == pid) {
      if (WIFSIGNALED(status)) {
        obs.kind = FUZZ_OBS_SIGNAL;
        obs.err = (fr_err_t)WTERMSIG(status);
      } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        ssize_t n = read(pipefd[0], &obs, sizeof(obs));
        if (n != (ssize_t)sizeof(obs)) {
          obs.kind = FUZZ_OBS_IO;
          obs.err = FR_ERR_IO;
        }
      } else if (WIFEXITED(status)) {
        obs.kind = FUZZ_OBS_EXIT;
        obs.err = (fr_err_t)WEXITSTATUS(status);
      }
      close(pipefd[0]);
      return obs;
    }
    if (done < 0 && errno != EINTR) {
      close(pipefd[0]);
      return obs;
    }
    usleep(1000);
  }

  kill(pid, SIGKILL);
  (void)waitpid(pid, NULL, 0);
  close(pipefd[0]);
  obs.kind = FUZZ_OBS_TIMEOUT;
  obs.err = FR_ERR_INTERRUPTED;
  return obs;
}

static void fuzz_make_source_case(uint32_t index, fuzz_rng_t *rng,
                                  fuzz_source_case_t *out) {
  char a[16];
  char b[16];
  char c[16];
  int32_t x = fuzz_int_value(rng);
  int32_t y = fuzz_int_value(rng);

  memset(out, 0, sizeof(*out));
  fuzz_name('f', index, a, (uint16_t)sizeof(a));
  fuzz_name('g', index, b, (uint16_t)sizeof(b));
  fuzz_name('c', index, c, (uint16_t)sizeof(c));

  switch (fuzz_rng_below(rng, 14)) {
  case 0:
    out->line_count = 1;
    (void)snprintf(out->lines[0], FUZZ_LINE_BYTES, "to %s with [ 1 ]", a);
    break;
  case 1:
    out->line_count = 1;
    (void)snprintf(out->lines[0], FUZZ_LINE_BYTES, "%s is fn [ 1 ;", a);
    break;
  case 2:
    out->line_count = 1;
    (void)snprintf(out->lines[0], FUZZ_LINE_BYTES, "%" PRId32 " + %" PRId32,
                   x, y);
    break;
  case 3:
    out->line_count = 2;
    (void)snprintf(out->lines[0], FUZZ_LINE_BYTES, "to %s with a, b [ a + b ]",
                   a);
    (void)snprintf(out->lines[1], FUZZ_LINE_BYTES, "%s: %" PRId32 ", %" PRId32,
                   a, (int32_t)(x % 100), (int32_t)(y % 100));
    break;
  case 4:
    out->line_count = 2;
    (void)snprintf(out->lines[0], FUZZ_LINE_BYTES,
                   "to %s with n [ if n < 1 [ 0 ] else [ %s: n - 1 ] ]", a,
                   a);
    (void)snprintf(out->lines[1], FUZZ_LINE_BYTES, "%s: %" PRIu32, a,
                   fuzz_rng_below(rng, 5));
    break;
  case 5:
    out->line_count = 2;
    (void)snprintf(out->lines[0], FUZZ_LINE_BYTES,
                   "to %s with n [ here x is n * 2 ; x + 1 ]", a);
    (void)snprintf(out->lines[1], FUZZ_LINE_BYTES, "%s: %" PRIu32, a,
                   fuzz_rng_below(rng, 20));
    break;
  case 6:
    out->line_count = 4;
    (void)snprintf(out->lines[0], FUZZ_LINE_BYTES, "%s is cells(4)", c);
    (void)snprintf(out->lines[1], FUZZ_LINE_BYTES, "set %s[0] to %" PRId32, c,
                   (int32_t)(x % 100));
    (void)snprintf(out->lines[2], FUZZ_LINE_BYTES,
                   "here i is 1 ; set %s[i] to %" PRId32, c,
                   (int32_t)(y % 100));
    (void)snprintf(out->lines[3], FUZZ_LINE_BYTES, "%s[0] + %s[1]", c, c);
    break;
  case 7:
    out->line_count = 2;
    (void)snprintf(out->lines[0], FUZZ_LINE_BYTES,
                   "to %s with n [ here x is 0 ; repeat n [ set x to x + 1 ]; x ]",
                   a);
    (void)snprintf(out->lines[1], FUZZ_LINE_BYTES, "%s: %" PRIu32, a,
                   fuzz_rng_below(rng, 6));
    break;
  case 8:
    out->line_count = 2;
    (void)snprintf(out->lines[0], FUZZ_LINE_BYTES,
                   "to %s with n [ here x is 0 ; while x < n [ set x to x + 1 ]; x ]",
                   a);
    (void)snprintf(out->lines[1], FUZZ_LINE_BYTES, "%s: %" PRIu32, a,
                   fuzz_rng_below(rng, 6));
    break;
  case 9:
    out->line_count = 1;
    (void)snprintf(out->lines[0], FUZZ_LINE_BYTES,
                   "if %" PRId32 " > %" PRId32 " [ 1 ] else [ 0 ]", x, y);
    break;
  case 10:
    out->line_count = 1;
    (void)snprintf(out->lines[0], FUZZ_LINE_BYTES, "abs: %" PRId32,
                   (int32_t)(x % 1000));
    break;
  case 11:
    out->line_count = 1;
    (void)snprintf(out->lines[0], FUZZ_LINE_BYTES, "min: %" PRId32 ", %" PRId32,
                   (int32_t)(x % 1000), (int32_t)(y % 1000));
    break;
  case 12:
    out->line_count = 2;
    (void)snprintf(out->lines[0], FUZZ_LINE_BYTES, "%s is abs: %" PRId32, b,
                   (int32_t)(x % 1000));
    (void)snprintf(out->lines[1], FUZZ_LINE_BYTES, "%s", b);
    break;
  default:
    out->line_count = 2;
    (void)snprintf(out->lines[0], FUZZ_LINE_BYTES,
                   "to %s with n [ if n = 0 [ true ] else [ false ] ]", a);
    (void)snprintf(out->lines[1], FUZZ_LINE_BYTES, "%s: %" PRIu32, a,
                   fuzz_rng_below(rng, 3));
    break;
  }
}

static void fuzz_make_seed_case(uint32_t index, fuzz_rng_t *rng,
                                fuzz_seed_case_t *out) {
  char f[16];
  char c[16];

  memset(out, 0, sizeof(*out));
  fuzz_name('f', index, f, (uint16_t)sizeof(f));
  fuzz_name('c', index, c, (uint16_t)sizeof(c));

  switch (fuzz_rng_below(rng, 10)) {
  case 0:
    out->kind = FUZZ_SEED_EXPRESSION;
    (void)snprintf(out->source, FUZZ_LINE_BYTES, "1 + 2 * 3");
    break;
  case 1:
    out->kind = FUZZ_SEED_EXPRESSION;
    (void)snprintf(out->source, FUZZ_LINE_BYTES, "here x is 4 ; x + 1");
    break;
  case 2:
    out->kind = FUZZ_SEED_EXPRESSION;
    (void)snprintf(out->source, FUZZ_LINE_BYTES, "abs: -5");
    break;
  case 3:
    out->kind = FUZZ_SEED_OVERLAY;
    (void)snprintf(out->source, FUZZ_LINE_BYTES, "to %s with a, b [ a + b ]",
                   f);
    (void)snprintf(out->run_line, FUZZ_LINE_BYTES, "%s: 3, 4", f);
    break;
  case 4:
    out->kind = FUZZ_SEED_OVERLAY;
    (void)snprintf(out->source, FUZZ_LINE_BYTES,
                   "to %s with n [ if n < 1 [ 0 ] else [ %s: n - 1 ] ]", f,
                   f);
    (void)snprintf(out->run_line, FUZZ_LINE_BYTES, "%s: 3", f);
    break;
  case 5:
    out->kind = FUZZ_SEED_OVERLAY;
    out->setup_count = 1;
    (void)snprintf(out->setup[0], FUZZ_LINE_BYTES, "%s is cells(4)", c);
    (void)snprintf(out->source, FUZZ_LINE_BYTES,
                   "to %s with i [ set %s[i] to i + 1 ; %s[i] ]", f, c, c);
    (void)snprintf(out->run_line, FUZZ_LINE_BYTES, "%s: 2", f);
    break;
  case 6:
    out->kind = FUZZ_SEED_OVERLAY;
    out->setup_count = 1;
    (void)snprintf(out->setup[0], FUZZ_LINE_BYTES, "%s is cells(4)", c);
    (void)snprintf(out->source, FUZZ_LINE_BYTES,
                   "%s is fn [ set %s[1] to 9 ; %s[1] ]", f, c, c);
    (void)snprintf(out->run_line, FUZZ_LINE_BYTES, "%s:", f);
    break;
  case 7:
    out->kind = FUZZ_SEED_OVERLAY;
    (void)snprintf(out->source, FUZZ_LINE_BYTES,
                   "to %s with n [ here x is 0 ; repeat n [ set x to x + 1 ]; x ]",
                   f);
    (void)snprintf(out->run_line, FUZZ_LINE_BYTES, "%s: 3", f);
    break;
  case 8:
    out->kind = FUZZ_SEED_OVERLAY;
    (void)snprintf(out->source, FUZZ_LINE_BYTES,
                   "to %s with n [ here x is 0 ; while x < n [ set x to x + 1 ]; x ]",
                   f);
    (void)snprintf(out->run_line, FUZZ_LINE_BYTES, "%s: 3", f);
    break;
  default:
    out->kind = FUZZ_SEED_OVERLAY;
    (void)snprintf(out->source, FUZZ_LINE_BYTES,
                   "%s is fn [ if 3 > 2 [ 1 ] else [ 0 ] ]", f);
    (void)snprintf(out->run_line, FUZZ_LINE_BYTES, "%s:", f);
    break;
  }
}

static uint16_t fuzz_instruction_next(const uint8_t *bytes, uint16_t length,
                                      uint16_t ip) {
  fr_opcode_t op;

  if (ip >= length) {
    return length;
  }
  op = (fr_opcode_t)bytes[ip];
  switch (op) {
  case FR_OP_RETURN:
  case FR_OP_ADD_INT:
  case FR_OP_SUB_INT:
  case FR_OP_MUL_INT:
  case FR_OP_DIV_INT:
  case FR_OP_LT_INT:
  case FR_OP_GT_INT:
  case FR_OP_LE_INT:
  case FR_OP_GE_INT:
  case FR_OP_EQ_INT:
  case FR_OP_NE_INT:
  case FR_OP_DROP:
  case FR_OP_PUSH_NIL:
  case FR_OP_PUSH_FALSE:
  case FR_OP_PUSH_TRUE:
  case FR_OP_BYTES_RESET:
    return (uint16_t)(ip + 1u);
  case FR_OP_LOAD_ARG:
  case FR_OP_LOAD_LOCAL:
  case FR_OP_STORE_LOCAL:
    return (uint16_t)(ip + 2u);
  case FR_OP_LOAD_SLOT:
  case FR_OP_STORE_SLOT:
  case FR_OP_CALL_SLOT:
  case FR_OP_CALL_NATIVE_SLOT:
  case FR_OP_JUMP:
  case FR_OP_JUMP_IF_FALSY:
  case FR_OP_REPEAT_BEGIN:
  case FR_OP_REPEAT_NEXT:
  case FR_OP_LOAD_CELL_DYNAMIC:
  case FR_OP_STORE_CELL_DYNAMIC:
    return (uint16_t)(ip + 3u);
  case FR_OP_CALL_SLOT_ARG:
    return (uint16_t)(ip + 4u);
  case FR_OP_PUSH_INT:
    return (uint16_t)(ip + FR_INSTRUCTION_PUSH_INT_SIZE);
  case FR_OP_PUSH_OBJECT_ID:
    return (uint16_t)(ip + FR_INSTRUCTION_PUSH_OBJECT_ID_SIZE);
  case FR_OP_PUSH_CODE_ID:
    return (uint16_t)(ip + FR_INSTRUCTION_PUSH_CODE_ID_SIZE);
  case FR_OP_LOAD_CELL:
  case FR_OP_STORE_CELL:
    return (uint16_t)(ip + 5u);
  case FR_OP_LOAD_FIELD:
  case FR_OP_STORE_FIELD:
    if ((uint16_t)(ip + 1u) >= length) {
      return length;
    }
    return (uint16_t)(ip + 2u + bytes[ip + 1]);
  default:
    return length;
  }
}

static uint16_t fuzz_collect_instructions(const uint8_t *bytes, uint16_t length,
                                          uint16_t starts[], uint16_t cap) {
  uint16_t count = 0;
  uint16_t ip;

  if (length < FR_INSTRUCTION_MIN_HEADER_SIZE) {
    return 0;
  }
  ip = bytes[1];
  while (ip < length && count < cap) {
    uint16_t next = fuzz_instruction_next(bytes, length, ip);
    starts[count++] = ip;
    if (next <= ip || next > length) {
      break;
    }
    ip = next;
  }
  return count;
}

static bool fuzz_pick_operand_instruction(const uint8_t *bytes, uint16_t length,
                                          const uint16_t starts[],
                                          uint16_t start_count,
                                          fuzz_rng_t *rng, uint16_t *out_ip) {
  uint16_t candidates[128];
  uint16_t candidate_count = 0;

  for (uint16_t i = 0; i < start_count && candidate_count < 128; i++) {
    uint16_t ip = starts[i];
    fr_opcode_t op = (fr_opcode_t)bytes[ip];

    switch (op) {
    case FR_OP_LOAD_SLOT:
    case FR_OP_STORE_SLOT:
    case FR_OP_CALL_SLOT:
    case FR_OP_CALL_NATIVE_SLOT:
    case FR_OP_LOAD_CELL_DYNAMIC:
    case FR_OP_STORE_CELL_DYNAMIC:
    case FR_OP_PUSH_INT:
    case FR_OP_PUSH_OBJECT_ID:
    case FR_OP_PUSH_CODE_ID:
    case FR_OP_LOAD_ARG:
    case FR_OP_LOAD_LOCAL:
    case FR_OP_STORE_LOCAL:
    case FR_OP_CALL_SLOT_ARG:
    case FR_OP_LOAD_CELL:
    case FR_OP_STORE_CELL:
      if (fuzz_instruction_next(bytes, length, ip) <= length) {
        candidates[candidate_count++] = ip;
      }
      break;
    default:
      break;
    }
  }
  if (candidate_count == 0) {
    return false;
  }
  *out_ip = candidates[fuzz_rng_below(rng, candidate_count)];
  return true;
}

static bool fuzz_pick_jump_instruction(const uint8_t *bytes,
                                       const uint16_t starts[],
                                       uint16_t start_count, fuzz_rng_t *rng,
                                       uint16_t *out_ip) {
  uint16_t candidates[64];
  uint16_t candidate_count = 0;

  for (uint16_t i = 0; i < start_count && candidate_count < 64; i++) {
    fr_opcode_t op = (fr_opcode_t)bytes[starts[i]];
    if (op == FR_OP_JUMP || op == FR_OP_JUMP_IF_FALSY ||
        op == FR_OP_REPEAT_BEGIN || op == FR_OP_REPEAT_NEXT) {
      candidates[candidate_count++] = starts[i];
    }
  }
  if (candidate_count == 0) {
    return false;
  }
  *out_ip = candidates[fuzz_rng_below(rng, candidate_count)];
  return true;
}

static uint16_t fuzz_mid_instruction_target(const uint8_t *bytes,
                                            uint16_t length,
                                            const uint16_t starts[],
                                            uint16_t start_count) {
  for (uint16_t i = 0; i < start_count; i++) {
    uint16_t ip = starts[i];
    uint16_t next = fuzz_instruction_next(bytes, length, ip);
    if (next > (uint16_t)(ip + 1u) && (uint16_t)(ip + 1u) < length) {
      return (uint16_t)(ip + 1u);
    }
  }
  return length;
}

static void fuzz_mutate_stream(const uint8_t *seed_bytes, uint16_t seed_length,
                               fuzz_rng_t *rng, fuzz_mutant_t *out) {
  uint16_t starts[128];
  uint16_t start_count;
  uint16_t ip = 0;
  uint32_t choice;

  memset(out, 0, sizeof(*out));
  memcpy(out->bytes, seed_bytes, seed_length);
  out->length = seed_length;
  starts[0] = 0;
  start_count = fuzz_collect_instructions(out->bytes, out->length, starts, 128);
  choice = fuzz_rng_below(rng, 10);

  if (choice == 0 && out->length > 0) {
    out->length = (uint16_t)fuzz_rng_below(rng, out->length);
    return;
  }

  if (choice == 1 && out->length < FUZZ_MAX_BYTES) {
    out->bytes[out->length] = 0xffu;
    out->length = (uint16_t)(out->length + 1u);
    return;
  }

  if (choice == 2 && start_count > 0) {
    ip = starts[fuzz_rng_below(rng, start_count)];
    out->bytes[ip] = (uint8_t)(FR_OP_STORE_CELL_DYNAMIC + 1u);
    return;
  }

  if (choice == 3 && fuzz_pick_operand_instruction(out->bytes, out->length,
                                                   starts, start_count, rng,
                                                   &ip)) {
    fr_opcode_t op = (fr_opcode_t)out->bytes[ip];
    uint16_t boundary = 0;

    switch (op) {
    case FR_OP_LOAD_SLOT:
    case FR_OP_STORE_SLOT:
    case FR_OP_CALL_SLOT:
    case FR_OP_CALL_NATIVE_SLOT:
    case FR_OP_LOAD_CELL_DYNAMIC:
    case FR_OP_STORE_CELL_DYNAMIC:
      boundary = fuzz_rng_below(rng, 2) == 0 ? FR_PROFILE_MAX_SLOTS
                                             : (uint16_t)0xffffu;
      fuzz_write_u16(&out->bytes[ip + 1], boundary);
      return;
    case FR_OP_PUSH_INT:
      fuzz_write_i32(&out->bytes[ip + 1],
                     fuzz_rng_below(rng, 2) == 0 ? INT32_MAX : INT32_MIN);
      return;
    case FR_OP_PUSH_OBJECT_ID:
      boundary = fuzz_rng_below(rng, 2) == 0 ? FR_PROFILE_OBJECT_TABLE_SIZE
                                             : (uint16_t)0xffffu;
      fuzz_write_u16(&out->bytes[ip + 1], boundary);
      return;
    case FR_OP_PUSH_CODE_ID:
      boundary = fuzz_rng_below(rng, 2) == 0 ? FR_PROFILE_CODE_OBJECT_TABLE_SIZE
                                             : (uint16_t)0xffffu;
      fuzz_write_u16(&out->bytes[ip + 1], boundary);
      return;
    case FR_OP_LOAD_ARG:
      out->bytes[ip + 1] = (uint8_t)(FR_PROFILE_MAX_STACK_DEPTH + 1u);
      return;
    case FR_OP_LOAD_LOCAL:
    case FR_OP_STORE_LOCAL:
      out->bytes[ip + 1] = (uint8_t)(FR_PROFILE_MAX_STACK_DEPTH + 1u);
      return;
    case FR_OP_CALL_SLOT_ARG:
      fuzz_write_u16(&out->bytes[ip + 1], FR_PROFILE_MAX_SLOTS);
      out->bytes[ip + 3] = (uint8_t)(FR_PROFILE_MAX_STACK_DEPTH + 1u);
      return;
    case FR_OP_LOAD_CELL:
    case FR_OP_STORE_CELL:
      fuzz_write_u16(&out->bytes[ip + 1], FR_PROFILE_MAX_SLOTS);
      fuzz_write_u16(&out->bytes[ip + 3], (uint16_t)0xffffu);
      return;
    default:
      break;
    }
  }

  if (choice == 4 && fuzz_pick_jump_instruction(out->bytes, starts, start_count,
                                                rng, &ip)) {
    uint16_t targets[] = {
        out->bytes[1],
        (uint16_t)(out->length > 0 ? out->length - 1u : 0u),
        out->length,
        fuzz_mid_instruction_target(out->bytes, out->length, starts,
                                    start_count),
        (uint16_t)0xffffu,
    };
    fuzz_write_u16(&out->bytes[ip + 1],
                   targets[fuzz_rng_below(rng, (uint32_t)(sizeof(targets) /
                                                          sizeof(targets[0])))]);
    return;
  }

  if (choice == 5 && out->length >= FR_INSTRUCTION_MIN_HEADER_SIZE) {
    out->bytes[0] = fuzz_rng_below(rng, 2) == 0
                        ? (uint8_t)(FR_INSTRUCTION_FORMAT_VERSION + 1u)
                        : out->bytes[0];
    out->bytes[1] = fuzz_rng_below(rng, 2) == 0 ? 0xffu : 1u;
    return;
  }

  if (choice == 6 && out->length > FR_INSTRUCTION_MIN_HEADER_SIZE) {
    uint16_t pos = (uint16_t)fuzz_rng_below(rng, out->length);
    if (pos < out->bytes[1]) {
      pos = out->bytes[1];
    }
    out->bytes[pos] ^= (uint8_t)(1u << fuzz_rng_below(rng, 8));
    return;
  }

  if (choice == 7 && out->length > FR_INSTRUCTION_MIN_HEADER_SIZE) {
    uint16_t pos = (uint16_t)fuzz_rng_below(rng, out->length);
    if (pos < out->bytes[1]) {
      pos = out->bytes[1];
    }
    out->bytes[pos] = (uint8_t)fuzz_rng_below(rng, 256);
    return;
  }

  if (out->length > FR_INSTRUCTION_MIN_HEADER_SIZE) {
    uint16_t pos = (uint16_t)(FR_INSTRUCTION_MIN_HEADER_SIZE +
                              fuzz_rng_below(rng, out->length -
                                                      FR_INSTRUCTION_MIN_HEADER_SIZE));
    out->bytes[pos] ^= 0xffu;
  }
}

static fr_err_t fuzz_build_seed(const fr_runtime_t *base,
                                const fuzz_seed_case_t *seed,
                                uint8_t *out_bytes, uint16_t *out_length) {
  fr_runtime_t runtime = *base;

  FR_TRY(fuzz_apply_setup(&runtime, seed));
  if (seed->kind == FUZZ_SEED_EXPRESSION) {
    fr_compile_expression_t expression;

    FR_TRY(fr_compile_expression_for_runtime(&runtime, seed->source,
                                             &expression));
    memcpy(out_bytes, expression.instructions.bytes, expression.instructions.length);
    *out_length = expression.instructions.length;
    return FR_OK;
  } else {
    fr_compile_overlay_update_t update;

    FR_TRY(fr_compile_overlay_update_for_runtime(&runtime, seed->source,
                                                 &update));
    if (update.overlay_update.code_object_count == 0 ||
        update.overlay_update.code_objects == NULL) {
      return FR_ERR_INVALID;
    }
    memcpy(out_bytes, update.overlay_update.code_objects[0].instructions.bytes,
           update.overlay_update.code_objects[0].instructions.length);
    *out_length = update.overlay_update.code_objects[0].instructions.length;
    return FR_OK;
  }
}

static void fuzz_print_source_dump(const fuzz_source_case_t *source_case) {
  fprintf(stderr, "source-lines=%u\n", source_case->line_count);
  for (uint8_t i = 0; i < source_case->line_count; i++) {
    fprintf(stderr, "source[%u]=", i);
    fuzz_hex_print_bytes((const uint8_t *)source_case->lines[i],
                         (uint16_t)strlen(source_case->lines[i]));
    fprintf(stderr, "\n");
  }
}

static void fuzz_print_seed_dump(const fuzz_seed_case_t *seed,
                                 const uint8_t *seed_bytes,
                                 uint16_t seed_length,
                                 const fuzz_mutant_t *mutant) {
  fprintf(stderr, "seed-kind=%s\n",
          seed->kind == FUZZ_SEED_EXPRESSION ? "expression" : "overlay");
  for (uint8_t i = 0; i < seed->setup_count; i++) {
    fprintf(stderr, "setup[%u]=", i);
    fuzz_hex_print_bytes((const uint8_t *)seed->setup[i],
                         (uint16_t)strlen(seed->setup[i]));
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "source=");
  fuzz_hex_print_bytes((const uint8_t *)seed->source,
                       (uint16_t)strlen(seed->source));
  fprintf(stderr, "\nrun-line=");
  fuzz_hex_print_bytes((const uint8_t *)seed->run_line,
                       (uint16_t)strlen(seed->run_line));
  fprintf(stderr, "\nseed-bytes=");
  fuzz_hex_print_bytes(seed_bytes, seed_length);
  fprintf(stderr, "\nmutant-bytes=");
  fuzz_hex_print_bytes(mutant->bytes, mutant->length);
  fprintf(stderr, "\n");
}

static int fuzz_run_source(uint32_t seed, uint32_t count, int32_t dump_case) {
  fuzz_rng_t rng = {.state = seed == 0 ? 1u : seed};

  for (uint32_t i = 0; i < count; i++) {
    fuzz_source_case_t source_case;
    fuzz_observation_t obs;

    fuzz_make_source_case(i, &rng, &source_case);
    obs = fuzz_run_child(fuzz_run_source_case, &source_case);
    if (dump_case == (int32_t)i) {
      fuzz_print_source_dump(&source_case);
    }
    printf("%" PRIu32 " source ", i);
    fuzz_observation_print(&obs);
    printf("\n");
  }
  return 0;
}

static int fuzz_run_corruption(uint32_t seed, uint32_t count,
                               int32_t dump_case) {
  fuzz_rng_t rng = {.state = seed == 0 ? 1u : seed};
  fr_runtime_t base;
  uint32_t rejected = 0;
  uint32_t accepted = 0;
  uint32_t build_errors = 0;

  if (fr_base_image_install(&base) != FR_OK) {
    fprintf(stderr, "failed to install base image\n");
    return 2;
  }

  for (uint32_t i = 0; i < count; i++) {
    fuzz_seed_case_t seed_case;
    uint8_t seed_bytes[FUZZ_MAX_BYTES];
    uint16_t seed_length = 0;
    fuzz_mutant_t mutant;
    fr_instruction_stream_t view;
    fr_err_t err;

    fuzz_make_seed_case(i, &rng, &seed_case);
    err = fuzz_build_seed(&base, &seed_case, seed_bytes, &seed_length);
    if (err != FR_OK) {
      build_errors++;
      printf("%" PRIu32 " corruption seed-error err=%d\n", i, (int)err);
      continue;
    }
    fuzz_mutate_stream(seed_bytes, seed_length, &rng, &mutant);
    view = (fr_instruction_stream_t){.bytes = mutant.bytes,
                                     .length = mutant.length};
    err = fr_verify_code_object(&view);
    if (dump_case == (int32_t)i) {
      fuzz_print_seed_dump(&seed_case, seed_bytes, seed_length, &mutant);
    }
    if (err != FR_OK) {
      rejected++;
      printf("%" PRIu32 " corruption rejected err=%d\n", i, (int)err);
      continue;
    }

    {
      fuzz_child_run_t run = {
          .base = &base,
          .seed = &seed_case,
          .mutant = &mutant,
      };
      fuzz_observation_t obs =
          seed_case.kind == FUZZ_SEED_EXPRESSION
              ? fuzz_run_child(fuzz_run_expression_mutant, &run)
              : fuzz_run_child(fuzz_run_overlay_mutant, &run);

      accepted++;
      printf("%" PRIu32 " corruption accepted ", i);
      fuzz_observation_print(&obs);
      printf("\n");
    }
  }

  fprintf(stderr,
          "corruption-summary seed=%" PRIu32 " count=%" PRIu32
          " rejected=%" PRIu32 " accepted=%" PRIu32
          " seed_errors=%" PRIu32 " reject_rate_x100=%" PRIu32 "\n",
          seed, count, rejected, accepted, build_errors,
          count == 0 ? 0u : (rejected * 10000u) / count);

  if (count >= 100 &&
      rejected * 100u < count * FUZZ_CORRUPTION_MIN_REJECT_PERCENT) {
    fprintf(stderr, "corruption reject rate below healthy threshold\n");
    return 3;
  }
  return 0;
}

static bool fuzz_parse_u32(const char *text, uint32_t *out) {
  char *end = NULL;
  unsigned long value = strtoul(text, &end, 0);

  if (text == NULL || *text == '\0' || end == NULL || *end != '\0' ||
      value > UINT32_MAX) {
    return false;
  }
  *out = (uint32_t)value;
  return true;
}

static void fuzz_usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s --seed N --count M --mode source|corruption [--dump-case I]\n",
          argv0);
}

int main(int argc, char **argv) {
  uint32_t seed = 1;
  uint32_t count = 0;
  fuzz_mode_t mode = FUZZ_MODE_SOURCE;
  bool have_mode = false;
  int32_t dump_case = -1;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      if (!fuzz_parse_u32(argv[++i], &seed)) {
        fuzz_usage(argv[0]);
        return 2;
      }
    } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
      if (!fuzz_parse_u32(argv[++i], &count)) {
        fuzz_usage(argv[0]);
        return 2;
      }
    } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      i++;
      if (strcmp(argv[i], "source") == 0) {
        mode = FUZZ_MODE_SOURCE;
      } else if (strcmp(argv[i], "corruption") == 0) {
        mode = FUZZ_MODE_CORRUPTION;
      } else {
        fuzz_usage(argv[0]);
        return 2;
      }
      have_mode = true;
    } else if (strcmp(argv[i], "--dump-case") == 0 && i + 1 < argc) {
      uint32_t value = 0;
      if (!fuzz_parse_u32(argv[++i], &value) || value > INT32_MAX) {
        fuzz_usage(argv[0]);
        return 2;
      }
      dump_case = (int32_t)value;
    } else {
      fuzz_usage(argv[0]);
      return 2;
    }
  }

  if (!have_mode) {
    fuzz_usage(argv[0]);
    return 2;
  }

  if (mode == FUZZ_MODE_SOURCE) {
    return fuzz_run_source(seed, count, dump_case);
  }
  return fuzz_run_corruption(seed, count, dump_case);
}
