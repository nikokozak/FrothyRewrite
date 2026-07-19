#include "base_defs.h"

#include "code.h"
#include "event.h"
#include "handle.h"
#include "object.h"
#include "pad.h"
#include "platform.h"
#include "runtime.h"
#include "tagged.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if FR_FEATURE_CONSOLE_ROUTING || FR_FEATURE_BLE
static fr_err_t fr_native_write_rendered_line(const char *line, size_t cap,
                                              int written);
#endif

#if FR_FEATURE_TRACE || FR_FEATURE_PULSE
typedef char fr_signal_nanoseconds_must_fit_tagged_int[
    ((uint64_t)FR_SIGNAL_MAX_TICKS * FR_SIGNAL_TICK_NS <=
     FR_TAGGED_INT_MAX)
        ? 1
        : -1];
#endif

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
  uint32_t ms = 0;
  uint32_t shown_ms = 0;

  (void)runtime;
  if (args == NULL && arg_count > 0) {
    return FR_ERR_INVALID;
  }
  if (arg_count != 0) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_platform_millis(&ms));
  /* The tagged-int band is the user-visible clock ceiling: millis wraps after
   * FR_TAGGED_INT_MAX + 1 ms, about 12.4 days on the 32-bit runtime. */
  shown_ms = ms % ((uint32_t)FR_TAGGED_INT_MAX + 1u);
  return fr_tagged_encode_int((int32_t)shown_ms, out);
}

static fr_err_t fr_native_micros(fr_runtime_t *runtime, const fr_tagged_t *args,
                                 uint8_t arg_count, fr_tagged_t *out) {
  uint32_t us = 0;
  uint32_t shown_us = 0;

  (void)runtime;
  if (args == NULL && arg_count > 0) {
    return FR_ERR_INVALID;
  }
  if (arg_count != 0) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_platform_micros(&us));
  /* The tagged-int band is the user-visible clock ceiling: micros wraps after
   * about 17.9 minutes on the 32-bit runtime. A modular delta across that wrap
   * is still correct for short spans. */
  shown_us = us % ((uint32_t)FR_TAGGED_INT_MAX + 1u);
  return fr_tagged_encode_int((int32_t)shown_us, out);
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
  uint16_t port = 0;
  fr_int_t baud = 0;
  fr_handle_ref_t ref = {0};
  fr_tagged_t handle = 0;
  uint16_t platform_index = 0;
  fr_err_t err = FR_OK;

  if (runtime == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &port));
  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, 1, &baud));

  FR_TRY(fr_handle_reserve(runtime, FR_HANDLE_KIND_UART, &ref, &handle));
  err = fr_platform_uart_open(port, (uint32_t)baud, &platform_index);
  if (err != FR_OK) {
    (void)fr_handle_release_reserved(runtime, ref);
    if (err == FR_ERR_BUSY) {
      fr_native_diag_note_actual(runtime, args[0], FR_DIAG_ACTUAL_VALUE);
    }
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

/* uart.open-on: caller picks tx/rx pins instead of letting the platform use
 * its defaults. Pin conflicts are rejected by the platform with
 * FR_ERR_DOMAIN; what counts as a conflict is platform-specific (esp-idf
 * also guards the console UART; host only guards already-open custom pins). */
static fr_err_t fr_native_uart_open_on(fr_runtime_t *runtime,
                                       const fr_tagged_t *args,
                                       uint8_t arg_count, fr_tagged_t *out) {
  uint16_t port = 0;
  uint16_t tx = 0;
  uint16_t rx = 0;
  fr_int_t baud = 0;
  fr_handle_ref_t ref = {0};
  fr_tagged_t handle = 0;
  uint16_t platform_index = 0;
  fr_err_t err = FR_OK;

  if (runtime == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &port));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &tx));
  FR_TRY(fr_native_decode_u16(args, arg_count, 2, &rx));
  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, 3, &baud));

  FR_TRY(fr_handle_reserve(runtime, FR_HANDLE_KIND_UART, &ref, &handle));
  err = fr_platform_uart_open_on(port, tx, rx, (uint32_t)baud,
                                 &platform_index);
  if (err != FR_OK) {
    (void)fr_handle_release_reserved(runtime, ref);
    if (err == FR_ERR_BUSY) {
      fr_native_diag_note_actual(runtime, args[0], FR_DIAG_ACTUAL_VALUE);
    }
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

#if FR_FEATURE_PWM
enum {
  FR_NATIVE_PWM_DUTY_MAX = 10000,
};

static fr_err_t fr_native_decode_pwm_handle(fr_runtime_t *runtime,
                                            const fr_tagged_t *args,
                                            uint8_t arg_count, uint8_t index,
                                            uint16_t *out_platform_index) {
  fr_handle_ref_t ref = {0};

  if (runtime == NULL || args == NULL || index >= arg_count ||
      out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_handle_ref(args[index], &ref));
  return fr_handle_lookup(runtime, ref, FR_HANDLE_KIND_PWM, NULL,
                          out_platform_index);
}

static fr_err_t fr_native_pwm_open(fr_runtime_t *runtime,
                                   const fr_tagged_t *args, uint8_t arg_count,
                                   fr_tagged_t *out) {
  uint16_t pin = 0;
  uint16_t freq = 0;
  fr_handle_ref_t ref = {0};
  fr_tagged_t handle = 0;
  uint16_t platform_index = 0;
  fr_err_t err = FR_OK;

  if (runtime == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &pin));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &freq));

  FR_TRY(fr_handle_reserve(runtime, FR_HANDLE_KIND_PWM, &ref, &handle));
  err = fr_platform_pwm_open(pin, freq, &platform_index);
  if (err != FR_OK) {
    (void)fr_handle_release_reserved(runtime, ref);
    return err;
  }

  err = fr_handle_activate(runtime, ref, platform_index);
  if (err != FR_OK) {
    (void)fr_platform_handle_close(FR_HANDLE_KIND_PWM, platform_index);
    (void)fr_handle_release_reserved(runtime, ref);
    return err;
  }

  *out = handle;
  return FR_OK;
}

static fr_err_t fr_native_pwm_write(fr_runtime_t *runtime,
                                    const fr_tagged_t *args, uint8_t arg_count,
                                    fr_tagged_t *out) {
  uint16_t platform_index = 0;
  fr_int_t duty = 0;

  if (out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_pwm_handle(runtime, args, arg_count, 0,
                                     &platform_index));
  FR_TRY(fr_tagged_decode_int(args[1], &duty));
  if (duty < 0 || duty > FR_NATIVE_PWM_DUTY_MAX) {
    return FR_ERR_DOMAIN;
  }
  FR_TRY(fr_platform_pwm_write(platform_index, (uint16_t)duty));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_pwm_close(fr_runtime_t *runtime,
                                    const fr_tagged_t *args, uint8_t arg_count,
                                    fr_tagged_t *out) {
  fr_handle_ref_t ref = {0};

  if (runtime == NULL || args == NULL || arg_count == 0 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_handle_ref(args[0], &ref));
  FR_TRY(fr_handle_lookup(runtime, ref, FR_HANDLE_KIND_PWM, NULL, NULL));
  FR_TRY(fr_handle_close(runtime, ref));
  *out = fr_tagged_nil();
  return FR_OK;
}
#endif

#if FR_FEATURE_TEXT || FR_FEATURE_I2C || FR_FEATURE_NET
static fr_err_t fr_native_decode_text_or_bytes_view(
    const fr_runtime_t *runtime, fr_tagged_t tagged, const uint8_t **out_bytes,
    uint16_t *out_length) {
  fr_object_id_t object_id = 0;

  if (fr_tagged_decode_object_id(tagged, &object_id) == FR_OK) {
    return fr_text_view(runtime, object_id, out_bytes, out_length);
  }
  {
    fr_bytes_ref_t ref = {0};
    FR_TRY(fr_tagged_decode_bytes_ref(tagged, &ref));
    return fr_bytes_view(runtime, ref, out_bytes, out_length);
  }
}
#endif

#if FR_FEATURE_I2C
enum {
  FR_NATIVE_I2C_ADDR_MAX = 0x7F,
};

static fr_err_t fr_native_decode_i2c_handle(fr_runtime_t *runtime,
                                            const fr_tagged_t *args,
                                            uint8_t arg_count, uint8_t index,
                                            uint16_t *out_platform_index) {
  fr_handle_ref_t ref = {0};

  if (runtime == NULL || args == NULL || index >= arg_count ||
      out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_handle_ref(args[index], &ref));
  return fr_handle_lookup(runtime, ref, FR_HANDLE_KIND_I2C_BUS, NULL,
                          out_platform_index);
}

static fr_err_t fr_native_decode_i2c_addr(const fr_tagged_t *args,
                                          uint8_t arg_count, uint8_t index,
                                          uint8_t *out_addr) {
  fr_int_t addr = 0;

  if (args == NULL || index >= arg_count || out_addr == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_int(args[index], &addr));
  if (addr < 0 || addr > FR_NATIVE_I2C_ADDR_MAX) {
    return FR_ERR_DOMAIN;
  }
  *out_addr = (uint8_t)addr;
  return FR_OK;
}

static fr_err_t fr_native_i2c_open(fr_runtime_t *runtime,
                                   const fr_tagged_t *args, uint8_t arg_count,
                                   fr_tagged_t *out) {
  uint16_t port = 0;
  uint16_t sda = 0;
  uint16_t scl = 0;
  fr_int_t freq = 0;
  fr_handle_ref_t ref = {0};
  fr_tagged_t handle = 0;
  uint16_t platform_index = 0;
  fr_err_t err = FR_OK;

  if (runtime == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &port));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &sda));
  FR_TRY(fr_native_decode_u16(args, arg_count, 2, &scl));
  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, 3, &freq));
  if (freq == 0) {
    return FR_ERR_DOMAIN;
  }

  FR_TRY(fr_handle_reserve(runtime, FR_HANDLE_KIND_I2C_BUS, &ref, &handle));
  err = fr_platform_i2c_open(port, sda, scl, (uint32_t)freq, &platform_index);
  if (err != FR_OK) {
    (void)fr_handle_release_reserved(runtime, ref);
    return err;
  }

  err = fr_handle_activate(runtime, ref, platform_index);
  if (err != FR_OK) {
    (void)fr_platform_handle_close(FR_HANDLE_KIND_I2C_BUS, platform_index);
    (void)fr_handle_release_reserved(runtime, ref);
    return err;
  }

  *out = handle;
  return FR_OK;
}

static fr_err_t fr_native_i2c_write(fr_runtime_t *runtime,
                                    const fr_tagged_t *args, uint8_t arg_count,
                                    fr_tagged_t *out) {
  uint16_t platform_index = 0;
  uint8_t addr = 0;
  const uint8_t *bytes = NULL;
  uint16_t length = 0;

  if (runtime == NULL || args == NULL || arg_count != 3 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_i2c_handle(runtime, args, arg_count, 0,
                                     &platform_index));
  FR_TRY(fr_native_decode_i2c_addr(args, arg_count, 1, &addr));
  FR_TRY(fr_native_decode_text_or_bytes_view(runtime, args[2], &bytes,
                                             &length));
  if (length == 0) {
    return FR_ERR_DOMAIN;
  }
  FR_TRY(fr_platform_i2c_write(platform_index, addr, bytes, length));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_i2c_read(fr_runtime_t *runtime,
                                   const fr_tagged_t *args, uint8_t arg_count,
                                   fr_tagged_t *out) {
  uint16_t platform_index = 0;
  uint8_t addr = 0;
  fr_int_t count = 0;
  uint8_t buffer[FR_PROFILE_MAX_TEXT_LENGTH];

  if (runtime == NULL || args == NULL || arg_count != 3 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_i2c_handle(runtime, args, arg_count, 0,
                                     &platform_index));
  FR_TRY(fr_native_decode_i2c_addr(args, arg_count, 1, &addr));
  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, 2, &count));
  if (count > FR_PROFILE_MAX_TEXT_LENGTH) {
    return FR_ERR_RANGE;
  }
  if (count > 0) {
    FR_TRY(fr_platform_i2c_read(platform_index, addr, buffer, (uint16_t)count));
  }
  return fr_bytes_install(runtime, buffer, (uint16_t)count, out);
}

static fr_err_t fr_native_i2c_close(fr_runtime_t *runtime,
                                    const fr_tagged_t *args, uint8_t arg_count,
                                    fr_tagged_t *out) {
  fr_handle_ref_t ref = {0};

  if (runtime == NULL || args == NULL || arg_count == 0 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_handle_ref(args[0], &ref));
  FR_TRY(fr_handle_lookup(runtime, ref, FR_HANDLE_KIND_I2C_BUS, NULL, NULL));
  FR_TRY(fr_handle_close(runtime, ref));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_i2c_read_reg(fr_runtime_t *runtime,
                                       const fr_tagged_t *args,
                                       uint8_t arg_count, fr_tagged_t *out) {
  uint16_t platform_index = 0;
  uint8_t addr = 0;
  uint16_t reg = 0;
  uint8_t wbytes[1];
  uint8_t rbytes[1];

  if (runtime == NULL || args == NULL || arg_count != 3 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_i2c_handle(runtime, args, arg_count, 0,
                                     &platform_index));
  FR_TRY(fr_native_decode_i2c_addr(args, arg_count, 1, &addr));
  FR_TRY(fr_native_decode_u16(args, arg_count, 2, &reg));
  if (reg > 0xFF) {
    return FR_ERR_DOMAIN;
  }
  wbytes[0] = (uint8_t)reg;
  FR_TRY(fr_platform_i2c_write_read(platform_index, addr, wbytes, 1, rbytes,
                                    1));
  return fr_tagged_encode_int((int32_t)rbytes[0], out);
}

static fr_err_t fr_native_i2c_read_reg16(fr_runtime_t *runtime,
                                         const fr_tagged_t *args,
                                         uint8_t arg_count, fr_tagged_t *out) {
  uint16_t platform_index = 0;
  uint8_t addr = 0;
  uint16_t reg = 0;
  uint8_t wbytes[1];
  uint8_t rbytes[2];

  if (runtime == NULL || args == NULL || arg_count != 3 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_i2c_handle(runtime, args, arg_count, 0,
                                     &platform_index));
  FR_TRY(fr_native_decode_i2c_addr(args, arg_count, 1, &addr));
  FR_TRY(fr_native_decode_u16(args, arg_count, 2, &reg));
  if (reg > 0xFF) {
    return FR_ERR_DOMAIN;
  }
  wbytes[0] = (uint8_t)reg;
  FR_TRY(fr_platform_i2c_write_read(platform_index, addr, wbytes, 1, rbytes,
                                    2));
  /* D3 — 16-bit register reads clock MSB first on the wire. */
  return fr_tagged_encode_int(((int32_t)rbytes[0] << 8) | (int32_t)rbytes[1],
                              out);
}

static fr_err_t fr_native_i2c_write_reg(fr_runtime_t *runtime,
                                        const fr_tagged_t *args,
                                        uint8_t arg_count, fr_tagged_t *out) {
  uint16_t platform_index = 0;
  uint8_t addr = 0;
  uint16_t reg = 0;
  uint16_t value = 0;
  uint8_t wbytes[2];

  if (runtime == NULL || args == NULL || arg_count != 4 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_i2c_handle(runtime, args, arg_count, 0,
                                     &platform_index));
  FR_TRY(fr_native_decode_i2c_addr(args, arg_count, 1, &addr));
  FR_TRY(fr_native_decode_u16(args, arg_count, 2, &reg));
  FR_TRY(fr_native_decode_u16(args, arg_count, 3, &value));
  if (reg > 0xFF || value > 0xFF) {
    return FR_ERR_DOMAIN;
  }
  wbytes[0] = (uint8_t)reg;
  wbytes[1] = (uint8_t)value;
  FR_TRY(fr_platform_i2c_write_read(platform_index, addr, wbytes, 2, NULL, 0));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_i2c_write_reg16(fr_runtime_t *runtime,
                                          const fr_tagged_t *args,
                                          uint8_t arg_count,
                                          fr_tagged_t *out) {
  uint16_t platform_index = 0;
  uint8_t addr = 0;
  uint16_t reg = 0;
  uint16_t value = 0;
  uint8_t wbytes[3];

  if (runtime == NULL || args == NULL || arg_count != 4 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_i2c_handle(runtime, args, arg_count, 0,
                                     &platform_index));
  FR_TRY(fr_native_decode_i2c_addr(args, arg_count, 1, &addr));
  FR_TRY(fr_native_decode_u16(args, arg_count, 2, &reg));
  FR_TRY(fr_native_decode_u16(args, arg_count, 3, &value));
  if (reg > 0xFF) {
    return FR_ERR_DOMAIN;
  }
  wbytes[0] = (uint8_t)reg;
  /* D3 — 16-bit register writes clock MSB first on the wire. */
  wbytes[1] = (uint8_t)((value >> 8) & 0xFF);
  wbytes[2] = (uint8_t)(value & 0xFF);
  FR_TRY(fr_platform_i2c_write_read(platform_index, addr, wbytes, 3, NULL, 0));
  *out = fr_tagged_nil();
  return FR_OK;
}
#endif

#if FR_FEATURE_NET
enum {
  FR_NATIVE_WIFI_SSID_MAX = 32,
  FR_NATIVE_WIFI_PASS_MAX = 64,
  /* RFC 1035 5.1 caps a DNS name at 253 octets; round up to leave room
   * for a future dotted-quad without changing the buffer. */
  FR_NATIVE_TCP_HOST_MAX = 255,
};

static fr_err_t fr_native_decode_text_view(const fr_runtime_t *runtime,
                                           const fr_tagged_t *args,
                                           uint8_t arg_count, uint8_t index,
                                           const uint8_t **out_bytes,
                                           uint16_t *out_length) {
  fr_object_id_t object_id = 0;

  if (args == NULL || index >= arg_count || out_bytes == NULL ||
      out_length == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_object_id(args[index], &object_id));
  return fr_text_view(runtime, object_id, out_bytes, out_length);
}

/* cap counts the NUL slot; the caller's text must fit in cap - 1 bytes. */
static fr_err_t fr_native_copy_text_cstring(const fr_runtime_t *runtime,
                                            const fr_tagged_t *args,
                                            uint8_t arg_count, uint8_t index,
                                            char *out_buf, uint16_t cap) {
  const uint8_t *bytes = NULL;
  uint16_t length = 0;

  if (out_buf == NULL || cap == 0) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_text_view(runtime, args, arg_count, index, &bytes,
                                    &length));
  if ((uint32_t)length + 1u > cap) {
    return FR_ERR_DOMAIN;
  }
  if (length > 0) {
    memcpy(out_buf, bytes, length);
  }
  out_buf[length] = '\0';
  return FR_OK;
}

static fr_err_t fr_native_wifi_save(fr_runtime_t *runtime,
                                    const fr_tagged_t *args, uint8_t arg_count,
                                    fr_tagged_t *out) {
  char ssid[FR_NATIVE_WIFI_SSID_MAX + 1];
  char pass[FR_NATIVE_WIFI_PASS_MAX + 1];

  if (runtime == NULL || args == NULL || arg_count != 2 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_copy_text_cstring(runtime, args, arg_count, 0, ssid,
                                     sizeof ssid));
  FR_TRY(fr_native_copy_text_cstring(runtime, args, arg_count, 1, pass,
                                     sizeof pass));
  FR_TRY(fr_platform_wifi_save(ssid, pass));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_wifi_connect(fr_runtime_t *runtime,
                                       const fr_tagged_t *args,
                                       uint8_t arg_count, fr_tagged_t *out) {
  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_platform_wifi_connect(runtime));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_wifi_ready_p(fr_runtime_t *runtime,
                                       const fr_tagged_t *args,
                                       uint8_t arg_count, fr_tagged_t *out) {
  bool ready = false;

  (void)runtime;
  (void)args;
  if (arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_platform_wifi_ready(&ready));
  return fr_tagged_encode_bool(ready, out);
}

static fr_err_t fr_native_http_get(fr_runtime_t *runtime,
                                   const fr_tagged_t *args, uint8_t arg_count,
                                   fr_tagged_t *out) {
  char url[FR_PROFILE_MAX_TEXT_LENGTH + 1];
  uint8_t body[FR_HTTP_MAX_BODY];
  uint16_t length = 0;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_copy_text_cstring(runtime, args, arg_count, 0, url,
                                     sizeof url));
  FR_TRY(fr_platform_http_get(url, body, FR_HTTP_MAX_BODY, &length));
  return fr_bytes_install(runtime, body, length, out);
}

static fr_err_t fr_native_decode_tcp_handle(fr_runtime_t *runtime,
                                            const fr_tagged_t *args,
                                            uint8_t arg_count, uint8_t index,
                                            uint16_t *out_platform_index) {
  fr_handle_ref_t ref = {0};

  if (runtime == NULL || args == NULL || index >= arg_count ||
      out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_handle_ref(args[index], &ref));
  return fr_handle_lookup(runtime, ref, FR_HANDLE_KIND_TCP, NULL,
                          out_platform_index);
}

static fr_err_t fr_native_tcp_open(fr_runtime_t *runtime,
                                   const fr_tagged_t *args, uint8_t arg_count,
                                   fr_tagged_t *out) {
  char host[FR_NATIVE_TCP_HOST_MAX + 1];
  fr_int_t port_in = 0;
  fr_handle_ref_t ref = {0};
  fr_tagged_t handle = 0;
  uint16_t platform_index = 0;
  fr_err_t err = FR_OK;

  if (runtime == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_copy_text_cstring(runtime, args, arg_count, 0, host,
                                     sizeof host));
  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, 1, &port_in));
  if (host[0] == '\0' || port_in == 0 || port_in > 0xFFFF) {
    return FR_ERR_DOMAIN;
  }

  FR_TRY(fr_handle_reserve(runtime, FR_HANDLE_KIND_TCP, &ref, &handle));
  err = fr_platform_tcp_open(runtime, host, (uint16_t)port_in, &platform_index);
  if (err != FR_OK) {
    (void)fr_handle_release_reserved(runtime, ref);
    return err;
  }

  err = fr_handle_activate(runtime, ref, platform_index);
  if (err != FR_OK) {
    (void)fr_platform_tcp_close(platform_index);
    (void)fr_handle_release_reserved(runtime, ref);
    return err;
  }

  *out = handle;
  return FR_OK;
}

static fr_err_t fr_native_tcp_read(fr_runtime_t *runtime,
                                   const fr_tagged_t *args, uint8_t arg_count,
                                   fr_tagged_t *out) {
  uint16_t platform_index = 0;
  fr_int_t count = 0;
  uint8_t buffer[FR_PROFILE_MAX_TEXT_LENGTH];
  uint16_t length = 0;

  if (runtime == NULL || args == NULL || arg_count != 2 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_tcp_handle(runtime, args, arg_count, 0,
                                     &platform_index));
  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, 1, &count));
  if (count == 0) {
    return FR_ERR_INVALID;
  }
  if (count > FR_PROFILE_MAX_TEXT_LENGTH) {
    return FR_ERR_DOMAIN;
  }
  FR_TRY(fr_platform_tcp_read(runtime, platform_index, buffer, (uint16_t)count,
                              &length));
  return fr_bytes_install(runtime, buffer, length, out);
}

