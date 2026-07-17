#include "platform.h"

#include "handle.h"
#include "persist_format.h"
#include "runtime.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
  FR_HOST_MAX_PIN = 39,
  FR_HOST_ADC_MAX_PIN = 39,
#if FR_FEATURE_UART || FR_FEATURE_CONSOLE_ROUTING
  FR_HOST_UART_BAUD_MAX = 5000000,
#endif
#if FR_FEATURE_UART
  FR_HOST_UART_SCRIPT_LENGTH = 5,
#endif
};

static uint8_t fr_host_gpio_values[FR_HOST_MAX_PIN + 1];
static uint32_t fr_host_millis;

fr_err_t fr_platform_restart(void) { return FR_ERR_UNSUPPORTED; }

#if FR_FEATURE_CONSOLE_ROUTING
static fr_console_route_t fr_host_console_route = {
    .transport = FR_CONSOLE_TRANSPORT_HOST,
};
static bool fr_host_recovery_next;
static bool fr_host_console_fail_switch_next;

static bool fr_host_console_pin_conflict(uint16_t tx, uint16_t rx) {
  return fr_host_console_route.transport == FR_CONSOLE_TRANSPORT_UART &&
         (fr_host_console_route.tx == tx || fr_host_console_route.tx == rx ||
          fr_host_console_route.rx == tx || fr_host_console_route.rx == rx);
}
#endif

#if FR_FEATURE_TRACE
typedef struct fr_host_trace_edge_t {
  uint32_t tick;
  uint8_t channel;
  uint8_t level;
} fr_host_trace_edge_t;

typedef struct fr_host_trace_t {
  bool in_use;
  fr_trace_state_t state;
  uint16_t pins[FR_TRACE_CHANNEL_CAP];
  uint8_t channel_count;
  fr_host_trace_edge_t edges[FR_TRACE_EVENT_CAP];
  uint16_t event_count;
  bool has_first_edge;
  uint32_t first_edge_ms;
  uint32_t latest_tick;
} fr_host_trace_t;

static fr_host_trace_t fr_host_trace;

static fr_err_t fr_host_trace_entry(uint16_t platform_index,
                                    fr_host_trace_t **out_trace) {
  if (out_trace == NULL) {
    return FR_ERR_INVALID;
  }
  if (platform_index != 0 || !fr_host_trace.in_use) {
    return FR_ERR_HANDLE;
  }

  *out_trace = &fr_host_trace;
  return FR_OK;
}

static bool fr_host_trace_edge_after(const fr_host_trace_edge_t *a,
                                     const fr_host_trace_edge_t *b) {
  return a->tick > b->tick ||
         (a->tick == b->tick && a->channel > b->channel);
}

static void fr_host_trace_sort(fr_host_trace_t *trace) {
  for (uint16_t i = 1; i < trace->event_count; i++) {
    fr_host_trace_edge_t edge = trace->edges[i];
    uint16_t at = i;

    while (at > 0 && fr_host_trace_edge_after(&trace->edges[at - 1], &edge)) {
      trace->edges[at] = trace->edges[at - 1];
      at -= 1u;
    }
    trace->edges[at] = edge;
  }
}

static void fr_host_trace_finish(fr_host_trace_t *trace) {
  if (trace->state == FR_TRACE_COMPLETE) {
    return;
  }
  trace->state = FR_TRACE_COMPLETE;
  fr_host_trace_sort(trace);
}
#endif

#if FR_FEATURE_PULSE
typedef struct fr_host_pulse_t {
  bool in_use;
  uint16_t pin;
  uint8_t idle;
  fr_pulse_segment_t segments[FR_PULSE_SEGMENT_CAP];
  uint16_t segment_count;
  uint32_t total_ticks;
} fr_host_pulse_t;

static fr_host_pulse_t fr_host_pulse;

static fr_err_t fr_host_pulse_entry(uint16_t platform_index,
                                    fr_host_pulse_t **out_pulse) {
  if (out_pulse == NULL) {
    return FR_ERR_INVALID;
  }
  if (platform_index != 0 || !fr_host_pulse.in_use) {
    return FR_ERR_HANDLE;
  }

  *out_pulse = &fr_host_pulse;
  return FR_OK;
}
#endif

#if FR_FEATURE_PWM
enum {
  FR_HOST_PWM_RING_CAP = 8,
};

typedef struct fr_host_pwm_t {
  bool in_use;
  uint16_t pin;
  uint16_t freq;
  /* Recorded duty values for test assertion; oldest dropped on overflow. */
  uint16_t write_ring[FR_HOST_PWM_RING_CAP];
  uint8_t write_head;
  uint8_t write_count;
} fr_host_pwm_t;

static fr_host_pwm_t fr_host_pwms[FR_PROFILE_MAX_HANDLES];

static bool fr_host_pwm_pin_in_use(uint16_t pin) {
  for (uint16_t i = 0; i < FR_PROFILE_MAX_HANDLES; i++) {
    if (fr_host_pwms[i].in_use && fr_host_pwms[i].pin == pin) {
      return true;
    }
  }
  return false;
}

static fr_err_t fr_host_pwm_entry(uint16_t platform_index,
                                  fr_host_pwm_t **out_pwm) {
  if (out_pwm == NULL) {
    return FR_ERR_INVALID;
  }
  if (platform_index >= FR_PROFILE_MAX_HANDLES ||
      !fr_host_pwms[platform_index].in_use) {
    return FR_ERR_HANDLE;
  }

  *out_pwm = &fr_host_pwms[platform_index];
  return FR_OK;
}
#endif

#if FR_FEATURE_I2C
enum {
  FR_HOST_I2C_MAX_PORT = 1,
  FR_HOST_I2C_ADDR_MAX = 0x7F,
  FR_HOST_I2C_RING_CAP = 64,
};

typedef struct fr_host_i2c_t {
  bool in_use;
  uint16_t port;
  uint16_t sda;
  uint16_t scl;
  uint32_t freq;
  /* Recorded write-phase bytes for test assertion; oldest dropped on overflow. */
  uint8_t write_ring[FR_HOST_I2C_RING_CAP];
  uint8_t write_head;
  uint8_t write_count;
  /* Canned read-phase bytes pre-queued by tests. */
  uint8_t read_queue[FR_HOST_I2C_RING_CAP];
  uint8_t read_head;
  uint8_t read_count;
} fr_host_i2c_t;

static fr_host_i2c_t fr_host_i2cs[FR_PROFILE_MAX_HANDLES];

static bool fr_host_i2c_port_in_use(uint16_t port) {
  for (uint16_t i = 0; i < FR_PROFILE_MAX_HANDLES; i++) {
    if (fr_host_i2cs[i].in_use && fr_host_i2cs[i].port == port) {
      return true;
    }
  }
  return false;
}

static bool fr_host_i2c_pin_in_use(uint16_t sda, uint16_t scl) {
  for (uint16_t i = 0; i < FR_PROFILE_MAX_HANDLES; i++) {
    const fr_host_i2c_t *i2c = &fr_host_i2cs[i];

    if (!i2c->in_use) {
      continue;
    }
    if (i2c->sda == sda || i2c->sda == scl || i2c->scl == sda ||
        i2c->scl == scl) {
      return true;
    }
  }
  return false;
}

static fr_err_t fr_host_i2c_entry(uint16_t platform_index,
                                  fr_host_i2c_t **out_i2c) {
  if (out_i2c == NULL) {
    return FR_ERR_INVALID;
  }
  if (platform_index >= FR_PROFILE_MAX_HANDLES ||
      !fr_host_i2cs[platform_index].in_use) {
    return FR_ERR_HANDLE;
  }

  *out_i2c = &fr_host_i2cs[platform_index];
  return FR_OK;
}
#endif

#if FR_FEATURE_UART
enum {
  FR_HOST_UART_MAX_PORT = 7,
};

typedef struct fr_host_uart_t {
  bool in_use;
  bool custom_pins; /* true only for uart.open-on; tx/rx hold the chosen pins */
  uint16_t port;
  uint16_t tx;
  uint16_t rx;
  uint32_t baud;
  uint8_t read_index;
  uint8_t last_written;
  uint16_t write_count;
} fr_host_uart_t;

static fr_host_uart_t fr_host_uarts[FR_PROFILE_MAX_HANDLES];
static const uint8_t fr_host_uart_script[FR_HOST_UART_SCRIPT_LENGTH] = {
    'f', 'r', 'o', 't', 'h',
};

static bool fr_host_uart_baud_valid(uint32_t baud) {
  return baud > 0 && baud <= FR_HOST_UART_BAUD_MAX;
}

static bool fr_host_uart_port_in_use(uint16_t port) {
  for (uint16_t i = 0; i < FR_PROFILE_MAX_HANDLES; i++) {
    const fr_host_uart_t *uart = &fr_host_uarts[i];

    if (uart->in_use && uart->port == port) {
      return true;
    }
  }
  return false;
}

static fr_err_t fr_host_uart_entry(uint16_t platform_index,
                                   fr_host_uart_t **out_uart) {
  if (out_uart == NULL) {
    return FR_ERR_INVALID;
  }
  if (platform_index >= FR_PROFILE_MAX_HANDLES ||
      !fr_host_uarts[platform_index].in_use) {
    return FR_ERR_HANDLE;
  }

  *out_uart = &fr_host_uarts[platform_index];
  return FR_OK;
}
#endif

fr_err_t fr_platform_delay_ms(uint16_t ms) {
  fr_host_millis += ms;
  return FR_OK;
}

fr_err_t fr_platform_millis(uint32_t *out_ms) {
  if (out_ms == NULL) {
    return FR_ERR_INVALID;
  }

  *out_ms = fr_host_millis;
  return FR_OK;
}

fr_err_t fr_platform_micros(uint32_t *out_us) {
  if (out_us == NULL) {
    return FR_ERR_INVALID;
  }

  *out_us = fr_host_millis * 1000u;
  return FR_OK;
}

void fr_platform_yield(void) {}

fr_err_t fr_platform_gpio_mode(uint16_t pin, uint16_t mode) {
  if (pin > FR_HOST_MAX_PIN) {
    return FR_ERR_DOMAIN;
  }
  if (mode > 2) {
    return FR_ERR_DOMAIN;
  }
  return FR_OK;
}

fr_err_t fr_platform_gpio_write(uint16_t pin, uint16_t value) {
  if (pin > FR_HOST_MAX_PIN) {
    return FR_ERR_DOMAIN;
  }

  fr_host_gpio_values[pin] = value == 0 ? 0 : 1;
  return FR_OK;
}

fr_err_t fr_platform_gpio_read(uint16_t pin, uint16_t *out_value) {
  if (out_value == NULL) {
    return FR_ERR_INVALID;
  }
  if (pin > FR_HOST_MAX_PIN) {
    return FR_ERR_DOMAIN;
  }

  *out_value = fr_host_gpio_values[pin];
  return FR_OK;
}

fr_err_t fr_platform_adc_read(uint16_t pin, uint16_t *out_value) {
  if (out_value == NULL) {
    return FR_ERR_INVALID;
  }
  if (pin > 5 && (pin < 14 || pin > 19) &&
      (pin < 32 || pin > FR_HOST_ADC_MAX_PIN)) {
    return FR_ERR_DOMAIN;
  }

  *out_value = 512;
  return FR_OK;
}

fr_err_t fr_platform_poll_interrupt(fr_runtime_t *runtime) {
  (void)runtime;
  return FR_OK;
}

fr_err_t fr_platform_heap_free(uint32_t *out_bytes) {
  if (out_bytes == NULL) {
    return FR_ERR_INVALID;
  }
  *out_bytes = 0;
  return FR_OK;
}

fr_err_t fr_platform_heap_largest(uint32_t *out_bytes) {
  if (out_bytes == NULL) {
    return FR_ERR_INVALID;
  }
  *out_bytes = 0;
  return FR_OK;
}

fr_err_t fr_platform_handle_close(fr_handle_kind_t kind,
                                  uint16_t platform_index) {
#if FR_FEATURE_UART
  if (kind == FR_HANDLE_KIND_UART) {
    if (platform_index < FR_PROFILE_MAX_HANDLES) {
      memset(&fr_host_uarts[platform_index], 0,
             sizeof(fr_host_uarts[platform_index]));
    }
    return FR_OK;
  }
#endif
#if FR_FEATURE_PWM
  if (kind == FR_HANDLE_KIND_PWM) {
    return fr_platform_pwm_close(platform_index);
  }
#endif
#if FR_FEATURE_I2C
  if (kind == FR_HANDLE_KIND_I2C_BUS) {
    return fr_platform_i2c_close(platform_index);
  }
#endif
#if FR_FEATURE_TRACE
  if (kind == FR_HANDLE_KIND_TRACE) {
    return fr_platform_trace_close(platform_index);
  }
#endif
#if FR_FEATURE_PULSE
  if (kind == FR_HANDLE_KIND_PULSE) {
    return fr_platform_pulse_close(platform_index);
  }
#endif
#if FR_FEATURE_NET
  if (kind == FR_HANDLE_KIND_TCP) {
    return fr_platform_tcp_close(platform_index);
  }
#endif
#if FR_FEATURE_BLE && (FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL)
  if (kind == FR_HANDLE_KIND_BLE_CONNECTION) {
    return fr_platform_ble_connection_close(platform_index);
  }
#endif
  (void)kind;
  (void)platform_index;
  return FR_OK;
}

#if FR_FEATURE_TRACE
fr_err_t fr_platform_trace_open(uint16_t *out_platform_index) {
  if (out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_host_trace.in_use) {
    return FR_ERR_CAPACITY;
  }

  memset(&fr_host_trace, 0, sizeof(fr_host_trace));
  fr_host_trace.in_use = true;
  fr_host_trace.state = FR_TRACE_CONFIGURING;
  *out_platform_index = 0;
  return FR_OK;
}

fr_err_t fr_platform_trace_watch(uint16_t platform_index, uint16_t pin,
                                 uint8_t *out_channel) {
  fr_host_trace_t *trace = NULL;

  if (out_channel == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_trace_entry(platform_index, &trace));
  if (trace->state != FR_TRACE_CONFIGURING || pin > FR_HOST_MAX_PIN) {
    return FR_ERR_DOMAIN;
  }
  for (uint8_t i = 0; i < trace->channel_count; i++) {
    if (trace->pins[i] == pin) {
      return FR_ERR_DOMAIN;
    }
  }
  if (trace->channel_count >= FR_TRACE_CHANNEL_CAP) {
    return FR_ERR_CAPACITY;
  }

  *out_channel = trace->channel_count;
  trace->pins[trace->channel_count] = pin;
  trace->channel_count += 1u;
  return FR_OK;
}

fr_err_t fr_platform_trace_arm(uint16_t platform_index) {
  fr_host_trace_t *trace = NULL;

  FR_TRY(fr_host_trace_entry(platform_index, &trace));
  if ((trace->state != FR_TRACE_CONFIGURING &&
       trace->state != FR_TRACE_COMPLETE) ||
      trace->channel_count == 0) {
    return FR_ERR_DOMAIN;
  }

  trace->event_count = 0;
  trace->has_first_edge = false;
  trace->first_edge_ms = 0;
  trace->latest_tick = 0;
  trace->state = FR_TRACE_ARMED;
  return FR_OK;
}

fr_err_t fr_platform_trace_stop(uint16_t platform_index) {
  fr_host_trace_t *trace = NULL;

  FR_TRY(fr_host_trace_entry(platform_index, &trace));
  if (trace->state == FR_TRACE_CONFIGURING) {
    return FR_ERR_DOMAIN;
  }
  fr_host_trace_finish(trace);
  return FR_OK;
}

fr_err_t fr_platform_trace_status(uint16_t platform_index,
                                  fr_trace_status_t *out_status) {
  fr_host_trace_t *trace = NULL;

  if (out_status == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_trace_entry(platform_index, &trace));
  if (trace->state == FR_TRACE_ARMED && trace->has_first_edge &&
      (uint32_t)(fr_host_millis - trace->first_edge_ms) >= 1000u) {
    fr_host_trace_finish(trace);
  }

  *out_status = (fr_trace_status_t){
      .state = trace->state,
      .channel_count = trace->channel_count,
      .event_count = trace->event_count,
  };
  memcpy(out_status->pins, trace->pins, sizeof(out_status->pins));
  return FR_OK;
}

fr_err_t fr_platform_trace_event(uint16_t platform_index,
                                 uint16_t event_index,
                                 fr_trace_event_t *out_event) {
  fr_host_trace_t *trace = NULL;

  if (out_event == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_trace_entry(platform_index, &trace));
  if (trace->state != FR_TRACE_COMPLETE) {
    return FR_ERR_DOMAIN;
  }
  if (event_index >= trace->event_count) {
    return FR_ERR_RANGE;
  }

  *out_event = (fr_trace_event_t){
      .channel = trace->edges[event_index].channel,
      .level = trace->edges[event_index].level,
      .delta_ns = event_index == 0
                      ? 0
                      : (trace->edges[event_index].tick -
                         trace->edges[event_index - 1].tick) *
                            FR_SIGNAL_TICK_NS,
  };
  return FR_OK;
}

fr_err_t fr_platform_trace_close(uint16_t platform_index) {
  if (platform_index == 0) {
    memset(&fr_host_trace, 0, sizeof(fr_host_trace));
  }
  return FR_OK;
}

#ifdef FR_HOST_TEST_HELPERS
fr_err_t fr_host_trace_push_edge(uint16_t platform_index, uint8_t channel,
                                 uint8_t level, uint32_t delta_ns) {
  fr_host_trace_t *trace = NULL;
  uint32_t delta_ticks = 0;

  FR_TRY(fr_host_trace_entry(platform_index, &trace));
  if (trace->state == FR_TRACE_COMPLETE) {
    return FR_OK;
  }
  if (trace->state != FR_TRACE_ARMED || channel >= trace->channel_count ||
      level > 1 || delta_ns % FR_SIGNAL_TICK_NS != 0) {
    return FR_ERR_DOMAIN;
  }
  if (!trace->has_first_edge && delta_ns != 0) {
    return FR_ERR_DOMAIN;
  }

  delta_ticks = delta_ns / FR_SIGNAL_TICK_NS;
  if (trace->has_first_edge &&
      delta_ticks > FR_SIGNAL_MAX_TICKS - trace->latest_tick) {
    fr_host_trace_finish(trace);
    return FR_OK;
  }
  if (!trace->has_first_edge) {
    trace->has_first_edge = true;
    trace->first_edge_ms = fr_host_millis;
  } else {
    trace->latest_tick += delta_ticks;
  }

  trace->edges[trace->event_count] = (fr_host_trace_edge_t){
      .tick = trace->latest_tick,
      .channel = channel,
      .level = level,
  };
  trace->event_count += 1u;
  if (trace->event_count == FR_TRACE_EVENT_CAP ||
      trace->latest_tick == FR_SIGNAL_MAX_TICKS) {
    fr_host_trace_finish(trace);
  }
  return FR_OK;
}
#endif
#endif

