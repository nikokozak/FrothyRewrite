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

#if FR_VM_TRUST
#define FR_VM_READ_SLOT(view, ip, out)                                        \
  fr_instruction_decode_slot((view), (ip), (out))
#define FR_VM_READ_INT(view, ip, out)                                         \
  fr_instruction_decode_int((view), (ip), (out))
#define FR_VM_READ_OBJECT_ID(view, ip, out)                                   \
  fr_instruction_decode_object_id((view), (ip), (out))
#define FR_VM_READ_CODE_ID(view, ip, out)                                     \
  fr_instruction_decode_code_id((view), (ip), (out))
#define FR_VM_READ_JUMP(view, ip, out)                                        \
  fr_instruction_decode_jump((view), (ip), (out))
#define FR_VM_READ_ARG(view, ip, out)                                         \
  fr_instruction_decode_arg((view), (ip), (out))
#define FR_VM_READ_LOCAL(view, ip, out)                                       \
  fr_instruction_decode_local((view), (ip), (out))
#define FR_VM_READ_CALL_SLOT_ARG(view, ip, out_slot, out_count)               \
  fr_instruction_decode_call_slot_arg((view), (ip), (out_slot), (out_count))
#if FR_FEATURE_CELLS
#define FR_VM_READ_CELL(view, ip, out_slot, out_index)                        \
  fr_instruction_decode_cell((view), (ip), (out_slot), (out_index))
#endif
#else
#define FR_VM_READ_SLOT(view, ip, out)                                        \
  fr_instruction_read_slot_operand((view), (ip), (out))
#define FR_VM_READ_INT(view, ip, out)                                         \
  fr_instruction_read_int_operand((view), (ip), (out))
#define FR_VM_READ_OBJECT_ID(view, ip, out)                                   \
  fr_instruction_read_object_id_operand((view), (ip), (out))
#define FR_VM_READ_CODE_ID(view, ip, out)                                     \
  fr_instruction_read_code_id_operand((view), (ip), (out))
#define FR_VM_READ_JUMP(view, ip, out)                                        \
  fr_instruction_read_jump_operand((view), (ip), (out))
#define FR_VM_READ_ARG(view, ip, out)                                         \
  fr_instruction_read_arg_operand((view), (ip), (out))
#define FR_VM_READ_LOCAL(view, ip, out)                                       \
  fr_instruction_read_local_operand((view), (ip), (out))
#define FR_VM_READ_CALL_SLOT_ARG(view, ip, out_slot, out_count)               \
  fr_instruction_read_call_slot_arg_operands((view), (ip), (out_slot),        \
                                             (out_count))
#if FR_FEATURE_CELLS
#define FR_VM_READ_CELL(view, ip, out_slot, out_index)                        \
  fr_instruction_read_cell_operands((view), (ip), (out_slot), (out_index))
#endif
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

static fr_err_t fr_vm_push(fr_vm_state_t *state, fr_tagged_t tagged) {
  if (state->depth >= FR_PROFILE_MAX_STACK_DEPTH) {
    return FR_ERR_OVERFLOW;
  }
  state->stack[state->depth] = tagged;
  state->depth += 1;
  return FR_OK;
}