static fr_err_t fr_native_tcp_write(fr_runtime_t *runtime,
                                    const fr_tagged_t *args, uint8_t arg_count,
                                    fr_tagged_t *out) {
  uint16_t platform_index = 0;
  const uint8_t *bytes = NULL;
  uint16_t length = 0;

  if (runtime == NULL || args == NULL || arg_count != 2 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_tcp_handle(runtime, args, arg_count, 0,
                                     &platform_index));
  FR_TRY(fr_native_decode_text_or_bytes_view(runtime, args[1], &bytes,
                                             &length));
  FR_TRY(fr_platform_tcp_write(runtime, platform_index, bytes, length));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_tcp_close(fr_runtime_t *runtime,
                                    const fr_tagged_t *args, uint8_t arg_count,
                                    fr_tagged_t *out) {
  fr_handle_ref_t ref = {0};

  if (runtime == NULL || args == NULL || arg_count == 0 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_handle_ref(args[0], &ref));
  FR_TRY(fr_handle_lookup(runtime, ref, FR_HANDLE_KIND_TCP, NULL, NULL));
  FR_TRY(fr_handle_close(runtime, ref));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_tcp_bytes_ready_p(fr_runtime_t *runtime,
                                            const fr_tagged_t *args,
                                            uint8_t arg_count,
                                            fr_tagged_t *out) {
  uint16_t platform_index = 0;
  uint16_t count = 0;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_tcp_handle(runtime, args, arg_count, 0,
                                     &platform_index));
  FR_TRY(fr_platform_tcp_bytes_ready(runtime, platform_index, &count));
  return fr_tagged_encode_int((fr_int_t)count, out);
}

#endif

#if FR_FEATURE_POWER
enum {
  /* D9: clamp matches ESP-IDF's own Task WDT Kconfig "range 1 60" (s),
   * the canonical "what makes sense" span. The runtime API would accept
   * any uint32_t. */
  FR_NATIVE_WATCHDOG_TIMEOUT_MIN_MS = 1000,
  FR_NATIVE_WATCHDOG_TIMEOUT_MAX_MS = 60000,
};

/* D11/D17: the kernel owns the user-visible armed state. The platform
 * carries no duplicate flag. Re-arm replaces by reconfiguring the WDT
 * (D10), and the platform's first-subscribe gate is platform-side. */
static bool fr_native_watchdog_armed;

static fr_err_t fr_native_watchdog_arm(fr_runtime_t *runtime,
                                       const fr_tagged_t *args,
                                       uint8_t arg_count, fr_tagged_t *out) {
  fr_int_t timeout = 0;

  (void)runtime;
  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, 0, &timeout));
  if (timeout < FR_NATIVE_WATCHDOG_TIMEOUT_MIN_MS ||
      timeout > FR_NATIVE_WATCHDOG_TIMEOUT_MAX_MS) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_watchdog_arm((uint32_t)timeout));
  fr_native_watchdog_armed = true;
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_watchdog_feed(fr_runtime_t *runtime,
                                        const fr_tagged_t *args,
                                        uint8_t arg_count, fr_tagged_t *out) {
  (void)runtime;
  (void)args;
  if (arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_native_watchdog_armed) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_watchdog_feed());
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_sleep_deep(fr_runtime_t *runtime,
                                     const fr_tagged_t *args,
                                     uint8_t arg_count, fr_tagged_t *out) {
  fr_int_t ms = 0;

  (void)runtime;
  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, 0, &ms));
  FR_TRY(fr_platform_sleep_deep((uint32_t)ms));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_sleep_wake_on_gpio(fr_runtime_t *runtime,
                                             const fr_tagged_t *args,
                                             uint8_t arg_count,
                                             fr_tagged_t *out) {
  uint16_t pin = 0;
  uint16_t level = 0;

  (void)runtime;
  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &pin));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &level));
  FR_TRY(fr_platform_sleep_wake_on_gpio(pin, level));
  *out = fr_tagged_nil();
  return FR_OK;
}

#ifdef FR_HOST_TEST_HELPERS
/* D17/D19: simulates the WDT fire by wiping the kernel armed flag.
 * On a real device a missed feed panics and cold-boots the chip;
 * armed lives in RAM and is gone post-boot. The host fixture mirrors
 * that — after force_timeout the next watchdog.feed: surfaces
 * FR_ERR_INVALID (D11) since armed is back to its post-boot value.
 * Lives here because the armed flag is file-static to this TU. */
void fr_host_watchdog_force_timeout(void) {
  fr_native_watchdog_armed = false;
}
#endif
#endif

#if FR_FEATURE_BYTES
static fr_err_t fr_native_bytes_from_text(fr_runtime_t *runtime,
                                          const fr_tagged_t *args,
                                          uint8_t arg_count,
                                          fr_tagged_t *out) {
  fr_object_id_t object_id = 0;
  const uint8_t *bytes = NULL;
  uint16_t length = 0;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_tagged_decode_object_id(args[0], &object_id));
  FR_TRY(fr_text_view(runtime, object_id, &bytes, &length));
  return fr_bytes_install(runtime, bytes, length, out);
}

static fr_err_t fr_native_bytes_from_byte(fr_runtime_t *runtime,
                                          const fr_tagged_t *args,
                                          uint8_t arg_count,
                                          fr_tagged_t *out) {
  uint8_t byte = 0;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_byte(args, arg_count, 0, &byte));
  return fr_bytes_install(runtime, &byte, 1, out);
}

static fr_err_t fr_native_bytes_from_int(fr_runtime_t *runtime,
                                         const fr_tagged_t *args,
                                         uint8_t arg_count, fr_tagged_t *out) {
  char buffer[12];
  fr_int_t value = 0;
  int written = 0;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_tagged_decode_int(args[0], &value));
  written = snprintf(buffer, sizeof(buffer), "%ld", (long)value);
  if (written <= 0 || (size_t)written >= sizeof(buffer)) {
    return FR_ERR_RANGE;
  }
  return fr_bytes_install(runtime, (const uint8_t *)buffer, (uint16_t)written,
                          out);
}

static fr_err_t fr_native_bytes_length(fr_runtime_t *runtime,
                                       const fr_tagged_t *args,
                                       uint8_t arg_count, fr_tagged_t *out) {
  fr_bytes_ref_t ref = {0};
  const uint8_t *bytes = NULL;
  uint16_t length = 0;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_tagged_decode_bytes_ref(args[0], &ref));
  FR_TRY(fr_bytes_view(runtime, ref, &bytes, &length));
  return fr_tagged_encode_int((int32_t)length, out);
}

static fr_err_t fr_native_bytes_at(fr_runtime_t *runtime,
                                   const fr_tagged_t *args, uint8_t arg_count,
                                   fr_tagged_t *out) {
  fr_bytes_ref_t ref = {0};
  const uint8_t *bytes = NULL;
  uint16_t length = 0;
  fr_int_t index = 0;

  if (runtime == NULL || args == NULL || arg_count != 2 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_tagged_decode_bytes_ref(args[0], &ref));
  FR_TRY(fr_bytes_view(runtime, ref, &bytes, &length));
  FR_TRY(fr_tagged_decode_int(args[1], &index));
  if (index < 0 || (uint32_t)index >= length) {
    return FR_ERR_RANGE;
  }
  return fr_tagged_encode_int((int32_t)bytes[index], out);
}

static fr_err_t fr_native_bytes_equals_p(fr_runtime_t *runtime,
                                         const fr_tagged_t *args,
                                         uint8_t arg_count, fr_tagged_t *out) {
  fr_bytes_ref_t a_ref = {0};
  fr_bytes_ref_t b_ref = {0};
  const uint8_t *a_bytes = NULL;
  const uint8_t *b_bytes = NULL;
  uint16_t a_length = 0;
  uint16_t b_length = 0;

  if (runtime == NULL || args == NULL || arg_count != 2 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_tagged_decode_bytes_ref(args[0], &a_ref));
  FR_TRY(fr_tagged_decode_bytes_ref(args[1], &b_ref));
  FR_TRY(fr_bytes_view(runtime, a_ref, &a_bytes, &a_length));
  FR_TRY(fr_bytes_view(runtime, b_ref, &b_bytes, &b_length));
  return fr_tagged_encode_bool(
      a_length == b_length && memcmp(a_bytes, b_bytes, a_length) == 0, out);
}

static fr_err_t fr_native_bytes_concat(fr_runtime_t *runtime,
                                       const fr_tagged_t *args,
                                       uint8_t arg_count, fr_tagged_t *out) {
  fr_bytes_ref_t a_ref = {0};
  fr_bytes_ref_t b_ref = {0};
  const uint8_t *a_bytes = NULL;
  const uint8_t *b_bytes = NULL;
  uint16_t a_length = 0;
  uint16_t b_length = 0;
  uint8_t joined[FR_PROFILE_MAX_TEXT_LENGTH];

  if (runtime == NULL || args == NULL || arg_count != 2 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_tagged_decode_bytes_ref(args[0], &a_ref));
  FR_TRY(fr_tagged_decode_bytes_ref(args[1], &b_ref));
  FR_TRY(fr_bytes_view(runtime, a_ref, &a_bytes, &a_length));
  FR_TRY(fr_bytes_view(runtime, b_ref, &b_bytes, &b_length));
  if ((uint32_t)a_length + (uint32_t)b_length > FR_PROFILE_MAX_TEXT_LENGTH) {
    return FR_ERR_RANGE;
  }
  if (a_length > 0) {
    memcpy(joined, a_bytes, a_length);
  }
  if (b_length > 0) {
    memcpy(joined + a_length, b_bytes, b_length);
  }
  return fr_bytes_install(runtime, joined, (uint16_t)(a_length + b_length),
                          out);
}

static fr_err_t fr_native_text_pack(fr_runtime_t *runtime,
                                    const fr_tagged_t *args, uint8_t arg_count,
                                    fr_tagged_t *out) {
  fr_bytes_ref_t ref = {0};
  const uint8_t *bytes = NULL;
  uint16_t length = 0;
  fr_object_id_t object_id = 0;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_tagged_decode_bytes_ref(args[0], &ref));
  FR_TRY(fr_bytes_view(runtime, ref, &bytes, &length));
  FR_TRY(fr_text_install(runtime, bytes, length, &object_id));
  return fr_tagged_encode_object_id(object_id, out);
}
#endif

#if FR_FEATURE_CONSOLE_ROUTING
static fr_err_t fr_native_console_uart(fr_runtime_t *runtime,
                                       const fr_tagged_t *args,
                                       uint8_t arg_count, fr_tagged_t *out) {
  uint16_t tx = 0;
  uint16_t rx = 0;
  fr_int_t baud = 0;

  (void)runtime;
  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &tx));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &rx));
  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, 2, &baud));
  if (baud == 0) {
    return FR_ERR_DOMAIN;
  }
  FR_TRY(fr_platform_console_set_uart(tx, rx, (uint32_t)baud));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_console_default(fr_runtime_t *runtime,
                                          const fr_tagged_t *args,
                                          uint8_t arg_count,
                                          fr_tagged_t *out) {
  (void)runtime;
  (void)args;
  (void)arg_count;
  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_console_restore_default());
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_console_info(fr_runtime_t *runtime,
                                       const fr_tagged_t *args,
                                       uint8_t arg_count, fr_tagged_t *out) {
  fr_console_route_t route = {0};
  char line[80];
  int written = 0;

  (void)runtime;
  (void)args;
  (void)arg_count;
  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_console_get_route(&route));
  switch (route.transport) {
  case FR_CONSOLE_TRANSPORT_HOST:
    written = snprintf(line, sizeof(line), "console host\n");
    break;
  case FR_CONSOLE_TRANSPORT_UART:
    written = snprintf(line, sizeof(line),
                       "console uart tx=%u rx=%u baud=%lu\n",
                       (unsigned)route.tx, (unsigned)route.rx,
                       (unsigned long)route.baud);
    break;
  case FR_CONSOLE_TRANSPORT_USB:
    written = snprintf(line, sizeof(line), "console usb\n");
    break;
  default:
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));
  *out = fr_tagged_nil();
  return FR_OK;
}
#endif

#if FR_FEATURE_BLE
static const char *fr_native_ble_radio_state_name(
    fr_ble_radio_state_t state) {
  switch (state) {
  case FR_BLE_RADIO_OFF:
    return "off";
  case FR_BLE_RADIO_STARTING:
    return "starting";
  case FR_BLE_RADIO_READY:
    return "ready";
  case FR_BLE_RADIO_STOPPING:
    return "stopping";
  case FR_BLE_RADIO_FAILED:
    return "failed";
  default:
    return "unknown";
  }
}

static const char *fr_native_ble_scan_state_name(fr_ble_scan_state_t state) {
  switch (state) {
  case FR_BLE_SCAN_IDLE:
    return "idle";
  case FR_BLE_SCAN_ACTIVE:
    return "active";
  case FR_BLE_SCAN_STOPPING:
    return "stopping";
  default:
    return "unknown";
  }
}

#if FR_BLE_ENABLE_BROADCASTER
static const char *fr_native_ble_advertise_state_name(
    fr_ble_advertise_state_t state) {
  switch (state) {
  case FR_BLE_ADVERTISE_IDLE:
    return "idle";
  case FR_BLE_ADVERTISE_ACTIVE:
    return "active";
  case FR_BLE_ADVERTISE_STOPPING:
    return "stopping";
  default:
    return "unknown";
  }
}
#endif

#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
static const char *fr_native_ble_connection_state_name(
    fr_ble_connection_state_t state) {
  switch (state) {
  case FR_BLE_CONNECTION_FREE:
    return "free";
  case FR_BLE_CONNECTION_CONNECTING:
    return "connecting";
  case FR_BLE_CONNECTION_PENDING:
    return "pending";
  case FR_BLE_CONNECTION_LIVE:
    return "live";
  case FR_BLE_CONNECTION_DISCONNECTED:
    return "disconnected";
  case FR_BLE_CONNECTION_CLOSING:
    return "closing";
  default:
    return "unknown";
  }
}

static const char *fr_native_ble_connection_role_name(
    fr_ble_connection_role_t role) {
  return role == FR_BLE_CONNECTION_ROLE_PERIPHERAL ? "peripheral" : "central";
}
#endif

static const char *fr_native_ble_address_type_name(
    fr_ble_address_type_t type) {
  switch (type) {
  case FR_BLE_ADDRESS_PUBLIC:
    return "public";
  case FR_BLE_ADDRESS_RANDOM:
    return "random";
  case FR_BLE_ADDRESS_PUBLIC_ID:
    return "public-id";
  case FR_BLE_ADDRESS_RANDOM_ID:
    return "random-id";
  default:
    return "unknown";
  }
}

static const char *fr_native_ble_operation_name(fr_ble_operation_t operation) {
  switch (operation) {
  case FR_BLE_OP_NONE:
    return "none";
  case FR_BLE_OP_ON:
    return "on";
  case FR_BLE_OP_SCAN_START:
    return "scan.start";
  case FR_BLE_OP_SCAN_STOP:
    return "scan.stop";
  case FR_BLE_OP_SCAN_NEXT:
    return "scan.next";
  case FR_BLE_OP_ADVERTISE_START:
    return "advertise.start";
  case FR_BLE_OP_ADVERTISE_STOP:
    return "advertise.stop";
  case FR_BLE_OP_OFF:
    return "off";
  case FR_BLE_OP_CONNECT:
    return "connect";
  case FR_BLE_OP_ACCEPT:
    return "accept";
  case FR_BLE_OP_CONNECTION_CLOSE:
    return "connection.close";
  case FR_BLE_OP_CONNECTION_PARAMS:
    return "connection.params";
  case FR_BLE_OP_CONNECTION_MTU:
    return "connection.mtu";
  case FR_BLE_OP_GATT_INSTALL:
    return "gatt.install";
  case FR_BLE_OP_GATT_SET:
    return "gatt.set";
  case FR_BLE_OP_GATT_NOTIFY:
    return "gatt.notify";
  case FR_BLE_OP_GATT_INDICATE:
    return "gatt.indicate";
  case FR_BLE_OP_GATT_WRITE_NEXT:
    return "gatt.next-write";
  case FR_BLE_OP_GATT_FIND:
    return "gatt.find";
  case FR_BLE_OP_GATT_READ:
    return "gatt.read";
  case FR_BLE_OP_GATT_WRITE:
    return "gatt.write";
  case FR_BLE_OP_GATT_SUBSCRIBE:
    return "gatt.subscribe";
  case FR_BLE_OP_GATT_UNSUBSCRIBE:
    return "gatt.unsubscribe";
  case FR_BLE_OP_GATT_NOTIFICATION_NEXT:
    return "gatt.next-notification";
  default:
    return "unknown";
  }
}

static fr_err_t fr_native_ble_roles_text(uint8_t roles, char *out,
                                         size_t cap) {
  static const struct {
    uint8_t bit;
    const char *name;
  } labels[] = {
      {FR_BLE_ROLE_OBSERVER, "observer"},
      {FR_BLE_ROLE_BROADCASTER, "broadcaster"},
      {FR_BLE_ROLE_CENTRAL, "central"},
      {FR_BLE_ROLE_PERIPHERAL, "peripheral"},
  };
  size_t used = 0;

  if (out == NULL || cap == 0) {
    return FR_ERR_INVALID;
  }
  out[0] = '\0';
  for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
    int written = 0;

    if ((roles & labels[i].bit) == 0) {
      continue;
    }
    written = snprintf(out + used, cap - used, "%s%s", used > 0 ? "," : "",
                       labels[i].name);
    if (written < 0) {
      return FR_ERR_IO;
    }
    if ((size_t)written >= cap - used) {
      return FR_ERR_CAPACITY;
    }
    used += (size_t)written;
  }
  if (used == 0) {
    int written = snprintf(out, cap, "none");

    if (written < 0) {
      return FR_ERR_IO;
    }
    if ((size_t)written >= cap) {
      return FR_ERR_CAPACITY;
    }
  }
  return FR_OK;
}

static fr_err_t fr_native_ble_result_text(fr_err_t result, char *out,
                                          size_t cap) {
  const char *name = result == FR_OK ? "ok" : fr_err_name(result);
  int written = 0;

  if (out == NULL || cap == 0) {
    return FR_ERR_INVALID;
  }
  if (name == NULL) {
    name = "unknown";
  }
  written = snprintf(out, cap, "%s", name);
  if (written < 0) {
    return FR_ERR_IO;
  }
  if ((size_t)written >= cap) {
    return FR_ERR_CAPACITY;
  }
  for (int i = 0; i < written; i++) {
    if (out[i] == ' ') {
      out[i] = '-';
    }
  }
  return FR_OK;
}

static fr_err_t fr_native_ble_on(fr_runtime_t *runtime,
                                 const fr_tagged_t *args, uint8_t arg_count,
                                 fr_tagged_t *out) {
  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_on(runtime));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_ble_off(fr_runtime_t *runtime,
                                  const fr_tagged_t *args,
                                  uint8_t arg_count, fr_tagged_t *out) {
  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
  /* Turning the radio off owns the final outcome. Closing a handle can report
   * an in-progress disconnect even though off completes the shutdown. */
  (void)fr_handle_close_kind(runtime, FR_HANDLE_KIND_BLE_CONNECTION);
#endif
  FR_TRY(fr_platform_ble_off(runtime));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_ble_info(fr_runtime_t *runtime,
                                   const fr_tagged_t *args,
                                   uint8_t arg_count, fr_tagged_t *out) {
  fr_ble_status_t status = {0};
  const char *backend = NULL;
  char roles[48];
  char result[32];
  char line[224];
  int written = 0;

  (void)runtime;
  (void)args;
  if (arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_status(&status));
  backend = fr_platform_ble_backend_name();
  if (backend == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_ble_roles_text(status.roles, roles, sizeof(roles)));
  FR_TRY(fr_native_ble_result_text(status.last_result, result,
                                   sizeof(result)));

  written = snprintf(line, sizeof(line),
                     "ble backend=%s state=%s roles=%s coexistence=%s\n",
                     backend, fr_native_ble_radio_state_name(status.radio_state),
                     roles,
                     status.coexistence_enabled ? "enabled" : "disabled");
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));

  if (status.own_address_valid) {
    written = snprintf(
        line, sizeof(line), "ble address=%s:%02X:%02X:%02X:%02X:%02X:%02X\n",
        fr_native_ble_address_type_name(status.own_address_type),
        (unsigned)status.own_address[0], (unsigned)status.own_address[1],
        (unsigned)status.own_address[2], (unsigned)status.own_address[3],
        (unsigned)status.own_address[4], (unsigned)status.own_address[5]);
  } else {
    written = snprintf(line, sizeof(line), "ble address=none\n");
  }
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));

  if (status.requested_interval_ms > 0) {
    written = snprintf(
        line, sizeof(line),
        "ble scan=%s requested=%u/%ums actual=%lu/%luus active=%u repeats=%u min-rssi=%d\n",
        fr_native_ble_scan_state_name(status.scan_state),
        (unsigned)status.requested_interval_ms,
        (unsigned)status.requested_window_ms,
        (unsigned long)status.actual_interval_us,
        (unsigned long)status.actual_window_us,
        status.active_scan ? 1u : 0u, status.repeats ? 1u : 0u,
        (int)status.minimum_rssi);
  } else {
    written = snprintf(line, sizeof(line), "ble scan=%s parameters=none\n",
                       fr_native_ble_scan_state_name(status.scan_state));
  }
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));

#if FR_BLE_ENABLE_BROADCASTER
  if (status.advertise_requested_interval_ms > 0) {
    written = snprintf(
        line, sizeof(line),
        "ble advertise=%s requested=%ums actual=%luus connectable=%u data=%u scan-response=%u starts=%lu stops=%lu\n",
        fr_native_ble_advertise_state_name(status.advertise_state),
        (unsigned)status.advertise_requested_interval_ms,
        (unsigned long)status.advertise_actual_interval_us,
        status.advertise_connectable ? 1u : 0u,
        (unsigned)status.advertising_data_length,
        (unsigned)status.scan_response_data_length,
        (unsigned long)status.advertise_starts,
        (unsigned long)status.advertise_stops);
  } else {
    written = snprintf(
        line, sizeof(line), "ble advertise=%s parameters=none starts=%lu stops=%lu\n",
        fr_native_ble_advertise_state_name(status.advertise_state),
        (unsigned long)status.advertise_starts,
        (unsigned long)status.advertise_stops);
  }
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));
#endif

