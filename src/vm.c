/*
 * Frothy is not stack-based Froth. Stable runtime state starts with slots.
 * The VM's tagged-word stack is private execution machinery owned by a run
 * context.
 */

#include "vm.h"

#include "base_image.h"
#include "code.h"
#include "event.h"
#include "native.h"
#include "object.h"
#include "platform.h"
#include "slot.h"
#include "tagged.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef FR_VM_YIELD_SAFE_POINTS
#define FR_VM_YIELD_SAFE_POINTS 4096u
#endif

#if FR_VM_YIELD_SAFE_POINTS == 0 || FR_VM_YIELD_SAFE_POINTS > UINT16_MAX
#error "FR_VM_YIELD_SAFE_POINTS must be 1..UINT16_MAX"
#endif

/* Poll the interrupt line (Ctrl-C, boot button) every Nth safe point rather
   than every one: the poll's UART-readiness + GPIO reads cost ~382 cyc, and a
   tight loop hits a safe point every iteration. N safe points of Ctrl-C latency
   is sub-millisecond for a fast loop. The interrupted flag itself is still
   checked every safe point (cheap bool), so an async SIGINT on host and a flag
   set by the last poll both bail immediately. */
#ifndef FR_VM_POLL_SAFE_POINTS
#define FR_VM_POLL_SAFE_POINTS 64u
#endif

#if FR_VM_POLL_SAFE_POINTS == 0 || FR_VM_POLL_SAFE_POINTS > UINT16_MAX
#error "FR_VM_POLL_SAFE_POINTS must be 1..UINT16_MAX"
#endif

typedef struct fr_vm_state_t {
  fr_code_offset_t ip;
  fr_tagged_t stack[FR_PROFILE_MAX_STACK_DEPTH];
  /* Args at frame[0..arity-1], locals at frame[arity..arity+local_count-1].
   * Locals start as nil; STORE_LOCAL writes them, LOAD_LOCAL reads them. */
  fr_tagged_t frame[FR_PROFILE_MAX_STACK_DEPTH];
  uint16_t depth;
  uint16_t call_depth;
  uint8_t arg_count;
  uint8_t local_count;
  bool returned;
} fr_vm_state_t;

static fr_err_t fr_vm_run_instruction_stream_depth(
    fr_runtime_t *runtime, const fr_instruction_stream_t *view,
    const fr_tagged_t *args, uint8_t arg_count, fr_tagged_t *out_tagged,
    uint16_t call_depth);
static fr_err_t fr_vm_run_code_object_depth(fr_runtime_t *runtime,
                                            fr_code_object_id_t code_object_id,
                                            const fr_tagged_t *args,
                                            uint8_t arg_count,
                                            fr_tagged_t *out_tagged,
                                            uint16_t call_depth);
static fr_err_t fr_vm_run_slot_depth(fr_runtime_t *runtime,
                                     fr_slot_id_t slot_id,
                                     const fr_tagged_t *args,
                                     uint8_t arg_count,
                                     fr_tagged_t *out_tagged,
                                     uint16_t call_depth);

static bool fr_vm_diag_empty(const fr_runtime_t *runtime) {
  return runtime != NULL && runtime->diag != NULL &&
         runtime->diag->kind == FR_DIAG_NONE;
}

static void fr_vm_note_message(fr_runtime_t *runtime, fr_diag_kind_t kind,
                               uint16_t message_id, fr_int_t expected,
                               fr_int_t got) {
  if (!fr_vm_diag_empty(runtime)) {
    return;
  }

  runtime->diag->kind = kind;
  runtime->diag->message_id = message_id;
  runtime->diag->expected = expected;
  runtime->diag->got = got;
}

static void fr_vm_note_type(fr_runtime_t *runtime,
                            fr_diag_value_kind_t expected,
                            fr_tagged_t got) {
  if (!fr_vm_diag_empty(runtime)) {
    return;
  }

  runtime->diag->kind = FR_DIAG_TYPE;
  runtime->diag->expected = expected;
  runtime->diag->got = fr_tagged_diag_value_kind(got);
}

static void fr_vm_note_too_few_args(fr_runtime_t *runtime, uint8_t expected,
                                    uint16_t got) {
  fr_vm_note_message(runtime, FR_DIAG_ARITY,
                     FR_DIAG_MSG_RUNTIME_TOO_FEW_ARGS, expected, got);
}

static void fr_vm_note_call_depth(fr_runtime_t *runtime) {
  fr_vm_note_message(runtime, FR_DIAG_LIMIT, FR_DIAG_MSG_RUNTIME_CALL_DEPTH,
                     FR_PROFILE_MAX_CALL_DEPTH, 0);
}

static void fr_vm_note_stack_overflow(fr_runtime_t *runtime) {
  fr_vm_note_message(runtime, FR_DIAG_LIMIT,
                     FR_DIAG_MSG_RUNTIME_STACK_OVERFLOW, 0, 0);
}

static void fr_vm_note_stack_underflow(fr_runtime_t *runtime) {
  fr_vm_note_message(runtime, FR_DIAG_LIMIT,
                     FR_DIAG_MSG_RUNTIME_STACK_UNDERFLOW, 0, 0);
}