#if FR_FEATURE_PULSE
fr_err_t fr_platform_pulse_open(uint16_t pin, uint8_t idle,
                                uint16_t *out_platform_index) {
  if (out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (pin > FR_HOST_MAX_PIN || idle > 1) {
    return FR_ERR_DOMAIN;
  }
  if (fr_host_pulse.in_use) {
    return FR_ERR_CAPACITY;
  }

  memset(&fr_host_pulse, 0, sizeof(fr_host_pulse));
  fr_host_pulse.in_use = true;
  fr_host_pulse.pin = pin;
  fr_host_pulse.idle = idle;
  *out_platform_index = 0;
  return FR_OK;
}

fr_err_t fr_platform_pulse_add(uint16_t platform_index, uint8_t level,
                               uint32_t duration_ns,
                               uint16_t *out_segment_index) {
  fr_host_pulse_t *pulse = NULL;
  uint32_t ticks = 0;

  if (out_segment_index == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_pulse_entry(platform_index, &pulse));
  if (level > 1 || duration_ns == 0 ||
      duration_ns % FR_SIGNAL_TICK_NS != 0) {
    return FR_ERR_DOMAIN;
  }
  ticks = duration_ns / FR_SIGNAL_TICK_NS;
  if (pulse->segment_count >= FR_PULSE_SEGMENT_CAP ||
      ticks > FR_SIGNAL_MAX_TICKS - pulse->total_ticks) {
    return FR_ERR_CAPACITY;
  }

  *out_segment_index = pulse->segment_count;
  pulse->segments[pulse->segment_count] = (fr_pulse_segment_t){
      .level = level,
      .duration_ns = duration_ns,
  };
  pulse->segment_count += 1u;
  pulse->total_ticks += ticks;
  return FR_OK;
}

fr_err_t fr_platform_pulse_clear(uint16_t platform_index) {
  fr_host_pulse_t *pulse = NULL;

  FR_TRY(fr_host_pulse_entry(platform_index, &pulse));
  pulse->segment_count = 0;
  pulse->total_ticks = 0;
  return FR_OK;
}

fr_err_t fr_platform_pulse_status(uint16_t platform_index,
                                  fr_pulse_status_t *out_status) {
  fr_host_pulse_t *pulse = NULL;

  if (out_status == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_pulse_entry(platform_index, &pulse));
  *out_status = (fr_pulse_status_t){
      .pin = pulse->pin,
      .segment_count = pulse->segment_count,
      .total_ns = pulse->total_ticks * FR_SIGNAL_TICK_NS,
      .idle = pulse->idle,
  };
  return FR_OK;
}

fr_err_t fr_platform_pulse_segment(uint16_t platform_index,
                                   uint16_t segment_index,
                                   fr_pulse_segment_t *out_segment) {
  fr_host_pulse_t *pulse = NULL;

  if (out_segment == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_pulse_entry(platform_index, &pulse));
  if (segment_index >= pulse->segment_count) {
    return FR_ERR_RANGE;
  }
  *out_segment = pulse->segments[segment_index];
  return FR_OK;
}

fr_err_t fr_platform_pulse_play(uint16_t platform_index) {
  fr_host_pulse_t *pulse = NULL;

  FR_TRY(fr_host_pulse_entry(platform_index, &pulse));
  if (pulse->segment_count == 0) {
    return FR_ERR_DOMAIN;
  }
  return FR_OK;
}

fr_err_t fr_platform_pulse_close(uint16_t platform_index) {
  if (platform_index == 0) {
    memset(&fr_host_pulse, 0, sizeof(fr_host_pulse));
  }
  return FR_OK;
}

#endif

#if FR_FEATURE_BLE
#if FR_BLE_ENABLE_OBSERVER
enum {
  FR_HOST_BLE_INTERVAL_MIN_MS = 3,
  FR_HOST_BLE_INTERVAL_MAX_MS = 10240,
  FR_HOST_BLE_RSSI_MIN = -127,
  FR_HOST_BLE_RSSI_MAX = 20,
};

#ifdef FR_HOST_TEST_HELPERS
enum {
  FR_HOST_BLE_REPORT_FLAGS =
      FR_BLE_REPORT_CONNECTABLE | FR_BLE_REPORT_SCANNABLE |
      FR_BLE_REPORT_DIRECTED | FR_BLE_REPORT_SCAN_RESPONSE |
      FR_BLE_REPORT_LEGACY,
};
#endif
#endif

#if FR_BLE_ENABLE_BROADCASTER
enum {
  FR_HOST_BLE_ADVERTISE_INTERVAL_MIN_MS = 20,
  FR_HOST_BLE_ADVERTISE_INTERVAL_MAX_MS = 10240,
};
#endif

#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
enum {
  FR_HOST_BLE_CONNECTION_INDEX = 0,
  FR_HOST_BLE_DEFAULT_INTERVAL_US = 30000,
  FR_HOST_BLE_DEFAULT_SUPERVISION_TIMEOUT_US = 4000000,
  FR_HOST_BLE_DEFAULT_MTU = 23,
  FR_HOST_BLE_DEFAULT_RSSI = -42,
};

typedef struct fr_host_ble_connection_t {
  fr_ble_connection_info_t info;
  fr_handle_ref_t runtime_ref;
  uint32_t generation;
  bool has_runtime_ref;
} fr_host_ble_connection_t;

#if FR_BLE_ENABLE_PERIPHERAL
typedef struct fr_host_ble_connection_notice_t {
  uint16_t platform_index;
  uint32_t generation;
} fr_host_ble_connection_notice_t;
#endif
#endif

#if FR_BLE_ENABLE_GATT_SERVER
typedef struct fr_host_ble_gatt_t {
  fr_ble_gatt_table_t table;
  uint8_t value_bytes[FR_BLE_GATT_VALUE_BYTES];
  uint32_t table_generation;

  fr_ble_gatt_subscription_t subscriptions[FR_BLE_GATT_CCCD_COUNT];
  uint8_t subscription_count;

  fr_ble_gatt_write_t writes[FR_BLE_GATT_WRITE_QUEUE_COUNT];
  uint8_t write_head;
  uint8_t write_count;
  uint8_t write_high_water;
  uint32_t write_overflow;
  uint32_t write_stale;
  uint32_t preaccept_write_rejected;
  bool current_write_valid;
  fr_ble_gatt_write_t current_write;

  bool indication_pending;
  int32_t last_att_error;
  int32_t last_platform_code;
} fr_host_ble_gatt_t;
#endif

#if FR_BLE_ENABLE_GATT_CLIENT
typedef struct fr_host_ble_gatt_client_cache_t {
  fr_ble_uuid_t service_uuid;
  fr_ble_uuid_t characteristic_uuid;
  uint32_t connection_generation;
  uint16_t value_handle;
  uint16_t cccd_handle;
  uint16_t properties;
  fr_ble_gatt_subscription_mode_t subscription_mode;
  bool valid;
} fr_host_ble_gatt_client_cache_t;

typedef struct fr_host_ble_gatt_client_t {
  fr_host_ble_gatt_client_cache_t cache[FR_BLE_GATT_CLIENT_CACHE_COUNT];
  uint8_t cache_count;

  fr_ble_gatt_notification_t
      notifications[FR_BLE_GATT_NOTIFICATION_QUEUE_COUNT];
  uint8_t notification_head;
  uint8_t notification_count;
  uint8_t notification_high_water;
  uint32_t notification_dropped;
  uint32_t notification_stale;
  bool current_notification_valid;
  fr_ble_gatt_notification_t current_notification;

  uint16_t service_match_count;
  uint16_t characteristic_match_count;
  int32_t last_att_error;
  int32_t last_platform_code;
} fr_host_ble_gatt_client_t;

typedef struct fr_host_ble_remote_gatt_t {
  fr_ble_uuid_t service_uuid;
  fr_ble_uuid_t characteristic_uuid;
  uint16_t value_handle;
  uint16_t cccd_handle;
  uint16_t properties;
  uint8_t value[FR_BLE_GATT_CLIENT_DATA_BYTES];
  uint8_t value_length;
} fr_host_ble_remote_gatt_t;
#endif

typedef struct fr_host_ble_state_t {
  fr_ble_radio_state_t radio_state;
#if FR_BLE_ENABLE_OBSERVER
  fr_ble_scan_state_t scan_state;
#endif
  uint32_t lifecycle_generation;
#if FR_BLE_ENABLE_OBSERVER
  uint32_t scan_generation;
#endif
  bool shutdown_in_progress;
  bool cleanup_required;
  uint32_t late_callback_count;

#if FR_BLE_ENABLE_OBSERVER
  uint16_t requested_interval_ms;
  uint16_t requested_window_ms;
  uint32_t actual_interval_us;
  uint32_t actual_window_us;
  int8_t minimum_rssi;
  bool active_scan;
  bool repeats;

  fr_ble_scan_report_t queue[FR_BLE_SCAN_QUEUE_COUNT];
  uint8_t head;
  uint8_t count;
  uint8_t high_water;
  uint32_t received;
  uint32_t accepted;
  uint32_t filtered_rssi;
  uint32_t dequeued;
  uint32_t dropped;
  uint32_t malformed;
  bool current_valid;
  fr_ble_scan_report_t current;
#endif

#if FR_BLE_ENABLE_BROADCASTER
  fr_ble_advertise_state_t advertise_state;
  uint16_t advertise_requested_interval_ms;
  uint32_t advertise_actual_interval_us;
  bool advertise_connectable;
  uint8_t advertising_data_length;
  uint8_t advertising_data[FR_BLE_ADVERTISEMENT_DATA_BYTES];
  uint8_t scan_response_data_length;
  uint8_t scan_response_data[FR_BLE_ADVERTISEMENT_DATA_BYTES];
  uint32_t advertise_starts;
  uint32_t advertise_stops;
#endif

#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
  fr_host_ble_connection_t connection;
  uint32_t connection_connects;
  uint32_t connection_accepts;
  uint32_t connection_disconnects;
  uint32_t incoming_rejected;
#endif
#if FR_BLE_ENABLE_PERIPHERAL
  fr_host_ble_connection_notice_t
      connection_notices[FR_BLE_CONNECTION_NOTICE_COUNT];
  uint8_t connection_notice_head;
  uint8_t connection_notice_count;
#endif
#if FR_BLE_ENABLE_GATT_SERVER
  fr_host_ble_gatt_t gatt;
#endif
#if FR_BLE_ENABLE_GATT_CLIENT
  fr_host_ble_gatt_client_t gatt_client;
  fr_host_ble_remote_gatt_t remote_gatt;
#endif

  fr_ble_operation_t last_operation;
  fr_err_t last_result;
  int32_t last_platform_code;
  int32_t last_protocol_reason;
  uint32_t last_operation_ms;
  uint32_t reset_count;

#ifdef FR_HOST_TEST_HELPERS
  fr_err_t fail_next_on;
#if FR_BLE_ENABLE_OBSERVER
  fr_err_t fail_next_scan_start;
#endif
  bool timeout_next_on;
#if FR_BLE_ENABLE_OBSERVER
  bool timeout_next_scan_stop;
#endif
#if FR_BLE_ENABLE_GATT_SERVER
  bool timeout_next_indication;
#endif
  int32_t fail_next_on_raw_code;
#if FR_BLE_ENABLE_OBSERVER
  int32_t fail_next_scan_start_raw_code;
#endif
#endif
} fr_host_ble_state_t;

static fr_host_ble_state_t fr_host_ble;
static const uint8_t fr_host_ble_own_address[6] = {0xaa, 0xbb, 0xcc,
                                                   0xdd, 0xee, 0xff};

static uint8_t fr_host_ble_roles(void) {
  uint8_t roles = 0;

#if FR_BLE_ENABLE_OBSERVER
  roles |= FR_BLE_ROLE_OBSERVER;
#endif
#if FR_BLE_ENABLE_BROADCASTER
  roles |= FR_BLE_ROLE_BROADCASTER;
#endif
#if FR_BLE_ENABLE_CENTRAL
  roles |= FR_BLE_ROLE_CENTRAL;
#endif
#if FR_BLE_ENABLE_PERIPHERAL
  roles |= FR_BLE_ROLE_PERIPHERAL;
#endif
  return roles;
}

static void fr_host_ble_record(fr_ble_operation_t operation, fr_err_t result,
                               int32_t platform_code,
                               int32_t protocol_reason) {
  fr_host_ble.last_operation = operation;
  fr_host_ble.last_result = result;
  fr_host_ble.last_platform_code = platform_code;
  fr_host_ble.last_protocol_reason = protocol_reason;
  fr_host_ble.last_operation_ms = fr_host_millis;
}

#if FR_BLE_ENABLE_GATT_SERVER
enum {
  FR_HOST_BLE_ATT_INVALID_HANDLE = 0x01,
  FR_HOST_BLE_ATT_WRITE_NOT_PERMITTED = 0x03,
  FR_HOST_BLE_ATT_INVALID_VALUE_LENGTH = 0x0d,
  FR_HOST_BLE_ATT_INSUFFICIENT_RESOURCES = 0x11,
};

static fr_ble_gatt_characteristic_row_t *
fr_host_ble_gatt_characteristic(uint16_t attribute_id) {
  for (uint16_t i = 0; i < fr_host_ble.gatt.table.characteristic_count; i++) {
    if (fr_host_ble.gatt.table.characteristics[i].attribute_id ==
        attribute_id) {
      return &fr_host_ble.gatt.table.characteristics[i];
    }
  }
  return NULL;
}

static bool fr_host_ble_gatt_table_valid(const fr_ble_gatt_table_t *table) {
  uint16_t expected_value_offset = 0;
  uint16_t expected_characteristic = 0;

  if (table == NULL || table->service_count == 0 ||
      table->service_count > FR_BLE_GATT_SERVICE_COUNT ||
      table->characteristic_count > FR_BLE_GATT_CHARACTERISTIC_COUNT ||
      table->row_count != table->service_count + table->characteristic_count ||
      table->value_bytes_used > FR_BLE_GATT_VALUE_BYTES) {
    return false;
  }
  for (uint16_t i = 0; i < table->service_count; i++) {
    const fr_ble_gatt_service_row_t *service = &table->services[i];

    if (service->attribute_id >= table->row_count ||
        service->first_characteristic != expected_characteristic ||
        service->characteristic_count >
            table->characteristic_count - expected_characteristic) {
      return false;
    }
    expected_characteristic =
        (uint16_t)(expected_characteristic + service->characteristic_count);
  }
  if (expected_characteristic != table->characteristic_count) {
    return false;
  }
  for (uint16_t i = 0; i < table->characteristic_count; i++) {
    const fr_ble_gatt_characteristic_row_t *characteristic =
        &table->characteristics[i];

    if (characteristic->attribute_id >= table->row_count ||
        characteristic->value_offset != expected_value_offset ||
        characteristic->maximum_length >
            table->value_bytes_used - expected_value_offset) {
      return false;
    }
    expected_value_offset =
        (uint16_t)(expected_value_offset + characteristic->maximum_length);
  }
  return expected_value_offset == table->value_bytes_used;
}

static void fr_host_ble_gatt_clear_subscriptions(void) {
  memset(fr_host_ble.gatt.subscriptions, 0,
         sizeof(fr_host_ble.gatt.subscriptions));
  fr_host_ble.gatt.subscription_count = 0;
}

static void fr_host_ble_gatt_connection_closed(void) {
  fr_host_ble_gatt_clear_subscriptions();
  fr_host_ble.gatt.indication_pending = false;
}

static void fr_host_ble_gatt_radio_on(void) {
  for (uint16_t i = 0; i < fr_host_ble.gatt.table.characteristic_count; i++) {
    fr_host_ble.gatt.table.characteristics[i].target_value_handle =
        (uint16_t)(i + 1u);
  }
}

static void fr_host_ble_gatt_radio_off(void) {
  for (uint16_t i = 0; i < fr_host_ble.gatt.table.characteristic_count; i++) {
    fr_host_ble.gatt.table.characteristics[i].target_value_handle = 0;
  }
  fr_host_ble_gatt_connection_closed();
  fr_host_ble.gatt.write_stale += fr_host_ble.gatt.write_count;
  memset(fr_host_ble.gatt.writes, 0, sizeof(fr_host_ble.gatt.writes));
  fr_host_ble.gatt.write_head = 0;
  fr_host_ble.gatt.write_count = 0;
  fr_host_ble.gatt.current_write_valid = false;
  memset(&fr_host_ble.gatt.current_write, 0,
         sizeof(fr_host_ble.gatt.current_write));
}

static fr_ble_gatt_subscription_t *
fr_host_ble_gatt_subscription(uint16_t attribute_id) {
  for (uint8_t i = 0; i < fr_host_ble.gatt.subscription_count; i++) {
    fr_ble_gatt_subscription_t *subscription =
        &fr_host_ble.gatt.subscriptions[i];

    if (subscription->attribute_id == attribute_id &&
        subscription->connection_index == FR_HOST_BLE_CONNECTION_INDEX &&
        subscription->connection_generation ==
            fr_host_ble.connection.generation) {
      return subscription;
    }
  }
  return NULL;
}

#endif

#if FR_BLE_ENABLE_GATT_CLIENT
static void fr_host_ble_gatt_client_connection_closed(void);
#endif

#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
static void fr_host_ble_connection_begin(fr_ble_connection_role_t role,
                                         const uint8_t peer[7],
                                         fr_ble_connection_state_t state) {
  uint32_t generation = fr_host_ble.connection.generation + 1u;

  memset(&fr_host_ble.connection, 0, sizeof(fr_host_ble.connection));
  fr_host_ble.connection.generation = generation;
  fr_host_ble.connection.info = (fr_ble_connection_info_t){
      .state = state,
      .role = role,
      .peer_address_type = (fr_ble_address_type_t)peer[0],
      .interval_us = FR_HOST_BLE_DEFAULT_INTERVAL_US,
      .supervision_timeout_us = FR_HOST_BLE_DEFAULT_SUPERVISION_TIMEOUT_US,
      .mtu = FR_HOST_BLE_DEFAULT_MTU,
      .rssi_valid = true,
      .last_rssi = FR_HOST_BLE_DEFAULT_RSSI,
      .connected_at_ms = fr_host_millis,
  };
  memcpy(fr_host_ble.connection.info.peer_address, &peer[1],
         sizeof(fr_host_ble.connection.info.peer_address));
}

static void fr_host_ble_connection_free(void) {
  uint32_t generation = fr_host_ble.connection.generation;

#if FR_BLE_ENABLE_GATT_SERVER
  fr_host_ble_gatt_connection_closed();
#endif
#if FR_BLE_ENABLE_GATT_CLIENT
  fr_host_ble_gatt_client_connection_closed();
#endif
  memset(&fr_host_ble.connection, 0, sizeof(fr_host_ble.connection));
  fr_host_ble.connection.generation = generation;
  fr_host_ble.connection.info.state = FR_BLE_CONNECTION_FREE;
}

static fr_err_t fr_host_ble_connection_entry(
    uint16_t platform_index, fr_host_ble_connection_t **out_connection) {
  if (out_connection == NULL) {
    return FR_ERR_INVALID;
  }
  if (platform_index != FR_HOST_BLE_CONNECTION_INDEX ||
      fr_host_ble.connection.info.state == FR_BLE_CONNECTION_FREE) {
    return FR_ERR_HANDLE;
  }

  *out_connection = &fr_host_ble.connection;
  return FR_OK;
}

static bool fr_host_ble_connection_is_live(
    const fr_host_ble_connection_t *connection) {
  return connection->info.state == FR_BLE_CONNECTION_LIVE;
}

#ifdef FR_HOST_TEST_HELPERS
static void fr_host_ble_connection_mark_disconnected(int32_t reason) {
#if FR_BLE_ENABLE_GATT_SERVER
  fr_host_ble_gatt_connection_closed();
#endif
#if FR_BLE_ENABLE_GATT_CLIENT
  fr_host_ble_gatt_client_connection_closed();
#endif
  fr_host_ble.connection.info.state = FR_BLE_CONNECTION_DISCONNECTED;
  fr_host_ble.connection.info.rssi_valid = false;
  fr_host_ble.connection.info.last_reason = reason;
  fr_host_ble.connection.info.disconnected_at_ms = fr_host_millis;
  fr_host_ble.connection_disconnects += 1u;
}
#endif
#endif

#if FR_BLE_ENABLE_GATT_CLIENT
enum {
  FR_HOST_BLE_REMOTE_VALUE_HANDLE = 3,
  FR_HOST_BLE_REMOTE_CCCD_HANDLE = 4,
};

static bool fr_host_ble_uuid_equal(const fr_ble_uuid_t *left,
                                   const fr_ble_uuid_t *right) {
  size_t length = 0;

  if (left == NULL || right == NULL || left->kind != right->kind) {
    return false;
  }
  length = left->kind == FR_BLE_UUID_16 ? 2u : 16u;
  return memcmp(left->bytes, right->bytes, length) == 0;
}

static void fr_host_ble_remote_gatt_init(void) {
  fr_host_ble.remote_gatt = (fr_host_ble_remote_gatt_t){
      .service_uuid = {.kind = FR_BLE_UUID_16, .bytes = {0x18, 0x0f}},
      .characteristic_uuid = {.kind = FR_BLE_UUID_16,
                              .bytes = {0x2a, 0x19}},
      .value_handle = FR_HOST_BLE_REMOTE_VALUE_HANDLE,
      .cccd_handle = FR_HOST_BLE_REMOTE_CCCD_HANDLE,
      .properties = FR_BLE_GATT_CHR_READ | FR_BLE_GATT_CHR_WRITE |
                    FR_BLE_GATT_CHR_WRITE_COMMAND | FR_BLE_GATT_CHR_NOTIFY |
                    FR_BLE_GATT_CHR_INDICATE,
      .value = {42},
      .value_length = 1,
  };
}

static void fr_host_ble_gatt_client_connection_closed(void) {
  memset(fr_host_ble.gatt_client.cache, 0,
         sizeof(fr_host_ble.gatt_client.cache));
  fr_host_ble.gatt_client.cache_count = 0;
}

static void fr_host_ble_gatt_client_clear(void) {
  memset(&fr_host_ble.gatt_client, 0, sizeof(fr_host_ble.gatt_client));
}

static fr_err_t fr_host_ble_gatt_client_check(uint16_t connection_index,
                                              uint16_t timeout_ms) {
  fr_host_ble_connection_t *connection = NULL;

  if (timeout_ms == 0 || timeout_ms > FR_BLE_GATT_CLIENT_TIMEOUT_MAX_MS) {
    return FR_ERR_RANGE;
  }
  if (fr_host_ble.radio_state != FR_BLE_RADIO_READY) {
    return FR_ERR_BLE_NOT_READY;
  }
  FR_TRY(fr_host_ble_connection_entry(connection_index, &connection));
  if (!fr_host_ble_connection_is_live(connection) ||
      connection->info.role != FR_BLE_CONNECTION_ROLE_CENTRAL) {
    return FR_ERR_BLE_DISCONNECTED;
  }
  return FR_OK;
}

static fr_host_ble_gatt_client_cache_t *
fr_host_ble_gatt_client_cache(uint16_t attribute_handle) {
  for (uint8_t i = 0; i < FR_BLE_GATT_CLIENT_CACHE_COUNT; i++) {
    fr_host_ble_gatt_client_cache_t *entry =
        &fr_host_ble.gatt_client.cache[i];

    if (entry->valid && entry->value_handle == attribute_handle &&
        entry->connection_generation == fr_host_ble.connection.generation) {
      return entry;
    }
  }
  return NULL;
}

static uint8_t fr_host_ble_gatt_client_subscription_count(void) {
  uint8_t count = 0;

  for (uint8_t i = 0; i < FR_BLE_GATT_CLIENT_CACHE_COUNT; i++) {
    const fr_host_ble_gatt_client_cache_t *entry =
        &fr_host_ble.gatt_client.cache[i];

    if (entry->valid && entry->subscription_mode != 0) {
      count += 1u;
    }
  }
  return count;
}
#endif

#if FR_BLE_ENABLE_PERIPHERAL
static void fr_host_ble_notice_pop(void) {
  if (fr_host_ble.connection_notice_count == 0) {
    return;
  }
  fr_host_ble.connection_notice_head =
      (uint8_t)((fr_host_ble.connection_notice_head + 1u) %
                FR_BLE_CONNECTION_NOTICE_COUNT);
  fr_host_ble.connection_notice_count -= 1u;
}

static bool fr_host_ble_notice_current(void) {
  const fr_host_ble_connection_notice_t *notice = NULL;

  if (fr_host_ble.connection_notice_count == 0) {
    return false;
  }
  notice =
      &fr_host_ble.connection_notices[fr_host_ble.connection_notice_head];
  return notice->platform_index == FR_HOST_BLE_CONNECTION_INDEX &&
         notice->generation == fr_host_ble.connection.generation &&
         fr_host_ble.connection.info.state == FR_BLE_CONNECTION_PENDING;
}

static bool fr_host_ble_notice_find_current(void) {
  while (fr_host_ble.connection_notice_count > 0 &&
         !fr_host_ble_notice_current()) {
    fr_host_ble_notice_pop();
  }
  return fr_host_ble.connection_notice_count > 0;
}

static void fr_host_ble_notices_clear(void) {
  memset(fr_host_ble.connection_notices, 0,
         sizeof(fr_host_ble.connection_notices));
  fr_host_ble.connection_notice_head = 0;
  fr_host_ble.connection_notice_count = 0;
}
#endif

#if FR_BLE_ENABLE_OBSERVER
static void fr_host_ble_clear_reports(void) {
  memset(fr_host_ble.queue, 0, sizeof(fr_host_ble.queue));
  fr_host_ble.head = 0;
  fr_host_ble.count = 0;
  fr_host_ble.high_water = 0;
  fr_host_ble.received = 0;
  fr_host_ble.accepted = 0;
  fr_host_ble.filtered_rssi = 0;
  fr_host_ble.dequeued = 0;
  fr_host_ble.dropped = 0;
  fr_host_ble.malformed = 0;
  fr_host_ble.current_valid = false;
  memset(&fr_host_ble.current, 0, sizeof(fr_host_ble.current));
}

static void fr_host_ble_clear_parameters(void) {
  fr_host_ble.requested_interval_ms = 0;
  fr_host_ble.requested_window_ms = 0;
  fr_host_ble.actual_interval_us = 0;
  fr_host_ble.actual_window_us = 0;
  fr_host_ble.minimum_rssi = 0;
  fr_host_ble.active_scan = false;
  fr_host_ble.repeats = false;
}

#ifdef FR_HOST_TEST_HELPERS
static bool fr_host_ble_report_valid(const fr_ble_scan_report_t *report) {
  return report->address_type >= FR_BLE_ADDRESS_PUBLIC &&
         report->address_type <= FR_BLE_ADDRESS_RANDOM_ID &&
         report->rssi >= FR_HOST_BLE_RSSI_MIN &&
         report->rssi <= FR_HOST_BLE_RSSI_MAX &&
         report->data_length <= FR_BLE_SCAN_DATA_BYTES &&
         (report->flags & (uint8_t)~FR_HOST_BLE_REPORT_FLAGS) == 0;
}
#endif
#endif

const char *fr_platform_ble_backend_name(void) { return "host-fixture"; }

fr_err_t fr_platform_ble_on(fr_runtime_t *runtime) {
  fr_err_t failure = FR_OK;
  int32_t raw_code = 0;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_host_ble.radio_state == FR_BLE_RADIO_READY) {
    fr_host_ble_record(FR_BLE_OP_ON, FR_OK, 0, 0);
    return FR_OK;
  }
  if (fr_host_ble.radio_state == FR_BLE_RADIO_STARTING ||
      fr_host_ble.radio_state == FR_BLE_RADIO_STOPPING) {
    fr_host_ble_record(FR_BLE_OP_ON, FR_ERR_BLE_BUSY, 0, 0);
    return FR_ERR_BLE_BUSY;
  }

  fr_host_ble.lifecycle_generation += 1u;
  fr_host_ble.radio_state = FR_BLE_RADIO_STARTING;
  fr_host_ble.shutdown_in_progress = false;
  fr_host_ble.cleanup_required = false;

#ifdef FR_HOST_TEST_HELPERS
  failure = fr_host_ble.fail_next_on;
  fr_host_ble.fail_next_on = FR_OK;
  raw_code = fr_host_ble.fail_next_on_raw_code;
  fr_host_ble.fail_next_on_raw_code = 0;
  if (fr_host_ble.timeout_next_on) {
    fr_host_ble.timeout_next_on = false;
    fr_host_ble.radio_state = FR_BLE_RADIO_STOPPING;
    fr_host_ble.shutdown_in_progress = true;
    fr_host_ble.cleanup_required = true;
    fr_host_ble_record(FR_BLE_OP_ON, FR_ERR_BLE_TIMEOUT, raw_code, 0);
    return FR_ERR_BLE_TIMEOUT;
  }
#endif
  if (failure != FR_OK) {
    fr_host_ble.radio_state = FR_BLE_RADIO_FAILED;
    fr_host_ble_record(FR_BLE_OP_ON, failure, raw_code, 0);
    return failure;
  }

#if FR_BLE_ENABLE_GATT_SERVER
  fr_host_ble_gatt_radio_on();
#endif
  fr_host_ble.radio_state = FR_BLE_RADIO_READY;
  fr_host_ble_record(FR_BLE_OP_ON, FR_OK, 0, 0);
  return FR_OK;
}

