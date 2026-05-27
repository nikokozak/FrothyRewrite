#include "base_defs.h"

#include "handle.h"
#include "object.h"
#include "pad.h"
#include "platform.h"
#include "runtime.h"
#include "tagged.h"

#include <stdint.h>

static fr_err_t fr_native_decode_nonnegative_int(const fr_tagged_t *args,
                                                 uint8_t arg_count,
                                                 uint8_t index,
                                                 fr_int_t *out_value) {
  if (args == NULL || index >= arg_count || out_value == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_int(args[index], out_value));
  if (*out_value < 0) {
    return FR_ERR_DOMAIN;
  }
  return FR_OK;
}

static fr_err_t fr_native_decode_u16(const fr_tagged_t *args,
                                     uint8_t arg_count, uint8_t index,
                                     uint16_t *out_value) {
  fr_int_t value = 0;

  if (out_value == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, index, &value));
  if ((uint32_t)value > UINT16_MAX) {
    return FR_ERR_DOMAIN;
  }

  *out_value = (uint16_t)value;
  return FR_OK;
}

static fr_err_t fr_native_ms(fr_runtime_t *runtime, const fr_tagged_t *args,
                             uint8_t arg_count, fr_tagged_t *out) {
  fr_int_t ms = 0;

  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, 0, &ms));
  while (ms > 0) {
    FR_TRY(fr_platform_poll_interrupt(runtime));
    if (fr_runtime_is_interrupted(runtime)) {
      return FR_ERR_INTERRUPTED;
    }
    FR_TRY(fr_platform_delay_ms(1));
    ms -= 1;
  }
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_millis(fr_runtime_t *runtime, const fr_tagged_t *args,
                                 uint8_t arg_count, fr_tagged_t *out) {
  uint16_t ms = 0;

  (void)runtime;
  if (args == NULL && arg_count > 0) {
    return FR_ERR_INVALID;
  }
  if (arg_count != 0) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_platform_millis(&ms));
  return fr_tagged_encode_int((int32_t)ms, out);
}

static fr_err_t fr_native_gpio_mode(fr_runtime_t *runtime,
                                    const fr_tagged_t *args,
                                    uint8_t arg_count, fr_tagged_t *out) {
  uint16_t pin = 0;
  uint16_t mode = 0;

  (void)runtime;

  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &pin));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &mode));
  FR_TRY(fr_platform_gpio_mode(pin, mode));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_gpio_write(fr_runtime_t *runtime,
                                     const fr_tagged_t *args,
                                     uint8_t arg_count, fr_tagged_t *out) {
  uint16_t pin = 0;
  uint16_t value = 0;

  (void)runtime;

  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &pin));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &value));
  FR_TRY(fr_platform_gpio_write(pin, value));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_gpio_read(fr_runtime_t *runtime,
                                    const fr_tagged_t *args, uint8_t arg_count,
                                    fr_tagged_t *out) {
  uint16_t pin = 0;
  uint16_t value = 0;

  (void)runtime;

  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &pin));
  FR_TRY(fr_platform_gpio_read(pin, &value));
  return fr_tagged_encode_int((int32_t)value, out);
}

static fr_err_t fr_native_adc_read(fr_runtime_t *runtime,
                                   const fr_tagged_t *args, uint8_t arg_count,
                                   fr_tagged_t *out) {
  uint16_t pin = 0;
  uint16_t value = 0;

  (void)runtime;

  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &pin));
  FR_TRY(fr_platform_adc_read(pin, &value));
  return fr_tagged_encode_int((int32_t)value, out);
}

static fr_err_t fr_native_adc_above(fr_runtime_t *runtime,
                                    const fr_tagged_t *args, uint8_t arg_count,
                                    fr_tagged_t *out) {
  uint16_t pin = 0;
  uint16_t threshold = 0;
  uint16_t value = 0;

  (void)runtime;

  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &pin));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &threshold));
  FR_TRY(fr_platform_adc_read(pin, &value));
  *out = value > threshold ? fr_tagged_true() : fr_tagged_false();
  return FR_OK;
}

#if FR_FEATURE_UART || FR_FEATURE_PAD
static fr_err_t fr_native_decode_byte(const fr_tagged_t *args,
                                      uint8_t arg_count, uint8_t index,
                                      uint8_t *out_byte) {
  fr_int_t value = 0;

  if (out_byte == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, index, &value));
  if (value > 255) {
    return FR_ERR_DOMAIN;
  }

  *out_byte = (uint8_t)value;
  return FR_OK;
}
#endif

