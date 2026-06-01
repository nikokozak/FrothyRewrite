#include "platform.h"

#include "handle.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
  FR_HOST_MAX_PIN = 39,
  FR_HOST_ADC_MAX_PIN = 39,
  FR_HOST_MILLIS_WRAP = 16384,
#if FR_FEATURE_UART
  FR_HOST_UART_SCRIPT_LENGTH = 5,
#endif
};

static uint8_t fr_host_gpio_values[FR_HOST_MAX_PIN + 1];
static uint16_t fr_host_millis;

#if FR_FEATURE_PWM
typedef struct fr_host_pwm_t {
  bool in_use;
  uint16_t pin;
  uint16_t freq;
  uint16_t duty;
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
  uint16_t rate_code;
  uint8_t read_index;
  uint8_t last_written;
  uint16_t write_count;
} fr_host_uart_t;

static fr_host_uart_t fr_host_uarts[FR_PROFILE_MAX_HANDLES];
static const uint8_t fr_host_uart_script[FR_HOST_UART_SCRIPT_LENGTH] = {
    'f', 'r', 'o', 't', 'h',
};

static bool fr_host_uart_rate_valid(uint16_t rate_code) {
  switch (rate_code) {
  case FR_UART_RATE_9600:
  case FR_UART_RATE_19200:
  case FR_UART_RATE_38400:
  case FR_UART_RATE_57600:
  case FR_UART_RATE_115200:
    return true;
  default:
    return false;
  }
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
  fr_host_millis = (uint16_t)((fr_host_millis + ms) % FR_HOST_MILLIS_WRAP);
  return FR_OK;
}

fr_err_t fr_platform_millis(uint16_t *out_ms) {
  if (out_ms == NULL) {
    return FR_ERR_INVALID;
  }

  *out_ms = fr_host_millis;
  return FR_OK;
}

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
  (void)kind;
  (void)platform_index;
  return FR_OK;
}

#if FR_FEATURE_UART
fr_err_t fr_platform_uart_open(uint16_t port, uint16_t rate_code,
                               uint16_t *out_platform_index) {
  if (out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (port > FR_HOST_UART_MAX_PORT || !fr_host_uart_rate_valid(rate_code) ||
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
        .rate_code = rate_code,
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
                                  uint16_t rate_code,
                                  uint16_t *out_platform_index) {
  if (out_platform_index == NULL) {
    return FR_ERR_INVALID;
  }
  if (port > FR_HOST_UART_MAX_PORT || !fr_host_uart_rate_valid(rate_code) ||
      fr_host_uart_port_in_use(port) || tx == rx ||
      fr_host_uart_pin_conflict(tx, rx)) {
    return FR_ERR_DOMAIN;
  }

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
        .rate_code = rate_code,
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

#if FR_FEATURE_REPL
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
  pwm->duty = duty;
  return FR_OK;
}

fr_err_t fr_platform_pwm_close(uint16_t platform_index) {
  if (platform_index >= FR_PROFILE_MAX_HANDLES) {
    return FR_OK;
  }
  memset(&fr_host_pwms[platform_index], 0, sizeof(fr_host_pwms[platform_index]));
  return FR_OK;
}
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

#if FR_FEATURE_PAD
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
#endif

#if FR_FEATURE_PERSISTENCE
enum {
  FR_PLATFORM_STORAGE_SLOT_COUNT = 2,
};

static uint8_t
    fr_platform_storage[FR_PLATFORM_STORAGE_SLOT_COUNT][FR_PROFILE_PERSISTENCE_BYTES];

static bool fr_platform_storage_bounds(uint8_t slot, uint16_t offset,
                                       uint16_t length) {
  return slot < FR_PLATFORM_STORAGE_SLOT_COUNT &&
         (uint32_t)offset + length <= FR_PROFILE_PERSISTENCE_BYTES;
}

fr_err_t fr_platform_storage_read(uint8_t slot, uint16_t offset, uint8_t *bytes,
                                  uint16_t length) {
  if ((bytes == NULL && length > 0) ||
      !fr_platform_storage_bounds(slot, offset, length)) {
    return FR_ERR_INVALID;
  }

  if (length > 0) {
    memcpy(bytes, &fr_platform_storage[slot][offset], length);
  }
  return FR_OK;
}

fr_err_t fr_platform_storage_write(uint8_t slot, uint16_t offset,
                                   const uint8_t *bytes, uint16_t length) {
  if ((bytes == NULL && length > 0) ||
      !fr_platform_storage_bounds(slot, offset, length)) {
    return FR_ERR_INVALID;
  }

  if (length > 0) {
    memcpy(&fr_platform_storage[slot][offset], bytes, length);
  }
  return FR_OK;
}

fr_err_t fr_platform_storage_erase(uint8_t slot) {
  if (slot >= FR_PLATFORM_STORAGE_SLOT_COUNT) {
    return FR_ERR_INVALID;
  }

  memset(fr_platform_storage[slot], 0xff, sizeof(fr_platform_storage[slot]));
  return FR_OK;
}

void fr_platform_storage_debug_reset(void) {
  for (uint8_t slot = 0; slot < FR_PLATFORM_STORAGE_SLOT_COUNT; slot++) {
    (void)fr_platform_storage_erase(slot);
  }
}
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

fr_err_t fr_platform_event_timer_install(fr_event_kind_t kind, uint32_t ms,
                                         uint16_t binding_index,
                                         uint16_t generation) {
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