#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
  written = snprintf(
      line, sizeof(line),
      "ble connections=%u/%u pending=%u notices=%u/%u connects=%lu accepts=%lu disconnects=%lu rejected=%lu\n",
      (unsigned)status.connection_count,
      (unsigned)status.connection_capacity,
      (unsigned)status.pending_connection_count,
      (unsigned)status.connection_notice_count,
      (unsigned)status.connection_notice_capacity,
      (unsigned long)status.connection_connects,
      (unsigned long)status.connection_accepts,
      (unsigned long)status.connection_disconnects,
      (unsigned long)status.incoming_rejected);
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));
#endif

  if (status.current_valid) {
    written = snprintf(
        line, sizeof(line),
        "ble queue=%u/%u high-water=%u received=%lu accepted=%lu filtered-rssi=%lu dequeued=%lu dropped=%lu malformed=%lu current=rssi:%d,flags:%u,data:%u\n",
        (unsigned)status.queue_count, (unsigned)status.queue_capacity,
        (unsigned)status.queue_high_water, (unsigned long)status.received,
        (unsigned long)status.accepted, (unsigned long)status.filtered_rssi,
        (unsigned long)status.dequeued, (unsigned long)status.dropped,
        (unsigned long)status.malformed, (int)status.current_rssi,
        (unsigned)status.current_flags, (unsigned)status.current_data_length);
  } else {
    written = snprintf(
        line, sizeof(line),
        "ble queue=%u/%u high-water=%u received=%lu accepted=%lu filtered-rssi=%lu dequeued=%lu dropped=%lu malformed=%lu current=no\n",
        (unsigned)status.queue_count, (unsigned)status.queue_capacity,
        (unsigned)status.queue_high_water, (unsigned long)status.received,
        (unsigned long)status.accepted, (unsigned long)status.filtered_rssi,
        (unsigned long)status.dequeued, (unsigned long)status.dropped,
        (unsigned long)status.malformed);
  }
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));

  written = snprintf(
      line, sizeof(line),
      "ble lifecycle=%lu shutdown=%u cleanup=%s late-callbacks=%lu resets=%lu\n",
      (unsigned long)status.lifecycle_generation,
      status.shutdown_in_progress ? 1u : 0u,
      status.cleanup_required ? "required" : "clean",
      (unsigned long)status.late_callback_count,
      (unsigned long)status.reset_count);
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));

  written = snprintf(
      line, sizeof(line),
      "ble last-op=%s result=%s platform=%ld reason=%ld at-ms=%lu\n",
      fr_native_ble_operation_name(status.last_operation), result,
      (long)status.last_platform_code, (long)status.last_protocol_reason,
      (unsigned long)status.last_operation_ms);
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));

  *out = fr_tagged_nil();
  return FR_OK;
}

#if FR_BLE_ENABLE_GATT_SERVER || FR_BLE_ENABLE_GATT_CLIENT
static fr_err_t fr_native_ble_connection_handle(
    fr_runtime_t *runtime, fr_tagged_t tagged, fr_handle_ref_t *out_ref,
    uint16_t *out_platform_index);

#if FR_BLE_ENABLE_GATT_SERVER
static const fr_record_name_t fr_native_ble_gatt_kind_field = {
    (const uint8_t *)"kind", 4};
static const fr_record_name_t fr_native_ble_gatt_uuid_field = {
    (const uint8_t *)"uuid", 4};
static const fr_record_name_t fr_native_ble_gatt_flags_field = {
    (const uint8_t *)"flags", 5};
static const fr_record_name_t fr_native_ble_gatt_size_field = {
    (const uint8_t *)"size", 4};
#endif

static int fr_native_ble_gatt_hex(uint8_t byte) {
  if (byte >= '0' && byte <= '9') {
    return byte - '0';
  }
  if (byte >= 'a' && byte <= 'f') {
    return byte - 'a' + 10;
  }
  if (byte >= 'A' && byte <= 'F') {
    return byte - 'A' + 10;
  }
  return -1;
}

static fr_err_t fr_native_ble_gatt_uuid_parse(const uint8_t *text,
                                              uint16_t length,
                                              fr_ble_uuid_t *out_uuid) {
  uint16_t text_index = 0;
  uint8_t byte_index = 0;

  if (text == NULL || out_uuid == NULL) {
    return FR_ERR_INVALID;
  }
  memset(out_uuid, 0, sizeof(*out_uuid));
  if (length == 4) {
    out_uuid->kind = FR_BLE_UUID_16;
    for (uint8_t i = 0; i < 2; i++) {
      int high = fr_native_ble_gatt_hex(text[i * 2u]);
      int low = fr_native_ble_gatt_hex(text[i * 2u + 1u]);

      if (high < 0 || low < 0) {
        return FR_ERR_DOMAIN;
      }
      out_uuid->bytes[i] = (uint8_t)((high << 4) | low);
    }
    return FR_OK;
  }
  if (length != 36) {
    return FR_ERR_DOMAIN;
  }

  out_uuid->kind = FR_BLE_UUID_128;
  while (text_index < length) {
    int high = 0;
    int low = 0;

    if (text_index == 8 || text_index == 13 || text_index == 18 ||
        text_index == 23) {
      if (text[text_index] != '-') {
        return FR_ERR_DOMAIN;
      }
      text_index += 1u;
      continue;
    }
    if (text_index + 1u >= length || byte_index >= 16) {
      return FR_ERR_DOMAIN;
    }
    high = fr_native_ble_gatt_hex(text[text_index]);
    low = fr_native_ble_gatt_hex(text[text_index + 1u]);
    if (high < 0 || low < 0) {
      return FR_ERR_DOMAIN;
    }
    out_uuid->bytes[byte_index++] = (uint8_t)((high << 4) | low);
    text_index += 2u;
  }
  return byte_index == 16 ? FR_OK : FR_ERR_DOMAIN;
}

#if FR_BLE_ENABLE_GATT_SERVER
static fr_err_t fr_native_ble_gatt_uuid_text(const fr_ble_uuid_t *uuid,
                                             char *out, size_t capacity) {
  static const char hex[] = "0123456789abcdef";
  static const uint8_t hyphens[] = {8, 13, 18, 23};
  uint8_t byte_index = 0;

  if (uuid == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (uuid->kind == FR_BLE_UUID_16) {
    if (capacity < 5) {
      return FR_ERR_CAPACITY;
    }
    for (uint8_t i = 0; i < 2; i++) {
      out[i * 2u] = hex[uuid->bytes[i] >> 4];
      out[i * 2u + 1u] = hex[uuid->bytes[i] & 0x0fu];
    }
    out[4] = '\0';
    return FR_OK;
  }
  if (uuid->kind != FR_BLE_UUID_128 || capacity < 37) {
    return uuid->kind == FR_BLE_UUID_128 ? FR_ERR_CAPACITY : FR_ERR_INVALID;
  }

  for (uint8_t text_index = 0; text_index < 36; text_index++) {
    bool hyphen = false;

    for (uint8_t i = 0; i < sizeof(hyphens); i++) {
      if (text_index == hyphens[i]) {
        hyphen = true;
        break;
      }
    }
    if (hyphen) {
      out[text_index] = '-';
    } else {
      uint8_t byte = uuid->bytes[byte_index / 2u];

      out[text_index] =
          hex[(byte_index & 1u) == 0 ? byte >> 4 : byte & 0x0fu];
      byte_index += 1u;
    }
  }
  out[36] = '\0';
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_GATT_SERVER
static fr_err_t fr_native_ble_gatt_parse_table(
    const fr_runtime_t *runtime, fr_tagged_t rows_tagged,
    fr_ble_gatt_table_t *out_table) {
  enum {
    FR_NATIVE_BLE_GATT_PROPERTIES =
        FR_BLE_GATT_CHR_READ | FR_BLE_GATT_CHR_WRITE |
        FR_BLE_GATT_CHR_WRITE_COMMAND | FR_BLE_GATT_CHR_NOTIFY |
        FR_BLE_GATT_CHR_INDICATE,
    FR_NATIVE_BLE_GATT_SECURITY =
        FR_BLE_GATT_CHR_READ_ENCRYPTED |
        FR_BLE_GATT_CHR_WRITE_ENCRYPTED |
        FR_BLE_GATT_CHR_READ_AUTHENTICATED |
        FR_BLE_GATT_CHR_WRITE_AUTHENTICATED,
    FR_NATIVE_BLE_GATT_FLAGS =
        FR_NATIVE_BLE_GATT_PROPERTIES | FR_NATIVE_BLE_GATT_SECURITY,
  };
  fr_ble_status_t status = {0};
  fr_object_id_t rows_id = 0;
  uint16_t row_count = 0;
  int16_t current_service = -1;
  uint16_t cccd_count = 0;

  if (runtime == NULL || out_table == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_status(&status));
  if (status.radio_state != FR_BLE_RADIO_OFF) {
    return FR_ERR_BLE_BUSY;
  }
  FR_TRY(fr_tagged_decode_object_id(rows_tagged, &rows_id));
  FR_TRY(fr_cells_length(runtime, rows_id, &row_count));
  if (row_count == 0 ||
      row_count > FR_BLE_GATT_SERVICE_COUNT +
                      FR_BLE_GATT_CHARACTERISTIC_COUNT) {
    return FR_ERR_CAPACITY;
  }

  memset(out_table, 0, sizeof(*out_table));
  out_table->row_count = row_count;
  for (uint16_t row_id = 0; row_id < row_count; row_id++) {
    fr_tagged_t row_tagged = fr_tagged_nil();
    fr_tagged_t kind_tagged = fr_tagged_nil();
    fr_tagged_t uuid_tagged = fr_tagged_nil();
    fr_tagged_t flags_tagged = fr_tagged_nil();
    fr_tagged_t size_tagged = fr_tagged_nil();
    fr_object_id_t row_object_id = 0;
    fr_object_id_t uuid_object_id = 0;
    const uint8_t *uuid_text = NULL;
    uint16_t uuid_length = 0;
    fr_int_t kind = 0;
    fr_int_t flags = 0;
    fr_int_t size = 0;
    fr_ble_uuid_t uuid = {0};

    FR_TRY(fr_cells_read(runtime, rows_id, row_id, &row_tagged));
    FR_TRY(fr_tagged_decode_object_id(row_tagged, &row_object_id));
    FR_TRY(fr_record_read_field(runtime, row_object_id,
                                fr_native_ble_gatt_kind_field, &kind_tagged));
    FR_TRY(fr_record_read_field(runtime, row_object_id,
                                fr_native_ble_gatt_uuid_field, &uuid_tagged));
    FR_TRY(fr_record_read_field(runtime, row_object_id,
                                fr_native_ble_gatt_flags_field,
                                &flags_tagged));
    FR_TRY(fr_record_read_field(runtime, row_object_id,
                                fr_native_ble_gatt_size_field, &size_tagged));
    if (fr_tagged_is_bool(kind_tagged) || fr_tagged_is_bool(flags_tagged) ||
        fr_tagged_is_bool(size_tagged)) {
      return FR_ERR_TYPE;
    }
    FR_TRY(fr_tagged_decode_int(kind_tagged, &kind));
    FR_TRY(fr_tagged_decode_int(flags_tagged, &flags));
    FR_TRY(fr_tagged_decode_int(size_tagged, &size));
    FR_TRY(fr_tagged_decode_object_id(uuid_tagged, &uuid_object_id));
    FR_TRY(fr_text_view(runtime, uuid_object_id, &uuid_text, &uuid_length));
    FR_TRY(fr_native_ble_gatt_uuid_parse(uuid_text, uuid_length, &uuid));

    if (kind == FR_BLE_GATT_KIND_SERVICE) {
      fr_ble_gatt_service_row_t *service = NULL;

      if (out_table->service_count == FR_BLE_GATT_SERVICE_COUNT) {
        return FR_ERR_CAPACITY;
      }
      if (flags != FR_BLE_GATT_SERVICE_PRIMARY &&
          flags != FR_BLE_GATT_SERVICE_SECONDARY) {
        return FR_ERR_DOMAIN;
      }
      if (size != 0) {
        return FR_ERR_DOMAIN;
      }
      if (current_service >= 0) {
        fr_ble_gatt_service_row_t *prior =
            &out_table->services[current_service];

        prior->characteristic_count =
            (uint16_t)(out_table->characteristic_count -
                       prior->first_characteristic);
      }

      current_service = (int16_t)out_table->service_count;
      service = &out_table->services[out_table->service_count++];
      *service = (fr_ble_gatt_service_row_t){
          .uuid = uuid,
          .attribute_id = row_id,
          .first_characteristic = out_table->characteristic_count,
          .secondary = flags == FR_BLE_GATT_SERVICE_SECONDARY,
      };
      continue;
    }

    if (kind == FR_BLE_GATT_KIND_CHARACTERISTIC) {
      fr_ble_gatt_characteristic_row_t *characteristic = NULL;

      if (current_service < 0) {
        return FR_ERR_DOMAIN;
      }
      if (out_table->characteristic_count ==
          FR_BLE_GATT_CHARACTERISTIC_COUNT) {
        return FR_ERR_CAPACITY;
      }
      if (flags < 0 || (flags & ~FR_NATIVE_BLE_GATT_FLAGS) != 0 ||
          (flags & FR_NATIVE_BLE_GATT_PROPERTIES) == 0) {
        return FR_ERR_DOMAIN;
      }
      if ((flags & FR_NATIVE_BLE_GATT_SECURITY) != 0) {
        return FR_ERR_UNSUPPORTED;
      }
      if (size < 0 || size > FR_BLE_GATT_VALUE_BYTES ||
          (uint32_t)out_table->value_bytes_used + (uint32_t)size >
              FR_BLE_GATT_VALUE_BYTES) {
        return FR_ERR_CAPACITY;
      }
      if ((flags & (FR_BLE_GATT_CHR_NOTIFY | FR_BLE_GATT_CHR_INDICATE)) != 0) {
        cccd_count += 1u;
        if (cccd_count > FR_BLE_GATT_CCCD_COUNT) {
          return FR_ERR_CAPACITY;
        }
      }

      characteristic =
          &out_table->characteristics[out_table->characteristic_count++];
      *characteristic = (fr_ble_gatt_characteristic_row_t){
          .uuid = uuid,
          .attribute_id = row_id,
          .portable_flags = (uint16_t)flags,
          .maximum_length = (uint16_t)size,
          .value_offset = out_table->value_bytes_used,
      };
      out_table->value_bytes_used =
          (uint16_t)(out_table->value_bytes_used + (uint16_t)size);
      continue;
    }

    return FR_ERR_DOMAIN;
  }

  if (current_service < 0) {
    return FR_ERR_DOMAIN;
  }
  out_table->services[current_service].characteristic_count =
      (uint16_t)(out_table->characteristic_count -
                 out_table->services[current_service].first_characteristic);
  return FR_OK;
}

static fr_err_t fr_native_ble_gatt_install(fr_runtime_t *runtime,
                                           const fr_tagged_t *args,
                                           uint8_t arg_count,
                                           fr_tagged_t *out) {
  fr_ble_gatt_table_t table;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_ble_gatt_parse_table(runtime, args[0], &table));
  FR_TRY(fr_platform_ble_gatt_install(&table));
  *out = fr_tagged_nil();
  return FR_OK;
}
#endif

static fr_err_t fr_native_ble_gatt_info(fr_runtime_t *runtime,
                                        const fr_tagged_t *args,
                                        uint8_t arg_count, fr_tagged_t *out) {
#if FR_BLE_ENABLE_GATT_SERVER
  fr_ble_gatt_status_t status = {0};
  char uuid[37];
#endif
#if FR_BLE_ENABLE_GATT_CLIENT
  fr_ble_gatt_client_status_t client = {0};
#endif
  char line[224];
  int written = 0;

  (void)runtime;
  (void)args;
  if (arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
#if FR_BLE_ENABLE_GATT_SERVER
  FR_TRY(fr_platform_ble_gatt_status(&status));
  written = snprintf(
      line, sizeof(line),
      "ble gatt generation=%lu rows=%u services=%u/%u characteristics=%u/%u values=%u/%u cccds=%u/%u\n",
      (unsigned long)status.table_generation,
      (unsigned)status.table.row_count, (unsigned)status.table.service_count,
      (unsigned)FR_BLE_GATT_SERVICE_COUNT,
      (unsigned)status.table.characteristic_count,
      (unsigned)FR_BLE_GATT_CHARACTERISTIC_COUNT,
      (unsigned)status.table.value_bytes_used,
      (unsigned)FR_BLE_GATT_VALUE_BYTES, (unsigned)status.subscription_count,
      (unsigned)FR_BLE_GATT_CCCD_COUNT);
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));

  for (uint16_t row_id = 0; row_id < status.table.row_count; row_id++) {
    bool found = false;

    for (uint16_t i = 0; i < status.table.service_count; i++) {
      const fr_ble_gatt_service_row_t *service = &status.table.services[i];

      if (service->attribute_id != row_id) {
        continue;
      }
      FR_TRY(fr_native_ble_gatt_uuid_text(&service->uuid, uuid, sizeof(uuid)));
      written = snprintf(
          line, sizeof(line),
          "ble gatt row=%u kind=service uuid=%s flags=%s characteristics=%u\n",
          (unsigned)row_id, uuid,
          service->secondary ? "secondary" : "primary",
          (unsigned)service->characteristic_count);
      FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));
      found = true;
      break;
    }
    if (found) {
      continue;
    }
    for (uint16_t i = 0; i < status.table.characteristic_count; i++) {
      const fr_ble_gatt_characteristic_row_t *characteristic =
          &status.table.characteristics[i];

      if (characteristic->attribute_id != row_id) {
        continue;
      }
      FR_TRY(fr_native_ble_gatt_uuid_text(&characteristic->uuid, uuid,
                                          sizeof(uuid)));
      written = snprintf(
          line, sizeof(line),
          "ble gatt row=%u kind=characteristic uuid=%s flags=0x%04x max=%u length=%u handle=%u\n",
          (unsigned)row_id, uuid, (unsigned)characteristic->portable_flags,
          (unsigned)characteristic->maximum_length,
          (unsigned)characteristic->value_length,
          (unsigned)characteristic->target_value_handle);
      FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));
      found = true;
      break;
    }
    if (!found) {
      return FR_ERR_CORRUPT;
    }
  }

  if (status.current_write_valid) {
    written = snprintf(
        line, sizeof(line),
        "ble gatt writes=%u/%u high-water=%u overflow=%lu stale=%lu preaccept-rejected=%lu current=%u:%u indication=%s\n",
        (unsigned)status.write_queue_count,
        (unsigned)FR_BLE_GATT_WRITE_QUEUE_COUNT,
        (unsigned)status.write_queue_high_water,
        (unsigned long)status.write_queue_overflow,
        (unsigned long)status.write_queue_stale,
        (unsigned long)status.preaccept_write_rejected,
        (unsigned)status.current_write_attribute_id,
        (unsigned)status.current_write_data_length,
        status.indication_pending ? "pending" : "idle");
  } else {
    written = snprintf(
        line, sizeof(line),
        "ble gatt writes=%u/%u high-water=%u overflow=%lu stale=%lu preaccept-rejected=%lu current=none indication=%s\n",
        (unsigned)status.write_queue_count,
        (unsigned)FR_BLE_GATT_WRITE_QUEUE_COUNT,
        (unsigned)status.write_queue_high_water,
        (unsigned long)status.write_queue_overflow,
        (unsigned long)status.write_queue_stale,
        (unsigned long)status.preaccept_write_rejected,
        status.indication_pending ? "pending" : "idle");
  }
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));

  written = snprintf(line, sizeof(line),
                     "ble gatt last-att=%ld platform=%ld\n",
                     (long)status.last_att_error,
                     (long)status.last_platform_code);
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));
#endif

#if FR_BLE_ENABLE_GATT_CLIENT
  FR_TRY(fr_platform_ble_gatt_client_status(&client));
  if (client.current_notification_valid) {
    written = snprintf(
        line, sizeof(line),
        "ble gatt client cache=%u/%u subscriptions=%u notifications=%u/%u high-water=%u dropped=%lu stale=%lu current=%u:%u:%s\n",
        (unsigned)client.cache_count,
        (unsigned)FR_BLE_GATT_CLIENT_CACHE_COUNT,
        (unsigned)client.subscription_count,
        (unsigned)client.notification_queue_count,
        (unsigned)FR_BLE_GATT_NOTIFICATION_QUEUE_COUNT,
        (unsigned)client.notification_queue_high_water,
        (unsigned long)client.notification_dropped,
        (unsigned long)client.notification_stale,
        (unsigned)client.current_notification_attribute_handle,
        (unsigned)client.current_notification_data_length,
        client.current_notification_indication ? "indication" : "notification");
  } else {
    written = snprintf(
        line, sizeof(line),
        "ble gatt client cache=%u/%u subscriptions=%u notifications=%u/%u high-water=%u dropped=%lu stale=%lu current=none\n",
        (unsigned)client.cache_count,
        (unsigned)FR_BLE_GATT_CLIENT_CACHE_COUNT,
        (unsigned)client.subscription_count,
        (unsigned)client.notification_queue_count,
        (unsigned)FR_BLE_GATT_NOTIFICATION_QUEUE_COUNT,
        (unsigned)client.notification_queue_high_water,
        (unsigned long)client.notification_dropped,
        (unsigned long)client.notification_stale);
  }
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));
  written = snprintf(
      line, sizeof(line),
      "ble gatt client procedure=%s attribute=%u service-matches=%u characteristic-matches=%u last-att=%ld platform=%ld\n",
      client.procedure_pending
          ? fr_native_ble_operation_name(client.procedure_operation)
          : "idle",
      (unsigned)client.procedure_attribute_handle,
      (unsigned)client.service_match_count,
      (unsigned)client.characteristic_match_count,
      (long)client.last_att_error, (long)client.last_platform_code);
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));
#endif
  *out = fr_tagged_nil();
  return FR_OK;
}

#if FR_BLE_ENABLE_GATT_SERVER
static fr_err_t fr_native_ble_gatt_set(fr_runtime_t *runtime,
                                       const fr_tagged_t *args,
                                       uint8_t arg_count, fr_tagged_t *out) {
  uint16_t attribute_id = 0;
  const uint8_t *bytes = NULL;
  uint16_t length = 0;

  if (runtime == NULL || args == NULL || arg_count != 2 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &attribute_id));
  FR_TRY(fr_native_decode_text_or_bytes_view(runtime, args[1], &bytes,
                                             &length));
  FR_TRY(fr_platform_ble_gatt_set(attribute_id, bytes, length));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_ble_gatt_get(fr_runtime_t *runtime,
                                       const fr_tagged_t *args,
                                       uint8_t arg_count, fr_tagged_t *out) {
  uint8_t bytes[FR_BLE_GATT_VALUE_BYTES];
  uint16_t attribute_id = 0;
  uint16_t length = 0;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &attribute_id));
  FR_TRY(fr_platform_ble_gatt_get(attribute_id, bytes, sizeof(bytes),
                                  &length));
  return fr_bytes_install(runtime, bytes, length, out);
}

