#pragma once
#include "persist_format.h"
#include "types.h"

#include <stdbool.h>

enum {
  FR_UART_BAUD_1200 = 1200,
  FR_UART_BAUD_9600 = 9600,
  FR_UART_BAUD_19200 = 19200,
  FR_UART_BAUD_38400 = 38400,
  FR_UART_BAUD_57600 = 57600,
  FR_UART_BAUD_115200 = 115200,
};

#if FR_FEATURE_CONSOLE_ROUTING
typedef enum fr_console_transport_t {
  FR_CONSOLE_TRANSPORT_HOST = 0,
  FR_CONSOLE_TRANSPORT_UART = 1,
  FR_CONSOLE_TRANSPORT_USB = 2,
} fr_console_transport_t;

typedef struct fr_console_route_t {
  fr_console_transport_t transport;
  uint16_t tx;
  uint16_t rx;
  uint32_t baud;
} fr_console_route_t;
#endif

enum {
  FR_SIGNAL_TICK_NS = 100,
  FR_SIGNAL_CLOCK_HZ = 10000000,
  FR_SIGNAL_MAX_TICKS = 10000000,
  FR_SIGNAL_MAX_SPAN_NS = 1000000000,
  FR_TRACE_CHANNEL_CAP = 3,
  FR_TRACE_EVENT_CAP = 256,
  FR_TRACE_WAIT_MAX_MS = 60000,
  FR_PULSE_SEGMENT_CAP = 256,
};

typedef enum fr_trace_state_t {
  FR_TRACE_CONFIGURING = 0,
  FR_TRACE_ARMED = 1,
  FR_TRACE_COMPLETE = 2,
} fr_trace_state_t;

typedef struct fr_trace_status_t {
  fr_trace_state_t state;
  uint16_t pins[FR_TRACE_CHANNEL_CAP];
  uint8_t channel_count;
  uint16_t event_count;
} fr_trace_status_t;

typedef struct fr_trace_event_t {
  uint8_t channel;
  uint8_t level;
  uint32_t delta_ns;
} fr_trace_event_t;

typedef struct fr_pulse_segment_t {
  uint8_t level;
  uint32_t duration_ns;
} fr_pulse_segment_t;

typedef struct fr_pulse_status_t {
  uint16_t pin;
  uint16_t segment_count;
  uint32_t total_ns;
  uint8_t idle;
} fr_pulse_status_t;

fr_err_t fr_platform_delay_ms(uint16_t ms);
fr_err_t fr_platform_millis(uint32_t *out_ms);
fr_err_t fr_platform_micros(uint32_t *out_us);
/* Let the platform scheduler run without changing Frothy program state. */
void fr_platform_yield(void);
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
#if FR_FEATURE_NET
/* D19: wifi events ride the same candidate queue as gpio and timer; the
 * platform tracks a binding per wifi kind because each kind has at most one
 * binding and there is no per-pin / per-period source dimension. */
fr_err_t fr_platform_event_wifi_install(fr_event_kind_t kind,
                                        uint16_t binding_index,
                                        uint16_t generation);
fr_err_t fr_platform_event_wifi_remove(uint16_t binding_index);
#endif
fr_err_t fr_platform_event_drain(fr_event_candidate_t *out_events,
                                 uint8_t out_cap, uint8_t *out_count,
                                 uint32_t *overflow_delta);
/* Test-only injection. Host backs it with the same queue drain reads from;
 * other targets stub. Returns FR_ERR_CAPACITY when the test queue is full. */
fr_err_t fr_platform_event_post_test_candidate(uint16_t binding_index,
                                               uint16_t generation,
                                               uint32_t timestamp_ms);
#ifdef FR_HOST_TEST_HELPERS
void fr_host_event_debug_fail_next_timer_install(void);
#endif

#if FR_FEATURE_UART
fr_err_t fr_platform_uart_open(uint16_t port, uint32_t baud,
                               uint16_t *out_platform_index);
