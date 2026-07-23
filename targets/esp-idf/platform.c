#include "platform.h"

#include "board.h"
#include "crc.h"
#include "handle.h"
#include "persist_format.h"
#include "runtime.h"

#include "driver/gpio.h"
#if FR_FEATURE_I2C
#include "driver/i2c_master.h"
#endif
#if FR_FEATURE_TRACE
#include "driver/mcpwm_cap.h"
#endif
#if FR_FEATURE_PULSE
#include "driver/rmt_common.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "hal/rmt_types.h"
#endif
#if FR_FEATURE_PWM
#include "driver/ledc.h"
#endif
#if defined(FR_BOARD_CONSOLE_UART) || FR_FEATURE_UART ||                     \
    FR_FEATURE_CONSOLE_ROUTING
#include "driver/uart.h"
#endif
#if defined(FR_BOARD_CONSOLE_USB_SERIAL_JTAG)
#include "driver/usb_serial_jtag.h"
#endif
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#if FR_FEATURE_POWER
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#endif
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "soc/soc_caps.h"

#include <stddef.h>
#include <string.h>

#if defined(FR_BOARD_CONSOLE_UART) &&                                    \
    defined(FR_BOARD_CONSOLE_USB_SERIAL_JTAG)
#error "board must select exactly one Frothy console"
#elif !defined(FR_BOARD_CONSOLE_UART) &&                                 \
    !defined(FR_BOARD_CONSOLE_USB_SERIAL_JTAG)
#error "board must select a Frothy console"
#endif

enum {
  FR_ESP_CONSOLE_RX_BYTES = 256,
  FR_ESP_CONSOLE_TX_BYTES = 256,
  /* Bytes the safe-point interrupt poll may consume ahead of the console
     reader. Sized for a pasted console.read-line line; overflow drops the
     newest bytes. */
  FR_ESP_TYPEAHEAD_BYTES = 128,
#if FR_FEATURE_CONSOLE_ROUTING
  FR_ESP_CONSOLE_TX_WAIT_MS = 100,
#endif
#if FR_FEATURE_UART
  FR_ESP_APP_UART_RX_BYTES = 256,
  FR_ESP_APP_UART_TX_BYTES = 256,
#endif
  FR_ESP_CTRL_C = 3,
  FR_ESP_BACKSPACE = 8,
  FR_ESP_DELETE = 127,
  FR_ESP_PERSIST_SLOT_COUNT = 2,
#if FR_FEATURE_PERSISTENCE
  FR_ESP_PERSIST_PARTITION_SUBTYPE = 0x40,
  FR_ESP_FLASH_SECTOR_BYTES = 0x1000,
  FR_ESP_FLASH_WRITE_ALIGN_BYTES = 4,
  FR_ESP_PERSIST_READ_CHUNK_BYTES = 64,
  FR_ESP_PERSIST_SLOT_BYTES = 0x20000,
  FR_ESP_PERSIST_PARTITION_BYTES =
      FR_ESP_PERSIST_SLOT_COUNT * FR_ESP_PERSIST_SLOT_BYTES,
#endif
  FR_ESP_VM_YIELD_INTERVAL_US = 250000,
};

#if FR_FEATURE_PERSISTENCE
_Static_assert((FR_ESP_PERSIST_SLOT_BYTES % FR_ESP_FLASH_SECTOR_BYTES) == 0,
               "frothy persist slots must be sector aligned");
_Static_assert((uint32_t)FR_PERSIST_STORAGE_BYTES <=
                   (uint32_t)FR_ESP_PERSIST_SLOT_BYTES,
               "frothy persist slot is too small for the S1 envelope");
#endif

#if FR_FEATURE_UART
typedef struct fr_esp_app_uart_t {
  bool in_use;
  bool custom_pins; /* true only for uart.open-on; tx/rx hold the chosen pins */
  uint16_t tx;
  uint16_t rx;
  uint32_t baud;
} fr_esp_app_uart_t;

#if SOC_UART_HP_NUM <= 1
#error "FR_FEATURE_UART requires an ESP target with an application UART"
#endif

static const uart_port_t fr_esp_app_uart_ports[] = {
#if SOC_UART_HP_NUM > 1
    UART_NUM_1,
#endif
#if SOC_UART_HP_NUM > 2
    UART_NUM_2,
#endif
};

static fr_esp_app_uart_t
    fr_esp_app_uarts[sizeof(fr_esp_app_uart_ports) /
                     sizeof(fr_esp_app_uart_ports[0])];
#endif

#if FR_FEATURE_I2C
enum {
  FR_ESP_I2C_MAX = SOC_I2C_NUM,
  FR_ESP_I2C_ADDR_MAX = 0x7F,
};

typedef struct fr_esp_i2c_t {
  bool in_use;
  uint16_t port;
  uint16_t sda;
  uint16_t scl;
  uint32_t freq;
  i2c_master_bus_handle_t handle;
} fr_esp_i2c_t;

static fr_esp_i2c_t fr_esp_i2cs[FR_ESP_I2C_MAX];

static bool fr_esp_i2c_port_in_use(uint16_t port) {
  for (uint16_t i = 0; i < FR_ESP_I2C_MAX; i++) {
    if (fr_esp_i2cs[i].in_use && fr_esp_i2cs[i].port == port) {
      return true;
    }
  }
  return false;
}

