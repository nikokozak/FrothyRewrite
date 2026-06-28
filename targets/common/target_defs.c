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
  uint16_t port = 0;
  uint16_t rate_code = 0;
  fr_handle_ref_t ref = {0};
  fr_tagged_t handle = 0;
  uint16_t platform_index = 0;
  fr_err_t err = FR_OK;

  if (runtime == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_native_decode_u16(args, arg_count, 0, &port));
  FR_TRY(fr_native_decode_u16(args, arg_count, 1, &rate_code));

  FR_TRY(fr_handle_reserve(runtime, FR_HANDLE_KIND_UART, &ref, &handle));
  err = fr_platform_uart_open(port, rate_code, &platform_index);
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
  uint16_t rate_code = 0;
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
  FR_TRY(fr_native_decode_u16(args, arg_count, 3, &rate_code));

  FR_TRY(fr_handle_reserve(runtime, FR_HANDLE_KIND_UART, &ref, &handle));
  err = fr_platform_uart_open_on(port, tx, rx, rate_code, &platform_index);
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

#if FR_FEATURE_TEXT && FR_FEATURE_REPL
static fr_err_t fr_native_print(fr_runtime_t *runtime, const fr_tagged_t *args,
                                uint8_t arg_count, fr_tagged_t *out) {
  const uint8_t *bytes = NULL;
  uint16_t length = 0;

  if (runtime == NULL || args == NULL || arg_count != 1 || out == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_native_decode_text_or_bytes_view(runtime, args[0], &bytes,
                                             &length));
  FR_TRY(fr_platform_write_bytes(bytes, length));
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
  uint16_t now_ms = 0;

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
                                                   (uint32_t)now_ms));
      *out = fr_tagged_nil();
      return FR_OK;
    }
  }
  return FR_ERR_NOT_FOUND;
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
    .help = "read the millisecond clock since boot",
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
    {"pass", FR_NATIVE_VALUE_TEXT},
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