fr_err_t fr_platform_uart_open_on(uint16_t port, uint16_t tx, uint16_t rx,
                                  uint32_t baud,
                                  uint16_t *out_platform_index);
fr_err_t fr_platform_uart_write_byte(uint16_t platform_index, uint8_t byte);
fr_err_t fr_platform_uart_read_byte(uint16_t platform_index, uint8_t *out_byte,
                                    bool *out_has_byte);
fr_err_t fr_platform_uart_available(uint16_t platform_index,
                                    uint16_t *out_count);
#endif

#if FR_FEATURE_CONSOLE_ROUTING
fr_err_t fr_platform_console_set_uart(uint16_t tx, uint16_t rx,
                                      uint32_t baud);
fr_err_t fr_platform_console_restore_default(void);
fr_err_t fr_platform_console_get_route(fr_console_route_t *out_route);
/* Wait on the board's fixed recovery inputs before saved code can replace the
 * default console. Official ESP32 boards accept Ctrl-C or a post-reset BOOT
 * press; holding GPIO0 through reset remains a ROM-boot gesture. */
fr_err_t fr_platform_recovery_requested(uint16_t window_ms,
                                        bool *out_requested);
#ifdef FR_HOST_TEST_HELPERS
void fr_host_console_reset(void);
void fr_host_request_recovery(void);
void fr_host_console_fail_next_switch(void);
#endif
#endif

#if FR_FEATURE_REPL
fr_err_t fr_platform_read_line(char *line, uint16_t cap, bool *out_eof);
fr_err_t fr_platform_write_text(const char *text);
/* Idle-servicing hook. The REPL registers a handler the platform calls while
 * read_line waits for the next byte, so timer and interrupt events fire at an
 * idle prompt instead of only while a program runs. A platform whose read
 * blocks with no poll loop (host stdin) treats registration as a no-op. */
typedef fr_err_t (*fr_platform_idle_fn)(void *ctx);
void fr_platform_set_idle_handler(fr_platform_idle_fn handler, void *ctx);
#endif

#if FR_FEATURE_REPL || FR_FEATURE_PAD
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

#if FR_FEATURE_TRACE
fr_err_t fr_platform_trace_open(uint16_t *out_platform_index);
fr_err_t fr_platform_trace_watch(uint16_t platform_index, uint16_t pin,
                                 uint8_t *out_channel);
fr_err_t fr_platform_trace_arm(uint16_t platform_index);
fr_err_t fr_platform_trace_stop(uint16_t platform_index);
fr_err_t fr_platform_trace_status(uint16_t platform_index,
                                  fr_trace_status_t *out_status);
fr_err_t fr_platform_trace_event(uint16_t platform_index,
                                 uint16_t event_index,
                                 fr_trace_event_t *out_event);
fr_err_t fr_platform_trace_close(uint16_t platform_index);
#ifdef FR_HOST_TEST_HELPERS
fr_err_t fr_host_trace_push_edge(uint16_t platform_index, uint8_t channel,
                                 uint8_t level, uint32_t delta_ns);
#endif
#endif

#if FR_FEATURE_PULSE
fr_err_t fr_platform_pulse_open(uint16_t pin, uint8_t idle,
                                uint16_t *out_platform_index);
fr_err_t fr_platform_pulse_add(uint16_t platform_index, uint8_t level,
                               uint32_t duration_ns,
                               uint16_t *out_segment_index);
fr_err_t fr_platform_pulse_clear(uint16_t platform_index);
fr_err_t fr_platform_pulse_status(uint16_t platform_index,
                                  fr_pulse_status_t *out_status);
fr_err_t fr_platform_pulse_segment(uint16_t platform_index,
                                   uint16_t segment_index,
                                   fr_pulse_segment_t *out_segment);