static fr_err_t fr_native_ble_gatt_notify(fr_runtime_t *runtime,
                                          const fr_tagged_t *args,
                                          uint8_t arg_count,
                                          fr_tagged_t *out) {
  uint16_t connection_index = 0;
  uint16_t attribute_id = 0;
  const uint8_t *bytes = NULL;
  uint16_t length = 0;

  if (runtime == NULL || args == NULL || arg_count != 3 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_ble_connection_handle(runtime, args[0], NULL,
                                          &connection_index));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &attribute_id));
  FR_TRY(fr_native_decode_text_or_bytes_view(runtime, args[2], &bytes,
                                             &length));
  FR_TRY(fr_platform_ble_gatt_notify(connection_index, attribute_id, bytes,
                                     length));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_ble_gatt_indicate(fr_runtime_t *runtime,
                                            const fr_tagged_t *args,
                                            uint8_t arg_count,
                                            fr_tagged_t *out) {
  uint16_t connection_index = 0;
  uint16_t attribute_id = 0;
  uint16_t timeout_ms = 0;
  const uint8_t *bytes = NULL;
  uint16_t length = 0;

  if (runtime == NULL || args == NULL || arg_count != 4 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_ble_connection_handle(runtime, args[0], NULL,
                                          &connection_index));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &attribute_id));
  FR_TRY(fr_native_decode_text_or_bytes_view(runtime, args[2], &bytes,
                                             &length));
  FR_TRY(fr_native_decode_u16(args, arg_count, 3, &timeout_ms));
  if (timeout_ms == 0) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_platform_ble_gatt_indicate(runtime, connection_index,
                                       attribute_id, bytes, length,
                                       timeout_ms));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_ble_gatt_next_write(fr_runtime_t *runtime,
                                              const fr_tagged_t *args,
                                              uint8_t arg_count,
                                              fr_tagged_t *out) {
  fr_handle_ref_t connection_ref = {0};
  bool has_write = false;

  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_gatt_write_next(&has_write, &connection_ref));
  if (!has_write) {
    *out = fr_tagged_nil();
    return FR_OK;
  }
  FR_TRY(fr_handle_lookup(runtime, connection_ref,
                          FR_HANDLE_KIND_BLE_CONNECTION, NULL, NULL));
  return fr_tagged_encode_handle_ref(connection_ref, out);
}

static fr_err_t fr_native_ble_gatt_write_attribute(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  fr_ble_gatt_write_t write = {0};

  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_gatt_write_current(&write));
  return fr_tagged_encode_int(write.attribute_id, out);
}

static fr_err_t fr_native_ble_gatt_write_data(fr_runtime_t *runtime,
                                              const fr_tagged_t *args,
                                              uint8_t arg_count,
                                              fr_tagged_t *out) {
  fr_ble_gatt_write_t write = {0};

  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_gatt_write_current(&write));
  return fr_bytes_install(runtime, write.data, write.data_length, out);
}
#endif

#if FR_BLE_ENABLE_GATT_CLIENT
static fr_err_t fr_native_ble_gatt_uuid_arg(const fr_runtime_t *runtime,
                                            fr_tagged_t tagged,
                                            fr_ble_uuid_t *out_uuid) {
  fr_object_id_t object_id = 0;
  const uint8_t *text = NULL;
  uint16_t length = 0;

  if (runtime == NULL || out_uuid == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_tagged_decode_object_id(tagged, &object_id));
  FR_TRY(fr_text_view(runtime, object_id, &text, &length));
  return fr_native_ble_gatt_uuid_parse(text, length, out_uuid);
}

static fr_err_t fr_native_ble_gatt_client_find(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  fr_ble_uuid_t service_uuid = {0};
  fr_ble_uuid_t characteristic_uuid = {0};
  uint16_t connection_index = 0;
  uint16_t timeout_ms = 0;
  uint16_t attribute_handle = 0;

  if (runtime == NULL || args == NULL || arg_count != 4 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_ble_connection_handle(runtime, args[0], NULL,
                                          &connection_index));
  FR_TRY(fr_native_ble_gatt_uuid_arg(runtime, args[1], &service_uuid));
  FR_TRY(fr_native_ble_gatt_uuid_arg(runtime, args[2],
                                    &characteristic_uuid));
  FR_TRY(fr_native_decode_u16(args, arg_count, 3, &timeout_ms));
  if (timeout_ms == 0 || timeout_ms > FR_BLE_GATT_CLIENT_TIMEOUT_MAX_MS) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_platform_ble_gatt_client_find(
      runtime, connection_index, &service_uuid, &characteristic_uuid,
      timeout_ms, &attribute_handle));
  return fr_tagged_encode_int(attribute_handle, out);
}

static fr_err_t fr_native_ble_gatt_client_read(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  uint8_t bytes[FR_BLE_GATT_CLIENT_DATA_BYTES];
  uint16_t connection_index = 0;
  uint16_t attribute_handle = 0;
  uint16_t timeout_ms = 0;
  uint16_t length = 0;

  if (runtime == NULL || args == NULL || arg_count != 3 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_ble_connection_handle(runtime, args[0], NULL,
                                          &connection_index));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &attribute_handle));
  FR_TRY(fr_native_decode_u16(args, arg_count, 2, &timeout_ms));
  if (timeout_ms == 0 || timeout_ms > FR_BLE_GATT_CLIENT_TIMEOUT_MAX_MS) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_platform_ble_gatt_client_read(
      runtime, connection_index, attribute_handle, timeout_ms, bytes,
      sizeof(bytes), &length));
  return fr_bytes_install(runtime, bytes, length, out);
}

static fr_err_t fr_native_ble_gatt_client_write(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  const uint8_t *bytes = NULL;
  uint16_t connection_index = 0;
  uint16_t attribute_handle = 0;
  uint16_t length = 0;
  uint16_t with_response = 0;
  uint16_t timeout_ms = 0;

  if (runtime == NULL || args == NULL || arg_count != 5 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_ble_connection_handle(runtime, args[0], NULL,
                                          &connection_index));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &attribute_handle));
  FR_TRY(fr_native_decode_text_or_bytes_view(runtime, args[2], &bytes,
                                             &length));
  FR_TRY(fr_native_decode_u16(args, arg_count, 3, &with_response));
  FR_TRY(fr_native_decode_u16(args, arg_count, 4, &timeout_ms));
  if (length > FR_BLE_GATT_CLIENT_DATA_BYTES || with_response > 1 ||
      timeout_ms == 0 || timeout_ms > FR_BLE_GATT_CLIENT_TIMEOUT_MAX_MS) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_platform_ble_gatt_client_write(
      runtime, connection_index, attribute_handle, bytes, length,
      with_response == 1, timeout_ms));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_ble_gatt_client_subscribe(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  uint16_t connection_index = 0;
  uint16_t attribute_handle = 0;
  uint16_t mode = 0;
  uint16_t timeout_ms = 0;

  if (runtime == NULL || args == NULL || arg_count != 4 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_ble_connection_handle(runtime, args[0], NULL,
                                          &connection_index));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &attribute_handle));
  FR_TRY(fr_native_decode_u16(args, arg_count, 2, &mode));
  FR_TRY(fr_native_decode_u16(args, arg_count, 3, &timeout_ms));
  if ((mode != FR_BLE_GATT_SUBSCRIBE_NOTIFICATIONS &&
       mode != FR_BLE_GATT_SUBSCRIBE_INDICATIONS) ||
      timeout_ms == 0 || timeout_ms > FR_BLE_GATT_CLIENT_TIMEOUT_MAX_MS) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_platform_ble_gatt_client_subscribe(
      runtime, connection_index, attribute_handle,
      (fr_ble_gatt_subscription_mode_t)mode, timeout_ms));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_ble_gatt_client_unsubscribe(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  uint16_t connection_index = 0;
  uint16_t attribute_handle = 0;
  uint16_t timeout_ms = 0;

  if (runtime == NULL || args == NULL || arg_count != 3 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_ble_connection_handle(runtime, args[0], NULL,
                                          &connection_index));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &attribute_handle));
  FR_TRY(fr_native_decode_u16(args, arg_count, 2, &timeout_ms));
  if (timeout_ms == 0 || timeout_ms > FR_BLE_GATT_CLIENT_TIMEOUT_MAX_MS) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_platform_ble_gatt_client_unsubscribe(
      runtime, connection_index, attribute_handle, timeout_ms));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_ble_gatt_next_notification(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  fr_handle_ref_t connection_ref = {0};
  bool has_notification = false;

  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_gatt_notification_next(&has_notification,
                                                &connection_ref));
  if (!has_notification) {
    *out = fr_tagged_nil();
    return FR_OK;
  }
  FR_TRY(fr_handle_lookup(runtime, connection_ref,
                          FR_HANDLE_KIND_BLE_CONNECTION, NULL, NULL));
  return fr_tagged_encode_handle_ref(connection_ref, out);
}

static fr_err_t fr_native_ble_gatt_notification_attribute(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  fr_ble_gatt_notification_t notification = {0};

  (void)runtime;
  (void)args;
  if (arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_gatt_notification_current(&notification));
  return fr_tagged_encode_int(notification.attribute_handle, out);
}

static fr_err_t fr_native_ble_gatt_notification_data(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  fr_ble_gatt_notification_t notification = {0};

  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_gatt_notification_current(&notification));
  return fr_bytes_install(runtime, notification.data, notification.data_length,
                          out);
}
#endif
#endif

#if FR_BLE_ENABLE_OBSERVER
static fr_err_t fr_native_ble_scan_start(fr_runtime_t *runtime,
                                         const fr_tagged_t *args,
                                         uint8_t arg_count,
                                         fr_tagged_t *out) {
  fr_int_t interval_ms = 0;
  fr_int_t window_ms = 0;
  fr_int_t active = 0;
  fr_int_t repeats = 0;
  fr_int_t minimum_rssi = 0;

  if (runtime == NULL || args == NULL || arg_count != 5 || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_tagged_is_bool(args[0]) || fr_tagged_is_bool(args[1]) ||
      fr_tagged_is_bool(args[2]) || fr_tagged_is_bool(args[3]) ||
      fr_tagged_is_bool(args[4])) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_tagged_decode_int(args[0], &interval_ms));
  FR_TRY(fr_tagged_decode_int(args[1], &window_ms));
  FR_TRY(fr_tagged_decode_int(args[2], &active));
  FR_TRY(fr_tagged_decode_int(args[3], &repeats));
  FR_TRY(fr_tagged_decode_int(args[4], &minimum_rssi));
  if (interval_ms < 0 || interval_ms > UINT16_MAX || window_ms < 0 ||
      window_ms > UINT16_MAX || active < 0 || active > 1 || repeats < 0 ||
      repeats > 1 || minimum_rssi < INT8_MIN || minimum_rssi > INT8_MAX) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_platform_ble_scan_start(
      (uint16_t)interval_ms, (uint16_t)window_ms, active == 1, repeats == 1,
      (int8_t)minimum_rssi));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_ble_scan_stop(fr_runtime_t *runtime,
                                        const fr_tagged_t *args,
                                        uint8_t arg_count,
                                        fr_tagged_t *out) {
  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_scan_stop(runtime));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_ble_scan_next(fr_runtime_t *runtime,
                                        const fr_tagged_t *args,
                                        uint8_t arg_count,
                                        fr_tagged_t *out) {
  bool has_report = false;

  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_scan_next(&has_report));
  return fr_tagged_encode_bool(has_report, out);
}

static fr_err_t fr_native_ble_scan_rssi(fr_runtime_t *runtime,
                                        const fr_tagged_t *args,
                                        uint8_t arg_count,
                                        fr_tagged_t *out) {
  fr_ble_scan_report_t report = {0};

  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_scan_current(&report));
  return fr_tagged_encode_int(report.rssi, out);
}

static fr_err_t fr_native_ble_scan_peer(fr_runtime_t *runtime,
                                        const fr_tagged_t *args,
                                        uint8_t arg_count,
                                        fr_tagged_t *out) {
  fr_ble_scan_report_t report = {0};
  uint8_t peer[7];

  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_scan_current(&report));
  peer[0] = (uint8_t)report.address_type;
  memcpy(&peer[1], report.address, sizeof(report.address));
  return fr_bytes_install(runtime, peer, sizeof(peer), out);
}

static fr_err_t fr_native_ble_scan_flags(fr_runtime_t *runtime,
                                         const fr_tagged_t *args,
                                         uint8_t arg_count,
                                         fr_tagged_t *out) {
  fr_ble_scan_report_t report = {0};

  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_scan_current(&report));
  return fr_tagged_encode_int(report.flags, out);
}

static fr_err_t fr_native_ble_scan_data(fr_runtime_t *runtime,
                                        const fr_tagged_t *args,
                                        uint8_t arg_count,
                                        fr_tagged_t *out) {
  fr_ble_scan_report_t report = {0};

  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_scan_current(&report));
  return fr_bytes_install(runtime, report.data, report.data_length, out);
}
#endif

#if FR_BLE_ENABLE_BROADCASTER
static fr_err_t fr_native_ble_validate_ad_data(const uint8_t *bytes,
                                               uint16_t length) {
  uint16_t offset = 0;

  if (length > FR_BLE_ADVERTISEMENT_DATA_BYTES ||
      (length > 0 && bytes == NULL)) {
    return FR_ERR_CAPACITY;
  }
  while (offset < length) {
    uint8_t field_length = bytes[offset];

    if (field_length == 0 ||
        (uint16_t)(offset + 1u + field_length) > length) {
      return FR_ERR_INVALID;
    }
    offset = (uint16_t)(offset + 1u + field_length);
  }
  return FR_OK;
}

static fr_err_t fr_native_ble_advertise_start(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  const uint8_t *advertising_data = NULL;
  const uint8_t *scan_response_data = NULL;
  uint16_t advertising_data_length = 0;
  uint16_t scan_response_data_length = 0;
  fr_int_t interval_ms = 0;
  fr_int_t connectable = 0;

  if (runtime == NULL || args == NULL || arg_count != 4 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_text_or_bytes_view(
      runtime, args[0], &advertising_data, &advertising_data_length));
  FR_TRY(fr_native_decode_text_or_bytes_view(
      runtime, args[1], &scan_response_data, &scan_response_data_length));
  FR_TRY(fr_native_ble_validate_ad_data(advertising_data,
                                       advertising_data_length));
  FR_TRY(fr_native_ble_validate_ad_data(scan_response_data,
                                       scan_response_data_length));
  if (fr_tagged_is_bool(args[2]) || fr_tagged_is_bool(args[3])) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_tagged_decode_int(args[2], &interval_ms));
  FR_TRY(fr_tagged_decode_int(args[3], &connectable));
  if (interval_ms < 20 || interval_ms > 10240 || connectable < 0 ||
      connectable > 1) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_platform_ble_advertise_start(
      advertising_data, (uint8_t)advertising_data_length, scan_response_data,
      (uint8_t)scan_response_data_length, (uint16_t)interval_ms,
      connectable == 1));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_ble_advertise_stop(fr_runtime_t *runtime,
                                             const fr_tagged_t *args,
                                             uint8_t arg_count,
                                             fr_tagged_t *out) {
  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_advertise_stop());
  *out = fr_tagged_nil();
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
static fr_err_t fr_native_ble_connection_handle(
    fr_runtime_t *runtime, fr_tagged_t tagged, fr_handle_ref_t *out_ref,
    uint16_t *out_platform_index) {
  fr_handle_ref_t ref = {0};

  if (runtime == NULL || out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_tagged_decode_handle_ref(tagged, &ref));
  FR_TRY(fr_handle_lookup(runtime, ref, FR_HANDLE_KIND_BLE_CONNECTION, NULL,
                          out_platform_index));
  if (out_ref != NULL) {
    *out_ref = ref;
  }
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_CENTRAL
static fr_err_t fr_native_ble_connect(fr_runtime_t *runtime,
                                      const fr_tagged_t *args,
                                      uint8_t arg_count, fr_tagged_t *out) {
  fr_bytes_ref_t peer_ref = {0};
  const uint8_t *peer_bytes = NULL;
  uint16_t peer_length = 0;
  uint8_t peer[7];
  fr_int_t timeout_ms = 0;
  fr_handle_ref_t handle_ref = {0};
  fr_tagged_t handle = fr_tagged_nil();
  uint16_t platform_index = 0;
  fr_err_t err = FR_OK;

  if (runtime == NULL || args == NULL || arg_count != 2 || out == NULL ||
      fr_tagged_is_bool(args[1])) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_tagged_decode_bytes_ref(args[0], &peer_ref));
  FR_TRY(fr_bytes_view(runtime, peer_ref, &peer_bytes, &peer_length));
  FR_TRY(fr_tagged_decode_int(args[1], &timeout_ms));
  if (peer_length != sizeof(peer) || peer_bytes[0] > FR_BLE_ADDRESS_RANDOM_ID) {
    return FR_ERR_INVALID;
  }
  if (timeout_ms < 1 || timeout_ms > FR_BLE_CONNECT_TIMEOUT_MAX_MS) {
    return FR_ERR_RANGE;
  }
  memcpy(peer, peer_bytes, sizeof(peer));

  FR_TRY(fr_handle_reserve(runtime, FR_HANDLE_KIND_BLE_CONNECTION,
                           &handle_ref, &handle));
  err = fr_platform_ble_connect(runtime, peer, (uint16_t)timeout_ms, handle_ref,
                                &platform_index);
  if (err != FR_OK) {
    (void)fr_handle_release_reserved(runtime, handle_ref);
    return err;
  }
  err = fr_handle_activate(runtime, handle_ref, platform_index);
  if (err != FR_OK) {
    (void)fr_platform_ble_connection_close(platform_index);
    (void)fr_handle_release_reserved(runtime, handle_ref);
    return err;
  }
  *out = handle;
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_PERIPHERAL
static fr_err_t fr_native_ble_accept(fr_runtime_t *runtime,
                                     const fr_tagged_t *args,
                                     uint8_t arg_count, fr_tagged_t *out) {
  fr_handle_ref_t handle_ref = {0};
  fr_tagged_t handle = fr_tagged_nil();
  uint16_t platform_index = 0;
  bool pending = false;
  bool accepted = false;
  fr_err_t err = FR_OK;

  (void)args;
  if (runtime == NULL || arg_count != 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_platform_ble_connection_pending(&pending));
  if (!pending) {
    *out = fr_tagged_nil();
    return FR_OK;
  }

  err = fr_handle_reserve(runtime, FR_HANDLE_KIND_BLE_CONNECTION,
                          &handle_ref, &handle);
  if (err != FR_OK) {
    (void)fr_platform_ble_reject_pending();
    return err;
  }
  err = fr_platform_ble_accept(handle_ref, &platform_index, &accepted);
  if (err != FR_OK || !accepted) {
    (void)fr_handle_release_reserved(runtime, handle_ref);
    if (err != FR_OK) {
      return err;
    }
    *out = fr_tagged_nil();
    return FR_OK;
  }
  err = fr_handle_activate(runtime, handle_ref, platform_index);
  if (err != FR_OK) {
    (void)fr_platform_ble_connection_close(platform_index);
    (void)fr_handle_release_reserved(runtime, handle_ref);
    return err;
  }
  *out = handle;
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
static fr_err_t fr_native_ble_connection_ready(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  uint16_t platform_index = 0;
  bool ready = false;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_ble_connection_handle(runtime, args[0], NULL,
                                          &platform_index));
  FR_TRY(fr_platform_ble_connection_ready(platform_index, &ready));
  return fr_tagged_encode_bool(ready, out);
}

static fr_err_t fr_native_ble_connection_close(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  fr_handle_ref_t handle_ref = {0};
  uint16_t platform_index = 0;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_ble_connection_handle(runtime, args[0], &handle_ref,
                                          &platform_index));
  (void)platform_index;
  FR_TRY(fr_handle_close(runtime, handle_ref));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_ble_connection_info(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  fr_ble_connection_info_t info = {0};
  uint16_t platform_index = 0;
  char line[224];
  int written = 0;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_ble_connection_handle(runtime, args[0], NULL,
                                          &platform_index));
  FR_TRY(fr_platform_ble_connection_info(platform_index, &info));

  written = snprintf(
      line, sizeof(line),
      "ble connection=%s role=%s peer=%s:%02X:%02X:%02X:%02X:%02X:%02X interval=%luus latency=%u timeout=%luus mtu=%u\n",
      fr_native_ble_connection_state_name(info.state),
      fr_native_ble_connection_role_name(info.role),
      fr_native_ble_address_type_name(info.peer_address_type),
      (unsigned)info.peer_address[0], (unsigned)info.peer_address[1],
      (unsigned)info.peer_address[2], (unsigned)info.peer_address[3],
      (unsigned)info.peer_address[4], (unsigned)info.peer_address[5],
      (unsigned long)info.interval_us, (unsigned)info.latency,
      (unsigned long)info.supervision_timeout_us, (unsigned)info.mtu);
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));

  if (info.rssi_valid) {
    written = snprintf(
        line, sizeof(line),
        "ble security encrypted=%u authenticated=%u bonded=%u rssi=%d connected-at-ms=%lu disconnected-at-ms=%lu reason=%ld\n",
        info.encrypted ? 1u : 0u, info.authenticated ? 1u : 0u,
        info.bonded ? 1u : 0u, (int)info.last_rssi,
        (unsigned long)info.connected_at_ms,
        (unsigned long)info.disconnected_at_ms, (long)info.last_reason);
  } else {
    written = snprintf(
        line, sizeof(line),
        "ble security encrypted=%u authenticated=%u bonded=%u rssi=unknown connected-at-ms=%lu disconnected-at-ms=%lu reason=%ld\n",
        info.encrypted ? 1u : 0u, info.authenticated ? 1u : 0u,
        info.bonded ? 1u : 0u, (unsigned long)info.connected_at_ms,
        (unsigned long)info.disconnected_at_ms, (long)info.last_reason);
  }
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_ble_connection_rssi(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  uint16_t platform_index = 0;
  int8_t rssi = 0;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_ble_connection_handle(runtime, args[0], NULL,
                                          &platform_index));
  FR_TRY(fr_platform_ble_connection_rssi(platform_index, &rssi));
  return fr_tagged_encode_int(rssi, out);
}

static fr_err_t fr_native_ble_connection_params(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  uint16_t platform_index = 0;
  fr_int_t minimum_interval_ms = 0;
  fr_int_t maximum_interval_ms = 0;
  fr_int_t latency = 0;
  fr_int_t supervision_timeout_ms = 0;

  if (runtime == NULL || args == NULL || arg_count != 5 || out == NULL) {
    return FR_ERR_INVALID;
  }
  for (uint8_t i = 1; i < arg_count; i++) {
    if (fr_tagged_is_bool(args[i])) {
      return FR_ERR_INVALID;
    }
  }
  FR_TRY(fr_native_ble_connection_handle(runtime, args[0], NULL,
                                          &platform_index));
  FR_TRY(fr_tagged_decode_int(args[1], &minimum_interval_ms));
  FR_TRY(fr_tagged_decode_int(args[2], &maximum_interval_ms));
  FR_TRY(fr_tagged_decode_int(args[3], &latency));
  FR_TRY(fr_tagged_decode_int(args[4], &supervision_timeout_ms));
  if (minimum_interval_ms < 8 || maximum_interval_ms < minimum_interval_ms ||
      maximum_interval_ms > 4000 || latency < 0 || latency > 499 ||
      supervision_timeout_ms < 100 || supervision_timeout_ms > 32000) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_platform_ble_connection_params(
      platform_index, (uint16_t)minimum_interval_ms,
      (uint16_t)maximum_interval_ms, (uint16_t)latency,
      (uint16_t)supervision_timeout_ms));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_ble_connection_mtu(
    fr_runtime_t *runtime, const fr_tagged_t *args, uint8_t arg_count,
    fr_tagged_t *out) {
  uint16_t platform_index = 0;
  uint16_t actual_mtu = 0;
  fr_int_t requested_mtu = 0;
  fr_int_t timeout_ms = 0;

  if (runtime == NULL || args == NULL || arg_count != 3 || out == NULL ||
      fr_tagged_is_bool(args[1]) || fr_tagged_is_bool(args[2])) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_ble_connection_handle(runtime, args[0], NULL,
                                          &platform_index));
  FR_TRY(fr_tagged_decode_int(args[1], &requested_mtu));
  FR_TRY(fr_tagged_decode_int(args[2], &timeout_ms));
  if (requested_mtu < 23 || requested_mtu > 517 || timeout_ms < 1 ||
      timeout_ms > 60000) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_platform_ble_connection_mtu(
      runtime, platform_index, (uint16_t)requested_mtu,
      (uint16_t)timeout_ms, &actual_mtu));
  return fr_tagged_encode_int(actual_mtu, out);
}
#endif
#endif