static fr_err_t fr_vm_pop(fr_vm_state_t *state, fr_tagged_t *out_tagged) {
  if (state->depth == 0) {
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

static fr_err_t fr_vm_drop(fr_vm_state_t *state) {
  fr_tagged_t ignored = 0;
  state->ip += 1;
  return fr_vm_pop(state, &ignored);
}

static fr_err_t fr_vm_load_slot(fr_runtime_t *runtime,
                                const fr_instruction_stream_t *view,
                                fr_vm_state_t *state) {
  fr_slot_id_t slot_id = 0;
  fr_tagged_t tagged = 0;

  FR_TRY(FR_VM_READ_SLOT(view, state->ip, &slot_id));
  FR_TRY(fr_slot_read(runtime, slot_id, &tagged));

  state->ip += 3;
  return fr_vm_push(state, tagged);
}

static fr_err_t fr_vm_store_slot(fr_runtime_t *runtime,
                                 const fr_instruction_stream_t *view,
                                 fr_vm_state_t *state) {
  fr_slot_id_t slot_id = 0;
  fr_tagged_t tagged = 0;

  FR_TRY(FR_VM_READ_SLOT(view, state->ip, &slot_id));
  FR_TRY(fr_vm_pop(state, &tagged));
  FR_TRY(fr_slot_write(runtime, slot_id, tagged));

  state->ip += 3;
  return fr_vm_push(state, fr_tagged_nil());
}

static fr_err_t fr_vm_push_int(const fr_instruction_stream_t *view,
                               fr_vm_state_t *state) {
  fr_int_t int_operand = 0;
  fr_tagged_t tagged = 0;

  FR_TRY(FR_VM_READ_INT(view, state->ip, &int_operand));
  FR_TRY(fr_tagged_encode_int(int_operand, &tagged));

  state->ip += FR_INSTRUCTION_PUSH_INT_SIZE;
  return fr_vm_push(state, tagged);
}

static fr_err_t fr_vm_push_object_id(const fr_instruction_stream_t *view,
                                     fr_vm_state_t *state) {
  fr_object_id_t object_id = 0;
  fr_tagged_t tagged = 0;

  FR_TRY(FR_VM_READ_OBJECT_ID(view, state->ip, &object_id));
  FR_TRY(fr_tagged_encode_object_id(object_id, &tagged));

  state->ip += FR_INSTRUCTION_PUSH_OBJECT_ID_SIZE;
  return fr_vm_push(state, tagged);
}

/* The body code object id reaches the event-register native as a plain int.
 * The operand is patched from a local code index to the runtime id at install
 * time, the same path the text patcher uses for object refs. */
static fr_err_t fr_vm_push_code_id(const fr_instruction_stream_t *view,
                                   fr_vm_state_t *state) {
  fr_code_object_id_t code_id = 0;
  fr_tagged_t tagged = 0;

  FR_TRY(FR_VM_READ_CODE_ID(view, state->ip, &code_id));
  FR_TRY(fr_tagged_encode_int((fr_int_t)code_id, &tagged));

  state->ip += FR_INSTRUCTION_PUSH_CODE_ID_SIZE;
  return fr_vm_push(state, tagged);
}

static fr_err_t fr_vm_push_nil(fr_vm_state_t *state) {
  state->ip += 1;
  return fr_vm_push(state, fr_tagged_nil());
}

static fr_err_t fr_vm_push_bool(fr_vm_state_t *state, bool value) {
  fr_tagged_t tagged = 0;

  FR_TRY(fr_tagged_encode_bool(value, &tagged));
  state->ip += 1;
  return fr_vm_push(state, tagged);
}

static fr_err_t fr_vm_load_arg(const fr_instruction_stream_t *view,
                               fr_vm_state_t *state) {
  uint8_t arg_index = 0;

  FR_TRY(FR_VM_READ_ARG(view, state->ip, &arg_index));
  if (arg_index >= state->arg_count) {
    return FR_ERR_INVALID;
  }

  state->ip += 2;
  return fr_vm_push(state, state->frame[arg_index]);
}

static fr_err_t fr_vm_load_local(const fr_instruction_stream_t *view,
                                 fr_vm_state_t *state) {
  uint8_t local_index = 0;

  FR_TRY(FR_VM_READ_LOCAL(view, state->ip, &local_index));
  if (local_index >= state->local_count) {
    return FR_ERR_INVALID;
  }

  state->ip += 2;
  return fr_vm_push(state, state->frame[state->arg_count + local_index]);
}

static fr_err_t fr_vm_store_local(const fr_instruction_stream_t *view,
                                  fr_vm_state_t *state) {
  uint8_t local_index = 0;
  fr_tagged_t value = 0;

  FR_TRY(FR_VM_READ_LOCAL(view, state->ip, &local_index));
  if (local_index >= state->local_count) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_vm_pop(state, &value));

  state->frame[state->arg_count + local_index] = value;
  state->ip += 2;
  return fr_vm_push(state, fr_tagged_nil());
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

static fr_err_t fr_vm_pop_cell_index(fr_runtime_t *runtime,
                                     fr_vm_state_t *state,
                                     fr_object_id_t object_id,
                                     uint16_t *out_index) {
  fr_tagged_t tagged = 0;
  fr_int_t raw_index = 0;
  uint16_t length = 0;

  if (out_index == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_vm_pop(state, &tagged));
  FR_TRY(fr_tagged_decode_int(tagged, &raw_index));
  FR_TRY(fr_cells_length(runtime, object_id, &length));
  if (raw_index < 0 || (uint32_t)raw_index >= length) {
    return FR_ERR_RANGE;
  }
  *out_index = (uint16_t)raw_index;
  return FR_OK;
}

static fr_err_t fr_vm_load_cell(fr_runtime_t *runtime,
                                const fr_instruction_stream_t *view,
                                fr_vm_state_t *state) {
  fr_slot_id_t slot_id = 0;
  uint16_t index = 0;
  fr_tagged_t tagged = 0;
  fr_object_id_t object_id = 0;

  FR_TRY(FR_VM_READ_CELL(view, state->ip, &slot_id, &index));
  FR_TRY(fr_vm_cell_object_for_slot(runtime, slot_id, &object_id));
  FR_TRY(fr_cells_read(runtime, object_id, index, &tagged));

  state->ip += 5;
  return fr_vm_push(state, tagged);
}

static fr_err_t fr_vm_store_cell(fr_runtime_t *runtime,
                                 const fr_instruction_stream_t *view,
                                 fr_vm_state_t *state) {
  fr_slot_id_t slot_id = 0;
  uint16_t index = 0;
  fr_tagged_t value = 0;
  fr_object_id_t object_id = 0;

  FR_TRY(FR_VM_READ_CELL(view, state->ip, &slot_id, &index));
  FR_TRY(fr_vm_pop(state, &value));
  FR_TRY(fr_vm_cell_object_for_slot(runtime, slot_id, &object_id));
  FR_TRY(fr_cells_write(runtime, object_id, index, value));

  state->ip += 5;
  return fr_vm_push(state, fr_tagged_nil());
}

static fr_err_t fr_vm_load_cell_dynamic(fr_runtime_t *runtime,
                                        const fr_instruction_stream_t *view,
                                        fr_vm_state_t *state) {
  fr_slot_id_t slot_id = 0;
  uint16_t index = 0;
  fr_tagged_t tagged = 0;
  fr_object_id_t object_id = 0;

  FR_TRY(FR_VM_READ_SLOT(view, state->ip, &slot_id));
  FR_TRY(fr_vm_cell_object_for_slot(runtime, slot_id, &object_id));
  FR_TRY(fr_vm_pop_cell_index(runtime, state, object_id, &index));
  FR_TRY(fr_cells_read(runtime, object_id, index, &tagged));

  state->ip += 3;
  return fr_vm_push(state, tagged);
}

static fr_err_t fr_vm_store_cell_dynamic(fr_runtime_t *runtime,
                                         const fr_instruction_stream_t *view,
                                         fr_vm_state_t *state) {
  fr_slot_id_t slot_id = 0;
  uint16_t index = 0;
  fr_tagged_t value = 0;
  fr_object_id_t object_id = 0;

  FR_TRY(FR_VM_READ_SLOT(view, state->ip, &slot_id));
  FR_TRY(fr_vm_cell_object_for_slot(runtime, slot_id, &object_id));
  FR_TRY(fr_vm_pop_cell_index(runtime, state, object_id, &index));
  FR_TRY(fr_vm_pop(state, &value));
  FR_TRY(fr_cells_write(runtime, object_id, index, value));

  state->ip += 3;
  return fr_vm_push(state, fr_tagged_nil());
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
  FR_TRY(fr_vm_pop(state, &tagged));
  FR_TRY(fr_tagged_decode_object_id(tagged, &object_id));
  FR_TRY(fr_record_read_field(
      runtime, object_id,
      (fr_record_name_t){.bytes = field_name, .length = field_length},
      &tagged));

  state->ip = (fr_code_offset_t)(state->ip + 2u + field_length);
  return fr_vm_push(state, tagged);
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
  FR_TRY(fr_vm_pop(state, &value));
  FR_TRY(fr_vm_pop(state, &record));
  FR_TRY(fr_tagged_decode_object_id(record, &object_id));
  FR_TRY(fr_record_write_field(
      runtime, object_id,
      (fr_record_name_t){.bytes = field_name, .length = field_length}, value));

  state->ip = (fr_code_offset_t)(state->ip + 2u + field_length);
  return fr_vm_push(state, fr_tagged_nil());
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

  FR_TRY(FR_VM_READ_SLOT(view, state->ip, &slot_id));
  FR_TRY(fr_vm_run_slot_depth(runtime, slot_id, NULL, 0, &result,
                              (uint16_t)(state->call_depth + 1)));

  state->ip += 3;
  return fr_vm_push(state, result);
}

static fr_err_t fr_vm_call_slot_arg(fr_runtime_t *runtime,
                                    const fr_instruction_stream_t *view,
                                    fr_vm_state_t *state) {
  fr_slot_id_t slot_id = 0;
  uint8_t arg_count = 0;
  fr_tagged_t result = 0;
  fr_tagged_t args[FR_PROFILE_MAX_STACK_DEPTH];

  FR_TRY(FR_VM_READ_CALL_SLOT_ARG(view, state->ip, &slot_id, &arg_count));
  if (state->depth < arg_count) {
    return FR_ERR_UNDERFLOW;
  }
  for (uint8_t i = 0; i < arg_count; i++) {
    FR_TRY(fr_vm_pop(state, &args[arg_count - 1 - i]));
  }
  FR_TRY(fr_vm_run_slot_depth(runtime, slot_id, args, arg_count, &result,
                              (uint16_t)(state->call_depth + 1)));

  state->ip += 4;
  return fr_vm_push(state, result);
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

  FR_TRY(FR_VM_READ_SLOT(view, state->ip, &slot_id));
  FR_TRY(fr_slot_read(runtime, slot_id, &slot_tagged));
  FR_TRY(fr_tagged_decode_native_id(slot_tagged, &native_id));
  FR_TRY(fr_native_get(runtime, native_id, &entry));
  if (state->depth < entry->arity) {
    return FR_ERR_UNDERFLOW;
  }
  for (uint8_t i = 0; i < entry->arity; i++) {
    FR_TRY(fr_vm_pop(state, &args[entry->arity - 1 - i]));
  }

  FR_TRY(fr_native_call(runtime, entry, args, entry->arity, &result));

  state->ip += 3;
  return fr_vm_push(state, result);
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

static fr_err_t fr_vm_add_int(fr_vm_state_t *state) {
  fr_tagged_t rhs_tagged = 0;
  fr_tagged_t lhs_tagged = 0;
  fr_int_t rhs = 0;
  fr_int_t lhs = 0;
  int64_t sum = 0;

  FR_TRY(fr_vm_pop(state, &rhs_tagged));
  FR_TRY(fr_vm_pop(state, &lhs_tagged));
  FR_TRY(fr_tagged_decode_int(rhs_tagged, &rhs));
  FR_TRY(fr_tagged_decode_int(lhs_tagged, &lhs));
  /* Wide temp: range check is independent of fr_int_t's native bounds. */
  sum = (int64_t)lhs + (int64_t)rhs;
  if (sum > (int64_t)FR_TAGGED_INT_MAX || sum < (int64_t)FR_TAGGED_INT_MIN) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_tagged_encode_int((fr_int_t)sum, &lhs_tagged));

  state->ip += 1;
  return fr_vm_push(state, lhs_tagged);
}

static fr_err_t fr_vm_arith_int(fr_vm_state_t *state, fr_opcode_t op) {
  fr_tagged_t rhs_tagged = 0;
  fr_tagged_t lhs_tagged = 0;
  fr_int_t rhs = 0;
  fr_int_t lhs = 0;
  int64_t result = 0;

  FR_TRY(fr_vm_pop(state, &rhs_tagged));
  FR_TRY(fr_vm_pop(state, &lhs_tagged));
  FR_TRY(fr_tagged_decode_int(rhs_tagged, &rhs));
  FR_TRY(fr_tagged_decode_int(lhs_tagged, &lhs));

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
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_tagged_encode_int((fr_int_t)result, &lhs_tagged));

  state->ip += 1;
  return fr_vm_push(state, lhs_tagged);
}

static fr_err_t fr_vm_compare_int(fr_vm_state_t *state, fr_opcode_t op) {
  fr_tagged_t rhs_tagged = 0;
  fr_tagged_t lhs_tagged = 0;
  fr_int_t rhs = 0;
  fr_int_t lhs = 0;
  bool result = false;

  FR_TRY(fr_vm_pop(state, &rhs_tagged));
  FR_TRY(fr_vm_pop(state, &lhs_tagged));
  FR_TRY(fr_tagged_decode_int(rhs_tagged, &rhs));
  FR_TRY(fr_tagged_decode_int(lhs_tagged, &lhs));

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
  return fr_vm_push(state, lhs_tagged);
}

static fr_err_t fr_vm_jump(const fr_instruction_stream_t *view,
                           fr_vm_state_t *state) {
  fr_code_offset_t target = 0;
  FR_TRY(FR_VM_READ_JUMP(view, state->ip, &target));
  state->ip = target;
  return FR_OK;
}

static fr_err_t fr_vm_jump_if_falsy(const fr_instruction_stream_t *view,
                                    fr_vm_state_t *state) {
  fr_code_offset_t target = 0;
  fr_tagged_t condition = 0;

  FR_TRY(fr_vm_pop(state, &condition));
  FR_TRY(FR_VM_READ_JUMP(view, state->ip, &target));

  if (fr_tagged_is_falsy(condition)) {
    state->ip = target;
  } else {
    state->ip += 3;
  }
  return FR_OK;
}

static fr_err_t fr_vm_repeat_begin(const fr_instruction_stream_t *view,
                                   fr_vm_state_t *state) {
  fr_code_offset_t target = 0;
  fr_tagged_t tagged = 0;
  fr_int_t count = 0;

  FR_TRY(FR_VM_READ_JUMP(view, state->ip, &target));
  if (state->depth == 0) {
    return FR_ERR_UNDERFLOW;
  }

  tagged = state->stack[state->depth - 1];
  FR_TRY(fr_tagged_decode_int(tagged, &count));
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

static fr_err_t fr_vm_repeat_next(const fr_instruction_stream_t *view,
                                  fr_vm_state_t *state) {
  fr_code_offset_t target = 0;
  fr_tagged_t tagged = 0;
  fr_int_t count = 0;

  FR_TRY(FR_VM_READ_JUMP(view, state->ip, &target));
  if (state->depth == 0) {
    return FR_ERR_UNDERFLOW;
  }

  tagged = state->stack[state->depth - 1];
  FR_TRY(fr_tagged_decode_int(tagged, &count));
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
    return fr_vm_push_int(view, state);
  case FR_OP_PUSH_OBJECT_ID:
    return fr_vm_push_object_id(view, state);
  case FR_OP_PUSH_CODE_ID:
    return fr_vm_push_code_id(view, state);
  case FR_OP_LOAD_ARG:
    return fr_vm_load_arg(view, state);
  case FR_OP_LOAD_LOCAL:
    return fr_vm_load_local(view, state);
  case FR_OP_STORE_LOCAL:
    return fr_vm_store_local(view, state);
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
    return fr_vm_add_int(state);
  case FR_OP_SUB_INT:
  case FR_OP_MUL_INT:
  case FR_OP_DIV_INT:
    return fr_vm_arith_int(state, (fr_opcode_t)view->bytes[state->ip]);
  case FR_OP_LT_INT:
  case FR_OP_GT_INT:
  case FR_OP_LE_INT:
  case FR_OP_GE_INT:
  case FR_OP_EQ_INT:
  case FR_OP_NE_INT:
    return fr_vm_compare_int(state, (fr_opcode_t)view->bytes[state->ip]);
  case FR_OP_JUMP:
    return fr_vm_jump(view, state);
  case FR_OP_JUMP_IF_FALSY:
    return fr_vm_jump_if_falsy(view, state);
  case FR_OP_DROP:
    return fr_vm_drop(state);
  case FR_OP_PUSH_NIL:
    return fr_vm_push_nil(state);
  case FR_OP_PUSH_FALSE:
    return fr_vm_push_bool(state, false);
  case FR_OP_PUSH_TRUE:
    return fr_vm_push_bool(state, true);
  case FR_OP_REPEAT_BEGIN:
    return fr_vm_repeat_begin(view, state);
  case FR_OP_REPEAT_NEXT:
    return fr_vm_repeat_next(view, state);
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

  if (call_depth >= FR_PROFILE_MAX_CALL_DEPTH) {
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
      FR_TRY(fr_platform_poll_interrupt(runtime));
      if (fr_runtime_is_interrupted(runtime)) {
        return FR_ERR_INTERRUPTED;
      }
      FR_TRY(fr_event_drain(runtime));
      yield_countdown--;
      if (yield_countdown == 0) {
        fr_platform_yield();
        yield_countdown = FR_VM_YIELD_SAFE_POINTS;
      }
      fr_event_report_overflow(runtime);
      FR_TRY(fr_event_dispatch(runtime));
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
  FR_TRY(fr_verify_code_object(view));
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