#if FR_FEATURE_UART
static fr_err_t fr_native_decode_uart_handle(fr_runtime_t *runtime,
                                             const fr_tagged_t *args,
                                             uint8_t arg_count,
                                             uint8_t index,
                                             uint16_t *out_platform_index) {
  fr_handle_ref_t ref = {0};

  if (runtime == NULL || args == NULL || index >= arg_count ||
      out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_handle_ref(args[index], &ref));
  return fr_handle_lookup(runtime, ref, FR_HANDLE_KIND_UART, NULL,
                          out_platform_index);
}

static fr_err_t fr_native_uart_open(fr_runtime_t *runtime,
                                    const fr_tagged_t *args,
                                    uint8_t arg_count, fr_tagged_t *out) {
  uint16_t tx = 0;
  uint16_t rx = 0;
  uint16_t rate_code = 0;
  fr_handle_ref_t ref = {0};
  fr_tagged_t handle = 0;
  uint16_t platform_index = 0;
  fr_err_t err = FR_OK;

  if (runtime == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &tx));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &rx));
  FR_TRY(fr_native_decode_u16(args, arg_count, 2, &rate_code));

  FR_TRY(fr_handle_reserve(runtime, FR_HANDLE_KIND_UART, &ref, &handle));
  err = fr_platform_uart_open(tx, rx, rate_code, &platform_index);
  if (err != FR_OK) {
    (void)fr_handle_release_reserved(runtime, ref);
    return err;
  }

  err = fr_handle_activate(runtime, ref, platform_index);
  if (err != FR_OK) {
    (void)fr_platform_handle_close(FR_HANDLE_KIND_UART, platform_index);
    (void)fr_handle_release_reserved(runtime, ref);
    return err;
  }

  *out = handle;
  return FR_OK;
}

static fr_err_t fr_native_uart_write_byte(fr_runtime_t *runtime,
                                          const fr_tagged_t *args,
                                          uint8_t arg_count,
                                          fr_tagged_t *out) {
  uint16_t platform_index = 0;
  uint8_t byte = 0;

  FR_TRY(
      fr_native_decode_uart_handle(runtime, args, arg_count, 0, &platform_index));
  FR_TRY(fr_native_decode_byte(args, arg_count, 1, &byte));
  FR_TRY(fr_platform_uart_write_byte(platform_index, byte));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_uart_read_byte(fr_runtime_t *runtime,
                                         const fr_tagged_t *args,
                                         uint8_t arg_count, fr_tagged_t *out) {
  uint16_t platform_index = 0;
  uint8_t byte = 0;
  bool has_byte = false;

  FR_TRY(
      fr_native_decode_uart_handle(runtime, args, arg_count, 0, &platform_index));
  FR_TRY(fr_platform_uart_read_byte(platform_index, &byte, &has_byte));
  return fr_tagged_encode_int(has_byte ? (int32_t)byte : -1, out);
}

static fr_err_t fr_native_uart_available(fr_runtime_t *runtime,
                                         const fr_tagged_t *args,
                                         uint8_t arg_count, fr_tagged_t *out) {
  uint16_t platform_index = 0;
  uint16_t count = 0;

  FR_TRY(
      fr_native_decode_uart_handle(runtime, args, arg_count, 0, &platform_index));
  FR_TRY(fr_platform_uart_available(platform_index, &count));
  return fr_tagged_encode_int((int32_t)count, out);
}

static fr_err_t fr_native_uart_close(fr_runtime_t *runtime,
                                     const fr_tagged_t *args,
                                     uint8_t arg_count, fr_tagged_t *out) {
  fr_handle_ref_t ref = {0};
  fr_handle_kind_t kind = FR_HANDLE_KIND_NONE;
  uint16_t platform_index = 0;

  if (runtime == NULL || args == NULL || arg_count == 0 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_handle_ref(args[0], &ref));
  FR_TRY(fr_handle_lookup(runtime, ref, FR_HANDLE_KIND_UART, &kind,
                          &platform_index));
  (void)kind;
  (void)platform_index;
  FR_TRY(fr_handle_close(runtime, ref));
  *out = fr_tagged_nil();
  return FR_OK;
}
#endif

#if FR_FEATURE_PAD
static fr_err_t fr_native_pad_reset(fr_runtime_t *runtime,
                                    const fr_tagged_t *args,
                                    uint8_t arg_count, fr_tagged_t *out) {
  if (args == NULL && arg_count > 0) {
    return FR_ERR_INVALID;
  }
  if (arg_count != 0) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_pad_reset(runtime));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_pad_emit_byte(fr_runtime_t *runtime,
                                        const fr_tagged_t *args,
                                        uint8_t arg_count,
                                        fr_tagged_t *out) {
  uint8_t byte = 0;

  FR_TRY(fr_native_decode_byte(args, arg_count, 0, &byte));
  FR_TRY(fr_pad_emit_byte(runtime, byte));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_pad_len(fr_runtime_t *runtime,
                                  const fr_tagged_t *args, uint8_t arg_count,
                                  fr_tagged_t *out) {
  uint16_t length = 0;

  if (args == NULL && arg_count > 0) {
    return FR_ERR_INVALID;
  }
  if (arg_count != 0) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_pad_length(runtime, &length));
  return fr_tagged_encode_int((int32_t)length, out);
}

static fr_err_t fr_native_pad_type(fr_runtime_t *runtime,
                                   const fr_tagged_t *args, uint8_t arg_count,
                                   fr_tagged_t *out) {
  const uint8_t *bytes = NULL;
  uint16_t length = 0;

  if (args == NULL && arg_count > 0) {
    return FR_ERR_INVALID;
  }
  if (arg_count != 0) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_pad_view(runtime, &bytes, &length));
  FR_TRY(fr_platform_write_bytes(bytes, length));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_pad_peek_byte(fr_runtime_t *runtime,
                                        const fr_tagged_t *args,
                                        uint8_t arg_count,
                                        fr_tagged_t *out) {
  const uint8_t *bytes = NULL;
  uint16_t length = 0;
  uint16_t index = 0;

  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &index));
  FR_TRY(fr_pad_view(runtime, &bytes, &length));
  if (index >= length) {
    return FR_ERR_RANGE;
  }
  return fr_tagged_encode_int((int32_t)bytes[index], out);
}