static void fr_vm_note_integer_overflow(fr_runtime_t *runtime) {
  fr_vm_note_message(runtime, FR_DIAG_LIMIT,
                     FR_DIAG_MSG_RUNTIME_INTEGER_OVERFLOW, 0, 0);
}

static fr_err_t fr_vm_decode_int(fr_runtime_t *runtime, fr_tagged_t tagged,
                                 fr_int_t *out_int) {
  if (out_int == NULL) {
    return FR_ERR_INVALID;
  }
  if (tagged > FR_TAGGED_INT_END) {
    fr_vm_note_type(runtime, FR_DIAG_VALUE_INT, tagged);
    return FR_ERR_TYPE;
  }
  *out_int = (fr_int_t)((int32_t)tagged - FR_TAGGED_INT_BIAS);
  return FR_OK;
}

#if FR_FEATURE_CELLS
static void fr_vm_note_cell_index(fr_runtime_t *runtime, fr_int_t index,
                                  uint16_t length) {
  fr_vm_note_message(runtime, FR_DIAG_LIMIT,
                     FR_DIAG_MSG_RUNTIME_CELL_INDEX_OOB, length, index);
}
#endif

static fr_err_t fr_vm_push(fr_runtime_t *runtime, fr_vm_state_t *state,
                           fr_tagged_t tagged) {
  if (state->depth >= FR_PROFILE_MAX_STACK_DEPTH) {
    fr_vm_note_stack_overflow(runtime);
    return FR_ERR_OVERFLOW;
  }
  state->stack[state->depth] = tagged;
  state->depth += 1;
  return FR_OK;
}

static fr_err_t fr_vm_pop(fr_runtime_t *runtime, fr_vm_state_t *state,
                          fr_tagged_t *out_tagged) {
  if (state->depth == 0) {
    fr_vm_note_stack_underflow(runtime);
    return FR_ERR_UNDERFLOW;
  }
  state->depth -= 1;
  if (out_tagged != NULL) {
    *out_tagged = state->stack[state->depth];
  }
  return FR_OK;
}

static fr_err_t fr_vm_return(fr_vm_state_t *state) {
  state->ip += 1;
  state->returned = true;
  return FR_OK;
}

static fr_err_t fr_vm_drop(fr_runtime_t *runtime, fr_vm_state_t *state) {
  fr_tagged_t ignored = 0;
  state->ip += 1;
  return fr_vm_pop(runtime, state, &ignored);
}

static fr_err_t fr_vm_load_slot(fr_runtime_t *runtime,
                                const fr_instruction_stream_t *view,
                                fr_vm_state_t *state) {
  fr_slot_id_t slot_id = 0;
  fr_tagged_t tagged = 0;

  FR_TRY(fr_instruction_read_slot_operand(view, state->ip, &slot_id));
  FR_TRY(fr_slot_read(runtime, slot_id, &tagged));

  state->ip += 3;
  return fr_vm_push(runtime, state, tagged);
}

static fr_err_t fr_vm_store_slot(fr_runtime_t *runtime,
                                 const fr_instruction_stream_t *view,
                                 fr_vm_state_t *state) {
  fr_slot_id_t slot_id = 0;
  fr_tagged_t tagged = 0;

  FR_TRY(fr_instruction_read_slot_operand(view, state->ip, &slot_id));
  FR_TRY(fr_vm_pop(runtime, state, &tagged));
  FR_TRY(fr_slot_write(runtime, slot_id, tagged));

  state->ip += 3;
  return fr_vm_push(runtime, state, fr_tagged_nil());
}

static fr_err_t fr_vm_push_int(fr_runtime_t *runtime,
                               const fr_instruction_stream_t *view,
                               fr_vm_state_t *state) {
  fr_int_t int_operand = 0;
  fr_tagged_t tagged = 0;

  FR_TRY(fr_instruction_read_int_operand(view, state->ip, &int_operand));
  FR_TRY(fr_tagged_encode_int(int_operand, &tagged));

  state->ip += FR_INSTRUCTION_PUSH_INT_SIZE;
  return fr_vm_push(runtime, state, tagged);
}

static fr_err_t fr_vm_push_object_id(fr_runtime_t *runtime,
                                     const fr_instruction_stream_t *view,
                                     fr_vm_state_t *state) {
  fr_object_id_t object_id = 0;
  fr_tagged_t tagged = 0;

  FR_TRY(fr_instruction_read_object_id_operand(view, state->ip, &object_id));
  FR_TRY(fr_tagged_encode_object_id(object_id, &tagged));

  state->ip += FR_INSTRUCTION_PUSH_OBJECT_ID_SIZE;
  return fr_vm_push(runtime, state, tagged);
}

/* The body code object id reaches the event-register native as a plain int.
 * The operand is patched from a local code index to the runtime id at install
 * time, the same path the text patcher uses for object refs. */
static fr_err_t fr_vm_push_code_id(fr_runtime_t *runtime,
                                   const fr_instruction_stream_t *view,
                                   fr_vm_state_t *state) {
  fr_code_object_id_t code_id = 0;
  fr_tagged_t tagged = 0;

  FR_TRY(fr_instruction_read_code_id_operand(view, state->ip, &code_id));
  FR_TRY(fr_tagged_encode_int((fr_int_t)code_id, &tagged));

  state->ip += FR_INSTRUCTION_PUSH_CODE_ID_SIZE;
  return fr_vm_push(runtime, state, tagged);
}