fr_err_t fr_platform_ble_off(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

#if FR_BLE_ENABLE_OBSERVER
  if (fr_host_ble.scan_state != FR_BLE_SCAN_IDLE || fr_host_ble.count > 0 ||
      fr_host_ble.current_valid) {
    fr_host_ble.scan_generation += 1u;
  }
  fr_host_ble.scan_state = FR_BLE_SCAN_IDLE;
  fr_host_ble_clear_reports();
  fr_host_ble_clear_parameters();
#endif
#if FR_BLE_ENABLE_BROADCASTER
  fr_host_ble.advertise_state = FR_BLE_ADVERTISE_IDLE;
  fr_host_ble.advertise_requested_interval_ms = 0;
  fr_host_ble.advertise_actual_interval_us = 0;
  fr_host_ble.advertise_connectable = false;
  fr_host_ble.advertising_data_length = 0;
  memset(fr_host_ble.advertising_data, 0,
         sizeof(fr_host_ble.advertising_data));
  fr_host_ble.scan_response_data_length = 0;
  memset(fr_host_ble.scan_response_data, 0,
         sizeof(fr_host_ble.scan_response_data));
#endif
#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
  fr_host_ble_connection_free();
#endif
#if FR_BLE_ENABLE_PERIPHERAL
  fr_host_ble_notices_clear();
#endif
#if FR_BLE_ENABLE_GATT_SERVER
  fr_host_ble_gatt_radio_off();
#endif
#if FR_BLE_ENABLE_GATT_CLIENT
  fr_host_ble_gatt_client_clear();
#endif
  fr_host_ble.radio_state = FR_BLE_RADIO_OFF;
  fr_host_ble.shutdown_in_progress = false;
  fr_host_ble.cleanup_required = false;
  fr_host_ble_record(FR_BLE_OP_OFF, FR_OK, 0, 0);
  return FR_OK;
}

fr_err_t fr_platform_ble_project_clear(void) {
#if FR_BLE_ENABLE_OBSERVER
  if (fr_host_ble.scan_state != FR_BLE_SCAN_IDLE || fr_host_ble.count > 0 ||
      fr_host_ble.current_valid) {
    fr_host_ble.scan_generation += 1u;
  }
#endif
#if FR_BLE_ENABLE_BROADCASTER
  fr_host_ble.advertise_state = FR_BLE_ADVERTISE_IDLE;
  fr_host_ble.advertise_requested_interval_ms = 0;
  fr_host_ble.advertise_actual_interval_us = 0;
  fr_host_ble.advertise_connectable = false;
  fr_host_ble.advertising_data_length = 0;
  memset(fr_host_ble.advertising_data, 0,
         sizeof(fr_host_ble.advertising_data));
  fr_host_ble.scan_response_data_length = 0;
  memset(fr_host_ble.scan_response_data, 0,
         sizeof(fr_host_ble.scan_response_data));
  fr_host_ble.advertise_starts = 0;
  fr_host_ble.advertise_stops = 0;
#endif
#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
  fr_host_ble_connection_free();
  fr_host_ble.connection_connects = 0;
  fr_host_ble.connection_accepts = 0;
  fr_host_ble.connection_disconnects = 0;
  fr_host_ble.incoming_rejected = 0;
#endif
#if FR_BLE_ENABLE_PERIPHERAL
  fr_host_ble_notices_clear();
#endif
#if FR_BLE_ENABLE_GATT_SERVER
  memset(&fr_host_ble.gatt, 0, sizeof(fr_host_ble.gatt));
#endif
#if FR_BLE_ENABLE_GATT_CLIENT
  fr_host_ble_gatt_client_clear();
#endif
  fr_host_ble.radio_state = FR_BLE_RADIO_OFF;
#if FR_BLE_ENABLE_OBSERVER
  fr_host_ble.scan_state = FR_BLE_SCAN_IDLE;
#endif
  fr_host_ble.shutdown_in_progress = false;
  fr_host_ble.cleanup_required = false;
#if FR_BLE_ENABLE_OBSERVER
  fr_host_ble_clear_reports();
  fr_host_ble_clear_parameters();
#endif
  fr_host_ble.last_operation = FR_BLE_OP_NONE;
  fr_host_ble.last_result = FR_OK;
  fr_host_ble.last_platform_code = 0;
  fr_host_ble.last_protocol_reason = 0;
  fr_host_ble.last_operation_ms = fr_host_millis;
  return FR_OK;
}

fr_err_t fr_platform_ble_status(fr_ble_status_t *out_status) {
  if (out_status == NULL) {
    return FR_ERR_INVALID;
  }

  *out_status = (fr_ble_status_t){
      .radio_state = fr_host_ble.radio_state,
#if FR_BLE_ENABLE_OBSERVER
      .scan_state = fr_host_ble.scan_state,
#else
      .scan_state = FR_BLE_SCAN_IDLE,
#endif
#if FR_BLE_ENABLE_BROADCASTER
      .advertise_state = fr_host_ble.advertise_state,
#else
      .advertise_state = FR_BLE_ADVERTISE_IDLE,
#endif
      .roles = fr_host_ble_roles(),
      .coexistence_enabled = false,
      .lifecycle_generation = fr_host_ble.lifecycle_generation,
#if FR_BLE_ENABLE_OBSERVER
      .scan_generation = fr_host_ble.scan_generation,
#endif
      .shutdown_in_progress = fr_host_ble.shutdown_in_progress,
      .cleanup_required = fr_host_ble.cleanup_required,
      .late_callback_count = fr_host_ble.late_callback_count,
#if FR_BLE_ENABLE_OBSERVER
      .requested_interval_ms = fr_host_ble.requested_interval_ms,
      .requested_window_ms = fr_host_ble.requested_window_ms,
      .actual_interval_us = fr_host_ble.actual_interval_us,
      .actual_window_us = fr_host_ble.actual_window_us,
      .minimum_rssi = fr_host_ble.minimum_rssi,
      .active_scan = fr_host_ble.active_scan,
      .repeats = fr_host_ble.repeats,
      .queue_count = fr_host_ble.count,
      .queue_capacity = FR_BLE_SCAN_QUEUE_COUNT,
      .queue_high_water = fr_host_ble.high_water,
      .received = fr_host_ble.received,
      .accepted = fr_host_ble.accepted,
      .filtered_rssi = fr_host_ble.filtered_rssi,
      .dequeued = fr_host_ble.dequeued,
      .dropped = fr_host_ble.dropped,
      .malformed = fr_host_ble.malformed,
      .current_valid = fr_host_ble.current_valid,
      .current_rssi = fr_host_ble.current.rssi,
      .current_flags = fr_host_ble.current.flags,
      .current_data_length = fr_host_ble.current.data_length,
#endif
#if FR_BLE_ENABLE_BROADCASTER
      .advertise_requested_interval_ms =
          fr_host_ble.advertise_requested_interval_ms,
      .advertise_actual_interval_us = fr_host_ble.advertise_actual_interval_us,
      .advertise_connectable = fr_host_ble.advertise_connectable,
      .advertising_data_length = fr_host_ble.advertising_data_length,
      .scan_response_data_length = fr_host_ble.scan_response_data_length,
      .advertise_starts = fr_host_ble.advertise_starts,
      .advertise_stops = fr_host_ble.advertise_stops,
#endif
#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
      .connection_count =
          fr_host_ble.connection.info.state == FR_BLE_CONNECTION_FREE ? 0 : 1,
      .connection_capacity = FR_BLE_CONNECTION_COUNT,
#if FR_BLE_ENABLE_PERIPHERAL
      .pending_connection_count =
          fr_host_ble.connection.info.state == FR_BLE_CONNECTION_PENDING ? 1
                                                                         : 0,
      .connection_notice_count = fr_host_ble.connection_notice_count,
      .connection_notice_capacity = FR_BLE_CONNECTION_NOTICE_COUNT,
#endif
      .connection_connects = fr_host_ble.connection_connects,
      .connection_accepts = fr_host_ble.connection_accepts,
      .connection_disconnects = fr_host_ble.connection_disconnects,
      .incoming_rejected = fr_host_ble.incoming_rejected,
#endif
      .last_operation = fr_host_ble.last_operation,
      .last_result = fr_host_ble.last_result,
      .last_platform_code = fr_host_ble.last_platform_code,
      .last_protocol_reason = fr_host_ble.last_protocol_reason,
      .last_operation_ms = fr_host_ble.last_operation_ms,
      .reset_count = fr_host_ble.reset_count,
  };
  if (fr_host_ble.radio_state == FR_BLE_RADIO_READY) {
    out_status->own_address_type = FR_BLE_ADDRESS_PUBLIC;
    memcpy(out_status->own_address, fr_host_ble_own_address,
           sizeof(out_status->own_address));
    out_status->own_address_valid = true;
  }
  return FR_OK;
}

#if FR_BLE_ENABLE_OBSERVER
fr_err_t fr_platform_ble_scan_start(uint16_t interval_ms, uint16_t window_ms,
                                    bool active, bool repeats,
                                    int8_t minimum_rssi) {
  fr_err_t failure = FR_OK;
  int32_t raw_code = 0;

  if (interval_ms < FR_HOST_BLE_INTERVAL_MIN_MS ||
      interval_ms > FR_HOST_BLE_INTERVAL_MAX_MS ||
      window_ms < FR_HOST_BLE_INTERVAL_MIN_MS || window_ms > interval_ms ||
      minimum_rssi < FR_HOST_BLE_RSSI_MIN ||
      minimum_rssi > FR_HOST_BLE_RSSI_MAX) {
    return FR_ERR_RANGE;
  }
  if (fr_host_ble.radio_state == FR_BLE_RADIO_STARTING ||
      fr_host_ble.radio_state == FR_BLE_RADIO_STOPPING) {
    fr_host_ble_record(FR_BLE_OP_SCAN_START, FR_ERR_BLE_BUSY, 0, 0);
    return FR_ERR_BLE_BUSY;
  }
  if (fr_host_ble.radio_state != FR_BLE_RADIO_READY) {
    fr_host_ble_record(FR_BLE_OP_SCAN_START, FR_ERR_BLE_NOT_READY, 0, 0);
    return FR_ERR_BLE_NOT_READY;
  }
  if (fr_host_ble.scan_state != FR_BLE_SCAN_IDLE) {
    fr_host_ble_record(FR_BLE_OP_SCAN_START, FR_ERR_BLE_BUSY, 0, 0);
    return FR_ERR_BLE_BUSY;
  }
#if FR_BLE_ENABLE_BROADCASTER
  if (fr_host_ble.advertise_state != FR_BLE_ADVERTISE_IDLE) {
    fr_host_ble_record(FR_BLE_OP_SCAN_START, FR_ERR_BLE_BUSY, 0, 0);
    return FR_ERR_BLE_BUSY;
  }
#endif

  fr_host_ble_clear_reports();
  fr_host_ble.requested_interval_ms = interval_ms;
  fr_host_ble.requested_window_ms = window_ms;
  fr_host_ble.actual_interval_us = (uint32_t)interval_ms * 1000u;
  fr_host_ble.actual_window_us = (uint32_t)window_ms * 1000u;
  fr_host_ble.minimum_rssi = minimum_rssi;
  fr_host_ble.active_scan = active;
  fr_host_ble.repeats = repeats;
  fr_host_ble.scan_generation += 1u;

#ifdef FR_HOST_TEST_HELPERS
  failure = fr_host_ble.fail_next_scan_start;
  fr_host_ble.fail_next_scan_start = FR_OK;
  raw_code = fr_host_ble.fail_next_scan_start_raw_code;
  fr_host_ble.fail_next_scan_start_raw_code = 0;
#endif
  if (failure != FR_OK) {
    fr_host_ble.scan_state = FR_BLE_SCAN_IDLE;
    fr_host_ble_record(FR_BLE_OP_SCAN_START, failure, raw_code, 0);
    return failure;
  }

  fr_host_ble.scan_state = FR_BLE_SCAN_ACTIVE;
  fr_host_ble_record(FR_BLE_OP_SCAN_START, FR_OK, 0, 0);
  return FR_OK;
}

fr_err_t fr_platform_ble_scan_stop(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  fr_host_ble.current_valid = false;
  memset(&fr_host_ble.current, 0, sizeof(fr_host_ble.current));
  if (fr_host_ble.scan_state == FR_BLE_SCAN_IDLE) {
    fr_host_ble_record(FR_BLE_OP_SCAN_STOP, FR_OK, 0, 0);
    return FR_OK;
  }
#ifdef FR_HOST_TEST_HELPERS
  if (fr_host_ble.timeout_next_scan_stop) {
    fr_host_ble.timeout_next_scan_stop = false;
    fr_host_ble.scan_state = FR_BLE_SCAN_STOPPING;
    fr_host_ble_record(FR_BLE_OP_SCAN_STOP, FR_ERR_BLE_TIMEOUT, 0, 0);
    return FR_ERR_BLE_TIMEOUT;
  }
#endif
  fr_host_ble.scan_state = FR_BLE_SCAN_IDLE;
  fr_host_ble_record(FR_BLE_OP_SCAN_STOP, FR_OK, 0, 0);
  return FR_OK;
}

fr_err_t fr_platform_ble_scan_next(bool *out_has_report) {
  if (out_has_report == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_host_ble.count == 0) {
    fr_host_ble.current_valid = false;
    memset(&fr_host_ble.current, 0, sizeof(fr_host_ble.current));
    *out_has_report = false;
    return FR_OK;
  }

  fr_host_ble.current = fr_host_ble.queue[fr_host_ble.head];
  fr_host_ble.current_valid = true;
  fr_host_ble.head =
      (uint8_t)((fr_host_ble.head + 1u) % FR_BLE_SCAN_QUEUE_COUNT);
  fr_host_ble.count -= 1u;
  fr_host_ble.dequeued += 1u;
  *out_has_report = true;
  return FR_OK;
}

