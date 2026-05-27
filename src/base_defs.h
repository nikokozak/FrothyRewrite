#pragma once

#include "image.h"
#include "native.h"
#include "types.h"

enum {
  FR_SLOT_BOOT = 0,
  FR_SLOT_MS = 1,
  FR_SLOT_ONE = 2,
  FR_SLOT_GPIO_WRITE = 3,
  FR_SLOT_LED_BUILTIN = 4,
  FR_SLOT_SAVE = 5,
  FR_SLOT_RESTORE = 6,
  FR_SLOT_WIPE = 7,
  FR_SLOT_GPIO_MODE = 8,
  FR_SLOT_GPIO_READ = 9,
  FR_SLOT_ADC_READ = 10,
  FR_SLOT_ADC_ABOVE = 11,
  FR_SLOT_MILLIS = 12,
#if FR_FEATURE_UART
  FR_SLOT_UART_OPEN = 13,
  FR_SLOT_UART_WRITE_BYTE = 14,
  FR_SLOT_UART_READ_BYTE = 15,
  FR_SLOT_UART_AVAILABLE = 16,
  FR_SLOT_UART_CLOSE = 17,
  FR_SLOT_BAUD_9600 = 18,
  FR_SLOT_BAUD_19200 = 19,
  FR_SLOT_BAUD_38400 = 20,
  FR_SLOT_BAUD_57600 = 21,
  FR_SLOT_BAUD_115200 = 22,
  FR_SLOT_AFTER_UART = 23,
#else
  FR_SLOT_AFTER_UART = 13,
#endif
#if FR_FEATURE_PAD
  FR_SLOT_PAD_RESET = FR_SLOT_AFTER_UART,
  FR_SLOT_PAD_EMIT_BYTE = FR_SLOT_AFTER_UART + 1,
  FR_SLOT_PAD_LEN = FR_SLOT_AFTER_UART + 2,
  FR_SLOT_PAD_TYPE = FR_SLOT_AFTER_UART + 3,
  FR_SLOT_PAD_PEEK_BYTE = FR_SLOT_AFTER_UART + 4,
#if FR_FEATURE_TEXT
  FR_SLOT_PAD_PACK = FR_SLOT_AFTER_UART + 5,
  FR_SLOT_BOARD_LOCAL_BASE = FR_SLOT_AFTER_UART + 6,
#else
  FR_SLOT_BOARD_LOCAL_BASE = FR_SLOT_AFTER_UART + 5,
#endif
#else
  FR_SLOT_BOARD_LOCAL_BASE = FR_SLOT_AFTER_UART,
#endif
};

#if FR_SLOT_BOARD_LOCAL_BASE > FR_PROFILE_MAX_SLOTS
#error "base slot contract exceeds FR_PROFILE_MAX_SLOTS"
#endif

typedef enum fr_base_layer_t {
  FR_BASE_LAYER_CORE,
  FR_BASE_LAYER_TARGET,
  FR_BASE_LAYER_BOARD,
  FR_BASE_LAYER_PERSISTENCE,
} fr_base_layer_t;

typedef enum fr_base_def_kind_t {
  FR_BASE_DEF_LITERAL,
  FR_BASE_DEF_NATIVE,
} fr_base_def_kind_t;

typedef struct fr_base_def_t {
  fr_slot_id_t slot_id;
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  const char *name;
  const char *alias;
#endif
  fr_base_def_kind_t kind;
  fr_tagged_t literal_tagged;
  fr_native_fn_t native_fn;
  uint8_t native_arity;
#if FR_FEATURE_NATIVE_SIGNATURES
  const fr_native_signature_t *native_signature;
#endif
} fr_base_def_t;

typedef struct fr_base_def_layer_t {
  fr_base_layer_t layer;
  const fr_base_def_t *defs;
  uint16_t count;
} fr_base_def_layer_t;

extern const fr_base_def_t fr_target_base_defs[];
extern const uint16_t fr_target_base_def_count;
extern const fr_base_def_t fr_board_base_defs[];
extern const uint16_t fr_board_base_def_count;

uint16_t fr_base_def_layer_count(void);
fr_err_t fr_base_def_layer_at(uint16_t index,
                              fr_base_def_layer_t *out_layer);
uint16_t fr_base_def_count(void);
fr_err_t fr_base_def_at(uint16_t index, const fr_base_def_t **out_def,
                        fr_base_layer_t *out_layer);
fr_err_t fr_base_def_for_slot(fr_slot_id_t slot_id,
                              const fr_base_def_t **out_def,
                              fr_base_layer_t *out_layer);

uint16_t fr_base_slot_count(void);
const char *fr_base_slot_name_at(uint16_t index);
const char *fr_base_slot_name(fr_slot_id_t slot_id);
fr_err_t fr_base_slot_id_for_name(const char *name, fr_slot_id_t *out_slot_id);
fr_err_t fr_base_slot_layer(fr_slot_id_t slot_id,
                            fr_base_layer_t *out_layer);
fr_err_t fr_base_slot_ref(fr_slot_id_t slot_id, fr_image_ref_t *out_ref);