static fr_err_t fr_vm_push_nil(fr_runtime_t *runtime, fr_vm_state_t *state) {
  state->ip += 1;
  return fr_vm_push(runtime, state, fr_tagged_nil());
}

static fr_err_t fr_vm_push_bool(fr_runtime_t *runtime, fr_vm_state_t *state,
                                bool value) {
  fr_tagged_t tagged = 0;

  FR_TRY(fr_tagged_encode_bool(value, &tagged));
  state->ip += 1;
  return fr_vm_push(runtime, state, tagged);
}

static fr_err_t fr_vm_load_arg(fr_runtime_t *runtime,
                               const fr_instruction_stream_t *view,
                               fr_vm_state_t *state) {
  uint8_t arg_index = 0;

  FR_TRY(fr_instruction_read_arg_operand(view, state->ip, &arg_index));
  if (arg_index >= state->arg_count) {
    return FR_ERR_INVALID;
  }

  state->ip += 2;
  return fr_vm_push(runtime, state, state->frame[arg_index]);
}

static fr_err_t fr_vm_load_local(fr_runtime_t *runtime,
                                 const fr_instruction_stream_t *view,
                                 fr_vm_state_t *state) {
  uint8_t local_index = 0;

  FR_TRY(fr_instruction_read_local_operand(view, state->ip, &local_index));
  if (local_index >= state->local_count) {
    return FR_ERR_INVALID;
  }

  state->ip += 2;
  return fr_vm_push(runtime, state,
                    state->frame[state->arg_count + local_index]);
}

static fr_err_t fr_vm_store_local(fr_runtime_t *runtime,
                                  const fr_instruction_stream_t *view,
                                  fr_vm_state_t *state) {
  uint8_t local_index = 0;
  fr_tagged_t value = 0;

  FR_TRY(fr_instruction_read_local_operand(view, state->ip, &local_index));
  if (local_index >= state->local_count) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_vm_pop(runtime, state, &value));

  state->frame[state->arg_count + local_index] = value;
  state->ip += 2;
  return fr_vm_push(runtime, state, fr_tagged_nil());
}

#if FR_FEATURE_CELLS
static fr_err_t fr_vm_cell_object_for_slot(fr_runtime_t *runtime,
                                           fr_slot_id_t slot_id,
                                           fr_object_id_t *out_object_id) {
  fr_tagged_t tagged = 0;

  if (out_object_id == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_slot_read(runtime, slot_id, &tagged));
  return fr_tagged_decode_object_id(tagged, out_object_id);
}

static fr_err_t fr_vm_cell_index_in_bounds(fr_runtime_t *runtime,
                                           fr_object_id_t object_id,
                                           fr_int_t raw_index,
                                           uint16_t *out_index) {
  uint16_t length = 0;

  if (out_index == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_cells_length(runtime, object_id, &length));
  if (raw_index < 0 || (uint32_t)raw_index >= length) {
    fr_vm_note_cell_index(runtime, raw_index, length);
    return FR_ERR_RANGE;
  }
  *out_index = (uint16_t)raw_index;
  return FR_OK;
}

static fr_err_t fr_vm_pop_cell_index(fr_runtime_t *runtime,
                                     fr_vm_state_t *state,
                                     fr_object_id_t object_id,
                                     uint16_t *out_index) {
  fr_tagged_t tagged = 0;
  fr_int_t raw_index = 0;

  FR_TRY(fr_vm_pop(runtime, state, &tagged));
  FR_TRY(fr_vm_decode_int(runtime, tagged, &raw_index));
  return fr_vm_cell_index_in_bounds(runtime, object_id, raw_index, out_index);
}

static fr_err_t fr_vm_load_cell(fr_runtime_t *runtime,
                                const fr_instruction_stream_t *view,
                                fr_vm_state_t *state) {
  fr_slot_id_t slot_id = 0;
  uint16_t index = 0;
  fr_tagged_t tagged = 0;
  fr_object_id_t object_id = 0;

  FR_TRY(fr_instruction_read_cell_operands(view, state->ip, &slot_id, &index));
  FR_TRY(fr_vm_cell_object_for_slot(runtime, slot_id, &object_id));
  FR_TRY(fr_vm_cell_index_in_bounds(runtime, object_id, index, &index));
  FR_TRY(fr_cells_read(runtime, object_id, index, &tagged));

  state->ip += 5;
  return fr_vm_push(runtime, state, tagged);
}

static fr_err_t fr_vm_store_cell(fr_runtime_t *runtime,
                                 const fr_instruction_stream_t *view,
                                 fr_vm_state_t *state) {
  fr_slot_id_t slot_id = 0;
  uint16_t index = 0;
  fr_tagged_t value = 0;
  fr_object_id_t object_id = 0;

  FR_TRY(fr_instruction_read_cell_operands(view, state->ip, &slot_id, &index));
  FR_TRY(fr_vm_pop(runtime, state, &value));
  FR_TRY(fr_vm_cell_object_for_slot(runtime, slot_id, &object_id));
  FR_TRY(fr_vm_cell_index_in_bounds(runtime, object_id, index, &index));
  FR_TRY(fr_cells_write(runtime, object_id, index, value));

  state->ip += 5;
  return fr_vm_push(runtime, state, fr_tagged_nil());
}

