#pragma once
#include "types.h"

#include <stdbool.h>

enum {
  FR_UART_RATE_9600 = 1,
  FR_UART_RATE_19200 = 2,
  FR_UART_RATE_38400 = 3,
  FR_UART_RATE_57600 = 4,
  FR_UART_RATE_115200 = 5,
};

fr_err_t fr_platform_delay_ms(uint16_t ms);
fr_err_t fr_platform_millis(uint16_t *out_ms);
fr_err_t fr_platform_gpio_mode(uint16_t pin, uint16_t mode);
fr_err_t fr_platform_gpio_write(uint16_t pin, uint16_t value);
fr_err_t fr_platform_gpio_read(uint16_t pin, uint16_t *out_value);
fr_err_t fr_platform_adc_read(uint16_t pin, uint16_t *out_value);
fr_err_t fr_platform_poll_interrupt(fr_runtime_t *runtime);
fr_err_t fr_platform_handle_close(fr_handle_kind_t kind,
                                  uint16_t platform_index);

#if FR_FEATURE_UART
fr_err_t fr_platform_uart_open(uint16_t port, uint16_t rate_code,
                               uint16_t *out_platform_index);
fr_err_t fr_platform_uart_open_on(uint16_t port, uint16_t tx, uint16_t rx,
                                  uint16_t rate_code,
                                  uint16_t *out_platform_index);
fr_err_t fr_platform_uart_write_byte(uint16_t platform_index, uint8_t byte);
fr_err_t fr_platform_uart_read_byte(uint16_t platform_index, uint8_t *out_byte,
                                    bool *out_has_byte);
fr_err_t fr_platform_uart_available(uint16_t platform_index,
                                    uint16_t *out_count);
#endif

#if FR_FEATURE_REPL
fr_err_t fr_platform_read_line(char *line, uint16_t cap, bool *out_eof);
fr_err_t fr_platform_write_text(const char *text);
#endif

#if FR_FEATURE_PAD
fr_err_t fr_platform_write_bytes(const uint8_t *bytes, uint16_t length);
#endif

#if FR_FEATURE_RANDOM
uint32_t fr_platform_random_next(void);
void fr_platform_random_seed(uint32_t seed);
#endif

#if FR_FEATURE_PWM
fr_err_t fr_platform_pwm_open(uint16_t pin, uint16_t freq,
                              uint16_t *out_platform_index);
fr_err_t fr_platform_pwm_write(uint16_t platform_index, uint16_t duty);
fr_err_t fr_platform_pwm_close(uint16_t platform_index);
#endif

#if FR_FEATURE_PERSISTENCE
fr_err_t fr_platform_storage_read(uint8_t slot, uint16_t offset, uint8_t *bytes,
                                  uint16_t length);
fr_err_t fr_platform_storage_write(uint8_t slot, uint16_t offset,
                                   const uint8_t *bytes, uint16_t length);
fr_err_t fr_platform_storage_erase(uint8_t slot);

void fr_platform_storage_debug_reset(void);
#endif