#if FR_FEATURE_TEXT
static fr_err_t fr_native_pad_pack(fr_runtime_t *runtime,
                                   const fr_tagged_t *args,
                                   uint8_t arg_count, fr_tagged_t *out) {
  const uint8_t *bytes = NULL;
  uint16_t length = 0;
  fr_object_id_t object_id = 0;

  if (args == NULL && arg_count > 0) {
    return FR_ERR_INVALID;
  }
  if (arg_count != 0) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_pad_view(runtime, &bytes, &length));
  FR_TRY(fr_text_install(runtime, bytes, length, &object_id));
  return fr_tagged_encode_object_id(object_id, out);
}
#endif
#endif

#if FR_FEATURE_NATIVE_SIGNATURES
static const fr_native_value_kind_t fr_native_int_args[] = {
    FR_NATIVE_VALUE_INT,
};

static const fr_native_value_kind_t fr_native_two_int_args[] = {
    FR_NATIVE_VALUE_INT,
    FR_NATIVE_VALUE_INT,
};

#if FR_FEATURE_UART
static const fr_native_value_kind_t fr_native_three_int_args[] = {
    FR_NATIVE_VALUE_INT,
    FR_NATIVE_VALUE_INT,
    FR_NATIVE_VALUE_INT,
};

static const fr_native_value_kind_t fr_native_handle_args[] = {
    FR_NATIVE_VALUE_HANDLE,
};

static const fr_native_value_kind_t fr_native_handle_int_args[] = {
    FR_NATIVE_VALUE_HANDLE,
    FR_NATIVE_VALUE_INT,
};
#endif

static const fr_native_signature_t fr_native_nil_to_int_signature = {
    .args = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_INT,
};

#if FR_FEATURE_PAD
static const fr_native_signature_t fr_native_nil_to_nil_signature = {
    .args = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_NIL,
};

#if FR_FEATURE_TEXT
static const fr_native_signature_t fr_native_nil_to_text_signature = {
    .args = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_TEXT,
};
#endif
#endif

static const fr_native_signature_t fr_native_int_to_nil_signature = {
    .args = fr_native_int_args,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
};

static const fr_native_signature_t fr_native_int_to_int_signature = {
    .args = fr_native_int_args,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_INT,
};

static const fr_native_signature_t fr_native_gpio_write_to_nil_signature = {
    .args = fr_native_two_int_args,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_NIL,
};