static fr_err_t fr_vm_load_cell_dynamic(fr_runtime_t *runtime,
                                        const fr_instruction_stream_t *view,
                                        fr_vm_state_t *state) {
  fr_slot_id_t slot_id = 0;
  uint16_t index = 0;
  fr_tagged_t tagged = 0;
  fr_object_id_t object_id = 0;

  FR_TRY(fr_instruction_read_slot_operand(view, state->ip, &slot_id));
  FR_TRY(fr_vm_cell_object_for_slot(runtime, slot_id, &object_id));
  FR_TRY(fr_vm_pop_cell_index(runtime, state, object_id, &index));
  FR_TRY(fr_cells_read(runtime, object_id, index, &tagged));

  state->ip += 3;
  return fr_vm_push(runtime, state, tagged);
}

static fr_err_t fr_vm_store_cell_dynamic(fr_runtime_t *runtime,
                                         const fr_instruction_stream_t *view,
                                         fr_vm_state_t *state) {
  fr_slot_id_t slot_id = 0;
  uint16_t index = 0;
  fr_tagged_t value = 0;
  fr_object_id_t object_id = 0;

  FR_TRY(fr_instruction_read_slot_operand(view, state->ip, &slot_id));
  FR_TRY(fr_vm_cell_object_for_slot(runtime, slot_id, &object_id));
  FR_TRY(fr_vm_pop_cell_index(runtime, state, object_id, &index));
  FR_TRY(fr_vm_pop(runtime, state, &value));
  FR_TRY(fr_cells_write(runtime, object_id, index, value));

  state->ip += 3;
  return fr_vm_push(runtime, state, fr_tagged_nil());
}
#endif

#if FR_FEATURE_RECORDS
static fr_err_t fr_vm_load_field(fr_runtime_t *runtime,
                                 const fr_instruction_stream_t *view,
                                 fr_vm_state_t *state) {
  const uint8_t *field_name = NULL;
  uint8_t field_length = 0;
  fr_tagged_t tagged = 0;
  fr_object_id_t object_id = 0;

  FR_TRY(fr_instruction_read_field_operand(view, state->ip, &field_name,
                                           &field_length));
  FR_TRY(fr_vm_pop(runtime, state, &tagged));
  FR_TRY(fr_tagged_decode_object_id(tagged, &object_id));
  FR_TRY(fr_record_read_field(
      runtime, object_id,
      (fr_record_name_t){.bytes = field_name, .length = field_length},
      &tagged));

  state->ip = (fr_code_offset_t)(state->ip + 2u + field_length);
  return fr_vm_push(runtime, state, tagged);
}

static fr_err_t fr_vm_store_field(fr_runtime_t *runtime,
                                  const fr_instruction_stream_t *view,
                                  fr_vm_state_t *state) {
  const uint8_t *field_name = NULL;
  uint8_t field_length = 0;
  fr_tagged_t value = 0;
  fr_tagged_t record = 0;
  fr_object_id_t object_id = 0;

  FR_TRY(fr_instruction_read_field_operand(view, state->ip, &field_name,
                                           &field_length));
  FR_TRY(fr_vm_pop(runtime, state, &value));
  FR_TRY(fr_vm_pop(runtime, state, &record));
  FR_TRY(fr_tagged_decode_object_id(record, &object_id));
  FR_TRY(fr_record_write_field(
      runtime, object_id,
      (fr_record_name_t){.bytes = field_name, .length = field_length}, value));

  state->ip = (fr_code_offset_t)(state->ip + 2u + field_length);
  return fr_vm_push(runtime, state, fr_tagged_nil());
}
#endif

static fr_err_t fr_vm_run_code_object_depth(fr_runtime_t *runtime,
                                            fr_code_object_id_t code_object_id,
                                            const fr_tagged_t *args,
                                            uint8_t arg_count,
                                            fr_tagged_t *out_tagged,
                                            uint16_t call_depth) {
  fr_instruction_stream_t view;

  if (call_depth >= FR_PROFILE_MAX_CALL_DEPTH) {
    fr_vm_note_call_depth(runtime);
    return FR_ERR_OVERFLOW;
  }
  if (runtime == NULL || out_tagged == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_code_get_instructions(runtime, code_object_id, &view));
  return fr_vm_run_instruction_stream_depth(runtime, &view, args, arg_count,
                                            out_tagged, call_depth);
}

fr_err_t fr_vm_run_code_object(fr_runtime_t *runtime,
                               fr_code_object_id_t code_object_id,
                               fr_tagged_t *out_tagged) {
  return fr_vm_run_code_object_depth(runtime, code_object_id, NULL, 0,
                                     out_tagged, 0);
}

