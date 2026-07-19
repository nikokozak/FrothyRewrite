#include "native.h"

#include "object.h"
#include "runtime.h"

#include <string.h>

#if FR_FEATURE_NATIVE_SIGNATURES
static fr_diag_value_kind_t
fr_native_value_diag_kind(fr_native_value_kind_t kind) {
  switch (kind) {
  case FR_NATIVE_VALUE_ANY:
    return FR_DIAG_VALUE_ANY;
  case FR_NATIVE_VALUE_INT:
    return FR_DIAG_VALUE_INT;
  case FR_NATIVE_VALUE_HANDLE:
    return FR_DIAG_VALUE_HANDLE;
  case FR_NATIVE_VALUE_NIL:
    return FR_DIAG_VALUE_NIL;
  case FR_NATIVE_VALUE_TEXT:
  case FR_NATIVE_VALUE_SECRET_TEXT:
    return FR_DIAG_VALUE_TEXT;
  case FR_NATIVE_VALUE_TEXT_OR_BYTES:
    return FR_DIAG_VALUE_TEXT_OR_BYTES;
  case FR_NATIVE_VALUE_BYTES:
    return FR_DIAG_VALUE_BYTES;
  default:
    return FR_DIAG_VALUE_NONE;
  }
}
#endif

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
  case FR_NATIVE_VALUE_SECRET_TEXT:
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
  case FR_NATIVE_VALUE_TEXT_OR_BYTES: {
#if FR_FEATURE_TEXT
    fr_object_id_t text_id = 0;
    const uint8_t *ignored_bytes = NULL;
    uint16_t ignored_length = 0;

    if (runtime != NULL &&
        fr_tagged_decode_object_id(value, &text_id) == FR_OK &&
        fr_text_view(runtime, text_id, &ignored_bytes, &ignored_length) ==
            FR_OK) {
      return true;
    }
#endif
#if FR_FEATURE_BYTES
    {
      fr_bytes_ref_t bytes_ref = {0};
      return fr_tagged_decode_bytes_ref(value, &bytes_ref) == FR_OK;
    }
#else
    return false;
#endif
  }
  case FR_NATIVE_VALUE_BYTES:
#if FR_FEATURE_BYTES
    return fr_tagged_kind(value) == FR_TAGGED_BYTES;
#else
    return false;
#endif
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

static bool fr_native_diag_empty(const fr_runtime_t *runtime) {
  return runtime != NULL && runtime->diag != NULL &&
         runtime->diag->kind == FR_DIAG_NONE;
}

fr_err_t fr_native_reject_arg(fr_runtime_t *runtime, const fr_tagged_t *args,
                              uint8_t arg_count, uint8_t index, fr_err_t err) {
  switch (err) {
  case FR_ERR_RANGE:
  case FR_ERR_TYPE:
  case FR_ERR_DOMAIN:
  case FR_ERR_CAPACITY:
  case FR_ERR_OVERFLOW:
  case FR_ERR_UNDERFLOW:
  case FR_ERR_NOT_FOUND:
  case FR_ERR_INVALID:
  case FR_ERR_UNSUPPORTED:
  case FR_ERR_HANDLE:
  case FR_ERR_VOLATILE:
  case FR_ERR_NET_TOO_LARGE:
  case FR_ERR_BUSY:
    break;
  case FR_OK:
  case FR_ERR_INTERRUPTED:
  case FR_ERR_CORRUPT:
  case FR_ERR_IO:
  case FR_ERR_NET_DISCONNECTED:
  case FR_ERR_NET_TIMEOUT:
  case FR_ERR_NET_DNS:
  case FR_ERR_NET_REFUSED:
  case FR_ERR_NET_PROTOCOL:
  case FR_ERR_BLE_NOT_READY:
  case FR_ERR_BLE_BUSY:
  case FR_ERR_BLE_TIMEOUT:
  case FR_ERR_BLE_DISCONNECTED:
  default:
    return err;
  }

  if (!fr_native_diag_empty(runtime) || args == NULL || index >= arg_count) {
    return err;
  }

  *runtime->diag = (fr_diagnostic_t){0};
  runtime->diag->kind = FR_DIAG_NOTE;
  runtime->diag->message_id = FR_DIAG_MSG_RUNTIME_REJECTED_ARGUMENT;
  runtime->diag->got = fr_runtime_diag_value_kind(runtime, args[index]);
  runtime->diag->actual = args[index];
  /* Default closed. The dispatcher below reveals only declared public args. */
  runtime->diag->actual_state = FR_DIAG_ACTUAL_REDACTED;
  runtime->diag->index = index;
  return err;
}