fr_err_t fr_platform_ble_scan_current(fr_ble_scan_report_t *out_report) {
  if (out_report == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_host_ble.current_valid) {
    return FR_ERR_NOT_FOUND;
  }
  *out_report = fr_host_ble.current;
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_BROADCASTER
fr_err_t fr_platform_ble_advertise_start(
    const uint8_t *advertising_data, uint8_t advertising_data_length,
    const uint8_t *scan_response_data, uint8_t scan_response_data_length,
    uint16_t interval_ms, bool connectable) {
  if ((advertising_data_length > 0 && advertising_data == NULL) ||
      (scan_response_data_length > 0 && scan_response_data == NULL)) {
    return FR_ERR_INVALID;
  }
  if (advertising_data_length > FR_BLE_ADVERTISEMENT_DATA_BYTES ||
      scan_response_data_length > FR_BLE_ADVERTISEMENT_DATA_BYTES) {
    return FR_ERR_CAPACITY;
  }
  if (interval_ms < FR_HOST_BLE_ADVERTISE_INTERVAL_MIN_MS ||
      interval_ms > FR_HOST_BLE_ADVERTISE_INTERVAL_MAX_MS) {
    return FR_ERR_RANGE;
  }
  if (fr_host_ble.radio_state != FR_BLE_RADIO_READY) {
    fr_host_ble_record(FR_BLE_OP_ADVERTISE_START, FR_ERR_BLE_NOT_READY, 0, 0);
    return FR_ERR_BLE_NOT_READY;
  }
#if FR_BLE_ENABLE_OBSERVER
  if (fr_host_ble.scan_state != FR_BLE_SCAN_IDLE) {
    fr_host_ble_record(FR_BLE_OP_ADVERTISE_START, FR_ERR_BLE_BUSY, 0, 0);
    return FR_ERR_BLE_BUSY;
  }
#endif
  if (fr_host_ble.advertise_state != FR_BLE_ADVERTISE_IDLE) {
    fr_host_ble_record(FR_BLE_OP_ADVERTISE_START, FR_ERR_BLE_BUSY, 0, 0);
    return FR_ERR_BLE_BUSY;
  }

  memset(fr_host_ble.advertising_data, 0,
         sizeof(fr_host_ble.advertising_data));
  if (advertising_data_length > 0) {
    memcpy(fr_host_ble.advertising_data, advertising_data,
           advertising_data_length);
  }
  memset(fr_host_ble.scan_response_data, 0,
         sizeof(fr_host_ble.scan_response_data));
  if (scan_response_data_length > 0) {
    memcpy(fr_host_ble.scan_response_data, scan_response_data,
           scan_response_data_length);
  }
  fr_host_ble.advertising_data_length = advertising_data_length;
  fr_host_ble.scan_response_data_length = scan_response_data_length;
  fr_host_ble.advertise_requested_interval_ms = interval_ms;
  fr_host_ble.advertise_actual_interval_us = (uint32_t)interval_ms * 1000u;
  fr_host_ble.advertise_connectable = connectable;
  fr_host_ble.advertise_state = FR_BLE_ADVERTISE_ACTIVE;
  fr_host_ble.advertise_starts += 1u;
  fr_host_ble_record(FR_BLE_OP_ADVERTISE_START, FR_OK, 0, 0);
  return FR_OK;
}

fr_err_t fr_platform_ble_advertise_stop(void) {
  if (fr_host_ble.advertise_state == FR_BLE_ADVERTISE_IDLE) {
    fr_host_ble_record(FR_BLE_OP_ADVERTISE_STOP, FR_OK, 0, 0);
    return FR_OK;
  }
  fr_host_ble.advertise_state = FR_BLE_ADVERTISE_IDLE;
  fr_host_ble.advertise_stops += 1u;
  fr_host_ble_record(FR_BLE_OP_ADVERTISE_STOP, FR_OK, 0, 0);
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_CENTRAL
fr_err_t fr_platform_ble_connect(fr_runtime_t *runtime, const uint8_t peer[7],
                                 uint16_t timeout_ms,
                                 fr_handle_ref_t runtime_ref,
                                 uint16_t *out_platform_index) {
  if (runtime == NULL || peer == NULL || out_platform_index == NULL ||
      peer[0] > FR_BLE_ADDRESS_RANDOM_ID) {
    return FR_ERR_INVALID;
  }
  if (timeout_ms == 0 || timeout_ms > FR_BLE_CONNECT_TIMEOUT_MAX_MS) {
    return FR_ERR_RANGE;
  }
  if (fr_host_ble.radio_state != FR_BLE_RADIO_READY) {
    fr_host_ble_record(FR_BLE_OP_CONNECT, FR_ERR_BLE_NOT_READY, 0, 0);
    return FR_ERR_BLE_NOT_READY;
  }
#if FR_BLE_ENABLE_OBSERVER
  if (fr_host_ble.scan_state != FR_BLE_SCAN_IDLE) {
    fr_host_ble_record(FR_BLE_OP_CONNECT, FR_ERR_BLE_BUSY, 0, 0);
    return FR_ERR_BLE_BUSY;
  }
#endif
#if FR_BLE_ENABLE_BROADCASTER
  if (fr_host_ble.advertise_state != FR_BLE_ADVERTISE_IDLE) {
    fr_host_ble_record(FR_BLE_OP_CONNECT, FR_ERR_BLE_BUSY, 0, 0);
    return FR_ERR_BLE_BUSY;
  }
#endif
  if (fr_host_ble.connection.info.state != FR_BLE_CONNECTION_FREE) {
    fr_host_ble_record(FR_BLE_OP_CONNECT, FR_ERR_CAPACITY, 0, 0);
    return FR_ERR_CAPACITY;
  }

  fr_host_ble_connection_begin(FR_BLE_CONNECTION_ROLE_CENTRAL, peer,
                               FR_BLE_CONNECTION_LIVE);
  fr_host_ble.connection.runtime_ref = runtime_ref;
  fr_host_ble.connection.has_runtime_ref = true;
  fr_host_ble.connection_connects += 1u;
  *out_platform_index = FR_HOST_BLE_CONNECTION_INDEX;
  fr_host_ble_record(FR_BLE_OP_CONNECT, FR_OK, 0, 0);
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_PERIPHERAL
fr_err_t fr_platform_ble_connection_pending(bool *out_pending) {
  if (out_pending == NULL) {
    return FR_ERR_INVALID;
  }
  *out_pending = fr_host_ble_notice_find_current();
  fr_host_ble_record(FR_BLE_OP_ACCEPT, FR_OK, 0, 0);
  return FR_OK;
}

fr_err_t fr_platform_ble_accept(fr_handle_ref_t runtime_ref,
                                uint16_t *out_platform_index,
                                bool *out_accepted) {
  if (out_platform_index == NULL || out_accepted == NULL) {
    return FR_ERR_INVALID;
  }
  *out_platform_index = FR_HANDLE_PLATFORM_NONE;
  *out_accepted = false;
  if (!fr_host_ble_notice_find_current()) {
    fr_host_ble_record(FR_BLE_OP_ACCEPT, FR_OK, 0, 0);
    return FR_OK;
  }

  fr_host_ble_notice_pop();
  fr_host_ble.connection.runtime_ref = runtime_ref;
  fr_host_ble.connection.has_runtime_ref = true;
  fr_host_ble.connection.info.state = FR_BLE_CONNECTION_LIVE;
  fr_host_ble.connection_accepts += 1u;
  *out_platform_index = FR_HOST_BLE_CONNECTION_INDEX;
  *out_accepted = true;
  fr_host_ble_record(FR_BLE_OP_ACCEPT, FR_OK, 0, 0);
  return FR_OK;
}

fr_err_t fr_platform_ble_reject_pending(void) {
  if (!fr_host_ble_notice_find_current()) {
    return FR_OK;
  }

  fr_host_ble_notice_pop();
  fr_host_ble.connection_disconnects += 1u;
  fr_host_ble.incoming_rejected += 1u;
  fr_host_ble_connection_free();
  fr_host_ble_record(FR_BLE_OP_ACCEPT, FR_ERR_CAPACITY, 0, 0);
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
fr_err_t fr_platform_ble_connection_ready(uint16_t platform_index,
                                          bool *out_ready) {
  fr_host_ble_connection_t *connection = NULL;

  if (out_ready == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_ble_connection_entry(platform_index, &connection));
  *out_ready = fr_host_ble_connection_is_live(connection);
  return FR_OK;
}

fr_err_t fr_platform_ble_connection_close(uint16_t platform_index) {
  fr_host_ble_connection_t *connection = NULL;

  FR_TRY(fr_host_ble_connection_entry(platform_index, &connection));
  if (connection->info.state != FR_BLE_CONNECTION_DISCONNECTED) {
    fr_host_ble.connection_disconnects += 1u;
  }
  fr_host_ble_connection_free();
  fr_host_ble_record(FR_BLE_OP_CONNECTION_CLOSE, FR_OK, 0, 0);
  return FR_OK;
}

fr_err_t fr_platform_ble_connection_info(
    uint16_t platform_index, fr_ble_connection_info_t *out_info) {
  fr_host_ble_connection_t *connection = NULL;

  if (out_info == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_ble_connection_entry(platform_index, &connection));
  *out_info = connection->info;
  return FR_OK;
}

fr_err_t fr_platform_ble_connection_rssi(uint16_t platform_index,
                                         int8_t *out_rssi) {
  fr_host_ble_connection_t *connection = NULL;

  if (out_rssi == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_ble_connection_entry(platform_index, &connection));
  if (!fr_host_ble_connection_is_live(connection)) {
    return FR_ERR_BLE_DISCONNECTED;
  }
  if (!connection->info.rssi_valid) {
    return FR_ERR_NOT_FOUND;
  }
  *out_rssi = connection->info.last_rssi;
  return FR_OK;
}

fr_err_t fr_platform_ble_connection_params(
    uint16_t platform_index, uint16_t minimum_interval_ms,
    uint16_t maximum_interval_ms, uint16_t latency,
    uint16_t supervision_timeout_ms) {
  fr_host_ble_connection_t *connection = NULL;
  uint32_t minimum_interval_units =
      ((uint32_t)minimum_interval_ms * 4u + 2u) / 5u;
  uint32_t maximum_interval_units =
      ((uint32_t)maximum_interval_ms * 4u + 2u) / 5u;
  uint32_t supervision_timeout_units =
      ((uint32_t)supervision_timeout_ms + 5u) / 10u;

  FR_TRY(fr_host_ble_connection_entry(platform_index, &connection));
  if (!fr_host_ble_connection_is_live(connection)) {
    fr_host_ble_record(FR_BLE_OP_CONNECTION_PARAMS,
                       FR_ERR_BLE_DISCONNECTED, 0, 0);
    return FR_ERR_BLE_DISCONNECTED;
  }
  if (minimum_interval_units < 6u || maximum_interval_units > 3200u ||
      minimum_interval_units > maximum_interval_units || latency > 499u ||
      supervision_timeout_units < 10u ||
      supervision_timeout_units > 3200u ||
      supervision_timeout_units * 8u <=
          ((uint32_t)latency + 1u) * maximum_interval_units * 2u) {
    fr_host_ble_record(FR_BLE_OP_CONNECTION_PARAMS, FR_ERR_RANGE, 0, 0);
    return FR_ERR_RANGE;
  }

  connection->info.interval_us = maximum_interval_units * 1250u;
  connection->info.latency = latency;
  connection->info.supervision_timeout_us =
      supervision_timeout_units * 10000u;
  fr_host_ble_record(FR_BLE_OP_CONNECTION_PARAMS, FR_OK, 0, 0);
  return FR_OK;
}

fr_err_t fr_platform_ble_connection_mtu(fr_runtime_t *runtime,
                                        uint16_t platform_index,
                                        uint16_t requested_mtu,
                                        uint16_t timeout_ms,
                                        uint16_t *out_actual_mtu) {
  fr_host_ble_connection_t *connection = NULL;

  if (runtime == NULL || out_actual_mtu == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_ble_connection_entry(platform_index, &connection));
  if (!fr_host_ble_connection_is_live(connection)) {
    fr_host_ble_record(FR_BLE_OP_CONNECTION_MTU, FR_ERR_BLE_DISCONNECTED, 0,
                       0);
    return FR_ERR_BLE_DISCONNECTED;
  }
  if (requested_mtu != FR_HOST_BLE_DEFAULT_MTU || timeout_ms == 0 ||
      timeout_ms > 60000u) {
    fr_host_ble_record(FR_BLE_OP_CONNECTION_MTU, FR_ERR_RANGE, 0, 0);
    return FR_ERR_RANGE;
  }

  connection->info.mtu = FR_HOST_BLE_DEFAULT_MTU;
  *out_actual_mtu = connection->info.mtu;
  fr_host_ble_record(FR_BLE_OP_CONNECTION_MTU, FR_OK, 0, 0);
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_GATT_SERVER
fr_err_t fr_platform_ble_gatt_install(const fr_ble_gatt_table_t *table) {
  fr_ble_gatt_table_t copy;
  uint32_t generation = fr_host_ble.gatt.table_generation + 1u;

  if (table == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_host_ble.radio_state != FR_BLE_RADIO_OFF) {
    fr_host_ble_record(FR_BLE_OP_GATT_INSTALL, FR_ERR_BLE_BUSY, 0, 0);
    return FR_ERR_BLE_BUSY;
  }
  if (!fr_host_ble_gatt_table_valid(table)) {
    fr_host_ble_record(FR_BLE_OP_GATT_INSTALL, FR_ERR_INVALID, 0, 0);
    return FR_ERR_INVALID;
  }

  copy = *table;
  for (uint16_t i = 0; i < copy.characteristic_count; i++) {
    copy.characteristics[i].value_length = 0;
    copy.characteristics[i].target_value_handle = 0;
  }
  memset(&fr_host_ble.gatt, 0, sizeof(fr_host_ble.gatt));
  fr_host_ble.gatt.table = copy;
  fr_host_ble.gatt.table_generation = generation;
  fr_host_ble_record(FR_BLE_OP_GATT_INSTALL, FR_OK, 0, 0);
  return FR_OK;
}

fr_err_t fr_platform_ble_gatt_status(fr_ble_gatt_status_t *out_status) {
  if (out_status == NULL) {
    return FR_ERR_INVALID;
  }

  *out_status = (fr_ble_gatt_status_t){
      .table = fr_host_ble.gatt.table,
      .table_generation = fr_host_ble.gatt.table_generation,
      .subscription_count = fr_host_ble.gatt.subscription_count,
      .write_queue_count = fr_host_ble.gatt.write_count,
      .write_queue_high_water = fr_host_ble.gatt.write_high_water,
      .write_queue_overflow = fr_host_ble.gatt.write_overflow,
      .write_queue_stale = fr_host_ble.gatt.write_stale,
      .preaccept_write_rejected =
          fr_host_ble.gatt.preaccept_write_rejected,
      .current_write_valid = fr_host_ble.gatt.current_write_valid,
      .current_write_attribute_id =
          fr_host_ble.gatt.current_write.attribute_id,
      .current_write_data_length = fr_host_ble.gatt.current_write.data_length,
      .indication_pending = fr_host_ble.gatt.indication_pending,
      .last_att_error = fr_host_ble.gatt.last_att_error,
      .last_platform_code = fr_host_ble.gatt.last_platform_code,
  };
  memcpy(out_status->subscriptions, fr_host_ble.gatt.subscriptions,
         sizeof(out_status->subscriptions));
  return FR_OK;
}

fr_err_t fr_platform_ble_gatt_set(uint16_t attribute_id,
                                  const uint8_t *bytes, uint16_t length) {
  fr_ble_gatt_characteristic_row_t *characteristic =
      fr_host_ble_gatt_characteristic(attribute_id);

  if (length > 0 && bytes == NULL) {
    return FR_ERR_INVALID;
  }
  if (characteristic == NULL) {
    fr_host_ble_record(FR_BLE_OP_GATT_SET, FR_ERR_NOT_FOUND, 0, 0);
    return FR_ERR_NOT_FOUND;
  }
  if (length > characteristic->maximum_length) {
    fr_host_ble_record(FR_BLE_OP_GATT_SET, FR_ERR_CAPACITY, 0, 0);
    return FR_ERR_CAPACITY;
  }

  if (length > 0) {
    memcpy(&fr_host_ble.gatt.value_bytes[characteristic->value_offset], bytes,
           length);
  }
  characteristic->value_length = length;
  fr_host_ble_record(FR_BLE_OP_GATT_SET, FR_OK, 0, 0);
  return FR_OK;
}

fr_err_t fr_platform_ble_gatt_get(uint16_t attribute_id, uint8_t *out_bytes,
                                  uint16_t capacity, uint16_t *out_length) {
  fr_ble_gatt_characteristic_row_t *characteristic =
      fr_host_ble_gatt_characteristic(attribute_id);

  if (out_bytes == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }
  if (characteristic == NULL) {
    return FR_ERR_NOT_FOUND;
  }
  if (characteristic->value_length > capacity) {
    return FR_ERR_CAPACITY;
  }

  if (characteristic->value_length > 0) {
    memcpy(out_bytes,
           &fr_host_ble.gatt.value_bytes[characteristic->value_offset],
           characteristic->value_length);
  }
  *out_length = characteristic->value_length;
  return FR_OK;
}

static fr_err_t fr_host_ble_gatt_send_check(
    uint16_t connection_index, uint16_t attribute_id, const uint8_t *bytes,
    uint16_t length, uint16_t required_flag, bool require_indicate) {
  fr_host_ble_connection_t *connection = NULL;
  fr_ble_gatt_characteristic_row_t *characteristic = NULL;
  fr_ble_gatt_subscription_t *subscription = NULL;

  if (length > 0 && bytes == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_ble_connection_entry(connection_index, &connection));
  if (!fr_host_ble_connection_is_live(connection) ||
      connection->info.role != FR_BLE_CONNECTION_ROLE_PERIPHERAL) {
    return FR_ERR_BLE_DISCONNECTED;
  }
  if (fr_host_ble.radio_state != FR_BLE_RADIO_READY) {
    return FR_ERR_BLE_NOT_READY;
  }

  characteristic = fr_host_ble_gatt_characteristic(attribute_id);
  if (characteristic == NULL) {
    return FR_ERR_NOT_FOUND;
  }
  if ((characteristic->portable_flags & required_flag) == 0) {
    return FR_ERR_UNSUPPORTED;
  }
  if (length > characteristic->maximum_length ||
      length > (uint16_t)(connection->info.mtu - 3u)) {
    return FR_ERR_CAPACITY;
  }

  subscription = fr_host_ble_gatt_subscription(attribute_id);
  if (subscription == NULL ||
      (require_indicate ? !subscription->indicate : !subscription->notify)) {
    return FR_ERR_BLE_NOT_READY;
  }
  return FR_OK;
}

fr_err_t fr_platform_ble_gatt_notify(uint16_t connection_index,
                                     uint16_t attribute_id,
                                     const uint8_t *bytes, uint16_t length) {
  fr_err_t err = fr_host_ble_gatt_send_check(
      connection_index, attribute_id, bytes, length, FR_BLE_GATT_CHR_NOTIFY,
      false);

  fr_host_ble_record(FR_BLE_OP_GATT_NOTIFY, err, 0, 0);
  return err;
}

fr_err_t fr_platform_ble_gatt_indicate(fr_runtime_t *runtime,
                                       uint16_t connection_index,
                                       uint16_t attribute_id,
                                       const uint8_t *bytes, uint16_t length,
                                       uint16_t timeout_ms) {
  bool timeout = false;
  fr_err_t err = FR_OK;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (timeout_ms == 0 || timeout_ms > 60000u) {
    fr_host_ble_record(FR_BLE_OP_GATT_INDICATE, FR_ERR_RANGE, 0, 0);
    return FR_ERR_RANGE;
  }
  if (fr_host_ble.gatt.indication_pending) {
    fr_host_ble_record(FR_BLE_OP_GATT_INDICATE, FR_ERR_BLE_BUSY, 0, 0);
    return FR_ERR_BLE_BUSY;
  }

  err = fr_host_ble_gatt_send_check(
      connection_index, attribute_id, bytes, length,
      FR_BLE_GATT_CHR_INDICATE, true);
  if (err == FR_OK) {
#ifdef FR_HOST_TEST_HELPERS
    timeout = fr_host_ble.timeout_next_indication;
    fr_host_ble.timeout_next_indication = false;
#endif
    fr_host_ble.gatt.indication_pending = true;
    if (timeout) {
      err = FR_ERR_BLE_TIMEOUT;
    } else {
      fr_host_ble.gatt.indication_pending = false;
    }
  }
  fr_host_ble_record(FR_BLE_OP_GATT_INDICATE, err, 0, 0);
  return err;
}

fr_err_t fr_platform_ble_gatt_write_next(bool *out_has_write,
                                         fr_handle_ref_t *out_runtime_ref) {
  if (out_has_write == NULL || out_runtime_ref == NULL) {
    return FR_ERR_INVALID;
  }

  *out_has_write = false;
  *out_runtime_ref = (fr_handle_ref_t){0};
  fr_host_ble.gatt.current_write_valid = false;
  memset(&fr_host_ble.gatt.current_write, 0,
         sizeof(fr_host_ble.gatt.current_write));

  while (fr_host_ble.gatt.write_count > 0) {
    fr_ble_gatt_write_t write =
        fr_host_ble.gatt.writes[fr_host_ble.gatt.write_head];

    fr_host_ble.gatt.write_head =
        (uint8_t)((fr_host_ble.gatt.write_head + 1u) %
                  FR_BLE_GATT_WRITE_QUEUE_COUNT);
    fr_host_ble.gatt.write_count -= 1u;
    if (write.table_generation != fr_host_ble.gatt.table_generation ||
        write.connection_index != FR_HOST_BLE_CONNECTION_INDEX ||
        write.connection_generation != fr_host_ble.connection.generation ||
        !fr_host_ble.connection.has_runtime_ref ||
        fr_host_ble.connection.info.state == FR_BLE_CONNECTION_FREE) {
      fr_host_ble.gatt.write_stale += 1u;
      continue;
    }

    fr_host_ble.gatt.current_write = write;
    fr_host_ble.gatt.current_write_valid = true;
    *out_runtime_ref = fr_host_ble.connection.runtime_ref;
    *out_has_write = true;
    break;
  }

  fr_host_ble_record(FR_BLE_OP_GATT_WRITE_NEXT, FR_OK, 0, 0);
  return FR_OK;
}

fr_err_t fr_platform_ble_gatt_write_current(fr_ble_gatt_write_t *out_write) {
  if (out_write == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_host_ble.gatt.current_write_valid) {
    return FR_ERR_NOT_FOUND;
  }
  *out_write = fr_host_ble.gatt.current_write;
  return FR_OK;
}

#endif

#if FR_BLE_ENABLE_GATT_CLIENT
enum {
  FR_HOST_BLE_CLIENT_ATT_INVALID_HANDLE = 0x01,
  FR_HOST_BLE_CLIENT_ATT_READ_NOT_PERMITTED = 0x02,
  FR_HOST_BLE_CLIENT_ATT_WRITE_NOT_PERMITTED = 0x03,
  FR_HOST_BLE_CLIENT_ATT_INVALID_VALUE_LENGTH = 0x0d,
};

fr_err_t fr_platform_ble_gatt_client_find(
    fr_runtime_t *runtime, uint16_t connection_index,
    const fr_ble_uuid_t *service_uuid,
    const fr_ble_uuid_t *characteristic_uuid, uint16_t timeout_ms,
    uint16_t *out_attribute_handle) {
  fr_host_ble_gatt_client_cache_t *entry = NULL;
  fr_err_t err = FR_OK;

  if (runtime == NULL || service_uuid == NULL || characteristic_uuid == NULL ||
      out_attribute_handle == NULL) {
    return FR_ERR_INVALID;
  }
  err = fr_host_ble_gatt_client_check(connection_index, timeout_ms);
  if (err != FR_OK) {
    fr_host_ble_record(FR_BLE_OP_GATT_FIND, err, 0, 0);
    return err;
  }

  fr_host_ble.gatt_client.service_match_count =
      fr_host_ble_uuid_equal(service_uuid,
                             &fr_host_ble.remote_gatt.service_uuid)
          ? 1u
          : 0u;
  fr_host_ble.gatt_client.characteristic_match_count =
      fr_host_ble.gatt_client.service_match_count > 0 &&
              fr_host_ble_uuid_equal(
                  characteristic_uuid,
                  &fr_host_ble.remote_gatt.characteristic_uuid)
          ? 1u
          : 0u;
  if (fr_host_ble.gatt_client.characteristic_match_count == 0) {
    fr_host_ble_record(FR_BLE_OP_GATT_FIND, FR_ERR_NOT_FOUND, 0,
                       FR_HOST_BLE_CLIENT_ATT_INVALID_HANDLE);
    return FR_ERR_NOT_FOUND;
  }

  entry = fr_host_ble_gatt_client_cache(fr_host_ble.remote_gatt.value_handle);
  if (entry == NULL) {
    for (uint8_t i = 0; i < FR_BLE_GATT_CLIENT_CACHE_COUNT; i++) {
      if (!fr_host_ble.gatt_client.cache[i].valid) {
        entry = &fr_host_ble.gatt_client.cache[i];
        fr_host_ble.gatt_client.cache_count += 1u;
        break;
      }
    }
  }
  if (entry == NULL) {
    fr_host_ble_record(FR_BLE_OP_GATT_FIND, FR_ERR_CAPACITY, 0, 0);
    return FR_ERR_CAPACITY;
  }

  *entry = (fr_host_ble_gatt_client_cache_t){
      .service_uuid = *service_uuid,
      .characteristic_uuid = *characteristic_uuid,
      .connection_generation = fr_host_ble.connection.generation,
      .value_handle = fr_host_ble.remote_gatt.value_handle,
      .cccd_handle = fr_host_ble.remote_gatt.cccd_handle,
      .properties = fr_host_ble.remote_gatt.properties,
      .valid = true,
  };
  *out_attribute_handle = entry->value_handle;
  fr_host_ble.gatt_client.last_att_error = 0;
  fr_host_ble_record(FR_BLE_OP_GATT_FIND, FR_OK, 0, 0);
  return FR_OK;
}

fr_err_t fr_platform_ble_gatt_client_read(
    fr_runtime_t *runtime, uint16_t connection_index,
    uint16_t attribute_handle, uint16_t timeout_ms, uint8_t *out_bytes,
    uint16_t capacity, uint16_t *out_length) {
  fr_host_ble_gatt_client_cache_t *entry = NULL;
  fr_err_t err = FR_OK;

  if (runtime == NULL || out_bytes == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }
  err = fr_host_ble_gatt_client_check(connection_index, timeout_ms);
  if (err == FR_OK) {
    entry = fr_host_ble_gatt_client_cache(attribute_handle);
    if (entry == NULL) {
      err = FR_ERR_NOT_FOUND;
      fr_host_ble.gatt_client.last_att_error =
          FR_HOST_BLE_CLIENT_ATT_INVALID_HANDLE;
    } else if ((entry->properties & FR_BLE_GATT_CHR_READ) == 0) {
      err = FR_ERR_UNSUPPORTED;
      fr_host_ble.gatt_client.last_att_error =
          FR_HOST_BLE_CLIENT_ATT_READ_NOT_PERMITTED;
    } else if (fr_host_ble.remote_gatt.value_length > capacity) {
      err = FR_ERR_CAPACITY;
      fr_host_ble.gatt_client.last_att_error =
          FR_HOST_BLE_CLIENT_ATT_INVALID_VALUE_LENGTH;
    }
  }
  if (err == FR_OK) {
    memcpy(out_bytes, fr_host_ble.remote_gatt.value,
           fr_host_ble.remote_gatt.value_length);
    *out_length = fr_host_ble.remote_gatt.value_length;
    fr_host_ble.gatt_client.last_att_error = 0;
  }
  fr_host_ble_record(FR_BLE_OP_GATT_READ, err, 0,
                     fr_host_ble.gatt_client.last_att_error);
  return err;
}

fr_err_t fr_platform_ble_gatt_client_write(
    fr_runtime_t *runtime, uint16_t connection_index,
    uint16_t attribute_handle, const uint8_t *bytes, uint16_t length,
    bool with_response, uint16_t timeout_ms) {
  fr_host_ble_gatt_client_cache_t *entry = NULL;
  uint16_t required_property = with_response ? FR_BLE_GATT_CHR_WRITE
                                             : FR_BLE_GATT_CHR_WRITE_COMMAND;
  fr_err_t err = FR_OK;

  if (runtime == NULL || (length > 0 && bytes == NULL)) {
    return FR_ERR_INVALID;
  }
  err = fr_host_ble_gatt_client_check(connection_index, timeout_ms);
  if (err == FR_OK) {
    entry = fr_host_ble_gatt_client_cache(attribute_handle);
    if (entry == NULL) {
      err = FR_ERR_NOT_FOUND;
      fr_host_ble.gatt_client.last_att_error =
          FR_HOST_BLE_CLIENT_ATT_INVALID_HANDLE;
    } else if ((entry->properties & required_property) == 0) {
      err = FR_ERR_UNSUPPORTED;
      fr_host_ble.gatt_client.last_att_error =
          FR_HOST_BLE_CLIENT_ATT_WRITE_NOT_PERMITTED;
    } else if (length > FR_BLE_GATT_CLIENT_DATA_BYTES) {
      err = FR_ERR_CAPACITY;
      fr_host_ble.gatt_client.last_att_error =
          FR_HOST_BLE_CLIENT_ATT_INVALID_VALUE_LENGTH;
    }
  }
  if (err == FR_OK) {
    memset(fr_host_ble.remote_gatt.value, 0,
           sizeof(fr_host_ble.remote_gatt.value));
    if (length > 0) {
      memcpy(fr_host_ble.remote_gatt.value, bytes, length);
    }
    fr_host_ble.remote_gatt.value_length = (uint8_t)length;
    fr_host_ble.gatt_client.last_att_error = 0;
  }
  fr_host_ble_record(FR_BLE_OP_GATT_WRITE, err, 0,
                     fr_host_ble.gatt_client.last_att_error);
  return err;
}

fr_err_t fr_platform_ble_gatt_client_subscribe(
    fr_runtime_t *runtime, uint16_t connection_index,
    uint16_t attribute_handle, fr_ble_gatt_subscription_mode_t mode,
    uint16_t timeout_ms) {
  fr_host_ble_gatt_client_cache_t *entry = NULL;
  uint16_t required_property = 0;
  fr_err_t err = FR_OK;

  if (runtime == NULL ||
      (mode != FR_BLE_GATT_SUBSCRIBE_NOTIFICATIONS &&
       mode != FR_BLE_GATT_SUBSCRIBE_INDICATIONS)) {
    return FR_ERR_INVALID;
  }
  required_property = mode == FR_BLE_GATT_SUBSCRIBE_NOTIFICATIONS
                          ? FR_BLE_GATT_CHR_NOTIFY
                          : FR_BLE_GATT_CHR_INDICATE;
  err = fr_host_ble_gatt_client_check(connection_index, timeout_ms);
  if (err == FR_OK) {
    entry = fr_host_ble_gatt_client_cache(attribute_handle);
    if (entry == NULL || entry->cccd_handle == 0) {
      err = FR_ERR_NOT_FOUND;
    } else if ((entry->properties & required_property) == 0) {
      err = FR_ERR_UNSUPPORTED;
    }
  }
  if (err == FR_OK) {
    entry->subscription_mode = mode;
  }
  fr_host_ble_record(FR_BLE_OP_GATT_SUBSCRIBE, err, 0, 0);
  return err;
}

fr_err_t fr_platform_ble_gatt_client_unsubscribe(
    fr_runtime_t *runtime, uint16_t connection_index,
    uint16_t attribute_handle, uint16_t timeout_ms) {
  fr_host_ble_gatt_client_cache_t *entry = NULL;
  fr_err_t err = FR_OK;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  err = fr_host_ble_gatt_client_check(connection_index, timeout_ms);
  if (err == FR_OK) {
    entry = fr_host_ble_gatt_client_cache(attribute_handle);
    if (entry == NULL || entry->cccd_handle == 0) {
      err = FR_ERR_NOT_FOUND;
    } else {
      entry->subscription_mode = 0;
    }
  }
  fr_host_ble_record(FR_BLE_OP_GATT_UNSUBSCRIBE, err, 0, 0);
  return err;
}

fr_err_t fr_platform_ble_gatt_notification_next(
    bool *out_has_notification, fr_handle_ref_t *out_runtime_ref) {
  if (out_has_notification == NULL || out_runtime_ref == NULL) {
    return FR_ERR_INVALID;
  }

  *out_has_notification = false;
  *out_runtime_ref = (fr_handle_ref_t){0};
  fr_host_ble.gatt_client.current_notification_valid = false;
  memset(&fr_host_ble.gatt_client.current_notification, 0,
         sizeof(fr_host_ble.gatt_client.current_notification));
  while (fr_host_ble.gatt_client.notification_count > 0) {
    fr_ble_gatt_notification_t notification =
        fr_host_ble.gatt_client
            .notifications[fr_host_ble.gatt_client.notification_head];

    fr_host_ble.gatt_client.notification_head =
        (uint8_t)((fr_host_ble.gatt_client.notification_head + 1u) %
                  FR_BLE_GATT_NOTIFICATION_QUEUE_COUNT);
    fr_host_ble.gatt_client.notification_count -= 1u;
    if (notification.connection_index != FR_HOST_BLE_CONNECTION_INDEX ||
        notification.connection_generation !=
            fr_host_ble.connection.generation ||
        !fr_host_ble.connection.has_runtime_ref ||
        fr_host_ble.connection.info.state == FR_BLE_CONNECTION_FREE) {
      fr_host_ble.gatt_client.notification_stale += 1u;
      continue;
    }

    fr_host_ble.gatt_client.current_notification = notification;
    fr_host_ble.gatt_client.current_notification_valid = true;
    *out_runtime_ref = fr_host_ble.connection.runtime_ref;
    *out_has_notification = true;
    break;
  }
  fr_host_ble_record(FR_BLE_OP_GATT_NOTIFICATION_NEXT, FR_OK, 0, 0);
  return FR_OK;
}

fr_err_t fr_platform_ble_gatt_notification_current(
    fr_ble_gatt_notification_t *out_notification) {
  if (out_notification == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_host_ble.gatt_client.current_notification_valid) {
    return FR_ERR_NOT_FOUND;
  }
  *out_notification = fr_host_ble.gatt_client.current_notification;
  return FR_OK;
}

fr_err_t fr_platform_ble_gatt_client_status(
    fr_ble_gatt_client_status_t *out_status) {
  if (out_status == NULL) {
    return FR_ERR_INVALID;
  }
  *out_status = (fr_ble_gatt_client_status_t){
      .cache_count = fr_host_ble.gatt_client.cache_count,
      .subscription_count = fr_host_ble_gatt_client_subscription_count(),
      .notification_queue_count =
          fr_host_ble.gatt_client.notification_count,
      .notification_queue_high_water =
          fr_host_ble.gatt_client.notification_high_water,
      .notification_dropped = fr_host_ble.gatt_client.notification_dropped,
      .notification_stale = fr_host_ble.gatt_client.notification_stale,
      .current_notification_valid =
          fr_host_ble.gatt_client.current_notification_valid,
      .current_notification_attribute_handle =
          fr_host_ble.gatt_client.current_notification.attribute_handle,
      .current_notification_data_length =
          fr_host_ble.gatt_client.current_notification.data_length,
      .current_notification_indication =
          fr_host_ble.gatt_client.current_notification.indication,
      .procedure_operation = FR_BLE_OP_NONE,
      .service_match_count = fr_host_ble.gatt_client.service_match_count,
      .characteristic_match_count =
          fr_host_ble.gatt_client.characteristic_match_count,
      .last_att_error = fr_host_ble.gatt_client.last_att_error,
      .last_platform_code = fr_host_ble.gatt_client.last_platform_code,
  };
  return FR_OK;
}
#endif

#ifdef FR_HOST_TEST_HELPERS
void fr_host_ble_reset(void) {
  memset(&fr_host_ble, 0, sizeof(fr_host_ble));
  fr_host_ble.radio_state = FR_BLE_RADIO_OFF;
#if FR_BLE_ENABLE_OBSERVER
  fr_host_ble.scan_state = FR_BLE_SCAN_IDLE;
#endif
#if FR_BLE_ENABLE_BROADCASTER
  fr_host_ble.advertise_state = FR_BLE_ADVERTISE_IDLE;
#endif
#if FR_BLE_ENABLE_GATT_CLIENT
  fr_host_ble_remote_gatt_init();
#endif
  fr_host_ble.last_result = FR_OK;
}

#if FR_BLE_ENABLE_OBSERVER
fr_err_t fr_host_ble_push_scan_report(const fr_ble_scan_report_t *report) {
  uint8_t tail = 0;

  if (report == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_host_ble.scan_state != FR_BLE_SCAN_ACTIVE) {
    fr_host_ble.late_callback_count += 1u;
    return FR_OK;
  }

  fr_host_ble.received += 1u;
  if (!fr_host_ble_report_valid(report)) {
    fr_host_ble.malformed += 1u;
    return FR_OK;
  }
  if (report->rssi < fr_host_ble.minimum_rssi) {
    fr_host_ble.filtered_rssi += 1u;
    return FR_OK;
  }
  if (fr_host_ble.count == FR_BLE_SCAN_QUEUE_COUNT) {
    fr_host_ble.head =
        (uint8_t)((fr_host_ble.head + 1u) % FR_BLE_SCAN_QUEUE_COUNT);
    fr_host_ble.count -= 1u;
    fr_host_ble.dropped += 1u;
  }
  tail = (uint8_t)((fr_host_ble.head + fr_host_ble.count) %
                   FR_BLE_SCAN_QUEUE_COUNT);
  fr_host_ble.queue[tail] = *report;
  fr_host_ble.count += 1u;
  fr_host_ble.accepted += 1u;
  if (fr_host_ble.count > fr_host_ble.high_water) {
    fr_host_ble.high_water = fr_host_ble.count;
  }
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_PERIPHERAL
fr_err_t fr_host_ble_queue_incoming(const uint8_t peer[7]) {
  uint8_t tail = 0;

  if (peer == NULL || peer[0] > FR_BLE_ADDRESS_RANDOM_ID) {
    return FR_ERR_INVALID;
  }
  if (fr_host_ble.radio_state != FR_BLE_RADIO_READY
#if FR_BLE_ENABLE_BROADCASTER
      || fr_host_ble.advertise_state != FR_BLE_ADVERTISE_ACTIVE ||
      !fr_host_ble.advertise_connectable
#endif
  ) {
    return FR_ERR_BLE_NOT_READY;
  }
  if (fr_host_ble.connection.info.state != FR_BLE_CONNECTION_FREE ||
      fr_host_ble.connection_notice_count == FR_BLE_CONNECTION_NOTICE_COUNT) {
#if FR_BLE_ENABLE_BROADCASTER
    fr_host_ble.advertise_state = FR_BLE_ADVERTISE_IDLE;
#endif
    fr_host_ble.incoming_rejected += 1u;
    return FR_ERR_CAPACITY;
  }

  fr_host_ble_connection_begin(FR_BLE_CONNECTION_ROLE_PERIPHERAL, peer,
                               FR_BLE_CONNECTION_PENDING);
  tail = (uint8_t)((fr_host_ble.connection_notice_head +
                    fr_host_ble.connection_notice_count) %
                   FR_BLE_CONNECTION_NOTICE_COUNT);
  fr_host_ble.connection_notices[tail] = (fr_host_ble_connection_notice_t){
      .platform_index = FR_HOST_BLE_CONNECTION_INDEX,
      .generation = fr_host_ble.connection.generation,
  };
  fr_host_ble.connection_notice_count += 1u;
#if FR_BLE_ENABLE_BROADCASTER
  fr_host_ble.advertise_state = FR_BLE_ADVERTISE_IDLE;
#endif
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
fr_err_t fr_host_ble_disconnect(uint16_t platform_index, int32_t reason) {
  fr_host_ble_connection_t *connection = NULL;

  FR_TRY(fr_host_ble_connection_entry(platform_index, &connection));
  if (connection->info.state == FR_BLE_CONNECTION_DISCONNECTED) {
    return FR_OK;
  }
  if (connection->info.state == FR_BLE_CONNECTION_PENDING) {
    fr_host_ble_connection_mark_disconnected(reason);
    fr_host_ble_connection_free();
  } else if (fr_host_ble_connection_is_live(connection)) {
    fr_host_ble_connection_mark_disconnected(reason);
  } else {
    return FR_ERR_BLE_BUSY;
  }
  fr_host_ble.last_protocol_reason = reason;
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_GATT_CLIENT
fr_err_t fr_host_ble_gatt_client_notify(uint16_t attribute_handle,
                                        const uint8_t *bytes, uint16_t length,
                                        bool indication) {
  fr_host_ble_gatt_client_cache_t *entry =
      fr_host_ble_gatt_client_cache(attribute_handle);
  fr_ble_gatt_subscription_mode_t required_mode =
      indication ? FR_BLE_GATT_SUBSCRIBE_INDICATIONS
                 : FR_BLE_GATT_SUBSCRIBE_NOTIFICATIONS;
  uint8_t tail = 0;

  if (length > 0 && bytes == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_host_ble.connection.info.state != FR_BLE_CONNECTION_LIVE ||
      !fr_host_ble.connection.has_runtime_ref ||
      fr_host_ble.connection.info.role != FR_BLE_CONNECTION_ROLE_CENTRAL) {
    return FR_ERR_BLE_DISCONNECTED;
  }
  if (entry == NULL || entry->subscription_mode != required_mode) {
    return FR_ERR_BLE_NOT_READY;
  }
  if (length > FR_BLE_GATT_CLIENT_DATA_BYTES) {
    return FR_ERR_CAPACITY;
  }
  if (fr_host_ble.gatt_client.notification_count ==
      FR_BLE_GATT_NOTIFICATION_QUEUE_COUNT) {
    fr_host_ble.gatt_client.notification_dropped += 1u;
    return FR_ERR_CAPACITY;
  }

  tail = (uint8_t)((fr_host_ble.gatt_client.notification_head +
                    fr_host_ble.gatt_client.notification_count) %
                   FR_BLE_GATT_NOTIFICATION_QUEUE_COUNT);
  fr_host_ble.gatt_client.notifications[tail] =
      (fr_ble_gatt_notification_t){
          .connection_index = FR_HOST_BLE_CONNECTION_INDEX,
          .connection_generation = fr_host_ble.connection.generation,
          .attribute_handle = attribute_handle,
          .data_length = (uint8_t)length,
          .timestamp_ms = fr_host_millis,
          .indication = indication,
      };
  if (length > 0) {
    memcpy(fr_host_ble.gatt_client.notifications[tail].data, bytes, length);
  }
  fr_host_ble.gatt_client.notification_count += 1u;
  if (fr_host_ble.gatt_client.notification_count >
      fr_host_ble.gatt_client.notification_high_water) {
    fr_host_ble.gatt_client.notification_high_water =
        fr_host_ble.gatt_client.notification_count;
  }
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_GATT_SERVER
fr_err_t fr_host_ble_gatt_remote_write(uint16_t attribute_id,
                                       const uint8_t *bytes,
                                       uint16_t length) {
  fr_ble_gatt_characteristic_row_t *characteristic =
      fr_host_ble_gatt_characteristic(attribute_id);
  uint8_t tail = 0;

  if (length > 0 && bytes == NULL) {
    return FR_ERR_INVALID;
  }
  if (characteristic == NULL) {
    fr_host_ble.gatt.last_att_error = FR_HOST_BLE_ATT_INVALID_HANDLE;
    return FR_ERR_NOT_FOUND;
  }
  if (fr_host_ble.connection.info.state != FR_BLE_CONNECTION_LIVE ||
      !fr_host_ble.connection.has_runtime_ref ||
      fr_host_ble.connection.info.role != FR_BLE_CONNECTION_ROLE_PERIPHERAL) {
    fr_host_ble.gatt.preaccept_write_rejected += 1u;
    fr_host_ble.gatt.last_att_error = FR_HOST_BLE_ATT_WRITE_NOT_PERMITTED;
    return FR_ERR_BLE_NOT_READY;
  }
  if ((characteristic->portable_flags &
       (FR_BLE_GATT_CHR_WRITE | FR_BLE_GATT_CHR_WRITE_COMMAND)) == 0) {
    fr_host_ble.gatt.last_att_error = FR_HOST_BLE_ATT_WRITE_NOT_PERMITTED;
    return FR_ERR_UNSUPPORTED;
  }
  if (length > characteristic->maximum_length ||
      length > FR_BLE_GATT_WRITE_DATA_BYTES) {
    fr_host_ble.gatt.last_att_error = FR_HOST_BLE_ATT_INVALID_VALUE_LENGTH;
    return FR_ERR_CAPACITY;
  }
  if (fr_host_ble.gatt.write_count == FR_BLE_GATT_WRITE_QUEUE_COUNT) {
    fr_host_ble.gatt.write_overflow += 1u;
    fr_host_ble.gatt.last_att_error =
        FR_HOST_BLE_ATT_INSUFFICIENT_RESOURCES;
    return FR_ERR_CAPACITY;
  }

  tail = (uint8_t)((fr_host_ble.gatt.write_head +
                    fr_host_ble.gatt.write_count) %
                   FR_BLE_GATT_WRITE_QUEUE_COUNT);
  fr_host_ble.gatt.writes[tail] = (fr_ble_gatt_write_t){
      .connection_index = FR_HOST_BLE_CONNECTION_INDEX,
      .connection_generation = fr_host_ble.connection.generation,
      .table_generation = fr_host_ble.gatt.table_generation,
      .attribute_id = attribute_id,
      .data_length = length,
      .timestamp_ms = fr_host_millis,
  };
  if (length > 0) {
    memcpy(fr_host_ble.gatt.writes[tail].data, bytes, length);
    memcpy(&fr_host_ble.gatt.value_bytes[characteristic->value_offset], bytes,
           length);
  }
  characteristic->value_length = length;
  fr_host_ble.gatt.write_count += 1u;
  if (fr_host_ble.gatt.write_count > fr_host_ble.gatt.write_high_water) {
    fr_host_ble.gatt.write_high_water = fr_host_ble.gatt.write_count;
  }
  fr_host_ble.gatt.last_att_error = 0;
  return FR_OK;
}

fr_err_t fr_host_ble_gatt_subscribe(uint16_t attribute_id, bool notify,
                                    bool indicate) {
  fr_ble_gatt_characteristic_row_t *characteristic =
      fr_host_ble_gatt_characteristic(attribute_id);
  fr_ble_gatt_subscription_t *subscription = NULL;

  if (characteristic == NULL) {
    return FR_ERR_NOT_FOUND;
  }
  if (fr_host_ble.connection.info.state == FR_BLE_CONNECTION_FREE ||
      fr_host_ble.connection.info.role != FR_BLE_CONNECTION_ROLE_PERIPHERAL) {
    return FR_ERR_BLE_DISCONNECTED;
  }
  if ((notify &&
       (characteristic->portable_flags & FR_BLE_GATT_CHR_NOTIFY) == 0) ||
      (indicate &&
       (characteristic->portable_flags & FR_BLE_GATT_CHR_INDICATE) == 0)) {
    return FR_ERR_UNSUPPORTED;
  }

  subscription = fr_host_ble_gatt_subscription(attribute_id);
  if (!notify && !indicate) {
    if (subscription != NULL) {
      uint8_t index = (uint8_t)(subscription - fr_host_ble.gatt.subscriptions);

      fr_host_ble.gatt.subscription_count -= 1u;
      fr_host_ble.gatt.subscriptions[index] =
          fr_host_ble.gatt
              .subscriptions[fr_host_ble.gatt.subscription_count];
      memset(&fr_host_ble.gatt
                  .subscriptions[fr_host_ble.gatt.subscription_count],
             0, sizeof(fr_host_ble.gatt.subscriptions[0]));
    }
    return FR_OK;
  }
  if (subscription == NULL) {
    if (fr_host_ble.gatt.subscription_count == FR_BLE_GATT_CCCD_COUNT) {
      return FR_ERR_CAPACITY;
    }
    subscription =
        &fr_host_ble.gatt
             .subscriptions[fr_host_ble.gatt.subscription_count++];
  }

  *subscription = (fr_ble_gatt_subscription_t){
      .connection_index = FR_HOST_BLE_CONNECTION_INDEX,
      .connection_generation = fr_host_ble.connection.generation,
      .attribute_id = attribute_id,
      .notify = notify,
      .indicate = indicate,
  };
  return FR_OK;
}

void fr_host_ble_timeout_next_indication(void) {
  fr_host_ble.timeout_next_indication = true;
}
#endif

void fr_host_ble_fail_next_on(fr_err_t err, int32_t raw_code) {
  fr_host_ble.fail_next_on = err;
  fr_host_ble.fail_next_on_raw_code = raw_code;
  fr_host_ble.timeout_next_on = false;
}

#if FR_BLE_ENABLE_OBSERVER
void fr_host_ble_fail_next_scan_start(fr_err_t err, int32_t raw_code) {
  fr_host_ble.fail_next_scan_start = err;
  fr_host_ble.fail_next_scan_start_raw_code = raw_code;
}
#endif

void fr_host_ble_timeout_next_on(void) {
  fr_host_ble.fail_next_on = FR_OK;
  fr_host_ble.fail_next_on_raw_code = 0;
  fr_host_ble.timeout_next_on = true;
}

#if FR_BLE_ENABLE_OBSERVER
void fr_host_ble_timeout_next_scan_stop(void) {
  fr_host_ble.timeout_next_scan_stop = true;
}
#endif

void fr_host_ble_post_reset(int32_t raw_reason) {
  if (fr_host_ble.radio_state == FR_BLE_RADIO_OFF) {
    fr_host_ble.late_callback_count += 1u;
    return;
  }

  fr_host_ble.reset_count += 1u;
  fr_host_ble.last_protocol_reason = raw_reason;
#if FR_BLE_ENABLE_OBSERVER
  fr_host_ble.dropped += fr_host_ble.count;
  fr_host_ble.head = 0;
  fr_host_ble.count = 0;
  fr_host_ble.current_valid = false;
  memset(&fr_host_ble.current, 0, sizeof(fr_host_ble.current));
  fr_host_ble.scan_state = FR_BLE_SCAN_IDLE;
  fr_host_ble.scan_generation += 1u;
#endif
#if FR_BLE_ENABLE_BROADCASTER
  fr_host_ble.advertise_state = FR_BLE_ADVERTISE_IDLE;
#endif
#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
  if (fr_host_ble.connection.info.state == FR_BLE_CONNECTION_LIVE) {
    fr_host_ble_connection_mark_disconnected(raw_reason);
  } else if (fr_host_ble.connection.info.state == FR_BLE_CONNECTION_PENDING) {
    fr_host_ble.connection_disconnects += 1u;
    fr_host_ble_connection_free();
  }
#endif
  if (fr_host_ble.shutdown_in_progress) {
    fr_host_ble.radio_state = FR_BLE_RADIO_OFF;
    fr_host_ble.shutdown_in_progress = false;
    fr_host_ble.cleanup_required = false;
  } else {
    fr_host_ble.radio_state = FR_BLE_RADIO_FAILED;
    fr_host_ble.cleanup_required = true;
  }
}
#endif
#endif

#if FR_FEATURE_UART
fr_err_t fr_platform_uart_open(uint16_t port, uint32_t baud,
                               uint16_t *out_platform_index) {
  if (out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (port > FR_HOST_UART_MAX_PORT || !fr_host_uart_baud_valid(baud) ||
      fr_host_uart_port_in_use(port)) {
    return FR_ERR_DOMAIN;
  }

  for (uint16_t i = 0; i < FR_PROFILE_MAX_HANDLES; i++) {
    fr_host_uart_t *uart = &fr_host_uarts[i];

    if (uart->in_use) {
      continue;
    }

    *uart = (fr_host_uart_t){
        .in_use = true,
        .port = port,
        .baud = baud,
    };
    *out_platform_index = i;
    return FR_OK;
  }

  return FR_ERR_CAPACITY;
}

/* Host has no real GPIO lines, so there is no console UART to collide with.
 * The host still records tx/rx and rejects two open-on calls that pick the
 * same pin, which is what lets the test suite exercise the override path
 * without an ESP32 in the room. */
static bool fr_host_uart_pin_conflict(uint16_t tx, uint16_t rx) {
  for (uint16_t i = 0; i < FR_PROFILE_MAX_HANDLES; i++) {
    const fr_host_uart_t *uart = &fr_host_uarts[i];

    if (!uart->in_use || !uart->custom_pins) {
      continue;
    }
    if (uart->tx == tx || uart->tx == rx || uart->rx == tx ||
        uart->rx == rx) {
      return true;
    }
  }
  return false;
}

fr_err_t fr_platform_uart_open_on(uint16_t port, uint16_t tx, uint16_t rx,
                                  uint32_t baud,
                                  uint16_t *out_platform_index) {
  if (out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (port > FR_HOST_UART_MAX_PORT || !fr_host_uart_baud_valid(baud) ||
      fr_host_uart_port_in_use(port) || tx == rx ||
      fr_host_uart_pin_conflict(tx, rx)) {
    return FR_ERR_DOMAIN;
  }
#if FR_FEATURE_CONSOLE_ROUTING
  if (fr_host_console_pin_conflict(tx, rx)) {
    return FR_ERR_DOMAIN;
  }
#endif

  for (uint16_t i = 0; i < FR_PROFILE_MAX_HANDLES; i++) {
    fr_host_uart_t *uart = &fr_host_uarts[i];

    if (uart->in_use) {
      continue;
    }

    *uart = (fr_host_uart_t){
        .in_use = true,
        .custom_pins = true,
        .port = port,
        .tx = tx,
        .rx = rx,
        .baud = baud,
    };
    *out_platform_index = i;
    return FR_OK;
  }

  return FR_ERR_CAPACITY;
}

fr_err_t fr_platform_uart_write_byte(uint16_t platform_index, uint8_t byte) {
  fr_host_uart_t *uart = NULL;

  FR_TRY(fr_host_uart_entry(platform_index, &uart));
  uart->last_written = byte;
  uart->write_count = (uint16_t)(uart->write_count + 1u);
  return FR_OK;
}

fr_err_t fr_platform_uart_read_byte(uint16_t platform_index, uint8_t *out_byte,
                                    bool *out_has_byte) {
  fr_host_uart_t *uart = NULL;

  if (out_byte == NULL || out_has_byte == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_host_uart_entry(platform_index, &uart));
  if (uart->read_index >= FR_HOST_UART_SCRIPT_LENGTH) {
    *out_has_byte = false;
    *out_byte = 0;
    return FR_OK;
  }

  *out_byte = fr_host_uart_script[uart->read_index];
  uart->read_index = (uint8_t)(uart->read_index + 1u);
  *out_has_byte = true;
  return FR_OK;
}

fr_err_t fr_platform_uart_available(uint16_t platform_index,
                                    uint16_t *out_count) {
  fr_host_uart_t *uart = NULL;

  if (out_count == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_host_uart_entry(platform_index, &uart));
  *out_count = (uint16_t)(FR_HOST_UART_SCRIPT_LENGTH - uart->read_index);
  return FR_OK;
}
#endif

#if FR_FEATURE_CONSOLE_ROUTING
fr_err_t fr_platform_console_set_uart(uint16_t tx, uint16_t rx,
                                      uint32_t baud) {
  if (tx > FR_HOST_MAX_PIN || rx > FR_HOST_MAX_PIN || tx == rx || baud == 0 ||
      baud > FR_HOST_UART_BAUD_MAX) {
    return FR_ERR_DOMAIN;
  }
#if FR_FEATURE_UART
  if (fr_host_uart_pin_conflict(tx, rx)) {
    return FR_ERR_DOMAIN;
  }
#endif
  if (fr_host_console_fail_switch_next) {
    fr_host_console_fail_switch_next = false;
    return FR_ERR_IO;
  }

  fr_host_console_route = (fr_console_route_t){
      .transport = FR_CONSOLE_TRANSPORT_UART,
      .tx = tx,
      .rx = rx,
      .baud = baud,
  };
  return FR_OK;
}

fr_err_t fr_platform_console_restore_default(void) {
  fr_host_console_route = (fr_console_route_t){
      .transport = FR_CONSOLE_TRANSPORT_HOST,
  };
  return FR_OK;
}

fr_err_t fr_platform_console_get_route(fr_console_route_t *out_route) {
  if (out_route == NULL) {
    return FR_ERR_INVALID;
  }
  *out_route = fr_host_console_route;
  return FR_OK;
}

fr_err_t fr_platform_recovery_requested(uint16_t window_ms,
                                        bool *out_requested) {
  (void)window_ms;
  if (out_requested == NULL) {
    return FR_ERR_INVALID;
  }
  *out_requested = fr_host_recovery_next;
  fr_host_recovery_next = false;
  return FR_OK;
}

#ifdef FR_HOST_TEST_HELPERS
void fr_host_console_reset(void) {
  fr_host_console_route = (fr_console_route_t){
      .transport = FR_CONSOLE_TRANSPORT_HOST,
  };
  fr_host_recovery_next = false;
  fr_host_console_fail_switch_next = false;
}

void fr_host_request_recovery(void) { fr_host_recovery_next = true; }

void fr_host_console_fail_next_switch(void) {
  fr_host_console_fail_switch_next = true;
}
#endif
#endif

#if FR_FEATURE_REPL
/* Host read blocks on fgets with no poll loop, so there is no idle window in
 * which to service events; registration is a no-op. */
void fr_platform_set_idle_handler(fr_platform_idle_fn handler, void *ctx) {
  (void)handler;
  (void)ctx;
}

fr_err_t fr_platform_read_line(char *line, uint16_t cap, bool *out_eof) {
  size_t length = 0;

  if (line == NULL || cap == 0 || out_eof == NULL) {
    return FR_ERR_INVALID;
  }

  *out_eof = false;
  if (fgets(line, cap, stdin) == NULL) {
    if (feof(stdin)) {
      line[0] = '\0';
      *out_eof = true;
      return FR_OK;
    }
    return FR_ERR_IO;
  }

  length = strlen(line);
  if (length > 0 && line[length - 1] == '\n') {
    line[length - 1] = '\0';
    length -= 1;
  }
  if (length > 0 && line[length - 1] == '\r') {
    line[length - 1] = '\0';
    length -= 1;
  }
  if (length + 1 >= cap && !feof(stdin)) {
    return FR_ERR_RANGE;
  }

  return FR_OK;
}

fr_err_t fr_platform_write_text(const char *text) {
  if (text == NULL) {
    return FR_ERR_INVALID;
  }
  if (fputs(text, stdout) == EOF || fflush(stdout) == EOF) {
    return FR_ERR_IO;
  }
  return FR_OK;
}
#endif

#if FR_FEATURE_RANDOM
/* xorshift32 (Marsaglia 13/17/5). Zero state locks; the seed setter swaps 0
 * to 1 to avoid it. */
static uint32_t fr_host_random_state = 1;

uint32_t fr_platform_random_next(void) {
  uint32_t state = fr_host_random_state;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  fr_host_random_state = state;
  return state;
}

void fr_platform_random_seed(uint32_t seed) {
  fr_host_random_state = seed == 0u ? 1u : seed;
}
#endif

#if FR_FEATURE_PWM
fr_err_t fr_platform_pwm_open(uint16_t pin, uint16_t freq,
                              uint16_t *out_platform_index) {
  if (out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (pin > FR_HOST_MAX_PIN || freq == 0 || fr_host_pwm_pin_in_use(pin)) {
    return FR_ERR_DOMAIN;
  }

  for (uint16_t i = 0; i < FR_PROFILE_MAX_HANDLES; i++) {
    fr_host_pwm_t *pwm = &fr_host_pwms[i];

    if (pwm->in_use) {
      continue;
    }

    *pwm = (fr_host_pwm_t){
        .in_use = true,
        .pin = pin,
        .freq = freq,
    };
    *out_platform_index = i;
    return FR_OK;
  }

  return FR_ERR_CAPACITY;
}

fr_err_t fr_platform_pwm_write(uint16_t platform_index, uint16_t duty) {
  fr_host_pwm_t *pwm = NULL;

  FR_TRY(fr_host_pwm_entry(platform_index, &pwm));
  pwm->write_ring[pwm->write_head] = duty;
  pwm->write_head = (uint8_t)((pwm->write_head + 1u) % FR_HOST_PWM_RING_CAP);
  if (pwm->write_count < FR_HOST_PWM_RING_CAP) {
    pwm->write_count += 1u;
  }
  return FR_OK;
}

fr_err_t fr_platform_pwm_close(uint16_t platform_index) {
  if (platform_index >= FR_PROFILE_MAX_HANDLES) {
    return FR_OK;
  }
  memset(&fr_host_pwms[platform_index], 0, sizeof(fr_host_pwms[platform_index]));
  return FR_OK;
}

#ifdef FR_HOST_TEST_HELPERS
uint16_t fr_host_pwm_drain_writes(uint16_t platform_index, uint16_t *out_duties,
                                  uint16_t max_count) {
  if (platform_index >= FR_PROFILE_MAX_HANDLES ||
      !fr_host_pwms[platform_index].in_use || out_duties == NULL) {
    return 0;
  }

  fr_host_pwm_t *pwm = &fr_host_pwms[platform_index];
  uint16_t avail = pwm->write_count;
  uint16_t take = avail < max_count ? avail : max_count;
  uint8_t oldest =
      (uint8_t)((pwm->write_head + FR_HOST_PWM_RING_CAP - pwm->write_count) %
                FR_HOST_PWM_RING_CAP);

  for (uint16_t i = 0; i < take; i++) {
    out_duties[i] = pwm->write_ring[oldest];
    oldest = (uint8_t)((oldest + 1u) % FR_HOST_PWM_RING_CAP);
  }
  pwm->write_head = 0;
  pwm->write_count = 0;
  return take;
}
#endif
#endif

#if FR_FEATURE_I2C
fr_err_t fr_platform_i2c_open(uint16_t port, uint16_t sda, uint16_t scl,
                              uint32_t freq, uint16_t *out_platform_index) {
  if (out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (port > FR_HOST_I2C_MAX_PORT || sda > FR_HOST_MAX_PIN ||
      scl > FR_HOST_MAX_PIN || sda == scl || freq == 0 ||
      fr_host_i2c_port_in_use(port) || fr_host_i2c_pin_in_use(sda, scl)) {
    return FR_ERR_DOMAIN;
  }

  for (uint16_t i = 0; i < FR_PROFILE_MAX_HANDLES; i++) {
    fr_host_i2c_t *i2c = &fr_host_i2cs[i];

    if (i2c->in_use) {
      continue;
    }

    *i2c = (fr_host_i2c_t){
        .in_use = true,
        .port = port,
        .sda = sda,
        .scl = scl,
        .freq = freq,
    };
    *out_platform_index = i;
    return FR_OK;
  }

  return FR_ERR_CAPACITY;
}

fr_err_t fr_platform_i2c_write(uint16_t platform_index, uint8_t addr,
                               const uint8_t *bytes, uint16_t length) {
  fr_host_i2c_t *i2c = NULL;

  if (addr > FR_HOST_I2C_ADDR_MAX) {
    return FR_ERR_DOMAIN;
  }
  if (bytes == NULL && length > 0) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_i2c_entry(platform_index, &i2c));
  (void)i2c;
  return FR_OK;
}

/* Deterministic per-address pattern so tests can assert exact bytes without
 * an i2c slave. */
fr_err_t fr_platform_i2c_read(uint16_t platform_index, uint8_t addr,
                              uint8_t *bytes, uint16_t length) {
  fr_host_i2c_t *i2c = NULL;

  if (addr > FR_HOST_I2C_ADDR_MAX) {
    return FR_ERR_DOMAIN;
  }
  if (bytes == NULL && length > 0) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_i2c_entry(platform_index, &i2c));
  (void)i2c;
  for (uint16_t i = 0; i < length; i++) {
    bytes[i] = (uint8_t)(addr + i);
  }
  return FR_OK;
}

fr_err_t fr_platform_i2c_write_read(uint16_t platform_index, uint8_t addr,
                                    const uint8_t *wbytes, uint16_t wlength,
                                    uint8_t *rbytes, uint16_t rlength) {
  fr_host_i2c_t *i2c = NULL;

  if (addr > FR_HOST_I2C_ADDR_MAX) {
    return FR_ERR_DOMAIN;
  }
  if ((wbytes == NULL && wlength > 0) || (rbytes == NULL && rlength > 0)) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_i2c_entry(platform_index, &i2c));

  for (uint16_t i = 0; i < wlength; i++) {
    i2c->write_ring[i2c->write_head] = wbytes[i];
    i2c->write_head =
        (uint8_t)((i2c->write_head + 1u) % FR_HOST_I2C_RING_CAP);
    if (i2c->write_count < FR_HOST_I2C_RING_CAP) {
      i2c->write_count += 1u;
    }
  }

  for (uint16_t i = 0; i < rlength; i++) {
    if (i2c->read_count == 0) {
      rbytes[i] = 0;
      continue;
    }
    uint8_t oldest =
        (uint8_t)((i2c->read_head + FR_HOST_I2C_RING_CAP - i2c->read_count) %
                  FR_HOST_I2C_RING_CAP);
    rbytes[i] = i2c->read_queue[oldest];
    i2c->read_count -= 1u;
  }
  return FR_OK;
}

#ifdef FR_HOST_TEST_HELPERS
uint16_t fr_host_i2c_drain_writes(uint16_t platform_index, uint8_t *out_bytes,
                                  uint16_t max_length) {
  if (platform_index >= FR_PROFILE_MAX_HANDLES ||
      !fr_host_i2cs[platform_index].in_use || out_bytes == NULL) {
    return 0;
  }

  fr_host_i2c_t *i2c = &fr_host_i2cs[platform_index];
  uint16_t avail = i2c->write_count;
  uint16_t take = avail < max_length ? avail : max_length;
  uint8_t oldest =
      (uint8_t)((i2c->write_head + FR_HOST_I2C_RING_CAP - i2c->write_count) %
                FR_HOST_I2C_RING_CAP);

  for (uint16_t i = 0; i < take; i++) {
    out_bytes[i] = i2c->write_ring[oldest];
    oldest = (uint8_t)((oldest + 1u) % FR_HOST_I2C_RING_CAP);
  }
  i2c->write_head = 0;
  i2c->write_count = 0;
  return take;
}

void fr_host_i2c_queue_read(uint16_t platform_index, const uint8_t *bytes,
                            uint16_t length) {
  if (platform_index >= FR_PROFILE_MAX_HANDLES ||
      !fr_host_i2cs[platform_index].in_use || (bytes == NULL && length > 0)) {
    return;
  }

  fr_host_i2c_t *i2c = &fr_host_i2cs[platform_index];
  for (uint16_t i = 0; i < length; i++) {
    i2c->read_queue[i2c->read_head] = bytes[i];
    i2c->read_head =
        (uint8_t)((i2c->read_head + 1u) % FR_HOST_I2C_RING_CAP);
    if (i2c->read_count < FR_HOST_I2C_RING_CAP) {
      i2c->read_count += 1u;
    }
  }
}
#endif

fr_err_t fr_platform_i2c_close(uint16_t platform_index) {
  if (platform_index >= FR_PROFILE_MAX_HANDLES) {
    return FR_OK;
  }
  memset(&fr_host_i2cs[platform_index], 0, sizeof(fr_host_i2cs[platform_index]));
  return FR_OK;
}
#endif

fr_err_t fr_platform_write_bytes(const uint8_t *bytes, uint16_t length) {
  if (bytes == NULL && length > 0) {
    return FR_ERR_INVALID;
  }
  if (length > 0 && fwrite(bytes, 1, length, stdout) != length) {
    return FR_ERR_IO;
  }
  if (fflush(stdout) == EOF) {
    return FR_ERR_IO;
  }
  return FR_OK;
}

#if FR_FEATURE_PERSISTENCE
enum {
  FR_HOST_PERSIST_SLOT_COUNT = 2,
};

typedef struct fr_host_persist_slot_t {
  bool in_use;
  uint16_t length;
  uint8_t bytes[FR_PERSIST_STORAGE_BYTES];
} fr_host_persist_slot_t;

static fr_host_persist_slot_t fr_host_persist_slots[FR_HOST_PERSIST_SLOT_COUNT];
static uint8_t fr_host_persist_active_mount_slot;
static uint8_t fr_host_persist_candidate_mount_slot;
static bool fr_host_persist_active_mounted;
static bool fr_host_persist_candidate_mounted;
static struct {
  bool active;
  uint8_t slot;
  uint16_t cursor;
  uint32_t backend_generation;
} fr_host_persist_stream;

#ifdef FR_HOST_TEST_HELPERS
static bool fr_host_persist_interrupt_header_write;
static bool fr_host_persist_fail_next_mount_commit;
static bool fr_host_persist_shadow_mounts;
static bool fr_host_persist_direct_code_pointers = true;
static uint8_t fr_host_persist_mount_maps[2][FR_PERSIST_STORAGE_BYTES];
static uint16_t fr_host_persist_mount_map_lengths[2];
static uint8_t fr_host_persist_active_map;
static uint8_t fr_host_persist_candidate_map;
static bool fr_host_persist_active_map_mounted;
static bool fr_host_persist_candidate_map_mounted;
#endif

static fr_err_t fr_host_persist_slot_info(uint8_t slot,
                                          fr_persist_format_info_t *out) {
  if (slot >= FR_HOST_PERSIST_SLOT_COUNT || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_host_persist_slots[slot].in_use) {
    return FR_ERR_NOT_FOUND;
  }
  return fr_persist_format_validate(fr_host_persist_slots[slot].bytes,
                                    fr_host_persist_slots[slot].length, out);
}

static fr_err_t fr_host_persist_pick_read_slot(
    uint8_t image_index, uint8_t *out_slot, fr_persist_format_info_t *out_info) {
  fr_persist_format_info_t info[FR_HOST_PERSIST_SLOT_COUNT];
  bool valid[FR_HOST_PERSIST_SLOT_COUNT] = {false, false};
  bool saw_corrupt = false;
  uint8_t slots[FR_HOST_PERSIST_SLOT_COUNT] = {0, 1};
  uint8_t valid_count = 0;

  if (out_slot == NULL || out_info == NULL) {
    return FR_ERR_INVALID;
  }

  for (uint8_t slot = 0; slot < FR_HOST_PERSIST_SLOT_COUNT; slot++) {
    fr_err_t err = fr_host_persist_slot_info(slot, &info[slot]);
    valid[slot] = err == FR_OK;
    if (err != FR_OK && err != FR_ERR_NOT_FOUND) {
      saw_corrupt = true;
    }
  }

  if (valid[0] && valid[1]) {
    if (info[1].backend_generation > info[0].backend_generation) {
      slots[0] = 1;
      slots[1] = 0;
    }
    valid_count = 2;
  } else if (valid[1]) {
    slots[0] = 1;
    valid_count = 1;
  } else if (valid[0]) {
    valid_count = 1;
  } else {
    return saw_corrupt ? FR_ERR_CORRUPT : FR_ERR_NOT_FOUND;
  }

  if (image_index >= valid_count) {
    return FR_ERR_NOT_FOUND;
  }
  *out_slot = slots[image_index];
  *out_info = info[*out_slot];
  return FR_OK;
}

static fr_err_t fr_host_persist_pick_commit_slot(uint8_t *out_slot,
                                                 uint32_t *out_generation) {
  fr_persist_format_info_t info[FR_HOST_PERSIST_SLOT_COUNT];
  bool valid[FR_HOST_PERSIST_SLOT_COUNT] = {false, false};

  if (out_slot == NULL || out_generation == NULL) {
    return FR_ERR_INVALID;
  }

  for (uint8_t slot = 0; slot < FR_HOST_PERSIST_SLOT_COUNT; slot++) {
    valid[slot] = fr_host_persist_slot_info(slot, &info[slot]) == FR_OK;
  }

  if (valid[0] && valid[1]) {
    if (info[0].backend_generation <= info[1].backend_generation) {
      *out_slot = 0;
      *out_generation = info[1].backend_generation + 1u;
    } else {
      *out_slot = 1;
      *out_generation = info[0].backend_generation + 1u;
    }
  } else if (valid[0]) {
    *out_slot = 1;
    *out_generation = info[0].backend_generation + 1u;
  } else if (valid[1]) {
    *out_slot = 0;
    *out_generation = info[1].backend_generation + 1u;
  } else {
    *out_slot = 0;
    *out_generation = 1;
  }
  return FR_OK;
}

static bool fr_host_persist_pointer_in_range(const uint8_t *bytes,
                                             uint16_t bytes_length,
                                             const void *ptr,
                                             uint16_t length) {
  uintptr_t p = (uintptr_t)ptr;
  uintptr_t b = 0;

  if (length == 0) {
    return true;
  }
  if (bytes == NULL || ptr == NULL) {
    return false;
  }
  b = (uintptr_t)bytes;
  if (p < b) {
    return false;
  }
  return (uint32_t)(p - b) + length <= bytes_length;
}

static bool fr_host_persist_pointer_in_slot(uint8_t slot, const void *ptr,
                                            uint16_t length) {
  if (slot >= FR_HOST_PERSIST_SLOT_COUNT) {
    return false;
  }
  return fr_host_persist_pointer_in_range(fr_host_persist_slots[slot].bytes,
                                          fr_host_persist_slots[slot].length,
                                          ptr, length);
}

static fr_err_t fr_host_persist_offset_in_range(const uint8_t *bytes,
                                                uint16_t bytes_length,
                                                const void *ptr,
                                                uint16_t length,
                                                uint16_t *out_offset) {
  uintptr_t p = (uintptr_t)ptr;
  uintptr_t b = (uintptr_t)bytes;

  if (out_offset == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_host_persist_pointer_in_range(bytes, bytes_length, ptr, length)) {
    return FR_ERR_NOT_FOUND;
  }
  *out_offset = (uint16_t)(p - b);
  return FR_OK;
}

fr_err_t fr_platform_persist_read(uint8_t *bytes, uint16_t cap,
                                  uint16_t *out_length, uint8_t image_index) {
  uint8_t slot = 0;
  fr_persist_format_info_t info = {0};

  if (bytes == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_persist_pick_read_slot(image_index, &slot, &info));
  if (info.total_length > UINT16_MAX) {
    return FR_ERR_CAPACITY;
  }
  if (cap < info.total_length) {
    return FR_ERR_CAPACITY;
  }

  memcpy(bytes, fr_host_persist_slots[slot].bytes, (uint16_t)info.total_length);
  *out_length = (uint16_t)info.total_length;
  return FR_OK;
}

fr_err_t fr_platform_persist_mount(uint8_t image_index,
                                   const uint8_t **out_bytes,
                                   uint16_t *out_length) {
  uint8_t slot = 0;
  fr_persist_format_info_t info = {0};

  if (out_bytes == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }
  fr_platform_persist_mount_discard();
  FR_TRY(fr_host_persist_pick_read_slot(image_index, &slot, &info));
  if (info.total_length > UINT16_MAX) {
    return FR_ERR_CAPACITY;
  }

  fr_host_persist_candidate_mount_slot = slot;
  fr_host_persist_candidate_mounted = true;
#ifdef FR_HOST_TEST_HELPERS
  if (fr_host_persist_shadow_mounts) {
    uint8_t map = fr_host_persist_active_map_mounted
                      ? (uint8_t)(1u - fr_host_persist_active_map)
                      : 0u;

    memcpy(fr_host_persist_mount_maps[map], fr_host_persist_slots[slot].bytes,
           (uint16_t)info.total_length);
    fr_host_persist_mount_map_lengths[map] = (uint16_t)info.total_length;
    fr_host_persist_candidate_map = map;
    fr_host_persist_candidate_map_mounted = true;
    *out_bytes = fr_host_persist_mount_maps[map];
    *out_length = (uint16_t)info.total_length;
    return FR_OK;
  }
  fr_host_persist_candidate_map_mounted = false;
#endif
  *out_bytes = fr_host_persist_slots[slot].bytes;
  *out_length = (uint16_t)info.total_length;
  return FR_OK;
}

void fr_platform_persist_unmount(void) {
  fr_platform_persist_mount_discard();
  fr_host_persist_active_mounted = false;
#ifdef FR_HOST_TEST_HELPERS
  fr_host_persist_active_map_mounted = false;
#endif
}

fr_err_t fr_platform_persist_mount_commit(void) {
  if (!fr_host_persist_candidate_mounted) {
    return FR_ERR_INVALID;
  }
#ifdef FR_HOST_TEST_HELPERS
  if (fr_host_persist_fail_next_mount_commit) {
    fr_host_persist_fail_next_mount_commit = false;
    return FR_ERR_IO;
  }
#endif
  fr_host_persist_active_mount_slot = fr_host_persist_candidate_mount_slot;
  fr_host_persist_active_mounted = true;
#ifdef FR_HOST_TEST_HELPERS
  if (fr_host_persist_candidate_map_mounted) {
    fr_host_persist_active_map = fr_host_persist_candidate_map;
    fr_host_persist_active_map_mounted = true;
    fr_host_persist_candidate_map_mounted = false;
  } else {
    fr_host_persist_active_map_mounted = false;
  }
#endif
  fr_host_persist_candidate_mounted = false;
  return FR_OK;
}

void fr_platform_persist_mount_discard(void) {
  fr_host_persist_candidate_mounted = false;
#ifdef FR_HOST_TEST_HELPERS
  fr_host_persist_candidate_map_mounted = false;
#endif
}

bool fr_platform_persist_pointer_is_mounted(const void *ptr, uint16_t length) {
  if (!fr_host_persist_active_mounted) {
    return false;
  }
#ifdef FR_HOST_TEST_HELPERS
  if (fr_host_persist_active_map_mounted) {
    return fr_host_persist_pointer_in_range(
        fr_host_persist_mount_maps[fr_host_persist_active_map],
        fr_host_persist_mount_map_lengths[fr_host_persist_active_map], ptr,
        length);
  }
#endif
  return fr_host_persist_pointer_in_slot(fr_host_persist_active_mount_slot, ptr,
                                        length);
}

bool fr_platform_persist_code_pointer_is_direct(const void *ptr,
                                                uint16_t length) {
#ifdef FR_HOST_TEST_HELPERS
  if (!fr_host_persist_direct_code_pointers) {
    (void)ptr;
    (void)length;
    return false;
  }
#endif
  if (fr_host_persist_candidate_mounted) {
#ifdef FR_HOST_TEST_HELPERS
    if (fr_host_persist_candidate_map_mounted) {
      return fr_host_persist_pointer_in_range(
          fr_host_persist_mount_maps[fr_host_persist_candidate_map],
          fr_host_persist_mount_map_lengths[fr_host_persist_candidate_map],
          ptr, length);
    }
#endif
    return fr_host_persist_pointer_in_slot(fr_host_persist_candidate_mount_slot,
                                           ptr, length);
  }
  return fr_platform_persist_pointer_is_mounted(ptr, length);
}

fr_err_t fr_platform_persist_mounted_offset(const void *ptr, uint16_t length,
                                            uint16_t *out_offset) {
  if (out_offset == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_host_persist_candidate_mounted) {
#ifdef FR_HOST_TEST_HELPERS
    if (fr_host_persist_candidate_map_mounted) {
      fr_err_t err = fr_host_persist_offset_in_range(
          fr_host_persist_mount_maps[fr_host_persist_candidate_map],
          fr_host_persist_mount_map_lengths[fr_host_persist_candidate_map],
          ptr, length, out_offset);
      if (err == FR_OK) {
        return FR_OK;
      }
    }
#endif
    {
      fr_host_persist_slot_t *slot =
          &fr_host_persist_slots[fr_host_persist_candidate_mount_slot];
      fr_err_t err = fr_host_persist_offset_in_range(
          slot->bytes, slot->length, ptr, length, out_offset);
      if (err == FR_OK) {
        return FR_OK;
      }
    }
  }
  if (fr_host_persist_active_mounted) {
#ifdef FR_HOST_TEST_HELPERS
    if (fr_host_persist_active_map_mounted) {
      fr_err_t err = fr_host_persist_offset_in_range(
          fr_host_persist_mount_maps[fr_host_persist_active_map],
          fr_host_persist_mount_map_lengths[fr_host_persist_active_map], ptr,
          length, out_offset);
      if (err == FR_OK) {
        return FR_OK;
      }
    }
#endif
    {
      fr_host_persist_slot_t *slot =
          &fr_host_persist_slots[fr_host_persist_active_mount_slot];
      return fr_host_persist_offset_in_range(slot->bytes, slot->length, ptr,
                                             length, out_offset);
    }
  }
  return FR_ERR_NOT_FOUND;
}

fr_err_t fr_platform_persist_read_mounted(uint16_t offset, uint8_t *dst,
                                          uint16_t length) {
  const uint8_t *bytes = NULL;
  uint16_t bytes_length = 0;

  if (dst == NULL && length > 0) {
    return FR_ERR_INVALID;
  }
  if (fr_host_persist_candidate_mounted) {
#ifdef FR_HOST_TEST_HELPERS
    if (fr_host_persist_candidate_map_mounted) {
      bytes = fr_host_persist_mount_maps[fr_host_persist_candidate_map];
      bytes_length =
          fr_host_persist_mount_map_lengths[fr_host_persist_candidate_map];
    } else
#endif
    {
      bytes = fr_host_persist_slots[fr_host_persist_candidate_mount_slot].bytes;
      bytes_length =
          fr_host_persist_slots[fr_host_persist_candidate_mount_slot].length;
    }
  } else if (fr_host_persist_active_mounted) {
#ifdef FR_HOST_TEST_HELPERS
    if (fr_host_persist_active_map_mounted) {
      bytes = fr_host_persist_mount_maps[fr_host_persist_active_map];
      bytes_length =
          fr_host_persist_mount_map_lengths[fr_host_persist_active_map];
    } else
#endif
    {
      bytes = fr_host_persist_slots[fr_host_persist_active_mount_slot].bytes;
      bytes_length =
          fr_host_persist_slots[fr_host_persist_active_mount_slot].length;
    }
  } else {
    return FR_ERR_NOT_FOUND;
  }
  if (offset > bytes_length || length > bytes_length - offset) {
    return FR_ERR_RANGE;
  }
  if (length > 0) {
    memcpy(dst, &bytes[offset], length);
  }
  return FR_OK;
}

fr_err_t fr_platform_persist_stream_begin(void) {
  uint8_t slot = 0;
  uint32_t backend_generation = 0;

  fr_platform_persist_stream_abort();
  FR_TRY(fr_host_persist_pick_commit_slot(&slot, &backend_generation));
  memset(fr_host_persist_slots[slot].bytes, 0xff,
         sizeof(fr_host_persist_slots[slot].bytes));
  fr_host_persist_slots[slot].length = FR_PERSIST_HEADER_BYTES;
  fr_host_persist_slots[slot].in_use = true;
  fr_host_persist_stream.active = true;
  fr_host_persist_stream.slot = slot;
  fr_host_persist_stream.cursor = FR_PERSIST_HEADER_BYTES;
  fr_host_persist_stream.backend_generation = backend_generation;
  return FR_OK;
}

fr_err_t fr_platform_persist_stream_write(const uint8_t *bytes,
                                          uint16_t length) {
  fr_host_persist_slot_t *slot = NULL;
  uint32_t next = 0;

  if (bytes == NULL && length > 0) {
    return FR_ERR_INVALID;
  }
  if (!fr_host_persist_stream.active ||
      fr_host_persist_stream.slot >= FR_HOST_PERSIST_SLOT_COUNT) {
    return FR_ERR_INVALID;
  }
  next = (uint32_t)fr_host_persist_stream.cursor + length;
  if (next > FR_PERSIST_STORAGE_BYTES) {
    return FR_ERR_CAPACITY;
  }
  slot = &fr_host_persist_slots[fr_host_persist_stream.slot];
  if (length > 0) {
    memcpy(&slot->bytes[fr_host_persist_stream.cursor], bytes, length);
  }
  fr_host_persist_stream.cursor = (uint16_t)next;
  slot->length = fr_host_persist_stream.cursor;
  return FR_OK;
}

fr_err_t fr_platform_persist_stream_finalize(
    const uint8_t header[FR_PERSIST_HEADER_BYTES]) {
  uint8_t stamped[FR_PERSIST_HEADER_BYTES];
  fr_host_persist_slot_t *slot = NULL;

  if (header == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_host_persist_stream.active ||
      fr_host_persist_stream.slot >= FR_HOST_PERSIST_SLOT_COUNT) {
    return FR_ERR_INVALID;
  }
  slot = &fr_host_persist_slots[fr_host_persist_stream.slot];
  memcpy(stamped, header, sizeof(stamped));
  FR_TRY(fr_persist_format_stamp_generation(
      stamped, fr_host_persist_stream.backend_generation));
#ifdef FR_HOST_TEST_HELPERS
  if (fr_host_persist_interrupt_header_write) {
    fr_host_persist_interrupt_header_write = false;
    fr_host_persist_stream.active = false;
    return FR_ERR_IO;
  }
#endif
  memcpy(slot->bytes, stamped, sizeof(stamped));
  slot->length = fr_host_persist_stream.cursor;
  slot->in_use = true;
  fr_host_persist_stream.active = false;
  return FR_OK;
}

void fr_platform_persist_stream_abort(void) {
  fr_host_persist_stream.active = false;
}

fr_err_t fr_platform_persist_clear(void) {
  fr_platform_persist_unmount();
  fr_platform_persist_stream_abort();
  for (uint8_t slot = 0; slot < FR_HOST_PERSIST_SLOT_COUNT; slot++) {
    memset(&fr_host_persist_slots[slot], 0,
           sizeof(fr_host_persist_slots[slot]));
  }
#ifdef FR_HOST_TEST_HELPERS
  fr_host_persist_interrupt_header_write = false;
  fr_host_persist_fail_next_mount_commit = false;
  fr_host_persist_shadow_mounts = false;
  fr_host_persist_direct_code_pointers = true;
  fr_host_persist_active_map_mounted = false;
  fr_host_persist_candidate_map_mounted = false;
#endif
  return FR_OK;
}

#ifdef FR_HOST_TEST_HELPERS
fr_err_t fr_host_persist_debug_corrupt_newest(uint16_t offset, uint8_t value) {
  uint8_t slot = 0;
  fr_persist_format_info_t info = {0};

  FR_TRY(fr_host_persist_pick_read_slot(0, &slot, &info));
  if (offset >= fr_host_persist_slots[slot].length) {
    return FR_ERR_RANGE;
  }
  fr_host_persist_slots[slot].bytes[offset] = value;
  return FR_OK;
}

void fr_host_persist_debug_interrupt_next_header_write(void) {
  fr_host_persist_interrupt_header_write = true;
}

void fr_host_persist_debug_fail_next_mount_commit(void) {
  fr_host_persist_fail_next_mount_commit = true;
}

void fr_host_persist_debug_shadow_mounts(bool enabled) {
  fr_host_persist_shadow_mounts = enabled;
}

void fr_host_persist_debug_direct_code_pointers(bool enabled) {
  fr_host_persist_direct_code_pointers = enabled;
}
#endif
#endif

fr_err_t fr_platform_event_gpio_install(fr_event_kind_t kind, uint16_t pin,
                                        uint16_t binding_index,
                                        uint16_t generation) {
  (void)kind;
  (void)pin;
  (void)binding_index;
  (void)generation;
  return FR_OK;
}

fr_err_t fr_platform_event_gpio_remove(uint16_t pin) {
  (void)pin;
  return FR_OK;
}

#ifdef FR_HOST_TEST_HELPERS
static bool fr_host_event_fail_next_timer_install;

void fr_host_event_debug_fail_next_timer_install(void) {
  fr_host_event_fail_next_timer_install = true;
}
#endif

fr_err_t fr_platform_event_timer_install(fr_event_kind_t kind, uint32_t ms,
                                         uint16_t binding_index,
                                         uint16_t generation) {
#ifdef FR_HOST_TEST_HELPERS
  if (fr_host_event_fail_next_timer_install) {
    fr_host_event_fail_next_timer_install = false;
    return FR_ERR_IO;
  }
#endif
  (void)kind;
  (void)ms;
  (void)binding_index;
  (void)generation;
  return FR_OK;
}

fr_err_t fr_platform_event_timer_remove(uint16_t binding_index) {
  (void)binding_index;
  return FR_OK;
}

/* Host event queue: ring of pending candidates the test native pushes and the
 * shared drain reads. Sized to match the runtime binding capacity so the
 * worst-case single-drain transfer fits. */
enum {
  FR_HOST_EVENT_QUEUE_CAP = 16,
};

static fr_event_candidate_t fr_host_event_queue[FR_HOST_EVENT_QUEUE_CAP];
static uint8_t fr_host_event_queue_head;
static uint8_t fr_host_event_queue_count;
static uint32_t fr_host_event_overflow;

fr_err_t fr_platform_event_drain(fr_event_candidate_t *out_events,
                                 uint8_t out_cap, uint8_t *out_count,
                                 uint32_t *overflow_delta) {
  uint8_t transfer = fr_host_event_queue_count;

  if (out_count == NULL || overflow_delta == NULL) {
    return FR_ERR_INVALID;
  }
  if (out_events == NULL && out_cap > 0) {
    return FR_ERR_INVALID;
  }
  if (transfer > out_cap) {
    transfer = out_cap;
  }
  for (uint8_t i = 0; i < transfer; i++) {
    out_events[i] = fr_host_event_queue[fr_host_event_queue_head];
    fr_host_event_queue_head =
        (uint8_t)((fr_host_event_queue_head + 1u) % FR_HOST_EVENT_QUEUE_CAP);
  }
  fr_host_event_queue_count = (uint8_t)(fr_host_event_queue_count - transfer);
  *out_count = transfer;
  *overflow_delta = fr_host_event_overflow;
  fr_host_event_overflow = 0;
  return FR_OK;
}

fr_err_t fr_platform_event_post_test_candidate(uint16_t binding_index,
                                               uint16_t generation,
                                               uint32_t timestamp_ms) {
  uint8_t tail;

  if (fr_host_event_queue_count == FR_HOST_EVENT_QUEUE_CAP) {
    fr_host_event_overflow++;
    return FR_ERR_CAPACITY;
  }
  tail = (uint8_t)((fr_host_event_queue_head + fr_host_event_queue_count) %
                   FR_HOST_EVENT_QUEUE_CAP);
  fr_host_event_queue[tail] = (fr_event_candidate_t){
      .binding_index = binding_index,
      .generation = generation,
      .timestamp_ms = timestamp_ms,
  };
  fr_host_event_queue_count = (uint8_t)(fr_host_event_queue_count + 1u);
  return FR_OK;
}

#if FR_FEATURE_NET
enum {
  FR_HOST_WIFI_SSID_CAP = 32,
  FR_HOST_WIFI_PASS_CAP = 64,
};

static char fr_host_wifi_ssid[FR_HOST_WIFI_SSID_CAP + 1];
static char fr_host_wifi_pass[FR_HOST_WIFI_PASS_CAP + 1];
static bool fr_host_wifi_ready_flag;

typedef struct fr_host_http_response_t {
  bool queued;
  uint16_t status;
  uint16_t length;
  uint8_t body[FR_HTTP_MAX_BODY];
} fr_host_http_response_t;

static fr_host_http_response_t fr_host_http_response;

fr_err_t fr_platform_wifi_save(const char *ssid, const char *pass) {
  if (ssid == NULL || pass == NULL) {
    return FR_ERR_INVALID;
  }

  size_t ssid_len = strlen(ssid);
  size_t pass_len = strlen(pass);
  if (ssid_len == 0 || ssid_len > FR_HOST_WIFI_SSID_CAP ||
      pass_len > FR_HOST_WIFI_PASS_CAP) {
    return FR_ERR_DOMAIN;
  }

  memcpy(fr_host_wifi_ssid, ssid, ssid_len + 1);
  memcpy(fr_host_wifi_pass, pass, pass_len + 1);
  return FR_OK;
}

fr_err_t fr_platform_wifi_connect(fr_runtime_t *runtime) {
  /* D7: host stub flips state to connected. Saved creds gate the call so an
   * unconfigured fixture surfaces the right error. */
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_host_wifi_ssid[0] == '\0') {
    return FR_ERR_NET_DISCONNECTED;
  }
  fr_host_wifi_ready_flag = true;
  return FR_OK;
}

fr_err_t fr_platform_wifi_ready(bool *out_ready) {
  if (out_ready == NULL) {
    return FR_ERR_INVALID;
  }
  *out_ready = fr_host_wifi_ready_flag;
  return FR_OK;
}

fr_err_t fr_platform_http_get(const char *url, uint8_t *out_body, uint16_t cap,
                              uint16_t *out_length) {
  if (url == NULL || out_length == NULL || (out_body == NULL && cap > 0)) {
    return FR_ERR_INVALID;
  }
  if (url[0] == '\0') {
    return FR_ERR_NET_PROTOCOL;
  }
  if (!fr_host_wifi_ready_flag) {
    return FR_ERR_NET_DISCONNECTED;
  }
  if (!fr_host_http_response.queued) {
    return FR_ERR_NET_REFUSED;
  }

  uint16_t length = fr_host_http_response.length;
  uint16_t status = fr_host_http_response.status;
  fr_host_http_response.queued = false;

  if (length > cap) {
    *out_length = 0;
    return FR_ERR_NET_TOO_LARGE;
  }
  if (status < 200u || status >= 300u) {
    *out_length = 0;
    return FR_ERR_NET_REFUSED;
  }
  if (length > 0) {
    memcpy(out_body, fr_host_http_response.body, length);
  }
  *out_length = length;
  return FR_OK;
}

/* T15b D17 host TCP. queued gates open success per the SPEC: a slot must be
 * pre-staged via fr_host_tcp_queue_response, otherwise open returns
 * FR_ERR_NET_REFUSED. Magic hostnames "fr.test.dns" / "fr.test.timeout"
 * route open to FR_ERR_NET_DNS / FR_ERR_NET_TIMEOUT for the matching
 * acceptance #10 paths. Ring cap matches the per-profile text length so a
 * single tcp.read: can drain a full queued response without truncation. */
enum {
  FR_HOST_TCP_RING_CAP = FR_PROFILE_MAX_TEXT_LENGTH,
};

typedef struct fr_host_tcp_t {
  bool in_use;
  bool queued;
  bool wifi_down;
  uint16_t rx_head;
  uint16_t rx_count;
  uint16_t tx_head;
  uint16_t tx_count;
  uint8_t rx_ring[FR_HOST_TCP_RING_CAP];
  uint8_t tx_ring[FR_HOST_TCP_RING_CAP];
} fr_host_tcp_t;

static fr_host_tcp_t fr_host_tcps[FR_TCP_HANDLE_COUNT];

static fr_err_t fr_host_tcp_check_alive(fr_runtime_t *runtime,
                                        uint16_t platform_index) {
  if (platform_index >= FR_TCP_HANDLE_COUNT ||
      !fr_host_tcps[platform_index].in_use) {
    return FR_ERR_HANDLE;
  }
  if (runtime->tcp_handles[platform_index].failed) {
    return FR_ERR_NET_DISCONNECTED;
  }
  if (fr_host_tcps[platform_index].wifi_down) {
    runtime->tcp_handles[platform_index].failed = true;
    return FR_ERR_NET_DISCONNECTED;
  }
  return FR_OK;
}

fr_err_t fr_platform_tcp_open(fr_runtime_t *runtime, const char *host,
                              uint16_t port, uint16_t *out_platform_index) {
  if (runtime == NULL || host == NULL || out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (port == 0) {
    return FR_ERR_DOMAIN;
  }
  if (strcmp(host, "fr.test.dns") == 0) {
    return FR_ERR_NET_DNS;
  }
  if (strcmp(host, "fr.test.timeout") == 0) {
    return FR_ERR_NET_TIMEOUT;
  }
  for (uint16_t i = 0; i < FR_TCP_HANDLE_COUNT; i++) {
    if (fr_host_tcps[i].in_use) {
      continue;
    }
    if (!fr_host_tcps[i].queued) {
      continue;
    }
    fr_host_tcps[i].in_use = true;
    fr_host_tcps[i].wifi_down = false;
    fr_host_tcps[i].tx_head = 0;
    fr_host_tcps[i].tx_count = 0;
    runtime->tcp_handles[i].failed = false;
    *out_platform_index = i;
    return FR_OK;
  }
  for (uint16_t i = 0; i < FR_TCP_HANDLE_COUNT; i++) {
    if (!fr_host_tcps[i].in_use) {
      return FR_ERR_NET_REFUSED;
    }
  }
  return FR_ERR_CAPACITY;
}

fr_err_t fr_platform_tcp_read(fr_runtime_t *runtime, uint16_t platform_index,
                              uint8_t *out_bytes, uint16_t cap,
                              uint16_t *out_length) {
  fr_host_tcp_t *slot = NULL;
  uint16_t take = 0;

  if (runtime == NULL || out_length == NULL || (out_bytes == NULL && cap > 0)) {
    return FR_ERR_INVALID;
  }
  if (fr_runtime_is_interrupted(runtime)) {
    return FR_ERR_INTERRUPTED;
  }
  FR_TRY(fr_host_tcp_check_alive(runtime, platform_index));
  slot = &fr_host_tcps[platform_index];
  take = slot->rx_count < cap ? slot->rx_count : cap;
  for (uint16_t i = 0; i < take; i++) {
    out_bytes[i] = slot->rx_ring[slot->rx_head];
    slot->rx_head =
        (uint16_t)((slot->rx_head + 1u) % FR_HOST_TCP_RING_CAP);
  }
  slot->rx_count = (uint16_t)(slot->rx_count - take);
  *out_length = take;
  return FR_OK;
}

fr_err_t fr_platform_tcp_write(fr_runtime_t *runtime, uint16_t platform_index,
                               const uint8_t *bytes, uint16_t length) {
  fr_host_tcp_t *slot = NULL;

  if (runtime == NULL || (bytes == NULL && length > 0)) {
    return FR_ERR_INVALID;
  }
  if (fr_runtime_is_interrupted(runtime)) {
    return FR_ERR_INTERRUPTED;
  }
  FR_TRY(fr_host_tcp_check_alive(runtime, platform_index));
  slot = &fr_host_tcps[platform_index];
  for (uint16_t i = 0; i < length; i++) {
    slot->tx_ring[(uint16_t)((slot->tx_head + slot->tx_count) %
                             FR_HOST_TCP_RING_CAP)] = bytes[i];
    if (slot->tx_count < FR_HOST_TCP_RING_CAP) {
      slot->tx_count = (uint16_t)(slot->tx_count + 1u);
    } else {
      slot->tx_head =
          (uint16_t)((slot->tx_head + 1u) % FR_HOST_TCP_RING_CAP);
    }
  }
  return FR_OK;
}

fr_err_t fr_platform_tcp_close(uint16_t platform_index) {
  if (platform_index < FR_TCP_HANDLE_COUNT) {
    memset(&fr_host_tcps[platform_index], 0,
           sizeof(fr_host_tcps[platform_index]));
  }
  return FR_OK;
}

fr_err_t fr_platform_tcp_bytes_ready(fr_runtime_t *runtime,
                                     uint16_t platform_index,
                                     uint16_t *out_count) {
  if (runtime == NULL || out_count == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_host_tcp_check_alive(runtime, platform_index));
  *out_count = fr_host_tcps[platform_index].rx_count;
  return FR_OK;
}

typedef struct fr_host_wifi_slot_t {
  uint16_t binding_index;
  uint16_t generation;
  bool active;
} fr_host_wifi_slot_t;

/* One slot per wifi kind; FR_EVENT_KIND_WIFI_DISCONNECTED → 0,
 * FR_EVENT_KIND_WIFI_RECONNECTED → 1. */
static fr_host_wifi_slot_t fr_host_wifi_slots[2];

static uint8_t fr_host_wifi_slot_index(fr_event_kind_t kind) {
  return kind == FR_EVENT_KIND_WIFI_DISCONNECTED ? 0u : 1u;
}

fr_err_t fr_platform_event_wifi_install(fr_event_kind_t kind,
                                        uint16_t binding_index,
                                        uint16_t generation) {
  uint8_t i;
  if (kind != FR_EVENT_KIND_WIFI_DISCONNECTED &&
      kind != FR_EVENT_KIND_WIFI_RECONNECTED) {
    return FR_ERR_INVALID;
  }
  i = fr_host_wifi_slot_index(kind);
  fr_host_wifi_slots[i].binding_index = binding_index;
  fr_host_wifi_slots[i].generation = generation;
  fr_host_wifi_slots[i].active = true;
  return FR_OK;
}

fr_err_t fr_platform_event_wifi_remove(uint16_t binding_index) {
  for (uint8_t i = 0; i < 2; i++) {
    if (fr_host_wifi_slots[i].active &&
        fr_host_wifi_slots[i].binding_index == binding_index) {
      fr_host_wifi_slots[i].active = false;
      fr_host_wifi_slots[i].binding_index = 0;
      fr_host_wifi_slots[i].generation = 0;
    }
  }
  return FR_OK;
}

#ifdef FR_HOST_TEST_HELPERS
void fr_host_wifi_set_connected(bool connected) {
  fr_host_wifi_ready_flag = connected;
}

void fr_host_http_queue_response(uint16_t status, const uint8_t *body,
                                 uint16_t length) {
  uint16_t copy = length > FR_HTTP_MAX_BODY ? FR_HTTP_MAX_BODY : length;

  fr_host_http_response.queued = true;
  fr_host_http_response.status = status;
  fr_host_http_response.length = length;
  if (copy > 0 && body != NULL) {
    memcpy(fr_host_http_response.body, body, copy);
  }
}

void fr_host_wifi_fire_event(fr_event_kind_t kind) {
  const fr_host_wifi_slot_t *slot;
  if (kind != FR_EVENT_KIND_WIFI_DISCONNECTED &&
      kind != FR_EVENT_KIND_WIFI_RECONNECTED) {
    return;
  }
  slot = &fr_host_wifi_slots[fr_host_wifi_slot_index(kind)];
  if (!slot->active) {
    return;
  }
  /* fr_event_drain drops candidates with timestamp_ms < registered_at_ms
   * (src/event.c). Match the registration clock so a fixture that called
   * fr_platform_delay_ms before binding still receives the candidate. */
  (void)fr_platform_event_post_test_candidate(slot->binding_index,
                                              slot->generation,
                                              fr_host_millis);
}

void fr_host_tcp_queue_response(uint16_t handle_platform_index,
                                const uint8_t *bytes, uint16_t length) {
  fr_host_tcp_t *slot = NULL;

  if (handle_platform_index >= FR_TCP_HANDLE_COUNT ||
      (bytes == NULL && length > 0)) {
    return;
  }
  slot = &fr_host_tcps[handle_platform_index];
  slot->queued = true;
  for (uint16_t i = 0; i < length; i++) {
    slot->rx_ring[(uint16_t)((slot->rx_head + slot->rx_count) %
                             FR_HOST_TCP_RING_CAP)] = bytes[i];
    if (slot->rx_count < FR_HOST_TCP_RING_CAP) {
      slot->rx_count = (uint16_t)(slot->rx_count + 1u);
    } else {
      slot->rx_head =
          (uint16_t)((slot->rx_head + 1u) % FR_HOST_TCP_RING_CAP);
    }
  }
}

fr_err_t fr_host_tcp_drain_writes(uint16_t handle_platform_index,
                                  uint8_t *out_bytes, uint16_t cap,
                                  uint16_t *out_length) {
  fr_host_tcp_t *slot = NULL;
  uint16_t take = 0;

  if (out_length == NULL || (out_bytes == NULL && cap > 0)) {
    return FR_ERR_INVALID;
  }
  if (handle_platform_index >= FR_TCP_HANDLE_COUNT ||
      !fr_host_tcps[handle_platform_index].in_use) {
    return FR_ERR_HANDLE;
  }
  slot = &fr_host_tcps[handle_platform_index];
  take = slot->tx_count < cap ? slot->tx_count : cap;
  for (uint16_t i = 0; i < take; i++) {
    out_bytes[i] = slot->tx_ring[slot->tx_head];
    slot->tx_head =
        (uint16_t)((slot->tx_head + 1u) % FR_HOST_TCP_RING_CAP);
  }
  slot->tx_count = (uint16_t)(slot->tx_count - take);
  *out_length = take;
  return FR_OK;
}

void fr_host_tcp_force_disconnect(uint16_t handle_platform_index) {
  if (handle_platform_index >= FR_TCP_HANDLE_COUNT) {
    return;
  }
  fr_host_tcps[handle_platform_index].wifi_down = true;
}

void fr_host_net_reset(void) {
  uint8_t i;
  memset(fr_host_wifi_ssid, 0, sizeof(fr_host_wifi_ssid));
  memset(fr_host_wifi_pass, 0, sizeof(fr_host_wifi_pass));
  fr_host_wifi_ready_flag = false;
  memset(&fr_host_http_response, 0, sizeof(fr_host_http_response));
  for (i = 0; i < 2; i++) {
    fr_host_wifi_slots[i].binding_index = 0;
    fr_host_wifi_slots[i].generation = 0;
    fr_host_wifi_slots[i].active = false;
  }
  memset(fr_host_tcps, 0, sizeof(fr_host_tcps));
  fr_host_event_queue_head = 0;
  fr_host_event_queue_count = 0;
  fr_host_event_overflow = 0;
}

#endif
#endif

#if FR_FEATURE_POWER
/* T14 D17. arm and feed are pure no-ops: the kernel owns the armed
 * state (target_defs.c), so the host has nothing to record for the
 * arm/re-arm/feed paths. sleep records the args from the most recent
 * sleep.deep: call and the pending GPIO config so D19's tests can read
 * what was passed. */
static uint32_t fr_host_sleep_captured_ms;
static uint16_t fr_host_sleep_pending_pin;
static uint16_t fr_host_sleep_pending_level;
static bool fr_host_sleep_pending;

/* RTC-capable wake pins on ESP32; ext0 wake accepts these only
 * (esp_sleep.h:262-267). Host mirrors the list so D19's reject-non-RTC
 * test runs against the same set the device enforces. */
static bool fr_host_sleep_is_rtc_pin(uint16_t pin) {
  switch (pin) {
  case 0:
  case 2:
  case 4:
  case 12:
  case 13:
  case 14:
  case 15:
  case 25:
  case 26:
  case 27:
  case 32:
  case 33:
  case 34:
  case 35:
  case 36:
  case 37:
  case 38:
  case 39:
    return true;
  default:
    return false;
  }
}

fr_err_t fr_platform_watchdog_arm(uint32_t timeout_ms) {
  (void)timeout_ms;
  return FR_OK;
}

fr_err_t fr_platform_watchdog_feed(void) { return FR_OK; }

fr_err_t fr_platform_sleep_deep(uint32_t ms) {
  if (ms == 0 && !fr_host_sleep_pending) {
    return FR_ERR_INVALID;
  }
  fr_host_sleep_captured_ms = ms;
  /* The pending config is consumed by this call; user must re-configure
   * after the simulated cold-boot (D12). Captures of pin/level stay
   * readable so D19 can assert what was used. */
  fr_host_sleep_pending = false;
  return FR_OK;
}

fr_err_t fr_platform_sleep_wake_on_gpio(uint16_t pin, uint16_t level) {
  if (!fr_host_sleep_is_rtc_pin(pin)) {
    return FR_ERR_INVALID;
  }
  if (level > 1) {
    return FR_ERR_INVALID;
  }
  fr_host_sleep_pending = true;
  fr_host_sleep_pending_pin = pin;
  fr_host_sleep_pending_level = level;
  return FR_OK;
}

#ifdef FR_HOST_TEST_HELPERS
/* fr_host_watchdog_force_timeout lives in targets/common/target_defs.c
 * since it needs to clear the kernel armed flag (file-static there). */
void fr_host_sleep_deep_captures(uint32_t *out_ms, uint16_t *out_pin,
                                 uint16_t *out_level) {
  if (out_ms != NULL) {
    *out_ms = fr_host_sleep_captured_ms;
  }
  if (out_pin != NULL) {
    *out_pin = fr_host_sleep_pending_pin;
  }
  if (out_level != NULL) {
    *out_level = fr_host_sleep_pending_level;
  }
}
#endif
#endif