static fr_err_t fr_vm_run_slot_depth(fr_runtime_t *runtime,
                                     fr_slot_id_t slot_id,
                                     const fr_tagged_t *args,
                                     uint8_t arg_count,
                                     fr_tagged_t *out_tagged,
                                     uint16_t call_depth) {
  fr_tagged_t tagged = 0;
  fr_code_object_id_t code_object_id = 0;

  if (call_depth >= FR_PROFILE_MAX_CALL_DEPTH) {
    fr_vm_note_call_depth(runtime);
    return FR_ERR_OVERFLOW;
  }
  if (runtime == NULL || out_tagged == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_slot_read(runtime, slot_id, &tagged));
  FR_TRY(fr_tagged_decode_code_object_id(tagged, &code_object_id));
  return fr_vm_run_code_object_depth(runtime, code_object_id, args, arg_count,
                                     out_tagged, call_depth);
}

fr_err_t fr_vm_run_slot(fr_runtime_t *runtime, fr_slot_id_t slot_id,
                        fr_tagged_t *out_tagged) {
  return fr_vm_run_slot_depth(runtime, slot_id, NULL, 0, out_tagged, 0);
}

static fr_err_t fr_vm_call_slot(fr_runtime_t *runtime,
                                const fr_instruction_stream_t *view,
                                fr_vm_state_t *state) {
  fr_slot_id_t slot_id = 0;
  fr_tagged_t result = 0;

  FR_TRY(fr_instruction_read_slot_operand(view, state->ip, &slot_id));
  FR_TRY(fr_vm_run_slot_depth(runtime, slot_id, NULL, 0, &result,
                              (uint16_t)(state->call_depth + 1)));

  state->ip += 3;
  return fr_vm_push(runtime, state, result);
}

static fr_err_t fr_vm_call_slot_arg(fr_runtime_t *runtime,
                                    const fr_instruction_stream_t *view,
                                    fr_vm_state_t *state) {
  fr_slot_id_t slot_id = 0;
  uint8_t arg_count = 0;
  fr_tagged_t result = 0;
  fr_tagged_t args[FR_PROFILE_MAX_STACK_DEPTH];

  FR_TRY(fr_instruction_read_call_slot_arg_operands(view, state->ip, &slot_id,
                                                    &arg_count));
  if (state->depth < arg_count) {
    fr_vm_note_too_few_args(runtime, arg_count, state->depth);
    return FR_ERR_UNDERFLOW;
  }
  for (uint8_t i = 0; i < arg_count; i++) {
    FR_TRY(fr_vm_pop(runtime, state, &args[arg_count - 1 - i]));
  }
  FR_TRY(fr_vm_run_slot_depth(runtime, slot_id, args, arg_count, &result,
                              (uint16_t)(state->call_depth + 1)));

  state->ip += 4;
  return fr_vm_push(runtime, state, result);
}

static fr_err_t fr_vm_call_native_slot(fr_runtime_t *runtime,
                                       const fr_instruction_stream_t *view,
                                       fr_vm_state_t *state) {
  const fr_native_entry_t *entry = NULL;
  fr_slot_id_t slot_id = 0;
  fr_native_id_t native_id = 0;
  fr_tagged_t slot_tagged = 0;
  fr_tagged_t result = 0;
  fr_tagged_t args[FR_PROFILE_MAX_STACK_DEPTH];

  FR_TRY(fr_instruction_read_slot_operand(view, state->ip, &slot_id));
  FR_TRY(fr_slot_read(runtime, slot_id, &slot_tagged));
  FR_TRY(fr_tagged_decode_native_id(slot_tagged, &native_id));
  FR_TRY(fr_native_get(runtime, native_id, &entry));
  if (state->depth < entry->arity) {
    fr_vm_note_too_few_args(runtime, entry->arity, state->depth);
    return FR_ERR_UNDERFLOW;
  }
  for (uint8_t i = 0; i < entry->arity; i++) {
    FR_TRY(fr_vm_pop(runtime, state, &args[entry->arity - 1 - i]));
  }

  FR_TRY(fr_native_call_named(runtime, entry, fr_slot_name(runtime, slot_id),
                              args, entry->arity, &result));

  state->ip += 3;
  return fr_vm_push(runtime, state, result);
}

/* fr_vm_add_int sums into a temp wider than fr_int_t so lhs + rhs can't
 * wrap before the range check. The partition is small relative to fr_int_t,
 * so a runtime overflow test can't tell the wide temp from the older
 * sign-split precheck — both reject. These two type checks fail the build
 * if the temp ever loses its width, or if the partition grows past what an
 * int64_t sum can hold. The negative array size is the C99 trick. */
typedef char fr_vm_add_int_sum_must_be_wider_than_fr_int[
    (sizeof(int64_t) > sizeof(fr_int_t)) ? 1 : -1];
typedef char fr_vm_add_int_partition_must_fit_int64_sum[
    ((int64_t)FR_TAGGED_INT_MAX + (int64_t)FR_TAGGED_INT_MAX <= INT64_MAX)
        ? 1 : -1];

/* Same wide-temp discipline as fr_vm_add_int for sub/mul/div. The mul case
 * has the tightest constraint: the product of the partition extremes must
 * fit int64_t before the range check decides. */