fr_err_t fr_platform_pulse_play(uint16_t platform_index);
fr_err_t fr_platform_pulse_close(uint16_t platform_index);
#endif

#if FR_FEATURE_PERSISTENCE
/* Read a committed persistence image by backend generation order. Index 0 is
 * the newest valid-envelope image; higher indexes walk older valid envelopes.
 * The platform owns torn-write atomicity, envelope CRC, and generation order. */
fr_err_t fr_platform_persist_read(uint8_t *bytes, uint16_t cap,
                                  uint16_t *out_length, uint8_t image_index);
/* Borrow the committed image bytes by backend generation order. The pointer is
 * valid until the next mount, unmount, clear, or target reset. */
fr_err_t fr_platform_persist_mount(uint8_t image_index,
                                   const uint8_t **out_bytes,
                                   uint16_t *out_length);
fr_err_t fr_platform_persist_mount_commit(void);
void fr_platform_persist_mount_discard(void);
void fr_platform_persist_unmount(void);
bool fr_platform_persist_pointer_is_mounted(const void *ptr, uint16_t length);
bool fr_platform_persist_code_pointer_is_direct(const void *ptr,
                                                uint16_t length);
fr_err_t fr_platform_persist_mounted_offset(const void *ptr, uint16_t length,
                                            uint16_t *out_offset);
fr_err_t fr_platform_persist_read_mounted(uint16_t offset, uint8_t *dst,
                                          uint16_t length);
/* Stream a replacement image into the inactive slot. The platform erases the
 * inactive slot on begin, appends payload bytes after the reserved S1 header
 * area, and writes the final stamped header last. Until finalize succeeds, the
 * previous committed image remains the newest good image. */
fr_err_t fr_platform_persist_stream_begin(void);
fr_err_t fr_platform_persist_stream_write(const uint8_t *bytes,
                                          uint16_t length);
fr_err_t fr_platform_persist_stream_finalize(
    const uint8_t header[FR_PERSIST_HEADER_BYTES]);
void fr_platform_persist_stream_abort(void);
fr_err_t fr_platform_persist_clear(void);

#ifdef FR_HOST_TEST_HELPERS
/* Host fault injection for durability tests. Corrupts the newest internally
 * stored image without exposing the backend's slot layout. */
fr_err_t fr_host_persist_debug_corrupt_newest(uint16_t offset, uint8_t value);
void fr_host_persist_debug_interrupt_next_header_write(void);
void fr_host_persist_debug_fail_next_mount_commit(void);
void fr_host_persist_debug_shadow_mounts(bool enabled);
void fr_host_persist_debug_direct_code_pointers(bool enabled);
#endif
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

/* T15b D17. host is a NUL-terminated DNS name or dotted-quad; the platform
 * resolves and connects under the D7 10 s budget. out_platform_index receives
 * the per-target TCP-handle slot index (0..FR_TCP_HANDLE_COUNT-1). runtime
 * carries the per-handle state array (D5) and is the Ctrl-C / interrupt
 * source the connect poll loop reads. */
fr_err_t fr_platform_tcp_open(fr_runtime_t *runtime, const char *host,
                              uint16_t port, uint16_t *out_platform_index);
/* D8: blocks until >=1 byte / EOF / 5 s / Ctrl-C. EOF surfaces as FR_OK with
 * *out_length == 0, not as an error. */
fr_err_t fr_platform_tcp_read(fr_runtime_t *runtime, uint16_t platform_index,
                              uint8_t *out_bytes, uint16_t cap,
                              uint16_t *out_length);
/* D9: blocks until all length bytes accepted by lwip, 5 s timeout, or an
 * error. length == 0 is a no-op returning FR_OK. */
fr_err_t fr_platform_tcp_write(fr_runtime_t *runtime, uint16_t platform_index,
                               const uint8_t *bytes, uint16_t length);
