#include "platform.h"

#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <stdbool.h>
#include <util/delay.h>

#include "board.h"
#include "runtime.h"

enum {
  FR_AVR_CTRL_C = 0x03u,
  FR_AVR_MILLIS_WRAP = 16384u,
  FR_AVR_GPIO_MODE_INPUT = 0u,
  FR_AVR_GPIO_MODE_OUTPUT = 1u,
  FR_AVR_GPIO_MODE_INPUT_PULLUP = 2u,
};

static bool fr_avr_uart_pending = false;
static uint8_t fr_avr_uart_pending_ch = 0;
static volatile uint16_t fr_avr_millis = 0;

#define FR_AVR_TIMER0_COMPARE ((F_CPU / 64UL / 1000UL) - 1UL)

#if FR_AVR_TIMER0_COMPARE > 255UL
#error "Timer0 compare value does not fit 8-bit OCR0A"
#endif

ISR(TIMER0_COMPA_vect) {
  fr_avr_millis = (uint16_t)((fr_avr_millis + 1u) % FR_AVR_MILLIS_WRAP);
}

static void fr_avr_millis_init(void) {
  static bool initialized = false;

  if (initialized) {
    return;
  }

  TCCR0A = (uint8_t)(1u << WGM01);
  TCCR0B = (uint8_t)((1u << CS01) | (1u << CS00));
  OCR0A = (uint8_t)FR_AVR_TIMER0_COMPARE;
  TIMSK0 |= (uint8_t)(1u << OCIE0A);
  sei();
  initialized = true;
}

static void fr_avr_uart_init(void) {
  static bool initialized = false;
  uint16_t ubrr =
      (uint16_t)(((F_CPU + (4UL * FR_BOARD_UART_BAUD)) /
                  (8UL * FR_BOARD_UART_BAUD)) -
                 1UL);

  if (initialized) {
    return;
  }

  UCSR0A = (uint8_t)(1u << U2X0);
  UBRR0H = (uint8_t)(ubrr >> 8);
  UBRR0L = (uint8_t)ubrr;
  UCSR0B = (uint8_t)((1u << RXEN0) | (1u << TXEN0));
  UCSR0C = (uint8_t)((1u << UCSZ01) | (1u << UCSZ00));
  initialized = true;
}

static void fr_avr_uart_put(char ch) {
  fr_avr_uart_init();
  while ((UCSR0A & (1u << UDRE0)) == 0) {
  }
  UDR0 = (uint8_t)ch;
}

static uint8_t fr_avr_uart_get(void) {
  fr_avr_uart_init();
  if (fr_avr_uart_pending) {
    fr_avr_uart_pending = false;
    return fr_avr_uart_pending_ch;
  }
  while ((UCSR0A & (1u << RXC0)) == 0) {
  }
  return UDR0;
}

static bool fr_avr_uart_poll(uint8_t *out_ch) {
  fr_avr_uart_init();
  if (out_ch == NULL || fr_avr_uart_pending ||
      (UCSR0A & (1u << RXC0)) == 0) {
    return false;
  }

  *out_ch = UDR0;
  return true;
}

static void fr_avr_uart_write_crlf(void) {
  fr_avr_uart_put('\r');
  fr_avr_uart_put('\n');
}

static void fr_avr_uart_write_backspace(void) {
  fr_avr_uart_put('\b');
  fr_avr_uart_put(' ');
  fr_avr_uart_put('\b');
}

fr_err_t fr_platform_delay_ms(uint16_t ms) {
  fr_avr_millis_init();
  while (ms > 0) {
    _delay_ms(1.0);
    ms -= 1;
  }
  return FR_OK;
}

fr_err_t fr_platform_millis(uint16_t *out_ms) {
  uint8_t sreg = 0;

  if (out_ms == NULL) {
    return FR_ERR_INVALID;
  }

  fr_avr_millis_init();
  sreg = SREG;
  cli();
  *out_ms = fr_avr_millis;
  SREG = sreg;
  return FR_OK;
}