#if FR_FEATURE_NATIVE_SIGNATURES
#if FR_FEATURE_TRACE
static const fr_native_param_t fr_native_trace_params[] = {
    {"trace", FR_NATIVE_VALUE_HANDLE},
};
static const fr_native_param_t fr_native_trace_watch_params[] = {
    {"trace", FR_NATIVE_VALUE_HANDLE},
    {"pin", FR_NATIVE_VALUE_INT},
};
static const fr_native_param_t fr_native_trace_wait_params[] = {
    {"trace", FR_NATIVE_VALUE_HANDLE},
    {"timeout_ms", FR_NATIVE_VALUE_INT},
};
static const fr_native_param_t fr_native_trace_index_params[] = {
    {"trace", FR_NATIVE_VALUE_HANDLE},
    {"index", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_trace_open_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_HANDLE,
    .help = "open one bounded digital edge capture",
};
static const fr_native_signature_t fr_native_trace_watch_signature = {
    .params = fr_native_trace_watch_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_INT,
    .help = "watch both edges on a pin; returns channel 0..2",
};
static const fr_native_signature_t fr_native_trace_arm_signature = {
    .params = fr_native_trace_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "clear old edges and arm capture",
};
static const fr_native_signature_t fr_native_trace_wait_signature = {
    .params = fr_native_trace_wait_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "wait up to timeout_ms; true when capture completes",
};
static const fr_native_signature_t fr_native_trace_stop_signature = {
    .params = fr_native_trace_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "finish an armed capture now",
};
static const fr_native_signature_t fr_native_trace_count_signature = {
    .params = fr_native_trace_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return the number of captured edges",
};
static const fr_native_signature_t fr_native_trace_channel_signature = {
    .params = fr_native_trace_index_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return an edge's watched-channel index",
};
static const fr_native_signature_t fr_native_trace_level_signature = {
    .params = fr_native_trace_index_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return the level after an edge",
};
static const fr_native_signature_t fr_native_trace_delta_ns_signature = {
    .params = fr_native_trace_index_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return nanoseconds since the prior captured edge",
};
static const fr_native_signature_t fr_native_trace_complete_p_signature = {
    .params = fr_native_trace_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "true when capture stopped, filled, or reached its span",
};
static const fr_native_signature_t fr_native_trace_dump_signature = {
    .params = fr_native_trace_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "print watched pins and captured edges",
};
static const fr_native_signature_t fr_native_trace_close_signature = {
    .params = fr_native_trace_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "release a trace capture",
};
#endif

#if FR_FEATURE_PULSE
static const fr_native_param_t fr_native_pulse_params[] = {
    {"pulse", FR_NATIVE_VALUE_HANDLE},
};
static const fr_native_param_t fr_native_pulse_open_params[] = {
    {"pin", FR_NATIVE_VALUE_INT},
    {"idle", FR_NATIVE_VALUE_INT},
};
static const fr_native_param_t fr_native_pulse_add_params[] = {
    {"pulse", FR_NATIVE_VALUE_HANDLE},
    {"level", FR_NATIVE_VALUE_INT},
    {"duration_ns", FR_NATIVE_VALUE_INT},
};
static const fr_native_param_t fr_native_pulse_index_params[] = {
    {"pulse", FR_NATIVE_VALUE_HANDLE},
    {"index", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_pulse_open_signature = {
    .params = fr_native_pulse_open_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_HANDLE,
    .help = "open one timed digital output with idle level 0 or 1",
};
static const fr_native_signature_t fr_native_pulse_add_signature = {
    .params = fr_native_pulse_add_params,
    .arg_count = 3,
    .result = FR_NATIVE_VALUE_INT,
    .help = "append one quantized high or low span",
};
static const fr_native_signature_t fr_native_pulse_clear_signature = {
    .params = fr_native_pulse_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "clear all spans while keeping the output open",
};
static const fr_native_signature_t fr_native_pulse_count_signature = {
    .params = fr_native_pulse_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return the number of waveform spans",
};
static const fr_native_signature_t fr_native_pulse_level_signature = {
    .params = fr_native_pulse_index_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return a waveform span's level",
};
static const fr_native_signature_t fr_native_pulse_duration_ns_signature = {
    .params = fr_native_pulse_index_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return a waveform span's actual duration",
};
static const fr_native_signature_t fr_native_pulse_dump_signature = {
    .params = fr_native_pulse_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "print the quantized waveform",
};
static const fr_native_signature_t fr_native_pulse_play_signature = {
    .params = fr_native_pulse_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "transmit the waveform once",
};
static const fr_native_signature_t fr_native_pulse_close_signature = {
    .params = fr_native_pulse_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "release a pulse output",
};
#endif
#endif

#if FR_FEATURE_TEXT && FR_FEATURE_REPL
/* An event handler runs outside the request/response stream (fired at the idle
 * prompt, or interleaved into a running program at a safe point). Its print
 * output is framed with the reserved async-line prefix: each line begins with
 * "! " and ends with a newline. "! " is reserved for this — eval results never
 * begin with it — so a host can route these lines to an event lane. See
 * docs/wire-protocol.md (v1.1). */
static fr_err_t fr_native_print_async(const uint8_t *bytes, uint16_t length) {
  static const uint8_t prefix[2] = {'!', ' '};
  static const uint8_t newline = '\n';
  uint16_t run_start = 0;

  /* An empty print writes nothing (like the synchronous path); emitting a bare
   * "! " line for no payload would be a fake event. */
  if (length == 0) {
    return FR_OK;
  }
  FR_TRY(fr_platform_write_bytes(prefix, sizeof(prefix)));
  for (uint16_t i = 0; i < length; i++) {
    if (bytes[i] == '\n') {
      FR_TRY(fr_platform_write_bytes(&bytes[run_start], (uint16_t)(i + 1 - run_start)));
      run_start = (uint16_t)(i + 1);
      if (run_start < length) {
        FR_TRY(fr_platform_write_bytes(prefix, sizeof(prefix)));
      }
    }
  }
  if (run_start < length) {
    FR_TRY(fr_platform_write_bytes(&bytes[run_start], (uint16_t)(length - run_start)));
  }
  if (bytes[length - 1] != '\n') {
    FR_TRY(fr_platform_write_bytes(&newline, 1));
  }
  return FR_OK;
}

static fr_err_t fr_native_print(fr_runtime_t *runtime, const fr_tagged_t *args,
                                uint8_t arg_count, fr_tagged_t *out) {
  const uint8_t *bytes = NULL;
  uint16_t length = 0;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_text_or_bytes_view(runtime, args[0], &bytes,
                                             &length));
  if (runtime->dispatching_event) {
    FR_TRY(fr_native_print_async(bytes, length));
  } else {
    FR_TRY(fr_platform_write_bytes(bytes, length));
  }
  *out = fr_tagged_nil();
  return FR_OK;
}
#endif

#if FR_FEATURE_MATH
static fr_err_t fr_native_abs(fr_runtime_t *runtime, const fr_tagged_t *args,
                              uint8_t arg_count, fr_tagged_t *out) {
  fr_int_t x = 0;
  int32_t result = 0;

  (void)runtime;
  if (args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_int(args[0], &x));
  result = x < 0 ? -(int32_t)x : (int32_t)x;
  if (result > FR_TAGGED_INT_MAX) {
    return FR_ERR_RANGE;
  }
  return fr_tagged_encode_int(result, out);
}

static fr_err_t fr_native_min(fr_runtime_t *runtime, const fr_tagged_t *args,
                              uint8_t arg_count, fr_tagged_t *out) {
  fr_int_t a = 0;
  fr_int_t b = 0;

  (void)runtime;
  if (args == NULL || arg_count != 2 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_int(args[0], &a));
  FR_TRY(fr_tagged_decode_int(args[1], &b));
  return fr_tagged_encode_int(a < b ? a : b, out);
}

static fr_err_t fr_native_max(fr_runtime_t *runtime, const fr_tagged_t *args,
                              uint8_t arg_count, fr_tagged_t *out) {
  fr_int_t a = 0;
  fr_int_t b = 0;

  (void)runtime;
  if (args == NULL || arg_count != 2 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_int(args[0], &a));
  FR_TRY(fr_tagged_decode_int(args[1], &b));
  return fr_tagged_encode_int(a > b ? a : b, out);
}

static fr_err_t fr_native_clamp(fr_runtime_t *runtime,
                                const fr_tagged_t *args, uint8_t arg_count,
                                fr_tagged_t *out) {
  fr_int_t x = 0;
  fr_int_t lo = 0;
  fr_int_t hi = 0;

  (void)runtime;
  if (args == NULL || arg_count != 3 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_int(args[0], &x));
  FR_TRY(fr_tagged_decode_int(args[1], &lo));
  FR_TRY(fr_tagged_decode_int(args[2], &hi));
  if (lo > hi) {
    return FR_ERR_DOMAIN;
  }
  return fr_tagged_encode_int(x < lo ? lo : (x > hi ? hi : x), out);
}

/* Wide temp because (x - in_lo) * (out_hi - out_lo) can blow past the
 * tagged band before the division pulls it back. The negative-array
 * trick catches a future tranche that widens the tagged band past
 * what int64_t can hold for that product. */
typedef char fr_native_map_diff_product_must_fit_int64[
    (((int64_t)FR_TAGGED_INT_MAX - (int64_t)FR_TAGGED_INT_MIN) *
         ((int64_t)FR_TAGGED_INT_MAX - (int64_t)FR_TAGGED_INT_MIN) <=
     INT64_MAX)
        ? 1 : -1];

static fr_err_t fr_native_map(fr_runtime_t *runtime, const fr_tagged_t *args,
                              uint8_t arg_count, fr_tagged_t *out) {
  fr_int_t x = 0;
  fr_int_t in_lo = 0;
  fr_int_t in_hi = 0;
  fr_int_t out_lo = 0;
  fr_int_t out_hi = 0;
  int64_t result = 0;

  (void)runtime;
  if (args == NULL || arg_count != 5 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_int(args[0], &x));
  FR_TRY(fr_tagged_decode_int(args[1], &in_lo));
  FR_TRY(fr_tagged_decode_int(args[2], &in_hi));
  FR_TRY(fr_tagged_decode_int(args[3], &out_lo));
  FR_TRY(fr_tagged_decode_int(args[4], &out_hi));
  if (in_hi == in_lo) {
    return FR_ERR_DOMAIN;
  }
  result = (int64_t)out_lo +
           ((int64_t)x - (int64_t)in_lo) *
               ((int64_t)out_hi - (int64_t)out_lo) /
               ((int64_t)in_hi - (int64_t)in_lo);
  if (result > (int64_t)FR_TAGGED_INT_MAX ||
      result < (int64_t)FR_TAGGED_INT_MIN) {
    return FR_ERR_RANGE;
  }
  return fr_tagged_encode_int((int32_t)result, out);
}

static fr_err_t fr_native_mod(fr_runtime_t *runtime, const fr_tagged_t *args,
                              uint8_t arg_count, fr_tagged_t *out) {
  fr_int_t a = 0;
  fr_int_t b = 0;

  (void)runtime;
  if (args == NULL || arg_count != 2 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_int(args[0], &a));
  FR_TRY(fr_tagged_decode_int(args[1], &b));
  if (b == 0) {
    return FR_ERR_DOMAIN;
  }
  return fr_tagged_encode_int(a % b, out);
}

static fr_err_t fr_native_sqrt(fr_runtime_t *runtime, const fr_tagged_t *args,
                               uint8_t arg_count, fr_tagged_t *out) {
  fr_int_t x = 0;
  uint32_t remainder = 0;
  uint32_t root = 0;
  uint32_t bit = UINT32_C(1) << 30;

  (void)runtime;
  if (args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_int(args[0], &x));
  if (x < 0) {
    return FR_ERR_DOMAIN;
  }

  remainder = (uint32_t)x;
  while (bit > remainder) {
    bit >>= 2;
  }
  while (bit != 0) {
    if (remainder >= root + bit) {
      remainder -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }

  return fr_tagged_encode_int((int32_t)root, out);
}
#endif

#if FR_FEATURE_RANDOM
static fr_err_t fr_native_random_next(fr_runtime_t *runtime,
                                      const fr_tagged_t *args,
                                      uint8_t arg_count, fr_tagged_t *out) {
  (void)runtime;
  if (args == NULL && arg_count > 0) {
    return FR_ERR_INVALID;
  }
  if (arg_count != 0) {
    return FR_ERR_INVALID;
  }

  /* Mask to the tagged-int payload range so the result is nonnegative on
   * both 16- and 32-bit; skews toward the low bits, accepted for T5b. */
  return fr_tagged_encode_int(
      (int32_t)(fr_platform_random_next() & (uint32_t)FR_TAGGED_INT_MAX), out);
}

static fr_err_t fr_native_random_below(fr_runtime_t *runtime,
                                       const fr_tagged_t *args,
                                       uint8_t arg_count, fr_tagged_t *out) {
  fr_int_t limit = 0;

  (void)runtime;
  if (args == NULL || arg_count != 1) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_int(args[0], &limit));
  if (limit <= 0) {
    return FR_ERR_DOMAIN;
  }

  /* `% limit` accepts a small modulo bias; a reject-and-retry uniform
   * variant is a follow-up. */
  return fr_tagged_encode_int(
      (int32_t)(fr_platform_random_next() % (uint32_t)limit), out);
}

static fr_err_t fr_native_random_seed(fr_runtime_t *runtime,
                                      const fr_tagged_t *args,
                                      uint8_t arg_count, fr_tagged_t *out) {
  fr_int_t seed = 0;

  (void)runtime;
  if (args == NULL || arg_count != 1) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_int(args[0], &seed));
  fr_platform_random_seed((uint32_t)seed);
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

#if FR_FEATURE_TEXT
static fr_err_t fr_native_text_length(fr_runtime_t *runtime,
                                      const fr_tagged_t *args,
                                      uint8_t arg_count, fr_tagged_t *out) {
  fr_object_id_t object_id = 0;
  const uint8_t *bytes = NULL;
  uint16_t length = 0;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_object_id(args[0], &object_id));
  FR_TRY(fr_text_view(runtime, object_id, &bytes, &length));
  return fr_tagged_encode_int((int32_t)length, out);
}

static fr_err_t fr_native_text_equals_p(fr_runtime_t *runtime,
                                        const fr_tagged_t *args,
                                        uint8_t arg_count, fr_tagged_t *out) {
  fr_object_id_t a_id = 0;
  fr_object_id_t b_id = 0;
  const uint8_t *a_bytes = NULL;
  const uint8_t *b_bytes = NULL;
  uint16_t a_length = 0;
  uint16_t b_length = 0;

  if (runtime == NULL || args == NULL || arg_count != 2 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_object_id(args[0], &a_id));
  FR_TRY(fr_tagged_decode_object_id(args[1], &b_id));
  FR_TRY(fr_text_view(runtime, a_id, &a_bytes, &a_length));
  FR_TRY(fr_text_view(runtime, b_id, &b_bytes, &b_length));
  return fr_tagged_encode_bool(
      a_length == b_length && memcmp(a_bytes, b_bytes, a_length) == 0, out);
}

static fr_err_t fr_native_text_at(fr_runtime_t *runtime,
                                  const fr_tagged_t *args, uint8_t arg_count,
                                  fr_tagged_t *out) {
  fr_object_id_t object_id = 0;
  const uint8_t *bytes = NULL;
  uint16_t length = 0;
  fr_int_t index = 0;

  if (runtime == NULL || args == NULL || arg_count != 2 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_object_id(args[0], &object_id));
  FR_TRY(fr_text_view(runtime, object_id, &bytes, &length));
  FR_TRY(fr_tagged_decode_int(args[1], &index));
  if (index < 0 || (uint32_t)index >= length) {
    return FR_ERR_RANGE;
  }
  return fr_tagged_encode_int((int32_t)bytes[index], out);
}

static fr_err_t fr_native_text_concat(fr_runtime_t *runtime,
                                      const fr_tagged_t *args,
                                      uint8_t arg_count, fr_tagged_t *out) {
  fr_object_id_t a_id = 0;
  fr_object_id_t b_id = 0;
  const uint8_t *a_bytes = NULL;
  const uint8_t *b_bytes = NULL;
  uint16_t a_length = 0;
  uint16_t b_length = 0;
  uint8_t joined[FR_PROFILE_MAX_TEXT_LENGTH];
  fr_object_id_t object_id = 0;

  if (runtime == NULL || args == NULL || arg_count != 2 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_object_id(args[0], &a_id));
  FR_TRY(fr_tagged_decode_object_id(args[1], &b_id));
  FR_TRY(fr_text_view(runtime, a_id, &a_bytes, &a_length));
  FR_TRY(fr_text_view(runtime, b_id, &b_bytes, &b_length));
  if ((uint32_t)a_length + (uint32_t)b_length > FR_PROFILE_MAX_TEXT_LENGTH) {
    return FR_ERR_RANGE;
  }
  if (a_length > 0) {
    memcpy(joined, a_bytes, a_length);
  }
  if (b_length > 0) {
    memcpy(joined + a_length, b_bytes, b_length);
  }
  FR_TRY(fr_text_install(runtime, joined, (uint16_t)(a_length + b_length),
                         &object_id));
  return fr_tagged_encode_object_id(object_id, out);
}

static fr_err_t fr_native_text_from_int(fr_runtime_t *runtime,
                                        const fr_tagged_t *args,
                                        uint8_t arg_count, fr_tagged_t *out) {
  /* Twelve bytes covers tagged int min "-1073741824" (11 chars) plus NUL. */
  char buffer[12];
  fr_int_t value = 0;
  int written = 0;
  fr_object_id_t object_id = 0;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_decode_int(args[0], &value));
  written = snprintf(buffer, sizeof(buffer), "%ld", (long)value);
  if (written <= 0 || (size_t)written >= sizeof(buffer)) {
    return FR_ERR_RANGE;
  }
  FR_TRY(fr_text_install(runtime, (const uint8_t *)buffer, (uint16_t)written,
                         &object_id));
  return fr_tagged_encode_object_id(object_id, out);
}
#endif

static fr_err_t fr_native_event_register(fr_runtime_t *runtime,
                                         const fr_tagged_t *args,
                                         uint8_t arg_count, fr_tagged_t *out) {
  fr_int_t kind_int = 0;
  uint16_t source = 0;
  uint16_t debounce_ms = 0;
  uint16_t body_int = 0;
  fr_instruction_stream_t body_view;

  if (runtime == NULL || args == NULL || arg_count != 4 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, 0, &kind_int));
  if (kind_int < FR_EVENT_KIND_GPIO_RISING ||
      kind_int > FR_EVENT_KIND_WIFI_RECONNECTED) {
    return FR_ERR_DOMAIN;
  }
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &source));
  FR_TRY(fr_native_decode_u16(args, arg_count, 2, &debounce_ms));
  FR_TRY(fr_native_decode_u16(args, arg_count, 3, &body_int));
  /* The body is a code object id; reject anything that does not resolve to a
     real code object so a stray int or text id cannot install a binding the
     dispatcher would fail to fire. */
  FR_TRY(fr_code_get_instructions(runtime, (fr_code_object_id_t)body_int,
                                  &body_view));
  FR_TRY(fr_event_register(runtime, (fr_event_kind_t)kind_int, source,
                           debounce_ms, (fr_code_object_id_t)body_int));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_event_cancel(fr_runtime_t *runtime,
                                       const fr_tagged_t *args,
                                       uint8_t arg_count, fr_tagged_t *out) {
  fr_int_t kind_int = 0;
  uint16_t source = 0;

  if (runtime == NULL || args == NULL || arg_count != 2 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, 0, &kind_int));
  if (kind_int < FR_EVENT_KIND_GPIO_RISING ||
      kind_int > FR_EVENT_KIND_WIFI_RECONNECTED) {
    return FR_ERR_DOMAIN;
  }
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &source));
  FR_TRY(fr_event_cancel(runtime, (fr_event_kind_t)kind_int, source));
  *out = fr_tagged_nil();
  return FR_OK;
}

#if FR_INCLUDE_TEST_NATIVES && FR_FEATURE_TEXT
static fr_err_t fr_native_fire_event_view_text(const fr_runtime_t *runtime,
                                               fr_tagged_t arg,
                                               const uint8_t **out_bytes,
                                               uint16_t *out_length) {
  fr_object_id_t object_id = 0;
  FR_TRY(fr_tagged_decode_object_id(arg, &object_id));
  return fr_text_view(runtime, object_id, out_bytes, out_length);
}

static bool fr_native_fire_event_text_equals(const uint8_t *bytes,
                                             uint16_t length, const char *s) {
  size_t s_length = strlen(s);
  return length == s_length && memcmp(bytes, s, s_length) == 0;
}