static const fr_native_signature_t fr_native_int_int_to_any_signature = {
    .args = fr_native_two_int_args,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_ANY,
};

#if FR_FEATURE_UART
static const fr_native_signature_t fr_native_uart_open_signature = {
    .args = fr_native_three_int_args,
    .arg_count = 3,
    .result = FR_NATIVE_VALUE_HANDLE,
};

static const fr_native_signature_t fr_native_uart_write_byte_signature = {
    .args = fr_native_handle_int_args,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_NIL,
};

static const fr_native_signature_t fr_native_handle_to_int_signature = {
    .args = fr_native_handle_args,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_INT,
};

static const fr_native_signature_t fr_native_handle_to_nil_signature = {
    .args = fr_native_handle_args,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
};
#endif
#endif

#if FR_FEATURE_UART
enum {
  FR_TARGET_TAGGED_BAUD_9600 = FR_TAGGED_INT_LITERAL(FR_UART_RATE_9600),
  FR_TARGET_TAGGED_BAUD_19200 = FR_TAGGED_INT_LITERAL(FR_UART_RATE_19200),
  FR_TARGET_TAGGED_BAUD_38400 = FR_TAGGED_INT_LITERAL(FR_UART_RATE_38400),
  FR_TARGET_TAGGED_BAUD_57600 = FR_TAGGED_INT_LITERAL(FR_UART_RATE_57600),
  FR_TARGET_TAGGED_BAUD_115200 = FR_TAGGED_INT_LITERAL(FR_UART_RATE_115200),
};
#endif

const fr_base_def_t fr_target_base_defs[] = {
    {
        .slot_id = FR_SLOT_MS,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ms",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ms,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_int_to_nil_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_GPIO_WRITE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "gpio.write",
        .alias = "pin",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_gpio_write,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_gpio_write_to_nil_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_GPIO_MODE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "gpio.mode",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_gpio_mode,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_gpio_write_to_nil_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_GPIO_READ,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "gpio.read",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_gpio_read,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_int_to_int_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_ADC_READ,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "adc.read",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_adc_read,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_int_to_int_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_ADC_ABOVE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "adc.above",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_adc_above,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_int_int_to_any_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_MILLIS,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "millis",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_millis,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_nil_to_int_signature,
#endif
    },
#if FR_FEATURE_UART
    {
        .slot_id = FR_SLOT_UART_OPEN,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "uart.open",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_uart_open,
        .native_arity = 3,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_uart_open_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_UART_WRITE_BYTE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "uart.write-byte",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_uart_write_byte,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_uart_write_byte_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_UART_READ_BYTE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "uart.read-byte",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_uart_read_byte,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_handle_to_int_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_UART_AVAILABLE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "uart.available",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_uart_available,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_handle_to_int_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_UART_CLOSE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "uart.close",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_uart_close,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_handle_to_nil_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BAUD_9600,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$baud_9600",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BAUD_9600,
    },
    {
        .slot_id = FR_SLOT_BAUD_19200,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$baud_19200",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BAUD_19200,
    },
    {
        .slot_id = FR_SLOT_BAUD_38400,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$baud_38400",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BAUD_38400,
    },
    {
        .slot_id = FR_SLOT_BAUD_57600,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$baud_57600",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BAUD_57600,
    },
    {
        .slot_id = FR_SLOT_BAUD_115200,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$baud_115200",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BAUD_115200,
    },
#endif
#if FR_FEATURE_PAD
    {
        .slot_id = FR_SLOT_PAD_RESET,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pad.reset",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pad_reset,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_nil_to_nil_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_PAD_EMIT_BYTE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pad.emit-byte",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pad_emit_byte,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_int_to_nil_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_PAD_LEN,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pad.len",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pad_len,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_nil_to_int_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_PAD_TYPE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pad.type",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pad_type,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_nil_to_nil_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_PAD_PEEK_BYTE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pad.peek-byte",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pad_peek_byte,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_int_to_int_signature,
#endif
    },
#if FR_FEATURE_TEXT
    {
        .slot_id = FR_SLOT_PAD_PACK,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pad.pack",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pad_pack,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_nil_to_text_signature,
#endif
    },
#endif
#endif
};

const uint16_t fr_target_base_def_count =
    (uint16_t)(sizeof(fr_target_base_defs) / sizeof(fr_target_base_defs[0]));