static fr_err_t fr_avr_gpio_pin(uint16_t pin, volatile uint8_t **out_ddr,
                                volatile uint8_t **out_port,
                                volatile uint8_t **out_pin,
                                uint8_t *out_bit) {
  if (out_ddr == NULL || out_port == NULL || out_pin == NULL ||
      out_bit == NULL) {
    return FR_ERR_INVALID;
  }

  if (pin <= 7) {
    *out_ddr = &DDRD;
    *out_port = &PORTD;
    *out_pin = &PIND;
    *out_bit = (uint8_t)pin;
    return FR_OK;
  }
  if (pin <= 13) {
    *out_ddr = &DDRB;
    *out_port = &PORTB;
    *out_pin = &PINB;
    *out_bit = (uint8_t)(pin - 8u);
    return FR_OK;
  }
  if (pin <= 19) {
    *out_ddr = &DDRC;
    *out_port = &PORTC;
    *out_pin = &PINC;
    *out_bit = (uint8_t)(pin - 14u);
    return FR_OK;
  }
  return FR_ERR_DOMAIN;
}

fr_err_t fr_platform_gpio_mode(uint16_t pin, uint16_t mode) {
  volatile uint8_t *ddr = NULL;
  volatile uint8_t *port = NULL;
  volatile uint8_t *pin_reg = NULL;
  uint8_t bit = 0;
  uint8_t mask = 0;

  FR_TRY(fr_avr_gpio_pin(pin, &ddr, &port, &pin_reg, &bit));
  (void)pin_reg;
  mask = (uint8_t)(1u << bit);
  if (mode == FR_AVR_GPIO_MODE_INPUT) {
    *ddr &= (uint8_t)(~mask);
    *port &= (uint8_t)(~mask);
    return FR_OK;
  }
  if (mode == FR_AVR_GPIO_MODE_OUTPUT) {
    *ddr |= mask;
    return FR_OK;
  }
  if (mode == FR_AVR_GPIO_MODE_INPUT_PULLUP) {
    *ddr &= (uint8_t)(~mask);
    *port |= mask;
    return FR_OK;
  }
  return FR_ERR_DOMAIN;
}

fr_err_t fr_platform_gpio_write(uint16_t pin, uint16_t value) {
  volatile uint8_t *ddr = NULL;
  volatile uint8_t *port = NULL;
  volatile uint8_t *pin_reg = NULL;
  uint8_t bit = 0;
  uint8_t mask = 0;

  FR_TRY(fr_avr_gpio_pin(pin, &ddr, &port, &pin_reg, &bit));
  (void)pin_reg;
  mask = (uint8_t)(1u << bit);
  *ddr |= mask;
  if (value == 0) {
    *port &= (uint8_t)(~mask);
  } else {
    *port |= mask;
  }
  return FR_OK;
}

fr_err_t fr_platform_gpio_read(uint16_t pin, uint16_t *out_value) {
  volatile uint8_t *ddr = NULL;
  volatile uint8_t *port = NULL;
  volatile uint8_t *pin_reg = NULL;
  uint8_t bit = 0;

  if (out_value == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_avr_gpio_pin(pin, &ddr, &port, &pin_reg, &bit));
  (void)ddr;
  (void)port;
  *out_value = (*pin_reg & (uint8_t)(1u << bit)) == 0 ? 0u : 1u;
  return FR_OK;
}

static void fr_avr_adc_init(void) {
  ADCSRA |= (uint8_t)((1u << ADEN) | (1u << ADPS2) | (1u << ADPS1) |
                      (1u << ADPS0));
}

static fr_err_t fr_avr_adc_channel(uint16_t pin, uint8_t *out_channel) {
  if (out_channel == NULL) {
    return FR_ERR_INVALID;
  }
  if (pin <= 5) {
    *out_channel = (uint8_t)pin;
    return FR_OK;
  }
  if (pin >= 14 && pin <= 19) {
    *out_channel = (uint8_t)(pin - 14u);
    return FR_OK;
  }
  return FR_ERR_DOMAIN;
}

fr_err_t fr_platform_adc_read(uint16_t pin, uint16_t *out_value) {
  uint8_t channel = 0;

  if (out_value == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_avr_adc_channel(pin, &channel));
  fr_avr_adc_init();
  ADMUX = (uint8_t)((1u << REFS0) | (channel & 0x0fu));
  ADCSRA |= (uint8_t)(1u << ADSC);
  while ((ADCSRA & (uint8_t)(1u << ADSC)) != 0) {
  }
  *out_value = ADC;
  return FR_OK;
}

