#include "native.h"

#include "object.h"
#include "runtime.h"

#include <string.h>

void fr_native_reset(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }

  runtime->natives.count = 0;
  runtime->natives.base_count = 0;
  memset(runtime->natives.entries, 0, sizeof(runtime->natives.entries));
}

void fr_native_mark_base(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }

  runtime->natives.base_count = runtime->natives.count;
}

void fr_native_restore_base(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }

  runtime->natives.count = runtime->natives.base_count;
  memset(&runtime->natives.entries[runtime->natives.count], 0,
         (FR_PROFILE_NATIVE_TABLE_SIZE - runtime->natives.count) *
             sizeof(runtime->natives.entries[0]));
}

#if FR_FEATURE_NATIVE_SIGNATURES
static bool fr_native_value_matches(const fr_runtime_t *runtime,
                                    fr_native_value_kind_t kind,
                                    fr_tagged_t value) {
#if !FR_FEATURE_TEXT
  (void)runtime;
#endif

  switch (kind) {
  case FR_NATIVE_VALUE_ANY:
    return true;
  case FR_NATIVE_VALUE_INT:
    return fr_tagged_kind(value) == FR_TAGGED_INT;
  case FR_NATIVE_VALUE_HANDLE:
#if FR_FEATURE_HANDLES
    return fr_tagged_kind(value) == FR_TAGGED_HANDLE;
#else
    return false;
#endif
  case FR_NATIVE_VALUE_TEXT:
#if FR_FEATURE_TEXT
  {
    fr_object_id_t object_id = 0;
    const uint8_t *ignored_bytes = NULL;
    uint16_t ignored_length = 0;

    return runtime != NULL &&
           fr_tagged_decode_object_id(value, &object_id) == FR_OK &&
           fr_text_view(runtime, object_id, &ignored_bytes, &ignored_length) ==
               FR_OK;
  }
#else
    return false;
#endif
  case FR_NATIVE_VALUE_NIL:
    return fr_tagged_is_nil(value);
  }

  return false;
}

static fr_err_t
fr_native_signature_check(const fr_native_signature_t *signature,
                          uint8_t arity) {
  if (signature == NULL) {
    return FR_OK;
  }
  if (signature->arg_count != arity) {
    return FR_ERR_INVALID;
  }
  if (signature->arg_count > 0 && signature->params == NULL) {
    return FR_ERR_INVALID;
  }
  return FR_OK;
}
#endif

fr_err_t fr_native_install(fr_runtime_t *runtime, fr_native_fn_t fn,
                           uint8_t arity,
                           const fr_native_signature_t *signature,
                           fr_native_id_t *out_native_id) {
  if (runtime == NULL || fn == NULL || out_native_id == NULL) {
    return FR_ERR_INVALID;
  }
  if (arity > FR_PROFILE_MAX_STACK_DEPTH) {
    return FR_ERR_INVALID;
  }
#if FR_FEATURE_NATIVE_SIGNATURES
  FR_TRY(fr_native_signature_check(signature, arity));
#else
  (void)signature;
#endif
  if (runtime->natives.count >= FR_PROFILE_NATIVE_TABLE_SIZE) {
    return FR_ERR_CAPACITY;
  }

  runtime->natives.entries[runtime->natives.count].fn = fn;
  runtime->natives.entries[runtime->natives.count].arity = arity;
#if FR_FEATURE_NATIVE_SIGNATURES
  runtime->natives.entries[runtime->natives.count].signature = signature;
#endif
  *out_native_id = runtime->natives.count;
  runtime->natives.count += 1;
  return FR_OK;
}

fr_err_t fr_native_get(const fr_runtime_t *runtime, fr_native_id_t native_id,
                       const fr_native_entry_t **out_entry) {
  if (runtime == NULL || out_entry == NULL) {
    return FR_ERR_INVALID;
  }
  if (native_id >= runtime->natives.count) {
    return FR_ERR_NOT_FOUND;
  }

  *out_entry = &runtime->natives.entries[native_id];
  if ((*out_entry)->fn == NULL) {
    return FR_ERR_INVALID;
  }
  return FR_OK;
}

fr_err_t fr_native_call(fr_runtime_t *runtime, const fr_native_entry_t *entry,
                        const fr_tagged_t *args, uint8_t arg_count,
                        fr_tagged_t *out) {
  if (runtime == NULL || entry == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (arg_count > 0 && args == NULL) {
    return FR_ERR_INVALID;
  }
  if (arg_count != entry->arity) {
    return FR_ERR_INVALID;
  }
#if FR_FEATURE_NATIVE_SIGNATURES
  if (entry->signature != NULL) {
    FR_TRY(fr_native_signature_check(entry->signature, entry->arity));
    for (uint8_t i = 0; i < arg_count; i++) {
      if (!fr_native_value_matches(runtime, entry->signature->params[i].type,
                                   args[i])) {
        return FR_ERR_TYPE;
      }
    }
  }

#endif

  FR_TRY(entry->fn(runtime, args, arg_count, out));

#if FR_FEATURE_NATIVE_SIGNATURES
  if (entry->signature != NULL &&
      !fr_native_value_matches(runtime, entry->signature->result, *out)) {
    return FR_ERR_TYPE;
  }
#endif

  return FR_OK;
}
