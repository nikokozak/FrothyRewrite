#include "platform.h"

#include "board.h"
#include "handle.h"
#include "runtime.h"

#include "driver/gpio.h"
#if FR_FEATURE_I2C
#include "driver/i2c_master.h"
#endif
#if FR_FEATURE_PWM
#include "driver/ledc.h"
#endif
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
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

enum {
  FR_ESP_UART_RX_BYTES = 256,
  FR_ESP_UART_TX_BYTES = 256,
#if FR_FEATURE_UART
  FR_ESP_APP_UART_RX_BYTES = 256,
  FR_ESP_APP_UART_TX_BYTES = 256,
#endif
  FR_ESP_CTRL_C = 3,
  FR_ESP_BACKSPACE = 8,
  FR_ESP_DELETE = 127,
  FR_ESP_STORAGE_SLOT_COUNT = 2,
  FR_ESP_VM_YIELD_INTERVAL_US = 250000,
};

#if FR_FEATURE_UART
typedef struct fr_esp_app_uart_t {
  bool in_use;
  bool custom_pins; /* true only for uart.open-on; tx/rx hold the chosen pins */
  uint16_t tx;
  uint16_t rx;
  uint16_t rate_code;
} fr_esp_app_uart_t;

#if SOC_UART_NUM <= 1
#error "FR_FEATURE_UART requires an ESP target with an application UART"
#endif

static const uart_port_t fr_esp_app_uart_ports[] = {
#if SOC_UART_NUM > 1
    UART_NUM_1,
#endif
#if SOC_UART_NUM > 2
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

static bool fr_esp_initialized;
static bool fr_esp_pending_byte_valid;
static uint8_t fr_esp_pending_byte;
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

static fr_err_t fr_esp_uart_init(void) {
  const uart_config_t config = {
      .baud_rate = FR_BOARD_UART_BAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  esp_err_t err = uart_driver_install(FR_BOARD_UART_PORT, FR_ESP_UART_RX_BYTES,
                                      FR_ESP_UART_TX_BYTES, 0, NULL, 0);

  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return fr_esp_err(err);
  }
  FR_TRY(fr_esp_err(uart_param_config(FR_BOARD_UART_PORT, &config)));
  FR_TRY(fr_esp_err(uart_set_pin(FR_BOARD_UART_PORT, FR_BOARD_UART_TX,
                                 FR_BOARD_UART_RX, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE)));
  FR_TRY(fr_esp_err(uart_set_mode(FR_BOARD_UART_PORT, UART_MODE_UART)));
  FR_TRY(fr_esp_err(uart_flush_input(FR_BOARD_UART_PORT)));
  return FR_OK;
}

static fr_err_t fr_esp_nvs_init(void) {
  esp_err_t err = nvs_flash_init();

  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    FR_TRY(fr_esp_err(nvs_flash_erase()));
    err = nvs_flash_init();
  }
  return fr_esp_err(err);
}

fr_err_t fr_esp_platform_init(void) {
  if (fr_esp_initialized) {
    return FR_OK;
  }

  FR_TRY(fr_esp_uart_init());
#if FR_FEATURE_PERSISTENCE
  FR_TRY(fr_esp_nvs_init());
#endif
  fr_esp_initialized = true;
  return FR_OK;
}

static fr_err_t fr_esp_read_byte(uint8_t *out_byte, TickType_t timeout) {
  int read = 0;

  if (out_byte == NULL) {
    return FR_ERR_INVALID;
  }
  if (fr_esp_pending_byte_valid) {
    *out_byte = fr_esp_pending_byte;
    fr_esp_pending_byte_valid = false;
    return FR_OK;
  }

  read = uart_read_bytes(FR_BOARD_UART_PORT, out_byte, 1, timeout);
  if (read < 0) {
    return FR_ERR_IO;
  }
  return read == 1 ? FR_OK : FR_ERR_NOT_FOUND;
}

static void fr_esp_save_pending_byte(uint8_t byte) {
  fr_esp_pending_byte = byte;
  fr_esp_pending_byte_valid = true;
}