static bool fr_esp_i2c_pin_in_use(uint16_t sda, uint16_t scl) {
  for (uint16_t i = 0; i < FR_ESP_I2C_MAX; i++) {
    const fr_esp_i2c_t *i2c = &fr_esp_i2cs[i];

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

static fr_err_t fr_esp_i2c_entry(uint16_t platform_index,
                                 fr_esp_i2c_t **out_i2c) {
  if (out_i2c == NULL) {
    return FR_ERR_INVALID;
  }
  if (platform_index >= FR_ESP_I2C_MAX || !fr_esp_i2cs[platform_index].in_use) {
    return FR_ERR_HANDLE;
  }

  *out_i2c = &fr_esp_i2cs[platform_index];
  return FR_OK;
}
#endif

#if FR_FEATURE_PWM
/* One LEDC timer per channel: timer sharing across channels is a deferred
 * optimization (see ADR 0046). Capacity is the timer count, not the channel
 * count. */
enum {
  FR_ESP_PWM_MAX = SOC_LEDC_TIMER_NUM,
  FR_ESP_PWM_DUTY_RESOLUTION_BITS = 10,
};

typedef struct fr_esp_pwm_t {
  bool in_use;
  ledc_channel_t channel;
  ledc_timer_t timer;
  uint16_t pin;
  uint16_t freq;
} fr_esp_pwm_t;

static fr_esp_pwm_t fr_esp_pwms[FR_ESP_PWM_MAX];

static bool fr_esp_pwm_pin_in_use(uint16_t pin) {
  for (uint16_t i = 0; i < FR_ESP_PWM_MAX; i++) {
    if (fr_esp_pwms[i].in_use && fr_esp_pwms[i].pin == pin) {
      return true;
    }
  }
  return false;
}

static fr_err_t fr_esp_pwm_entry(uint16_t platform_index,
                                 fr_esp_pwm_t **out_pwm) {
  if (out_pwm == NULL) {
    return FR_ERR_INVALID;
  }
  if (platform_index >= FR_ESP_PWM_MAX || !fr_esp_pwms[platform_index].in_use) {
    return FR_ERR_HANDLE;
  }

  *out_pwm = &fr_esp_pwms[platform_index];
  return FR_OK;
}
#endif

#if FR_FEATURE_TRACE
#if !SOC_MCPWM_SUPPORTED || SOC_MCPWM_CAPTURE_TIMERS_PER_GROUP < 1 ||       \
    SOC_MCPWM_CAPTURE_CHANNELS_PER_TIMER < FR_TRACE_CHANNEL_CAP
#error "FR_FEATURE_TRACE requires one MCPWM capture timer with three channels"
#endif

typedef struct fr_esp_trace_edge_t {
  uint32_t tick;
  uint8_t channel;
  uint8_t level;
} fr_esp_trace_edge_t;

typedef struct fr_esp_trace_channel_t {
  mcpwm_cap_channel_handle_t handle;
  uint16_t pin;
  uint8_t index;
  bool enabled;
} fr_esp_trace_channel_t;

typedef struct fr_esp_trace_t {
  bool in_use;
  fr_trace_state_t state;
  mcpwm_cap_timer_handle_t timer;
  bool timer_enabled;
  bool timer_running;
  fr_esp_trace_channel_t channels[FR_TRACE_CHANNEL_CAP];
  uint8_t channel_count;
  fr_esp_trace_edge_t edges[FR_TRACE_EVENT_CAP];
  uint16_t event_count;
  bool has_first_edge;
  uint32_t first_tick;
  /* ESP32 capture is fixed at 80 MHz. Keep that finer clock private and
   * expose every target on Frothy's 10 MHz / 100 ns signal grid. */
  uint32_t hardware_ticks_per_signal_tick;
  uint32_t hardware_max_tick;
  int64_t first_edge_us;
  bool sorted;
} fr_esp_trace_t;

static fr_esp_trace_t fr_esp_trace;
static portMUX_TYPE fr_esp_trace_mux = portMUX_INITIALIZER_UNLOCKED;

static fr_err_t fr_esp_trace_entry(uint16_t platform_index,
                                   fr_esp_trace_t **out_trace) {
  if (out_trace == NULL) {
    return FR_ERR_INVALID;
  }
  if (platform_index != 0 || !fr_esp_trace.in_use) {
    return FR_ERR_HANDLE;
  }

  *out_trace = &fr_esp_trace;
  return FR_OK;
}

static bool fr_esp_trace_edge_after(const fr_esp_trace_edge_t *a,
                                    const fr_esp_trace_edge_t *b) {
  return a->tick > b->tick ||
         (a->tick == b->tick && a->channel > b->channel);
}

static void fr_esp_trace_sort(fr_esp_trace_t *trace) {
  if (trace->sorted) {
    return;
  }
  for (uint16_t i = 1; i < trace->event_count; i++) {
    fr_esp_trace_edge_t edge = trace->edges[i];
    uint16_t at = i;

    while (at > 0 && fr_esp_trace_edge_after(&trace->edges[at - 1], &edge)) {
      trace->edges[at] = trace->edges[at - 1];
      at -= 1u;
    }
    trace->edges[at] = edge;
  }
  for (uint16_t i = 0; i < trace->event_count; i++) {
    uint32_t hardware_tick = trace->edges[i].tick;
    uint32_t signal_tick =
        hardware_tick / trace->hardware_ticks_per_signal_tick;

    if (hardware_tick % trace->hardware_ticks_per_signal_tick >=
        (trace->hardware_ticks_per_signal_tick + 1u) / 2u) {
      signal_tick += 1u;
    }
    trace->edges[i].tick = signal_tick;
  }
  trace->sorted = true;
}
#endif

#if FR_FEATURE_PULSE
#if !SOC_RMT_SUPPORTED || SOC_RMT_TX_CANDIDATES_PER_GROUP < 1
#error "FR_FEATURE_PULSE requires one RMT transmit channel"
#endif

enum {
  FR_ESP_RMT_DURATION_MAX = 32767,
  FR_ESP_PULSE_SYMBOL_CAP = 281,
};

_Static_assert(FR_ESP_PULSE_SYMBOL_CAP * 2 >= 562,
               "pulse symbol scratch must hold every bounded span split");

typedef struct fr_esp_pulse_t {
  bool in_use;
  bool enabled;
  uint16_t pin;
  uint8_t idle;
  rmt_channel_handle_t channel;
  rmt_encoder_handle_t encoder;
  fr_pulse_segment_t segments[FR_PULSE_SEGMENT_CAP];
  uint16_t segment_count;
  uint32_t total_ticks;
} fr_esp_pulse_t;

static fr_esp_pulse_t fr_esp_pulse;

static fr_err_t fr_esp_pulse_entry(uint16_t platform_index,
                                   fr_esp_pulse_t **out_pulse) {
  if (out_pulse == NULL) {
    return FR_ERR_INVALID;
  }
  if (platform_index != 0 || !fr_esp_pulse.in_use) {
    return FR_ERR_HANDLE;
  }

  *out_pulse = &fr_esp_pulse;
  return FR_OK;
}
#endif

static bool fr_esp_initialized;
/* Console typeahead ring: bytes the interrupt poll or the recovery window
   consumed from the driver that belong to the next console read. Fixed
   storage; the poll never grows memory. */
static uint8_t fr_esp_typeahead[FR_ESP_TYPEAHEAD_BYTES];
static uint8_t fr_esp_typeahead_start;
static uint8_t fr_esp_typeahead_count;
static void fr_esp_typeahead_clear(void);
static adc_oneshot_unit_handle_t fr_esp_adc1;
static bool fr_esp_adc1_initialized;
static int64_t fr_esp_last_vm_yield_us;

static fr_err_t fr_esp_err(esp_err_t err) {
  switch (err) {
  case ESP_OK:
    return FR_OK;
  case ESP_ERR_NO_MEM:
    return FR_ERR_CAPACITY;
  case ESP_ERR_NOT_FOUND:
  case ESP_ERR_NVS_NOT_FOUND:
    return FR_ERR_NOT_FOUND;
  case ESP_ERR_INVALID_ARG:
  case ESP_ERR_INVALID_STATE:
    return FR_ERR_INVALID;
  default:
    return FR_ERR_IO;
  }
}

static bool fr_esp_gpio_pin_valid(uint16_t pin) {
  return pin < GPIO_NUM_MAX && ((1ULL << pin) & SOC_GPIO_VALID_GPIO_MASK) != 0;
}

static bool fr_esp_gpio_output_valid(uint16_t pin) {
  return pin < GPIO_NUM_MAX &&
         ((1ULL << pin) & SOC_GPIO_VALID_OUTPUT_GPIO_MASK) != 0;
}

#if FR_FEATURE_UART || FR_FEATURE_CONSOLE_ROUTING
static bool fr_esp_uart_baud_valid(uint32_t baud) {
  return baud > 0 && baud <= UART_BITRATE_MAX;
}
#endif

#if FR_FEATURE_UART
static bool fr_esp_app_uart_pin_conflict(uint16_t tx, uint16_t rx);
#endif
static bool fr_esp_boot_button_pressed(void);

/* FR_ESP_CONSOLE_IMPL_BEGIN
 * One active human console. Application UART stays outside this block. */
#if defined(FR_BOARD_CONSOLE_UART)
_Static_assert(FR_BOARD_UART_PORT == UART_NUM_0,
               "Frothy reserves UART0 for ROM boot and safe boot");
#endif

#if defined(FR_BOARD_CONSOLE_UART) || FR_FEATURE_CONSOLE_ROUTING
static esp_err_t fr_esp_console_uart_configure(uint16_t tx, uint16_t rx,
                                              uint32_t baud) {
  const uart_config_t config = {
      .baud_rate = (int)baud,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  esp_err_t err = uart_param_config(UART_NUM_0, &config);

  if (err == ESP_OK) {
    err = uart_set_pin(UART_NUM_0, tx, rx, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
  }
  if (err == ESP_OK) {
    err = uart_set_mode(UART_NUM_0, UART_MODE_UART);
  }
  if (err == ESP_OK) {
    err = uart_flush_input(UART_NUM_0);
  }
  return err;
}

static esp_err_t fr_esp_console_uart_prepare(uint16_t tx, uint16_t rx,
                                            uint32_t baud) {
  bool installed = uart_is_driver_installed(UART_NUM_0);
  esp_err_t err = ESP_OK;

  if (!installed) {
    err = uart_driver_install(UART_NUM_0, FR_ESP_CONSOLE_RX_BYTES,
                              FR_ESP_CONSOLE_TX_BYTES, 0, NULL, 0);
    if (err != ESP_OK) {
      return err;
    }
  }

  err = fr_esp_console_uart_configure(tx, rx, baud);
  if (err != ESP_OK && !installed) {
    (void)uart_driver_delete(UART_NUM_0);
  }
  return err;
}
#endif

#if defined(FR_BOARD_CONSOLE_USB_SERIAL_JTAG)
#if !SOC_USB_SERIAL_JTAG_SUPPORTED
#error "board selects USB Serial/JTAG on an unsupported ESP target"
#endif
static fr_err_t fr_esp_console_usb_prepare(void) {
  usb_serial_jtag_driver_config_t config = {
      .tx_buffer_size = FR_ESP_CONSOLE_TX_BYTES,
      .rx_buffer_size = FR_ESP_CONSOLE_RX_BYTES,
  };

  if (usb_serial_jtag_is_driver_installed()) {
    return FR_OK;
  }
  return fr_esp_err(usb_serial_jtag_driver_install(&config));
}
#endif

#if FR_FEATURE_CONSOLE_ROUTING
#if defined(FR_BOARD_CONSOLE_UART)
static const fr_console_route_t fr_esp_console_default_route = {
    .transport = FR_CONSOLE_TRANSPORT_UART,
    .tx = FR_BOARD_UART_TX,
    .rx = FR_BOARD_UART_RX,
    .baud = FR_BOARD_UART_BAUD,
};
#elif defined(FR_BOARD_CONSOLE_USB_SERIAL_JTAG)
static const fr_console_route_t fr_esp_console_default_route = {
    .transport = FR_CONSOLE_TRANSPORT_USB,
};
#endif

static fr_console_route_t fr_esp_console_route;

static bool fr_esp_console_route_equal(const fr_console_route_t *a,
                                       const fr_console_route_t *b) {
  return a->transport == b->transport && a->tx == b->tx && a->rx == b->rx &&
         a->baud == b->baud;
}

static fr_err_t
fr_esp_console_wait_tx_done(const fr_console_route_t *route) {
  esp_err_t err = ESP_ERR_INVALID_ARG;
  TickType_t timeout = pdMS_TO_TICKS(FR_ESP_CONSOLE_TX_WAIT_MS);

  switch (route->transport) {
  case FR_CONSOLE_TRANSPORT_UART:
    err = uart_wait_tx_done(UART_NUM_0, timeout);
    break;
  case FR_CONSOLE_TRANSPORT_USB:
#if defined(FR_BOARD_CONSOLE_USB_SERIAL_JTAG)
    err = usb_serial_jtag_wait_tx_done(timeout);
    break;
#else
    return FR_ERR_INVALID;
#endif
  default:
    return FR_ERR_INVALID;
  }
  return fr_esp_err(err);
}

static bool fr_esp_console_pin_conflict(uint16_t tx, uint16_t rx) {
  return fr_esp_console_route.transport == FR_CONSOLE_TRANSPORT_UART &&
         (fr_esp_console_route.tx == tx || fr_esp_console_route.tx == rx ||
          fr_esp_console_route.rx == tx || fr_esp_console_route.rx == rx);
}
#endif

static fr_err_t fr_esp_console_init(void) {
#if defined(FR_BOARD_CONSOLE_UART)
  FR_TRY(fr_esp_err(fr_esp_console_uart_prepare(
      FR_BOARD_UART_TX, FR_BOARD_UART_RX, FR_BOARD_UART_BAUD)));
#elif defined(FR_BOARD_CONSOLE_USB_SERIAL_JTAG)
  FR_TRY(fr_esp_console_usb_prepare());
#endif
#if FR_FEATURE_CONSOLE_ROUTING
  fr_esp_console_route = fr_esp_console_default_route;
#endif
  fr_esp_typeahead_clear();
  return FR_OK;
}

static fr_err_t fr_esp_console_driver_read(uint8_t *out_byte,
                                           TickType_t timeout) {
  int read = 0;

#if FR_FEATURE_CONSOLE_ROUTING
  switch (fr_esp_console_route.transport) {
  case FR_CONSOLE_TRANSPORT_UART:
    read = uart_read_bytes(UART_NUM_0, out_byte, 1, timeout);
    break;
  case FR_CONSOLE_TRANSPORT_USB:
#if defined(FR_BOARD_CONSOLE_USB_SERIAL_JTAG)
    read = usb_serial_jtag_read_bytes(out_byte, 1, timeout);
    break;
#else
    return FR_ERR_INVALID;
#endif
  default:
    return FR_ERR_INVALID;
  }
#elif defined(FR_BOARD_CONSOLE_UART)
  read = uart_read_bytes(FR_BOARD_UART_PORT, out_byte, 1, timeout);
#elif defined(FR_BOARD_CONSOLE_USB_SERIAL_JTAG)
  read = usb_serial_jtag_read_bytes(out_byte, 1, timeout);
#endif
  if (read < 0) {
    return FR_ERR_IO;
  }
  return read == 1 ? FR_OK : FR_ERR_NOT_FOUND;
}

static fr_err_t fr_esp_console_write(const void *bytes, uint16_t length) {
  int written = 0;

  if (length == 0) {
    return FR_OK;
  }
#if FR_FEATURE_CONSOLE_ROUTING
  switch (fr_esp_console_route.transport) {
  case FR_CONSOLE_TRANSPORT_UART:
    written = uart_write_bytes(UART_NUM_0, bytes, length);
    break;
  case FR_CONSOLE_TRANSPORT_USB:
#if defined(FR_BOARD_CONSOLE_USB_SERIAL_JTAG)
    written = usb_serial_jtag_write_bytes(bytes, length, portMAX_DELAY);
    break;
#else
    return FR_ERR_INVALID;
#endif
  default:
    return FR_ERR_INVALID;
  }
#elif defined(FR_BOARD_CONSOLE_UART)
  written = uart_write_bytes(FR_BOARD_UART_PORT, bytes, length);
#elif defined(FR_BOARD_CONSOLE_USB_SERIAL_JTAG)
  written = usb_serial_jtag_write_bytes(bytes, length, portMAX_DELAY);
#endif
  return written == length ? FR_OK : FR_ERR_IO;
}

static void fr_esp_typeahead_push(uint8_t byte) {
  uint8_t slot = 0;

  if (fr_esp_typeahead_count >= FR_ESP_TYPEAHEAD_BYTES) {
    return; /* full: drop the newest, keep read order intact */
  }
  slot = (uint8_t)((fr_esp_typeahead_start + fr_esp_typeahead_count) %
                   FR_ESP_TYPEAHEAD_BYTES);
  fr_esp_typeahead[slot] = byte;
  fr_esp_typeahead_count++;
}

static void fr_esp_typeahead_clear(void) {
  fr_esp_typeahead_start = 0;
  fr_esp_typeahead_count = 0;
}

static fr_err_t fr_esp_console_read_byte(uint8_t *out_byte,
                                         TickType_t timeout) {
  if (out_byte == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_esp_typeahead_count > 0) {
    *out_byte = fr_esp_typeahead[fr_esp_typeahead_start];
    fr_esp_typeahead_start =
        (uint8_t)((fr_esp_typeahead_start + 1) % FR_ESP_TYPEAHEAD_BYTES);
    fr_esp_typeahead_count--;
    return FR_OK;
  }

  return fr_esp_console_driver_read(out_byte, timeout);
}

#if FR_FEATURE_CONSOLE_ROUTING
fr_err_t fr_platform_console_set_uart(uint16_t tx, uint16_t rx,
                                      uint32_t baud) {
  const fr_console_route_t target = {
      .transport = FR_CONSOLE_TRANSPORT_UART,
      .tx = tx,
      .rx = rx,
      .baud = baud,
  };
  const fr_console_route_t old = fr_esp_console_route;
  esp_err_t err = ESP_OK;

  if (!fr_esp_gpio_output_valid(tx) || !fr_esp_gpio_pin_valid(rx) ||
      tx == rx || !fr_esp_uart_baud_valid(baud)) {
    return FR_ERR_DOMAIN;
  }
#if FR_FEATURE_UART
  if (fr_esp_app_uart_pin_conflict(tx, rx)) {
    return FR_ERR_DOMAIN;
  }
#endif
  if (fr_esp_console_route_equal(&target, &old)) {
    return FR_OK;
  }

  FR_TRY(fr_esp_console_wait_tx_done(&old));
  err = fr_esp_console_uart_prepare(target.tx, target.rx, target.baud);
  if (err != ESP_OK) {
    if (old.transport == FR_CONSOLE_TRANSPORT_UART &&
        fr_esp_console_uart_prepare(old.tx, old.rx, old.baud) != ESP_OK) {
      return FR_ERR_IO;
    }
    return fr_esp_err(err);
  }

  fr_esp_console_route = target;
  fr_esp_typeahead_clear();
  return FR_OK;
}

fr_err_t fr_platform_console_restore_default(void) {
#if defined(FR_BOARD_CONSOLE_UART)
  return fr_platform_console_set_uart(fr_esp_console_default_route.tx,
                                      fr_esp_console_default_route.rx,
                                      fr_esp_console_default_route.baud);
#elif defined(FR_BOARD_CONSOLE_USB_SERIAL_JTAG)
  if (fr_esp_console_route_equal(&fr_esp_console_route,
                                 &fr_esp_console_default_route)) {
    return FR_OK;
  }
  FR_TRY(fr_esp_console_wait_tx_done(&fr_esp_console_route));
  fr_esp_console_route = fr_esp_console_default_route;
  fr_esp_typeahead_clear();
  return FR_OK;
#endif
}

fr_err_t fr_platform_console_get_route(fr_console_route_t *out_route) {
  if (out_route == NULL) {
    return FR_ERR_INVALID;
  }
  *out_route = fr_esp_console_route;
  return FR_OK;
}

fr_err_t fr_platform_recovery_requested(uint16_t window_ms,
                                        bool *out_requested) {
  const int64_t deadline =
      esp_timer_get_time() + (int64_t)window_ms * 1000;
  TickType_t poll_ticks = pdMS_TO_TICKS(10);

  if (out_requested == NULL) {
    return FR_ERR_INVALID;
  }
  *out_requested = false;
  if (poll_ticks == 0) {
    poll_ticks = 1;
  }

  while (esp_timer_get_time() < deadline) {
    uint8_t byte = 0;
    fr_err_t err = FR_OK;

    if (fr_esp_boot_button_pressed()) {
      *out_requested = true;
      return FR_OK;
    }
    err = fr_esp_console_driver_read(&byte, poll_ticks);
    if (err == FR_ERR_NOT_FOUND) {
      continue;
    }
    FR_TRY(err);
    if (byte == FR_ESP_CTRL_C) {
      *out_requested = true;
      return FR_OK;
    }
    fr_esp_typeahead_push(byte);
    return FR_OK;
  }
  return FR_OK;
}
#endif
/* FR_ESP_CONSOLE_IMPL_END */

#if FR_FEATURE_NET
static fr_err_t fr_esp_nvs_init(void) {
  esp_err_t err = nvs_flash_init();

  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    FR_TRY(fr_esp_err(nvs_flash_erase()));
    err = nvs_flash_init();
  }
  return fr_esp_err(err);
}
#endif

static fr_err_t fr_esp_boot_button_init(void) {
#ifdef FR_BOARD_BOOT_BUTTON
  const gpio_config_t config = {
      .pin_bit_mask = 1ULL << FR_BOARD_BOOT_BUTTON,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };

  return fr_esp_err(gpio_config(&config));
#else
  return FR_OK;
#endif
}

static bool fr_esp_boot_button_pressed(void) {
#ifdef FR_BOARD_BOOT_BUTTON
  return gpio_get_level((gpio_num_t)FR_BOARD_BOOT_BUTTON) == 0;
#else
  return false;
#endif
}

fr_err_t fr_esp_platform_init(void) {
  if (fr_esp_initialized) {
    return FR_OK;
  }

  FR_TRY(fr_esp_console_init());
  FR_TRY(fr_esp_boot_button_init());
#if FR_FEATURE_NET
  FR_TRY(fr_esp_nvs_init());
#endif
  fr_esp_initialized = true;
  return FR_OK;
}

fr_err_t fr_platform_restart(void) {
  esp_restart();
  return FR_ERR_IO;
}

fr_err_t fr_platform_delay_ms(uint16_t ms) {
  if (ms == 0) {
    return FR_OK;
  }

  vTaskDelay(pdMS_TO_TICKS(ms));
  return FR_OK;
}

fr_err_t fr_platform_delay_us(uint16_t us) {
  esp_rom_delay_us(us);
  return FR_OK;
}

fr_err_t fr_platform_millis(uint32_t *out_ms) {
  int64_t ms = 0;

  if (out_ms == NULL) {
    return FR_ERR_INVALID;
  }

  ms = esp_timer_get_time() / 1000;
  *out_ms = (uint32_t)ms;
  return FR_OK;
}

fr_err_t fr_platform_micros(uint32_t *out_us) {
  int64_t us = 0;

  if (out_us == NULL) {
    return FR_ERR_INVALID;
  }

  us = esp_timer_get_time();
  *out_us = (uint32_t)us;
  return FR_OK;
}

void fr_platform_yield(void) {
  int64_t now_us = esp_timer_get_time();

  if (fr_esp_last_vm_yield_us == 0) {
    fr_esp_last_vm_yield_us = now_us;
    return;
  }
  if (now_us - fr_esp_last_vm_yield_us < FR_ESP_VM_YIELD_INTERVAL_US) {
    return;
  }

  fr_esp_last_vm_yield_us = now_us;
  vTaskDelay(1);
}

#if FR_FEATURE_UART
static fr_err_t fr_esp_app_uart_entry(uint16_t platform_index,
                                      fr_esp_app_uart_t **out_uart,
                                      uart_port_t *out_port) {
  if (out_uart == NULL || out_port == NULL) {
    return FR_ERR_INVALID;
  }
  if (platform_index >=
          sizeof(fr_esp_app_uarts) / sizeof(fr_esp_app_uarts[0]) ||
      !fr_esp_app_uarts[platform_index].in_use) {
    return FR_ERR_HANDLE;
  }

  *out_uart = &fr_esp_app_uarts[platform_index];
  *out_port = fr_esp_app_uart_ports[platform_index];
  return FR_OK;
}

/* Custom-pin conflict checks for uart.open-on. The default uart.open path
 * does not set pins (the ESP-IDF driver uses per-port defaults), so it does
 * not participate in the conflict list. */
static bool fr_esp_app_uart_console_conflict(uint16_t tx, uint16_t rx) {
#if FR_FEATURE_CONSOLE_ROUTING
  return fr_esp_console_pin_conflict(tx, rx);
#else
#if defined(FR_BOARD_CONSOLE_UART)
  return tx == FR_BOARD_UART_TX || tx == FR_BOARD_UART_RX ||
         rx == FR_BOARD_UART_TX || rx == FR_BOARD_UART_RX;
#else
  (void)tx;
  (void)rx;
  return false;
#endif
#endif
}

static bool fr_esp_app_uart_pin_conflict(uint16_t tx, uint16_t rx) {
  for (uint16_t i = 0;
       i < sizeof(fr_esp_app_uarts) / sizeof(fr_esp_app_uarts[0]); i++) {
    const fr_esp_app_uart_t *uart = &fr_esp_app_uarts[i];

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
#endif

fr_err_t fr_platform_gpio_mode(uint16_t pin, uint16_t mode) {
  gpio_config_t config = {0};

  if (!fr_esp_gpio_pin_valid(pin)) {
    return FR_ERR_DOMAIN;
  }

  switch (mode) {
  case 0:
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    break;
  case 1:
    if (!fr_esp_gpio_output_valid(pin)) {
      return FR_ERR_DOMAIN;
    }
    config.mode = GPIO_MODE_INPUT_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    break;
  case 2:
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    break;
  default:
    return FR_ERR_DOMAIN;
  }

#if FR_FEATURE_PWM
  /* Same protection as gpio_write: gpio_config detaches the pin from LEDC.
   * Checked after argument validation so both targets agree that domain
   * errors win over busy. */
  if (fr_esp_pwm_pin_in_use(pin)) {
    return FR_ERR_BUSY;
  }
#endif

  config.intr_type = GPIO_INTR_DISABLE;
  config.pin_bit_mask = 1ULL << pin;
  return fr_esp_err(gpio_config(&config));
}

fr_err_t fr_platform_gpio_write(uint16_t pin, uint16_t value) {
  if (!fr_esp_gpio_output_valid(pin)) {
    return FR_ERR_DOMAIN;
  }
#if FR_FEATURE_PWM
  /* gpio_set_direction re-routes the GPIO matrix away from LEDC while the
   * channel keeps running -- refuse instead of silently killing the PWM
   * output (ADR 0067). */
  if (fr_esp_pwm_pin_in_use(pin)) {
    return FR_ERR_BUSY;
  }
#endif

  FR_TRY(fr_esp_err(gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT_OUTPUT)));
  return fr_esp_err(gpio_set_level((gpio_num_t)pin, value == 0 ? 0 : 1));
}

fr_err_t fr_platform_gpio_read(uint16_t pin, uint16_t *out_value) {
  if (out_value == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_esp_gpio_pin_valid(pin)) {
    return FR_ERR_DOMAIN;
  }

  *out_value = gpio_get_level((gpio_num_t)pin) == 0 ? 0 : 1;
  return FR_OK;
}

static bool fr_esp_adc1_channel_for_pin(uint16_t pin,
                                        adc_channel_t *out_channel) {
  adc_unit_t unit = ADC_UNIT_1;

  if (out_channel == NULL) {
    return false;
  }

  if (adc_oneshot_io_to_channel((int)pin, &unit, out_channel) != ESP_OK) {
    return false;
  }
  return unit == ADC_UNIT_1;
}

static fr_err_t fr_esp_adc1_init(void) {
  const adc_oneshot_unit_init_cfg_t init_config = {
      .unit_id = ADC_UNIT_1,
      .ulp_mode = ADC_ULP_MODE_DISABLE,
  };

  if (fr_esp_adc1_initialized) {
    return FR_OK;
  }

  FR_TRY(fr_esp_err(adc_oneshot_new_unit(&init_config, &fr_esp_adc1)));
  fr_esp_adc1_initialized = true;
  return FR_OK;
}

fr_err_t fr_platform_adc_read(uint16_t pin, uint16_t *out_value) {
  adc_channel_t channel = ADC_CHANNEL_0;
  const adc_oneshot_chan_cfg_t config = {
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  int raw = 0;

  if (out_value == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_esp_adc1_channel_for_pin(pin, &channel)) {
    return FR_ERR_DOMAIN;
  }

  FR_TRY(fr_esp_adc1_init());
  FR_TRY(fr_esp_err(adc_oneshot_config_channel(fr_esp_adc1, channel,
                                               &config)));
  FR_TRY(fr_esp_err(adc_oneshot_read(fr_esp_adc1, channel, &raw)));
  if (raw < 0 || raw > 16383) {
    return FR_ERR_RANGE;
  }

  *out_value = (uint16_t)raw;
  return FR_OK;
}

/* Cold path of the interrupt poll: at least one byte is buffered. Ctrl-C
   interrupts and abandons any input queued behind it; other bytes go to the
   typeahead ring so input typed or streamed ahead of a console read survives
   the poll. Kept out of line so the empty-buffer poll stays a single probe. */
static IRAM_ATTR __attribute__((noinline)) fr_err_t
fr_esp_console_drain(fr_runtime_t *runtime, uint8_t byte) {
  for (;;) {
    if (byte == FR_ESP_CTRL_C) {
      fr_esp_typeahead_clear();
      fr_runtime_interrupt(runtime);
      return FR_OK;
    }
    fr_esp_typeahead_push(byte);
    if (fr_esp_console_driver_read(&byte, 0) != FR_OK) {
      return FR_OK;
    }
  }
}

fr_err_t fr_platform_poll_interrupt(fr_runtime_t *runtime) {
  uint8_t byte = 0;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_esp_boot_button_pressed()) {
    fr_runtime_interrupt(runtime);
    return FR_OK;
  }
  /* Hot path: one driver probe, nothing else. Reading back through the
     typeahead here would re-examine the same saved byte on every poll and
     never reach the driver again, so one buffered character would wall off
     a later Ctrl-C until reset. The stashing drain lives out of line: with
     it inlined in this loop the poll measurably slows tight programs
     (repeat 2M spin: 6.25s vs 5.88s on esp32_devkit_v1). */
  if (fr_esp_console_driver_read(&byte, 0) != FR_OK) {
    return FR_OK;
  }
  return fr_esp_console_drain(runtime, byte);
}

fr_err_t fr_platform_heap_free(uint32_t *out_bytes) {
  if (out_bytes == NULL) {
    return FR_ERR_INVALID;
  }
  *out_bytes = (uint32_t)esp_get_free_heap_size();
  return FR_OK;
}

fr_err_t fr_platform_heap_largest(uint32_t *out_bytes) {
  if (out_bytes == NULL) {
    return FR_ERR_INVALID;
  }
  *out_bytes = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  return FR_OK;
}

#if FR_FEATURE_TRACE
static bool IRAM_ATTR fr_esp_trace_capture(
    mcpwm_cap_channel_handle_t channel_handle,
    const mcpwm_capture_event_data_t *event, void *user_ctx) {
  fr_esp_trace_channel_t *channel = user_ctx;
  uint32_t hardware_delta = 0;

  (void)channel_handle;
  if (channel == NULL || event == NULL) {
    return false;
  }

  portENTER_CRITICAL_ISR(&fr_esp_trace_mux);
  if (fr_esp_trace.state != FR_TRACE_ARMED) {
    portEXIT_CRITICAL_ISR(&fr_esp_trace_mux);
    return false;
  }
  if (!fr_esp_trace.has_first_edge) {
    fr_esp_trace.has_first_edge = true;
    fr_esp_trace.first_tick = event->cap_value;
    fr_esp_trace.first_edge_us = esp_timer_get_time();
  }
  hardware_delta = event->cap_value - fr_esp_trace.first_tick;
  if (hardware_delta > fr_esp_trace.hardware_max_tick ||
      fr_esp_trace.event_count >= FR_TRACE_EVENT_CAP) {
    fr_esp_trace.state = FR_TRACE_COMPLETE;
    portEXIT_CRITICAL_ISR(&fr_esp_trace_mux);
    return false;
  }

  fr_esp_trace.edges[fr_esp_trace.event_count] = (fr_esp_trace_edge_t){
      .tick = hardware_delta,
      .channel = channel->index,
      .level = event->cap_edge == MCPWM_CAP_EDGE_POS ? 1 : 0,
  };
  fr_esp_trace.event_count += 1u;
  if (fr_esp_trace.event_count == FR_TRACE_EVENT_CAP ||
      hardware_delta == fr_esp_trace.hardware_max_tick) {
    fr_esp_trace.state = FR_TRACE_COMPLETE;
  }
  portEXIT_CRITICAL_ISR(&fr_esp_trace_mux);
  return false;
}

static fr_err_t fr_esp_trace_quiet(fr_esp_trace_t *trace) {
  fr_err_t first_error = FR_OK;

  if (trace->timer_running) {
    fr_err_t err = fr_esp_err(mcpwm_capture_timer_stop(trace->timer));

    if (err == FR_OK) {
      trace->timer_running = false;
    } else {
      first_error = err;
    }
  }
  for (uint8_t i = 0; i < trace->channel_count; i++) {
    fr_esp_trace_channel_t *channel = &trace->channels[i];

    if (!channel->enabled) {
      continue;
    }
    fr_err_t err = fr_esp_err(mcpwm_capture_channel_disable(channel->handle));
    if (err == FR_OK) {
      channel->enabled = false;
    } else if (first_error == FR_OK) {
      first_error = err;
    }
  }
  return first_error;
}

static fr_err_t fr_esp_trace_finish(fr_esp_trace_t *trace) {
  fr_err_t err = FR_OK;

  portENTER_CRITICAL(&fr_esp_trace_mux);
  trace->state = FR_TRACE_COMPLETE;
  portEXIT_CRITICAL(&fr_esp_trace_mux);
  err = fr_esp_trace_quiet(trace);
  fr_esp_trace_sort(trace);
  return err;
}

fr_err_t fr_platform_trace_open(uint16_t *out_platform_index) {
  mcpwm_cap_timer_handle_t timer = NULL;
  uint32_t resolution_hz = 0;
  esp_err_t err = ESP_ERR_NOT_FOUND;

  if (out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_esp_trace.in_use) {
    return FR_ERR_CAPACITY;
  }

  for (int group_id = 0; group_id < SOC_MCPWM_GROUPS; group_id++) {
    const mcpwm_capture_timer_config_t config = {
        .group_id = group_id,
        .clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT,
        .resolution_hz = 0,
    };

    err = mcpwm_new_capture_timer(&config, &timer);
    if (err == ESP_OK) {
      break;
    }
    if (err != ESP_ERR_NOT_FOUND) {
      return fr_esp_err(err);
    }
  }
  if (timer == NULL) {
    return FR_ERR_CAPACITY;
  }
  err = mcpwm_capture_timer_get_resolution(timer, &resolution_hz);
  if (err != ESP_OK || resolution_hz < FR_SIGNAL_CLOCK_HZ ||
      resolution_hz % FR_SIGNAL_CLOCK_HZ != 0) {
    (void)mcpwm_del_capture_timer(timer);
    return err == ESP_OK ? FR_ERR_UNSUPPORTED : fr_esp_err(err);
  }

  memset(&fr_esp_trace, 0, sizeof(fr_esp_trace));
  fr_esp_trace.in_use = true;
  fr_esp_trace.state = FR_TRACE_CONFIGURING;
  fr_esp_trace.timer = timer;
  fr_esp_trace.hardware_ticks_per_signal_tick =
      resolution_hz / FR_SIGNAL_CLOCK_HZ;
  if (fr_esp_trace.hardware_ticks_per_signal_tick >
      UINT32_MAX / FR_SIGNAL_MAX_TICKS) {
    (void)mcpwm_del_capture_timer(timer);
    memset(&fr_esp_trace, 0, sizeof(fr_esp_trace));
    return FR_ERR_UNSUPPORTED;
  }
  fr_esp_trace.hardware_max_tick =
      FR_SIGNAL_MAX_TICKS * fr_esp_trace.hardware_ticks_per_signal_tick;
  *out_platform_index = 0;
  return FR_OK;
}

fr_err_t fr_platform_trace_watch(uint16_t platform_index, uint16_t pin,
                                 uint8_t *out_channel) {
  fr_esp_trace_t *trace = NULL;
  fr_esp_trace_channel_t *channel = NULL;
  mcpwm_cap_channel_handle_t handle = NULL;
  esp_err_t err = ESP_OK;

  if (out_channel == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_esp_trace_entry(platform_index, &trace));
  if (trace->state != FR_TRACE_CONFIGURING || !fr_esp_gpio_pin_valid(pin)) {
    return FR_ERR_DOMAIN;
  }
  for (uint8_t i = 0; i < trace->channel_count; i++) {
    if (trace->channels[i].pin == pin) {
      return FR_ERR_DOMAIN;
    }
  }
  if (trace->channel_count >= FR_TRACE_CHANNEL_CAP) {
    return FR_ERR_CAPACITY;
  }

  const mcpwm_capture_channel_config_t config = {
      .gpio_num = pin,
      .prescale = 1,
      .flags.pos_edge = true,
      .flags.neg_edge = true,
  };
  const mcpwm_capture_event_callbacks_t callbacks = {
      .on_cap = fr_esp_trace_capture,
  };

  err = mcpwm_new_capture_channel(trace->timer, &config, &handle);
  if (err != ESP_OK) {
    return err == ESP_ERR_NOT_FOUND ? FR_ERR_CAPACITY : fr_esp_err(err);
  }
  channel = &trace->channels[trace->channel_count];
  *channel = (fr_esp_trace_channel_t){
      .handle = handle,
      .pin = pin,
      .index = trace->channel_count,
  };
  err = mcpwm_capture_channel_register_event_callbacks(
      handle, &callbacks, channel);
  if (err != ESP_OK) {
    (void)mcpwm_del_capture_channel(handle);
    memset(channel, 0, sizeof(*channel));
    return fr_esp_err(err);
  }

  *out_channel = trace->channel_count;
  trace->channel_count += 1u;
  return FR_OK;
}

fr_err_t fr_platform_trace_arm(uint16_t platform_index) {
  fr_esp_trace_t *trace = NULL;
  fr_trace_state_t prior_state = FR_TRACE_CONFIGURING;
  esp_err_t err = ESP_OK;

  FR_TRY(fr_esp_trace_entry(platform_index, &trace));
  portENTER_CRITICAL(&fr_esp_trace_mux);
  prior_state = trace->state;
  portEXIT_CRITICAL(&fr_esp_trace_mux);
  if ((prior_state != FR_TRACE_CONFIGURING &&
       prior_state != FR_TRACE_COMPLETE) ||
      trace->channel_count == 0) {
    return FR_ERR_DOMAIN;
  }
  if (prior_state == FR_TRACE_COMPLETE) {
    FR_TRY(fr_esp_trace_quiet(trace));
  }

  for (uint8_t i = 0; i < trace->channel_count; i++) {
    err = mcpwm_capture_channel_enable(trace->channels[i].handle);
    if (err != ESP_OK) {
      goto arm_failed;
    }
    trace->channels[i].enabled = true;
  }
  if (!trace->timer_enabled) {
    err = mcpwm_capture_timer_enable(trace->timer);
    if (err != ESP_OK) {
      goto arm_failed;
    }
    trace->timer_enabled = true;
  }

  err = mcpwm_capture_timer_start(trace->timer);
  if (err != ESP_OK) {
    goto arm_failed;
  }
  trace->timer_running = true;

  /* The callback ignores edges until ARMED. Starting first preserves the old
   * completed capture if hardware start fails; this transition begins capture. */
  portENTER_CRITICAL(&fr_esp_trace_mux);
  trace->event_count = 0;
  trace->has_first_edge = false;
  trace->first_tick = 0;
  trace->first_edge_us = 0;
  trace->sorted = false;
  trace->state = FR_TRACE_ARMED;
  portEXIT_CRITICAL(&fr_esp_trace_mux);
  return FR_OK;

arm_failed:
  portENTER_CRITICAL(&fr_esp_trace_mux);
  trace->state = prior_state;
  portEXIT_CRITICAL(&fr_esp_trace_mux);
  if (trace->timer_enabled) {
    if (mcpwm_capture_timer_disable(trace->timer) == ESP_OK) {
      trace->timer_enabled = false;
    }
  }
  for (uint8_t i = 0; i < trace->channel_count; i++) {
    if (trace->channels[i].enabled) {
      if (mcpwm_capture_channel_disable(trace->channels[i].handle) == ESP_OK) {
        trace->channels[i].enabled = false;
      }
    }
  }
  return fr_esp_err(err);
}

fr_err_t fr_platform_trace_stop(uint16_t platform_index) {
  fr_esp_trace_t *trace = NULL;
  fr_trace_state_t state = FR_TRACE_CONFIGURING;

  FR_TRY(fr_esp_trace_entry(platform_index, &trace));
  portENTER_CRITICAL(&fr_esp_trace_mux);
  state = trace->state;
  portEXIT_CRITICAL(&fr_esp_trace_mux);
  if (state == FR_TRACE_CONFIGURING) {
    return FR_ERR_DOMAIN;
  }
  return fr_esp_trace_finish(trace);
}

fr_err_t fr_platform_trace_status(uint16_t platform_index,
                                  fr_trace_status_t *out_status) {
  fr_esp_trace_t *trace = NULL;
  int64_t now_us = esp_timer_get_time();
  bool complete = false;

  if (out_status == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_esp_trace_entry(platform_index, &trace));
  portENTER_CRITICAL(&fr_esp_trace_mux);
  if (trace->state == FR_TRACE_ARMED && trace->has_first_edge &&
      now_us - trace->first_edge_us >= 1000000) {
    trace->state = FR_TRACE_COMPLETE;
  }
  complete = trace->state == FR_TRACE_COMPLETE;
  *out_status = (fr_trace_status_t){
      .state = trace->state,
      .channel_count = trace->channel_count,
      .event_count = trace->event_count,
  };
  for (uint8_t i = 0; i < trace->channel_count; i++) {
    out_status->pins[i] = trace->channels[i].pin;
  }
  portEXIT_CRITICAL(&fr_esp_trace_mux);

  if (complete) {
    FR_TRY(fr_esp_trace_quiet(trace));
    fr_esp_trace_sort(trace);
  }
  return FR_OK;
}

fr_err_t fr_platform_trace_event(uint16_t platform_index,
                                 uint16_t event_index,
                                 fr_trace_event_t *out_event) {
  fr_esp_trace_t *trace = NULL;
  fr_trace_status_t status = {0};

  if (out_event == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_esp_trace_entry(platform_index, &trace));
  FR_TRY(fr_platform_trace_status(platform_index, &status));
  if (status.state != FR_TRACE_COMPLETE) {
    return FR_ERR_DOMAIN;
  }
  if (event_index >= status.event_count) {
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
  fr_esp_trace_t *trace = NULL;
  fr_err_t first_error = FR_OK;

  FR_TRY(fr_esp_trace_entry(platform_index, &trace));
  portENTER_CRITICAL(&fr_esp_trace_mux);
  if (trace->state == FR_TRACE_ARMED) {
    trace->state = FR_TRACE_COMPLETE;
  }
  portEXIT_CRITICAL(&fr_esp_trace_mux);

  first_error = fr_esp_trace_quiet(trace);
  if (trace->timer_enabled) {
    fr_err_t err = fr_esp_err(mcpwm_capture_timer_disable(trace->timer));
    if (err == FR_OK) {
      trace->timer_enabled = false;
    } else if (first_error == FR_OK) {
      first_error = err;
    }
  }
  for (uint8_t i = 0; i < trace->channel_count; i++) {
    fr_esp_trace_channel_t *channel = &trace->channels[i];

    if (channel->enabled) {
      fr_err_t err =
          fr_esp_err(mcpwm_capture_channel_disable(channel->handle));
      if (err == FR_OK) {
        channel->enabled = false;
      } else if (first_error == FR_OK) {
        first_error = err;
      }
    }
    if (!channel->enabled && channel->handle != NULL) {
      fr_err_t err = fr_esp_err(mcpwm_del_capture_channel(channel->handle));
      if (err == FR_OK) {
        channel->handle = NULL;
      } else if (first_error == FR_OK) {
        first_error = err;
      }
    }
  }
  if (!trace->timer_enabled && trace->timer != NULL) {
    bool channels_deleted = true;

    for (uint8_t i = 0; i < trace->channel_count; i++) {
      if (trace->channels[i].handle != NULL) {
        channels_deleted = false;
      }
    }
    if (channels_deleted) {
      fr_err_t err = fr_esp_err(mcpwm_del_capture_timer(trace->timer));
      if (err == FR_OK) {
        trace->timer = NULL;
      } else if (first_error == FR_OK) {
        first_error = err;
      }
    }
  }
  /* Retain a partially closed entry so a later close can retry the driver
   * cleanup instead of losing the only references to live resources. */
  if (first_error == FR_OK && trace->timer == NULL) {
    memset(trace, 0, sizeof(*trace));
  }
  return first_error;
}
#endif

#if FR_FEATURE_PULSE
static esp_err_t fr_esp_pulse_set_idle(rmt_channel_handle_t channel,
                                       rmt_encoder_handle_t encoder,
                                       uint8_t idle) {
  const rmt_symbol_word_t symbol = {
      .duration0 = 1,
      .level0 = idle,
      .duration1 = 0,
      .level1 = idle,
  };
  const rmt_transmit_config_t config = {
      .loop_count = 0,
      .flags.eot_level = idle,
      .flags.queue_nonblocking = false,
  };
  esp_err_t err = rmt_enable(channel);
  esp_err_t disable_err = ESP_OK;

  if (err != ESP_OK) {
    return err;
  }
  err = rmt_transmit(channel, encoder, &symbol, sizeof(symbol), &config);
  if (err == ESP_OK) {
    err = rmt_tx_wait_all_done(channel, 100);
  }
  disable_err = rmt_disable(channel);
  return err != ESP_OK ? err : disable_err;
}

static fr_err_t fr_esp_pulse_pack(const fr_esp_pulse_t *pulse,
                                  rmt_symbol_word_t *symbols,
                                  uint16_t symbol_cap,
                                  uint16_t *out_symbol_count) {
  uint16_t symbol_count = 0;
  uint16_t half_count = 0;

  if (pulse == NULL || symbols == NULL || out_symbol_count == NULL) {
    return FR_ERR_INVALID;
  }

  for (uint16_t i = 0; i < pulse->segment_count; i++) {
    uint32_t ticks = pulse->segments[i].duration_ns / FR_SIGNAL_TICK_NS;

    while (ticks > 0) {
      uint16_t chunk = ticks > FR_ESP_RMT_DURATION_MAX
                           ? FR_ESP_RMT_DURATION_MAX
                           : (uint16_t)ticks;

      if ((half_count & 1u) == 0) {
        if (symbol_count >= symbol_cap) {
          return FR_ERR_CAPACITY;
        }
        symbols[symbol_count].val = 0;
        symbols[symbol_count].duration0 = chunk;
        symbols[symbol_count].level0 = pulse->segments[i].level;
      } else {
        symbols[symbol_count].duration1 = chunk;
        symbols[symbol_count].level1 = pulse->segments[i].level;
        symbol_count += 1u;
      }
      half_count += 1u;
      ticks -= chunk;
    }
  }

  if ((half_count & 1u) != 0) {
    symbols[symbol_count].duration1 = 0;
    symbols[symbol_count].level1 = pulse->idle;
    symbol_count += 1u;
  }
  *out_symbol_count = symbol_count;
  return FR_OK;
}

fr_err_t fr_platform_pulse_open(uint16_t pin, uint8_t idle,
                                uint16_t *out_platform_index) {
  const rmt_tx_channel_config_t channel_config = {
      .gpio_num = pin,
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = FR_SIGNAL_CLOCK_HZ,
      .mem_block_symbols = 64,
      .trans_queue_depth = 1,
      .intr_priority = 0,
      .flags.invert_out = false,
      .flags.with_dma = false,
      .flags.io_loop_back = false,
      .flags.io_od_mode = false,
      .flags.allow_pd = false,
  };
  const rmt_copy_encoder_config_t encoder_config = {};
  rmt_channel_handle_t channel = NULL;
  rmt_encoder_handle_t encoder = NULL;
  esp_err_t err = ESP_OK;

  if (out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_esp_gpio_output_valid(pin) || idle > 1) {
    return FR_ERR_DOMAIN;
  }
  if (fr_esp_pulse.in_use) {
    return FR_ERR_CAPACITY;
  }

  err = rmt_new_tx_channel(&channel_config, &channel);
  if (err != ESP_OK) {
    return err == ESP_ERR_NOT_FOUND ? FR_ERR_CAPACITY : fr_esp_err(err);
  }
  err = rmt_new_copy_encoder(&encoder_config, &encoder);
  if (err != ESP_OK) {
    (void)rmt_del_channel(channel);
    return fr_esp_err(err);
  }
  err = fr_esp_pulse_set_idle(channel, encoder, idle);
  if (err != ESP_OK) {
    (void)rmt_del_encoder(encoder);
    (void)rmt_del_channel(channel);
    return fr_esp_err(err);
  }

  memset(&fr_esp_pulse, 0, sizeof(fr_esp_pulse));
  fr_esp_pulse.in_use = true;
  fr_esp_pulse.pin = pin;
  fr_esp_pulse.idle = idle;
  fr_esp_pulse.channel = channel;
  fr_esp_pulse.encoder = encoder;
  *out_platform_index = 0;
  return FR_OK;
}

fr_err_t fr_platform_pulse_add(uint16_t platform_index, uint8_t level,
                               uint32_t duration_ns,
                               uint16_t *out_segment_index) {
  fr_esp_pulse_t *pulse = NULL;
  uint32_t ticks = 0;

  if (out_segment_index == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_esp_pulse_entry(platform_index, &pulse));
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
  fr_esp_pulse_t *pulse = NULL;

  FR_TRY(fr_esp_pulse_entry(platform_index, &pulse));
  pulse->segment_count = 0;
  pulse->total_ticks = 0;
  return FR_OK;
}

fr_err_t fr_platform_pulse_status(uint16_t platform_index,
                                  fr_pulse_status_t *out_status) {
  fr_esp_pulse_t *pulse = NULL;

  if (out_status == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_esp_pulse_entry(platform_index, &pulse));
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
  fr_esp_pulse_t *pulse = NULL;

  if (out_segment == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_esp_pulse_entry(platform_index, &pulse));
  if (segment_index >= pulse->segment_count) {
    return FR_ERR_RANGE;
  }
  *out_segment = pulse->segments[segment_index];
  return FR_OK;
}

fr_err_t fr_platform_pulse_play(uint16_t platform_index) {
  fr_esp_pulse_t *pulse = NULL;
  rmt_symbol_word_t symbols[FR_ESP_PULSE_SYMBOL_CAP];
  rmt_transmit_config_t config = {
      .loop_count = 0,
      .flags.eot_level = 0,
      .flags.queue_nonblocking = false,
  };
  uint16_t symbol_count = 0;
  uint32_t total_ns = 0;
  int wait_ms = 0;
  esp_err_t err = ESP_OK;
  fr_err_t first_error = FR_OK;

  FR_TRY(fr_esp_pulse_entry(platform_index, &pulse));
  if (pulse->segment_count == 0) {
    return FR_ERR_DOMAIN;
  }
  FR_TRY(fr_esp_pulse_pack(pulse, symbols, FR_ESP_PULSE_SYMBOL_CAP,
                           &symbol_count));

  total_ns = pulse->total_ticks * FR_SIGNAL_TICK_NS;
  wait_ms = (int)((total_ns + 999999u) / 1000000u) + 100;
  err = rmt_enable(pulse->channel);
  if (err != ESP_OK) {
    return fr_esp_err(err);
  }
  pulse->enabled = true;

  config.flags.eot_level = pulse->idle;
  err = rmt_transmit(pulse->channel, pulse->encoder, symbols,
                     (size_t)symbol_count * sizeof(symbols[0]), &config);
  if (err == ESP_OK) {
    err = rmt_tx_wait_all_done(pulse->channel, wait_ms);
  }
  if (err != ESP_OK) {
    first_error = fr_esp_err(err);
  }

  err = rmt_disable(pulse->channel);
  if (err == ESP_OK) {
    pulse->enabled = false;
  } else if (first_error == FR_OK) {
    first_error = fr_esp_err(err);
  }
  return first_error;
}

fr_err_t fr_platform_pulse_close(uint16_t platform_index) {
  fr_esp_pulse_t *pulse = NULL;
  fr_err_t first_error = FR_OK;

  FR_TRY(fr_esp_pulse_entry(platform_index, &pulse));
  if (pulse->enabled) {
    fr_err_t err = fr_esp_err(rmt_disable(pulse->channel));

    if (err == FR_OK) {
      pulse->enabled = false;
    } else {
      first_error = err;
    }
  }
  if (!pulse->enabled && pulse->encoder != NULL) {
    fr_err_t err = fr_esp_err(rmt_del_encoder(pulse->encoder));

    if (err == FR_OK) {
      pulse->encoder = NULL;
    } else if (first_error == FR_OK) {
      first_error = err;
    }
  }
  if (!pulse->enabled && pulse->channel != NULL) {
    fr_err_t err = fr_esp_err(rmt_del_channel(pulse->channel));

    if (err == FR_OK) {
      pulse->channel = NULL;
    } else if (first_error == FR_OK) {
      first_error = err;
    }
  }
  /* As with trace, keep failed cleanup reachable for a later close retry. */
  if (first_error == FR_OK && pulse->channel == NULL &&
      pulse->encoder == NULL) {
    memset(pulse, 0, sizeof(*pulse));
  }
  return first_error;
}
#endif

fr_err_t fr_platform_handle_close(fr_handle_kind_t kind,
                                  uint16_t platform_index) {
#if FR_FEATURE_UART
  if (kind == FR_HANDLE_KIND_UART) {
    fr_esp_app_uart_t *uart = NULL;
    uart_port_t port = UART_NUM_0;
    esp_err_t err = ESP_OK;

    FR_TRY(fr_esp_app_uart_entry(platform_index, &uart, &port));
    err = uart_driver_delete(port);
    if (err != ESP_OK) {
      return fr_esp_err(err);
    }
    memset(uart, 0, sizeof(*uart));
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

#if FR_FEATURE_UART
fr_err_t fr_platform_uart_open(uint16_t port, uint32_t baud,
                               uint16_t *out_platform_index) {
  fr_esp_app_uart_t *uart = NULL;
  uart_port_t target_port = UART_NUM_0;
  esp_err_t err = ESP_OK;
  uart_config_t config = {
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  if (out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (port >= sizeof(fr_esp_app_uart_ports) / sizeof(fr_esp_app_uart_ports[0])) {
    return FR_ERR_DOMAIN;
  }
  target_port = fr_esp_app_uart_ports[port];
  uart = &fr_esp_app_uarts[port];
  if (uart->in_use) {
    return FR_ERR_BUSY;
  }
  if (!fr_esp_uart_baud_valid(baud)) {
    return FR_ERR_DOMAIN;
  }
  config.baud_rate = (int)baud;

  err = uart_driver_install(target_port, FR_ESP_APP_UART_RX_BYTES,
                            FR_ESP_APP_UART_TX_BYTES, 0, NULL, 0);
  if (err != ESP_OK) {
    return err == ESP_ERR_INVALID_STATE ? FR_ERR_BUSY : fr_esp_err(err);
  }
  err = uart_param_config(target_port, &config);
  if (err == ESP_OK) {
    err = uart_set_mode(target_port, UART_MODE_UART);
  }
  if (err == ESP_OK) {
    err = uart_flush_input(target_port);
  }
  if (err != ESP_OK) {
    (void)uart_driver_delete(target_port);
    return fr_esp_err(err);
  }

  *uart = (fr_esp_app_uart_t){
      .in_use = true,
      .baud = baud,
  };
  *out_platform_index = port;
  return FR_OK;
}

/* uart.open-on: caller picks tx/rx pins, with conflict checks against the
 * console UART and any already-open custom-pin UART. Setup mirrors
 * fr_platform_uart_open and adds a uart_set_pin step before flushing input. */
fr_err_t fr_platform_uart_open_on(uint16_t port, uint16_t tx, uint16_t rx,
                                  uint32_t baud,
                                  uint16_t *out_platform_index) {
  fr_esp_app_uart_t *uart = NULL;
  uart_port_t target_port = UART_NUM_0;
  esp_err_t err = ESP_OK;
  uart_config_t config = {
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  if (out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (port >= sizeof(fr_esp_app_uart_ports) / sizeof(fr_esp_app_uart_ports[0])) {
    return FR_ERR_DOMAIN;
  }
  /* Bad caller-picked pins are a domain error, not the generic INVALID
   * uart_set_pin would map to after the driver is already installed. */
  if (!fr_esp_gpio_pin_valid(tx) || !fr_esp_gpio_pin_valid(rx) || tx == rx) {
    return FR_ERR_DOMAIN;
  }
  if (fr_esp_app_uart_console_conflict(tx, rx) ||
      fr_esp_app_uart_pin_conflict(tx, rx)) {
    return FR_ERR_DOMAIN;
  }
  target_port = fr_esp_app_uart_ports[port];
  uart = &fr_esp_app_uarts[port];
  if (uart->in_use) {
    return FR_ERR_BUSY;
  }
  if (!fr_esp_uart_baud_valid(baud)) {
    return FR_ERR_DOMAIN;
  }
  config.baud_rate = (int)baud;

  err = uart_driver_install(target_port, FR_ESP_APP_UART_RX_BYTES,
                            FR_ESP_APP_UART_TX_BYTES, 0, NULL, 0);
  if (err != ESP_OK) {
    return err == ESP_ERR_INVALID_STATE ? FR_ERR_BUSY : fr_esp_err(err);
  }
  err = uart_param_config(target_port, &config);
  if (err == ESP_OK) {
    err = uart_set_pin(target_port, tx, rx, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
  }
  if (err == ESP_OK) {
    err = uart_set_mode(target_port, UART_MODE_UART);
  }
  if (err == ESP_OK) {
    err = uart_flush_input(target_port);
  }
  if (err != ESP_OK) {
    (void)uart_driver_delete(target_port);
    return fr_esp_err(err);
  }

  *uart = (fr_esp_app_uart_t){
      .in_use = true,
      .custom_pins = true,
      .tx = tx,
      .rx = rx,
      .baud = baud,
  };
  *out_platform_index = port;
  return FR_OK;
}

fr_err_t fr_platform_uart_write_byte(uint16_t platform_index, uint8_t byte) {
  fr_esp_app_uart_t *uart = NULL;
  uart_port_t port = UART_NUM_0;

  FR_TRY(fr_esp_app_uart_entry(platform_index, &uart, &port));
  (void)uart;
  return uart_write_bytes(port, &byte, 1) == 1 ? FR_OK : FR_ERR_IO;
}

fr_err_t fr_platform_uart_read_byte(uint16_t platform_index, uint8_t *out_byte,
                                    bool *out_has_byte) {
  fr_esp_app_uart_t *uart = NULL;
  uart_port_t port = UART_NUM_0;
  int read = 0;

  if (out_byte == NULL || out_has_byte == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_esp_app_uart_entry(platform_index, &uart, &port));
  (void)uart;
  read = uart_read_bytes(port, out_byte, 1, 0);
  if (read < 0) {
    return FR_ERR_IO;
  }
  *out_has_byte = read == 1;
  if (!*out_has_byte) {
    *out_byte = 0;
  }
  return FR_OK;
}

fr_err_t fr_platform_uart_available(uint16_t platform_index,
                                    uint16_t *out_count) {
  fr_esp_app_uart_t *uart = NULL;
  uart_port_t port = UART_NUM_0;
  size_t length = 0;

  if (out_count == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_esp_app_uart_entry(platform_index, &uart, &port));
  (void)uart;
  FR_TRY(fr_esp_err(uart_get_buffered_data_len(port, &length)));
  if (length > 16383u) {
    return FR_ERR_RANGE;
  }
  *out_count = (uint16_t)length;
  return FR_OK;
}
#endif

#if FR_FEATURE_REPL
static fr_platform_idle_fn fr_esp_idle_handler;
static void *fr_esp_idle_ctx;

void fr_platform_set_idle_handler(fr_platform_idle_fn handler, void *ctx) {
  fr_esp_idle_handler = handler;
  fr_esp_idle_ctx = ctx;
}

static fr_err_t fr_esp_read_edited_line(char *line, uint16_t cap,
                                        bool program_input, bool *out_eof,
                                        uint16_t *out_length) {
  uint16_t used = 0;

  if (line == NULL || cap == 0 || out_eof == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }

  *out_eof = false;
  *out_length = 0;
  line[0] = '\0';
  for (;;) {
    uint8_t byte = 0;
    fr_err_t err = fr_esp_console_read_byte(&byte, pdMS_TO_TICKS(20));

    if (err == FR_ERR_NOT_FOUND) {
      if (program_input && fr_esp_boot_button_pressed()) {
        return FR_ERR_INTERRUPTED;
      }
      /* No byte this tick: service timer/interrupt events so they fire at the
       * idle prompt. The handler reports its own faults and returns OK; the
       * read loop keeps running regardless. */
      if (!program_input && fr_esp_idle_handler != NULL) {
        (void)fr_esp_idle_handler(fr_esp_idle_ctx);
      }
      continue;
    }
    FR_TRY(err);

    if (byte == '\r' || byte == '\n') {
      line[used] = '\0';
      *out_length = used;
      fr_platform_write_text("\n");
      return FR_OK;
    }
    if (byte == FR_ESP_CTRL_C) {
      line[0] = '\0';
      fr_platform_write_text("^C\n");
      return program_input ? FR_ERR_INTERRUPTED : FR_OK;
    }
    if (byte == FR_ESP_BACKSPACE || byte == FR_ESP_DELETE) {
      if (used > 0) {
        used -= 1;
        line[used] = '\0';
        fr_platform_write_text("\b \b");
      }
      continue;
    }
    if (byte < 32 || byte > 126) {
      continue;
    }
    if ((uint16_t)(used + 1) >= cap) {
      return FR_ERR_RANGE;
    }

    line[used] = (char)byte;
    used += 1;
    line[used] = '\0';
    (void)fr_esp_console_write(&byte, 1);
  }
}

fr_err_t fr_platform_read_line(char *line, uint16_t cap, bool *out_eof) {
  uint16_t length = 0;

  return fr_esp_read_edited_line(line, cap, false, out_eof, &length);
}

fr_err_t fr_platform_console_read_line(uint8_t *bytes, uint16_t cap,
                                       uint16_t *out_length) {
  bool eof = false;

  return fr_esp_read_edited_line((char *)bytes, cap, true, &eof, out_length);
}

fr_err_t fr_platform_write_text(const char *text) {
  if (text == NULL) {
    return FR_ERR_INVALID;
  }

  while (*text != '\0') {
    if (*text == '\n') {
      const char cr = '\r';
      FR_TRY(fr_esp_console_write(&cr, 1));
    }
    FR_TRY(fr_esp_console_write(text, 1));
    text += 1;
  }
  return FR_OK;
}
#endif

#if FR_FEATURE_REPL || FR_FEATURE_PAD
fr_err_t fr_platform_write_bytes(const uint8_t *bytes, uint16_t length) {
  if (bytes == NULL && length > 0) {
    return FR_ERR_INVALID;
  }
  return fr_esp_console_write(bytes, length);
}
#endif

#if FR_FEATURE_RANDOM
/* Same xorshift32 (Marsaglia 13/17/5) as host so a given seed produces the
 * same sequence on both targets. Zero state locks; the seed setter swaps 0
 * to 1 to avoid it. */
static uint32_t fr_esp_random_state = 1;

uint32_t fr_platform_random_next(void) {
  uint32_t state = fr_esp_random_state;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  fr_esp_random_state = state;
  return state;
}

void fr_platform_random_seed(uint32_t seed) {
  fr_esp_random_state = seed == 0u ? 1u : seed;
}
#endif

#if FR_FEATURE_PWM
fr_err_t fr_platform_pwm_open(uint16_t pin, uint16_t freq,
                              uint16_t *out_platform_index) {
  if (out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_esp_gpio_output_valid(pin) || freq == 0) {
    return FR_ERR_DOMAIN;
  }
  if (fr_esp_pwm_pin_in_use(pin)) {
    return FR_ERR_BUSY;
  }

  for (uint16_t i = 0; i < FR_ESP_PWM_MAX; i++) {
    fr_esp_pwm_t *pwm = &fr_esp_pwms[i];
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = (ledc_timer_t)i,
        .freq_hz = freq,
        .duty_resolution = (ledc_timer_bit_t)FR_ESP_PWM_DUTY_RESOLUTION_BITS,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t channel_conf = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)i,
        .timer_sel = (ledc_timer_t)i,
        .duty = 0,
        .hpoint = 0,
        .intr_type = LEDC_INTR_DISABLE,
    };

    if (pwm->in_use) {
      continue;
    }

    FR_TRY(fr_esp_err(ledc_timer_config(&timer_conf)));
    FR_TRY(fr_esp_err(ledc_channel_config(&channel_conf)));
    *pwm = (fr_esp_pwm_t){
        .in_use = true,
        .channel = (ledc_channel_t)i,
        .timer = (ledc_timer_t)i,
        .pin = pin,
        .freq = freq,
    };
    *out_platform_index = i;
    return FR_OK;
  }

  return FR_ERR_CAPACITY;
}

fr_err_t fr_platform_pwm_find(uint16_t pin, uint16_t freq,
                              uint16_t *out_platform_index) {
  if (out_platform_index == NULL || freq == 0) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < FR_ESP_PWM_MAX; i++) {
    const fr_esp_pwm_t *pwm = &fr_esp_pwms[i];

    if (!pwm->in_use || pwm->pin != pin) {
      continue;
    }
    if (pwm->freq != freq) {
      return FR_ERR_BUSY;
    }
    *out_platform_index = i;
    return FR_OK;
  }
  return FR_ERR_NOT_FOUND;
}

fr_err_t fr_platform_pwm_write(uint16_t platform_index, uint16_t duty) {
  fr_esp_pwm_t *pwm = NULL;
  /* Input is parts-per-10k; LEDC native duty is 10-bit. */
  uint32_t native_duty = ((uint32_t)duty * 1023U) / 10000U;

  FR_TRY(fr_esp_pwm_entry(platform_index, &pwm));
  FR_TRY(fr_esp_err(
      ledc_set_duty(LEDC_LOW_SPEED_MODE, pwm->channel, native_duty)));
  return fr_esp_err(ledc_update_duty(LEDC_LOW_SPEED_MODE, pwm->channel));
}

fr_err_t fr_platform_pwm_close(uint16_t platform_index) {
  fr_esp_pwm_t *pwm = NULL;

  FR_TRY(fr_esp_pwm_entry(platform_index, &pwm));
  FR_TRY(fr_esp_err(ledc_stop(LEDC_LOW_SPEED_MODE, pwm->channel, 0)));
  memset(pwm, 0, sizeof(*pwm));
  return FR_OK;
}
#endif

#if FR_FEATURE_I2C
/* Per-write/read transactions construct a transient device on the bus using
 * the stored frequency as scl_speed_hz. Caching the device handle is a
 * deferred optimization. */
static fr_err_t fr_esp_i2c_dev(fr_esp_i2c_t *i2c, uint8_t addr,
                               i2c_master_dev_handle_t *out_dev) {
  const i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = addr,
      .scl_speed_hz = i2c->freq,
  };
  return fr_esp_err(i2c_master_bus_add_device(i2c->handle, &dev_cfg, out_dev));
}

fr_err_t fr_platform_i2c_open(uint16_t port, uint16_t sda, uint16_t scl,
                              uint32_t freq, uint16_t *out_platform_index) {
  if (out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (port >= FR_ESP_I2C_MAX || !fr_esp_gpio_output_valid(sda) ||
      !fr_esp_gpio_output_valid(scl) || sda == scl || freq == 0 ||
      fr_esp_i2c_port_in_use(port) || fr_esp_i2c_pin_in_use(sda, scl)) {
    return FR_ERR_DOMAIN;
  }

  for (uint16_t i = 0; i < FR_ESP_I2C_MAX; i++) {
    fr_esp_i2c_t *i2c = &fr_esp_i2cs[i];
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = (int)port,
        .sda_io_num = (gpio_num_t)sda,
        .scl_io_num = (gpio_num_t)scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t handle = NULL;

    if (i2c->in_use) {
      continue;
    }

    FR_TRY(fr_esp_err(i2c_new_master_bus(&bus_cfg, &handle)));
    *i2c = (fr_esp_i2c_t){
        .in_use = true,
        .port = port,
        .sda = sda,
        .scl = scl,
        .freq = freq,
        .handle = handle,
    };
    *out_platform_index = i;
    return FR_OK;
  }

  return FR_ERR_CAPACITY;
}

fr_err_t fr_platform_i2c_write(uint16_t platform_index, uint8_t addr,
                               const uint8_t *bytes, uint16_t length) {
  fr_esp_i2c_t *i2c = NULL;
  i2c_master_dev_handle_t dev = NULL;
  fr_err_t err = FR_OK;

  if (addr > FR_ESP_I2C_ADDR_MAX) {
    return FR_ERR_DOMAIN;
  }
  if (bytes == NULL && length > 0) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_esp_i2c_entry(platform_index, &i2c));
  FR_TRY(fr_esp_i2c_dev(i2c, addr, &dev));
  err = fr_esp_err(i2c_master_transmit(dev, bytes, length, -1));
  (void)i2c_master_bus_rm_device(dev);
  return err;
}

fr_err_t fr_platform_i2c_read(uint16_t platform_index, uint8_t addr,
                              uint8_t *bytes, uint16_t length) {
  fr_esp_i2c_t *i2c = NULL;
  i2c_master_dev_handle_t dev = NULL;
  fr_err_t err = FR_OK;

  if (addr > FR_ESP_I2C_ADDR_MAX) {
    return FR_ERR_DOMAIN;
  }
  if (bytes == NULL && length > 0) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_esp_i2c_entry(platform_index, &i2c));
  if (length == 0) {
    return FR_OK;
  }
  FR_TRY(fr_esp_i2c_dev(i2c, addr, &dev));
  err = fr_esp_err(i2c_master_receive(dev, bytes, length, -1));
  (void)i2c_master_bus_rm_device(dev);
  return err;
}

fr_err_t fr_platform_i2c_write_read(uint16_t platform_index, uint8_t addr,
                                    const uint8_t *wbytes, uint16_t wlength,
                                    uint8_t *rbytes, uint16_t rlength) {
  fr_esp_i2c_t *i2c = NULL;
  i2c_master_dev_handle_t dev = NULL;
  fr_err_t err = FR_OK;

  if (addr > FR_ESP_I2C_ADDR_MAX) {
    return FR_ERR_DOMAIN;
  }
  if ((wbytes == NULL && wlength > 0) || (rbytes == NULL && rlength > 0)) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_esp_i2c_entry(platform_index, &i2c));
  if (wlength == 0 && rlength == 0) {
    return FR_OK;
  }
  FR_TRY(fr_esp_i2c_dev(i2c, addr, &dev));
  err = fr_esp_err(
      i2c_master_transmit_receive(dev, wbytes, wlength, rbytes, rlength, -1));
  (void)i2c_master_bus_rm_device(dev);
  return err;
}

fr_err_t fr_platform_i2c_close(uint16_t platform_index) {
  fr_esp_i2c_t *i2c = NULL;

  FR_TRY(fr_esp_i2c_entry(platform_index, &i2c));
  FR_TRY(fr_esp_err(i2c_del_master_bus(i2c->handle)));
  memset(i2c, 0, sizeof(*i2c));
  return FR_OK;
}
#endif

#if FR_FEATURE_PERSISTENCE
static const esp_partition_t *fr_esp_persist_partition;
static esp_partition_mmap_handle_t fr_esp_persist_mmap_handle;
static esp_partition_mmap_handle_t fr_esp_persist_candidate_mmap_handle;
static const uint8_t *fr_esp_persist_mmap_bytes;
static const uint8_t *fr_esp_persist_candidate_mmap_bytes;
static uint16_t fr_esp_persist_mmap_length;
static uint16_t fr_esp_persist_candidate_mmap_length;
static bool fr_esp_persist_mmap_active;
static bool fr_esp_persist_candidate_mmap_active;
static uint8_t fr_esp_persist_mmap_slot;
static uint8_t fr_esp_persist_candidate_mmap_slot;
static bool fr_esp_persist_candidate_aliases_active;
static struct {
  bool active;
  const esp_partition_t *partition;
  uint8_t slot;
  uint32_t write_offset;
  uint32_t payload_length;
  uint32_t backend_generation;
  uint8_t tail[FR_ESP_FLASH_WRITE_ALIGN_BYTES];
  uint8_t tail_length;
} fr_esp_persist_stream;

static bool fr_esp_persist_pointer_in_mmap(const uint8_t *bytes,
                                           uint16_t bytes_length,
                                           bool active, const void *ptr,
                                           uint16_t length) {
  uintptr_t p = (uintptr_t)ptr;
  uintptr_t b = (uintptr_t)bytes;

  if (!active || bytes == NULL) {
    return false;
  }
  if (length == 0) {
    return true;
  }
  if (ptr == NULL || p < b) {
    return false;
  }
  return (uint32_t)(p - b) + length <= bytes_length;
}

static fr_err_t fr_esp_persist_offset_in_mmap(const uint8_t *bytes,
                                              uint16_t bytes_length,
                                              bool active, const void *ptr,
                                              uint16_t length,
                                              uint16_t *out_offset) {
  if (out_offset == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_esp_persist_pointer_in_mmap(bytes, bytes_length, active, ptr,
                                      length)) {
    return FR_ERR_NOT_FOUND;
  }
  *out_offset = (uint16_t)((uintptr_t)ptr - (uintptr_t)bytes);
  return FR_OK;
}

static uint32_t fr_esp_persist_slot_offset(uint8_t slot) {
  return (uint32_t)slot * (uint32_t)FR_ESP_PERSIST_SLOT_BYTES;
}

static fr_err_t fr_esp_persist_open(const esp_partition_t **out_partition) {
  const esp_partition_t *partition = fr_esp_persist_partition;

  if (out_partition == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_esp_platform_init());
  if (partition == NULL) {
    partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        (esp_partition_subtype_t)FR_ESP_PERSIST_PARTITION_SUBTYPE, "frothy");
    if (partition == NULL) {
      return FR_ERR_NOT_FOUND;
    }
    if (partition->size < FR_ESP_PERSIST_PARTITION_BYTES) {
      return FR_ERR_CAPACITY;
    }
    fr_esp_persist_partition = partition;
  }
  *out_partition = partition;
  return FR_OK;
}

static fr_err_t fr_esp_persist_payload_crc(const esp_partition_t *partition,
                                           uint8_t slot,
                                           uint32_t payload_length,
                                           uint32_t *out_crc) {
  uint8_t chunk[FR_ESP_PERSIST_READ_CHUNK_BYTES];
  uint32_t crc = 0xffffffffu;
  uint32_t read_offset = fr_esp_persist_slot_offset(slot) +
                         (uint32_t)FR_PERSIST_HEADER_BYTES;
  uint32_t remaining = payload_length;

  if (partition == NULL || out_crc == NULL) {
    return FR_ERR_INVALID;
  }
  while (remaining > 0) {
    uint32_t n = remaining;

    if (n > sizeof(chunk)) {
      n = sizeof(chunk);
    }
    FR_TRY(fr_esp_err(esp_partition_read(partition, read_offset, chunk, n)));
    crc = fr_crc32_update(crc, chunk, n);
    read_offset += n;
    remaining -= n;
  }
  *out_crc = ~crc;
  return FR_OK;
}

static fr_err_t fr_esp_persist_slot_info(const esp_partition_t *partition,
                                         uint8_t slot,
                                         fr_persist_format_info_t *out) {
  uint8_t header[FR_PERSIST_HEADER_BYTES];
  fr_persist_format_info_t info = {0};
  uint32_t payload_crc = 0;

  if (partition == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (slot >= FR_ESP_PERSIST_SLOT_COUNT) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_esp_err(esp_partition_read(
      partition, fr_esp_persist_slot_offset(slot), header, sizeof(header))));
  FR_TRY(fr_persist_format_read_header(header, &info));
  if (info.total_length > FR_ESP_PERSIST_SLOT_BYTES ||
      info.total_length > UINT16_MAX) {
    return FR_ERR_CAPACITY;
  }
  FR_TRY(fr_esp_persist_payload_crc(partition, slot, info.payload_length,
                                    &payload_crc));
  FR_TRY(fr_persist_format_validate_header_payload_crc(header, payload_crc,
                                                       &info));
  *out = info;
  return FR_OK;
}

static fr_err_t fr_esp_persist_pick_read_slot(
    const esp_partition_t *partition, uint8_t image_index, uint8_t *out_slot,
    fr_persist_format_info_t *out_info) {
  fr_persist_format_info_t info[FR_ESP_PERSIST_SLOT_COUNT];
  bool valid[FR_ESP_PERSIST_SLOT_COUNT] = {false, false};
  bool saw_corrupt = false;
  uint8_t slots[FR_ESP_PERSIST_SLOT_COUNT] = {0, 1};
  uint8_t valid_count = 0;

  if (out_slot == NULL || out_info == NULL) {
    return FR_ERR_INVALID;
  }

  for (uint8_t slot = 0; slot < FR_ESP_PERSIST_SLOT_COUNT; slot++) {
    fr_err_t err = fr_esp_persist_slot_info(partition, slot, &info[slot]);
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

static fr_err_t fr_esp_persist_pick_commit_slot(const esp_partition_t *partition,
                                                uint8_t *out_slot,
                                                uint32_t *out_generation) {
  fr_persist_format_info_t info[FR_ESP_PERSIST_SLOT_COUNT];
  bool valid[FR_ESP_PERSIST_SLOT_COUNT] = {false, false};

  if (out_slot == NULL || out_generation == NULL) {
    return FR_ERR_INVALID;
  }

  for (uint8_t slot = 0; slot < FR_ESP_PERSIST_SLOT_COUNT; slot++) {
    valid[slot] =
        fr_esp_persist_slot_info(partition, slot, &info[slot]) == FR_OK;
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

fr_err_t fr_platform_persist_read(uint8_t *bytes, uint16_t cap,
                                  uint16_t *out_length, uint8_t image_index) {
  const esp_partition_t *partition = NULL;
  uint8_t slot = 0;
  fr_persist_format_info_t info = {0};
  fr_err_t err = FR_OK;

  if (bytes == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }

  err = fr_esp_persist_open(&partition);
  if (err != FR_OK) {
    return err;
  }
  err = fr_esp_persist_pick_read_slot(partition, image_index, &slot, &info);
  if (err == FR_OK) {
    if (info.total_length > UINT16_MAX) {
      err = FR_ERR_CAPACITY;
    } else if (cap < info.total_length) {
      err = FR_ERR_CAPACITY;
    } else {
      err = fr_esp_err(esp_partition_read(
          partition, fr_esp_persist_slot_offset(slot), bytes,
          (uint16_t)info.total_length));
      if (err == FR_OK) {
        *out_length = (uint16_t)info.total_length;
      }
    }
  }
  return err;
}

void fr_platform_persist_unmount(void) {
  fr_platform_persist_mount_discard();
  if (fr_esp_persist_mmap_active) {
    esp_partition_munmap(fr_esp_persist_mmap_handle);
    fr_esp_persist_mmap_active = false;
    fr_esp_persist_mmap_handle = 0;
    fr_esp_persist_mmap_bytes = NULL;
    fr_esp_persist_mmap_length = 0;
    fr_esp_persist_mmap_slot = 0;
    fr_esp_persist_candidate_aliases_active = false;
  }
}

void fr_platform_persist_mount_discard(void) {
  if (fr_esp_persist_candidate_mmap_active) {
    if (!fr_esp_persist_candidate_aliases_active) {
      esp_partition_munmap(fr_esp_persist_candidate_mmap_handle);
    }
    fr_esp_persist_candidate_mmap_active = false;
    fr_esp_persist_candidate_mmap_handle = 0;
    fr_esp_persist_candidate_mmap_bytes = NULL;
    fr_esp_persist_candidate_mmap_length = 0;
    fr_esp_persist_candidate_aliases_active = false;
  }
}

fr_err_t fr_platform_persist_mount_commit(void) {
  esp_partition_mmap_handle_t old_handle = 0;
  bool had_old = fr_esp_persist_mmap_active;

  if (!fr_esp_persist_candidate_mmap_active) {
    return FR_ERR_INVALID;
  }
  if (fr_esp_persist_candidate_aliases_active) {
    fr_esp_persist_candidate_mmap_handle = 0;
    fr_esp_persist_candidate_mmap_bytes = NULL;
    fr_esp_persist_candidate_mmap_length = 0;
    fr_esp_persist_candidate_mmap_active = false;
    fr_esp_persist_candidate_aliases_active = false;
    return FR_OK;
  }
  old_handle = fr_esp_persist_mmap_handle;
  fr_esp_persist_mmap_handle = fr_esp_persist_candidate_mmap_handle;
  fr_esp_persist_mmap_bytes = fr_esp_persist_candidate_mmap_bytes;
  fr_esp_persist_mmap_length = fr_esp_persist_candidate_mmap_length;
  fr_esp_persist_mmap_slot = fr_esp_persist_candidate_mmap_slot;
  fr_esp_persist_mmap_active = true;
  fr_esp_persist_candidate_mmap_handle = 0;
  fr_esp_persist_candidate_mmap_bytes = NULL;
  fr_esp_persist_candidate_mmap_length = 0;
  fr_esp_persist_candidate_mmap_active = false;
  if (had_old) {
    esp_partition_munmap(old_handle);
  }
  return FR_OK;
}

bool fr_platform_persist_pointer_is_mounted(const void *ptr, uint16_t length) {
  return fr_esp_persist_pointer_in_mmap(fr_esp_persist_mmap_bytes,
                                        fr_esp_persist_mmap_length,
                                        fr_esp_persist_mmap_active, ptr,
                                        length);
}

bool fr_platform_persist_code_pointer_is_direct(const void *ptr,
                                                uint16_t length) {
  return fr_esp_persist_pointer_in_mmap(
             fr_esp_persist_candidate_mmap_bytes,
             fr_esp_persist_candidate_mmap_length,
             fr_esp_persist_candidate_mmap_active, ptr, length) ||
         fr_platform_persist_pointer_is_mounted(ptr, length);
}

fr_err_t fr_platform_persist_mounted_offset(const void *ptr, uint16_t length,
                                            uint16_t *out_offset) {
  fr_err_t err = fr_esp_persist_offset_in_mmap(
      fr_esp_persist_candidate_mmap_bytes,
      fr_esp_persist_candidate_mmap_length,
      fr_esp_persist_candidate_mmap_active, ptr, length, out_offset);

  if (err == FR_OK) {
    return FR_OK;
  }
  return fr_esp_persist_offset_in_mmap(fr_esp_persist_mmap_bytes,
                                       fr_esp_persist_mmap_length,
                                       fr_esp_persist_mmap_active, ptr, length,
                                       out_offset);
}

fr_err_t fr_platform_persist_read_mounted(uint16_t offset, uint8_t *dst,
                                          uint16_t length) {
  const uint8_t *bytes = NULL;
  uint16_t bytes_length = 0;

  if (dst == NULL && length > 0) {
    return FR_ERR_INVALID;
  }
  if (fr_esp_persist_candidate_mmap_active) {
    bytes = fr_esp_persist_candidate_mmap_bytes;
    bytes_length = fr_esp_persist_candidate_mmap_length;
  } else if (fr_esp_persist_mmap_active) {
    bytes = fr_esp_persist_mmap_bytes;
    bytes_length = fr_esp_persist_mmap_length;
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

fr_err_t fr_platform_persist_mount(uint8_t image_index,
                                   const uint8_t **out_bytes,
                                   uint16_t *out_length) {
  const esp_partition_t *partition = NULL;
  const void *mapped = NULL;
  uint8_t slot = 0;
  fr_persist_format_info_t info = {0};
  fr_err_t err = FR_OK;

  if (out_bytes == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }

  fr_platform_persist_mount_discard();
  err = fr_esp_persist_open(&partition);
  if (err != FR_OK) {
    return err;
  }
  err = fr_esp_persist_pick_read_slot(partition, image_index, &slot, &info);
  if (err != FR_OK) {
    return err;
  }
  if (info.total_length > UINT16_MAX) {
    return FR_ERR_CAPACITY;
  }
  if (fr_esp_persist_mmap_active && slot == fr_esp_persist_mmap_slot) {
    /* The requested slot is already memory-mapped (active). Mapping it again
     * would make esp_mmu_map log a benign "already mapped" error, so reuse
     * the existing mapping as the candidate. The active mapping owns the
     * handle; commit/discard must not munmap this alias. */
    fr_esp_persist_candidate_aliases_active = true;
    fr_esp_persist_candidate_mmap_active = true;
    fr_esp_persist_candidate_mmap_handle = 0;
    fr_esp_persist_candidate_mmap_bytes = fr_esp_persist_mmap_bytes;
    fr_esp_persist_candidate_mmap_length = fr_esp_persist_mmap_length;
    fr_esp_persist_candidate_mmap_slot = slot;
    *out_bytes = fr_esp_persist_mmap_bytes;
    *out_length = fr_esp_persist_mmap_length;
    return FR_OK;
  }

  err = fr_esp_err(esp_partition_mmap(
      partition, fr_esp_persist_slot_offset(slot), info.total_length,
      ESP_PARTITION_MMAP_DATA, &mapped,
      &fr_esp_persist_candidate_mmap_handle));
  if (err != FR_OK) {
    return err;
  }
  fr_esp_persist_candidate_mmap_active = true;
  fr_esp_persist_candidate_mmap_bytes = (const uint8_t *)mapped;
  fr_esp_persist_candidate_mmap_length = (uint16_t)info.total_length;
  fr_esp_persist_candidate_mmap_slot = slot;
  fr_esp_persist_candidate_aliases_active = false;
  *out_bytes = (const uint8_t *)mapped;
  *out_length = (uint16_t)info.total_length;
  return FR_OK;
}

static fr_err_t fr_esp_persist_stream_write_aligned(const uint8_t *bytes,
                                                    uint16_t length) {
  if (bytes == NULL && length > 0) {
    return FR_ERR_INVALID;
  }
  if (!fr_esp_persist_stream.active ||
      fr_esp_persist_stream.partition == NULL) {
    return FR_ERR_INVALID;
  }
  if ((fr_esp_persist_stream.write_offset %
       FR_ESP_FLASH_WRITE_ALIGN_BYTES) != 0 ||
      (length % FR_ESP_FLASH_WRITE_ALIGN_BYTES) != 0) {
    return FR_ERR_INVALID;
  }
  if ((fr_esp_persist_stream.write_offset -
       fr_esp_persist_slot_offset(fr_esp_persist_stream.slot)) +
          length >
      FR_ESP_PERSIST_SLOT_BYTES) {
    return FR_ERR_CAPACITY;
  }
  if (length == 0) {
    return FR_OK;
  }
  FR_TRY(fr_esp_err(esp_partition_write(
      fr_esp_persist_stream.partition, fr_esp_persist_stream.write_offset,
      bytes, length)));
  fr_esp_persist_stream.write_offset += length;
  return FR_OK;
}

fr_err_t fr_platform_persist_stream_begin(void) {
  const esp_partition_t *partition = NULL;
  uint8_t slot = 0;
  uint32_t backend_generation = 0;
  uint32_t slot_offset = 0;
  fr_err_t err = FR_OK;

  fr_platform_persist_stream_abort();
  err = fr_esp_persist_open(&partition);
  if (err != FR_OK) {
    return err;
  }
  err = fr_esp_persist_pick_commit_slot(partition, &slot, &backend_generation);
  if (err == FR_OK) {
    slot_offset = fr_esp_persist_slot_offset(slot);
    err = fr_esp_err(
        esp_partition_erase_range(partition, slot_offset,
                                  FR_ESP_PERSIST_SLOT_BYTES));
  }
  if (err == FR_OK) {
    fr_esp_persist_stream.active = true;
    fr_esp_persist_stream.partition = partition;
    fr_esp_persist_stream.slot = slot;
    fr_esp_persist_stream.write_offset =
        slot_offset + (uint32_t)FR_PERSIST_HEADER_BYTES;
    fr_esp_persist_stream.payload_length = 0;
    fr_esp_persist_stream.backend_generation = backend_generation;
    fr_esp_persist_stream.tail_length = 0;
  }
  return err;
}

fr_err_t fr_platform_persist_stream_write(const uint8_t *bytes,
                                          uint16_t length) {
  const uint8_t *cursor = bytes;
  uint16_t remaining = length;
  uint32_t next_payload_length = 0;

  if (bytes == NULL && length > 0) {
    return FR_ERR_INVALID;
  }
  if (!fr_esp_persist_stream.active) {
    return FR_ERR_INVALID;
  }
  next_payload_length = fr_esp_persist_stream.payload_length + length;
  if (next_payload_length > FR_PERSIST_PAYLOAD_BYTES ||
      (uint32_t)FR_PERSIST_HEADER_BYTES + next_payload_length >
          FR_ESP_PERSIST_SLOT_BYTES) {
    return FR_ERR_CAPACITY;
  }
  fr_esp_persist_stream.payload_length = next_payload_length;

  if (fr_esp_persist_stream.tail_length > 0) {
    while (fr_esp_persist_stream.tail_length < FR_ESP_FLASH_WRITE_ALIGN_BYTES &&
           remaining > 0) {
      fr_esp_persist_stream.tail[fr_esp_persist_stream.tail_length++] =
          *cursor++;
      remaining = (uint16_t)(remaining - 1u);
    }
    if (fr_esp_persist_stream.tail_length == FR_ESP_FLASH_WRITE_ALIGN_BYTES) {
      FR_TRY(fr_esp_persist_stream_write_aligned(
          fr_esp_persist_stream.tail, FR_ESP_FLASH_WRITE_ALIGN_BYTES));
      fr_esp_persist_stream.tail_length = 0;
    }
  }

  if (remaining >= FR_ESP_FLASH_WRITE_ALIGN_BYTES) {
    uint16_t aligned = (uint16_t)(
        remaining - (remaining % FR_ESP_FLASH_WRITE_ALIGN_BYTES));

    FR_TRY(fr_esp_persist_stream_write_aligned(cursor, aligned));
    cursor += aligned;
    remaining = (uint16_t)(remaining - aligned);
  }

  while (remaining > 0) {
    fr_esp_persist_stream.tail[fr_esp_persist_stream.tail_length++] =
        *cursor++;
    remaining = (uint16_t)(remaining - 1u);
  }
  return FR_OK;
}

fr_err_t fr_platform_persist_stream_finalize(
    const uint8_t header[FR_PERSIST_HEADER_BYTES]) {
  uint8_t stamped[FR_PERSIST_HEADER_BYTES];
  uint32_t slot_offset = 0;

  if (header == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_esp_persist_stream.active ||
      fr_esp_persist_stream.partition == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_esp_persist_stream.tail_length > 0) {
    while (fr_esp_persist_stream.tail_length < FR_ESP_FLASH_WRITE_ALIGN_BYTES) {
      fr_esp_persist_stream.tail[fr_esp_persist_stream.tail_length++] = 0xffu;
    }
    FR_TRY(fr_esp_persist_stream_write_aligned(
        fr_esp_persist_stream.tail, FR_ESP_FLASH_WRITE_ALIGN_BYTES));
    fr_esp_persist_stream.tail_length = 0;
  }

  memcpy(stamped, header, sizeof(stamped));
  FR_TRY(fr_persist_format_stamp_generation(
      stamped, fr_esp_persist_stream.backend_generation));
  slot_offset = fr_esp_persist_slot_offset(fr_esp_persist_stream.slot);
  FR_TRY(fr_esp_err(esp_partition_write(fr_esp_persist_stream.partition,
                                        slot_offset, stamped,
                                        sizeof(stamped))));
  fr_esp_persist_stream.active = false;
  fr_esp_persist_stream.partition = NULL;
  return FR_OK;
}

void fr_platform_persist_stream_abort(void) {
  fr_esp_persist_stream.active = false;
  fr_esp_persist_stream.partition = NULL;
  fr_esp_persist_stream.tail_length = 0;
}

fr_err_t fr_platform_persist_clear(void) {
  const esp_partition_t *partition = NULL;
  fr_err_t err = FR_OK;

  fr_platform_persist_unmount();
  fr_platform_persist_stream_abort();
  err = fr_esp_persist_open(&partition);
  if (err != FR_OK) {
    return err;
  }
  return fr_esp_err(
      esp_partition_erase_range(partition, 0, FR_ESP_PERSIST_PARTITION_BYTES));
}
#endif

/* Queue depth 16 matches FR_EVENT_BINDING_COUNT: one full drain transfers
 * every live binding exactly once before any overflow. */
static QueueHandle_t fr_esp_event_queue;
static volatile uint32_t fr_esp_event_overflow;
static portMUX_TYPE fr_esp_event_overflow_mux = portMUX_INITIALIZER_UNLOCKED;
static bool fr_esp_isr_service_installed;
static esp_timer_handle_t fr_esp_event_timer_handles[FR_EVENT_BINDING_COUNT];

static uint32_t fr_esp_event_millis_now(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

static fr_err_t fr_esp_event_queue_ensure(void) {
  if (fr_esp_event_queue == NULL) {
    fr_esp_event_queue = xQueueCreate(16, sizeof(fr_event_candidate_t));
    if (fr_esp_event_queue == NULL) {
      return FR_ERR_CAPACITY;
    }
  }
  return FR_OK;
}

static fr_err_t fr_esp_event_isr_service_ensure(void) {
  esp_err_t e;
  if (fr_esp_isr_service_installed) {
    return FR_OK;
  }
  e = gpio_install_isr_service(0);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
    return FR_ERR_CAPACITY;
  }
  fr_esp_isr_service_installed = true;
  return FR_OK;
}

static void fr_esp_event_gpio_isr(void *arg) {
  uintptr_t packed = (uintptr_t)arg;
  fr_event_candidate_t candidate = {
      .binding_index = (uint16_t)(packed & 0xFFFFu),
      .generation = (uint16_t)((packed >> 16) & 0xFFFFu),
      .timestamp_ms = fr_esp_event_millis_now(),
  };
  BaseType_t woken = pdFALSE;
  if (xQueueSendFromISR(fr_esp_event_queue, &candidate, &woken) != pdTRUE) {
    portENTER_CRITICAL_ISR(&fr_esp_event_overflow_mux);
    fr_esp_event_overflow++;
    portEXIT_CRITICAL_ISR(&fr_esp_event_overflow_mux);
  }
  portYIELD_FROM_ISR(woken);
}

/* esp_timer dispatches this on the timer task, not in ISR context, so the
 * non-FromISR queue send is correct. Same small-shape contract as the GPIO
 * ISR: no allocation, no logging, no Frothy code. */
static void fr_esp_event_timer_callback(void *arg) {
  uintptr_t packed = (uintptr_t)arg;
  fr_event_candidate_t candidate = {
      .binding_index = (uint16_t)(packed & 0xFFFFu),
      .generation = (uint16_t)((packed >> 16) & 0xFFFFu),
      .timestamp_ms = fr_esp_event_millis_now(),
  };
  if (xQueueSend(fr_esp_event_queue, &candidate, 0) != pdTRUE) {
    portENTER_CRITICAL(&fr_esp_event_overflow_mux);
    fr_esp_event_overflow++;
    portEXIT_CRITICAL(&fr_esp_event_overflow_mux);
  }
}

fr_err_t fr_platform_event_gpio_install(fr_event_kind_t kind, uint16_t pin,
                                        uint16_t binding_index,
                                        uint16_t generation) {
  gpio_int_type_t intr;
  void *packed_arg;
  fr_err_t err;

  switch (kind) {
  case FR_EVENT_KIND_GPIO_RISING:
    intr = GPIO_INTR_POSEDGE;
    break;
  case FR_EVENT_KIND_GPIO_FALLING:
    intr = GPIO_INTR_NEGEDGE;
    break;
  case FR_EVENT_KIND_GPIO_CHANGES:
    intr = GPIO_INTR_ANYEDGE;
    break;
  default:
    return FR_ERR_INVALID;
  }

  err = fr_esp_event_queue_ensure();
  if (err != FR_OK) {
    return err;
  }
  err = fr_esp_event_isr_service_ensure();
  if (err != FR_OK) {
    return err;
  }

  packed_arg = (void *)(((uintptr_t)generation << 16) |
                        (uintptr_t)binding_index);
  if (gpio_isr_handler_add((gpio_num_t)pin, fr_esp_event_gpio_isr,
                           packed_arg) != ESP_OK) {
    return FR_ERR_CAPACITY;
  }
  if (gpio_set_intr_type((gpio_num_t)pin, intr) != ESP_OK) {
    (void)gpio_isr_handler_remove((gpio_num_t)pin);
    return FR_ERR_INVALID;
  }
  return FR_OK;
}

fr_err_t fr_platform_event_gpio_remove(uint16_t pin) {
  /* Report the first failure so the runtime sees a pin that may stay armed
   * or keep a stale handler rather than a clean clear. */
  esp_err_t disable_err = gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_DISABLE);
  esp_err_t remove_err = gpio_isr_handler_remove((gpio_num_t)pin);
  if (disable_err != ESP_OK || remove_err != ESP_OK) {
    return FR_ERR_INVALID;
  }
  return FR_OK;
}

fr_err_t fr_platform_event_timer_install(fr_event_kind_t kind, uint32_t ms,
                                         uint16_t binding_index,
                                         uint16_t generation) {
  esp_timer_create_args_t args;
  esp_timer_handle_t new_handle;
  esp_timer_handle_t old_handle;
  void *packed_arg;
  fr_err_t err;
  esp_err_t start_err;
  esp_err_t old_stop_err;
  esp_err_t new_stop_err;
  esp_err_t new_delete_err;

  if (kind != FR_EVENT_KIND_EVERY && kind != FR_EVENT_KIND_AFTER) {
    return FR_ERR_INVALID;
  }
  if (binding_index >= FR_EVENT_BINDING_COUNT) {
    return FR_ERR_INVALID;
  }

  err = fr_esp_event_queue_ensure();
  if (err != FR_OK) {
    return err;
  }

  packed_arg = (void *)(((uintptr_t)generation << 16) |
                        (uintptr_t)binding_index);
  args.callback = fr_esp_event_timer_callback;
  args.arg = packed_arg;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "frothy.event";
  args.skip_unhandled_events = false;
  if (esp_timer_create(&args, &new_handle) != ESP_OK) {
    return FR_ERR_CAPACITY;
  }
  if (kind == FR_EVENT_KIND_EVERY) {
    start_err = esp_timer_start_periodic(new_handle, (uint64_t)ms * 1000u);
  } else {
    start_err = esp_timer_start_once(new_handle, (uint64_t)ms * 1000u);
  }
  if (start_err != ESP_OK) {
    (void)esp_timer_delete(new_handle);
    return FR_ERR_INVALID;
  }

  /* Stage the replacement: the new handle is already running. Commit the slot
   * once the old handle is released. If old stop/delete fails, try to release
   * the new handle. INVALID_STATE from stop is the already-expired one-shot
   * case; on clean new release the old binding stays armed (slot keeps
   * old_handle, return FR_ERR_INVALID) and src/event.c:88-103 staging keeps
   * the runtime side matched. If the new handle also leaks, commit new to
   * the slot: the runtime must bump generation so the next install retry
   * packs a fresh value that can't collide with the leaked new handle's
   * callbacks - the old handle leaks instead with its earlier generation,
   * which the runtime's filter drops. */
  old_handle = fr_esp_event_timer_handles[binding_index];
  if (old_handle != NULL) {
    old_stop_err = esp_timer_stop(old_handle);
    if ((old_stop_err != ESP_OK && old_stop_err != ESP_ERR_INVALID_STATE) ||
        esp_timer_delete(old_handle) != ESP_OK) {
      new_stop_err = esp_timer_stop(new_handle);
      new_delete_err = esp_timer_delete(new_handle);
      if ((new_stop_err == ESP_OK || new_stop_err == ESP_ERR_INVALID_STATE) &&
          new_delete_err == ESP_OK) {
        return FR_ERR_INVALID;
      }
    }
  }
  fr_esp_event_timer_handles[binding_index] = new_handle;
  return FR_OK;
}

fr_err_t fr_platform_event_timer_remove(uint16_t binding_index) {
  esp_timer_handle_t handle;
  esp_err_t stop_err;
  esp_err_t delete_err;

  if (binding_index >= FR_EVENT_BINDING_COUNT) {
    return FR_ERR_INVALID;
  }
  handle = fr_esp_event_timer_handles[binding_index];
  if (handle == NULL) {
    return FR_OK;
  }
  /* INVALID_STATE from stop is benign for an already-expired one-shot;
   * delete still has to run to release the slot. Leave the slot pointing at
   * the handle until delete confirms the underlying memory is freed, so the
   * runtime can retry remove on failure rather than leaking the timer. */
  stop_err = esp_timer_stop(handle);
  if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
    return FR_ERR_INVALID;
  }
  delete_err = esp_timer_delete(handle);
  if (delete_err != ESP_OK) {
    return FR_ERR_INVALID;
  }
  fr_esp_event_timer_handles[binding_index] = NULL;
  return FR_OK;
}

fr_err_t fr_platform_event_drain(fr_event_candidate_t *out_events,
                                 uint8_t out_cap, uint8_t *out_count,
                                 uint32_t *overflow_delta) {
  uint8_t transferred = 0;

  if (out_count == NULL || overflow_delta == NULL) {
    return FR_ERR_INVALID;
  }
  if (out_events == NULL && out_cap > 0) {
    return FR_ERR_INVALID;
  }

  if (fr_esp_event_queue == NULL) {
    *out_count = 0;
    *overflow_delta = 0;
    return FR_OK;
  }

  /* Writers and drain share one mux so the drained delta is exact. */
  portENTER_CRITICAL(&fr_esp_event_overflow_mux);
  *overflow_delta = fr_esp_event_overflow;
  fr_esp_event_overflow = 0;
  portEXIT_CRITICAL(&fr_esp_event_overflow_mux);

  while (transferred < out_cap) {
    fr_event_candidate_t candidate;
    if (xQueueReceive(fr_esp_event_queue, &candidate, 0) != pdTRUE) {
      break;
    }
    out_events[transferred++] = candidate;
  }
  *out_count = transferred;
  return FR_OK;
}

fr_err_t fr_platform_event_post_test_candidate(uint16_t binding_index,
                                               uint16_t generation,
                                               uint32_t timestamp_ms) {
  (void)binding_index;
  (void)generation;
  (void)timestamp_ms;
  return FR_ERR_UNSUPPORTED;
}

#if FR_FEATURE_NET
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "event.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include <fcntl.h>

enum {
  FR_ESP_WIFI_SSID_MAX = 32,
  FR_ESP_WIFI_PASS_MAX = 64,
  FR_ESP_WIFI_CONNECT_TIMEOUT_MS = 30000,
  FR_ESP_WIFI_POLL_MS = 50,
  /* Best-effort settle after esp_wifi_disconnect() so the prior
   * association's STA_DISCONNECTED event is delivered before we
   * set_config + connect a new association. */
  FR_ESP_WIFI_RECONFIG_SETTLE_MS = 100,
  FR_ESP_HTTP_TIMEOUT_MS = 5000,
  /* D7 budgets. */
  FR_ESP_TCP_OPEN_TIMEOUT_MS = 10000,
  FR_ESP_TCP_RW_TIMEOUT_MS = 5000,
  /* Inner socket timeout drives the cooperative-yield cadence. Small enough
   * that Ctrl-C / wifi_down latency stays under ~100 ms; large enough that
   * we don't burn cycles spinning. */
  FR_ESP_TCP_POLL_MS = 100,
};

static bool fr_esp_wifi_initialized;
static volatile bool fr_esp_wifi_ready;
/* Suppresses the disconnect handler's auto-retry while user code is
 * tearing down and re-establishing the station association — held true
 * across the full disconnect → set_config → connect sequence. Without
 * it, esp_wifi_connect() from the handler races with our set_config
 * ("sta is connecting, cannot set config" → FR_ERR_IO) or undoes our
 * intended association in the tiny window before we issue our own
 * connect. */
static volatile bool fr_esp_wifi_reconfiguring;
/* D13/D19: track whether we've ever reached an IP. Initial got_ip is a fresh
 * connect (no wifi.reconnected event); a got_ip after disconnect is a
 * reconnect (wifi.reconnected fires). */
static volatile bool fr_esp_wifi_was_connected;
/* T15b D12: set when WIFI_EVENT_STA_DISCONNECTED fires outside the
 * reconfigure window; cleared on IP_EVENT_STA_GOT_IP. Any TCP native that
 * observes it returns FR_ERR_NET_DISCONNECTED and latches the per-handle
 * failed flag so a reassociation cannot silently revive a stale fd. */
static volatile bool fr_esp_wifi_down;
/* T15b D12: bumped each time a real disconnect fires (not reconfigure).
 * Each TCP handle captures the value at open; check_alive flips the
 * runtime failed flag when the captured epoch differs from the current
 * one, so an idle handle that was open across a disconnect+reconnect
 * cycle still fails on its next use even after wifi_down clears. */
static volatile uint32_t fr_esp_wifi_down_epoch;

/* D19 parallel install/remove pair backing. One slot per wifi kind because
 * each kind has at most one binding and the wifi handler has no per-pin or
 * per-period source dimension. The fields are word-sized so the sys_evt-task
 * read in the handler races safely against the main-task write in install /
 * remove; a stale candidate that slips through gets dropped by the runtime's
 * generation filter (src/event.c:192-194). */
typedef struct fr_esp_wifi_slot_t {
  uint16_t binding_index;
  uint16_t generation;
  bool active;
} fr_esp_wifi_slot_t;

static fr_esp_wifi_slot_t fr_esp_wifi_slots[2];

static uint8_t fr_esp_wifi_slot_index(fr_event_kind_t kind) {
  return kind == FR_EVENT_KIND_WIFI_DISCONNECTED ? 0u : 1u;
}

/* Push a candidate for the wifi kind onto the shared event queue. Same shape
 * as the timer-task path (fr_esp_event_timer_callback above) — non-FromISR
 * send because the wifi handler runs on the sys_evt task. */
static void fr_esp_wifi_enqueue(fr_event_kind_t kind) {
  const fr_esp_wifi_slot_t *slot =
      &fr_esp_wifi_slots[fr_esp_wifi_slot_index(kind)];
  fr_event_candidate_t candidate;
  if (!slot->active || fr_esp_event_queue == NULL) {
    return;
  }
  candidate.binding_index = slot->binding_index;
  candidate.generation = slot->generation;
  candidate.timestamp_ms = fr_esp_event_millis_now();
  if (xQueueSend(fr_esp_event_queue, &candidate, 0) != pdTRUE) {
    portENTER_CRITICAL(&fr_esp_event_overflow_mux);
    fr_esp_event_overflow++;
    portEXIT_CRITICAL(&fr_esp_event_overflow_mux);
  }
}

/* Wi-Fi events fire on the ESP-IDF sys_evt task. D13: Frothy owns reconnect,
 * so a disconnect retries esp_wifi_connect from here; the 30 s wifi.connect:
 * budget catches pathological loops (bad creds). D19: a wifi.disconnected
 * binding only fires after we have observed an IP at least once, and
 * wifi.reconnected only on re-establishment (initial got_ip stays silent). */
static void fr_esp_wifi_event_handler(void *arg, esp_event_base_t base,
                                      int32_t id, void *data) {
  bool was_connected;
  (void)arg;
  (void)data;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    was_connected = fr_esp_wifi_was_connected;
    fr_esp_wifi_ready = false;
    /* Skip the auto-retry while user code is mid-reconfigure — it will
     * issue its own esp_wifi_connect() once set_config completes. The
     * wifi_down flag is also gated: a reconfigure-driven drop should not
     * surface to in-flight TCP as a transport failure. */
    if (!fr_esp_wifi_reconfiguring) {
      fr_esp_wifi_down = true;
      fr_esp_wifi_down_epoch++;
      (void)esp_wifi_connect();
    }
    if (was_connected) {
      fr_esp_wifi_enqueue(FR_EVENT_KIND_WIFI_DISCONNECTED);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    was_connected = fr_esp_wifi_was_connected;
    fr_esp_wifi_ready = true;
    fr_esp_wifi_was_connected = true;
    fr_esp_wifi_down = false;
    if (was_connected) {
      fr_esp_wifi_enqueue(FR_EVENT_KIND_WIFI_RECONNECTED);
    }
  }
}

static fr_err_t fr_esp_wifi_init_once(void) {
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

  if (fr_esp_wifi_initialized) {
    return FR_OK;
  }
  FR_TRY(fr_esp_nvs_init());
  FR_TRY(fr_esp_err(esp_netif_init()));
  FR_TRY(fr_esp_err(esp_event_loop_create_default()));
  if (esp_netif_create_default_wifi_sta() == NULL) {
    return FR_ERR_CAPACITY;
  }
  FR_TRY(fr_esp_err(esp_wifi_init(&cfg)));
  /* esp_wifi caches its own station config in NVS when storage defaults to
   * FLASH; that cache survives chip reset and triggers an auto-reconnect
   * to the prior AP on esp_wifi_start, which then fights any later
   * wifi.connect: with new creds ("sta is connected, disconnect before
   * connecting to new ap"). Keep esp_wifi state in RAM only — Frothy
   * owns persistence via the frothy_wifi NVS namespace. */
  FR_TRY(fr_esp_err(esp_wifi_set_storage(WIFI_STORAGE_RAM)));
  FR_TRY(fr_esp_err(esp_event_handler_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, fr_esp_wifi_event_handler, NULL)));
  FR_TRY(fr_esp_err(esp_event_handler_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, fr_esp_wifi_event_handler, NULL)));
  FR_TRY(fr_esp_err(esp_wifi_set_mode(WIFI_MODE_STA)));
  FR_TRY(fr_esp_err(esp_wifi_start()));
  fr_esp_wifi_initialized = true;
  return FR_OK;
}

/* D15: dedicated frothy_wifi namespace, parallel to the user-tier frothy
 * namespace at line 1071. NVS init is shared. */
static fr_err_t fr_esp_wifi_nvs_open(nvs_open_mode_t mode,
                                     nvs_handle_t *out_handle) {
  if (out_handle == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_esp_platform_init());
  FR_TRY(fr_esp_nvs_init());
  return fr_esp_err(nvs_open("frothy_wifi", mode, out_handle));
}

fr_err_t fr_platform_wifi_save(const char *ssid, const char *pass) {
  nvs_handle_t handle = 0;
  size_t ssid_len = 0;
  size_t pass_len = 0;
  fr_err_t err = FR_OK;

  if (ssid == NULL || pass == NULL) {
    return FR_ERR_INVALID;
  }
  ssid_len = strlen(ssid);
  pass_len = strlen(pass);
  if (ssid_len == 0 || ssid_len > FR_ESP_WIFI_SSID_MAX ||
      pass_len > FR_ESP_WIFI_PASS_MAX) {
    return FR_ERR_DOMAIN;
  }

  err = fr_esp_wifi_nvs_open(NVS_READWRITE, &handle);
  if (err != FR_OK) {
    return err;
  }
  err = fr_esp_err(nvs_set_str(handle, "ssid", ssid));
  if (err == FR_OK) {
    err = fr_esp_err(nvs_set_str(handle, "pass", pass));
  }
  if (err == FR_OK) {
    err = fr_esp_err(nvs_commit(handle));
  }
  nvs_close(handle);
  return err;
}

fr_err_t fr_platform_wifi_connect(fr_runtime_t *runtime) {
  nvs_handle_t handle = 0;
  char ssid[FR_ESP_WIFI_SSID_MAX + 1];
  char pass[FR_ESP_WIFI_PASS_MAX + 1];
  size_t ssid_len = sizeof ssid;
  size_t pass_len = sizeof pass;
  wifi_config_t wifi_config = {0};
  esp_err_t nvs_err = ESP_OK;
  fr_err_t err = FR_OK;
  uint64_t start_us = 0;
  uint64_t budget_us = 0;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  err = fr_esp_wifi_nvs_open(NVS_READONLY, &handle);
  if (err == FR_ERR_NOT_FOUND) {
    return FR_ERR_NET_DISCONNECTED;
  }
  if (err != FR_OK) {
    return err;
  }
  nvs_err = nvs_get_str(handle, "ssid", ssid, &ssid_len);
  if (nvs_err == ESP_OK) {
    nvs_err = nvs_get_str(handle, "pass", pass, &pass_len);
  }
  nvs_close(handle);
  if (nvs_err == ESP_ERR_NVS_NOT_FOUND) {
    return FR_ERR_NET_DISCONNECTED;
  }
  FR_TRY(fr_esp_err(nvs_err));

  FR_TRY(fr_esp_wifi_init_once());

  fr_esp_wifi_ready = false;
  /* Tear down any prior association before set_config + connect. Without
   * this, calling wifi.connect: after a successful connection (or with
   * fresh creds via wifi.save:) is rejected by esp_wifi with "sta is
   * connected, disconnect before connecting to new ap" and the wait loop
   * below times out. The reconfiguring flag suppresses the disconnect
   * handler's auto-retry so it doesn't race set_config or undo our
   * intended association. A small settle lets the disconnect propagate
   * before we reconfigure. */
  fr_esp_wifi_reconfiguring = true;
  (void)esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(FR_ESP_WIFI_RECONFIG_SETTLE_MS));
  /* wifi_sta_config_t fields are byte arrays sized 32/64 (D15). NVS strings
   * are NUL-terminated; copy bytes without the terminator. */
  memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
  memcpy(wifi_config.sta.password, pass, strlen(pass));
  err = fr_esp_err(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  if (err != FR_OK) {
    fr_esp_wifi_reconfiguring = false;
    return err;
  }
  /* Re-clear ready: a stale GOT_IP from the prior association's wait
   * could otherwise satisfy the loop below for the wrong association. */
  fr_esp_wifi_ready = false;
  (void)esp_wifi_connect();
  fr_esp_wifi_reconfiguring = false;

  start_us = (uint64_t)esp_timer_get_time();
  budget_us = (uint64_t)FR_ESP_WIFI_CONNECT_TIMEOUT_MS * 1000u;
  while (!fr_esp_wifi_ready) {
    FR_TRY(fr_platform_poll_interrupt(runtime));
    if (fr_runtime_is_interrupted(runtime)) {
      return FR_ERR_INTERRUPTED;
    }
    if ((uint64_t)esp_timer_get_time() - start_us > budget_us) {
      return FR_ERR_NET_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(FR_ESP_WIFI_POLL_MS));
  }
  return FR_OK;
}

fr_err_t fr_platform_wifi_ready(bool *out_ready) {
  if (out_ready == NULL) {
    return FR_ERR_INVALID;
  }
  *out_ready = fr_esp_wifi_ready;
  return FR_OK;
}

fr_err_t fr_platform_event_wifi_install(fr_event_kind_t kind,
                                        uint16_t binding_index,
                                        uint16_t generation) {
  uint8_t i;
  if (kind != FR_EVENT_KIND_WIFI_DISCONNECTED &&
      kind != FR_EVENT_KIND_WIFI_RECONNECTED) {
    return FR_ERR_INVALID;
  }
  if (binding_index >= FR_EVENT_BINDING_COUNT) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_esp_event_queue_ensure());
  i = fr_esp_wifi_slot_index(kind);
  fr_esp_wifi_slots[i].binding_index = binding_index;
  fr_esp_wifi_slots[i].generation = generation;
  fr_esp_wifi_slots[i].active = true;
  return FR_OK;
}

fr_err_t fr_platform_event_wifi_remove(uint16_t binding_index) {
  for (uint8_t i = 0; i < 2; i++) {
    if (fr_esp_wifi_slots[i].active &&
        fr_esp_wifi_slots[i].binding_index == binding_index) {
      fr_esp_wifi_slots[i].active = false;
      fr_esp_wifi_slots[i].binding_index = 0;
      fr_esp_wifi_slots[i].generation = 0;
    }
  }
  return FR_OK;
}

typedef struct fr_esp_http_ctx_t {
  uint8_t *body;
  uint16_t cap;
  uint16_t written;
  bool too_large;
} fr_esp_http_ctx_t;

/* HTTP_EVENT_ON_DATA can deliver multiple chunks. D5: stop at the cap and
 * report no partial result; we drop further writes once too_large latches but
 * keep returning ESP_OK so the client finishes its session cleanly. */
static esp_err_t fr_esp_http_event(esp_http_client_event_t *evt) {
  fr_esp_http_ctx_t *ctx = (fr_esp_http_ctx_t *)evt->user_data;
  uint16_t remaining;
  if (ctx == NULL || evt->event_id != HTTP_EVENT_ON_DATA ||
      evt->data_len <= 0) {
    return ESP_OK;
  }
  if (ctx->too_large) {
    return ESP_OK;
  }
  remaining = (uint16_t)(ctx->cap - ctx->written);
  if ((uint32_t)evt->data_len > (uint32_t)remaining) {
    ctx->too_large = true;
    return ESP_OK;
  }
  memcpy(ctx->body + ctx->written, evt->data, (size_t)evt->data_len);
  ctx->written = (uint16_t)(ctx->written + (uint16_t)evt->data_len);
  return ESP_OK;
}

fr_err_t fr_platform_http_get(const char *url, uint8_t *out_body, uint16_t cap,
                              uint16_t *out_length) {
  fr_esp_http_ctx_t ctx;
  esp_http_client_config_t config;
  esp_http_client_handle_t client;
  esp_err_t perform_err;
  int status;

  if (url == NULL || url[0] == '\0' || out_body == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }
  /* T15-hwfix: esp_http_client_perform calls into lwip; if the TCP/IP
   * stack isn't running yet (because wifi.connect: was never called) lwip
   * asserts with "Invalid mbox" and panics the device. Refuse early. */
  if (!fr_esp_wifi_ready) {
    return FR_ERR_NET_DISCONNECTED;
  }
  *out_length = 0;

  ctx.body = out_body;
  ctx.cap = cap;
  ctx.written = 0;
  ctx.too_large = false;

  memset(&config, 0, sizeof config);
  config.url = url;
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = (int)FR_ESP_HTTP_TIMEOUT_MS;
  config.event_handler = fr_esp_http_event;
  config.user_data = &ctx;

  client = esp_http_client_init(&config);
  if (client == NULL) {
    return FR_ERR_NET_PROTOCOL;
  }
  perform_err = esp_http_client_perform(client);
  status = esp_http_client_get_status_code(client);
  (void)esp_http_client_cleanup(client);

  if (ctx.too_large) {
    return FR_ERR_NET_TOO_LARGE;
  }
  if (perform_err == ESP_ERR_HTTP_CONNECT) {
    return FR_ERR_NET_REFUSED;
  }
  if (perform_err == ESP_ERR_HTTP_EAGAIN) {
    return FR_ERR_NET_TIMEOUT;
  }
  if (perform_err == ESP_ERR_HTTP_INVALID_TRANSPORT ||
      perform_err == ESP_ERR_HTTP_FETCH_HEADER ||
      perform_err == ESP_ERR_HTTP_MAX_REDIRECT) {
    return FR_ERR_NET_PROTOCOL;
  }
  if (perform_err != ESP_OK) {
    /* TLS handshake failure, DNS failure, and other transport errors land
     * here; D11 maps no-bundle https failure to NET_PROTOCOL. */
    return FR_ERR_NET_PROTOCOL;
  }
  if (status < 200 || status >= 300) {
    return FR_ERR_NET_REFUSED;
  }
  *out_length = ctx.written;
  return FR_OK;
}

/* D17: target-side per-handle TCP state. Parallel to the runtime array
 * declared in src/runtime.h: the runtime array carries the kernel-visible
 * failed flag (D12); this array carries the OS resource (lwip fd) so
 * fr_platform_tcp_close, which fr_platform_handle_close routes to without
 * a runtime pointer, still has somewhere to look. Both are indexed by
 * platform_index in lockstep — open populates both, close clears this one
 * and the next open clears the runtime entry. */
typedef struct fr_esp_tcp_t {
  bool in_use;
  int fd;
  uint32_t open_epoch;
} fr_esp_tcp_t;

static fr_esp_tcp_t fr_esp_tcps[FR_TCP_HANDLE_COUNT];

static fr_err_t fr_esp_tcp_entry(uint16_t platform_index,
                                 fr_esp_tcp_t **out_entry) {
  if (out_entry == NULL) {
    return FR_ERR_INVALID;
  }
  if (platform_index >= FR_TCP_HANDLE_COUNT ||
      !fr_esp_tcps[platform_index].in_use) {
    return FR_ERR_HANDLE;
  }
  *out_entry = &fr_esp_tcps[platform_index];
  return FR_OK;
}

/* D12 gate. Catches three cases: wifi is down right now, the handle was
 * open across a disconnect window that has since cleared (epoch mismatch),
 * and a prior call already latched failure. Latches the runtime flag on
 * the first two so once a handle has failed it stays failed for the rest
 * of its life. */
static fr_err_t fr_esp_tcp_check_alive(fr_runtime_t *runtime,
                                       uint16_t platform_index) {
  if (runtime == NULL || platform_index >= FR_TCP_HANDLE_COUNT) {
    return FR_ERR_INVALID;
  }
  if (fr_esp_wifi_down ||
      fr_esp_wifi_down_epoch != fr_esp_tcps[platform_index].open_epoch) {
    runtime->tcp_handles[platform_index].failed = true;
  }
  if (runtime->tcp_handles[platform_index].failed) {
    return FR_ERR_NET_DISCONNECTED;
  }
  return FR_OK;
}

static fr_err_t fr_esp_tcp_set_rw_timeout(int fd) {
  struct timeval tv;
  tv.tv_sec = FR_ESP_TCP_POLL_MS / 1000;
  tv.tv_usec = (FR_ESP_TCP_POLL_MS % 1000) * 1000;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0) {
    return FR_ERR_IO;
  }
  if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) < 0) {
    return FR_ERR_IO;
  }
  return FR_OK;
}

/* Strict-aliasing-clean port write for the AF_INET sockaddr that
 * getaddrinfo returns. */
static void fr_esp_tcp_set_port(struct addrinfo *info, uint16_t port) {
  struct sockaddr_in *sin;
  if (info == NULL || info->ai_addr == NULL ||
      info->ai_addrlen < sizeof(struct sockaddr_in)) {
    return;
  }
  sin = (struct sockaddr_in *)info->ai_addr;
  sin->sin_port = htons(port);
}

fr_err_t fr_platform_tcp_open(fr_runtime_t *runtime, const char *host,
                              uint16_t port, uint16_t *out_platform_index) {
  struct addrinfo hints;
  struct addrinfo *info = NULL;
  uint16_t slot = 0;
  bool slot_found = false;
  int fd = -1;
  int rc = 0;
  int flags = 0;
  uint64_t start_us = 0;
  uint64_t budget_us = 0;
  fr_err_t err = FR_OK;

  if (runtime == NULL || host == NULL || host[0] == '\0' || port == 0 ||
      out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_esp_wifi_down || !fr_esp_wifi_ready) {
    return FR_ERR_NET_DISCONNECTED;
  }
  for (uint16_t i = 0; i < FR_TCP_HANDLE_COUNT; i++) {
    if (!fr_esp_tcps[i].in_use) {
      slot = i;
      slot_found = true;
      break;
    }
  }
  if (!slot_found) {
    return FR_ERR_CAPACITY;
  }

  /* D7 10 s budget covers DNS + connect together. getaddrinfo is
   * synchronously bounded by lwip's own retransmit; the post-DNS check
   * caps the total before the connect loop starts. */
  start_us = (uint64_t)esp_timer_get_time();
  budget_us = (uint64_t)FR_ESP_TCP_OPEN_TIMEOUT_MS * 1000u;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  rc = getaddrinfo(host, NULL, &hints, &info);
  if (rc != 0 || info == NULL) {
    if (info != NULL) {
      freeaddrinfo(info);
    }
    return FR_ERR_NET_DNS;
  }
  if ((uint64_t)esp_timer_get_time() - start_us > budget_us) {
    freeaddrinfo(info);
    return FR_ERR_NET_TIMEOUT;
  }
  fr_esp_tcp_set_port(info, port);

  fd = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
  if (fd < 0) {
    freeaddrinfo(info);
    return FR_ERR_NET_REFUSED;
  }
  err = fr_esp_tcp_set_rw_timeout(fd);
  if (err != FR_OK) {
    (void)close(fd);
    freeaddrinfo(info);
    return err;
  }

  /* Non-blocking for connect so the first connect() returns immediately
   * with EINPROGRESS and the loop checks wifi_down / Ctrl-C / budget at
   * the ~1 ms cooperative cadence instead of waiting up to SO_SNDTIMEO. */
  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    (void)close(fd);
    freeaddrinfo(info);
    return FR_ERR_IO;
  }

  for (;;) {
    rc = connect(fd, info->ai_addr, info->ai_addrlen);
    if (rc == 0) {
      break;
    }
    if (errno == EISCONN) {
      break;
    }
    if (errno != EINPROGRESS && errno != EALREADY && errno != EAGAIN &&
        errno != EWOULDBLOCK && errno != EINTR) {
      (void)close(fd);
      freeaddrinfo(info);
      return FR_ERR_NET_REFUSED;
    }
    if (fr_esp_wifi_down) {
      (void)close(fd);
      freeaddrinfo(info);
      return FR_ERR_NET_DISCONNECTED;
    }
    err = fr_platform_poll_interrupt(runtime);
    if (err != FR_OK) {
      (void)close(fd);
      freeaddrinfo(info);
      return err;
    }
    if (fr_runtime_is_interrupted(runtime)) {
      (void)close(fd);
      freeaddrinfo(info);
      return FR_ERR_INTERRUPTED;
    }
    if ((uint64_t)esp_timer_get_time() - start_us > budget_us) {
      (void)close(fd);
      freeaddrinfo(info);
      return FR_ERR_NET_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  freeaddrinfo(info);

  /* Restore the original flags so SO_RCVTIMEO / SO_SNDTIMEO drive the
   * read/write cadence. lwip recv/send ignore the timeout sockopts when
   * O_NONBLOCK is set. */
  if (fcntl(fd, F_SETFL, flags) < 0) {
    (void)close(fd);
    return FR_ERR_IO;
  }

  fr_esp_tcps[slot].in_use = true;
  fr_esp_tcps[slot].fd = fd;
  fr_esp_tcps[slot].open_epoch = fr_esp_wifi_down_epoch;
  runtime->tcp_handles[slot].failed = false;
  *out_platform_index = slot;
  return FR_OK;
}

fr_err_t fr_platform_tcp_read(fr_runtime_t *runtime, uint16_t platform_index,
                              uint8_t *out_bytes, uint16_t cap,
                              uint16_t *out_length) {
  fr_esp_tcp_t *entry = NULL;
  uint64_t start_us = 0;
  uint64_t budget_us = 0;
  int n = 0;
  fr_err_t err = FR_OK;

  if (out_bytes == NULL || out_length == NULL || cap == 0) {
    return FR_ERR_INVALID;
  }
  *out_length = 0;
  FR_TRY(fr_esp_tcp_check_alive(runtime, platform_index));
  FR_TRY(fr_esp_tcp_entry(platform_index, &entry));

  start_us = (uint64_t)esp_timer_get_time();
  budget_us = (uint64_t)FR_ESP_TCP_RW_TIMEOUT_MS * 1000u;
  for (;;) {
    n = recv(entry->fd, out_bytes, cap, 0);
    if (n > 0) {
      *out_length = (uint16_t)n;
      return FR_OK;
    }
    if (n == 0) {
      /* D8 (2): graceful EOF is FR_OK with zero length. */
      return FR_OK;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      return FR_ERR_NET_REFUSED;
    }
    if (fr_esp_wifi_down) {
      runtime->tcp_handles[platform_index].failed = true;
      return FR_ERR_NET_DISCONNECTED;
    }
    err = fr_platform_poll_interrupt(runtime);
    if (err != FR_OK) {
      return err;
    }
    if (fr_runtime_is_interrupted(runtime)) {
      return FR_ERR_INTERRUPTED;
    }
    if ((uint64_t)esp_timer_get_time() - start_us > budget_us) {
      return FR_ERR_NET_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

fr_err_t fr_platform_tcp_write(fr_runtime_t *runtime, uint16_t platform_index,
                               const uint8_t *bytes, uint16_t length) {
  fr_esp_tcp_t *entry = NULL;
  uint64_t start_us = 0;
  uint64_t budget_us = 0;
  uint16_t sent = 0;
  int n = 0;
  fr_err_t err = FR_OK;

  if (bytes == NULL && length > 0) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_esp_tcp_check_alive(runtime, platform_index));
  FR_TRY(fr_esp_tcp_entry(platform_index, &entry));
  if (length == 0) {
    return FR_OK;
  }

  start_us = (uint64_t)esp_timer_get_time();
  budget_us = (uint64_t)FR_ESP_TCP_RW_TIMEOUT_MS * 1000u;
  while (sent < length) {
    n = send(entry->fd, bytes + sent, (size_t)(length - sent), 0);
    if (n > 0) {
      sent = (uint16_t)(sent + (uint16_t)n);
      continue;
    }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      return FR_ERR_NET_REFUSED;
    }
    if (fr_esp_wifi_down) {
      runtime->tcp_handles[platform_index].failed = true;
      return FR_ERR_NET_DISCONNECTED;
    }
    err = fr_platform_poll_interrupt(runtime);
    if (err != FR_OK) {
      return err;
    }
    if (fr_runtime_is_interrupted(runtime)) {
      return FR_ERR_INTERRUPTED;
    }
    if ((uint64_t)esp_timer_get_time() - start_us > budget_us) {
      return FR_ERR_NET_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return FR_OK;
}

fr_err_t fr_platform_tcp_close(uint16_t platform_index) {
  fr_esp_tcp_t *entry = NULL;
  fr_err_t err = fr_esp_tcp_entry(platform_index, &entry);
  if (err != FR_OK) {
    return err;
  }
  (void)close(entry->fd);
  entry->in_use = false;
  entry->fd = -1;
  return FR_OK;
}

fr_err_t fr_platform_tcp_bytes_ready(fr_runtime_t *runtime,
                                     uint16_t platform_index,
                                     uint16_t *out_count) {
  fr_esp_tcp_t *entry = NULL;
  int ready = 0;
  if (out_count == NULL) {
    return FR_ERR_INVALID;
  }
  *out_count = 0;
  FR_TRY(fr_esp_tcp_check_alive(runtime, platform_index));
  FR_TRY(fr_esp_tcp_entry(platform_index, &entry));
  if (ioctl(entry->fd, FIONREAD, &ready) < 0) {
    /* lwip returns -1 once the peer has FIN'd. From the user model,
     * bytes-ready?: asks how many bytes are readable right now; post-EOF
     * that's zero. Latching REFUSED here would break the canonical drain
     * loop (until ready=0 and chunk=""), since the read that observes
     * EOF and the bytes-ready that follows cross the same FIN. The next
     * tcp.read: still surfaces empty (EOF) or a real per-handle error. */
    *out_count = 0;
    return FR_OK;
  }
  if (ready < 0) {
    ready = 0;
  }
  if (ready > UINT16_MAX) {
    ready = UINT16_MAX;
  }
  *out_count = (uint16_t)ready;
  return FR_OK;
}

#endif

#if FR_FEATURE_POWER
/* T14 D8/D10/D11 Task WDT. The Frothy-local armed flag in target_defs.c
 * drives the kernel's D11 "feed when not armed" contract. We keep a
 * platform-side subscribed flag so the call sequence runs as D8 spells
 * out: every arm calls esp_task_wdt_reconfigure (D10 replaces the
 * running config), and only the first arm calls esp_task_wdt_add(NULL)
 * to subscribe the calling task. ESP-IDF rejects a duplicate add with
 * ESP_ERR_INVALID_ARG (task_wdt.c:196), so re-arm skips it. The idle
 * mask mirrors what ESP-IDF's own startup builds
 * (freertos/app_startup.c:184-199) so the IDLE-task safety net stays
 * exactly as the default boot init set it up. */
static bool fr_esp_watchdog_subscribed;

static uint32_t fr_esp_watchdog_idle_mask(void) {
  uint32_t mask = 0;
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
  mask |= 1u << 0;
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
  mask |= 1u << 1;
#endif
  return mask;
}

fr_err_t fr_platform_watchdog_arm(uint32_t timeout_ms) {
  esp_task_wdt_config_t cfg = {
      .timeout_ms = timeout_ms,
      .idle_core_mask = fr_esp_watchdog_idle_mask(),
      .trigger_panic = true,
  };
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) {
    return FR_ERR_IO;
  }
  if (!fr_esp_watchdog_subscribed) {
    if (esp_task_wdt_add(NULL) != ESP_OK) {
      return FR_ERR_IO;
    }
    fr_esp_watchdog_subscribed = true;
  }
  return FR_OK;
}

fr_err_t fr_platform_watchdog_feed(void) {
  if (esp_task_wdt_reset() != ESP_OK) {
    return FR_ERR_IO;
  }
  return FR_OK;
}

/* T14 D12 deep sleep. Pending wake-on-gpio is RAM-only; deep sleep
 * cold-boots and the user must call sleep.wake-on-gpio: again before
 * the next sleep.deep:. esp_deep_sleep_start is __noreturn__
 * (esp_sleep.h:610-616); the trailing return statement is unreachable. */
static bool fr_esp_sleep_pending;
static uint16_t fr_esp_sleep_pending_pin;
static uint16_t fr_esp_sleep_pending_level;

fr_err_t fr_platform_sleep_deep(uint32_t ms) {
  if (ms == 0 && !fr_esp_sleep_pending) {
    return FR_ERR_INVALID;
  }
  if (ms > 0) {
    if (esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL) != ESP_OK) {
      return FR_ERR_IO;
    }
  }
  if (fr_esp_sleep_pending) {
#if SOC_PM_SUPPORT_EXT0_WAKEUP
    if (esp_sleep_enable_ext0_wakeup((gpio_num_t)fr_esp_sleep_pending_pin,
                                     (int)fr_esp_sleep_pending_level) !=
        ESP_OK) {
      return FR_ERR_IO;
    }
#elif SOC_PM_SUPPORT_EXT1_WAKEUP
    esp_sleep_ext1_wakeup_mode_t mode =
        fr_esp_sleep_pending_level ? ESP_EXT1_WAKEUP_ANY_HIGH
                                   : ESP_EXT1_WAKEUP_ANY_LOW;
    if (esp_sleep_enable_ext1_wakeup_io(
            1ULL << fr_esp_sleep_pending_pin, mode) != ESP_OK) {
      return FR_ERR_IO;
    }
#else
#error "FR_FEATURE_POWER requires GPIO wake support"
#endif
    fr_esp_sleep_pending = false;
  }
  esp_deep_sleep_start();
  return FR_OK;
}

fr_err_t fr_platform_sleep_wake_on_gpio(uint16_t pin, uint16_t level) {
  if (level > 1) {
    return FR_ERR_INVALID;
  }
  if (!esp_sleep_is_valid_wakeup_gpio((gpio_num_t)pin)) {
    return FR_ERR_INVALID;
  }
  fr_esp_sleep_pending = true;
  fr_esp_sleep_pending_pin = pin;
  fr_esp_sleep_pending_level = level;
  return FR_OK;
}
#endif