static fr_err_t fr_native_fire_event(fr_runtime_t *runtime,
                                     const fr_tagged_t *args,
                                     uint8_t arg_count, fr_tagged_t *out) {
  const uint8_t *kind_bytes = NULL;
  uint16_t kind_length = 0;
  const uint8_t *edge_bytes = NULL;
  uint16_t edge_length = 0;
  fr_int_t source_int = 0;
  uint16_t source = 0;
  bool kind_is_on = false;
  bool edge_is_rising = false;
  bool edge_is_falling = false;
  fr_event_kind_t timer_kind = FR_EVENT_KIND_NONE;
  uint32_t now_ms = 0;

  if (runtime == NULL || args == NULL || arg_count != 3 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_fire_event_view_text(runtime, args[0], &kind_bytes,
                                        &kind_length));
  FR_TRY(fr_tagged_decode_int(args[1], &source_int));
  if (source_int < 0 || source_int > 0xFFFF) {
    return FR_ERR_DOMAIN;
  }
  source = (uint16_t)source_int;

  if (fr_native_fire_event_text_equals(kind_bytes, kind_length, "on")) {
    kind_is_on = true;
    if (fr_tagged_is_nil(args[2])) {
      return FR_ERR_DOMAIN;
    }
    FR_TRY(fr_native_fire_event_view_text(runtime, args[2], &edge_bytes,
                                          &edge_length));
    if (fr_native_fire_event_text_equals(edge_bytes, edge_length, "rising")) {
      edge_is_rising = true;
    } else if (fr_native_fire_event_text_equals(edge_bytes, edge_length,
                                                "falling")) {
      edge_is_falling = true;
    } else {
      return FR_ERR_DOMAIN;
    }
  } else if (fr_native_fire_event_text_equals(kind_bytes, kind_length,
                                              "every")) {
    timer_kind = FR_EVENT_KIND_EVERY;
    if (!fr_tagged_is_nil(args[2])) {
      return FR_ERR_DOMAIN;
    }
  } else if (fr_native_fire_event_text_equals(kind_bytes, kind_length,
                                              "after")) {
    timer_kind = FR_EVENT_KIND_AFTER;
    if (!fr_tagged_is_nil(args[2])) {
      return FR_ERR_DOMAIN;
    }
  } else {
    return FR_ERR_DOMAIN;
  }

  for (uint16_t i = 0; i < FR_EVENT_BINDING_COUNT; i++) {
    const fr_event_binding_t *entry = &runtime->events.entries[i];
    bool matches = false;
    if (entry->kind == FR_EVENT_KIND_NONE) {
      continue;
    }
    if (entry->source != source) {
      continue;
    }
    if (kind_is_on) {
      if (entry->kind == FR_EVENT_KIND_GPIO_RISING) {
        matches = edge_is_rising;
      } else if (entry->kind == FR_EVENT_KIND_GPIO_FALLING) {
        matches = edge_is_falling;
      } else if (entry->kind == FR_EVENT_KIND_GPIO_CHANGES) {
        matches = edge_is_rising || edge_is_falling;
      }
    } else {
      matches = entry->kind == timer_kind;
    }
    if (matches) {
      FR_TRY(fr_platform_millis(&now_ms));
      FR_TRY(fr_platform_event_post_test_candidate(i, entry->generation,
                                                   now_ms));
      *out = fr_tagged_nil();
      return FR_OK;
    }
  }
  return FR_ERR_NOT_FOUND;
}
#endif

#if FR_FEATURE_TRACE || FR_FEATURE_PULSE || FR_FEATURE_CONSOLE_ROUTING ||       \
    FR_FEATURE_BLE
static fr_err_t fr_native_write_rendered_line(const char *line, size_t cap,
                                              int written) {
  if (line == NULL) {
    return FR_ERR_INVALID;
  }
  if (written < 0) {
    return FR_ERR_IO;
  }
  if ((size_t)written >= cap) {
    return FR_ERR_CAPACITY;
  }
  return fr_platform_write_text(line);
}
#endif

#if FR_FEATURE_TRACE
static fr_err_t fr_native_decode_trace_handle(fr_runtime_t *runtime,
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
  return fr_handle_lookup(runtime, ref, FR_HANDLE_KIND_TRACE, NULL,
                          out_platform_index);
}

static fr_err_t fr_native_trace_open(fr_runtime_t *runtime,
                                     const fr_tagged_t *args,
                                     uint8_t arg_count, fr_tagged_t *out) {
  fr_handle_ref_t ref = {0};
  fr_tagged_t handle = 0;
  uint16_t platform_index = 0;
  fr_err_t err = FR_OK;

  if (runtime == NULL || out == NULL || arg_count != 0 ||
      (args == NULL && arg_count > 0)) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_handle_reserve(runtime, FR_HANDLE_KIND_TRACE, &ref, &handle));
  err = fr_platform_trace_open(&platform_index);
  if (err != FR_OK) {
    (void)fr_handle_release_reserved(runtime, ref);
    return err;
  }
  err = fr_handle_activate(runtime, ref, platform_index);
  if (err != FR_OK) {
    (void)fr_platform_handle_close(FR_HANDLE_KIND_TRACE, platform_index);
    (void)fr_handle_release_reserved(runtime, ref);
    return err;
  }

  *out = handle;
  return FR_OK;
}

static fr_err_t fr_native_trace_watch(fr_runtime_t *runtime,
                                      const fr_tagged_t *args,
                                      uint8_t arg_count, fr_tagged_t *out) {
  uint16_t platform_index = 0;
  uint16_t pin = 0;
  uint8_t channel = 0;

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_trace_handle(runtime, args, arg_count, 0,
                                       &platform_index));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &pin));
  FR_TRY(fr_platform_trace_watch(platform_index, pin, &channel));
  return fr_tagged_encode_int((int32_t)channel, out);
}

static fr_err_t fr_native_trace_arm(fr_runtime_t *runtime,
                                    const fr_tagged_t *args,
                                    uint8_t arg_count, fr_tagged_t *out) {
  uint16_t platform_index = 0;

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_trace_handle(runtime, args, arg_count, 0,
                                       &platform_index));
  FR_TRY(fr_platform_trace_arm(platform_index));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_trace_wait(fr_runtime_t *runtime,
                                     const fr_tagged_t *args,
                                     uint8_t arg_count, fr_tagged_t *out) {
  uint16_t platform_index = 0;
  fr_int_t timeout_ms = 0;
  uint32_t start_ms = 0;

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_trace_handle(runtime, args, arg_count, 0,
                                       &platform_index));
  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, 1, &timeout_ms));
  if (timeout_ms > FR_TRACE_WAIT_MAX_MS) {
    return FR_ERR_DOMAIN;
  }
  FR_TRY(fr_platform_millis(&start_ms));

  for (;;) {
    fr_trace_status_t status = {0};
    uint32_t now_ms = 0;

    FR_TRY(fr_platform_trace_status(platform_index, &status));
    if (status.state == FR_TRACE_CONFIGURING) {
      return FR_ERR_DOMAIN;
    }
    if (status.state == FR_TRACE_COMPLETE) {
      *out = fr_tagged_true();
      return FR_OK;
    }
    FR_TRY(fr_platform_millis(&now_ms));
    if ((uint32_t)(now_ms - start_ms) >= (uint32_t)timeout_ms) {
      *out = fr_tagged_false();
      return FR_OK;
    }
    FR_TRY(fr_platform_poll_interrupt(runtime));
    if (fr_runtime_is_interrupted(runtime)) {
      return FR_ERR_INTERRUPTED;
    }
    FR_TRY(fr_platform_delay_ms(1));
  }
}

static fr_err_t fr_native_trace_stop(fr_runtime_t *runtime,
                                     const fr_tagged_t *args,
                                     uint8_t arg_count, fr_tagged_t *out) {
  uint16_t platform_index = 0;

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_trace_handle(runtime, args, arg_count, 0,
                                       &platform_index));
  FR_TRY(fr_platform_trace_stop(platform_index));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_trace_count(fr_runtime_t *runtime,
                                      const fr_tagged_t *args,
                                      uint8_t arg_count, fr_tagged_t *out) {
  uint16_t platform_index = 0;
  fr_trace_status_t status = {0};

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_trace_handle(runtime, args, arg_count, 0,
                                       &platform_index));
  FR_TRY(fr_platform_trace_status(platform_index, &status));
  return fr_tagged_encode_int((int32_t)status.event_count, out);
}

static fr_err_t fr_native_trace_event_field(fr_runtime_t *runtime,
                                            const fr_tagged_t *args,
                                            uint8_t arg_count,
                                            fr_trace_event_t *out_event) {
  uint16_t platform_index = 0;
  uint16_t event_index = 0;

  if (out_event == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_trace_handle(runtime, args, arg_count, 0,
                                       &platform_index));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &event_index));
  return fr_platform_trace_event(platform_index, event_index, out_event);
}

static fr_err_t fr_native_trace_channel(fr_runtime_t *runtime,
                                        const fr_tagged_t *args,
                                        uint8_t arg_count,
                                        fr_tagged_t *out) {
  fr_trace_event_t event = {0};

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_trace_event_field(runtime, args, arg_count, &event));
  return fr_tagged_encode_int((int32_t)event.channel, out);
}

static fr_err_t fr_native_trace_level(fr_runtime_t *runtime,
                                      const fr_tagged_t *args,
                                      uint8_t arg_count, fr_tagged_t *out) {
  fr_trace_event_t event = {0};

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_trace_event_field(runtime, args, arg_count, &event));
  return fr_tagged_encode_int((int32_t)event.level, out);
}

static fr_err_t fr_native_trace_delta_ns(fr_runtime_t *runtime,
                                         const fr_tagged_t *args,
                                         uint8_t arg_count,
                                         fr_tagged_t *out) {
  fr_trace_event_t event = {0};

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_trace_event_field(runtime, args, arg_count, &event));
  return fr_tagged_encode_int((int32_t)event.delta_ns, out);
}

static fr_err_t fr_native_trace_complete_p(fr_runtime_t *runtime,
                                           const fr_tagged_t *args,
                                           uint8_t arg_count,
                                           fr_tagged_t *out) {
  uint16_t platform_index = 0;
  fr_trace_status_t status = {0};

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_trace_handle(runtime, args, arg_count, 0,
                                       &platform_index));
  FR_TRY(fr_platform_trace_status(platform_index, &status));
  *out = status.state == FR_TRACE_COMPLETE ? fr_tagged_true()
                                           : fr_tagged_false();
  return FR_OK;
}

static fr_err_t fr_native_trace_dump(fr_runtime_t *runtime,
                                     const fr_tagged_t *args,
                                     uint8_t arg_count, fr_tagged_t *out) {
  char line[96];
  uint16_t platform_index = 0;
  fr_trace_status_t status = {0};
  int written = 0;

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_trace_handle(runtime, args, arg_count, 0,
                                       &platform_index));
  FR_TRY(fr_platform_trace_status(platform_index, &status));
  if (status.state != FR_TRACE_COMPLETE) {
    return FR_ERR_DOMAIN;
  }

  written = snprintf(line, sizeof(line),
                     "trace state=complete channels=%u events=%u tick_ns=%u\n",
                     (unsigned)status.channel_count,
                     (unsigned)status.event_count, (unsigned)FR_SIGNAL_TICK_NS);
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));

  for (uint8_t channel = 0; channel < status.channel_count; channel++) {
    written = snprintf(line, sizeof(line), "trace.channel %u pin=%u\n",
                       (unsigned)channel, (unsigned)status.pins[channel]);
    FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));
  }
  for (uint16_t i = 0; i < status.event_count; i++) {
    fr_trace_event_t event = {0};

    FR_TRY(fr_platform_trace_event(platform_index, i, &event));
    written = snprintf(
        line, sizeof(line),
        "trace.edge %u channel=%u level=%u delta_ns=%lu\n", (unsigned)i,
        (unsigned)event.channel, (unsigned)event.level,
        (unsigned long)event.delta_ns);
    FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));
  }

  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_trace_close(fr_runtime_t *runtime,
                                      const fr_tagged_t *args,
                                      uint8_t arg_count, fr_tagged_t *out) {
  fr_handle_ref_t ref = {0};

  if (runtime == NULL || args == NULL || arg_count == 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_tagged_decode_handle_ref(args[0], &ref));
  FR_TRY(fr_handle_lookup(runtime, ref, FR_HANDLE_KIND_TRACE, NULL, NULL));
  FR_TRY(fr_handle_close(runtime, ref));
  *out = fr_tagged_nil();
  return FR_OK;
}
#endif

#if FR_FEATURE_PULSE
static fr_err_t fr_native_decode_pulse_handle(fr_runtime_t *runtime,
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
  return fr_handle_lookup(runtime, ref, FR_HANDLE_KIND_PULSE, NULL,
                          out_platform_index);
}

static fr_err_t fr_native_pulse_open(fr_runtime_t *runtime,
                                     const fr_tagged_t *args,
                                     uint8_t arg_count, fr_tagged_t *out) {
  uint16_t pin = 0;
  uint16_t idle = 0;
  fr_handle_ref_t ref = {0};
  fr_tagged_t handle = 0;
  uint16_t platform_index = 0;
  fr_err_t err = FR_OK;

  if (runtime == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &pin));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &idle));
  if (idle > 1) {
    return FR_ERR_DOMAIN;
  }

  FR_TRY(fr_handle_reserve(runtime, FR_HANDLE_KIND_PULSE, &ref, &handle));
  err = fr_platform_pulse_open(pin, (uint8_t)idle, &platform_index);
  if (err != FR_OK) {
    (void)fr_handle_release_reserved(runtime, ref);
    return err;
  }
  err = fr_handle_activate(runtime, ref, platform_index);
  if (err != FR_OK) {
    (void)fr_platform_handle_close(FR_HANDLE_KIND_PULSE, platform_index);
    (void)fr_handle_release_reserved(runtime, ref);
    return err;
  }

  *out = handle;
  return FR_OK;
}

static fr_err_t fr_native_pulse_add(fr_runtime_t *runtime,
                                    const fr_tagged_t *args,
                                    uint8_t arg_count, fr_tagged_t *out) {
  uint16_t platform_index = 0;
  uint16_t level = 0;
  fr_int_t requested_ns = 0;
  uint32_t actual_ns = 0;
  uint16_t segment_index = 0;

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_pulse_handle(runtime, args, arg_count, 0,
                                       &platform_index));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &level));
  FR_TRY(fr_native_decode_nonnegative_int(args, arg_count, 2, &requested_ns));
  if (level > 1 || requested_ns < FR_SIGNAL_TICK_NS ||
      requested_ns > FR_SIGNAL_MAX_SPAN_NS) {
    return FR_ERR_DOMAIN;
  }
  actual_ns = (((uint32_t)requested_ns + FR_SIGNAL_TICK_NS / 2u) /
               FR_SIGNAL_TICK_NS) *
              FR_SIGNAL_TICK_NS;
  FR_TRY(fr_platform_pulse_add(platform_index, (uint8_t)level, actual_ns,
                               &segment_index));
  return fr_tagged_encode_int((int32_t)segment_index, out);
}

static fr_err_t fr_native_pulse_clear(fr_runtime_t *runtime,
                                      const fr_tagged_t *args,
                                      uint8_t arg_count, fr_tagged_t *out) {
  uint16_t platform_index = 0;

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_pulse_handle(runtime, args, arg_count, 0,
                                       &platform_index));
  FR_TRY(fr_platform_pulse_clear(platform_index));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_pulse_count(fr_runtime_t *runtime,
                                      const fr_tagged_t *args,
                                      uint8_t arg_count, fr_tagged_t *out) {
  uint16_t platform_index = 0;
  fr_pulse_status_t status = {0};

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_pulse_handle(runtime, args, arg_count, 0,
                                       &platform_index));
  FR_TRY(fr_platform_pulse_status(platform_index, &status));
  return fr_tagged_encode_int((int32_t)status.segment_count, out);
}

static fr_err_t fr_native_pulse_segment_field(fr_runtime_t *runtime,
                                              const fr_tagged_t *args,
                                              uint8_t arg_count,
                                              fr_pulse_segment_t *out_segment) {
  uint16_t platform_index = 0;
  uint16_t segment_index = 0;

  if (out_segment == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_pulse_handle(runtime, args, arg_count, 0,
                                       &platform_index));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &segment_index));
  return fr_platform_pulse_segment(platform_index, segment_index, out_segment);
}

static fr_err_t fr_native_pulse_level(fr_runtime_t *runtime,
                                      const fr_tagged_t *args,
                                      uint8_t arg_count, fr_tagged_t *out) {
  fr_pulse_segment_t segment = {0};

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_pulse_segment_field(runtime, args, arg_count, &segment));
  return fr_tagged_encode_int((int32_t)segment.level, out);
}

static fr_err_t fr_native_pulse_duration_ns(fr_runtime_t *runtime,
                                            const fr_tagged_t *args,
                                            uint8_t arg_count,
                                            fr_tagged_t *out) {
  fr_pulse_segment_t segment = {0};

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_pulse_segment_field(runtime, args, arg_count, &segment));
  return fr_tagged_encode_int((int32_t)segment.duration_ns, out);
}

static fr_err_t fr_native_pulse_dump(fr_runtime_t *runtime,
                                     const fr_tagged_t *args,
                                     uint8_t arg_count, fr_tagged_t *out) {
  char line[96];
  uint16_t platform_index = 0;
  fr_pulse_status_t status = {0};
  int written = 0;

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_pulse_handle(runtime, args, arg_count, 0,
                                       &platform_index));
  FR_TRY(fr_platform_pulse_status(platform_index, &status));

  written = snprintf(
      line, sizeof(line),
      "pulse pin=%u idle=%u segments=%u tick_ns=%u total_ns=%lu\n",
      (unsigned)status.pin, (unsigned)status.idle,
      (unsigned)status.segment_count, (unsigned)FR_SIGNAL_TICK_NS,
      (unsigned long)status.total_ns);
  FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));
  for (uint16_t i = 0; i < status.segment_count; i++) {
    fr_pulse_segment_t segment = {0};

    FR_TRY(fr_platform_pulse_segment(platform_index, i, &segment));
    written = snprintf(line, sizeof(line),
                       "pulse.segment %u level=%u duration_ns=%lu\n",
                       (unsigned)i, (unsigned)segment.level,
                       (unsigned long)segment.duration_ns);
    FR_TRY(fr_native_write_rendered_line(line, sizeof(line), written));
  }

  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_pulse_play(fr_runtime_t *runtime,
                                     const fr_tagged_t *args,
                                     uint8_t arg_count, fr_tagged_t *out) {
  uint16_t platform_index = 0;

  if (out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_pulse_handle(runtime, args, arg_count, 0,
                                       &platform_index));
  FR_TRY(fr_platform_pulse_play(platform_index));
  *out = fr_tagged_nil();
  return FR_OK;
}

static fr_err_t fr_native_pulse_close(fr_runtime_t *runtime,
                                      const fr_tagged_t *args,
                                      uint8_t arg_count, fr_tagged_t *out) {
  fr_handle_ref_t ref = {0};

  if (runtime == NULL || args == NULL || arg_count == 0 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_tagged_decode_handle_ref(args[0], &ref));
  FR_TRY(fr_handle_lookup(runtime, ref, FR_HANDLE_KIND_PULSE, NULL, NULL));
  FR_TRY(fr_handle_close(runtime, ref));
  *out = fr_tagged_nil();
  return FR_OK;
}
#endif

#if FR_FEATURE_NATIVE_SIGNATURES
static const fr_native_param_t fr_native_int_params[] = {
    {NULL, FR_NATIVE_VALUE_INT},
};

static const fr_native_param_t fr_native_two_int_params[] = {
    {NULL, FR_NATIVE_VALUE_INT},
    {NULL, FR_NATIVE_VALUE_INT},
};

#if FR_FEATURE_UART
static const fr_native_param_t fr_native_handle_params[] = {
    {NULL, FR_NATIVE_VALUE_HANDLE},
};

static const fr_native_param_t fr_native_handle_int_params[] = {
    {NULL, FR_NATIVE_VALUE_HANDLE},
    {NULL, FR_NATIVE_VALUE_INT},
};
#endif

static const fr_native_signature_t fr_native_millis_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_INT,
    .help = "read milliseconds since boot, wrapped to int range",
};

static const fr_native_signature_t fr_native_micros_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_INT,
    .help = "read microseconds since boot, wrapped to int range",
};

#if FR_FEATURE_PAD
static const fr_native_signature_t fr_native_nil_to_int_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_INT,
    .help = NULL,
};

static const fr_native_signature_t fr_native_nil_to_nil_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_NIL,
    .help = NULL,
};

static const fr_native_signature_t fr_native_int_to_nil_signature = {
    .params = fr_native_int_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = NULL,
};

#if FR_FEATURE_TEXT
static const fr_native_signature_t fr_native_pad_pack_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_TEXT,
    .help = "pack the pad bytes into a text value",
};
#endif
#endif

#if FR_FEATURE_TEXT
static const fr_native_param_t fr_native_text_length_params[] = {
    {"t", FR_NATIVE_VALUE_TEXT},
};
static const fr_native_signature_t fr_native_text_length_signature = {
    .params = fr_native_text_length_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return the byte length of a text",
};

static const fr_native_param_t fr_native_text_equals_p_params[] = {
    {"a", FR_NATIVE_VALUE_TEXT},
    {"b", FR_NATIVE_VALUE_TEXT},
};
static const fr_native_signature_t fr_native_text_equals_p_signature = {
    .params = fr_native_text_equals_p_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "return true if two texts have equal bytes",
};