static const char *fr_native_diag_context_name(fr_diagnostic_t *diag,
                                               const char *context_name) {
  uint16_t length = 0;

  if (diag == NULL) {
    return "native";
  }
  if (context_name == NULL) {
    return "native";
  }
  while (context_name[length] != '\0') {
    if (length >= FR_PROFILE_PARSE_MAX_TOKEN_BYTES) {
      return "native";
    }
    length = (uint16_t)(length + 1);
  }
  memcpy(diag->suggestion_text, context_name, length);
  diag->suggestion_text[length] = '\0';
  return diag->suggestion_text;
}

#if FR_FEATURE_NATIVE_SIGNATURES
static void fr_native_note_arg_type(fr_runtime_t *runtime,
                                    const char *context_name, uint8_t index,
                                    fr_native_value_kind_t expected,
                                    fr_tagged_t got) {
  if (!fr_native_diag_empty(runtime)) {
    return;
  }

  *runtime->diag = (fr_diagnostic_t){0};
  runtime->diag->kind = FR_DIAG_TYPE;
  runtime->diag->expected = fr_native_value_diag_kind(expected);
  runtime->diag->got = fr_runtime_diag_value_kind(runtime, got);
  runtime->diag->actual = got;
  runtime->diag->actual_state = expected == FR_NATIVE_VALUE_SECRET_TEXT
                                    ? FR_DIAG_ACTUAL_REDACTED
                                    : FR_DIAG_ACTUAL_VALUE;
  runtime->diag->index = index;
  runtime->diag->context_name =
      fr_native_diag_context_name(runtime->diag, context_name);
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
  return fr_native_call_named(runtime, entry, NULL, args, arg_count, out);
}

fr_err_t fr_native_call_named(fr_runtime_t *runtime,
                              const fr_native_entry_t *entry,
                              const char *context_name,
                              const fr_tagged_t *args, uint8_t arg_count,
                              fr_tagged_t *out) {
  fr_err_t err = FR_OK;

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
        fr_native_note_arg_type(runtime, context_name, i,
                                entry->signature->params[i].type, args[i]);
        return FR_ERR_TYPE;
      }
    }
  }

#endif

  err = entry->fn(runtime, args, arg_count, out);
  if (err != FR_OK) {
    if (runtime->diag != NULL &&
        runtime->diag->kind == FR_DIAG_NOTE &&
        runtime->diag->message_id == FR_DIAG_MSG_RUNTIME_REJECTED_ARGUMENT &&
        runtime->diag->index < arg_count) {
      runtime->diag->context_name =
          fr_native_diag_context_name(runtime->diag, context_name);
#if FR_FEATURE_NATIVE_SIGNATURES
      if (entry->signature != NULL &&
          runtime->diag->index < entry->signature->arg_count) {
        runtime->diag->actual_state =
            entry->signature->params[runtime->diag->index].type ==
                    FR_NATIVE_VALUE_SECRET_TEXT
                ? FR_DIAG_ACTUAL_REDACTED
                : FR_DIAG_ACTUAL_VALUE;
      } else {
        runtime->diag->actual_state = FR_DIAG_ACTUAL_NONE;
      }
#else
      runtime->diag->actual_state = FR_DIAG_ACTUAL_NONE;
#endif
    }
    return err;
  }
  if (runtime->diag != NULL && runtime->diag->kind == FR_DIAG_NOTE &&
      runtime->diag->message_id == FR_DIAG_MSG_RUNTIME_REJECTED_ARGUMENT) {
    memset(runtime->diag, 0, sizeof(*runtime->diag));
  }

#if FR_FEATURE_NATIVE_SIGNATURES
  if (entry->signature != NULL &&
      !fr_native_value_matches(runtime, entry->signature->result, *out)) {
    return FR_ERR_TYPE;
  }
#endif

  return FR_OK;
}