/* Routed by fr_platform_handle_close on FR_HANDLE_KIND_TCP; no runtime
 * because fr_platform_handle_close itself does not carry one. The platform
 * tracks the OS resource (lwip fd) in its own slot map so close does not
 * need runtime to find what to free. */
fr_err_t fr_platform_tcp_close(uint16_t platform_index);
/* D10: non-blocking receive-queue byte count. */
fr_err_t fr_platform_tcp_bytes_ready(fr_runtime_t *runtime,
                                     uint16_t platform_index,
                                     uint16_t *out_count);

#ifdef FR_HOST_TEST_HELPERS
/* Host net fixtures (D16). wifi_set_connected flips the stub ready state.
 * http_queue_response enqueues one response that the next fr_platform_http_get
 * consumes. wifi_fire_event posts a candidate for the bound kind so the unity
 * test can exercise wifi.disconnected / wifi.reconnected delivery. */
void fr_host_wifi_set_connected(bool connected);
void fr_host_http_queue_response(uint16_t status, const uint8_t *body,
                                 uint16_t length);
void fr_host_wifi_fire_event(fr_event_kind_t kind);
/* Clears credential buffers, ready flag, queued response, wifi slot table,
 * and the host event queue so Unity tests start from a known state. */
void fr_host_net_reset(void);

/* T15b D18. queue_response stages bytes for the slot the next tcp.open: will
 * pick up; absence routes that open to FR_ERR_NET_REFUSED. drain_writes
 * returns recorded tcp.write: bytes in FIFO order and empties the per-slot
 * ring. force_disconnect latches wifi-down on the slot so the next TCP op
 * surfaces FR_ERR_NET_DISCONNECTED and trips runtime->tcp_handles[i].failed
 * per D12 — no auto-close. */
void fr_host_tcp_queue_response(uint16_t handle_platform_index,
                                const uint8_t *bytes, uint16_t length);
fr_err_t fr_host_tcp_drain_writes(uint16_t handle_platform_index,
                                  uint8_t *out_bytes, uint16_t cap,
                                  uint16_t *out_length);
void fr_host_tcp_force_disconnect(uint16_t handle_platform_index);
#endif
#endif

#if FR_FEATURE_POWER
/* T14 D8/D9/D10/D11: timeout_ms is the caller-provided window. The kernel
 * shim validates the [1000, 60000] range and the not-armed feed case
 * (D9, D11), so the platform impl trusts its input. The ESP-IDF impl
 * reconfigures the Task WDT and subscribes the caller task once (D8). */
fr_err_t fr_platform_watchdog_arm(uint32_t timeout_ms);
fr_err_t fr_platform_watchdog_feed(void);

/* T14 D12: sleep.deep enters deep sleep for ms milliseconds. If a
 * wake-on-gpio config is pending, it is consumed; the chip cold-boots on
 * wake. ms == 0 with no pending wake returns FR_ERR_INVALID so the chip
 * never sleeps indefinitely. */
fr_err_t fr_platform_sleep_deep(uint32_t ms);
/* T14 D12: pin must be RTC-capable per the ESP32 ext0 list (0, 2, 4,
 * 12-15, 25-27, 32-39); else FR_ERR_INVALID. level is 0 or 1; other
 * values return FR_ERR_INVALID. */
fr_err_t fr_platform_sleep_wake_on_gpio(uint16_t pin, uint16_t level);

#ifdef FR_HOST_TEST_HELPERS
/* T14 D17 host fixtures. force_timeout simulates the WDT fire; the
 * kernel's armed flag stays the source of truth for the user model so
 * D19's arm and re-arm tests assert by return status. captures returns
 * the last sleep.deep args plus the pending GPIO config that was set by
 * the most recent sleep.wake-on-gpio: call. */
void fr_host_watchdog_force_timeout(void);
void fr_host_sleep_deep_captures(uint32_t *out_ms, uint16_t *out_pin,
                                 uint16_t *out_level);
#endif
#endif