typedef char fr_vm_arith_int_partition_product_must_fit_int64[
    ((int64_t)FR_TAGGED_INT_MAX * (int64_t)FR_TAGGED_INT_MAX <= INT64_MAX &&
     -(int64_t)FR_TAGGED_INT_MIN * -(int64_t)FR_TAGGED_INT_MIN <= INT64_MAX)
        ? 1 : -1];

static fr_err_t fr_vm_add_int(fr_runtime_t *runtime, fr_vm_state_t *state) {
  fr_tagged_t rhs_tagged = 0;
  fr_tagged_t lhs_tagged = 0;
  fr_int_t rhs = 0;
  fr_int_t lhs = 0;
  int64_t sum = 0;

  FR_TRY(fr_vm_pop(runtime, state, &rhs_tagged));
  FR_TRY(fr_vm_pop(runtime, state, &lhs_tagged));
  FR_TRY(fr_vm_decode_int(runtime, rhs_tagged, &rhs));
  FR_TRY(fr_vm_decode_int(runtime, lhs_tagged, &lhs));
  /* Wide temp: range check is independent of fr_int_t's native bounds. */
  sum = (int64_t)lhs + (int64_t)rhs;
  if (sum > (int64_t)FR_TAGGED_INT_MAX || sum < (int64_t)FR_TAGGED_INT_MIN) {
    fr_vm_note_integer_overflow(runtime);
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_tagged_encode_int((fr_int_t)sum, &lhs_tagged));

  state->ip += 1;
  return fr_vm_push(runtime, state, lhs_tagged);
}

static fr_err_t fr_vm_arith_int(fr_runtime_t *runtime, fr_vm_state_t *state,
                                fr_opcode_t op) {
  fr_tagged_t rhs_tagged = 0;
  fr_tagged_t lhs_tagged = 0;
  fr_int_t rhs = 0;
  fr_int_t lhs = 0;
  int64_t result = 0;

  FR_TRY(fr_vm_pop(runtime, state, &rhs_tagged));
  FR_TRY(fr_vm_pop(runtime, state, &lhs_tagged));
  FR_TRY(fr_vm_decode_int(runtime, rhs_tagged, &rhs));
  FR_TRY(fr_vm_decode_int(runtime, lhs_tagged, &lhs));

  switch (op) {
  case FR_OP_SUB_INT:
    result = (int64_t)lhs - (int64_t)rhs;
    break;
  case FR_OP_MUL_INT:
    result = (int64_t)lhs * (int64_t)rhs;
    break;
  case FR_OP_DIV_INT:
    if (rhs == 0) {
      return FR_ERR_RANGE;
    }
    result = (int64_t)lhs / (int64_t)rhs;
    break;
  default:
    return FR_ERR_INVALID;
  }
  if (result > (int64_t)FR_TAGGED_INT_MAX ||
      result < (int64_t)FR_TAGGED_INT_MIN) {
    fr_vm_note_integer_overflow(runtime);
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_tagged_encode_int((fr_int_t)result, &lhs_tagged));

  state->ip += 1;
  return fr_vm_push(runtime, state, lhs_tagged);
}

static fr_err_t fr_vm_compare_int(fr_runtime_t *runtime, fr_vm_state_t *state,
                                  fr_opcode_t op) {
  fr_tagged_t rhs_tagged = 0;
  fr_tagged_t lhs_tagged = 0;
  fr_int_t rhs = 0;
  fr_int_t lhs = 0;
  bool result = false;

  FR_TRY(fr_vm_pop(runtime, state, &rhs_tagged));
  FR_TRY(fr_vm_pop(runtime, state, &lhs_tagged));
  FR_TRY(fr_vm_decode_int(runtime, rhs_tagged, &rhs));
  FR_TRY(fr_vm_decode_int(runtime, lhs_tagged, &lhs));

  switch (op) {
  case FR_OP_LT_INT: result = lhs < rhs; break;
  case FR_OP_GT_INT: result = lhs > rhs; break;
  case FR_OP_LE_INT: result = lhs <= rhs; break;
  case FR_OP_GE_INT: result = lhs >= rhs; break;
  case FR_OP_EQ_INT: result = lhs == rhs; break;
  case FR_OP_NE_INT: result = lhs != rhs; break;
  default:
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_encode_bool(result, &lhs_tagged));
  state->ip += 1;
  return fr_vm_push(runtime, state, lhs_tagged);
}

static fr_err_t fr_vm_jump(const fr_instruction_stream_t *view,
                           fr_vm_state_t *state) {
  fr_code_offset_t target = 0;
  FR_TRY(fr_instruction_read_jump_operand(view, state->ip, &target));
  state->ip = target;
  return FR_OK;
}

static fr_err_t fr_vm_jump_if_falsy(fr_runtime_t *runtime,
                                    const fr_instruction_stream_t *view,
                                    fr_vm_state_t *state) {
  fr_code_offset_t target = 0;
  fr_tagged_t condition = 0;

  FR_TRY(fr_vm_pop(runtime, state, &condition));
  FR_TRY(fr_instruction_read_jump_operand(view, state->ip, &target));

  if (fr_tagged_is_falsy(condition)) {
    state->ip = target;
  } else {
    state->ip += 3;
  }
  return FR_OK;
}