static const fr_native_param_t fr_native_text_at_params[] = {
    {"t", FR_NATIVE_VALUE_TEXT},
    {"i", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_text_at_signature = {
    .params = fr_native_text_at_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return the byte at index i of a text",
};

static const fr_native_param_t fr_native_text_concat_params[] = {
    {"a", FR_NATIVE_VALUE_TEXT},
    {"b", FR_NATIVE_VALUE_TEXT},
};
static const fr_native_signature_t fr_native_text_concat_signature = {
    .params = fr_native_text_concat_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_TEXT,
    .help = "join two texts into a new text",
};

static const fr_native_param_t fr_native_text_from_int_params[] = {
    {"n", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_text_from_int_signature = {
    .params = fr_native_text_from_int_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_TEXT,
    .help = "render an int as decimal text",
};
#endif

static const fr_native_param_t fr_native_event_register_params[] = {
    {"kind", FR_NATIVE_VALUE_INT},
    {"source", FR_NATIVE_VALUE_INT},
    {"debounce", FR_NATIVE_VALUE_INT},
    {"body", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_event_register_signature = {
    .params = fr_native_event_register_params,
    .arg_count = 4,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "register an event binding from compiled on/every/after",
};

static const fr_native_param_t fr_native_event_cancel_params[] = {
    {"kind", FR_NATIVE_VALUE_INT},
    {"source", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_event_cancel_signature = {
    .params = fr_native_event_cancel_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "cancel an event binding from compiled cancel",
};

#if FR_INCLUDE_TEST_NATIVES && FR_FEATURE_TEXT
static const fr_native_param_t fr_native_fire_event_params[] = {
    {"kind", FR_NATIVE_VALUE_TEXT},
    {"source", FR_NATIVE_VALUE_INT},
    {"edge", FR_NATIVE_VALUE_ANY},
};
static const fr_native_signature_t fr_native_fire_event_signature = {
    .params = fr_native_fire_event_params,
    .arg_count = 3,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "test-only: queue an event candidate for a registered binding",
};
#endif

static const fr_native_param_t fr_native_ms_params[] = {
    {"millis", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_ms_signature = {
    .params = fr_native_ms_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "sleep for a number of milliseconds",
};

static const fr_native_param_t fr_native_gpio_read_params[] = {
    {"pin", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_gpio_read_signature = {
    .params = fr_native_gpio_read_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_INT,
    .help = "read the level of a gpio pin",
};

static const fr_native_signature_t fr_native_int_to_int_signature = {
    .params = fr_native_int_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_INT,
    .help = NULL,
};

static const fr_native_param_t fr_native_gpio_write_params[] = {
    {"pin", FR_NATIVE_VALUE_INT},
    {"level", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_gpio_write_signature = {
    .params = fr_native_gpio_write_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "set gpio pin to a level (0 or 1)",
};

static const fr_native_signature_t fr_native_gpio_write_to_nil_signature = {
    .params = fr_native_two_int_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_NIL,
    .help = NULL,
};

static const fr_native_param_t fr_native_adc_above_params[] = {
    {"pin", FR_NATIVE_VALUE_INT},
    {"threshold", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_adc_above_signature = {
    .params = fr_native_adc_above_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "true when an adc pin reads above a threshold",
};

#if FR_FEATURE_UART
static const fr_native_param_t fr_native_uart_open_params[] = {
    {"port", FR_NATIVE_VALUE_INT},
    {"baud", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_uart_open_signature = {
    .params = fr_native_uart_open_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_HANDLE,
    .help = "open a uart port at a baud rate",
};

static const fr_native_param_t fr_native_uart_open_on_params[] = {
    {"port", FR_NATIVE_VALUE_INT},
    {"tx", FR_NATIVE_VALUE_INT},
    {"rx", FR_NATIVE_VALUE_INT},
    {"baud", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_uart_open_on_signature = {
    .params = fr_native_uart_open_on_params,
    .arg_count = 4,
    .result = FR_NATIVE_VALUE_HANDLE,
    .help = "open a uart on caller-picked tx and rx pins",
};

static const fr_native_signature_t fr_native_uart_write_byte_signature = {
    .params = fr_native_handle_int_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_NIL,
    .help = NULL,
};

static const fr_native_signature_t fr_native_handle_to_int_signature = {
    .params = fr_native_handle_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_INT,
    .help = NULL,
};

static const fr_native_signature_t fr_native_handle_to_nil_signature = {
    .params = fr_native_handle_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = NULL,
};
#endif

#if FR_FEATURE_CONSOLE_ROUTING
static const fr_native_param_t fr_native_console_uart_params[] = {
    {"tx", FR_NATIVE_VALUE_INT},
    {"rx", FR_NATIVE_VALUE_INT},
    {"baud", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_console_uart_signature = {
    .params = fr_native_console_uart_params,
    .arg_count = 3,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "move the active REPL to UART pins at a literal baud rate",
};

static const fr_native_signature_t fr_native_console_default_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "restore the board's boot and recovery console",
};

static const fr_native_signature_t fr_native_console_info_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "print the active console route",
};
#endif

#if FR_FEATURE_BLE
static const fr_native_signature_t fr_native_ble_on_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "initialize the compiled BLE roles and wait for radio readiness",
};

static const fr_native_signature_t fr_native_ble_off_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "close BLE links and shut down the radio",
};

static const fr_native_signature_t fr_native_ble_info_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "print BLE roles, radio, scan, advertising, connections, queue "
            "pressure, and last raw reason",
};

#if FR_BLE_ENABLE_OBSERVER
static const fr_native_param_t fr_native_ble_scan_start_params[] = {
    {"interval_ms", FR_NATIVE_VALUE_INT},
    {"window_ms", FR_NATIVE_VALUE_INT},
    {"active", FR_NATIVE_VALUE_INT},
    {"repeats", FR_NATIVE_VALUE_INT},
    {"minimum_rssi", FR_NATIVE_VALUE_INT},
};

static const fr_native_signature_t fr_native_ble_scan_start_signature = {
    .params = fr_native_ble_scan_start_params,
    .arg_count = 5,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "start an indefinite BLE scan with interval/window ms, "
            "active/repeat flags, and minimum RSSI",
};

static const fr_native_signature_t fr_native_ble_scan_stop_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "stop the active BLE scan and retain queued reports",
};

static const fr_native_signature_t fr_native_ble_scan_next_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "move to the next queued BLE report and say whether one was "
            "available",
};

static const fr_native_signature_t fr_native_ble_scan_rssi_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return the current BLE report RSSI in dBm",
};

static const fr_native_signature_t fr_native_ble_scan_peer_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "copy the current BLE peer type and canonical address bytes",
};

static const fr_native_signature_t fr_native_ble_scan_flags_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return the current BLE report flags",
};

static const fr_native_signature_t fr_native_ble_scan_data_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "copy the current raw BLE advertisement bytes",
};
#endif

#if FR_BLE_ENABLE_BROADCASTER
static const fr_native_param_t fr_native_ble_advertise_start_params[] = {
    {"advertising_data", FR_NATIVE_VALUE_TEXT_OR_BYTES},
    {"scan_response_data", FR_NATIVE_VALUE_TEXT_OR_BYTES},
    {"interval_ms", FR_NATIVE_VALUE_INT},
    {"connectable", FR_NATIVE_VALUE_INT},
};

static const fr_native_signature_t fr_native_ble_advertise_start_signature = {
    .params = fr_native_ble_advertise_start_params,
    .arg_count = 4,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "start legacy BLE advertising with raw AD payloads",
};

static const fr_native_signature_t fr_native_ble_advertise_stop_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "stop legacy BLE advertising",
};
#endif

#if FR_BLE_ENABLE_CENTRAL
static const fr_native_param_t fr_native_ble_connect_params[] = {
    {"peer", FR_NATIVE_VALUE_ANY},
    {"timeout_ms", FR_NATIVE_VALUE_INT},
};

static const fr_native_signature_t fr_native_ble_connect_signature = {
    .params = fr_native_ble_connect_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_HANDLE,
    .help = "connect to one scanned BLE peer",
};
#endif

#if FR_BLE_ENABLE_PERIPHERAL
static const fr_native_signature_t fr_native_ble_accept_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "accept one pending BLE connection or return nil",
};
#endif

#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
static const fr_native_param_t fr_native_ble_connection_params_one[] = {
    {"connection", FR_NATIVE_VALUE_HANDLE},
};

static const fr_native_param_t fr_native_ble_connection_update_params[] = {
    {"connection", FR_NATIVE_VALUE_HANDLE},
    {"minimum_interval_ms", FR_NATIVE_VALUE_INT},
    {"maximum_interval_ms", FR_NATIVE_VALUE_INT},
    {"latency", FR_NATIVE_VALUE_INT},
    {"supervision_timeout_ms", FR_NATIVE_VALUE_INT},
};

static const fr_native_param_t fr_native_ble_connection_mtu_params[] = {
    {"connection", FR_NATIVE_VALUE_HANDLE},
    {"requested_mtu", FR_NATIVE_VALUE_INT},
    {"timeout_ms", FR_NATIVE_VALUE_INT},
};

static const fr_native_signature_t fr_native_ble_connection_ready_signature = {
    .params = fr_native_ble_connection_params_one,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "say whether a BLE connection is live",
};

static const fr_native_signature_t fr_native_ble_connection_close_signature = {
    .params = fr_native_ble_connection_params_one,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "disconnect and release a BLE connection handle",
};

static const fr_native_signature_t fr_native_ble_connection_info_signature = {
    .params = fr_native_ble_connection_params_one,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "print peer, link parameters, security state, and raw reason",
};

static const fr_native_signature_t fr_native_ble_connection_rssi_signature = {
    .params = fr_native_ble_connection_params_one,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_INT,
    .help = "read the live BLE connection RSSI in dBm",
};

static const fr_native_signature_t fr_native_ble_connection_params_signature = {
    .params = fr_native_ble_connection_update_params,
    .arg_count = 5,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "request bounded BLE connection parameters",
};

static const fr_native_signature_t fr_native_ble_connection_mtu_signature = {
    .params = fr_native_ble_connection_mtu_params,
    .arg_count = 3,
    .result = FR_NATIVE_VALUE_INT,
    .help = "exchange and return the actual BLE ATT MTU",
};
#endif

#if FR_BLE_ENABLE_GATT_SERVER || FR_BLE_ENABLE_GATT_CLIENT
#if FR_BLE_ENABLE_GATT_SERVER
static const fr_native_param_t fr_native_ble_gatt_install_params[] = {
    {"rows", FR_NATIVE_VALUE_ANY},
};

static const fr_native_signature_t fr_native_ble_gatt_install_signature = {
    .params = fr_native_ble_gatt_install_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "validate and copy one declarative local GATT table",
};
#endif

static const fr_native_signature_t fr_native_ble_gatt_info_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "print local-server and remote-client GATT state",
};

#if FR_BLE_ENABLE_GATT_SERVER
static const fr_native_param_t fr_native_ble_gatt_set_params[] = {
    {"attribute", FR_NATIVE_VALUE_INT},
    {"data", FR_NATIVE_VALUE_TEXT_OR_BYTES},
};

static const fr_native_signature_t fr_native_ble_gatt_set_signature = {
    .params = fr_native_ble_gatt_set_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "replace one local GATT value by source-row ID",
};

static const fr_native_param_t fr_native_ble_gatt_get_params[] = {
    {"attribute", FR_NATIVE_VALUE_INT},
};

static const fr_native_signature_t fr_native_ble_gatt_get_signature = {
    .params = fr_native_ble_gatt_get_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "copy one local GATT value by source-row ID",
};

static const fr_native_param_t fr_native_ble_gatt_notify_params[] = {
    {"connection", FR_NATIVE_VALUE_HANDLE},
    {"attribute", FR_NATIVE_VALUE_INT},
    {"data", FR_NATIVE_VALUE_TEXT_OR_BYTES},
};

static const fr_native_signature_t fr_native_ble_gatt_notify_signature = {
    .params = fr_native_ble_gatt_notify_params,
    .arg_count = 3,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "send one subscribed GATT notification",
};

static const fr_native_param_t fr_native_ble_gatt_indicate_params[] = {
    {"connection", FR_NATIVE_VALUE_HANDLE},
    {"attribute", FR_NATIVE_VALUE_INT},
    {"data", FR_NATIVE_VALUE_TEXT_OR_BYTES},
    {"timeout_ms", FR_NATIVE_VALUE_INT},
};

static const fr_native_signature_t fr_native_ble_gatt_indicate_signature = {
    .params = fr_native_ble_gatt_indicate_params,
    .arg_count = 4,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "send and wait interruptibly for one subscribed GATT indication",
};

static const fr_native_signature_t fr_native_ble_gatt_next_write_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "advance to the next accepted remote GATT write",
};

static const fr_native_signature_t
    fr_native_ble_gatt_write_attribute_signature = {
        .params = NULL,
        .arg_count = 0,
        .result = FR_NATIVE_VALUE_INT,
        .help = "return the current remote write source-row ID",
};

static const fr_native_signature_t fr_native_ble_gatt_write_data_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "copy the current remote GATT write data",
};
#endif

#if FR_BLE_ENABLE_GATT_CLIENT
static const fr_native_param_t fr_native_ble_gatt_client_find_params[] = {
    {"connection", FR_NATIVE_VALUE_HANDLE},
    {"service_uuid", FR_NATIVE_VALUE_TEXT},
    {"characteristic_uuid", FR_NATIVE_VALUE_TEXT},
    {"timeout_ms", FR_NATIVE_VALUE_INT},
};

static const fr_native_signature_t fr_native_ble_gatt_client_find_signature = {
    .params = fr_native_ble_gatt_client_find_params,
    .arg_count = 4,
    .result = FR_NATIVE_VALUE_INT,
    .help = "find one remote characteristic by service and characteristic UUID",
};

static const fr_native_param_t fr_native_ble_gatt_client_read_params[] = {
    {"connection", FR_NATIVE_VALUE_HANDLE},
    {"attribute", FR_NATIVE_VALUE_INT},
    {"timeout_ms", FR_NATIVE_VALUE_INT},
};

static const fr_native_signature_t fr_native_ble_gatt_client_read_signature = {
    .params = fr_native_ble_gatt_client_read_params,
    .arg_count = 3,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "read one short remote characteristic value",
};

static const fr_native_param_t fr_native_ble_gatt_client_write_params[] = {
    {"connection", FR_NATIVE_VALUE_HANDLE},
    {"attribute", FR_NATIVE_VALUE_INT},
    {"data", FR_NATIVE_VALUE_TEXT_OR_BYTES},
    {"with_response", FR_NATIVE_VALUE_INT},
    {"timeout_ms", FR_NATIVE_VALUE_INT},
};

static const fr_native_signature_t fr_native_ble_gatt_client_write_signature = {
    .params = fr_native_ble_gatt_client_write_params,
    .arg_count = 5,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "write one short remote characteristic value",
};

static const fr_native_param_t fr_native_ble_gatt_client_subscribe_params[] = {
    {"connection", FR_NATIVE_VALUE_HANDLE},
    {"attribute", FR_NATIVE_VALUE_INT},
    {"mode", FR_NATIVE_VALUE_INT},
    {"timeout_ms", FR_NATIVE_VALUE_INT},
};

static const fr_native_signature_t
    fr_native_ble_gatt_client_subscribe_signature = {
        .params = fr_native_ble_gatt_client_subscribe_params,
        .arg_count = 4,
        .result = FR_NATIVE_VALUE_NIL,
        .help = "subscribe to remote notifications or indications",
};

static const fr_native_param_t
    fr_native_ble_gatt_client_unsubscribe_params[] = {
        {"connection", FR_NATIVE_VALUE_HANDLE},
        {"attribute", FR_NATIVE_VALUE_INT},
        {"timeout_ms", FR_NATIVE_VALUE_INT},
};

static const fr_native_signature_t
    fr_native_ble_gatt_client_unsubscribe_signature = {
        .params = fr_native_ble_gatt_client_unsubscribe_params,
        .arg_count = 3,
        .result = FR_NATIVE_VALUE_NIL,
        .help = "unsubscribe from one remote characteristic",
};

static const fr_native_signature_t
    fr_native_ble_gatt_next_notification_signature = {
        .params = NULL,
        .arg_count = 0,
        .result = FR_NATIVE_VALUE_ANY,
        .help = "advance to the next remote GATT notification",
};

static const fr_native_signature_t
    fr_native_ble_gatt_notification_attribute_signature = {
        .params = NULL,
        .arg_count = 0,
        .result = FR_NATIVE_VALUE_INT,
        .help = "return the current remote notification ATT handle",
};

static const fr_native_signature_t
    fr_native_ble_gatt_notification_data_signature = {
        .params = NULL,
        .arg_count = 0,
        .result = FR_NATIVE_VALUE_ANY,
        .help = "copy the current remote notification data",
};
#endif
#endif
#endif

#if FR_FEATURE_PWM
static const fr_native_param_t fr_native_pwm_open_params[] = {
    {"pin", FR_NATIVE_VALUE_INT},
    {"freq", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_pwm_open_signature = {
    .params = fr_native_pwm_open_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_HANDLE,
    .help = "open a PWM channel on a pin at a frequency in Hz",
};

static const fr_native_param_t fr_native_pwm_write_params[] = {
    {"handle", FR_NATIVE_VALUE_HANDLE},
    {"duty", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_pwm_write_signature = {
    .params = fr_native_pwm_write_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "set a PWM duty cycle in [0, 10000]",
};

static const fr_native_param_t fr_native_pwm_close_params[] = {
    {"handle", FR_NATIVE_VALUE_HANDLE},
};
static const fr_native_signature_t fr_native_pwm_close_signature = {
    .params = fr_native_pwm_close_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "release a PWM channel",
};
#endif

#if FR_FEATURE_I2C
static const fr_native_param_t fr_native_i2c_open_params[] = {
    {"port", FR_NATIVE_VALUE_INT},
    {"sda", FR_NATIVE_VALUE_INT},
    {"scl", FR_NATIVE_VALUE_INT},
    {"freq", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_i2c_open_signature = {
    .params = fr_native_i2c_open_params,
    .arg_count = 4,
    .result = FR_NATIVE_VALUE_HANDLE,
    .help = "open an i2c bus on a port at sda/scl pins and frequency",
};

static const fr_native_param_t fr_native_i2c_write_params[] = {
    {"bus", FR_NATIVE_VALUE_HANDLE},
    {"addr", FR_NATIVE_VALUE_INT},
    {"bytes", FR_NATIVE_VALUE_TEXT_OR_BYTES},
};
static const fr_native_signature_t fr_native_i2c_write_signature = {
    .params = fr_native_i2c_write_params,
    .arg_count = 3,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "write bytes to a 7-bit i2c address",
};

static const fr_native_param_t fr_native_i2c_read_params[] = {
    {"bus", FR_NATIVE_VALUE_HANDLE},
    {"addr", FR_NATIVE_VALUE_INT},
    {"count", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_i2c_read_signature = {
    .params = fr_native_i2c_read_params,
    .arg_count = 3,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "read count bytes from a 7-bit i2c address",
};

static const fr_native_param_t fr_native_i2c_close_params[] = {
    {"bus", FR_NATIVE_VALUE_HANDLE},
};
static const fr_native_signature_t fr_native_i2c_close_signature = {
    .params = fr_native_i2c_close_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "release an i2c bus",
};

static const fr_native_param_t fr_native_i2c_read_reg_params[] = {
    {"bus", FR_NATIVE_VALUE_HANDLE},
    {"addr", FR_NATIVE_VALUE_INT},
    {"reg", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_i2c_read_reg_signature = {
    .params = fr_native_i2c_read_reg_params,
    .arg_count = 3,
    .result = FR_NATIVE_VALUE_INT,
    .help = "read a one-byte register from a 7-bit i2c address",
};

static const fr_native_signature_t fr_native_i2c_read_reg16_signature = {
    .params = fr_native_i2c_read_reg_params,
    .arg_count = 3,
    .result = FR_NATIVE_VALUE_INT,
    .help = "read a big-endian 16-bit register from a 7-bit i2c address",
};

static const fr_native_param_t fr_native_i2c_write_reg_params[] = {
    {"bus", FR_NATIVE_VALUE_HANDLE},
    {"addr", FR_NATIVE_VALUE_INT},
    {"reg", FR_NATIVE_VALUE_INT},
    {"value", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_i2c_write_reg_signature = {
    .params = fr_native_i2c_write_reg_params,
    .arg_count = 4,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "write a one-byte register at a 7-bit i2c address",
};

static const fr_native_signature_t fr_native_i2c_write_reg16_signature = {
    .params = fr_native_i2c_write_reg_params,
    .arg_count = 4,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "write a big-endian 16-bit register at a 7-bit i2c address",
};
#endif

#if FR_FEATURE_NET
static const fr_native_param_t fr_native_wifi_save_params[] = {
    {"ssid", FR_NATIVE_VALUE_TEXT},
    {"pass", FR_NATIVE_VALUE_SECRET_TEXT},
};
static const fr_native_signature_t fr_native_wifi_save_signature = {
    .params = fr_native_wifi_save_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "store wifi credentials in the frothy_wifi nvs namespace",
};

static const fr_native_signature_t fr_native_wifi_connect_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "connect to wifi using stored credentials",
};

static const fr_native_signature_t fr_native_wifi_ready_p_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "true when wifi is connected",
};

static const fr_native_param_t fr_native_http_get_params[] = {
    {"url", FR_NATIVE_VALUE_TEXT},
};
static const fr_native_signature_t fr_native_http_get_signature = {
    .params = fr_native_http_get_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "fetch a url and return the body up to the http body cap",
};

static const fr_native_param_t fr_native_tcp_open_params[] = {
    {"host", FR_NATIVE_VALUE_TEXT},
    {"port", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_tcp_open_signature = {
    .params = fr_native_tcp_open_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_HANDLE,
    .help = "open a tcp connection to host:port",
};

static const fr_native_param_t fr_native_tcp_read_params[] = {
    {"sock", FR_NATIVE_VALUE_HANDLE},
    {"count", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_tcp_read_signature = {
    .params = fr_native_tcp_read_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "read up to count bytes from a tcp socket",
};

static const fr_native_param_t fr_native_tcp_write_params[] = {
    {"sock", FR_NATIVE_VALUE_HANDLE},
    {"bytes", FR_NATIVE_VALUE_TEXT_OR_BYTES},
};
static const fr_native_signature_t fr_native_tcp_write_signature = {
    .params = fr_native_tcp_write_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "send the raw bytes of a text or bytes value to a tcp socket",
};

static const fr_native_param_t fr_native_tcp_close_params[] = {
    {"sock", FR_NATIVE_VALUE_HANDLE},
};
static const fr_native_signature_t fr_native_tcp_close_signature = {
    .params = fr_native_tcp_close_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "close a tcp socket and release the handle",
};

static const fr_native_param_t fr_native_tcp_bytes_ready_p_params[] = {
    {"sock", FR_NATIVE_VALUE_HANDLE},
};
static const fr_native_signature_t fr_native_tcp_bytes_ready_p_signature = {
    .params = fr_native_tcp_bytes_ready_p_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_INT,
    .help = "bytes available for immediate tcp.read",
};
#endif

#if FR_FEATURE_POWER
static const fr_native_param_t fr_native_watchdog_arm_params[] = {
    {"timeout_ms", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_watchdog_arm_signature = {
    .params = fr_native_watchdog_arm_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "arm the watchdog with a timeout in ms; window restarts now",
};

static const fr_native_signature_t fr_native_watchdog_feed_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "feed the armed watchdog; errors if not yet armed",
};

static const fr_native_param_t fr_native_sleep_deep_params[] = {
    {"ms", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_sleep_deep_signature = {
    .params = fr_native_sleep_deep_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "enter deep sleep for ms; chip cold-boots on wake",
};

static const fr_native_param_t fr_native_sleep_wake_on_gpio_params[] = {
    {"pin", FR_NATIVE_VALUE_INT},
    {"level", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_sleep_wake_on_gpio_signature = {
    .params = fr_native_sleep_wake_on_gpio_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "configure ext0 GPIO wake for the next sleep.deep",
};
#endif

#if FR_FEATURE_BYTES
static const fr_native_param_t fr_native_bytes_from_text_params[] = {
    {"text", FR_NATIVE_VALUE_TEXT},
};
static const fr_native_signature_t fr_native_bytes_from_text_signature = {
    .params = fr_native_bytes_from_text_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "copy a text value into a transient bytes buffer",
};

static const fr_native_param_t fr_native_bytes_from_byte_params[] = {
    {"byte", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_bytes_from_byte_signature = {
    .params = fr_native_bytes_from_byte_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "create a single-byte bytes buffer from a 0-255 int",
};

static const fr_native_param_t fr_native_bytes_from_int_params[] = {
    {"n", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_bytes_from_int_signature = {
    .params = fr_native_bytes_from_int_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "convert an int to its ASCII decimal representation as bytes",
};

static const fr_native_param_t fr_native_bytes_length_params[] = {
    {"buf", FR_NATIVE_VALUE_ANY},
};
static const fr_native_signature_t fr_native_bytes_length_signature = {
    .params = fr_native_bytes_length_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return the byte count of a bytes buffer",
};

static const fr_native_param_t fr_native_bytes_at_params[] = {
    {"buf", FR_NATIVE_VALUE_ANY},
    {"index", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_bytes_at_signature = {
    .params = fr_native_bytes_at_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return the byte at index as a 0-255 int",
};

static const fr_native_param_t fr_native_bytes_equals_p_params[] = {
    {"a", FR_NATIVE_VALUE_ANY},
    {"b", FR_NATIVE_VALUE_ANY},
};
static const fr_native_signature_t fr_native_bytes_equals_p_signature = {
    .params = fr_native_bytes_equals_p_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "true if two bytes buffers have equal contents",
};

static const fr_native_param_t fr_native_bytes_concat_params[] = {
    {"a", FR_NATIVE_VALUE_ANY},
    {"b", FR_NATIVE_VALUE_ANY},
};
static const fr_native_signature_t fr_native_bytes_concat_signature = {
    .params = fr_native_bytes_concat_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_ANY,
    .help = "concatenate two bytes buffers into a new bytes buffer",
};

static const fr_native_param_t fr_native_text_pack_params[] = {
    {"buf", FR_NATIVE_VALUE_ANY},
};
static const fr_native_signature_t fr_native_text_pack_signature = {
    .params = fr_native_text_pack_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_TEXT,
    .help = "copy a bytes buffer into the persistent text pool",
};
#endif

#if FR_FEATURE_TEXT && FR_FEATURE_REPL
static const fr_native_param_t fr_native_print_params[] = {
    {"value", FR_NATIVE_VALUE_TEXT_OR_BYTES},
};
static const fr_native_signature_t fr_native_print_signature = {
    .params = fr_native_print_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "write raw text or bytes to the console output",
};
#endif

#if FR_FEATURE_MATH
static const fr_native_param_t fr_native_abs_params[] = {
    {"x", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_abs_signature = {
    .params = fr_native_abs_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return the absolute value of an int",
};

static const fr_native_param_t fr_native_min_params[] = {
    {"a", FR_NATIVE_VALUE_INT},
    {"b", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_min_signature = {
    .params = fr_native_min_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return the smaller of two ints",
};

static const fr_native_param_t fr_native_max_params[] = {
    {"a", FR_NATIVE_VALUE_INT},
    {"b", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_max_signature = {
    .params = fr_native_max_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return the larger of two ints",
};

static const fr_native_param_t fr_native_clamp_params[] = {
    {"x", FR_NATIVE_VALUE_INT},
    {"lo", FR_NATIVE_VALUE_INT},
    {"hi", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_clamp_signature = {
    .params = fr_native_clamp_params,
    .arg_count = 3,
    .result = FR_NATIVE_VALUE_INT,
    .help = "clamp x to the inclusive range [lo, hi]",
};

static const fr_native_param_t fr_native_map_params[] = {
    {"x", FR_NATIVE_VALUE_INT},
    {"in_lo", FR_NATIVE_VALUE_INT},
    {"in_hi", FR_NATIVE_VALUE_INT},
    {"out_lo", FR_NATIVE_VALUE_INT},
    {"out_hi", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_map_signature = {
    .params = fr_native_map_params,
    .arg_count = 5,
    .result = FR_NATIVE_VALUE_INT,
    .help = "linearly remap x from one range to another",
};

static const fr_native_param_t fr_native_mod_params[] = {
    {"a", FR_NATIVE_VALUE_INT},
    {"b", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_mod_signature = {
    .params = fr_native_mod_params,
    .arg_count = 2,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return a modulo b (C truncating semantics)",
};

static const fr_native_param_t fr_native_sqrt_params[] = {
    {"x", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_sqrt_signature = {
    .params = fr_native_sqrt_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return the floor square root of a nonnegative int",
};
#endif

#if FR_FEATURE_RANDOM
static const fr_native_signature_t fr_native_random_next_signature = {
    .params = NULL,
    .arg_count = 0,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return the next pseudo-random nonnegative int",
};

static const fr_native_param_t fr_native_random_below_params[] = {
    {"limit", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_random_below_signature = {
    .params = fr_native_random_below_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_INT,
    .help = "return a pseudo-random int in [0, limit)",
};

static const fr_native_param_t fr_native_random_seed_params[] = {
    {"seed", FR_NATIVE_VALUE_INT},
};
static const fr_native_signature_t fr_native_random_seed_signature = {
    .params = fr_native_random_seed_params,
    .arg_count = 1,
    .result = FR_NATIVE_VALUE_NIL,
    .help = "seed the pseudo-random generator",
};
#endif
#endif

#if FR_FEATURE_UART
enum {
  FR_TARGET_TAGGED_BAUD_9600 = FR_TAGGED_INT_LITERAL(FR_UART_BAUD_9600),
  FR_TARGET_TAGGED_BAUD_19200 = FR_TAGGED_INT_LITERAL(FR_UART_BAUD_19200),
  FR_TARGET_TAGGED_BAUD_38400 = FR_TAGGED_INT_LITERAL(FR_UART_BAUD_38400),
  FR_TARGET_TAGGED_BAUD_57600 = FR_TAGGED_INT_LITERAL(FR_UART_BAUD_57600),
  FR_TARGET_TAGGED_BAUD_115200 = FR_TAGGED_INT_LITERAL(FR_UART_BAUD_115200),
  FR_TARGET_TAGGED_BAUD_1200 = FR_TAGGED_INT_LITERAL(FR_UART_BAUD_1200),
};
#endif

#if FR_BLE_ENABLE_GATT_SERVER
enum {
  FR_TARGET_TAGGED_BLE_GATT_SERVICE =
      FR_TAGGED_INT_LITERAL(FR_BLE_GATT_KIND_SERVICE),
  FR_TARGET_TAGGED_BLE_GATT_CHARACTERISTIC =
      FR_TAGGED_INT_LITERAL(FR_BLE_GATT_KIND_CHARACTERISTIC),
  FR_TARGET_TAGGED_BLE_GATT_PRIMARY =
      FR_TAGGED_INT_LITERAL(FR_BLE_GATT_SERVICE_PRIMARY),
  FR_TARGET_TAGGED_BLE_GATT_SECONDARY =
      FR_TAGGED_INT_LITERAL(FR_BLE_GATT_SERVICE_SECONDARY),
  FR_TARGET_TAGGED_BLE_GATT_READ =
      FR_TAGGED_INT_LITERAL(FR_BLE_GATT_CHR_READ),
  FR_TARGET_TAGGED_BLE_GATT_WRITE =
      FR_TAGGED_INT_LITERAL(FR_BLE_GATT_CHR_WRITE),
  FR_TARGET_TAGGED_BLE_GATT_WRITE_COMMAND =
      FR_TAGGED_INT_LITERAL(FR_BLE_GATT_CHR_WRITE_COMMAND),
  FR_TARGET_TAGGED_BLE_GATT_NOTIFY =
      FR_TAGGED_INT_LITERAL(FR_BLE_GATT_CHR_NOTIFY),
  FR_TARGET_TAGGED_BLE_GATT_INDICATE =
      FR_TAGGED_INT_LITERAL(FR_BLE_GATT_CHR_INDICATE),
  FR_TARGET_TAGGED_BLE_GATT_READ_ENCRYPTED =
      FR_TAGGED_INT_LITERAL(FR_BLE_GATT_CHR_READ_ENCRYPTED),
  FR_TARGET_TAGGED_BLE_GATT_WRITE_ENCRYPTED =
      FR_TAGGED_INT_LITERAL(FR_BLE_GATT_CHR_WRITE_ENCRYPTED),
  FR_TARGET_TAGGED_BLE_GATT_READ_AUTHENTICATED =
      FR_TAGGED_INT_LITERAL(FR_BLE_GATT_CHR_READ_AUTHENTICATED),
  FR_TARGET_TAGGED_BLE_GATT_WRITE_AUTHENTICATED =
      FR_TAGGED_INT_LITERAL(FR_BLE_GATT_CHR_WRITE_AUTHENTICATED),
};
#endif

#if FR_BLE_ENABLE_GATT_CLIENT
enum {
  FR_TARGET_TAGGED_BLE_GATT_NOTIFICATIONS =
      FR_TAGGED_INT_LITERAL(FR_BLE_GATT_SUBSCRIBE_NOTIFICATIONS),
  FR_TARGET_TAGGED_BLE_GATT_INDICATIONS =
      FR_TAGGED_INT_LITERAL(FR_BLE_GATT_SUBSCRIBE_INDICATIONS),
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
        .native_signature = &fr_native_ms_signature,
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
        .native_signature = &fr_native_gpio_write_signature,
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
        .native_signature = &fr_native_gpio_read_signature,
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
        .native_signature = &fr_native_adc_above_signature,
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
        .native_signature = &fr_native_millis_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_MICROS,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "micros",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_micros,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_micros_signature,
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
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_uart_open_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_UART_OPEN_ON,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "uart.open-on",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_uart_open_on,
        .native_arity = 4,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_uart_open_on_signature,
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
    {
        .slot_id = FR_SLOT_BAUD_1200,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$baud_1200",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BAUD_1200,
    },
#endif
#if FR_FEATURE_RANDOM
    {
        .slot_id = FR_SLOT_RANDOM_NEXT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "random.next",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_random_next,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_random_next_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_RANDOM_BELOW,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "random.below",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_random_below,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_random_below_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_RANDOM_SEED,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "random.seed",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_random_seed,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_random_seed_signature,
#endif
    },
#endif
#if FR_FEATURE_PWM
    {
        .slot_id = FR_SLOT_PWM_OPEN,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pwm.open",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pwm_open,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_pwm_open_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_PWM_WRITE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pwm.write",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pwm_write,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_pwm_write_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_PWM_CLOSE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pwm.close",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pwm_close,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_pwm_close_signature,
#endif
    },
#endif
#if FR_FEATURE_I2C
    {
        .slot_id = FR_SLOT_I2C_OPEN,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "i2c.open",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_i2c_open,
        .native_arity = 4,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_i2c_open_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_I2C_WRITE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "i2c.write",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_i2c_write,
        .native_arity = 3,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_i2c_write_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_I2C_READ,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "i2c.read",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_i2c_read,
        .native_arity = 3,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_i2c_read_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_I2C_CLOSE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "i2c.close",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_i2c_close,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_i2c_close_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_I2C_READ_REG,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "i2c.read-reg",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_i2c_read_reg,
        .native_arity = 3,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_i2c_read_reg_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_I2C_READ_REG16,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "i2c.read-reg16",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_i2c_read_reg16,
        .native_arity = 3,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_i2c_read_reg16_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_I2C_WRITE_REG,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "i2c.write-reg",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_i2c_write_reg,
        .native_arity = 4,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_i2c_write_reg_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_I2C_WRITE_REG16,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "i2c.write-reg16",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_i2c_write_reg16,
        .native_arity = 4,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_i2c_write_reg16_signature,
#endif
    },
#endif
#if FR_FEATURE_NET
    {
        .slot_id = FR_SLOT_WIFI_SAVE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "wifi.save",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_wifi_save,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_wifi_save_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_WIFI_CONNECT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "wifi.connect",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_wifi_connect,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_wifi_connect_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_WIFI_READY_P,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "wifi.ready?",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_wifi_ready_p,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_wifi_ready_p_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_HTTP_GET,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "http.get",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_http_get,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_http_get_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TCP_OPEN,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "tcp.open",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_tcp_open,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_tcp_open_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TCP_READ,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "tcp.read",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_tcp_read,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_tcp_read_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TCP_WRITE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "tcp.write",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_tcp_write,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_tcp_write_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TCP_CLOSE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "tcp.close",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_tcp_close,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_tcp_close_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TCP_BYTES_READY_P,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "tcp.bytes-ready?",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_tcp_bytes_ready_p,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_tcp_bytes_ready_p_signature,
#endif
    },
#endif
#if FR_FEATURE_POWER
    {
        .slot_id = FR_SLOT_WATCHDOG_ARM,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "watchdog.arm",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_watchdog_arm,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_watchdog_arm_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_WATCHDOG_FEED,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "watchdog.feed",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_watchdog_feed,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_watchdog_feed_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_SLEEP_DEEP,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "sleep.deep",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_sleep_deep,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_sleep_deep_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_SLEEP_WAKE_ON_GPIO,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "sleep.wake-on-gpio",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_sleep_wake_on_gpio,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_sleep_wake_on_gpio_signature,
#endif
    },
#endif
#if FR_FEATURE_BYTES
    {
        .slot_id = FR_SLOT_BYTES_FROM_TEXT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "bytes.from-text",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_bytes_from_text,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_bytes_from_text_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BYTES_FROM_BYTE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "bytes.from-byte",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_bytes_from_byte,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_bytes_from_byte_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BYTES_FROM_INT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "bytes.from-int",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_bytes_from_int,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_bytes_from_int_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BYTES_LENGTH,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "bytes.length",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_bytes_length,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_bytes_length_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BYTES_AT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "bytes.at",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_bytes_at,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_bytes_at_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BYTES_EQUALS_P,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "bytes.equals?",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_bytes_equals_p,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_bytes_equals_p_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BYTES_CONCAT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "bytes.concat",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_bytes_concat,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_bytes_concat_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TEXT_PACK,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "text.pack",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_text_pack,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_text_pack_signature,
#endif
    },
#endif
#if FR_FEATURE_MATH
    {
        .slot_id = FR_SLOT_ABS,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "abs",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_abs,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_abs_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_MIN,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "min",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_min,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_min_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_MAX,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "max",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_max,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_max_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_CLAMP,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "clamp",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_clamp,
        .native_arity = 3,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_clamp_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_MAP,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "map",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_map,
        .native_arity = 5,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_map_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_MOD,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "mod",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_mod,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_mod_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_SQRT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "sqrt",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_sqrt,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_sqrt_signature,
#endif
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
        .native_signature = &fr_native_pad_pack_signature,
#endif
    },
#endif
#endif
#if FR_FEATURE_TEXT
    {
        .slot_id = FR_SLOT_TEXT_LENGTH,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "text.length",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_text_length,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_text_length_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TEXT_EQUALS_P,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "text.equals?",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_text_equals_p,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_text_equals_p_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TEXT_CONCAT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "text.concat",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_text_concat,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_text_concat_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TEXT_AT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "text.at",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_text_at,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_text_at_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TEXT_FROM_INT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "text.from-int",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_text_from_int,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_text_from_int_signature,
#endif
    },
#if FR_FEATURE_REPL
    {
        .slot_id = FR_SLOT_PRINT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "print",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_print,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_print_signature,
#endif
    },
#endif
#endif
    {
        .slot_id = FR_SLOT_EVENT_REGISTER,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "frothy.event-register",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_event_register,
        .native_arity = 4,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_event_register_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_EVENT_CANCEL,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "frothy.event-cancel",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_event_cancel,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_event_cancel_signature,
#endif
    },
#if FR_FEATURE_TRACE
    {
        .slot_id = FR_SLOT_TRACE_OPEN,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "trace.open",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_trace_open,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_trace_open_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TRACE_WATCH,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "trace.watch",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_trace_watch,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_trace_watch_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TRACE_ARM,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "trace.arm",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_trace_arm,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_trace_arm_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TRACE_WAIT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "trace.wait",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_trace_wait,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_trace_wait_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TRACE_STOP,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "trace.stop",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_trace_stop,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_trace_stop_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TRACE_COUNT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "trace.count",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_trace_count,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_trace_count_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TRACE_CHANNEL,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "trace.channel",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_trace_channel,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_trace_channel_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TRACE_LEVEL,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "trace.level",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_trace_level,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_trace_level_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TRACE_DELTA_NS,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "trace.delta-ns",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_trace_delta_ns,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_trace_delta_ns_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TRACE_COMPLETE_P,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "trace.complete?",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_trace_complete_p,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_trace_complete_p_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TRACE_DUMP,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "trace.dump",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_trace_dump,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_trace_dump_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_TRACE_CLOSE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "trace.close",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_trace_close,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_trace_close_signature,
#endif
    },
#endif
#if FR_FEATURE_PULSE
    {
        .slot_id = FR_SLOT_PULSE_OPEN,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pulse.open",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pulse_open,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_pulse_open_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_PULSE_ADD,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pulse.add",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pulse_add,
        .native_arity = 3,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_pulse_add_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_PULSE_CLEAR,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pulse.clear",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pulse_clear,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_pulse_clear_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_PULSE_COUNT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pulse.count",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pulse_count,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_pulse_count_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_PULSE_LEVEL,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pulse.level",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pulse_level,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_pulse_level_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_PULSE_DURATION_NS,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pulse.duration-ns",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pulse_duration_ns,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_pulse_duration_ns_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_PULSE_DUMP,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pulse.dump",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pulse_dump,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_pulse_dump_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_PULSE_PLAY,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pulse.play",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pulse_play,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_pulse_play_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_PULSE_CLOSE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "pulse.close",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_pulse_close,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_pulse_close_signature,
#endif
    },
#endif
#if FR_FEATURE_CONSOLE_ROUTING
    {
        .slot_id = FR_SLOT_CONSOLE_UART,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "console.uart",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_console_uart,
        .native_arity = 3,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_console_uart_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_CONSOLE_DEFAULT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "console.default",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_console_default,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_console_default_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_CONSOLE_INFO,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "console.info",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_console_info,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_console_info_signature,
#endif
    },
#endif
#if FR_FEATURE_BLE
    {
        .slot_id = FR_SLOT_BLE_ON,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.on",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_on,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_on_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_INFO,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.info",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_info,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_info_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_OFF,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.off",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_off,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_off_signature,
#endif
    },
#if FR_BLE_ENABLE_OBSERVER
    {
        .slot_id = FR_SLOT_BLE_SCAN_START,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.scan.start",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_scan_start,
        .native_arity = 5,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_scan_start_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_SCAN_STOP,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.scan.stop",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_scan_stop,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_scan_stop_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_SCAN_NEXT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.scan.next?",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_scan_next,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_scan_next_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_SCAN_RSSI,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.scan.rssi",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_scan_rssi,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_scan_rssi_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_SCAN_PEER,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.scan.peer",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_scan_peer,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_scan_peer_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_SCAN_FLAGS,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.scan.flags",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_scan_flags,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_scan_flags_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_SCAN_DATA,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.scan.data",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_scan_data,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_scan_data_signature,
#endif
    },
#endif
#if FR_BLE_ENABLE_BROADCASTER
    {
        .slot_id = FR_SLOT_BLE_ADVERTISE_START,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.advertise.start",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_advertise_start,
        .native_arity = 4,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_advertise_start_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_ADVERTISE_STOP,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.advertise.stop",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_advertise_stop,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_advertise_stop_signature,
#endif
    },
#endif
#if FR_BLE_ENABLE_CENTRAL
    {
        .slot_id = FR_SLOT_BLE_CONNECT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.connect",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_connect,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_connect_signature,
#endif
    },
#endif
#if FR_BLE_ENABLE_PERIPHERAL
    {
        .slot_id = FR_SLOT_BLE_ACCEPT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.accept",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_accept,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_accept_signature,
#endif
    },
#endif
#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
    {
        .slot_id = FR_SLOT_BLE_CONNECTION_READY,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.connection.ready?",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_connection_ready,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_connection_ready_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_CONNECTION_CLOSE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.connection.close",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_connection_close,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_connection_close_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_CONNECTION_INFO,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.connection.info",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_connection_info,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_connection_info_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_CONNECTION_RSSI,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.connection.rssi",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_connection_rssi,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_connection_rssi_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_CONNECTION_PARAMS,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.connection.params",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_connection_params,
        .native_arity = 5,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_connection_params_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_CONNECTION_MTU,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.connection.mtu",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_connection_mtu,
        .native_arity = 3,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_connection_mtu_signature,
#endif
    },
#endif
#if FR_BLE_ENABLE_GATT_SERVER
    {
        .slot_id = FR_SLOT_BLE_GATT_SERVICE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$ble.gatt.service",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BLE_GATT_SERVICE,
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_CHARACTERISTIC,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$ble.gatt.characteristic",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BLE_GATT_CHARACTERISTIC,
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_PRIMARY,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$ble.gatt.primary",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BLE_GATT_PRIMARY,
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_SECONDARY,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$ble.gatt.secondary",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BLE_GATT_SECONDARY,
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_READ,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$ble.gatt.read",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BLE_GATT_READ,
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_WRITE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$ble.gatt.write",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BLE_GATT_WRITE,
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_WRITE_COMMAND,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$ble.gatt.write-command",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BLE_GATT_WRITE_COMMAND,
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_NOTIFY_FLAG,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$ble.gatt.notify",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BLE_GATT_NOTIFY,
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_INDICATE_FLAG,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$ble.gatt.indicate",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BLE_GATT_INDICATE,
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_READ_ENCRYPTED,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$ble.gatt.read-encrypted",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BLE_GATT_READ_ENCRYPTED,
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_WRITE_ENCRYPTED,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$ble.gatt.write-encrypted",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BLE_GATT_WRITE_ENCRYPTED,
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_READ_AUTHENTICATED,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$ble.gatt.read-authenticated",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BLE_GATT_READ_AUTHENTICATED,
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_WRITE_AUTHENTICATED,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$ble.gatt.write-authenticated",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BLE_GATT_WRITE_AUTHENTICATED,
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_INSTALL,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.install",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_install,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_install_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_INFO,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.info",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_info,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_info_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_SET,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.set",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_set,
        .native_arity = 2,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_set_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_GET,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.get",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_get,
        .native_arity = 1,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_get_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_NOTIFY,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.notify",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_notify,
        .native_arity = 3,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_notify_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_INDICATE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.indicate",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_indicate,
        .native_arity = 4,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_indicate_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_NEXT_WRITE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.next-write",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_next_write,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_next_write_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_WRITE_ATTRIBUTE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.write-attribute",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_write_attribute,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_write_attribute_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_WRITE_DATA,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.write-data",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_write_data,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_write_data_signature,
#endif
    },
#endif
#if FR_BLE_ENABLE_GATT_CLIENT && !FR_BLE_ENABLE_GATT_SERVER
    {
        .slot_id = FR_SLOT_BLE_GATT_INFO,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.info",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_info,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_info_signature,
#endif
    },
#endif
#if FR_BLE_ENABLE_GATT_CLIENT
    {
        .slot_id = FR_SLOT_BLE_GATT_NOTIFICATIONS,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$ble.gatt.notifications",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BLE_GATT_NOTIFICATIONS,
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_INDICATIONS,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "$ble.gatt.indications",
#endif
        .kind = FR_BASE_DEF_LITERAL,
        .literal_tagged = FR_TARGET_TAGGED_BLE_GATT_INDICATIONS,
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_FIND,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.find",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_client_find,
        .native_arity = 4,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_client_find_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_CLIENT_READ,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.read",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_client_read,
        .native_arity = 3,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_client_read_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_CLIENT_WRITE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.write",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_client_write,
        .native_arity = 5,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_client_write_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_SUBSCRIBE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.subscribe",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_client_subscribe,
        .native_arity = 4,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_client_subscribe_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_UNSUBSCRIBE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.unsubscribe",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_client_unsubscribe,
        .native_arity = 3,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_client_unsubscribe_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_NEXT_NOTIFICATION,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.next-notification",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_next_notification,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_next_notification_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_NOTIFICATION_ATTRIBUTE,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.notification-attribute",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_notification_attribute,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature =
            &fr_native_ble_gatt_notification_attribute_signature,
#endif
    },
    {
        .slot_id = FR_SLOT_BLE_GATT_NOTIFICATION_DATA,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "ble.gatt.notification-data",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_ble_gatt_notification_data,
        .native_arity = 0,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_ble_gatt_notification_data_signature,
#endif
    },
#endif
#endif
#if FR_INCLUDE_TEST_NATIVES && FR_FEATURE_TEXT
    {
        .slot_id = FR_SLOT_FIRE_EVENT,
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
        .name = "frothy.fire-event",
#endif
        .kind = FR_BASE_DEF_NATIVE,
        .native_fn = fr_native_fire_event,
        .native_arity = 3,
#if FR_FEATURE_NATIVE_SIGNATURES
        .native_signature = &fr_native_fire_event_signature,
#endif
    },
#endif
};

const uint16_t fr_target_base_def_count =
    (uint16_t)(sizeof(fr_target_base_defs) / sizeof(fr_target_base_defs[0]));