fr_err_t fr_platform_delay_ms(uint16_t ms) {
  if (ms == 0) {
    return FR_OK;
  }

  vTaskDelay(pdMS_TO_TICKS(ms));
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

static bool fr_esp_gpio_pin_valid(uint16_t pin) {
  return pin < GPIO_NUM_MAX && ((1ULL << pin) & SOC_GPIO_VALID_GPIO_MASK) != 0;
}

static bool fr_esp_gpio_output_valid(uint16_t pin) {
  return pin < GPIO_NUM_MAX &&
         ((1ULL << pin) & SOC_GPIO_VALID_OUTPUT_GPIO_MASK) != 0;
}

#if FR_FEATURE_UART
static fr_err_t fr_esp_uart_baud(uint16_t rate_code, int *out_baud) {
  if (out_baud == NULL) {
    return FR_ERR_INVALID;
  }

  switch (rate_code) {
  case FR_UART_RATE_9600:
    *out_baud = 9600;
    return FR_OK;
  case FR_UART_RATE_19200:
    *out_baud = 19200;
    return FR_OK;
  case FR_UART_RATE_38400:
    *out_baud = 38400;
    return FR_OK;
  case FR_UART_RATE_57600:
    *out_baud = 57600;
    return FR_OK;
  case FR_UART_RATE_115200:
    *out_baud = 115200;
    return FR_OK;
  default:
    return FR_ERR_DOMAIN;
  }
}

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
  return tx == FR_BOARD_UART_TX || tx == FR_BOARD_UART_RX ||
         rx == FR_BOARD_UART_TX || rx == FR_BOARD_UART_RX;
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

  config.intr_type = GPIO_INTR_DISABLE;
  config.pin_bit_mask = 1ULL << pin;
  return fr_esp_err(gpio_config(&config));
}

fr_err_t fr_platform_gpio_write(uint16_t pin, uint16_t value) {
  if (!fr_esp_gpio_output_valid(pin)) {
    return FR_ERR_DOMAIN;
  }

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
  if (out_channel == NULL) {
    return false;
  }

  switch (pin) {
  case 36:
    *out_channel = ADC_CHANNEL_0;
    return true;
  case 37:
    *out_channel = ADC_CHANNEL_1;
    return true;
  case 38:
    *out_channel = ADC_CHANNEL_2;
    return true;
  case 39:
    *out_channel = ADC_CHANNEL_3;
    return true;
  case 32:
    *out_channel = ADC_CHANNEL_4;
    return true;
  case 33:
    *out_channel = ADC_CHANNEL_5;
    return true;
  case 34:
    *out_channel = ADC_CHANNEL_6;
    return true;
  case 35:
    *out_channel = ADC_CHANNEL_7;
    return true;
  default:
    return false;
  }
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

fr_err_t fr_platform_poll_interrupt(fr_runtime_t *runtime) {
  uint8_t byte = 0;
  fr_err_t err = fr_esp_read_byte(&byte, 0);

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (err == FR_ERR_NOT_FOUND) {
    return FR_OK;
  }
  FR_TRY(err);
  if (byte == FR_ESP_CTRL_C) {
    fr_runtime_interrupt(runtime);
  } else {
    fr_esp_save_pending_byte(byte);
  }
  return FR_OK;
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
#if FR_FEATURE_NET
  if (kind == FR_HANDLE_KIND_TCP) {
    return fr_platform_tcp_close(platform_index);
  }
#endif
  (void)kind;
  (void)platform_index;
  return FR_OK;
}

#if FR_FEATURE_UART
fr_err_t fr_platform_uart_open(uint16_t port, uint16_t rate_code,
                               uint16_t *out_platform_index) {
  int baud = 0;
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
    return FR_ERR_DOMAIN;
  }
  FR_TRY(fr_esp_uart_baud(rate_code, &baud));
  config.baud_rate = baud;

  err = uart_driver_install(target_port, FR_ESP_APP_UART_RX_BYTES,
                            FR_ESP_APP_UART_TX_BYTES, 0, NULL, 0);
  if (err != ESP_OK) {
    return fr_esp_err(err);
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
      .rate_code = rate_code,
  };
  *out_platform_index = port;
  return FR_OK;
}