static fr_err_t fr_vm_repeat_begin(fr_runtime_t *runtime,
                                   const fr_instruction_stream_t *view,
                                   fr_vm_state_t *state) {
  fr_code_offset_t target = 0;
  fr_tagged_t tagged = 0;
  fr_int_t count = 0;

  FR_TRY(fr_instruction_read_jump_operand(view, state->ip, &target));
  if (state->depth == 0) {
    fr_vm_note_stack_underflow(runtime);
    return FR_ERR_UNDERFLOW;
  }

  tagged = state->stack[state->depth - 1];
  FR_TRY(fr_vm_decode_int(runtime, tagged, &count));
  if (count < 0) {
    return FR_ERR_RANGE;
  }
  if (count == 0) {
    state->depth -= 1;
    state->ip = target;
  } else {
    state->ip += 3;
  }
  return FR_OK;
}

static fr_err_t fr_vm_repeat_next(fr_runtime_t *runtime,
                                  const fr_instruction_stream_t *view,
                                  fr_vm_state_t *state) {
  fr_code_offset_t target = 0;
  fr_tagged_t tagged = 0;
  fr_int_t count = 0;

  FR_TRY(fr_instruction_read_jump_operand(view, state->ip, &target));
  if (state->depth == 0) {
    fr_vm_note_stack_underflow(runtime);
    return FR_ERR_UNDERFLOW;
  }

  tagged = state->stack[state->depth - 1];
  FR_TRY(fr_vm_decode_int(runtime, tagged, &count));
  if (count <= 0) {
    return FR_ERR_INVALID;
  }
  if (count == 1) {
    state->depth -= 1;
    state->ip += 3;
    return FR_OK;
  }

  FR_TRY(fr_tagged_encode_int((fr_int_t)(count - 1),
                              &state->stack[state->depth - 1]));
  state->ip = target;
  return FR_OK;
}

static fr_err_t fr_vm_step(fr_runtime_t *runtime,
                           const fr_instruction_stream_t *view,
                           fr_vm_state_t *state) {
  switch ((fr_opcode_t)view->bytes[state->ip]) {
  case FR_OP_RETURN:
    return fr_vm_return(state);
  case FR_OP_LOAD_SLOT:
    return fr_vm_load_slot(runtime, view, state);
  case FR_OP_STORE_SLOT:
    return fr_vm_store_slot(runtime, view, state);
  case FR_OP_PUSH_INT:
    return fr_vm_push_int(runtime, view, state);
  case FR_OP_PUSH_OBJECT_ID:
    return fr_vm_push_object_id(runtime, view, state);
  case FR_OP_PUSH_CODE_ID:
    return fr_vm_push_code_id(runtime, view, state);
  case FR_OP_LOAD_ARG:
    return fr_vm_load_arg(runtime, view, state);
  case FR_OP_LOAD_LOCAL:
    return fr_vm_load_local(runtime, view, state);
  case FR_OP_STORE_LOCAL:
    return fr_vm_store_local(runtime, view, state);
#if FR_FEATURE_CELLS
  case FR_OP_LOAD_CELL:
    return fr_vm_load_cell(runtime, view, state);
  case FR_OP_STORE_CELL:
    return fr_vm_store_cell(runtime, view, state);
  case FR_OP_LOAD_CELL_DYNAMIC:
    return fr_vm_load_cell_dynamic(runtime, view, state);
  case FR_OP_STORE_CELL_DYNAMIC:
    return fr_vm_store_cell_dynamic(runtime, view, state);
#else
  case FR_OP_LOAD_CELL:
  case FR_OP_STORE_CELL:
  case FR_OP_LOAD_CELL_DYNAMIC:
  case FR_OP_STORE_CELL_DYNAMIC:
    return FR_ERR_UNSUPPORTED;
#endif
#if FR_FEATURE_RECORDS
  case FR_OP_LOAD_FIELD:
    return fr_vm_load_field(runtime, view, state);
  case FR_OP_STORE_FIELD:
    return fr_vm_store_field(runtime, view, state);
#else
  case FR_OP_LOAD_FIELD:
  case FR_OP_STORE_FIELD:
    return FR_ERR_UNSUPPORTED;
#endif
  case FR_OP_CALL_SLOT:
    return fr_vm_call_slot(runtime, view, state);
  case FR_OP_CALL_SLOT_ARG:
    return fr_vm_call_slot_arg(runtime, view, state);
  case FR_OP_CALL_NATIVE_SLOT:
    return fr_vm_call_native_slot(runtime, view, state);
  case FR_OP_ADD_INT:
    return fr_vm_add_int(runtime, state);
  case FR_OP_SUB_INT:
  case FR_OP_MUL_INT:
  case FR_OP_DIV_INT:
    return fr_vm_arith_int(runtime, state,
                           (fr_opcode_t)view->bytes[state->ip]);
  case FR_OP_LT_INT:
  case FR_OP_GT_INT:
  case FR_OP_LE_INT:
  case FR_OP_GE_INT:
  case FR_OP_EQ_INT:
  case FR_OP_NE_INT:
    return fr_vm_compare_int(runtime, state,
                             (fr_opcode_t)view->bytes[state->ip]);
  case FR_OP_JUMP:
    return fr_vm_jump(view, state);
  case FR_OP_JUMP_IF_FALSY:
    return fr_vm_jump_if_falsy(runtime, view, state);
  case FR_OP_DROP:
    return fr_vm_drop(runtime, state);
  case FR_OP_PUSH_NIL:
    return fr_vm_push_nil(runtime, state);
  case FR_OP_PUSH_FALSE:
    return fr_vm_push_bool(runtime, state, false);
  case FR_OP_PUSH_TRUE:
    return fr_vm_push_bool(runtime, state, true);
  case FR_OP_REPEAT_BEGIN:
    return fr_vm_repeat_begin(runtime, view, state);
  case FR_OP_REPEAT_NEXT:
    return fr_vm_repeat_next(runtime, view, state);
  case FR_OP_BYTES_RESET:
#if FR_FEATURE_BYTES
    fr_bytes_reset_if_outermost(runtime);
#endif
    state->ip++;
    return FR_OK;
  default:
    return FR_ERR_INVALID;
  }
}

