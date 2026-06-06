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

/* Return 0 when the platform can't report (host, AVR); not an error. */
fr_err_t fr_platform_heap_free(uint32_t *out_bytes);
fr_err_t fr_platform_heap_largest(uint32_t *out_bytes);

typedef uint8_t fr_event_kind_t;

enum {
  FR_EVENT_KIND_NONE = 0,
  FR_EVENT_KIND_GPIO_RISING = 1,
  FR_EVENT_KIND_GPIO_FALLING = 2,
  FR_EVENT_KIND_GPIO_CHANGES = 3,
  FR_EVENT_KIND_EVERY = 4,
  FR_EVENT_KIND_AFTER = 5,
  FR_EVENT_KIND_WIFI_DISCONNECTED = 6,
  FR_EVENT_KIND_WIFI_RECONNECTED = 7,
};

typedef struct fr_event_candidate_t {
  uint16_t binding_index;
  uint16_t generation;
  uint32_t timestamp_ms;
} fr_event_candidate_t;

fr_err_t fr_platform_event_gpio_install(fr_event_kind_t kind, uint16_t pin,
                                        uint16_t binding_index,
                                        uint16_t generation);
fr_err_t fr_platform_event_gpio_remove(uint16_t pin);
fr_err_t fr_platform_event_timer_install(fr_event_kind_t kind, uint32_t ms,
                                         uint16_t binding_index,
                                         uint16_t generation);
fr_err_t fr_platform_event_timer_remove(uint16_t binding_index);
fr_err_t fr_platform_event_drain(fr_event_candidate_t *out_events,
                                 uint8_t out_cap, uint8_t *out_count,
                                 uint32_t *overflow_delta);
/* Test-only injection. Host backs it with the same queue drain reads from;
 * other targets stub. Returns FR_ERR_CAPACITY when the test queue is full. */
fr_err_t fr_platform_event_post_test_candidate(uint16_t binding_index,
                                               uint16_t generation,
                                               uint32_t timestamp_ms);

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
#ifdef FR_HOST_TEST_HELPERS
/* Host PWM test fixture. drain returns recorded duty values in FIFO order and
 * empties the per-handle ring; pwm has no read path so there is no queue
 * analog. */
uint16_t fr_host_pwm_drain_writes(uint16_t platform_index, uint16_t *out_duties,
                                  uint16_t max_count);
#endif
#endif

#if FR_FEATURE_I2C
fr_err_t fr_platform_i2c_open(uint16_t port, uint16_t sda, uint16_t scl,
                              uint32_t freq, uint16_t *out_platform_index);
fr_err_t fr_platform_i2c_write(uint16_t platform_index, uint8_t addr,
                               const uint8_t *bytes, uint16_t length);
fr_err_t fr_platform_i2c_read(uint16_t platform_index, uint8_t addr,
                              uint8_t *bytes, uint16_t length);
/* Combined write-then-read with a repeated-start between phases. Either
 * length may be zero. Devices that latch on a register pointer require the
 * bus not to be released between the two phases. */
fr_err_t fr_platform_i2c_write_read(uint16_t platform_index, uint8_t addr,
                                    const uint8_t *wbytes, uint16_t wlength,
                                    uint8_t *rbytes, uint16_t rlength);
fr_err_t fr_platform_i2c_close(uint16_t platform_index);
#ifdef FR_HOST_TEST_HELPERS
/* Host test fixtures. drain returns recorded write-phase bytes in FIFO order
 * and empties the ring; queue appends canned bytes that subsequent
 * fr_platform_i2c_write_read read phases consume. */
uint16_t fr_host_i2c_drain_writes(uint16_t platform_index, uint8_t *out_bytes,
                                  uint16_t max_length);
void fr_host_i2c_queue_read(uint16_t platform_index, const uint8_t *bytes,
                            uint16_t length);
#endif
#endif

#if FR_FEATURE_PERSISTENCE
fr_err_t fr_platform_storage_read(uint8_t slot, uint16_t offset, uint8_t *bytes,
                                  uint16_t length);
fr_err_t fr_platform_storage_write(uint8_t slot, uint16_t offset,
                                   const uint8_t *bytes, uint16_t length);
fr_err_t fr_platform_storage_erase(uint8_t slot);

void fr_platform_storage_debug_reset(void);
#endif

#if FR_FEATURE_NET
fr_err_t fr_platform_wifi_save(const char *ssid, const char *pass);
/* Blocks until ready or the 30 s D8 budget; polls fr_platform_poll_interrupt
 * between waits so Ctrl-C still wins (D2). Returns FR_ERR_INTERRUPTED on
 * Ctrl-C, mirroring the `ms` native (targets/common/target_defs.c:54-61). */
fr_err_t fr_platform_wifi_connect(fr_runtime_t *runtime);
fr_err_t fr_platform_wifi_ready(bool *out_ready);

/* out_body buffer is caller-owned with size cap; out_length receives the byte
 * count written. Bodies larger than cap return FR_ERR_NET_TOO_LARGE with no
 * partial result (D5). */
fr_err_t fr_platform_http_get(const char *url, uint8_t *out_body, uint16_t cap,
                              uint16_t *out_length);

fr_err_t fr_platform_tcp_open(const char *host, uint16_t port,
                              uint16_t *out_platform_index);
/* Blocks until >=1 byte arrives, peer EOF, the 5s timeout, or Ctrl-C (D20).
 * out_length carries the bytes written; 0 with FR_OK means graceful EOF. */
fr_err_t fr_platform_tcp_read(uint16_t platform_index, uint16_t count,
                              uint8_t *out_bytes, uint16_t *out_length);
fr_err_t fr_platform_tcp_write(uint16_t platform_index, const uint8_t *bytes,
                               uint16_t length);
fr_err_t fr_platform_tcp_close(uint16_t platform_index);
fr_err_t fr_platform_tcp_bytes_ready(uint16_t platform_index,
                                     uint16_t *out_count);

#ifdef FR_HOST_TEST_HELPERS
/* Host net fixtures (D16). wifi_set_connected flips the stub ready state.
 * http_queue_response enqueues one response that the next fr_platform_http_get
 * consumes. tcp_drain_writes returns recorded bytes from a per-handle ring;
 * tcp_queue_read appends bytes the next fr_platform_tcp_read consumes. */
void fr_host_wifi_set_connected(bool connected);
void fr_host_http_queue_response(uint16_t status, const uint8_t *body,
                                 uint16_t length);
uint16_t fr_host_tcp_drain_writes(uint16_t platform_index, uint8_t *out_bytes,
                                  uint16_t max_length);
void fr_host_tcp_queue_read(uint16_t platform_index, const uint8_t *bytes,
                            uint16_t length);
#endif
#endif
