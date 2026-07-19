#pragma once

#include "tagged.h"
#include "types.h"

typedef fr_err_t (*fr_native_fn_t)(fr_runtime_t *runtime,
                                   const fr_tagged_t *args, uint8_t arg_count,
                                   fr_tagged_t *out);

typedef enum fr_native_value_kind_t {
  FR_NATIVE_VALUE_ANY,
  FR_NATIVE_VALUE_INT,
  FR_NATIVE_VALUE_HANDLE,
  FR_NATIVE_VALUE_NIL,
  FR_NATIVE_VALUE_TEXT,
  FR_NATIVE_VALUE_SECRET_TEXT,
  FR_NATIVE_VALUE_TEXT_OR_BYTES,
} fr_native_value_kind_t;

typedef struct fr_native_param_t {
  const char *name; /* display-only; may be NULL */
  fr_native_value_kind_t type;
} fr_native_param_t;

typedef struct fr_native_signature_t {
  const fr_native_param_t *params; /* one row per arg */
  uint8_t arg_count;
  fr_native_value_kind_t result;
  const char *help; /* display-only; may be NULL */
} fr_native_signature_t;

typedef struct fr_native_entry_t {
  fr_native_fn_t fn;
  uint8_t arity;
#if FR_FEATURE_NATIVE_SIGNATURES
  const fr_native_signature_t *signature;
#endif
} fr_native_entry_t;

typedef struct fr_native_table_t {
  fr_native_entry_t entries[FR_PROFILE_NATIVE_TABLE_SIZE];
  uint16_t count;
  uint16_t base_count;
} fr_native_table_t;

void fr_native_reset(fr_runtime_t *runtime);
void fr_native_mark_base(fr_runtime_t *runtime);
void fr_native_restore_base(fr_runtime_t *runtime);
/* Record which native argument was rejected. fr_native_call_named applies the
 * signature-owned display/redaction policy before the diagnostic is exposed. */
void fr_native_diag_note_rejected_arg(fr_runtime_t *runtime,
                                      const fr_tagged_t *args,
                                      uint8_t arg_count, uint8_t index);
fr_err_t fr_native_install(fr_runtime_t *runtime, fr_native_fn_t fn,
                           uint8_t arity,
                           const fr_native_signature_t *signature,
                           fr_native_id_t *out_native_id);
fr_err_t fr_native_get(const fr_runtime_t *runtime, fr_native_id_t native_id,
                       const fr_native_entry_t **out_entry);
fr_err_t fr_native_call(fr_runtime_t *runtime, const fr_native_entry_t *entry,
                        const fr_tagged_t *args, uint8_t arg_count,
                        fr_tagged_t *out);
fr_err_t fr_native_call_named(fr_runtime_t *runtime,
                              const fr_native_entry_t *entry,
                              const char *context_name,
                              const fr_tagged_t *args, uint8_t arg_count,
                              fr_tagged_t *out);