/* uart.open-on: caller picks tx/rx pins, with conflict checks against the
 * console UART and any already-open custom-pin UART. Setup mirrors
 * fr_platform_uart_open and adds a uart_set_pin step before flushing input. */
fr_err_t fr_platform_uart_open_on(uint16_t port, uint16_t tx, uint16_t rx,
                                  uint16_t rate_code,
                                  uint16_t *out_platform_index) {
  int baud = 0;
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
    return FR_ERR_DOMAIN;
  }
  FR_TRY(fr_esp_uart_baud(rate_code, &baud));
  config.baud_rate = baud;

  err = uart_driver_install(target_port, FR_ESP_APP_UART_RX_BYTES,
                            FR_ESP_APP_UART_TX_BYTES, 0, NULL, 0);
  if (err != ESP_OK) {
    return fr_esp_err(err);
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
      .rate_code = rate_code,
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

fr_err_t fr_platform_read_line(char *line, uint16_t cap, bool *out_eof) {
  uint16_t used = 0;

  if (line == NULL || cap == 0 || out_eof == NULL) {
    return FR_ERR_INVALID;
  }

  *out_eof = false;
  line[0] = '\0';
  for (;;) {
    uint8_t byte = 0;
    fr_err_t err = fr_esp_read_byte(&byte, pdMS_TO_TICKS(20));

    if (err == FR_ERR_NOT_FOUND) {
      /* No byte this tick: service timer/interrupt events so they fire at the
       * idle prompt. The handler reports its own faults and returns OK; the
       * read loop keeps running regardless. */
      if (fr_esp_idle_handler != NULL) {
        (void)fr_esp_idle_handler(fr_esp_idle_ctx);
      }
      continue;
    }
    FR_TRY(err);

    if (byte == '\r' || byte == '\n') {
      line[used] = '\0';
      fr_platform_write_text("\n");
      return FR_OK;
    }
    if (byte == FR_ESP_CTRL_C) {
      line[0] = '\0';
      fr_platform_write_text("^C\n");
      return FR_OK;
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
    (void)uart_write_bytes(FR_BOARD_UART_PORT, &byte, 1);
  }
}

fr_err_t fr_platform_write_text(const char *text) {
  if (text == NULL) {
    return FR_ERR_INVALID;
  }

  while (*text != '\0') {
    if (*text == '\n') {
      const char cr = '\r';
      if (uart_write_bytes(FR_BOARD_UART_PORT, &cr, 1) < 0) {
        return FR_ERR_IO;
      }
    }
    if (uart_write_bytes(FR_BOARD_UART_PORT, text, 1) < 0) {
      return FR_ERR_IO;
    }
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
  if (length > 0 &&
      uart_write_bytes(FR_BOARD_UART_PORT, bytes, length) != length) {
    return FR_ERR_IO;
  }
  return FR_OK;
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
    return FR_ERR_DOMAIN;
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
static uint8_t fr_esp_storage_image[FR_PROFILE_PERSISTENCE_BYTES];

static bool fr_esp_storage_bounds(uint8_t slot, uint16_t offset,
                                  uint16_t length) {
  return slot < FR_ESP_STORAGE_SLOT_COUNT &&
         (uint32_t)offset + length <= FR_PROFILE_PERSISTENCE_BYTES;
}

static const char *fr_esp_storage_key(uint8_t slot) {
  switch (slot) {
  case 0:
    return "slot0";
  case 1:
    return "slot1";
  default:
    return NULL;
  }
}

static fr_err_t fr_esp_storage_open(nvs_open_mode_t mode,
                                    nvs_handle_t *out_handle) {
  if (out_handle == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_esp_platform_init());
  return fr_esp_err(nvs_open("frothy", mode, out_handle));
}

static fr_err_t fr_esp_storage_load(nvs_handle_t handle, const char *key,
                                    uint8_t *image) {
  size_t length = FR_PROFILE_PERSISTENCE_BYTES;
  esp_err_t err = ESP_OK;

  if (key == NULL || image == NULL) {
    return FR_ERR_INVALID;
  }

  memset(image, 0xff, FR_PROFILE_PERSISTENCE_BYTES);
  err = nvs_get_blob(handle, key, image, &length);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return FR_OK;
  }
  FR_TRY(fr_esp_err(err));
  if (length != FR_PROFILE_PERSISTENCE_BYTES) {
    return FR_ERR_CORRUPT;
  }
  return FR_OK;
}

fr_err_t fr_platform_storage_read(uint8_t slot, uint16_t offset, uint8_t *bytes,
                                  uint16_t length) {
  nvs_handle_t handle = 0;
  const char *key = fr_esp_storage_key(slot);
  fr_err_t err = FR_OK;

  if ((bytes == NULL && length > 0) ||
      !fr_esp_storage_bounds(slot, offset, length) || key == NULL) {
    return FR_ERR_INVALID;
  }

  err = fr_esp_storage_open(NVS_READONLY, &handle);
  if (err == FR_ERR_NOT_FOUND) {
    memset(fr_esp_storage_image, 0xff, FR_PROFILE_PERSISTENCE_BYTES);
    err = FR_OK;
  }
  if (err == FR_OK && handle != 0) {
    err = fr_esp_storage_load(handle, key, fr_esp_storage_image);
  }
  if (err == FR_OK && length > 0) {
    memcpy(bytes, &fr_esp_storage_image[offset], length);
  }
  if (handle != 0) {
    nvs_close(handle);
  }
  return err;
}

fr_err_t fr_platform_storage_write(uint8_t slot, uint16_t offset,
                                   const uint8_t *bytes, uint16_t length) {
  nvs_handle_t handle = 0;
  const char *key = fr_esp_storage_key(slot);
  fr_err_t err = FR_OK;

  if ((bytes == NULL && length > 0) ||
      !fr_esp_storage_bounds(slot, offset, length) || key == NULL) {
    return FR_ERR_INVALID;
  }

  err = fr_esp_storage_open(NVS_READWRITE, &handle);
  if (err == FR_OK) {
    err = fr_esp_storage_load(handle, key, fr_esp_storage_image);
  }
  if (err == FR_OK && length > 0) {
    memcpy(&fr_esp_storage_image[offset], bytes, length);
    err = fr_esp_err(nvs_set_blob(handle, key, fr_esp_storage_image,
                                  FR_PROFILE_PERSISTENCE_BYTES));
  }
  if (err == FR_OK) {
    err = fr_esp_err(nvs_commit(handle));
  }
  if (handle != 0) {
    nvs_close(handle);
  }
  return err;
}

fr_err_t fr_platform_storage_erase(uint8_t slot) {
  nvs_handle_t handle = 0;
  const char *key = fr_esp_storage_key(slot);
  fr_err_t err = FR_OK;
  esp_err_t erase_err = ESP_OK;

  if (slot >= FR_ESP_STORAGE_SLOT_COUNT || key == NULL) {
    return FR_ERR_INVALID;
  }

  err = fr_esp_storage_open(NVS_READWRITE, &handle);
  if (err != FR_OK) {
    return err;
  }
  erase_err = nvs_erase_key(handle, key);
  if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
    err = fr_esp_err(erase_err);
  }
  if (err == FR_OK) {
    err = fr_esp_err(nvs_commit(handle));
  }
  nvs_close(handle);
  return err;
}

void fr_platform_storage_debug_reset(void) {
  for (uint8_t slot = 0; slot < FR_ESP_STORAGE_SLOT_COUNT; slot++) {
    (void)fr_platform_storage_erase(slot);
  }
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
    if (esp_sleep_enable_ext0_wakeup((gpio_num_t)fr_esp_sleep_pending_pin,
                                     (int)fr_esp_sleep_pending_level) !=
        ESP_OK) {
      return FR_ERR_IO;
    }
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