fr_err_t fr_platform_poll_interrupt(fr_runtime_t *runtime) {
  uint8_t ch = 0;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_avr_uart_poll(&ch)) {
    return FR_OK;
  }
  if (ch == FR_AVR_CTRL_C) {
    fr_runtime_interrupt(runtime);
    return FR_OK;
  }

  fr_avr_uart_pending_ch = ch;
  fr_avr_uart_pending = true;
  return FR_OK;
}

fr_err_t fr_platform_handle_close(fr_handle_kind_t kind,
                                  uint16_t platform_index) {
  (void)kind;
  (void)platform_index;
  return FR_OK;
}

#if FR_FEATURE_REPL
fr_err_t fr_platform_read_line(char *line, uint16_t cap, bool *out_eof) {
  static bool skip_lf_after_cr = false;
  uint16_t used = 0;

  if (line == NULL || cap == 0 || out_eof == NULL) {
    return FR_ERR_INVALID;
  }

  *out_eof = false;
  while (true) {
    uint8_t ch = fr_avr_uart_get();

    if (skip_lf_after_cr && ch == '\n') {
      skip_lf_after_cr = false;
      continue;
    }
    skip_lf_after_cr = false;

    if (ch == '\r' || ch == '\n') {
      if (ch == '\r') {
        skip_lf_after_cr = true;
      }
      line[used] = '\0';
      fr_avr_uart_write_crlf();
      return FR_OK;
    }
    if (ch == '\b' || ch == 0x7fu) {
      if (used > 0) {
        used -= 1;
        fr_avr_uart_write_backspace();
      } else {
        fr_avr_uart_put('\a');
      }
      continue;
    }
    if (ch == '\t') {
      ch = ' ';
    } else if (ch < 0x20u || ch > 0x7eu) {
      continue;
    }
    if ((uint32_t)used + 1 >= cap) {
      fr_avr_uart_put('\a');
      continue;
    }
    line[used] = (char)ch;
    used += 1;
    fr_avr_uart_put((char)ch);
  }
}

fr_err_t fr_platform_write_text(const char *text) {
  if (text == NULL) {
    return FR_ERR_INVALID;
  }

  while (*text != '\0') {
    if (*text == '\n') {
      fr_avr_uart_put('\r');
    }
    fr_avr_uart_put(*text);
    text += 1;
  }
  return FR_OK;
}
#endif

#if FR_FEATURE_PERSISTENCE
enum {
  FR_PLATFORM_STORAGE_SLOT_COUNT = 2,
};

static uint8_t EEMEM
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
    eeprom_read_block(bytes, &fr_platform_storage[slot][offset], length);
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
    eeprom_update_block(bytes, &fr_platform_storage[slot][offset], length);
  }
  return FR_OK;
}

fr_err_t fr_platform_storage_erase(uint8_t slot) {
  if (slot >= FR_PLATFORM_STORAGE_SLOT_COUNT) {
    return FR_ERR_INVALID;
  }

  for (uint16_t i = 0; i < FR_PROFILE_PERSISTENCE_BYTES; i++) {
    eeprom_update_byte(&fr_platform_storage[slot][i], 0xffu);
  }
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
  return FR_ERR_UNSUPPORTED;
}

fr_err_t fr_platform_event_gpio_remove(uint16_t pin) {
  (void)pin;
  return FR_ERR_UNSUPPORTED;
}

fr_err_t fr_platform_event_timer_install(fr_event_kind_t kind, uint32_t ms,
                                         uint16_t binding_index,
                                         uint16_t generation) {
  (void)kind;
  (void)ms;
  (void)binding_index;
  (void)generation;
  return FR_ERR_UNSUPPORTED;
}

fr_err_t fr_platform_event_timer_remove(uint16_t binding_index) {
  (void)binding_index;
  return FR_ERR_UNSUPPORTED;
}

fr_err_t fr_platform_event_drain(fr_event_candidate_t *out_events,
                                 uint8_t out_cap, uint8_t *out_count,
                                 uint32_t *overflow_delta) {
  (void)out_events;
  (void)out_cap;
  if (out_count != NULL) {
    *out_count = 0;
  }
  if (overflow_delta != NULL) {
    *overflow_delta = 0;
  }
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