static fr_err_t fr_vm_run_instruction_stream_depth(
    fr_runtime_t *runtime, const fr_instruction_stream_t *view,
    const fr_tagged_t *args, uint8_t arg_count, fr_tagged_t *out_tagged,
    uint16_t call_depth) {
  fr_vm_state_t state = {.call_depth = call_depth};
  fr_instruction_header_t header;
  uint16_t yield_countdown = FR_VM_YIELD_SAFE_POINTS;
  uint16_t poll_countdown = FR_VM_POLL_SAFE_POINTS;

  if (call_depth >= FR_PROFILE_MAX_CALL_DEPTH) {
    fr_vm_note_call_depth(runtime);
    return FR_ERR_OVERFLOW;
  }
  if (runtime == NULL || view == NULL || out_tagged == NULL) {
    return FR_ERR_INVALID;
  }
  if (arg_count > 0 && args == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_instruction_read_header(view, &header));
  if (header.arity != arg_count) {
    if (arg_count < header.arity) {
      fr_vm_note_too_few_args(runtime, header.arity, arg_count);
    } else {
      fr_vm_note_message(runtime, FR_DIAG_ARITY, FR_DIAG_MSG_NONE,
                         header.arity, arg_count);
    }
    return FR_ERR_INVALID;
  }

  for (uint8_t i = 0; i < arg_count; i++) {
    state.frame[i] = args[i];
  }
  for (uint8_t i = 0; i < header.local_count; i++) {
    state.frame[arg_count + i] = fr_tagged_nil();
  }
  state.arg_count = arg_count;
  state.local_count = header.local_count;
  state.ip = header.header_size;
  while (state.ip < view->length && !state.returned) {
    fr_opcode_t op;

    op = (fr_opcode_t)view->bytes[state.ip];
    FR_TRY(fr_vm_step(runtime, view, &state));
    /* Spec §9 safe points: statement boundary (DROP) and end of any body
       (RETURN). Loop back-edges in repeat/while/forever emit DROP before
       the jump, so DROP also covers each loop iteration. */
    if (op == FR_OP_DROP || op == FR_OP_RETURN) {
      poll_countdown--;
      if (poll_countdown == 0) {
        FR_TRY(fr_platform_poll_interrupt(runtime));
        poll_countdown = FR_VM_POLL_SAFE_POINTS;
      }
      if (fr_runtime_is_interrupted(runtime)) {
        return FR_ERR_INTERRUPTED;
      }
      if (runtime->events.active_count > 0) {
        FR_TRY(fr_event_drain(runtime));
      }
      yield_countdown--;
      if (yield_countdown == 0) {
        fr_platform_yield();
        yield_countdown = FR_VM_YIELD_SAFE_POINTS;
      }
      if (runtime->events.active_count > 0) {
        fr_event_report_overflow(runtime);
        FR_TRY(fr_event_dispatch(runtime));
      }
    }
  }

  if (state.depth == 0) {
    *out_tagged = fr_tagged_nil();
  } else {
    *out_tagged = state.stack[state.depth - 1];
  }
  return FR_OK;
}

fr_err_t fr_vm_run_instruction_stream(fr_runtime_t *runtime,
                                      const fr_instruction_stream_t *view,
                                      fr_tagged_t *out_tagged) {
  return fr_vm_run_instruction_stream_depth(runtime, view, NULL, 0, out_tagged,
                                            0);
}

fr_err_t fr_vm_run_boot(fr_runtime_t *runtime, fr_tagged_t *out_tagged) {
  fr_tagged_t tagged = 0;

  if (runtime == NULL || out_tagged == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_slot_read(runtime, FR_SLOT_BOOT, &tagged));
  if (fr_tagged_is_nil(tagged)) {
    *out_tagged = fr_tagged_nil();
    return FR_OK;
  }

  {
    fr_err_t err = fr_vm_run_slot(runtime, FR_SLOT_BOOT, out_tagged);
#if FR_FEATURE_BYTES
    fr_bytes_reset_if_outermost(runtime);
#endif
    return err;
  }
}
