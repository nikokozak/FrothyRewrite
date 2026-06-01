#include "froth.h"
#include "crc.h"
#include "event.h"
#if FR_FEATURE_PERSISTENCE
#include "persist_payload.h"
#endif
#if FR_FEATURE_COMPILER
#include "compile.h"
#include "parse.h"
#endif
#include "platform.h"
#include "repl.h"
#if FR_FEATURE_SOURCE_BASE
#include "fr_source_base.h"
#endif

#ifndef FR_HOST_TINY_NAMES_MODE
#define FR_HOST_TINY_NAMES_MODE 0
#endif

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#if FR_FEATURE_UART
#define FR_TEST_UART_WORDS                                                   \
  " uart.open uart.open-on uart.write-byte uart.read-byte uart.available "   \
  "uart.close $baud_9600 $baud_19200 $baud_38400 $baud_57600 $baud_115200"
#define FR_TEST_UART_SLOT_COUNT 11
#else
#define FR_TEST_UART_WORDS ""
#define FR_TEST_UART_SLOT_COUNT 0
#endif

#if FR_FEATURE_RANDOM
#define FR_TEST_RANDOM_WORDS " random.next random.below random.seed"
#define FR_TEST_RANDOM_SLOT_COUNT 3
#else
#define FR_TEST_RANDOM_WORDS ""
#define FR_TEST_RANDOM_SLOT_COUNT 0
#endif

#if FR_FEATURE_PWM
#define FR_TEST_PWM_WORDS " pwm.open pwm.write pwm.close"
#define FR_TEST_PWM_SLOT_COUNT 3
#else
#define FR_TEST_PWM_WORDS ""
#define FR_TEST_PWM_SLOT_COUNT 0
#endif

#if FR_FEATURE_I2C
#define FR_TEST_I2C_WORDS " i2c.open i2c.write i2c.read i2c.close"
#define FR_TEST_I2C_SLOT_COUNT 4
#else
#define FR_TEST_I2C_WORDS ""
#define FR_TEST_I2C_SLOT_COUNT 0
#endif

#if FR_FEATURE_MATH
#define FR_TEST_MATH_WORDS " abs min max clamp map mod"
#define FR_TEST_MATH_SLOT_COUNT 6
#else
#define FR_TEST_MATH_WORDS ""
#define FR_TEST_MATH_SLOT_COUNT 0
#endif

#if FR_FEATURE_PAD
#if FR_FEATURE_TEXT
#define FR_TEST_PAD_PACK_WORD " pad.pack"
#define FR_TEST_PAD_PACK_SLOT_COUNT 1
#define FR_TEST_PAD_LAST_SLOT FR_SLOT_PAD_PACK
#else
#define FR_TEST_PAD_PACK_WORD ""
#define FR_TEST_PAD_PACK_SLOT_COUNT 0
#define FR_TEST_PAD_LAST_SLOT FR_SLOT_PAD_PEEK_BYTE
#endif
#define FR_TEST_PAD_WORDS                                                    \
  " pad.reset pad.emit-byte pad.len pad.type pad.peek-byte"                  \
      FR_TEST_PAD_PACK_WORD
#define FR_TEST_PAD_SLOT_COUNT (5 + FR_TEST_PAD_PACK_SLOT_COUNT)
#else
#define FR_TEST_PAD_WORDS ""
#define FR_TEST_PAD_SLOT_COUNT 0
#endif

#if FR_FEATURE_TEXT
#define FR_TEST_TEXT_WORDS                                                    \
  " text.length text.equals? text.concat text.at text.from-int"
#define FR_TEST_TEXT_SLOT_COUNT 5
#else
#define FR_TEST_TEXT_WORDS ""
#define FR_TEST_TEXT_SLOT_COUNT 0
#endif

#define FR_TEST_EVENT_REGISTER_WORDS " frothy.event-register"
#define FR_TEST_EVENT_REGISTER_SLOT_COUNT 1

#if FR_INCLUDE_TEST_NATIVES && FR_FEATURE_TEXT
#define FR_TEST_EVENT_TEST_WORDS " frothy.fire-event"
#define FR_TEST_EVENT_TEST_SLOT_COUNT 1
#else
#define FR_TEST_EVENT_TEST_WORDS ""
#define FR_TEST_EVENT_TEST_SLOT_COUNT 0
#endif

enum {
  FR_TEST_PERSIST_RECORD_BIND = 2,
  FR_TEST_PERSIST_RECORD_NAME = 3,
  FR_TEST_PERSIST_RECORD_CELLS = 4,
  FR_TEST_PERSIST_RECORD_TEXT = 5,
  FR_TEST_PERSIST_RECORD_RECORD_SHAPE = 6,
  FR_TEST_PERSIST_RECORD_RECORD = 7,
  FR_TEST_PERSIST_RECORD_END = 0xff,
  FR_TEST_PERSIST_VALUE_NIL = 0,
  FR_TEST_PERSIST_VALUE_INT = 3,
  FR_TEST_PERSIST_VALUE_OBJECT = 6,
};

#if FR_FEATURE_PERSISTENCE
#define FR_TEST_WORDS                                                        \
  "boot ms one gpio.write $led_builtin save restore dangerous.wipe gpio.mode "  \
  "gpio.read adc.read adc.above millis" FR_TEST_UART_WORDS FR_TEST_RANDOM_WORDS  \
      FR_TEST_PWM_WORDS FR_TEST_I2C_WORDS FR_TEST_MATH_WORDS FR_TEST_PAD_WORDS \
          FR_TEST_TEXT_WORDS FR_TEST_EVENT_REGISTER_WORDS                      \
              FR_TEST_EVENT_TEST_WORDS                                          \
              FR_TEST_SOURCE_WORDS "\nok\n"
#define FR_TEST_WORDS_WITH_LED                                                \
  "boot ms one gpio.write $led_builtin save restore dangerous.wipe gpio.mode "  \
  "gpio.read adc.read adc.above millis" FR_TEST_UART_WORDS FR_TEST_RANDOM_WORDS  \
      FR_TEST_PWM_WORDS FR_TEST_I2C_WORDS FR_TEST_MATH_WORDS FR_TEST_PAD_WORDS \
          FR_TEST_TEXT_WORDS FR_TEST_EVENT_REGISTER_WORDS                      \
              FR_TEST_EVENT_TEST_WORDS                                          \
              FR_TEST_SOURCE_WORDS " led\nok\n"
#define FR_TEST_WORDS_WITH_LED_AND_MYBLINK                                    \
  "boot ms one gpio.write $led_builtin save restore dangerous.wipe gpio.mode "  \
  "gpio.read adc.read adc.above millis" FR_TEST_UART_WORDS FR_TEST_RANDOM_WORDS  \
      FR_TEST_PWM_WORDS FR_TEST_I2C_WORDS FR_TEST_MATH_WORDS FR_TEST_PAD_WORDS \
          FR_TEST_TEXT_WORDS FR_TEST_EVENT_REGISTER_WORDS                      \
              FR_TEST_EVENT_TEST_WORDS                                          \
              FR_TEST_SOURCE_WORDS " led myblink\nok\n"
#define FR_TEST_BASE_SLOT_COUNT                                               \
  (13 + FR_TEST_UART_SLOT_COUNT + FR_TEST_RANDOM_SLOT_COUNT +                \
   FR_TEST_PWM_SLOT_COUNT + FR_TEST_I2C_SLOT_COUNT +                          \
   FR_TEST_MATH_SLOT_COUNT + FR_TEST_PAD_SLOT_COUNT +                         \
   FR_TEST_TEXT_SLOT_COUNT + FR_TEST_EVENT_REGISTER_SLOT_COUNT +              \
   FR_TEST_EVENT_TEST_SLOT_COUNT)
#else
#define FR_TEST_WORDS                                                        \
  "boot ms one gpio.write $led_builtin gpio.mode gpio.read adc.read "        \
  "adc.above millis" FR_TEST_UART_WORDS FR_TEST_RANDOM_WORDS                 \
      FR_TEST_PWM_WORDS FR_TEST_I2C_WORDS FR_TEST_MATH_WORDS FR_TEST_PAD_WORDS \
          FR_TEST_TEXT_WORDS FR_TEST_EVENT_REGISTER_WORDS                      \
              FR_TEST_EVENT_TEST_WORDS                                          \
              FR_TEST_SOURCE_WORDS "\nok\n"
#define FR_TEST_WORDS_WITH_LED                                                \
  "boot ms one gpio.write $led_builtin gpio.mode gpio.read adc.read "        \
  "adc.above millis" FR_TEST_UART_WORDS FR_TEST_RANDOM_WORDS                 \
      FR_TEST_PWM_WORDS FR_TEST_I2C_WORDS FR_TEST_MATH_WORDS FR_TEST_PAD_WORDS \
          FR_TEST_TEXT_WORDS FR_TEST_EVENT_REGISTER_WORDS                      \
              FR_TEST_EVENT_TEST_WORDS                                          \
              FR_TEST_SOURCE_WORDS " led\nok\n"
#define FR_TEST_WORDS_WITH_LED_AND_MYBLINK                                    \
  "boot ms one gpio.write $led_builtin gpio.mode gpio.read adc.read "        \
  "adc.above millis" FR_TEST_UART_WORDS FR_TEST_RANDOM_WORDS                 \
      FR_TEST_PWM_WORDS FR_TEST_I2C_WORDS FR_TEST_MATH_WORDS FR_TEST_PAD_WORDS \
          FR_TEST_TEXT_WORDS FR_TEST_EVENT_REGISTER_WORDS                      \
              FR_TEST_EVENT_TEST_WORDS                                          \
              FR_TEST_SOURCE_WORDS " led myblink\nok\n"
#define FR_TEST_BASE_SLOT_COUNT                                               \
  (10 + FR_TEST_UART_SLOT_COUNT + FR_TEST_RANDOM_SLOT_COUNT +                \
   FR_TEST_PWM_SLOT_COUNT + FR_TEST_I2C_SLOT_COUNT +                          \
   FR_TEST_MATH_SLOT_COUNT + FR_TEST_PAD_SLOT_COUNT +                         \
   FR_TEST_TEXT_SLOT_COUNT + FR_TEST_EVENT_REGISTER_SLOT_COUNT +              \
   FR_TEST_EVENT_TEST_SLOT_COUNT)
#endif

/* Boot compile binds base/core.frothy words at the first board-local slots, so
   user words in a fully installed base image start above them, and `words`
   lists them between the base and overlay names. */
#if FR_FEATURE_SOURCE_BASE
#define FR_TEST_SOURCE_BASE_WORD_COUNT 12
#define FR_TEST_SOURCE_WORDS                                                 \
  " gpio.high gpio.low gpio.toggle led.on led.off led.toggle blink "         \
  "led.blink wrap random.chance? random.percent? sign"
#else
#define FR_TEST_SOURCE_BASE_WORD_COUNT 0
#define FR_TEST_SOURCE_WORDS ""
#endif
#define FR_TEST_FIRST_USER_SLOT                                               \
  ((fr_slot_id_t)(FR_SLOT_BOARD_LOCAL_BASE + FR_TEST_SOURCE_BASE_WORD_COUNT))
#define FR_TEST_SYNTHETIC_HANDLE_KIND FR_HANDLE_KIND_PWM
#define FR_TEST_SYNTHETIC_HANDLE_NAME "pwm"

#define FR_TEST_INT_BYTE0(value)                                             \
  ((uint8_t)((uint32_t)(int32_t)(value) & 0xffu))
#define FR_TEST_INT_BYTE1(value)                                             \
  ((uint8_t)(((uint32_t)(int32_t)(value) >> 8) & 0xffu))
#define FR_TEST_INT_BYTE2(value)                                             \
  ((uint8_t)(((uint32_t)(int32_t)(value) >> 16) & 0xffu))
#define FR_TEST_INT_BYTE3(value)                                             \
  ((uint8_t)(((uint32_t)(int32_t)(value) >> 24) & 0xffu))
#if FR_WORD_SIZE == 16
#define FR_TEST_INT_BYTES(value)                                             \
  FR_TEST_INT_BYTE0(value), FR_TEST_INT_BYTE1(value)
#else
#define FR_TEST_INT_BYTES(value)                                             \
  FR_TEST_INT_BYTE0(value), FR_TEST_INT_BYTE1(value),                        \
      FR_TEST_INT_BYTE2(value), FR_TEST_INT_BYTE3(value)
#endif
#define FR_TEST_PUSH_INT(value) FR_OP_PUSH_INT, FR_TEST_INT_BYTES(value)
#if FR_WORD_SIZE == 32
#define FR_TEST_PERSIST_INT_SOURCE "100000"
#define FR_TEST_PERSIST_INT_VALUE 100000
#define FR_TEST_PERSIST_RECORD_INIT "point is Point: 100000, \"ready\""
#define FR_TEST_PERSIST_RECORD_SET "set point->x to 100001"
#define FR_TEST_PERSIST_RECORD_VALUE 100001
#else
#define FR_TEST_PERSIST_INT_SOURCE "7"
#define FR_TEST_PERSIST_INT_VALUE 7
#define FR_TEST_PERSIST_RECORD_INIT "point is Point: 10, \"ready\""
#define FR_TEST_PERSIST_RECORD_SET "set point->x to 11"
#define FR_TEST_PERSIST_RECORD_VALUE 11
#endif

#define CHECK(name, expr)                                                      \
  do {                                                                         \
    if (!(expr)) {                                                             \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, name);                    \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static void write_u16_little_endian(uint8_t *bytes, uint16_t word) {
  bytes[0] = (uint8_t)(word & 0xffu);
  bytes[1] = (uint8_t)(word >> 8);
}

static void write_u32_little_endian(uint8_t *bytes, uint32_t word) {
  bytes[0] = (uint8_t)(word & 0xffu);
  bytes[1] = (uint8_t)((word >> 8) & 0xffu);
  bytes[2] = (uint8_t)((word >> 16) & 0xffu);
  bytes[3] = (uint8_t)((word >> 24) & 0xffu);
}

static void write_overlay_crc(uint8_t *bytes, uint16_t length) {
  write_u32_little_endian(&bytes[length - 4u],
                          fr_crc32(bytes, (uint16_t)(length - 4u)));
}

static void write_instruction_header(uint8_t *bytes, uint8_t header_size) {
  bytes[0] = FR_INSTRUCTION_FORMAT_VERSION;
  bytes[1] = header_size;
}

static void write_instruction_header_arity(uint8_t *bytes, uint8_t arity) {
  bytes[0] = FR_INSTRUCTION_FORMAT_VERSION;
  bytes[1] = FR_INSTRUCTION_ARITY_HEADER_SIZE;
  bytes[2] = arity;
}

static void write_slot_operand(uint8_t *bytes, fr_slot_id_t slot_id) {
  write_u16_little_endian(bytes, slot_id);
}

static void write_jump_operand(uint8_t *bytes, fr_code_offset_t target) {
  write_u16_little_endian(bytes, target);
}

static void write_call_slot_arg_operands(uint8_t *bytes, fr_slot_id_t slot_id,
                                         uint8_t arg_count) {
  write_slot_operand(bytes, slot_id);
  bytes[2] = arg_count;
}

#if FR_FEATURE_CELLS
static void write_cell_operands(uint8_t *bytes, fr_slot_id_t slot_id,
                                uint16_t index) {
  write_slot_operand(bytes, slot_id);
  write_u16_little_endian(&bytes[2], index);
}
#endif

#if FR_FEATURE_PERSISTENCE
static uint32_t read_u32_little_endian(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}
#endif

static void test_base_def_contract(void) {
  bool seen_slots[FR_PROFILE_MAX_SLOTS] = {0};
  uint16_t expected_layer_count = FR_FEATURE_PERSISTENCE ? 4 : 3;
  uint16_t expected_native_count =
      (FR_FEATURE_PERSISTENCE ? 10 : 7) + (FR_FEATURE_UART ? 6 : 0) +
      (FR_FEATURE_RANDOM ? 3 : 0) + (FR_FEATURE_PWM ? 3 : 0) +
      (FR_FEATURE_I2C ? 4 : 0) + (FR_FEATURE_MATH ? 6 : 0) +
      FR_TEST_PAD_SLOT_COUNT + FR_TEST_TEXT_SLOT_COUNT +
      FR_TEST_EVENT_REGISTER_SLOT_COUNT + FR_TEST_EVENT_TEST_SLOT_COUNT;
  uint16_t global_index = 0;
  uint16_t native_count = 0;
  fr_slot_id_t highest_slot_id = 0;
  bool has_base_slot = false;

  CHECK("base def layer count excludes project layer",
        fr_base_def_layer_count() == expected_layer_count);
  CHECK("base def rejects one-past layer",
        fr_base_def_layer_at(fr_base_def_layer_count(),
                             &(fr_base_def_layer_t){0}) == FR_ERR_NOT_FOUND);
#if FR_FEATURE_PAD
  CHECK("pad slot ids follow math block",
        FR_SLOT_PAD_RESET == FR_SLOT_AFTER_MATH);
#endif
#if FR_FEATURE_TEXT
  CHECK("text slot ids follow pad block",
        FR_SLOT_TEXT_LENGTH == FR_SLOT_AFTER_PAD);
  CHECK("event register slot follows text ids",
        FR_SLOT_EVENT_REGISTER == FR_SLOT_TEXT_FROM_INT + 1);
#if FR_INCLUDE_TEST_NATIVES && FR_FEATURE_TEXT
  CHECK("fire-event slot follows event register slot",
        FR_SLOT_FIRE_EVENT == FR_SLOT_EVENT_REGISTER + 1);
  CHECK("board local slot ids follow fire-event slot",
        FR_SLOT_BOARD_LOCAL_BASE == FR_SLOT_FIRE_EVENT + 1);
#else
  CHECK("board local slot ids follow event register slot",
        FR_SLOT_BOARD_LOCAL_BASE == FR_SLOT_EVENT_REGISTER + 1);
#endif
#elif FR_FEATURE_PAD
  CHECK("event register slot follows pad ids",
        FR_SLOT_EVENT_REGISTER == FR_TEST_PAD_LAST_SLOT + 1);
  CHECK("board local slot ids follow event register slot",
        FR_SLOT_BOARD_LOCAL_BASE == FR_SLOT_EVENT_REGISTER + 1);
#else
  CHECK("event register slot follows math block",
        FR_SLOT_EVENT_REGISTER == FR_SLOT_AFTER_MATH);
  CHECK("board local slot ids follow event register slot",
        FR_SLOT_BOARD_LOCAL_BASE == FR_SLOT_EVENT_REGISTER + 1);
#endif

  for (uint16_t layer_index = 0; layer_index < fr_base_def_layer_count();
       layer_index++) {
    fr_base_def_layer_t def_layer = {0};

    CHECK("base def layer reads",
          fr_base_def_layer_at(layer_index, &def_layer) == FR_OK);
    CHECK("base def layer owns rows",
          def_layer.count == 0 || def_layer.defs != NULL);

    for (uint16_t row = 0; row < def_layer.count; row++) {
      const fr_base_def_t *def = &def_layer.defs[row];
      const fr_base_def_t *indexed_def = NULL;
      fr_base_layer_t indexed_layer = FR_BASE_LAYER_CORE;
      fr_base_layer_t resolved_layer = FR_BASE_LAYER_CORE;

      CHECK("base def index follows layer table",
            fr_base_def_at(global_index, &indexed_def, &indexed_layer) ==
                    FR_OK &&
                indexed_def == def && indexed_layer == def_layer.layer);
      CHECK("base def slot fits profile", def->slot_id < FR_PROFILE_MAX_SLOTS);
      CHECK("base def slot ids are unique", !seen_slots[def->slot_id]);
      seen_slots[def->slot_id] = true;
      if (!has_base_slot || def->slot_id > highest_slot_id) {
        highest_slot_id = def->slot_id;
        has_base_slot = true;
      }
      CHECK("base def slot resolves owning layer",
            fr_base_slot_layer(def->slot_id, &resolved_layer) == FR_OK &&
                resolved_layer == def_layer.layer);
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
      CHECK("base def owns nonempty name",
            def->name != NULL && def->name[0] != '\0');
      for (uint16_t previous = 0; previous < global_index; previous++) {
        const fr_base_def_t *previous_def = NULL;
        fr_base_layer_t previous_layer = FR_BASE_LAYER_CORE;

        CHECK("base def previous row reads",
              fr_base_def_at(previous, &previous_def, &previous_layer) ==
                  FR_OK);
        if (previous_def != NULL && previous_def->name != NULL &&
            def->name != NULL) {
          CHECK("base def names are unique",
                strcmp(previous_def->name, def->name) != 0);
        }
      }
#endif

      if (def->kind == FR_BASE_DEF_LITERAL) {
        CHECK("base literal stores valid tagged word",
              fr_tagged_is_valid(def->literal_tagged));
      } else if (def->kind == FR_BASE_DEF_NATIVE) {
        native_count += 1;
        CHECK("base native owns callback", def->native_fn != NULL);
        CHECK("base native arity fits stack",
              def->native_arity <= FR_PROFILE_MAX_STACK_DEPTH);
#if FR_FEATURE_NATIVE_SIGNATURES
        if (def->native_signature != NULL) {
          CHECK("base native signature arity matches",
                def->native_signature->arg_count == def->native_arity);
          CHECK("base native signature params present",
                def->native_signature->arg_count == 0 ||
                    def->native_signature->params != NULL);
        }
#endif
      } else {
        CHECK("base def kind is known", false);
      }

      global_index += 1;
    }
  }

  CHECK("base def count matches layer rows", global_index == fr_base_def_count());
  CHECK("base native count fits profile",
        native_count <= FR_PROFILE_NATIVE_TABLE_SIZE);
  CHECK("base native count matches enabled rows",
        native_count == expected_native_count);
  CHECK("base highest slot fits profile",
        has_base_slot && highest_slot_id < FR_PROFILE_MAX_SLOTS);
  CHECK("first project slot follows highest base",
        fr_slot_first_project_id() == (fr_slot_id_t)(highest_slot_id + 1));
#if FR_FEATURE_COMPILER
  CHECK("compiler profiles keep named overlay room",
        FR_PROFILE_MAX_OVERLAY_NAMES > 0);
#endif
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  CHECK("base leaves named overlay room",
        (uint32_t)highest_slot_id + 1u + FR_PROFILE_MAX_OVERLAY_NAMES <=
            FR_PROFILE_MAX_SLOTS);
#endif
}

static void test_profile_hash_word_size(void) {
  uint16_t other = (FR_WORD_SIZE == 16) ? 32u : 16u;
  CHECK("profile hash differs when word size differs",
        fr_profile_hash() != fr_profile_debug_hash_for_word_size(other));
#if FR_FEATURE_SOURCE_BASE
  /* A one-byte edit to base/core.frothy must produce a different hash; that
     proves a rebuild with changed source rejects an old persisted overlay. */
  {
    char perturbed[FR_PROFILE_REPL_LINE_BYTES];
    uint16_t length = fr_source_base_bytes_len;

    if (length > sizeof(perturbed)) {
      length = (uint16_t)sizeof(perturbed);
    }
    memcpy(perturbed, fr_source_base_bytes, length);
    perturbed[0] = (char)(perturbed[0] ^ 0x01);
    CHECK("profile hash differs when source bytes differ",
          fr_profile_hash() !=
              fr_profile_debug_hash_for_source(FR_WORD_SIZE, perturbed, length));
    CHECK("profile hash matches when source bytes match",
          fr_profile_hash() == fr_profile_debug_hash_for_source(
                                   FR_WORD_SIZE, fr_source_base_bytes,
                                   fr_source_base_bytes_len));
  }
#endif
}

#if FR_FEATURE_PERSISTENCE
/* A saved persist header carrying the opposite word size's profile hash
 * must be rejected outright, not silently restored. The gate is the hash
 * compare in fr_persist_header_parse. */
static void test_persist_cross_width_header_rejection(void) {
  fr_runtime_t runtime;
  uint8_t header[FR_PERSIST_HEADER_BYTES];
  const uint16_t other = (FR_WORD_SIZE == 16) ? 32u : 16u;

  CHECK("save populates storage for cross-width test",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK);
  CHECK("read saved header for tampering",
        fr_platform_storage_read(0, 0, header,
                                 (uint16_t)sizeof(header)) == FR_OK);

  fr_write_u32_le(&header[FR_PERSIST_PROFILE_HASH_OFFSET],
                  fr_profile_debug_hash_for_word_size(other));
  memset(&header[24], 0, 4);
  fr_write_u32_le(&header[24],
                  fr_crc32(header, FR_PERSIST_HEADER_BYTES));

  CHECK("write tampered headers back to both storage slots",
        fr_platform_storage_write(0, 0, header,
                                  (uint16_t)sizeof(header)) == FR_OK &&
            fr_platform_storage_write(1, 0, header,
                                      (uint16_t)sizeof(header)) == FR_OK);
  CHECK("restore rejects header with opposite-width profile hash",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_ERR_CORRUPT);
}

static void test_persist_code_id_round_trip(void) {
  fr_runtime_t runtime;
  fr_instruction_stream_t view;
  fr_code_object_id_t inner_id = 0;
  fr_code_object_id_t outer_id = 0;
  fr_code_object_id_t restored_outer_id = 0;
  fr_code_object_id_t restored_inner_id = 0;
  fr_tagged_t outer_tagged = 0;
  fr_tagged_t slot_value = 0;
  fr_tagged_t result = 0;
  fr_int_t decoded = 0;
  uint8_t inner_bytes[] = {0x00, FR_INSTRUCTION_MIN_HEADER_SIZE,
                           FR_TEST_PUSH_INT(42), FR_OP_RETURN};
  uint8_t outer_bytes[] = {0x00, FR_INSTRUCTION_MIN_HEADER_SIZE,
                           FR_OP_PUSH_CODE_ID, 0x00, 0x00, FR_OP_RETURN};
  const size_t outer_code_id_operand = FR_INSTRUCTION_MIN_HEADER_SIZE + 1u;

  fr_platform_storage_debug_reset();
  CHECK("code-id round-trip base image",
        fr_base_image_install(&runtime) == FR_OK);
  CHECK("code-id round-trip install inner",
        fr_instruction_stream_init(&view, inner_bytes,
                                   (uint16_t)sizeof(inner_bytes)) == FR_OK &&
            fr_code_install(&runtime, &view, NULL, 0, &inner_id) == FR_OK);
  write_u16_little_endian(&outer_bytes[outer_code_id_operand], inner_id);
  CHECK("code-id round-trip install outer",
        fr_instruction_stream_init(&view, outer_bytes,
                                   (uint16_t)sizeof(outer_bytes)) == FR_OK &&
            fr_code_install(&runtime, &view, NULL, 0, &outer_id) == FR_OK);
  CHECK("code-id round-trip stage slot",
        fr_tagged_encode_code_object_id(outer_id, &outer_tagged) == FR_OK &&
            fr_slot_write(&runtime, FR_TEST_FIRST_USER_SLOT, outer_tagged) ==
                FR_OK);
  CHECK("code-id round-trip save", fr_persist_save(&runtime) == FR_OK);
  CHECK("code-id round-trip restore",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK);
  CHECK("code-id round-trip slot decode",
        fr_slot_read(&runtime, FR_TEST_FIRST_USER_SLOT, &slot_value) == FR_OK &&
            fr_tagged_decode_code_object_id(slot_value, &restored_outer_id) ==
                FR_OK);
  CHECK("code-id round-trip outer pushes restored inner id",
        fr_vm_run_code_object(&runtime, restored_outer_id, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK);
  restored_inner_id = (fr_code_object_id_t)decoded;
  CHECK("code-id round-trip restored inner returns 42",
        fr_vm_run_code_object(&runtime, restored_inner_id, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 42);
}

#if FR_FEATURE_SOURCE_BASE
/* Same restore gate, but the saved hash reflects a one-byte-different
 * base/core.frothy. A rebuild with edited source rejects the old overlay. */
static void test_persist_source_change_header_rejection(void) {
  fr_runtime_t runtime;
  uint8_t header[FR_PERSIST_HEADER_BYTES];
  char perturbed[FR_PROFILE_REPL_LINE_BYTES];
  uint16_t length = fr_source_base_bytes_len;

  if (length > sizeof(perturbed)) {
    length = (uint16_t)sizeof(perturbed);
  }
  memcpy(perturbed, fr_source_base_bytes, length);
  perturbed[0] = (char)(perturbed[0] ^ 0x01);

  CHECK("save populates storage for source-change test",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK);
  CHECK("read saved header for source-change tampering",
        fr_platform_storage_read(0, 0, header,
                                 (uint16_t)sizeof(header)) == FR_OK);

  fr_write_u32_le(&header[FR_PERSIST_PROFILE_HASH_OFFSET],
                  fr_profile_debug_hash_for_source(FR_WORD_SIZE, perturbed,
                                                   length));
  memset(&header[24], 0, 4);
  fr_write_u32_le(&header[24], fr_crc32(header, FR_PERSIST_HEADER_BYTES));

  CHECK("write source-changed headers back to both storage slots",
        fr_platform_storage_write(0, 0, header,
                                  (uint16_t)sizeof(header)) == FR_OK &&
            fr_platform_storage_write(1, 0, header,
                                      (uint16_t)sizeof(header)) == FR_OK);
  CHECK("restore rejects overlay saved under different source bytes",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_ERR_CORRUPT);
}
#endif
#endif

#if FR_FEATURE_OVERLAY_APPLY_COMMAND
static char hex_digit(uint8_t value) {
  return value < 10 ? (char)('0' + value) : (char)('a' + value - 10);
}

static bool write_apply_hex_line(const uint8_t *bytes, uint16_t length,
                                 char *out, uint16_t out_cap) {
  const char prefix[] = "apply ";
  uint16_t used = (uint16_t)(sizeof(prefix) - 1);

  if (bytes == NULL || out == NULL ||
      (uint32_t)used + ((uint32_t)length * 2u) + 1u > out_cap) {
    return false;
  }

  memcpy(out, prefix, used);
  for (uint16_t i = 0; i < length; i++) {
    out[used++] = hex_digit((uint8_t)(bytes[i] >> 4));
    out[used++] = hex_digit((uint8_t)(bytes[i] & 0x0fu));
  }
  out[used] = '\0';
  return true;
}

static bool write_run_hex_line(const uint8_t *bytes, uint16_t length,
                               char *out, uint16_t out_cap) {
  const char prefix[] = "run ";
  uint16_t used = (uint16_t)(sizeof(prefix) - 1);

  if (bytes == NULL || out == NULL ||
      (uint32_t)used + ((uint32_t)length * 2u) + 1u > out_cap) {
    return false;
  }

  memcpy(out, prefix, used);
  for (uint16_t i = 0; i < length; i++) {
    out[used++] = hex_digit((uint8_t)(bytes[i] >> 4));
    out[used++] = hex_digit((uint8_t)(bytes[i] & 0x0fu));
  }
  out[used] = '\0';
  return true;
}
#endif

static void test_tagged_bands(void) {
  CHECK("int band start", fr_tagged_kind(0) == FR_TAGGED_INT);
  CHECK("int band end", fr_tagged_kind(FR_TAGGED_INT_END) == FR_TAGGED_INT);
  CHECK("special band start",
        fr_tagged_kind(FR_TAGGED_SPECIAL_BASE) == FR_TAGGED_SPECIAL);
  CHECK("special band end",
        fr_tagged_kind(FR_TAGGED_SPECIAL_END) == FR_TAGGED_SPECIAL);
  CHECK("slot band start",
        fr_tagged_kind(FR_TAGGED_SLOT_BASE) == FR_TAGGED_SLOT_ID);
  CHECK("slot band end",
        fr_tagged_kind(FR_TAGGED_SLOT_END) == FR_TAGGED_SLOT_ID);
  CHECK("code band start",
        fr_tagged_kind(FR_TAGGED_CODE_BASE) == FR_TAGGED_CODE_OBJECT_ID);
  CHECK("code band end",
        fr_tagged_kind(FR_TAGGED_CODE_END) == FR_TAGGED_CODE_OBJECT_ID);
  CHECK("native band start",
        fr_tagged_kind(FR_TAGGED_NATIVE_BASE) == FR_TAGGED_NATIVE_ID);
  CHECK("native band end",
        fr_tagged_kind(FR_TAGGED_NATIVE_END) == FR_TAGGED_NATIVE_ID);
  CHECK("object band start",
        fr_tagged_kind(FR_TAGGED_OBJECT_BASE) == FR_TAGGED_OBJECT_ID);
  CHECK("object band end",
        fr_tagged_kind(FR_TAGGED_OBJECT_END) == FR_TAGGED_OBJECT_ID);
#if FR_FEATURE_HANDLES
  CHECK("handle band start",
        fr_tagged_kind(FR_TAGGED_HANDLE_BASE) == FR_TAGGED_HANDLE);
  CHECK("handle band end",
        fr_tagged_kind(FR_TAGGED_HANDLE_END) == FR_TAGGED_HANDLE);
  CHECK("reserved band start",
        fr_tagged_kind(FR_TAGGED_RESERVED_BASE) == FR_TAGGED_RESERVED);
#else
  CHECK("reserved band start",
        fr_tagged_kind(FR_TAGGED_RESERVED_BASE) == FR_TAGGED_RESERVED);
#endif
  CHECK("reserved band end",
        fr_tagged_kind(FR_TAGGED_RESERVED_END) == FR_TAGGED_RESERVED);

#if FR_FEATURE_HANDLES
  CHECK("handle is valid", fr_tagged_is_valid(FR_TAGGED_HANDLE_BASE));
  CHECK("reserved is invalid", !fr_tagged_is_valid(FR_TAGGED_RESERVED_BASE));
#else
  CHECK("reserved is invalid", !fr_tagged_is_valid(FR_TAGGED_RESERVED_BASE));
#endif
  CHECK("object is valid", fr_tagged_is_valid(FR_TAGGED_OBJECT_BASE));
}

static void test_specials(void) {
  fr_tagged_t tagged = 0;
  bool decoded_bool = false;

  CHECK("nil", fr_tagged_is_nil(fr_tagged_nil()));
  CHECK("false", fr_tagged_is_false(fr_tagged_false()));
  CHECK("true", fr_tagged_is_true(fr_tagged_true()));
  CHECK("false is bool", fr_tagged_is_bool(fr_tagged_false()));
  CHECK("true is bool", fr_tagged_is_bool(fr_tagged_true()));
  CHECK("nil is not bool", !fr_tagged_is_bool(fr_tagged_nil()));

  CHECK("encode false", fr_tagged_encode_bool(false, &tagged) == FR_OK &&
                            tagged == fr_tagged_false());
  CHECK("encode true", fr_tagged_encode_bool(true, &tagged) == FR_OK &&
                           tagged == fr_tagged_true());
  CHECK("decode false", fr_tagged_decode_bool(fr_tagged_false(),
                                              &decoded_bool) == FR_OK &&
                            decoded_bool == false);
  CHECK("decode true", fr_tagged_decode_bool(fr_tagged_true(),
                                             &decoded_bool) == FR_OK &&
                           decoded_bool == true);
  CHECK("decode nil rejects",
        fr_tagged_decode_bool(fr_tagged_nil(), &decoded_bool) == FR_ERR_TYPE);
  CHECK("decode int rejects",
        fr_tagged_encode_int(1, &tagged) == FR_OK &&
            fr_tagged_decode_bool(tagged, &decoded_bool) == FR_ERR_TYPE);

  CHECK("nil is falsy", fr_tagged_is_falsy(fr_tagged_nil()));
  CHECK("false is falsy", fr_tagged_is_falsy(fr_tagged_false()));
  CHECK("true is truthy", !fr_tagged_is_falsy(fr_tagged_true()));
  CHECK("zero is falsy", fr_tagged_encode_int(0, &tagged) == FR_OK &&
                             fr_tagged_is_falsy(tagged));
  CHECK("one is truthy", fr_tagged_encode_int(1, &tagged) == FR_OK &&
                             !fr_tagged_is_falsy(tagged));
  CHECK("minus one is truthy", fr_tagged_encode_int(-1, &tagged) == FR_OK &&
                                   !fr_tagged_is_falsy(tagged));
  CHECK("slot id is truthy", fr_tagged_encode_slot_id(0, &tagged) == FR_OK &&
                                 !fr_tagged_is_falsy(tagged));
  CHECK("code-tagged is truthy",
        fr_tagged_encode_code_object_id(0, &tagged) == FR_OK &&
            !fr_tagged_is_falsy(tagged));
  CHECK("native-tagged is truthy",
        fr_tagged_encode_native_id(0, &tagged) == FR_OK &&
            !fr_tagged_is_falsy(tagged));
}

static void check_int_round_trip(int32_t raw_int) {
  fr_tagged_t encoded = 0xffffu;
  fr_int_t decoded = 0;

  CHECK("int fits", fr_tagged_can_encode_int(raw_int));
  CHECK("int encode", fr_tagged_encode_int(raw_int, &encoded) == FR_OK);
  CHECK("int decode", fr_tagged_decode_int(encoded, &decoded) == FR_OK);
  CHECK("int round trip", decoded == raw_int);
}

static void test_ints(void) {
  fr_tagged_t encoded = 0;
  fr_int_t decoded = 0;

  check_int_round_trip(FR_TAGGED_INT_MIN);
  check_int_round_trip(-1);
  check_int_round_trip(0);
  check_int_round_trip(1);
  check_int_round_trip(FR_TAGGED_INT_MAX);

  CHECK("low int rejects", !fr_tagged_can_encode_int(FR_TAGGED_INT_MIN - 1LL));
  CHECK("high int rejects", !fr_tagged_can_encode_int(FR_TAGGED_INT_MAX + 1LL));
  CHECK("low int returns range",
        fr_tagged_encode_int(FR_TAGGED_INT_MIN - 1LL, &encoded) ==
            FR_ERR_RANGE);
  CHECK("high int returns range",
        fr_tagged_encode_int(FR_TAGGED_INT_MAX + 1LL, &encoded) ==
            FR_ERR_RANGE);
  CHECK("int decode rejects null out",
        fr_tagged_decode_int(encoded, NULL) == FR_ERR_INVALID);
  CHECK("non-int returns type",
        fr_tagged_decode_int(fr_tagged_nil(), &decoded) == FR_ERR_TYPE);
}

static void test_refs(void) {
  fr_tagged_t tagged = 0;
  fr_slot_id_t slot_id = 0;
  fr_code_object_id_t code_object_id = 0;
  fr_native_id_t native_id = 0;
  fr_object_id_t object_id = 0;
#if FR_FEATURE_HANDLES
  fr_handle_ref_t handle_ref = {0};
#endif

  CHECK("first slot encodes", fr_tagged_encode_slot_id(0, &tagged) == FR_OK);
  CHECK("first slot decodes",
        fr_tagged_decode_slot_id(tagged, &slot_id) == FR_OK);
  CHECK("first slot round trip", slot_id == 0);
  CHECK("last slot encodes",
        fr_tagged_encode_slot_id((fr_slot_id_t)FR_TAGGED_SLOT_MAX_ID,
                                 &tagged) == FR_OK);
  CHECK("last slot decodes",
        fr_tagged_decode_slot_id(tagged, &slot_id) == FR_OK);
  CHECK("last slot round trip", slot_id == FR_TAGGED_SLOT_MAX_ID);
#if FR_WORD_SIZE == 16
  CHECK("slot rejects one past",
        fr_tagged_encode_slot_id((fr_slot_id_t)(FR_TAGGED_SLOT_MAX_ID + 1u),
                                 &tagged) == FR_ERR_RANGE);
#else
  CHECK("wide slot band rejects compact-id overflow",
        fr_tagged_decode_slot_id(
            (fr_tagged_t)(FR_TAGGED_SLOT_BASE + FR_TAGGED_SLOT_MAX_ID + 1u),
            &slot_id) == FR_ERR_RANGE);
  CHECK("wide slot band overflow is invalid",
        !fr_tagged_is_valid((fr_tagged_t)(FR_TAGGED_SLOT_BASE +
                                          FR_TAGGED_SLOT_MAX_ID + 1u)));
#endif
  CHECK("non-slot returns type",
        fr_tagged_decode_slot_id(fr_tagged_nil(), &slot_id) == FR_ERR_TYPE);

  CHECK("first code encodes",
        fr_tagged_encode_code_object_id(0, &tagged) == FR_OK);
  CHECK("first code decodes",
        fr_tagged_decode_code_object_id(tagged, &code_object_id) == FR_OK);
  CHECK("first code round trip", code_object_id == 0);
  CHECK("last code encodes",
        fr_tagged_encode_code_object_id(
            (fr_code_object_id_t)FR_TAGGED_CODE_MAX_ID, &tagged) == FR_OK);
  CHECK("last code decodes",
        fr_tagged_decode_code_object_id(tagged, &code_object_id) == FR_OK);
  CHECK("last code round trip", code_object_id == FR_TAGGED_CODE_MAX_ID);
#if FR_WORD_SIZE == 16
  CHECK("code rejects one past",
        fr_tagged_encode_code_object_id(
            (fr_code_object_id_t)(FR_TAGGED_CODE_MAX_ID + 1u), &tagged) ==
            FR_ERR_RANGE);
#else
  CHECK("wide code band rejects compact-id overflow",
        fr_tagged_decode_code_object_id(
            (fr_tagged_t)(FR_TAGGED_CODE_BASE + FR_TAGGED_CODE_MAX_ID + 1u),
            &code_object_id) == FR_ERR_RANGE);
  CHECK("wide code band overflow is invalid",
        !fr_tagged_is_valid((fr_tagged_t)(FR_TAGGED_CODE_BASE +
                                          FR_TAGGED_CODE_MAX_ID + 1u)));
#endif
  CHECK("non-code returns type",
        fr_tagged_decode_code_object_id(fr_tagged_nil(), &code_object_id) ==
            FR_ERR_TYPE);

  CHECK("first native encodes",
        fr_tagged_encode_native_id(0, &tagged) == FR_OK);
  CHECK("first native decodes",
        fr_tagged_decode_native_id(tagged, &native_id) == FR_OK);
  CHECK("first native round trip", native_id == 0);
  CHECK("last native encodes",
        fr_tagged_encode_native_id((fr_native_id_t)FR_TAGGED_NATIVE_MAX_ID,
                                   &tagged) == FR_OK);
  CHECK("last native decodes",
        fr_tagged_decode_native_id(tagged, &native_id) == FR_OK);
  CHECK("last native round trip", native_id == FR_TAGGED_NATIVE_MAX_ID);
#if FR_WORD_SIZE == 16
  CHECK("native rejects one past",
        fr_tagged_encode_native_id(
            (fr_native_id_t)(FR_TAGGED_NATIVE_MAX_ID + 1u), &tagged) ==
            FR_ERR_RANGE);
#else
  CHECK("wide native band rejects compact-id overflow",
        fr_tagged_decode_native_id((fr_tagged_t)(FR_TAGGED_NATIVE_BASE +
                                                 FR_TAGGED_NATIVE_MAX_ID + 1u),
                                   &native_id) == FR_ERR_RANGE);
  CHECK("wide native band overflow is invalid",
        !fr_tagged_is_valid((fr_tagged_t)(FR_TAGGED_NATIVE_BASE +
                                          FR_TAGGED_NATIVE_MAX_ID + 1u)));
#endif
  CHECK("non-native returns type",
        fr_tagged_decode_native_id(fr_tagged_nil(), &native_id) == FR_ERR_TYPE);

  CHECK("first object encodes",
        fr_tagged_encode_object_id(0, &tagged) == FR_OK);
  CHECK("first object decodes",
        fr_tagged_decode_object_id(tagged, &object_id) == FR_OK);
  CHECK("first object round trip", object_id == 0);
  CHECK("last object encodes",
        fr_tagged_encode_object_id((fr_object_id_t)FR_TAGGED_OBJECT_MAX_ID,
                                   &tagged) == FR_OK);
  CHECK("last object decodes",
        fr_tagged_decode_object_id(tagged, &object_id) == FR_OK);
  CHECK("last object round trip", object_id == FR_TAGGED_OBJECT_MAX_ID);
#if FR_WORD_SIZE == 16
  CHECK("object rejects one past",
        fr_tagged_encode_object_id(
            (fr_object_id_t)(FR_TAGGED_OBJECT_MAX_ID + 1u), &tagged) ==
            FR_ERR_RANGE);
#else
  CHECK("wide object band rejects compact-id overflow",
        fr_tagged_decode_object_id((fr_tagged_t)(FR_TAGGED_OBJECT_BASE +
                                                 FR_TAGGED_OBJECT_MAX_ID + 1u),
                                   &object_id) == FR_ERR_RANGE);
  CHECK("wide object band overflow is invalid",
        !fr_tagged_is_valid((fr_tagged_t)(FR_TAGGED_OBJECT_BASE +
                                          FR_TAGGED_OBJECT_MAX_ID + 1u)));
#endif
  CHECK("non-object returns type",
        fr_tagged_decode_object_id(fr_tagged_nil(), &object_id) == FR_ERR_TYPE);
#if FR_FEATURE_HANDLES
  CHECK("first handle encodes",
        fr_tagged_encode_handle_ref(
            (fr_handle_ref_t){.id = 0, .generation = 0}, &tagged) == FR_OK);
  CHECK("first handle decodes",
        fr_tagged_decode_handle_ref(tagged, &handle_ref) == FR_OK);
  CHECK("first handle round trip",
        handle_ref.id == 0 && handle_ref.generation == 0);
  CHECK("last handle encodes",
        fr_tagged_encode_handle_ref(
            (fr_handle_ref_t){.id = 15, .generation = 15}, &tagged) == FR_OK);
  CHECK("last handle decodes",
        fr_tagged_decode_handle_ref(tagged, &handle_ref) == FR_OK);
  CHECK("last handle round trip",
        handle_ref.id == 15 && handle_ref.generation == 15);
  CHECK("handle rejects one past id",
        fr_tagged_encode_handle_ref(
            (fr_handle_ref_t){.id = 16, .generation = 0}, &tagged) ==
            FR_ERR_RANGE);
  CHECK("handle rejects one past generation",
        fr_tagged_encode_handle_ref(
            (fr_handle_ref_t){.id = 0, .generation = 16}, &tagged) ==
            FR_ERR_RANGE);
  CHECK("non-handle returns type",
        fr_tagged_decode_handle_ref(fr_tagged_nil(), &handle_ref) ==
            FR_ERR_TYPE);
#else
  CHECK("handle encode unsupported without feature",
        fr_tagged_encode_handle_ref(
            (fr_handle_ref_t){.id = 0, .generation = 0}, &tagged) ==
            FR_ERR_UNSUPPORTED);
#endif
}

static void test_addr(void) {
  CHECK("addr preserves pointer width", sizeof(fr_addr_t) == sizeof(uintptr_t));
}

static void test_instruction_stream(void) {
  uint8_t return_bytes[] = {0x00, 0x00, FR_OP_RETURN};
  uint8_t load_slot_last[] = {0x00, 0x00, FR_OP_LOAD_SLOT,
                              0x00, 0x00, FR_OP_RETURN};
  uint8_t call_slot_arg_one[] = {0x00, 0x00, FR_TEST_PUSH_INT(1),
                                 FR_OP_CALL_SLOT_ARG, 0x00, 0x00, 0x00,
                                 FR_OP_RETURN};
  uint8_t call_native_slot_zero[] = {0x00, 0x00, FR_OP_CALL_NATIVE_SLOT,
                                     0x00, 0x00, FR_OP_RETURN};
  uint8_t push_nil[] = {0x00, 0x00, FR_OP_PUSH_NIL, FR_OP_RETURN};
  uint8_t push_int_store_slot[] = {0x00, 0x00, FR_TEST_PUSH_INT(7),
                                   FR_OP_STORE_SLOT, 0x00, 0x00,
                                   FR_OP_RETURN};
#if FR_FEATURE_CELLS
  uint8_t load_cell_zero[] = {0x00, 0x00, FR_OP_LOAD_CELL, 0x00,
                              0x00, 0x00, 0x00, FR_OP_RETURN};
  uint8_t store_cell_zero[] = {0x00, 0x00, FR_OP_STORE_CELL, 0x00,
                               0x00, 0x00, 0x00, FR_OP_RETURN};
#endif
#if FR_FEATURE_RECORDS
  uint8_t load_field_x[] = {0x00, 0x00, FR_OP_LOAD_FIELD,  0x01,
                            'x',  FR_OP_RETURN};
  uint8_t store_field_x[] = {0x00, 0x00, FR_OP_STORE_FIELD, 0x01,
                             'x',  FR_OP_RETURN};
#endif
  uint8_t push_int_drop[] = {0x00, 0x00, FR_TEST_PUSH_INT(7), FR_OP_DROP,
                             FR_OP_RETURN};
  uint8_t load_arg_zero[] = {0x00, 0x00, 0x00, FR_OP_LOAD_ARG, 0x00,
                             FR_OP_RETURN};
  uint8_t push_int_minus_one[] = {0x00, 0x00, FR_TEST_PUSH_INT(-1),
                                  FR_OP_RETURN};
  uint8_t jump_push_two[] = {
      0x00, 0x00, FR_OP_JUMP, 0x00, 0x00, FR_TEST_PUSH_INT(1),
      FR_TEST_PUSH_INT(2), FR_OP_RETURN};
  uint8_t repeat_two[] = {
      0x00, 0x00, FR_TEST_PUSH_INT(2), FR_OP_REPEAT_BEGIN, 0x00, 0x00,
      FR_TEST_PUSH_INT(1), FR_OP_DROP, FR_OP_REPEAT_NEXT, 0x00, 0x00,
      FR_OP_PUSH_NIL, FR_OP_RETURN};
  fr_instruction_stream_t view;
  fr_instruction_header_t header;
  char text[128];
  char expected_repeat_begin[32];
  char expected_repeat_next[32];
  uint16_t text_len = 0;
  fr_code_offset_t next_ip = 0;
  fr_slot_id_t slot_id = 0;
  fr_int_t int_operand = 0;
  fr_code_offset_t jump_target = 0;
  uint8_t arg_index = 0;
  uint8_t arg_count = 0;
  const fr_code_offset_t call_slot_arg_ip =
      2u + FR_INSTRUCTION_PUSH_INT_SIZE;
  const fr_code_offset_t jump_first_push_ip = 5u;
  const fr_code_offset_t jump_second_push_ip =
      jump_first_push_ip + FR_INSTRUCTION_PUSH_INT_SIZE;
  const fr_code_offset_t repeat_begin_ip =
      2u + FR_INSTRUCTION_PUSH_INT_SIZE;
  const fr_code_offset_t repeat_body_ip = repeat_begin_ip + 3u;
  const fr_code_offset_t repeat_drop_ip =
      repeat_body_ip + FR_INSTRUCTION_PUSH_INT_SIZE;
  const fr_code_offset_t repeat_next_ip = repeat_drop_ip + 1u;
  const fr_code_offset_t repeat_done_ip = repeat_next_ip + 3u;
#if FR_FEATURE_CELLS
  uint16_t cell_index = 0;
#endif
#if FR_FEATURE_RECORDS
  const uint8_t *field_name = NULL;
  uint8_t field_length = 0;
#endif

  write_instruction_header(return_bytes, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(load_slot_last, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(call_slot_arg_one, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(call_native_slot_zero,
                           FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(push_nil, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(push_int_store_slot, FR_INSTRUCTION_MIN_HEADER_SIZE);
#if FR_FEATURE_CELLS
  write_instruction_header(load_cell_zero, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(store_cell_zero, FR_INSTRUCTION_MIN_HEADER_SIZE);
#endif
#if FR_FEATURE_RECORDS
  write_instruction_header(load_field_x, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(store_field_x, FR_INSTRUCTION_MIN_HEADER_SIZE);
#endif
  write_instruction_header(push_int_drop, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header_arity(load_arg_zero, 1);
  write_instruction_header(push_int_minus_one, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(jump_push_two, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(repeat_two, FR_INSTRUCTION_MIN_HEADER_SIZE);
  snprintf(expected_repeat_begin, sizeof(expected_repeat_begin),
           "REPEAT_BEGIN %u", repeat_done_ip);
  snprintf(expected_repeat_next, sizeof(expected_repeat_next), "REPEAT_NEXT %u",
           repeat_body_ip);

  write_slot_operand(&load_slot_last[3], FR_PROFILE_MAX_SLOTS - 1);
  write_call_slot_arg_operands(&call_slot_arg_one[call_slot_arg_ip + 1], 0, 1);
  write_slot_operand(&call_native_slot_zero[3], 0);
  write_slot_operand(&push_int_store_slot[3 + FR_INSTRUCTION_INT_OPERAND_BYTES +
                                          1u],
                     0);
#if FR_FEATURE_CELLS
  write_cell_operands(&load_cell_zero[3], 1, 2);
  write_cell_operands(&store_cell_zero[3], 1, 2);
#endif
  load_arg_zero[4] = 0;
  write_jump_operand(&jump_push_two[3], jump_second_push_ip);
  write_jump_operand(&repeat_two[repeat_begin_ip + 1], repeat_done_ip);
  write_jump_operand(&repeat_two[repeat_next_ip + 1], repeat_body_ip);

  CHECK("view rejects instruction bytes beyond cap",
        fr_instruction_stream_init(&view, return_bytes,
                                   FR_PROFILE_MAX_INSTRUCTION_BYTES + 1) ==
            FR_ERR_RANGE);
  CHECK("view rejects null bytes with nonzero length",
        fr_instruction_stream_init(&view, NULL, 1) == FR_ERR_INVALID);
  CHECK("view accepts return bytes",
        fr_instruction_stream_init(&view, return_bytes, sizeof(return_bytes)) ==
            FR_OK);
  CHECK("header reads",
        fr_instruction_read_header(&view, &header) == FR_OK &&
            header.format_version == FR_INSTRUCTION_FORMAT_VERSION &&
            header.header_size == FR_INSTRUCTION_MIN_HEADER_SIZE &&
            header.arity == 0);
  CHECK("header reads arity",
        fr_instruction_stream_init(&view, load_arg_zero,
                                   sizeof(load_arg_zero)) == FR_OK &&
            fr_instruction_read_header(&view, &header) == FR_OK &&
            header.header_size == FR_INSTRUCTION_ARITY_HEADER_SIZE &&
            header.arity == 1);

  CHECK("read slot operand",
        fr_instruction_stream_init(&view, load_slot_last,
                                   sizeof(load_slot_last)) == FR_OK &&
            fr_instruction_read_slot_operand(&view, 2, &slot_id) == FR_OK &&
            slot_id == FR_PROFILE_MAX_SLOTS - 1);
  CHECK("read int operand",
        fr_instruction_stream_init(&view, push_int_minus_one,
                                   sizeof(push_int_minus_one)) == FR_OK &&
            fr_instruction_read_int_operand(&view, 2, &int_operand) == FR_OK &&
            int_operand == -1);
  CHECK("read jump operand",
        fr_instruction_stream_init(&view, jump_push_two,
                                   sizeof(jump_push_two)) == FR_OK &&
            fr_instruction_read_jump_operand(&view, 2, &jump_target) == FR_OK &&
            jump_target == jump_second_push_ip);
  CHECK("read repeat jump operand",
        fr_instruction_stream_init(&view, repeat_two, sizeof(repeat_two)) ==
                FR_OK &&
            fr_instruction_read_jump_operand(&view, repeat_begin_ip,
                                             &jump_target) == FR_OK &&
            jump_target == repeat_done_ip);
  CHECK("read arg operand",
        fr_instruction_stream_init(&view, load_arg_zero,
                                   sizeof(load_arg_zero)) == FR_OK &&
            fr_instruction_read_arg_operand(&view, 3, &arg_index) == FR_OK &&
            arg_index == 0);
  CHECK("read call slot arg operands",
        fr_instruction_stream_init(&view, call_slot_arg_one,
                                   sizeof(call_slot_arg_one)) == FR_OK &&
            fr_instruction_read_call_slot_arg_operands(
                &view, call_slot_arg_ip, &slot_id, &arg_count) == FR_OK &&
            slot_id == 0 && arg_count == 1);
#if FR_FEATURE_CELLS
  CHECK("read cell operands",
        fr_instruction_stream_init(&view, load_cell_zero,
                                   sizeof(load_cell_zero)) == FR_OK &&
            fr_instruction_read_cell_operands(&view, 2, &slot_id,
                                              &cell_index) == FR_OK &&
            slot_id == 1 && cell_index == 2);
#endif
#if FR_FEATURE_RECORDS
  CHECK("read field operand",
        fr_instruction_stream_init(&view, load_field_x,
                                   sizeof(load_field_x)) == FR_OK &&
            fr_instruction_read_field_operand(&view, 2, &field_name,
                                              &field_length) == FR_OK &&
            field_length == 1 && memcmp(field_name, "x", 1) == 0);
#endif

  CHECK("disassemble return",
        fr_instruction_stream_init(&view, return_bytes, sizeof(return_bytes)) ==
                FR_OK &&
            fr_instruction_disassemble_at(&view, 2, text, sizeof(text),
                                          &text_len, &next_ip) == FR_OK &&
            strcmp(text, "RETURN") == 0 && next_ip == 3);
  CHECK("disassemble call native slot",
        fr_instruction_stream_init(&view, call_native_slot_zero,
                                   sizeof(call_native_slot_zero)) == FR_OK &&
            fr_instruction_disassemble_at(&view, 2, text, sizeof(text),
                                          &text_len, &next_ip) == FR_OK &&
            strcmp(text, "CALL_NATIVE_SLOT 0") == 0 && next_ip == 5);
  CHECK("disassemble call slot arg",
        fr_instruction_stream_init(&view, call_slot_arg_one,
                                   sizeof(call_slot_arg_one)) == FR_OK &&
            fr_instruction_disassemble_at(&view, call_slot_arg_ip, text, sizeof(text),
                                          &text_len, &next_ip) == FR_OK &&
            strcmp(text, "CALL_SLOT_ARG 0 1") == 0 &&
            next_ip == call_slot_arg_ip + 4);
#if FR_FEATURE_CELLS
  CHECK("disassemble load cell",
        fr_instruction_stream_init(&view, load_cell_zero,
                                   sizeof(load_cell_zero)) == FR_OK &&
            fr_instruction_disassemble_at(&view, 2, text, sizeof(text),
                                          &text_len, &next_ip) == FR_OK &&
            strcmp(text, "LOAD_CELL 1 2") == 0 && next_ip == 7);
  CHECK("disassemble store cell",
        fr_instruction_stream_init(&view, store_cell_zero,
                                   sizeof(store_cell_zero)) == FR_OK &&
            fr_instruction_disassemble_at(&view, 2, text, sizeof(text),
                                          &text_len, &next_ip) == FR_OK &&
            strcmp(text, "STORE_CELL 1 2") == 0 && next_ip == 7);
#endif
#if FR_FEATURE_RECORDS
  CHECK("disassemble load field",
        fr_instruction_stream_init(&view, load_field_x,
                                   sizeof(load_field_x)) == FR_OK &&
            fr_instruction_disassemble_at(&view, 2, text, sizeof(text),
                                          &text_len, &next_ip) == FR_OK &&
            strcmp(text, "LOAD_FIELD x") == 0 && next_ip == 5);
  CHECK("disassemble store field",
        fr_instruction_stream_init(&view, store_field_x,
                                   sizeof(store_field_x)) == FR_OK &&
            fr_instruction_disassemble_at(&view, 2, text, sizeof(text),
                                          &text_len, &next_ip) == FR_OK &&
            strcmp(text, "STORE_FIELD x") == 0 && next_ip == 5);
#endif
  CHECK("disassemble drop",
        fr_instruction_stream_init(&view, push_int_drop,
                                   sizeof(push_int_drop)) == FR_OK &&
            fr_instruction_disassemble_at(
                &view, (fr_code_offset_t)(2u + FR_INSTRUCTION_PUSH_INT_SIZE),
                text, sizeof(text),
                                          &text_len, &next_ip) == FR_OK &&
            strcmp(text, "DROP") == 0 &&
            next_ip == 3u + FR_INSTRUCTION_PUSH_INT_SIZE);
  CHECK("disassemble push nil",
        fr_instruction_stream_init(&view, push_nil, sizeof(push_nil)) ==
                FR_OK &&
            fr_instruction_disassemble_at(&view, 2, text, sizeof(text),
                                          &text_len, &next_ip) == FR_OK &&
            strcmp(text, "PUSH_NIL") == 0 && next_ip == 3);
  CHECK("disassemble load arg",
        fr_instruction_stream_init(&view, load_arg_zero,
                                   sizeof(load_arg_zero)) == FR_OK &&
            fr_instruction_disassemble_at(&view, 3, text, sizeof(text),
                                          &text_len, &next_ip) == FR_OK &&
            strcmp(text, "LOAD_ARG 0") == 0 && next_ip == 5);
  CHECK("disassemble repeat begin",
        fr_instruction_stream_init(&view, repeat_two, sizeof(repeat_two)) ==
                FR_OK &&
            fr_instruction_disassemble_at(&view, repeat_begin_ip, text,
                                          sizeof(text),
                                          &text_len, &next_ip) == FR_OK &&
            strcmp(text, expected_repeat_begin) == 0 &&
            next_ip == repeat_body_ip);
  CHECK("disassemble repeat next",
        fr_instruction_stream_init(&view, repeat_two, sizeof(repeat_two)) ==
                FR_OK &&
            fr_instruction_disassemble_at(&view, repeat_next_ip, text,
                                          sizeof(text),
                                          &text_len, &next_ip) == FR_OK &&
            strcmp(text, expected_repeat_next) == 0 &&
            next_ip == repeat_done_ip);
  CHECK("disassemble full instruction stream",
        fr_instruction_stream_init(&view, push_int_store_slot,
                                   sizeof(push_int_store_slot)) == FR_OK &&
            fr_instruction_stream_disassemble(&view, text, sizeof(text),
                                              &text_len) == FR_OK &&
            strcmp(text, "PUSH_INT 7\nSTORE_SLOT 0\nRETURN\n") == 0);
  CHECK("disassemble full nil instruction stream",
        fr_instruction_stream_init(&view, push_nil, sizeof(push_nil)) ==
                FR_OK &&
            fr_instruction_stream_disassemble(&view, text, sizeof(text),
                                              &text_len) == FR_OK &&
            strcmp(text, "PUSH_NIL\nRETURN\n") == 0);
}

static void test_slots(void) {
  fr_runtime_t runtime;
  fr_runtime_limits_t limits;
  fr_tagged_t tagged = 0;
  fr_tagged_t one = 0;
  fr_tagged_t two = 0;
  fr_tagged_t three = 0;

  CHECK("runtime init", fr_runtime_init(&runtime) == FR_OK);
  CHECK("runtime starts uninterrupted", !fr_runtime_is_interrupted(&runtime));
  limits = fr_runtime_get_limits();
  CHECK("runtime reports slot limit", limits.max_slots == FR_PROFILE_MAX_SLOTS);
  CHECK("runtime reports instruction bytes",
        limits.max_instruction_bytes == FR_PROFILE_MAX_INSTRUCTION_BYTES);
  CHECK("runtime reports code objects",
        limits.max_code_objects == FR_PROFILE_CODE_OBJECT_TABLE_SIZE);
  CHECK("runtime reports native slots",
        limits.max_natives == FR_PROFILE_NATIVE_TABLE_SIZE);
  CHECK("runtime reports handles",
        limits.max_handles == FR_PROFILE_MAX_HANDLES);
  CHECK("runtime reports objects",
        limits.max_objects == FR_PROFILE_OBJECT_TABLE_SIZE);
  CHECK("runtime reports cell words",
        limits.max_cell_words == FR_PROFILE_MAX_CELL_WORDS);
  CHECK("runtime reports text bytes",
        limits.max_text_bytes == FR_PROFILE_MAX_TEXT_BYTES);
  CHECK("runtime reports record name bytes",
        limits.max_record_name_bytes == FR_PROFILE_MAX_RECORD_NAME_BYTES);
  CHECK("runtime reports record fields per shape",
        limits.max_record_fields_per_shape ==
            FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE);
  CHECK("runtime reports record shape fields",
        limits.max_record_shape_fields == FR_PROFILE_MAX_RECORD_SHAPE_FIELDS);
  CHECK("runtime reports record value fields",
        limits.max_record_value_fields == FR_PROFILE_MAX_RECORD_VALUE_FIELDS);
  CHECK("runtime reports pad bytes",
        limits.max_pad_bytes == FR_PROFILE_PAD_BYTES);
  CHECK("slot starts nil", fr_slot_read(&runtime, 0, &tagged) == FR_OK);
  CHECK("slot nil tagged", fr_tagged_is_nil(tagged));

  CHECK("encode one", fr_tagged_encode_int(1, &one) == FR_OK);
  CHECK("encode two", fr_tagged_encode_int(2, &two) == FR_OK);
  CHECK("encode three", fr_tagged_encode_int(3, &three) == FR_OK);
  CHECK("slot set base", fr_slot_set_base(&runtime, 0, one) == FR_OK);
  CHECK("slot read base", fr_slot_read(&runtime, 0, &tagged) == FR_OK);
  CHECK("slot stores base", tagged == one);
  CHECK("slot write one", fr_slot_write(&runtime, 0, one) == FR_OK);
  CHECK("slot stores one",
        fr_slot_read(&runtime, 0, &tagged) == FR_OK && tagged == one);
  CHECK("slot matching base is not overlay", !fr_slot_is_overlay(&runtime, 0));
  CHECK("slot rebind two", fr_slot_write(&runtime, 0, two) == FR_OK);
  CHECK("slot stores two",
        fr_slot_read(&runtime, 0, &tagged) == FR_OK && tagged == two);
  CHECK("slot rebind marks overlay", fr_slot_is_overlay(&runtime, 0));
  CHECK("slot count follows written ids", fr_slot_count(&runtime) == 1);

  CHECK("last slot write",
        fr_slot_write(&runtime, FR_PROFILE_MAX_SLOTS - 1, one) == FR_OK);
  CHECK("slot count reaches last written id",
        fr_slot_count(&runtime) == FR_PROFILE_MAX_SLOTS);
  CHECK("slot read one past",
        fr_slot_read(&runtime, FR_PROFILE_MAX_SLOTS, &tagged) == FR_ERR_RANGE);
  CHECK("slot write one past",
        fr_slot_write(&runtime, FR_PROFILE_MAX_SLOTS, one) == FR_ERR_RANGE);
  CHECK("slot base one past",
        fr_slot_set_base(&runtime, FR_PROFILE_MAX_SLOTS, one) == FR_ERR_RANGE);
  CHECK("slot restore one past",
        fr_slot_restore(&runtime, FR_PROFILE_MAX_SLOTS) == FR_ERR_RANGE);

  CHECK("slot 1 set base", fr_slot_set_base(&runtime, 1, two) == FR_OK);
  CHECK("slot 1 write overlay", fr_slot_write(&runtime, 1, three) == FR_OK);
  CHECK("slot restore", fr_slot_restore(&runtime, 0) == FR_OK);
  CHECK("slot restored base",
        fr_slot_read(&runtime, 0, &tagged) == FR_OK && tagged == one);
  CHECK("slot restore clears overlay", !fr_slot_is_overlay(&runtime, 0));
  CHECK("slot 1 current unchanged",
        fr_slot_read(&runtime, 1, &tagged) == FR_OK && tagged == three);

  fr_runtime_interrupt(&runtime);
  CHECK("runtime interrupted", fr_runtime_is_interrupted(&runtime));
  fr_runtime_clear_interrupt(&runtime);
  CHECK("runtime interrupt cleared", !fr_runtime_is_interrupted(&runtime));
  fr_runtime_interrupt(&runtime);
  CHECK("runtime interrupted before reset",
        fr_runtime_is_interrupted(&runtime));
  CHECK("runtime reset", fr_runtime_reset(&runtime) == FR_OK);
  CHECK("runtime reset clears interrupt", !fr_runtime_is_interrupted(&runtime));
  CHECK("slot reset base",
        fr_slot_read(&runtime, 0, &tagged) == FR_OK && tagged == one);
  CHECK("slot 1 reset base",
        fr_slot_read(&runtime, 1, &tagged) == FR_OK && tagged == two);
  CHECK("runtime reset clears overlay", !fr_slot_is_overlay(&runtime, 1));
  CHECK("runtime reset restores base slot count", fr_slot_count(&runtime) == 2);
  CHECK("slot project id detects base",
        !fr_slot_is_project_id(FR_SLOT_BOOT));
  CHECK("slot project id detects project",
        fr_slot_is_project_id(fr_slot_first_project_id()));

  CHECK("slot project clear reuses reset path",
        fr_slot_write(&runtime, 1, three) == FR_OK &&
            fr_runtime_clear_project(&runtime) == FR_OK &&
            fr_slot_read(&runtime, 1, &tagged) == FR_OK && tagged == two &&
            fr_slot_count(&runtime) == 2 &&
            !fr_slot_is_overlay(&runtime, 1));
}

#if FR_FEATURE_HANDLES
static void test_handles(void) {
  fr_runtime_t runtime;
  fr_handle_ref_t refs[FR_HANDLE_TABLE_CAPACITY];
  fr_handle_ref_t ref = {0};
  fr_handle_ref_t stale_ref = {0};
  fr_tagged_t tagged = 0;
  fr_tagged_t next_tagged = 0;
  fr_tagged_t code_tagged = 0;
  fr_tagged_t result = 0;
  fr_handle_kind_t kind = FR_HANDLE_KIND_NONE;
  fr_code_object_id_t code_id = 0;
  uint16_t platform_index = 0;
  uint8_t load_handle_bytes[] = {0x00, 0x00, FR_OP_LOAD_SLOT,
                                 0x00, 0x00, FR_OP_RETURN};
  fr_instruction_stream_t instructions;

  write_instruction_header(load_handle_bytes, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_slot_operand(&load_handle_bytes[3], FR_SLOT_BOOT);

  CHECK("handles runtime init", fr_runtime_init(&runtime) == FR_OK);
  CHECK("handles kind names are stable",
        strcmp(fr_handle_kind_name(FR_HANDLE_KIND_UART), "uart") == 0 &&
            strcmp(fr_handle_kind_name(FR_HANDLE_KIND_PWM), "pwm") == 0 &&
            strcmp(fr_handle_kind_name(FR_HANDLE_KIND_I2C_BUS), "i2c-bus") ==
                0 &&
            strcmp(fr_handle_kind_name(FR_HANDLE_KIND_I2C_DEVICE),
                   "i2c-device") == 0 &&
            strcmp(fr_handle_kind_name(FR_HANDLE_KIND_SPI), "spi") == 0 &&
            strcmp(fr_handle_kind_name(FR_HANDLE_KIND_COUNT), "unknown") ==
                0 &&
            strcmp(fr_handle_kind_name(255), "unknown") == 0);
  CHECK("handles reject none kind",
        fr_handle_reserve(&runtime, FR_HANDLE_KIND_NONE, &ref, &tagged) ==
            FR_ERR_INVALID);
  CHECK("handles name none kind",
        strcmp(fr_handle_kind_name(FR_HANDLE_KIND_NONE), "none") == 0);
  CHECK("handles reject unknown kind",
        fr_handle_reserve(&runtime, 255, &ref, &tagged) == FR_ERR_INVALID);
  CHECK("handles reject kind past last",
        fr_handle_reserve(&runtime, FR_HANDLE_KIND_COUNT, &ref, &tagged) ==
            FR_ERR_INVALID);
  CHECK("handles reserve first synthetic handle",
        fr_handle_reserve(&runtime, FR_TEST_SYNTHETIC_HANDLE_KIND, &ref,
                          &tagged) == FR_OK &&
            ref.id == 0 && ref.generation == 1 &&
            fr_tagged_kind(tagged) == FR_TAGGED_HANDLE);
  CHECK("handles decode reserved tag",
        fr_tagged_decode_handle_ref(tagged, &stale_ref) == FR_OK &&
            stale_ref.id == ref.id && stale_ref.generation == ref.generation);
  CHECK("handles reserved lookup is not active",
        fr_handle_lookup(&runtime, ref, FR_HANDLE_KIND_NONE, &kind,
                         &platform_index) == FR_ERR_HANDLE);
  CHECK("handles activate first synthetic handle",
        fr_handle_activate(&runtime, ref, 7) == FR_OK);
  CHECK("handles lookup active synthetic handle",
        fr_handle_lookup(&runtime, ref, FR_TEST_SYNTHETIC_HANDLE_KIND, &kind,
                         &platform_index) == FR_OK &&
            kind == FR_TEST_SYNTHETIC_HANDLE_KIND && platform_index == 7);
  CHECK("handles reject wrong expected kind",
        fr_handle_lookup(&runtime, ref, FR_HANDLE_KIND_UART, &kind,
                         &platform_index) == FR_ERR_TYPE);
  CHECK("handles reject double activate",
        fr_handle_activate(&runtime, ref, 8) == FR_ERR_INVALID);
  CHECK("handles close active",
        fr_handle_close(&runtime, ref) == FR_OK &&
            fr_handle_lookup(&runtime, ref, FR_HANDLE_KIND_NONE, &kind,
                             &platform_index) == FR_ERR_HANDLE);
  stale_ref = ref;
  CHECK("handles reuse index with new generation",
        fr_handle_reserve(&runtime, FR_TEST_SYNTHETIC_HANDLE_KIND, &ref, &next_tagged) ==
                FR_OK &&
            ref.id == stale_ref.id && ref.generation != stale_ref.generation &&
            next_tagged != tagged);
  CHECK("handles stale ref stays invalid",
        fr_handle_activate(&runtime, ref, 9) == FR_OK &&
            fr_handle_lookup(&runtime, stale_ref, FR_HANDLE_KIND_NONE, &kind,
                             &platform_index) == FR_ERR_HANDLE);
  CHECK("handles release rejects active",
        fr_handle_release_reserved(&runtime, ref) == FR_ERR_INVALID);
  CHECK("handles close reused active", fr_handle_close(&runtime, ref) == FR_OK);
  CHECK("handles release reserved",
        fr_handle_reserve(&runtime, FR_HANDLE_KIND_PWM, &ref, &tagged) ==
                FR_OK &&
            fr_handle_release_reserved(&runtime, ref) == FR_OK &&
            fr_handle_activate(&runtime, ref, 10) == FR_ERR_HANDLE);
  CHECK("handles pass through VM stack",
        fr_handle_reserve(&runtime, FR_TEST_SYNTHETIC_HANDLE_KIND, &ref, &tagged) ==
                FR_OK &&
            fr_handle_activate(&runtime, ref, 12) == FR_OK &&
            fr_slot_write(&runtime, FR_SLOT_BOOT, tagged) == FR_OK &&
            fr_instruction_stream_init(&instructions, load_handle_bytes,
                                       (uint16_t)sizeof(load_handle_bytes)) ==
                FR_OK &&
            fr_code_install(&runtime, &instructions, NULL, 0, &code_id) ==
                FR_OK &&
            fr_tagged_encode_code_object_id(code_id, &code_tagged) == FR_OK &&
            fr_slot_write(&runtime, 1, code_tagged) == FR_OK &&
            fr_vm_run_slot(&runtime, 1, &result) == FR_OK &&
            result == tagged &&
            fr_handle_close(&runtime, ref) == FR_OK);
#if FR_PROFILE_MAX_HANDLES > 1
  CHECK("handles retire exhausted generations",
        fr_runtime_init(&runtime) == FR_OK);
  for (uint8_t i = 0; i < 15; i++) {
    CHECK("handles cycle one entry",
          fr_handle_reserve(&runtime, FR_TEST_SYNTHETIC_HANDLE_KIND, &ref, &tagged) ==
                  FR_OK &&
              (i > 0 || (stale_ref = ref, true)) &&
              ref.id == 0 &&
              fr_handle_activate(&runtime, ref, 20) == FR_OK &&
              fr_handle_close(&runtime, ref) == FR_OK);
  }
  CHECK("handles do not wrap exhausted generation",
        fr_handle_reserve(&runtime, FR_TEST_SYNTHETIC_HANDLE_KIND, &ref, &tagged) ==
                FR_OK &&
            ref.id != stale_ref.id &&
            fr_handle_lookup(&runtime, stale_ref, FR_HANDLE_KIND_NONE, &kind,
                             &platform_index) == FR_ERR_HANDLE &&
            fr_handle_release_reserved(&runtime, ref) == FR_OK);
#endif

  CHECK("handles fill table reset", fr_runtime_init(&runtime) == FR_OK);
  for (fr_handle_id_t i = 0; i < FR_PROFILE_MAX_HANDLES; i++) {
    CHECK("handles fill capacity",
          fr_handle_reserve(&runtime, FR_TEST_SYNTHETIC_HANDLE_KIND, &refs[i],
                            &tagged) == FR_OK &&
              fr_handle_activate(&runtime, refs[i], (uint16_t)(i + 1u)) ==
                  FR_OK);
  }
  CHECK("handles report full table",
        fr_handle_reserve(&runtime, FR_TEST_SYNTHETIC_HANDLE_KIND, &ref, &tagged) ==
            FR_ERR_CAPACITY);
  CHECK("handles clear closes active table",
        fr_runtime_clear_project(&runtime) == FR_OK &&
            fr_handle_lookup(&runtime, refs[0], FR_HANDLE_KIND_NONE, &kind,
                             &platform_index) == FR_ERR_HANDLE);

  CHECK("handles cannot become base values",
        fr_handle_reserve(&runtime, FR_TEST_SYNTHETIC_HANDLE_KIND, &ref, &tagged) ==
                FR_OK &&
            fr_handle_activate(&runtime, ref, 11) == FR_OK &&
            fr_slot_set_base(&runtime, 0, tagged) == FR_ERR_VOLATILE);
  CHECK("handles image literal rejects volatile tagged word",
        fr_image_install(&runtime,
                         &(const fr_image_t){
                             .slot_inits =
                                 (const fr_image_slot_init_t[]){
                                     {0,
                                      {FR_IMAGE_REF_LITERAL_TAGGED, tagged,
                                       0}},
                                 },
                             .slot_init_count = 1,
                         }) == FR_ERR_VOLATILE);
  CHECK("handles overlay literal rejects volatile tagged word",
        fr_overlay_apply(&runtime,
                         &(const fr_overlay_update_t){
                             .slot_inits =
                                 (const fr_image_slot_init_t[]){
                                     {0,
                                      {FR_IMAGE_REF_LITERAL_TAGGED, tagged,
                                       0}},
                                 },
                             .slot_init_count = 1,
                         }) == FR_ERR_VOLATILE);
  CHECK("handles reset closes active handles",
        fr_slot_write(&runtime, FR_SLOT_BOOT, tagged) == FR_OK &&
            fr_runtime_reset(&runtime) == FR_OK &&
            fr_handle_lookup(&runtime, ref, FR_HANDLE_KIND_NONE, &kind,
                             &platform_index) == FR_ERR_HANDLE &&
            fr_slot_read(&runtime, FR_SLOT_BOOT, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
}
#endif

#if FR_FEATURE_PAD
static void test_pad(void) {
  fr_runtime_t runtime;
  const uint8_t *bytes = NULL;
  uint16_t length = 0;
  char out[128];

  CHECK("pad installs base image", fr_base_image_install(&runtime) == FR_OK);
  CHECK("pad starts empty",
        fr_pad_view(&runtime, &bytes, &length) == FR_OK && bytes != NULL &&
            length == 0);
#if FR_FEATURE_TEXT
  CHECK("pad pack copies empty bytes into text",
        fr_repl_eval_line(&runtime, "empty_pad is pad.pack:", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "empty_pad", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "\"\"\nok\n") == 0);
#endif
  CHECK("pad base words inspect as natives",
        fr_repl_eval_line(&runtime, "see pad.reset", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "base target native arity 0\nok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see pad.emit-byte", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "base target native arity 1\nok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see pad.len", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "base target native arity 0\nok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see pad.type", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "base target native arity 0\nok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see pad.peek-byte", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "base target native arity 1\nok\n") == 0);
#if FR_FEATURE_TEXT
  CHECK("pad.pack renders signature",
        fr_repl_eval_line(&runtime, "see pad.pack", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "pad.pack() -> text\n"
                   "pack the pad bytes into a text value\n"
                   "ok\n") == 0);
#endif
  CHECK("pad emits bytes through human call surface",
        fr_repl_eval_line(&runtime, "pad.emit-byte: 65", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "pad.emit-byte: 10", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_pad_view(&runtime, &bytes, &length) == FR_OK && length == 2 &&
            bytes[0] == 'A' && bytes[1] == '\n');
  CHECK("pad len reports byte count",
        fr_repl_eval_line(&runtime, "pad.len:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "2\nok\n") == 0);
  CHECK("pad peeks bytes without consuming",
        fr_repl_eval_line(&runtime, "pad.peek-byte: 0", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "65\nok\n") == 0 &&
            fr_repl_eval_line(&runtime, "pad.peek-byte: 1", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "10\nok\n") == 0 &&
            fr_repl_eval_line(&runtime, "pad.len:", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "2\nok\n") == 0);
  CHECK("pad rejects peek outside byte range",
        fr_repl_eval_line(&runtime, "pad.peek-byte: 2", out, sizeof(out)) ==
            FR_ERR_RANGE);
#if FR_FEATURE_TEXT
  CHECK("pad pack copies bytes into text",
        fr_repl_eval_line(&runtime, "packed is pad.pack:", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_pad_view(&runtime, &bytes, &length) == FR_OK && length == 2 &&
            fr_repl_eval_line(&runtime, "packed", out, sizeof(out)) == FR_OK &&
            strcmp(out, "\"A\\n\"\nok\n") == 0 &&
            fr_pad_reset(&runtime) == FR_OK &&
            fr_pad_view(&runtime, &bytes, &length) == FR_OK && length == 0 &&
            fr_repl_eval_line(&runtime, "packed", out, sizeof(out)) == FR_OK &&
            strcmp(out, "\"A\\n\"\nok\n") == 0);
#endif
  CHECK("pad reset clears bytes",
        fr_repl_eval_line(&runtime, "pad.reset", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "pad.len:", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "0\nok\n") == 0);
  CHECK("pad rejects byte outside range",
        fr_repl_eval_line(&runtime, "pad.emit-byte: 256", out, sizeof(out)) ==
            FR_ERR_DOMAIN);
  for (uint16_t i = 0; i < FR_PROFILE_PAD_BYTES; i++) {
    CHECK("pad fills exact capacity", fr_pad_emit_byte(&runtime, 0) == FR_OK);
  }
  CHECK("pad rejects capacity overflow",
        fr_pad_emit_byte(&runtime, 0) == FR_ERR_CAPACITY);
  CHECK("pad clear resets scratch state",
        fr_repl_eval_line(&runtime, "clear", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_pad_view(&runtime, &bytes, &length) == FR_OK && length == 0);
  CHECK("pad function calls native byte append",
        fr_repl_eval_line(&runtime,
                          "pad_two is fn [ pad.emit-byte: 66; pad.len: ]", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "pad_two:", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "1\nok\n") == 0);
  CHECK("pad native call result is nil",
        fr_pad_reset(&runtime) == FR_OK &&
            fr_pad_emit_byte(&runtime, 67) == FR_OK &&
            fr_repl_eval_line(&runtime, "pad.emit-byte: 68", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_pad_length(&runtime, &length) == FR_OK && length == 2 &&
            fr_repl_eval_line(&runtime, "pad.len:", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "2\nok\n") == 0);
}
#endif

#if FR_FEATURE_UART
static fr_err_t test_uart_entry(fr_runtime_t *runtime, fr_slot_id_t slot_id,
                                const fr_native_entry_t **out_entry) {
  fr_tagged_t tagged = 0;
  fr_native_id_t native_id = 0;

  if (runtime == NULL || out_entry == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_slot_read(runtime, slot_id, &tagged));
  FR_TRY(fr_tagged_decode_native_id(tagged, &native_id));
  return fr_native_get(runtime, native_id, out_entry);
}

static fr_err_t test_uart_open_call(fr_runtime_t *runtime,
                                    const fr_native_entry_t *open_entry,
                                    uint16_t port, uint16_t rate_code,
                                    fr_tagged_t *out_handle) {
  fr_tagged_t args[2] = {0};

  if (out_handle == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_encode_int((int32_t)port, &args[0]));
  FR_TRY(fr_tagged_encode_int((int32_t)rate_code, &args[1]));
  return fr_native_call(runtime, open_entry, args, 2, out_handle);
}

static fr_err_t test_uart_one_handle_call(fr_runtime_t *runtime,
                                          const fr_native_entry_t *entry,
                                          fr_tagged_t handle,
                                          fr_tagged_t *out) {
  return fr_native_call(runtime, entry, &handle, 1, out);
}

static void test_uart(void) {
  fr_runtime_t runtime;
  fr_runtime_t capacity_runtime;
  const fr_native_entry_t *open_entry = NULL;
  const fr_native_entry_t *write_entry = NULL;
  const fr_native_entry_t *read_entry = NULL;
  const fr_native_entry_t *available_entry = NULL;
  const fr_native_entry_t *close_entry = NULL;
  fr_handle_ref_t capacity_refs[FR_PROFILE_MAX_HANDLES];
  fr_tagged_t capacity_handles[FR_PROFILE_MAX_HANDLES];
  fr_tagged_t handle = 0;
  fr_tagged_t second_handle = 0;
  fr_tagged_t result = 0;
  fr_tagged_t write_args[2] = {0};
#if FR_TAGGED_INT_MAX > 65535
  fr_tagged_t wide_open_args[2] = {0};
#endif
  fr_tagged_t wrong_handle = 0;
  fr_handle_ref_t handle_ref = {0};
  fr_handle_ref_t wrong_ref = {0};
  fr_handle_kind_t kind = FR_HANDLE_KIND_NONE;
  uint16_t platform_index = 0;
  fr_int_t decoded = 0;
  char out[128];

  CHECK("uart installs base image", fr_base_image_install(&runtime) == FR_OK);
  CHECK("uart finds native entries",
        test_uart_entry(&runtime, FR_SLOT_UART_OPEN, &open_entry) == FR_OK &&
            test_uart_entry(&runtime, FR_SLOT_UART_WRITE_BYTE, &write_entry) ==
                FR_OK &&
            test_uart_entry(&runtime, FR_SLOT_UART_READ_BYTE, &read_entry) ==
                FR_OK &&
            test_uart_entry(&runtime, FR_SLOT_UART_AVAILABLE,
                            &available_entry) == FR_OK &&
            test_uart_entry(&runtime, FR_SLOT_UART_CLOSE, &close_entry) ==
                FR_OK);

  CHECK("uart see open renders signature",
        fr_repl_eval_line(&runtime, "see uart.open", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "uart.open(port: int, baud: int) -> handle\n"
                   "open a uart port at a baud rate\n"
                   "ok\n") == 0);
  CHECK("uart see open-on renders four-arg override signature",
        fr_repl_eval_line(&runtime, "see uart.open-on", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "uart.open-on(port: int, tx: int, rx: int, baud: int) -> "
                   "handle\n"
                   "open a uart on caller-picked tx and rx pins\n"
                   "ok\n") == 0);
  CHECK("uart see baud literal",
        fr_repl_eval_line(&runtime, "see $baud_115200", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "base target 5\nok\n") == 0);
  CHECK("uart open respects handle capacity",
        fr_base_image_install(&capacity_runtime) == FR_OK);
  for (fr_handle_id_t i = 0; i < FR_PROFILE_MAX_HANDLES; i++) {
    CHECK("uart fills handle table before open",
          fr_handle_reserve(&capacity_runtime, FR_TEST_SYNTHETIC_HANDLE_KIND,
                            &capacity_refs[i], &capacity_handles[i]) ==
              FR_OK);
  }
  CHECK("uart open fails before platform when handles are full",
        test_uart_open_call(&capacity_runtime, open_entry, 0,
                            FR_UART_RATE_9600, &handle) == FR_ERR_CAPACITY);
  for (fr_handle_id_t i = 0; i < FR_PROFILE_MAX_HANDLES; i++) {
    CHECK("uart releases capacity test handles",
          fr_handle_release_reserved(&capacity_runtime, capacity_refs[i]) ==
              FR_OK);
  }
  CHECK("uart repl binds open handle",
        fr_repl_eval_line(&runtime,
                          "appuart is uart.open: 0, $baud_9600", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "appuart", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "handle uart\nok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see appuart", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay volatile uart\nok\n") == 0);
  CHECK("uart repl uses bound handle",
        fr_repl_eval_line(&runtime, "uart.available: appuart", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "5\nok\n") == 0 &&
            fr_repl_eval_line(&runtime, "uart.read-byte: appuart", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "102\nok\n") == 0 &&
            fr_repl_eval_line(&runtime, "uart.write-byte: appuart, 65", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("uart repl close invalidates bound handle",
        fr_repl_eval_line(&runtime, "uart.close: appuart", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "uart.read-byte: appuart", out,
                              sizeof(out)) == FR_ERR_HANDLE);
  CHECK("uart repl can rebind volatile handle slot",
        fr_repl_eval_line(&runtime, "appuart is nil", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see appuart", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay nil\nok\n") == 0);

  CHECK("uart rejects invalid rate",
        test_uart_open_call(&runtime, open_entry, 0, 99, &handle) ==
            FR_ERR_DOMAIN);
#if FR_TAGGED_INT_MAX > 65535
  CHECK("uart rejects oversized platform argument before cast",
        fr_tagged_encode_int(65549, &wide_open_args[0]) == FR_OK &&
            fr_tagged_encode_int(FR_UART_RATE_9600, &wide_open_args[1]) ==
                FR_OK &&
            fr_native_call(&runtime, open_entry, wide_open_args, 2, &handle) ==
                FR_ERR_DOMAIN);
#endif
  CHECK("uart opens with each rate",
        test_uart_open_call(&runtime, open_entry, 0, FR_UART_RATE_9600,
                            &handle) == FR_OK &&
            test_uart_one_handle_call(&runtime, close_entry, handle,
                                      &result) == FR_OK &&
            test_uart_open_call(&runtime, open_entry, 0, FR_UART_RATE_19200,
                                &handle) == FR_OK &&
            test_uart_one_handle_call(&runtime, close_entry, handle,
                                      &result) == FR_OK &&
            test_uart_open_call(&runtime, open_entry, 0, FR_UART_RATE_38400,
                                &handle) == FR_OK &&
            test_uart_one_handle_call(&runtime, close_entry, handle,
                                      &result) == FR_OK &&
            test_uart_open_call(&runtime, open_entry, 0, FR_UART_RATE_57600,
                                &handle) == FR_OK &&
            test_uart_one_handle_call(&runtime, close_entry, handle,
                                      &result) == FR_OK &&
            test_uart_open_call(&runtime, open_entry, 0,
                                FR_UART_RATE_115200, &handle) == FR_OK &&
            test_uart_one_handle_call(&runtime, close_entry, handle,
                                      &result) == FR_OK);

  CHECK("uart opens handle",
        test_uart_open_call(&runtime, open_entry, 0, FR_UART_RATE_9600,
                            &handle) == FR_OK &&
            fr_tagged_decode_handle_ref(handle, &handle_ref) == FR_OK &&
            fr_handle_lookup(&runtime, handle_ref, FR_HANDLE_KIND_UART, &kind,
                             &platform_index) == FR_OK &&
            kind == FR_HANDLE_KIND_UART);
  CHECK("uart rejects double open on same port",
        test_uart_open_call(&runtime, open_entry, 0, FR_UART_RATE_9600,
                            &second_handle) == FR_ERR_DOMAIN);
  CHECK("uart available starts with host script",
        test_uart_one_handle_call(&runtime, available_entry, handle, &result) ==
                FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 5);
  CHECK("uart reads first host byte",
        test_uart_one_handle_call(&runtime, read_entry, handle, &result) ==
                FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 'f');

  write_args[0] = handle;
  CHECK("uart encodes write byte",
        fr_tagged_encode_int(65, &write_args[1]) == FR_OK);
  CHECK("uart writes byte",
        fr_native_call(&runtime, write_entry, write_args, 2, &result) == FR_OK &&
            fr_tagged_is_nil(result));
  CHECK("uart rejects out of range byte",
        fr_tagged_encode_int(256, &write_args[1]) == FR_OK &&
            fr_native_call(&runtime, write_entry, write_args, 2, &result) ==
                FR_ERR_DOMAIN);

  CHECK("uart drains host script",
        test_uart_one_handle_call(&runtime, read_entry, handle, &result) ==
                FR_OK &&
            test_uart_one_handle_call(&runtime, read_entry, handle, &result) ==
                FR_OK &&
            test_uart_one_handle_call(&runtime, read_entry, handle, &result) ==
                FR_OK &&
            test_uart_one_handle_call(&runtime, read_entry, handle, &result) ==
                FR_OK &&
            test_uart_one_handle_call(&runtime, read_entry, handle, &result) ==
                FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == -1);
  CHECK("uart close discards host script",
        test_uart_one_handle_call(&runtime, close_entry, handle, &result) ==
                FR_OK &&
            test_uart_open_call(&runtime, open_entry, 0, FR_UART_RATE_9600,
                                &handle) == FR_OK &&
            test_uart_one_handle_call(&runtime, available_entry, handle,
                                      &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 5);
  CHECK("uart close active handle",
        test_uart_one_handle_call(&runtime, close_entry, handle, &result) ==
                FR_OK &&
            fr_tagged_is_nil(result));
  CHECK("uart rejects double close",
        test_uart_one_handle_call(&runtime, close_entry, handle, &result) ==
            FR_ERR_HANDLE);
  CHECK("uart rejects closed handle",
        test_uart_one_handle_call(&runtime, read_entry, handle, &result) ==
            FR_ERR_HANDLE);

  CHECK("uart rejects wrong handle kind",
        fr_handle_reserve(&runtime, FR_TEST_SYNTHETIC_HANDLE_KIND, &wrong_ref,
                          &wrong_handle) == FR_OK &&
            fr_handle_activate(&runtime, wrong_ref, 7) == FR_OK &&
            test_uart_one_handle_call(&runtime, read_entry, wrong_handle,
                                      &result) == FR_ERR_TYPE &&
            test_uart_one_handle_call(&runtime, close_entry, wrong_handle,
                                      &result) == FR_ERR_TYPE &&
            fr_handle_close(&runtime, wrong_ref) == FR_OK);

#if FR_FEATURE_PERSISTENCE
  CHECK("uart save rejects live handle",
        test_uart_open_call(&runtime, open_entry, 0, FR_UART_RATE_9600,
                            &handle) == FR_OK &&
            fr_slot_write(&runtime, FR_SLOT_BOOT, handle) == FR_OK &&
            fr_persist_save(&runtime) == FR_ERR_VOLATILE &&
            test_uart_one_handle_call(&runtime, close_entry, handle,
                                      &result) == FR_OK &&
            fr_slot_write(&runtime, FR_SLOT_BOOT, fr_tagged_nil()) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK);
#endif

  CHECK("uart reset closes platform resource",
        test_uart_open_call(&runtime, open_entry, 0, FR_UART_RATE_9600,
                            &handle) == FR_OK &&
            fr_tagged_decode_handle_ref(handle, &handle_ref) == FR_OK &&
            fr_runtime_clear_project(&runtime) == FR_OK &&
            fr_handle_lookup(&runtime, handle_ref, FR_HANDLE_KIND_NONE, &kind,
                             &platform_index) == FR_ERR_HANDLE &&
            test_uart_open_call(&runtime, open_entry, 0, FR_UART_RATE_9600,
                                &handle) == FR_OK &&
            test_uart_one_handle_call(&runtime, close_entry, handle,
                                      &result) == FR_OK);

  /* uart.open-on: caller-picked pins. Host records the pins; esp-idf does
   * the same plus a console-pin check. The CHECKs below run on host but
   * exercise the conflict-detection path either platform takes. */
  CHECK("uart.open-on records pins on a free port",
        fr_repl_eval_line(&runtime,
                          "ovr is uart.open-on: 0, 4, 5, $baud_9600", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("uart.open-on rejects overlapping pins on another port",
        fr_repl_eval_line(&runtime,
                          "dup is uart.open-on: 1, 4, 5, $baud_9600", out,
                          sizeof(out)) == FR_ERR_DOMAIN);
  CHECK("uart.open-on rejects identical tx and rx",
        fr_repl_eval_line(&runtime,
                          "same is uart.open-on: 1, 7, 7, $baud_9600", out,
                          sizeof(out)) == FR_ERR_DOMAIN);
  CHECK("uart.open-on close frees pins for reuse",
        fr_repl_eval_line(&runtime, "uart.close: ovr", out, sizeof(out)) ==
                FR_OK &&
            fr_repl_eval_line(&runtime,
                              "again is uart.open-on: 1, 4, 5, $baud_9600",
                              out, sizeof(out)) == FR_OK);
}
#endif

#if FR_FEATURE_PWM
static fr_err_t test_pwm_entry(fr_runtime_t *runtime, fr_slot_id_t slot_id,
                               const fr_native_entry_t **out_entry) {
  fr_tagged_t tagged = 0;
  fr_native_id_t native_id = 0;

  if (runtime == NULL || out_entry == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_slot_read(runtime, slot_id, &tagged));
  FR_TRY(fr_tagged_decode_native_id(tagged, &native_id));
  return fr_native_get(runtime, native_id, out_entry);
}

static fr_err_t test_pwm_open_call(fr_runtime_t *runtime,
                                   const fr_native_entry_t *open_entry,
                                   uint16_t pin, uint16_t freq,
                                   fr_tagged_t *out_handle) {
  fr_tagged_t args[2] = {0};

  if (out_handle == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_encode_int((int32_t)pin, &args[0]));
  FR_TRY(fr_tagged_encode_int((int32_t)freq, &args[1]));
  return fr_native_call(runtime, open_entry, args, 2, out_handle);
}

static fr_err_t test_pwm_write_call(fr_runtime_t *runtime,
                                    const fr_native_entry_t *write_entry,
                                    fr_tagged_t handle, fr_int_t duty,
                                    fr_tagged_t *out) {
  fr_tagged_t args[2] = {0};

  args[0] = handle;
  FR_TRY(fr_tagged_encode_int((int32_t)duty, &args[1]));
  return fr_native_call(runtime, write_entry, args, 2, out);
}

static fr_err_t test_pwm_close_call(fr_runtime_t *runtime,
                                    const fr_native_entry_t *close_entry,
                                    fr_tagged_t handle, fr_tagged_t *out) {
  return fr_native_call(runtime, close_entry, &handle, 1, out);
}

static void test_pwm(void) {
  fr_runtime_t runtime;
  const fr_native_entry_t *open_entry = NULL;
  const fr_native_entry_t *write_entry = NULL;
  const fr_native_entry_t *close_entry = NULL;
  fr_tagged_t handle = 0;
  fr_tagged_t second_handle = 0;
  fr_tagged_t result = 0;
  char out[128];

  CHECK("pwm installs base image", fr_base_image_install(&runtime) == FR_OK);
  CHECK("pwm finds native entries",
        test_pwm_entry(&runtime, FR_SLOT_PWM_OPEN, &open_entry) == FR_OK &&
            test_pwm_entry(&runtime, FR_SLOT_PWM_WRITE, &write_entry) ==
                FR_OK &&
            test_pwm_entry(&runtime, FR_SLOT_PWM_CLOSE, &close_entry) ==
                FR_OK);

  CHECK("pwm see open renders signature",
        fr_repl_eval_line(&runtime, "see pwm.open", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "pwm.open(pin: int, freq: int) -> handle\n"
                   "open a PWM channel on a pin at a frequency in Hz\n"
                   "ok\n") == 0);
  CHECK("pwm see write renders signature",
        fr_repl_eval_line(&runtime, "see pwm.write", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "pwm.write(handle: handle, duty: int) -> nil\n"
                   "set a PWM duty cycle in [0, 1023]\n"
                   "ok\n") == 0);
  CHECK("pwm see close renders signature",
        fr_repl_eval_line(&runtime, "see pwm.close", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "pwm.close(handle: handle) -> nil\n"
                   "release a PWM channel\n"
                   "ok\n") == 0);

  CHECK("pwm full lifecycle",
        test_pwm_open_call(&runtime, open_entry, 4, 1000, &handle) == FR_OK &&
            test_pwm_write_call(&runtime, write_entry, handle, 0, &result) ==
                FR_OK &&
            fr_tagged_is_nil(result) &&
            test_pwm_write_call(&runtime, write_entry, handle, 512, &result) ==
                FR_OK &&
            test_pwm_write_call(&runtime, write_entry, handle, 1023,
                                &result) == FR_OK &&
            test_pwm_close_call(&runtime, close_entry, handle, &result) ==
                FR_OK &&
            fr_tagged_is_nil(result));

  CHECK("pwm rejects pin conflict",
        test_pwm_open_call(&runtime, open_entry, 5, 1000, &handle) == FR_OK &&
            test_pwm_open_call(&runtime, open_entry, 5, 2000,
                               &second_handle) == FR_ERR_DOMAIN &&
            test_pwm_close_call(&runtime, close_entry, handle, &result) ==
                FR_OK);

  CHECK("pwm rejects out-of-range duty",
        test_pwm_open_call(&runtime, open_entry, 6, 1000, &handle) == FR_OK &&
            test_pwm_write_call(&runtime, write_entry, handle, -1, &result) ==
                FR_ERR_DOMAIN &&
            test_pwm_write_call(&runtime, write_entry, handle, 1024,
                                &result) == FR_ERR_DOMAIN &&
            test_pwm_close_call(&runtime, close_entry, handle, &result) ==
                FR_OK);

  CHECK("pwm rejects write after close",
        test_pwm_open_call(&runtime, open_entry, 7, 1000, &handle) == FR_OK &&
            test_pwm_close_call(&runtime, close_entry, handle, &result) ==
                FR_OK &&
            test_pwm_write_call(&runtime, write_entry, handle, 100,
                                &result) == FR_ERR_HANDLE);

#if FR_FEATURE_PERSISTENCE
  CHECK("pwm save rejects live handle",
        test_pwm_open_call(&runtime, open_entry, 8, 1000, &handle) == FR_OK &&
            fr_slot_write(&runtime, FR_SLOT_BOOT, handle) == FR_OK &&
            fr_persist_save(&runtime) == FR_ERR_VOLATILE &&
            test_pwm_close_call(&runtime, close_entry, handle, &result) ==
                FR_OK &&
            fr_slot_write(&runtime, FR_SLOT_BOOT, fr_tagged_nil()) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK);
#endif
}
#endif

#if FR_FEATURE_I2C
static fr_err_t test_i2c_entry(fr_runtime_t *runtime, fr_slot_id_t slot_id,
                               const fr_native_entry_t **out_entry) {
  fr_tagged_t tagged = 0;
  fr_native_id_t native_id = 0;

  if (runtime == NULL || out_entry == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_slot_read(runtime, slot_id, &tagged));
  FR_TRY(fr_tagged_decode_native_id(tagged, &native_id));
  return fr_native_get(runtime, native_id, out_entry);
}

static fr_err_t test_i2c_open_call(fr_runtime_t *runtime,
                                   const fr_native_entry_t *open_entry,
                                   uint16_t port, uint16_t sda, uint16_t scl,
                                   fr_int_t freq, fr_tagged_t *out_handle) {
  fr_tagged_t args[4] = {0};

  if (out_handle == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_tagged_encode_int((int32_t)port, &args[0]));
  FR_TRY(fr_tagged_encode_int((int32_t)sda, &args[1]));
  FR_TRY(fr_tagged_encode_int((int32_t)scl, &args[2]));
  FR_TRY(fr_tagged_encode_int((int32_t)freq, &args[3]));
  return fr_native_call(runtime, open_entry, args, 4, out_handle);
}

static fr_err_t test_i2c_write_call(fr_runtime_t *runtime,
                                    const fr_native_entry_t *write_entry,
                                    fr_tagged_t handle, fr_int_t addr,
                                    fr_object_id_t text_id,
                                    fr_tagged_t *out) {
  fr_tagged_t args[3] = {0};

  args[0] = handle;
  FR_TRY(fr_tagged_encode_int((int32_t)addr, &args[1]));
  FR_TRY(fr_tagged_encode_object_id(text_id, &args[2]));
  return fr_native_call(runtime, write_entry, args, 3, out);
}

static fr_err_t test_i2c_read_call(fr_runtime_t *runtime,
                                   const fr_native_entry_t *read_entry,
                                   fr_tagged_t handle, fr_int_t addr,
                                   fr_int_t count, fr_tagged_t *out) {
  fr_tagged_t args[3] = {0};

  args[0] = handle;
  FR_TRY(fr_tagged_encode_int((int32_t)addr, &args[1]));
  FR_TRY(fr_tagged_encode_int((int32_t)count, &args[2]));
  return fr_native_call(runtime, read_entry, args, 3, out);
}

static fr_err_t test_i2c_close_call(fr_runtime_t *runtime,
                                    const fr_native_entry_t *close_entry,
                                    fr_tagged_t handle, fr_tagged_t *out) {
  return fr_native_call(runtime, close_entry, &handle, 1, out);
}

static void test_i2c(void) {
  fr_runtime_t runtime;
  const fr_native_entry_t *open_entry = NULL;
  const fr_native_entry_t *write_entry = NULL;
  const fr_native_entry_t *read_entry = NULL;
  const fr_native_entry_t *close_entry = NULL;
  const uint8_t payload[] = {0xDE, 0xAD, 0xBE};
  fr_object_id_t payload_id = 0;
  fr_object_id_t empty_id = 0;
  fr_object_id_t read_id = 0;
  fr_tagged_t handle = 0;
  fr_tagged_t second_handle = 0;
  fr_tagged_t result = 0;
  const uint8_t *bytes = NULL;
  uint16_t length = 0;
  char out[128];

  CHECK("i2c installs base image", fr_base_image_install(&runtime) == FR_OK);
  CHECK("i2c finds native entries",
        test_i2c_entry(&runtime, FR_SLOT_I2C_OPEN, &open_entry) == FR_OK &&
            test_i2c_entry(&runtime, FR_SLOT_I2C_WRITE, &write_entry) ==
                FR_OK &&
            test_i2c_entry(&runtime, FR_SLOT_I2C_READ, &read_entry) == FR_OK &&
            test_i2c_entry(&runtime, FR_SLOT_I2C_CLOSE, &close_entry) ==
                FR_OK);

  CHECK(
      "i2c see open renders signature",
      fr_repl_eval_line(&runtime, "see i2c.open", out, sizeof(out)) == FR_OK &&
          strcmp(out,
                 "i2c.open(port: int, sda: int, scl: int, freq: int) -> "
                 "handle\n"
                 "open an i2c bus on a port at sda/scl pins and frequency\n"
                 "ok\n") == 0);
  CHECK("i2c see write renders signature",
        fr_repl_eval_line(&runtime, "see i2c.write", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "i2c.write(bus: handle, addr: int, bytes: text) -> nil\n"
                   "write bytes to a 7-bit i2c address\n"
                   "ok\n") == 0);
  CHECK("i2c see read renders signature",
        fr_repl_eval_line(&runtime, "see i2c.read", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "i2c.read(bus: handle, addr: int, count: int) -> text\n"
                   "read count bytes from a 7-bit i2c address\n"
                   "ok\n") == 0);
  CHECK("i2c see close renders signature",
        fr_repl_eval_line(&runtime, "see i2c.close", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "i2c.close(bus: handle) -> nil\n"
                   "release an i2c bus\n"
                   "ok\n") == 0);

  CHECK("i2c full lifecycle reads host-stub pattern",
        fr_text_install(&runtime, payload, (uint16_t)sizeof(payload),
                        &payload_id) == FR_OK &&
            test_i2c_open_call(&runtime, open_entry, 0, 21, 22, 100000,
                               &handle) == FR_OK &&
            test_i2c_write_call(&runtime, write_entry, handle, 0x42,
                                payload_id, &result) == FR_OK &&
            fr_tagged_is_nil(result) &&
            test_i2c_read_call(&runtime, read_entry, handle, 0x42, 3,
                               &result) == FR_OK &&
            fr_tagged_decode_object_id(result, &read_id) == FR_OK &&
            fr_text_view(&runtime, read_id, &bytes, &length) == FR_OK &&
            length == 3 && bytes[0] == 0x42 && bytes[1] == 0x43 &&
            bytes[2] == 0x44 &&
            test_i2c_close_call(&runtime, close_entry, handle, &result) ==
                FR_OK &&
            fr_tagged_is_nil(result));

  CHECK("i2c open rejects duplicate live port",
        test_i2c_open_call(&runtime, open_entry, 0, 21, 22, 100000,
                           &handle) == FR_OK &&
            test_i2c_open_call(&runtime, open_entry, 0, 30, 31, 100000,
                               &second_handle) == FR_ERR_DOMAIN &&
            test_i2c_close_call(&runtime, close_entry, handle, &result) ==
                FR_OK);
  CHECK("i2c open rejects overlapping sda or scl",
        test_i2c_open_call(&runtime, open_entry, 0, 21, 22, 100000,
                           &handle) == FR_OK &&
            test_i2c_open_call(&runtime, open_entry, 1, 21, 23, 100000,
                               &second_handle) == FR_ERR_DOMAIN &&
            test_i2c_open_call(&runtime, open_entry, 1, 24, 22, 100000,
                               &second_handle) == FR_ERR_DOMAIN &&
            test_i2c_close_call(&runtime, close_entry, handle, &result) ==
                FR_OK);
  CHECK("i2c open rejects zero frequency",
        test_i2c_open_call(&runtime, open_entry, 0, 21, 22, 0, &handle) ==
            FR_ERR_DOMAIN);

  CHECK("i2c address range bites write and read",
        test_i2c_open_call(&runtime, open_entry, 0, 21, 22, 100000,
                           &handle) == FR_OK &&
            fr_text_install(&runtime, payload, (uint16_t)sizeof(payload),
                            &payload_id) == FR_OK &&
            test_i2c_write_call(&runtime, write_entry, handle, 0x80,
                                payload_id, &result) == FR_ERR_DOMAIN &&
            test_i2c_write_call(&runtime, write_entry, handle, -1, payload_id,
                                &result) == FR_ERR_DOMAIN &&
            test_i2c_read_call(&runtime, read_entry, handle, 0x80, 3,
                               &result) == FR_ERR_DOMAIN &&
            test_i2c_read_call(&runtime, read_entry, handle, -1, 3, &result) ==
                FR_ERR_DOMAIN);
  CHECK("i2c read with count above max rejects with range",
        test_i2c_read_call(&runtime, read_entry, handle, 0x42,
                           FR_PROFILE_MAX_TEXT_LENGTH + 1,
                           &result) == FR_ERR_RANGE);
  CHECK("i2c read with count zero returns empty text",
        test_i2c_read_call(&runtime, read_entry, handle, 0x42, 0, &result) ==
                FR_OK &&
            fr_tagged_decode_object_id(result, &read_id) == FR_OK &&
            fr_text_view(&runtime, read_id, &bytes, &length) == FR_OK &&
            length == 0);
  /* Host platform write returns OK for zero-length, so DOMAIN here proves
   * the wrapper rejected before the platform call. */
  CHECK("i2c empty write rejects without reaching platform",
        fr_text_install(&runtime, NULL, 0, &empty_id) == FR_OK &&
            test_i2c_write_call(&runtime, write_entry, handle, 0x42, empty_id,
                                &result) == FR_ERR_DOMAIN);

  CHECK("i2c rejects write and read after close",
        test_i2c_close_call(&runtime, close_entry, handle, &result) == FR_OK &&
            test_i2c_write_call(&runtime, write_entry, handle, 0x42,
                                payload_id, &result) == FR_ERR_HANDLE &&
            test_i2c_read_call(&runtime, read_entry, handle, 0x42, 3,
                               &result) == FR_ERR_HANDLE);

#if FR_FEATURE_PERSISTENCE
  CHECK("i2c save rejects live handle",
        test_i2c_open_call(&runtime, open_entry, 0, 21, 22, 100000,
                           &handle) == FR_OK &&
            fr_slot_write(&runtime, FR_SLOT_BOOT, handle) == FR_OK &&
            fr_persist_save(&runtime) == FR_ERR_VOLATILE &&
            test_i2c_close_call(&runtime, close_entry, handle, &result) ==
                FR_OK &&
            fr_slot_write(&runtime, FR_SLOT_BOOT, fr_tagged_nil()) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK);
#endif
}
#endif

#if FR_FEATURE_RANDOM
static void test_random(void) {
  fr_runtime_t runtime;
  char out[128];

  CHECK("random installs base image",
        fr_base_image_install(&runtime) == FR_OK);

  CHECK("random.next renders signature",
        fr_repl_eval_line(&runtime, "see random.next", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "random.next() -> int\n"
                   "return the next pseudo-random nonnegative int\n"
                   "ok\n") == 0);
  CHECK("random.below renders signature",
        fr_repl_eval_line(&runtime, "see random.below", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "random.below(limit: int) -> int\n"
                   "return a pseudo-random int in [0, limit)\n"
                   "ok\n") == 0);
  CHECK("random.seed renders signature",
        fr_repl_eval_line(&runtime, "see random.seed", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "random.seed(seed: int) -> nil\n"
                   "seed the pseudo-random generator\n"
                   "ok\n") == 0);

  /* The raw xorshift32 sequence is identical across every target; only the
   * FR_TAGGED_INT_MAX mask in the language wrapper changes the visible value
   * per word size. */
#if FR_WORD_SIZE == 16
#define FR_TEST_RANDOM_SEED1_NEXT1 "8225\nok\n"
#define FR_TEST_RANDOM_SEED1_NEXT2 "1537\nok\n"
#define FR_TEST_RANDOM_SEED1_NEXT3 "10437\nok\n"
#define FR_TEST_RANDOM_SEED2_NEXT1 "66\nok\n"
#else
#define FR_TEST_RANDOM_SEED1_NEXT1 "270369\nok\n"
#define FR_TEST_RANDOM_SEED1_NEXT2 "67634689\nok\n"
#define FR_TEST_RANDOM_SEED1_NEXT3 "499951813\nok\n"
#define FR_TEST_RANDOM_SEED2_NEXT1 "540738\nok\n"
#endif
  CHECK("random.seed=1 pins the first three next values",
        fr_repl_eval_line(&runtime, "random.seed: 1", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "random.next:", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, FR_TEST_RANDOM_SEED1_NEXT1) == 0 &&
            fr_repl_eval_line(&runtime, "random.next:", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, FR_TEST_RANDOM_SEED1_NEXT2) == 0 &&
            fr_repl_eval_line(&runtime, "random.next:", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, FR_TEST_RANDOM_SEED1_NEXT3) == 0);
  CHECK("random.seed=2 diverges from seed=1",
        fr_repl_eval_line(&runtime, "random.seed: 2", out, sizeof(out)) ==
                FR_OK &&
            fr_repl_eval_line(&runtime, "random.next:", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, FR_TEST_RANDOM_SEED2_NEXT1) == 0);

  CHECK("random.below: 0 rejects",
        fr_repl_eval_line(&runtime, "random.below: 0", out, sizeof(out)) ==
            FR_ERR_DOMAIN);
  CHECK("random.below: -1 rejects",
        fr_repl_eval_line(&runtime, "random.below: -1", out, sizeof(out)) ==
            FR_ERR_DOMAIN);

  CHECK("random.below: 10 reseeds for the range walk",
        fr_repl_eval_line(&runtime, "random.seed: 1", out, sizeof(out)) ==
            FR_OK);
  for (int i = 0; i < 10; i++) {
    CHECK("random.below: 10 sample is one decimal digit",
          fr_repl_eval_line(&runtime, "random.below: 10", out, sizeof(out)) ==
                  FR_OK &&
              strlen(out) == 5u && out[0] >= '0' && out[0] <= '9' &&
              strcmp(out + 1, "\nok\n") == 0);
  }
}
#endif

#if FR_FEATURE_MATH
static fr_err_t test_math_map_call(fr_runtime_t *runtime,
                                   const fr_native_entry_t *map_entry,
                                   fr_int_t x, fr_int_t in_lo, fr_int_t in_hi,
                                   fr_int_t out_lo, fr_int_t out_hi,
                                   fr_tagged_t *out) {
  fr_tagged_t args[5] = {0};

  FR_TRY(fr_tagged_encode_int((int32_t)x, &args[0]));
  FR_TRY(fr_tagged_encode_int((int32_t)in_lo, &args[1]));
  FR_TRY(fr_tagged_encode_int((int32_t)in_hi, &args[2]));
  FR_TRY(fr_tagged_encode_int((int32_t)out_lo, &args[3]));
  FR_TRY(fr_tagged_encode_int((int32_t)out_hi, &args[4]));
  return fr_native_call(runtime, map_entry, args, 5, out);
}

static void test_math(void) {
  fr_runtime_t runtime;
  fr_tagged_t map_tagged = 0;
  fr_native_id_t map_native_id = 0;
  const fr_native_entry_t *map_entry = NULL;
  fr_tagged_t map_result = 0;
  fr_int_t map_decoded = 0;
  char out[128];

  CHECK("math installs base image",
        fr_base_image_install(&runtime) == FR_OK);
  CHECK("math finds map entry",
        fr_slot_read(&runtime, FR_SLOT_MAP, &map_tagged) == FR_OK &&
            fr_tagged_decode_native_id(map_tagged, &map_native_id) == FR_OK &&
            fr_native_get(&runtime, map_native_id, &map_entry) == FR_OK);

  CHECK("abs renders signature",
        fr_repl_eval_line(&runtime, "see abs", out, sizeof(out)) == FR_OK &&
            strcmp(out,
                   "abs(x: int) -> int\n"
                   "return the absolute value of an int\n"
                   "ok\n") == 0);
  CHECK("min renders signature",
        fr_repl_eval_line(&runtime, "see min", out, sizeof(out)) == FR_OK &&
            strcmp(out,
                   "min(a: int, b: int) -> int\n"
                   "return the smaller of two ints\n"
                   "ok\n") == 0);
  CHECK("max renders signature",
        fr_repl_eval_line(&runtime, "see max", out, sizeof(out)) == FR_OK &&
            strcmp(out,
                   "max(a: int, b: int) -> int\n"
                   "return the larger of two ints\n"
                   "ok\n") == 0);
  CHECK("clamp renders signature",
        fr_repl_eval_line(&runtime, "see clamp", out, sizeof(out)) == FR_OK &&
            strcmp(out,
                   "clamp(x: int, lo: int, hi: int) -> int\n"
                   "clamp x to the inclusive range [lo, hi]\n"
                   "ok\n") == 0);
  CHECK("map renders signature",
        fr_repl_eval_line(&runtime, "see map", out, sizeof(out)) == FR_OK &&
            strcmp(out,
                   "map(x: int, in_lo: int, in_hi: int, out_lo: int, "
                   "out_hi: int) -> int\n"
                   "linearly remap x from one range to another\n"
                   "ok\n") == 0);
  CHECK("mod renders signature",
        fr_repl_eval_line(&runtime, "see mod", out, sizeof(out)) == FR_OK &&
            strcmp(out,
                   "mod(a: int, b: int) -> int\n"
                   "return a modulo b (C truncating semantics)\n"
                   "ok\n") == 0);

  CHECK("abs of negative flips sign",
        fr_repl_eval_line(&runtime, "abs: -5", out, sizeof(out)) == FR_OK &&
            strcmp(out, "5\nok\n") == 0);
  CHECK("abs of zero is zero",
        fr_repl_eval_line(&runtime, "abs: 0", out, sizeof(out)) == FR_OK &&
            strcmp(out, "0\nok\n") == 0);
  CHECK("abs of positive is identity",
        fr_repl_eval_line(&runtime, "abs: 5", out, sizeof(out)) == FR_OK &&
            strcmp(out, "5\nok\n") == 0);
  /* FR_TAGGED_INT_MIN on the 32-bit profile; magnitude exceeds MAX. */
  CHECK("abs of tagged-int-min rejects",
        fr_repl_eval_line(&runtime, "abs: -1073741824", out, sizeof(out)) ==
            FR_ERR_RANGE);

  CHECK("min picks the smaller",
        fr_repl_eval_line(&runtime, "min: 3, 7", out, sizeof(out)) == FR_OK &&
            strcmp(out, "3\nok\n") == 0);
  CHECK("max picks the larger",
        fr_repl_eval_line(&runtime, "max: 3, 7", out, sizeof(out)) == FR_OK &&
            strcmp(out, "7\nok\n") == 0);

  CHECK("clamp inside passes through",
        fr_repl_eval_line(&runtime, "clamp: 5, 0, 10", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "5\nok\n") == 0);
  CHECK("clamp below pins to lo",
        fr_repl_eval_line(&runtime, "clamp: -3, 0, 10", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "0\nok\n") == 0);
  CHECK("clamp above pins to hi",
        fr_repl_eval_line(&runtime, "clamp: 15, 0, 10", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "10\nok\n") == 0);
  CHECK("clamp with lo > hi rejects",
        fr_repl_eval_line(&runtime, "clamp: 5, 10, 0", out, sizeof(out)) ==
            FR_ERR_DOMAIN);

  /* map's five args exceed the REPL parser's per-call child cap, so its
   * behavior cases run as direct native calls. (50-0)*(1023-0)/(100-0) =
   * 51150/100 = 511 after truncation. */
  CHECK("map remaps linearly",
        test_math_map_call(&runtime, map_entry, 50, 0, 100, 0, 1023,
                           &map_result) == FR_OK &&
            fr_tagged_decode_int(map_result, &map_decoded) == FR_OK &&
            map_decoded == 511);
  CHECK("map with zero input span rejects",
        test_math_map_call(&runtime, map_entry, 0, 0, 0, 0, 100,
                           &map_result) == FR_ERR_DOMAIN);
  /* 1_000_000 * 3000 = 3e9 overflows int32_t; the int64_t temp catches it
   * and the division returns 3000 cleanly. */
  CHECK("map wide product fits after division",
        test_math_map_call(&runtime, map_entry, 1000000, 0, 1000000, 0, 3000,
                           &map_result) == FR_OK &&
            fr_tagged_decode_int(map_result, &map_decoded) == FR_OK &&
            map_decoded == 3000);
  /* 1073741823 * 2 = 2147483646, past FR_TAGGED_INT_MAX. */
  CHECK("map result above tagged max rejects",
        test_math_map_call(&runtime, map_entry, 1073741823, 0, 1, 0, 2,
                           &map_result) == FR_ERR_RANGE);

  CHECK("mod positive remainder",
        fr_repl_eval_line(&runtime, "mod: 7, 3", out, sizeof(out)) == FR_OK &&
            strcmp(out, "1\nok\n") == 0);
  CHECK("mod negative dividend keeps its sign",
        fr_repl_eval_line(&runtime, "mod: -7, 3", out, sizeof(out)) == FR_OK &&
            strcmp(out, "-1\nok\n") == 0);
  CHECK("mod negative divisor takes sign of dividend",
        fr_repl_eval_line(&runtime, "mod: 7, -3", out, sizeof(out)) == FR_OK &&
            strcmp(out, "1\nok\n") == 0);
  CHECK("mod by zero rejects",
        fr_repl_eval_line(&runtime, "mod: 5, 0", out, sizeof(out)) ==
            FR_ERR_DOMAIN);
  CHECK("percent infix positive remainder",
        fr_repl_eval_line(&runtime, "7 % 3", out, sizeof(out)) == FR_OK &&
            strcmp(out, "1\nok\n") == 0);
  CHECK("percent infix negative dividend keeps sign",
        fr_repl_eval_line(&runtime, "-7 % 3", out, sizeof(out)) == FR_OK &&
            strcmp(out, "-1\nok\n") == 0);
  CHECK("percent infix by zero rejects",
        fr_repl_eval_line(&runtime, "7 % 0", out, sizeof(out)) ==
            FR_ERR_DOMAIN);
  CHECK("percent infix binds left-to-right with star",
        fr_repl_eval_line(&runtime, "7 % 2 * 3", out, sizeof(out)) == FR_OK &&
            strcmp(out, "3\nok\n") == 0);
}
#endif

#if FR_FEATURE_CELLS
static void test_cells(void) {
  fr_runtime_t runtime;
  fr_object_id_t object_id = 0;
  fr_object_id_t extra_object_id = 0;
  fr_tagged_t tagged = 0;
  fr_tagged_t one = 0;
  fr_tagged_t two = 0;
  fr_tagged_t code_tagged = 0;
  fr_tagged_t object_tagged = 0;
#if FR_FEATURE_HANDLES
  fr_handle_ref_t handle_ref = {0};
  fr_tagged_t handle_tagged = 0;
#endif
  fr_int_t decoded = 0;
  uint16_t length = 0;

  CHECK("cells runtime init", fr_runtime_init(&runtime) == FR_OK);
  CHECK("cells encode one", fr_tagged_encode_int(1, &one) == FR_OK);
  CHECK("cells encode two", fr_tagged_encode_int(2, &two) == FR_OK);
  CHECK("cells encode code", fr_tagged_encode_code_object_id(0, &code_tagged) ==
                                 FR_OK);
  CHECK("cells encode object",
        fr_tagged_encode_object_id(0, &object_tagged) == FR_OK);
#if FR_FEATURE_HANDLES
  CHECK("cells reserve handle",
        fr_handle_reserve(&runtime, FR_TEST_SYNTHETIC_HANDLE_KIND, &handle_ref,
                          &handle_tagged) == FR_OK &&
            fr_handle_activate(&runtime, handle_ref, 3) == FR_OK);
#endif

  CHECK("cells reject zero length",
        fr_cells_install(&runtime, 0, NULL, &object_id) == FR_ERR_RANGE);
  CHECK("cells reject overlength",
        fr_cells_install(&runtime, FR_PROFILE_MAX_CELL_LENGTH + 1, NULL,
                         &object_id) == FR_ERR_RANGE);
  CHECK("cells reject null out",
        fr_cells_install(&runtime, 1, NULL, NULL) == FR_ERR_INVALID);
  CHECK("cells reject code initial value",
        fr_cells_install(&runtime, 1, &code_tagged, &object_id) ==
            FR_ERR_TYPE);
#if FR_FEATURE_HANDLES
  CHECK("cells reject handle initial value",
        fr_cells_install(&runtime, 1, &handle_tagged, &object_id) ==
            FR_ERR_TYPE);
#endif

  CHECK("cells install",
        fr_cells_install(&runtime, 2, NULL, &object_id) == FR_OK &&
            object_id == 0 && fr_object_count(&runtime) == 1 &&
            fr_object_is_overlay(&runtime, object_id));
  CHECK("cells length",
        fr_cells_length(&runtime, object_id, &length) == FR_OK &&
            length == 2);
  CHECK("cells start nil",
        fr_cells_read(&runtime, object_id, 0, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("cells second slot starts nil",
        fr_cells_read(&runtime, object_id, 1, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("cells write int",
        fr_cells_write(&runtime, object_id, 0, one) == FR_OK &&
            fr_cells_read(&runtime, object_id, 0, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("cells reject code value",
        fr_cells_write(&runtime, object_id, 0, code_tagged) == FR_ERR_TYPE);
  CHECK("cells reject object value",
        fr_cells_write(&runtime, object_id, 0, object_tagged) == FR_ERR_TYPE);
#if FR_FEATURE_HANDLES
  CHECK("cells reject handle value",
        fr_cells_write(&runtime, object_id, 0, handle_tagged) == FR_ERR_TYPE);
  CHECK("cells close handle", fr_handle_close(&runtime, handle_ref) == FR_OK);
#endif
  CHECK("cells reject read out of bounds",
        fr_cells_read(&runtime, object_id, 2, &tagged) == FR_ERR_RANGE);
  CHECK("cells reject write out of bounds",
        fr_cells_write(&runtime, object_id, 2, one) == FR_ERR_RANGE);

  CHECK("cells base restore",
        (fr_object_mark_base(&runtime), true) &&
            !fr_object_is_overlay(&runtime, object_id) &&
            fr_cells_write(&runtime, object_id, 0, two) == FR_OK &&
            fr_object_is_overlay(&runtime, object_id) &&
            (fr_object_restore_base(&runtime), true) &&
            fr_cells_read(&runtime, object_id, 0, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1 &&
            !fr_object_is_overlay(&runtime, object_id));
  CHECK("cells restore drops overlay object",
        fr_cells_install(&runtime, 1, NULL, &extra_object_id) == FR_OK &&
            extra_object_id == 1 && fr_object_is_overlay(&runtime, 1) &&
            (fr_object_restore_base(&runtime), true) &&
            fr_object_count(&runtime) == 1 &&
            fr_cells_length(&runtime, 1, &length) == FR_ERR_NOT_FOUND);

  CHECK("cells object capacity reset", fr_runtime_init(&runtime) == FR_OK);
  for (uint16_t i = 0; i < FR_PROFILE_OBJECT_TABLE_SIZE; i++) {
    CHECK("cells fill object table",
          fr_cells_install(&runtime, 1, NULL, &object_id) == FR_OK);
  }
  CHECK("cells object table full",
        fr_cells_install(&runtime, 1, NULL, &object_id) == FR_ERR_CAPACITY);

  CHECK("cells word capacity reset", fr_runtime_init(&runtime) == FR_OK);
  while (fr_cells_install(&runtime, FR_PROFILE_MAX_CELL_LENGTH, NULL,
                          &object_id) == FR_OK) {
  }
  CHECK("cells word table full",
        runtime.objects.used_cell_words + FR_PROFILE_MAX_CELL_LENGTH >
                FR_PROFILE_MAX_CELL_WORDS ||
            fr_object_count(&runtime) == FR_PROFILE_OBJECT_TABLE_SIZE);
  CHECK("cells no partial failed install",
        fr_cells_install(&runtime, 1, NULL, &extra_object_id) ==
            FR_ERR_CAPACITY);
}
#endif

#if FR_FEATURE_TEXT
static void test_text_objects(void) {
  fr_runtime_t runtime;
  const uint8_t text[] = {'r', 'e', 'a', 'd', 'y'};
  const uint8_t binary[] = {'a', '\0', 'b'};
  const uint8_t *bytes = NULL;
  fr_object_id_t text_id = 0;
  fr_object_id_t same_text_id = 0;
  fr_object_id_t binary_id = 0;
  fr_object_id_t empty_id = 0;
  fr_tagged_t text_tagged = 0;
  fr_tagged_t tagged = 0;
  bool filled_text_table = false;
  uint16_t length = 0;

  CHECK("text runtime init", fr_runtime_init(&runtime) == FR_OK);
  CHECK("text installs bytes",
        fr_text_install(&runtime, text, (uint16_t)sizeof(text), &text_id) ==
                FR_OK &&
            text_id == 0 && fr_object_count(&runtime) == 1 &&
            fr_object_is_overlay(&runtime, text_id));
  CHECK("text view returns exact bytes",
        fr_text_view(&runtime, text_id, &bytes, &length) == FR_OK &&
            length == sizeof(text) && memcmp(bytes, text, sizeof(text)) == 0);
  CHECK("text reuses exact bytes",
        fr_text_install(&runtime, text, (uint16_t)sizeof(text),
                        &same_text_id) == FR_OK &&
            same_text_id == text_id && fr_object_count(&runtime) == 1);
  CHECK("text preserves nul bytes",
        fr_text_install(&runtime, binary, (uint16_t)sizeof(binary),
                        &binary_id) == FR_OK &&
            fr_text_view(&runtime, binary_id, &bytes, &length) == FR_OK &&
            length == sizeof(binary) &&
            memcmp(bytes, binary, sizeof(binary)) == 0);
  CHECK("text allows empty text",
        fr_text_install(&runtime, NULL, 0, &empty_id) == FR_OK &&
            fr_text_view(&runtime, empty_id, &bytes, &length) == FR_OK &&
            length == 0);
  CHECK("text reuses empty text",
        fr_text_install(&runtime, NULL, 0, &same_text_id) == FR_OK &&
            same_text_id == empty_id);
  CHECK("text rejects oversized text",
        fr_text_install(&runtime, text, FR_PROFILE_MAX_TEXT_LENGTH + 1,
                        &same_text_id) == FR_ERR_RANGE);
  CHECK("text base restore drops overlay text",
        (fr_object_mark_base(&runtime), true) &&
            !fr_object_is_overlay(&runtime, text_id) &&
            fr_text_install(&runtime, (const uint8_t *)"new", 3,
                            &same_text_id) == FR_OK &&
            (fr_object_restore_base(&runtime), true) &&
            fr_text_view(&runtime, same_text_id, &bytes, &length) ==
                FR_ERR_NOT_FOUND &&
            fr_text_view(&runtime, text_id, &bytes, &length) == FR_OK);
#if FR_FEATURE_CELLS
  CHECK("cells can hold text values",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_text_install(&runtime, text, (uint16_t)sizeof(text),
                            &text_id) == FR_OK &&
            fr_tagged_encode_object_id(text_id, &text_tagged) == FR_OK &&
            fr_cells_install(&runtime, 1, &text_tagged, &binary_id) ==
                FR_OK &&
            fr_cells_read(&runtime, binary_id, 0, &tagged) == FR_OK &&
            tagged == text_tagged);
  filled_text_table = fr_runtime_init(&runtime) == FR_OK;
  for (uint16_t i = 0; i < FR_PROFILE_OBJECT_TABLE_SIZE; i++) {
    uint8_t byte = (uint8_t)(i + 1);

    filled_text_table =
        filled_text_table &&
        fr_text_install(&runtime, &byte, 1, &text_id) == FR_OK;
  }
  CHECK("shared object table blocks cells after text fills it",
        filled_text_table &&
            fr_object_count(&runtime) == FR_PROFILE_OBJECT_TABLE_SIZE &&
            fr_text_install(&runtime, (const uint8_t *)"\x01", 1,
                            &same_text_id) == FR_OK &&
            same_text_id == 0 &&
            fr_cells_install(&runtime, 1, NULL, &binary_id) ==
                FR_ERR_CAPACITY);
#endif
}

static fr_err_t test_text_native_id(fr_runtime_t *runtime, fr_slot_id_t slot_id,
                                    const fr_native_entry_t **out_entry) {
  fr_tagged_t slot_value = 0;
  fr_native_id_t native_id = 0;

  FR_TRY(fr_slot_read(runtime, slot_id, &slot_value));
  FR_TRY(fr_tagged_decode_native_id(slot_value, &native_id));
  return fr_native_get(runtime, native_id, out_entry);
}

/* Walk the named text via text.at, asserting each byte equals expected[i]. */
static int test_text_bytes_match(fr_runtime_t *runtime, const char *name,
                                 const char *expected) {
  char cmd[64];
  char out[32];
  char want[16];
  size_t n = strlen(expected);

  for (size_t i = 0; i < n; i++) {
    snprintf(cmd, sizeof(cmd), "text.at: %s, %zu", name, i);
    snprintf(want, sizeof(want), "%u\nok\n",
             (unsigned)(unsigned char)expected[i]);
    if (fr_repl_eval_line(runtime, cmd, out, sizeof(out)) != FR_OK) {
      return 0;
    }
    if (strcmp(out, want) != 0) {
      return 0;
    }
  }
  return 1;
}

static void test_text_natives(void) {
  fr_runtime_t runtime;
  char out[128];
  const fr_native_entry_t *concat_entry = NULL;
  fr_tagged_t concat_args[2] = {0};
  fr_tagged_t concat_result = 0;
  uint8_t long_bytes[FR_PROFILE_MAX_TEXT_LENGTH];
  fr_object_id_t long_id = 0;
  fr_object_id_t fill_id = 0;
  uint16_t fill_counter = 0;

  CHECK("text natives install base image",
        fr_base_image_install(&runtime) == FR_OK);

  CHECK("text.length renders signature",
        fr_repl_eval_line(&runtime, "see text.length", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "text.length(t: text) -> int\n"
                   "return the byte length of a text\n"
                   "ok\n") == 0);
  CHECK("text.equals? renders signature",
        fr_repl_eval_line(&runtime, "see text.equals?", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "text.equals?(a: text, b: text) -> any\n"
                   "return true if two texts have equal bytes\n"
                   "ok\n") == 0);
  CHECK("text.concat renders signature",
        fr_repl_eval_line(&runtime, "see text.concat", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "text.concat(a: text, b: text) -> text\n"
                   "join two texts into a new text\n"
                   "ok\n") == 0);
  CHECK("text.at renders signature",
        fr_repl_eval_line(&runtime, "see text.at", out, sizeof(out)) == FR_OK &&
            strcmp(out,
                   "text.at(t: text, i: int) -> int\n"
                   "return the byte at index i of a text\n"
                   "ok\n") == 0);
  CHECK("text.from-int renders signature",
        fr_repl_eval_line(&runtime, "see text.from-int", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "text.from-int(n: int) -> text\n"
                   "render an int as decimal text\n"
                   "ok\n") == 0);

  CHECK("text.length on bare literal evaluates",
        fr_repl_eval_line(&runtime, "text.length: \"hello\"", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "5\nok\n") == 0);
  CHECK("text.length on empty literal evaluates to zero",
        fr_repl_eval_line(&runtime, "text.length: \"\"", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "0\nok\n") == 0);
  CHECK("text.equals? on equal literals is true",
        fr_repl_eval_line(&runtime, "text.equals?: \"abc\", \"abc\"", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "true\nok\n") == 0);
  CHECK("text.concat on literal with text.from-int evaluates",
        fr_repl_eval_line(&runtime,
                          "labeled is text.concat: \"led=\", text.from-int: 1",
                          out, sizeof(out)) == FR_OK &&
            fr_repl_eval_line(&runtime, "text.length: labeled", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "5\nok\n") == 0 &&
            test_text_bytes_match(&runtime, "labeled", "led=1"));
  CHECK("bare text.concat literal expression has bytes 'led=1'",
        fr_repl_eval_line(
            &runtime,
            "text.equals?: (text.concat: \"led=\", text.from-int: 1), \"led=1\"",
            out, sizeof(out)) == FR_OK &&
            strcmp(out, "true\nok\n") == 0);
  CHECK("function body with text literal defines and evaluates",
        fr_repl_eval_line(&runtime,
                          "to greet with x [ text.concat: \"led=\", "
                          "text.from-int: x ]",
                          out, sizeof(out)) == FR_OK &&
            fr_repl_eval_line(&runtime, "labeled is greet: 1", out,
                              sizeof(out)) == FR_OK &&
            fr_repl_eval_line(&runtime, "text.length: labeled", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "5\nok\n") == 0 &&
            test_text_bytes_match(&runtime, "labeled", "led=1"));
  CHECK("see renders a saved function with a text literal",
        fr_repl_eval_line(&runtime, "see greet", out, sizeof(out)) == FR_OK &&
            strcmp(out,
                   "overlay code\n"
                   "to greet with x [ text.concat: \"led=\", "
                   "text.from-int: x ]\n"
                   "ok\n") == 0);
  {
    const uint8_t marker[] = {'i', 'n', 't', 'e', 'r', 'n', 'e', 'd'};
    fr_object_id_t first_id = 0;
    fr_object_id_t second_id = 0;

    CHECK("repeated bare literals intern to the same object id",
          fr_repl_eval_line(&runtime, "text.length: \"interned\"", out,
                            sizeof(out)) == FR_OK &&
              fr_text_find(&runtime, marker, (uint16_t)sizeof(marker), 0,
                           &first_id) == FR_OK &&
              fr_repl_eval_line(&runtime, "text.length: \"interned\"", out,
                                sizeof(out)) == FR_OK &&
              fr_text_find(&runtime, marker, (uint16_t)sizeof(marker), 0,
                           &second_id) == FR_OK &&
              first_id == second_id);
  }
  {
    char over_long[FR_PROFILE_MAX_TEXT_LENGTH + 16];
    over_long[0] = '"';
    memset(&over_long[1], 'x', FR_PROFILE_MAX_TEXT_LENGTH + 1);
    over_long[FR_PROFILE_MAX_TEXT_LENGTH + 2] = '"';
    over_long[FR_PROFILE_MAX_TEXT_LENGTH + 3] = '\0';
    CHECK("bare literal over the per-text cap is rejected",
          fr_repl_eval_line(&runtime, over_long, out, sizeof(out)) ==
              FR_ERR_RANGE);
  }

  CHECK("text natives bind sample texts",
        fr_repl_eval_line(&runtime, "hello is \"hello\"", out, sizeof(out)) ==
                FR_OK &&
            fr_repl_eval_line(&runtime, "empty is \"\"", out, sizeof(out)) ==
                FR_OK &&
            fr_repl_eval_line(&runtime, "abc is \"abc\"", out, sizeof(out)) ==
                FR_OK &&
            fr_repl_eval_line(&runtime, "abcd is \"abcd\"", out, sizeof(out)) ==
                FR_OK &&
            fr_repl_eval_line(&runtime, "abd is \"abd\"", out, sizeof(out)) ==
                FR_OK &&
            fr_repl_eval_line(&runtime, "foo is \"foo\"", out, sizeof(out)) ==
                FR_OK &&
            fr_repl_eval_line(&runtime, "bar is \"bar\"", out, sizeof(out)) ==
                FR_OK);

  CHECK("text.length of hello is five",
        fr_repl_eval_line(&runtime, "text.length: hello", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "5\nok\n") == 0);
  CHECK("text.length of empty is zero",
        fr_repl_eval_line(&runtime, "text.length: empty", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "0\nok\n") == 0);
  CHECK("text.length rejects int arg",
        fr_repl_eval_line(&runtime, "text.length: 7", out, sizeof(out)) ==
            FR_ERR_TYPE);

  CHECK("text.equals? of equal texts is true",
        fr_repl_eval_line(&runtime, "text.equals?: abc, abc", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "true\nok\n") == 0);
  CHECK("text.equals? rejects mismatched lengths",
        fr_repl_eval_line(&runtime, "text.equals?: abc, abcd", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "false\nok\n") == 0);
  CHECK("text.equals? rejects mismatched bytes",
        fr_repl_eval_line(&runtime, "text.equals?: abc, abd", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "false\nok\n") == 0);

  CHECK("text.concat joins two texts",
        fr_repl_eval_line(&runtime, "joined is text.concat: foo, bar", out,
                          sizeof(out)) == FR_OK &&
            fr_repl_eval_line(&runtime, "text.length: joined", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "6\nok\n") == 0 &&
            test_text_bytes_match(&runtime, "joined", "foobar"));
  CHECK("text.concat with empty interns to the original",
        fr_repl_eval_line(&runtime, "joined_empty is text.concat: abc, empty",
                          out, sizeof(out)) == FR_OK &&
            fr_repl_eval_line(&runtime, "text.equals?: joined_empty, abc", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "true\nok\n") == 0);

  {
    const uint8_t abc_bytes[3] = {'a', 'b', 'c'};
    const fr_native_entry_t *empty_concat_entry = NULL;
    fr_object_id_t abc_id = 0;
    fr_object_id_t empty_id = 0;
    fr_object_id_t result_id = 0;
    fr_tagged_t empty_args[2] = {0};
    fr_tagged_t empty_result = 0;

    CHECK("text.concat with empty returns the same object id as the original",
          fr_text_find(&runtime, abc_bytes, (uint16_t)sizeof(abc_bytes), 0,
                       &abc_id) == FR_OK &&
              fr_text_find(&runtime, NULL, 0, 0, &empty_id) == FR_OK &&
              fr_tagged_encode_object_id(abc_id, &empty_args[0]) == FR_OK &&
              fr_tagged_encode_object_id(empty_id, &empty_args[1]) == FR_OK &&
              test_text_native_id(&runtime, FR_SLOT_TEXT_CONCAT,
                                  &empty_concat_entry) == FR_OK &&
              fr_native_call(&runtime, empty_concat_entry, empty_args, 2,
                             &empty_result) == FR_OK &&
              fr_tagged_decode_object_id(empty_result, &result_id) == FR_OK &&
              result_id == abc_id);
  }

  CHECK("text.at reads first byte",
        fr_repl_eval_line(&runtime, "text.at: abc, 0", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "97\nok\n") == 0);
  CHECK("text.at reads last byte",
        fr_repl_eval_line(&runtime, "text.at: abc, 2", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "99\nok\n") == 0);
  CHECK("text.at rejects index at length",
        fr_repl_eval_line(&runtime, "text.at: abc, 3", out, sizeof(out)) ==
            FR_ERR_RANGE);
  CHECK("text.at rejects negative index",
        fr_repl_eval_line(&runtime, "text.at: abc, -1", out, sizeof(out)) ==
            FR_ERR_RANGE);

  CHECK("text.from-int renders zero",
        fr_repl_eval_line(&runtime, "zero_text is text.from-int: 0", out,
                          sizeof(out)) == FR_OK &&
            fr_repl_eval_line(&runtime, "text.length: zero_text", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "1\nok\n") == 0 &&
            fr_repl_eval_line(&runtime, "text.at: zero_text, 0", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "48\nok\n") == 0);
  CHECK("text.from-int renders 123 as three bytes",
        fr_repl_eval_line(&runtime, "n123 is text.from-int: 123", out,
                          sizeof(out)) == FR_OK &&
            fr_repl_eval_line(&runtime, "text.length: n123", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "3\nok\n") == 0 &&
            test_text_bytes_match(&runtime, "n123", "123"));
  CHECK("text.from-int renders tagged int min",
        fr_repl_eval_line(&runtime, "tmin is text.from-int: -1073741824", out,
                          sizeof(out)) == FR_OK &&
            fr_repl_eval_line(&runtime, "text.length: tmin", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "11\nok\n") == 0 &&
            test_text_bytes_match(&runtime, "tmin", "-1073741824"));
  CHECK("text.from-int rejects text arg",
        fr_repl_eval_line(&runtime, "text.from-int: abc", out, sizeof(out)) ==
            FR_ERR_TYPE);
  CHECK("text.from-int round-trips through text.length",
        fr_repl_eval_line(&runtime, "n42 is text.from-int: 42", out,
                          sizeof(out)) == FR_OK &&
            fr_repl_eval_line(&runtime, "text.length: n42", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "2\nok\n") == 0);

  memset(long_bytes, 'a', sizeof(long_bytes));
  CHECK("text.concat rejects joined length over the per-text cap",
        fr_text_install(&runtime, long_bytes, (uint16_t)sizeof(long_bytes),
                        &long_id) == FR_OK &&
            test_text_native_id(&runtime, FR_SLOT_TEXT_CONCAT, &concat_entry) ==
                FR_OK &&
            fr_tagged_encode_object_id(long_id, &concat_args[0]) == FR_OK &&
            (concat_args[1] = concat_args[0], true) &&
            fr_native_call(&runtime, concat_entry, concat_args, 2,
                           &concat_result) == FR_ERR_RANGE);

  while (fr_object_count(&runtime) < FR_PROFILE_OBJECT_TABLE_SIZE) {
    uint8_t fill_buf[2] = {(uint8_t)('A' + (fill_counter / 26)),
                           (uint8_t)('a' + (fill_counter % 26))};
    if (fr_text_install(&runtime, fill_buf, 2, &fill_id) != FR_OK) {
      break;
    }
    fill_counter++;
  }
  CHECK("text.concat hits capacity once the object table is full",
        fr_object_count(&runtime) == FR_PROFILE_OBJECT_TABLE_SIZE &&
            fr_repl_eval_line(&runtime, "text.concat: foo, foo", out,
                              sizeof(out)) == FR_ERR_CAPACITY);
}
#endif

#if FR_FEATURE_RECORDS
static void test_records(void) {
  fr_runtime_t runtime;
  const uint8_t point_name[] = {'P', 'o', 'i', 'n', 't'};
  const uint8_t x_name[] = {'x'};
  const uint8_t y_name[] = {'y'};
  const uint8_t ready_text[] = {'r', 'e', 'a', 'd', 'y'};
  const fr_record_name_t fields[] = {
      {.bytes = x_name, .length = sizeof(x_name)},
      {.bytes = y_name, .length = sizeof(y_name)},
  };
  const fr_record_name_t duplicate_fields[] = {
      {.bytes = x_name, .length = sizeof(x_name)},
      {.bytes = x_name, .length = sizeof(x_name)},
  };
  fr_record_name_t viewed_name = {0};
  fr_record_name_t viewed_field = {0};
  fr_object_id_t shape_id = 0;
  fr_object_id_t record_id = 0;
  fr_object_id_t text_id = 0;
#if FR_FEATURE_CELLS
  fr_object_id_t cells_id = 0;
#endif
  fr_tagged_t one = 0;
  fr_tagged_t two = 0;
  fr_tagged_t tagged = 0;
  fr_tagged_t text_tagged = 0;
  fr_tagged_t shape_tagged = 0;
  fr_tagged_t record_ref_tagged = 0;
#if FR_FEATURE_CELLS
  fr_tagged_t record_tagged = 0;
  fr_tagged_t cell_tagged = 0;
#endif
  fr_tagged_t code_tagged = 0;
#if FR_FEATURE_HANDLES
  fr_handle_ref_t handle_ref = {0};
  fr_tagged_t handle_tagged = 0;
#endif
  fr_tagged_t values[2];
  fr_tagged_t bad_values[2];
  fr_int_t decoded = 0;
  uint16_t field_count = 0;
  uint16_t field_index = 0;

  CHECK("records runtime init", fr_runtime_init(&runtime) == FR_OK);
  CHECK("records encode values",
        fr_tagged_encode_int(1, &one) == FR_OK &&
            fr_tagged_encode_int(2, &two) == FR_OK &&
            fr_tagged_encode_code_object_id(0, &code_tagged) == FR_OK);
#if FR_FEATURE_HANDLES
  CHECK("records reserve handle",
        fr_handle_reserve(&runtime, FR_TEST_SYNTHETIC_HANDLE_KIND, &handle_ref,
                          &handle_tagged) == FR_OK &&
            fr_handle_activate(&runtime, handle_ref, 4) == FR_OK);
#endif
  CHECK("records install shape",
        fr_record_shape_install(
            &runtime,
            (fr_record_name_t){.bytes = point_name,
                               .length = sizeof(point_name)},
            fields, 2, &shape_id) == FR_OK &&
            shape_id == 0 && fr_object_is_overlay(&runtime, shape_id));
  CHECK("records shape view",
        fr_record_shape_view(&runtime, shape_id, &viewed_name,
                             &field_count) == FR_OK &&
            field_count == 2 &&
            viewed_name.length == sizeof(point_name) &&
            memcmp(viewed_name.bytes, point_name, sizeof(point_name)) == 0);
  CHECK("records field name view",
        fr_record_shape_field_name(&runtime, shape_id, 1, &viewed_field) ==
                FR_OK &&
            viewed_field.length == sizeof(y_name) &&
            memcmp(viewed_field.bytes, y_name, sizeof(y_name)) == 0);
  CHECK("records field index",
        fr_record_shape_field_index(
            &runtime, shape_id,
            (fr_record_name_t){.bytes = y_name, .length = sizeof(y_name)},
            &field_index) == FR_OK &&
            field_index == 1);
  CHECK("records reject duplicate field",
        fr_record_shape_install(
            &runtime,
            (fr_record_name_t){.bytes = point_name,
                               .length = sizeof(point_name)},
            duplicate_fields, 2, &record_id) == FR_ERR_INVALID);
  CHECK("records install text field",
        fr_text_install(&runtime, ready_text, (uint16_t)sizeof(ready_text),
                        &text_id) == FR_OK &&
            fr_tagged_encode_object_id(text_id, &text_tagged) == FR_OK);
  values[0] = one;
  values[1] = text_tagged;
  CHECK("records install record",
        fr_record_install(&runtime, shape_id, values, 2, &record_id) ==
                FR_OK &&
            fr_object_is_overlay(&runtime, record_id));
  CHECK("records read int field",
        fr_record_read_field(
            &runtime, record_id,
            (fr_record_name_t){.bytes = x_name, .length = sizeof(x_name)},
            &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("records read text field",
        fr_record_read_field(
            &runtime, record_id,
            (fr_record_name_t){.bytes = y_name, .length = sizeof(y_name)},
            &tagged) == FR_OK &&
            tagged == text_tagged);
  CHECK("records read by index",
        fr_record_read_index(&runtime, record_id, 1, &tagged) == FR_OK &&
            tagged == text_tagged);
  CHECK("records write field",
        fr_record_write_field(
            &runtime, record_id,
            (fr_record_name_t){.bytes = y_name, .length = sizeof(y_name)},
            two) == FR_OK &&
            fr_record_read_field(
                &runtime, record_id,
                (fr_record_name_t){.bytes = y_name,
                                   .length = sizeof(y_name)},
                &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 2 &&
            fr_object_is_overlay(&runtime, record_id));
  CHECK("records reject missing field",
        fr_record_read_field(
            &runtime, record_id,
            (fr_record_name_t){.bytes = (const uint8_t *)"z", .length = 1},
            &tagged) == FR_ERR_NOT_FOUND);
  CHECK("records reject shape refs in fields",
        fr_tagged_encode_object_id(shape_id, &shape_tagged) == FR_OK &&
            fr_record_write_field(
                &runtime, record_id,
                (fr_record_name_t){.bytes = x_name,
                                   .length = sizeof(x_name)},
                shape_tagged) == FR_ERR_TYPE);
  CHECK("records reject record refs in fields",
        fr_tagged_encode_object_id(record_id, &record_ref_tagged) == FR_OK &&
            fr_record_write_field(
                &runtime, record_id,
                (fr_record_name_t){.bytes = x_name,
                                   .length = sizeof(x_name)},
                record_ref_tagged) == FR_ERR_TYPE);
  bad_values[0] = code_tagged;
  bad_values[1] = one;
  CHECK("records reject code field value",
        fr_record_install(&runtime, shape_id, bad_values, 2, &record_id) ==
            FR_ERR_TYPE);
#if FR_FEATURE_HANDLES
  bad_values[0] = handle_tagged;
  bad_values[1] = one;
  CHECK("records reject handle field value",
        fr_record_install(&runtime, shape_id, bad_values, 2, &record_id) ==
                FR_ERR_TYPE &&
            fr_record_write_field(
                &runtime, record_id,
                (fr_record_name_t){.bytes = x_name,
                                   .length = sizeof(x_name)},
                handle_tagged) == FR_ERR_TYPE &&
            fr_handle_close(&runtime, handle_ref) == FR_OK);
#endif
  CHECK("records base restore keeps installed fields",
        (fr_object_mark_base(&runtime), true) &&
            fr_record_write_field(
                &runtime, record_id,
                (fr_record_name_t){.bytes = x_name,
                                   .length = sizeof(x_name)},
                two) == FR_OK &&
            (fr_object_restore_base(&runtime), true) &&
            fr_record_read_field(
                &runtime, record_id,
                (fr_record_name_t){.bytes = x_name,
                                   .length = sizeof(x_name)},
                &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1 &&
            !fr_object_is_overlay(&runtime, record_id));
#if FR_FEATURE_CELLS
  CHECK("cells can hold record refs",
        fr_tagged_encode_object_id(record_id, &record_tagged) == FR_OK &&
            fr_cells_install(&runtime, 1, &record_tagged, &cells_id) ==
                FR_OK &&
            fr_cells_read(&runtime, cells_id, 0, &tagged) == FR_OK &&
            tagged == record_tagged);
  CHECK("records reject cell refs in fields",
        fr_tagged_encode_object_id(cells_id, &cell_tagged) == FR_OK &&
            fr_record_write_field(
                &runtime, record_id,
                (fr_record_name_t){.bytes = x_name,
                                   .length = sizeof(x_name)},
                cell_tagged) == FR_ERR_TYPE);
#endif
#if FR_PROFILE_MAX_RECORD_FIELDS_PER_SHAPE >= 4 &&                            \
    FR_PROFILE_MAX_RECORD_NAME_BYTES >= (FR_PROFILE_MAX_NAME_BYTES * 5)
  {
    uint8_t long_names[5][FR_PROFILE_MAX_NAME_BYTES];
    fr_record_name_t long_fields[4];

    CHECK("records name capacity reset", fr_runtime_init(&runtime) == FR_OK);
    for (uint16_t row = 0; row < 5; row++) {
      memset(long_names[row], (int)('a' + row), sizeof(long_names[row]));
    }
    for (uint16_t i = 0; i < 4; i++) {
      long_fields[i] = (fr_record_name_t){
          .bytes = long_names[i + 1],
          .length = FR_PROFILE_MAX_NAME_BYTES,
      };
    }
    CHECK("records fill name byte budget with long shape",
          fr_record_shape_install(
              &runtime,
              (fr_record_name_t){.bytes = long_names[0],
                                 .length = FR_PROFILE_MAX_NAME_BYTES},
              long_fields, 4, &shape_id) == FR_OK);
    CHECK("records reject exhausted name byte budget",
          fr_record_shape_install(
              &runtime,
              (fr_record_name_t){.bytes = long_names[4],
                                 .length = FR_PROFILE_MAX_NAME_BYTES},
              long_fields, 1, &shape_id) == FR_ERR_CAPACITY);
  }
#endif
}
#endif

static fr_err_t test_native_one(fr_runtime_t *runtime, const fr_tagged_t *args,
                                uint8_t arg_count, fr_tagged_t *out) {
  (void)runtime;
  (void)args;
  (void)arg_count;
  return fr_tagged_encode_int(1, out);
}

static fr_err_t test_native_add(fr_runtime_t *runtime, const fr_tagged_t *args,
                                uint8_t arg_count, fr_tagged_t *out) {
  fr_int_t lhs = 0;
  fr_int_t rhs = 0;

  (void)runtime;
  if (arg_count != 2) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_tagged_decode_int(args[0], &lhs));
  FR_TRY(fr_tagged_decode_int(args[1], &rhs));
  return fr_tagged_encode_int((int32_t)lhs + rhs, out);
}

static fr_err_t test_native_error(fr_runtime_t *runtime,
                                  const fr_tagged_t *args, uint8_t arg_count,
                                  fr_tagged_t *out) {
  (void)runtime;
  (void)args;
  (void)arg_count;
  (void)out;
  return FR_ERR_IO;
}

static fr_err_t test_native_interrupt(fr_runtime_t *runtime,
                                      const fr_tagged_t *args,
                                      uint8_t arg_count, fr_tagged_t *out) {
  (void)args;
  (void)arg_count;
  fr_runtime_interrupt(runtime);
  return fr_tagged_encode_int(1, out);
}

static void test_natives(void) {
  fr_runtime_t runtime;
  const fr_native_entry_t *entry = NULL;
#if FR_FEATURE_NATIVE_SIGNATURES
  const fr_native_param_t add_params[] = {
      {NULL, FR_NATIVE_VALUE_INT},
      {NULL, FR_NATIVE_VALUE_INT},
  };
  const fr_native_signature_t add_signature = {
      .params = add_params,
      .arg_count = 2,
      .result = FR_NATIVE_VALUE_INT,
      .help = NULL,
  };
  const fr_native_signature_t *add_signature_ptr = &add_signature;
#else
  const fr_native_signature_t *add_signature_ptr = NULL;
#endif
  fr_native_id_t native_id = 0;
  fr_native_id_t add_native_id = 0;
  fr_tagged_t tagged = 0;
  fr_tagged_t args[2];
  fr_tagged_t result = 0;

  CHECK("native runtime init", fr_runtime_init(&runtime) == FR_OK);
  CHECK("native add",
        fr_native_install(&runtime, test_native_one, 0, NULL, &native_id) == FR_OK &&
            native_id == 0);
  CHECK("native get", fr_native_get(&runtime, native_id, &entry) == FR_OK &&
                          entry->fn == test_native_one && entry->arity == 0);
  CHECK("native-tagged stores in slot",
        fr_tagged_encode_native_id(native_id, &tagged) == FR_OK &&
            fr_slot_write(&runtime, 1, tagged) == FR_OK &&
            fr_slot_read(&runtime, 1, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            native_id == 0);
  CHECK("native add install",
        fr_native_install(&runtime, test_native_add, 2, add_signature_ptr,
                          &add_native_id) == FR_OK &&
            add_native_id == 1);
  CHECK("native signature call",
        fr_tagged_encode_int(2, &args[0]) == FR_OK &&
            fr_tagged_encode_int(3, &args[1]) == FR_OK &&
            fr_native_get(&runtime, add_native_id, &entry) == FR_OK &&
            fr_native_call(&runtime, entry, args, 2, &result) == FR_OK &&
            fr_tagged_encode_int(5, &tagged) == FR_OK && result == tagged);
  CHECK("native signature rejects arg type",
        (args[0] = fr_tagged_nil(), true) &&
            fr_native_call(&runtime, entry, args, 2, &result) == FR_ERR_TYPE);
  for (uint16_t i = 2; i < FR_PROFILE_NATIVE_TABLE_SIZE; i++) {
    CHECK("native fill",
          fr_native_install(&runtime, test_native_one, 0, NULL, &native_id) == FR_OK);
  }
  CHECK("native table full", fr_native_install(&runtime, test_native_one, 0, NULL,
                                               &native_id) == FR_ERR_CAPACITY);
  CHECK("native rejects null fn",
        fr_native_install(&runtime, NULL, 0, NULL, &native_id) == FR_ERR_INVALID);
  CHECK("native rejects large arity",
        fr_native_install(&runtime, test_native_one,
                          FR_PROFILE_MAX_STACK_DEPTH + 1, NULL,
                          &native_id) == FR_ERR_INVALID);
  CHECK("native rejects null out",
        fr_native_install(&runtime, test_native_one, 0, NULL, NULL) ==
            FR_ERR_INVALID);
}

static void test_event_table(void) {
  fr_runtime_t runtime;
  fr_event_binding_t *slot;

  CHECK("runtime init", fr_runtime_init(&runtime) == FR_OK);
  CHECK("event capacity is sixteen", FR_EVENT_BINDING_COUNT == 16);
  CHECK("overflow starts at zero", runtime.events.overflow_count == 0);
  for (uint16_t i = 0; i < FR_EVENT_BINDING_COUNT; i++) {
    CHECK("entry kind starts none",
          runtime.events.entries[i].kind == FR_EVENT_KIND_NONE);
    CHECK("entry generation starts zero",
          runtime.events.entries[i].generation == 0);
    CHECK("entry pending starts false",
          runtime.events.entries[i].pending == false);
    CHECK("entry timestamps start zero",
          runtime.events.entries[i].registered_at_ms == 0 &&
              runtime.events.entries[i].last_fire_ms == 0);
  }

  slot = &runtime.events.entries[3];
  slot->kind = FR_EVENT_KIND_GPIO_RISING;
  slot->source = 5;
  slot->debounce_ms = 30;
  slot->generation = 42;
  slot->body = 7;
  slot->pending = true;
  slot->registered_at_ms = 12345;
  slot->last_fire_ms = 1000;
  runtime.events.overflow_count = 11;

  CHECK("entry round-trips fields",
        slot->kind == FR_EVENT_KIND_GPIO_RISING && slot->source == 5 &&
            slot->debounce_ms == 30 && slot->generation == 42 &&
            slot->body == 7 && slot->pending == true &&
            slot->registered_at_ms == 12345 && slot->last_fire_ms == 1000);
  CHECK("overflow round-trips", runtime.events.overflow_count == 11);

  CHECK("clear project clears bindings",
        fr_runtime_clear_project(&runtime) == FR_OK);
  CHECK("entry kind cleared", slot->kind == FR_EVENT_KIND_NONE);
  CHECK("entry source cleared", slot->source == 0);
  CHECK("entry debounce cleared", slot->debounce_ms == 0);
  CHECK("entry generation cleared", slot->generation == 0);
  CHECK("entry body cleared", slot->body == 0);
  CHECK("entry pending cleared", slot->pending == false);
  CHECK("entry timestamps cleared",
        slot->registered_at_ms == 0 && slot->last_fire_ms == 0);
  CHECK("overflow cleared", runtime.events.overflow_count == 0);
}

static void test_event_register_cancel(void) {
  fr_runtime_t runtime;
  fr_event_binding_t *entry;

  CHECK("event runtime init", fr_runtime_init(&runtime) == FR_OK);

  CHECK("register gpio rising",
        fr_event_register(&runtime, FR_EVENT_KIND_GPIO_RISING, 5, 30, 7) ==
            FR_OK);
  entry = &runtime.events.entries[0];
  CHECK("register fills kind", entry->kind == FR_EVENT_KIND_GPIO_RISING);
  CHECK("register fills source", entry->source == 5);
  CHECK("register fills debounce", entry->debounce_ms == 30);
  CHECK("register fills body", entry->body == 7);
  CHECK("register starts generation at one", entry->generation == 1);
  CHECK("register pending false", entry->pending == false);
  CHECK("register last fire zero", entry->last_fire_ms == 0);

  CHECK("re-register same pin different edge",
        fr_event_register(&runtime, FR_EVENT_KIND_GPIO_FALLING, 5, 0, 9) ==
            FR_OK);
  CHECK("re-register reuses slot zero",
        runtime.events.entries[0].kind == FR_EVENT_KIND_GPIO_FALLING);
  CHECK("re-register bumps generation",
        runtime.events.entries[0].generation == 2);
  CHECK("re-register replaces body", runtime.events.entries[0].body == 9);
  CHECK("re-register replaces debounce",
        runtime.events.entries[0].debounce_ms == 0);

  CHECK("register every takes next slot",
        fr_event_register(&runtime, FR_EVENT_KIND_EVERY, 100, 0, 11) == FR_OK);
  CHECK("every lands at slot one",
        runtime.events.entries[1].kind == FR_EVENT_KIND_EVERY &&
            runtime.events.entries[1].source == 100);

  CHECK("register after same source as every",
        fr_event_register(&runtime, FR_EVENT_KIND_AFTER, 100, 0, 13) == FR_OK);
  CHECK("after takes a separate slot",
        runtime.events.entries[2].kind == FR_EVENT_KIND_AFTER &&
            runtime.events.entries[2].source == 100);
  CHECK("every survives after registration",
        runtime.events.entries[1].kind == FR_EVENT_KIND_EVERY);

  CHECK("cancel gpio matches any edge on the pin",
        fr_event_cancel(&runtime, FR_EVENT_KIND_GPIO_CHANGES, 5) == FR_OK);
  CHECK("cancel clears kind",
        runtime.events.entries[0].kind == FR_EVENT_KIND_NONE);
  CHECK("cancel bumps generation",
        runtime.events.entries[0].generation == 3);
  CHECK("cancel clears source", runtime.events.entries[0].source == 0);
  CHECK("cancel clears body", runtime.events.entries[0].body == 0);

  CHECK("second cancel returns not found",
        fr_event_cancel(&runtime, FR_EVENT_KIND_GPIO_RISING, 5) ==
            FR_ERR_NOT_FOUND);

  CHECK("cancel every by exact kind",
        fr_event_cancel(&runtime, FR_EVENT_KIND_EVERY, 100) == FR_OK);
  CHECK("every slot cleared",
        runtime.events.entries[1].kind == FR_EVENT_KIND_NONE);
  CHECK("after slot still present",
        runtime.events.entries[2].kind == FR_EVENT_KIND_AFTER);

  CHECK("cancel after by exact kind",
        fr_event_cancel(&runtime, FR_EVENT_KIND_AFTER, 100) == FR_OK);
  CHECK("after slot cleared",
        runtime.events.entries[2].kind == FR_EVENT_KIND_NONE);

  for (uint16_t i = 0; i < FR_EVENT_BINDING_COUNT; i++) {
    CHECK("fill all sixteen slots",
          fr_event_register(&runtime, FR_EVENT_KIND_GPIO_RISING,
                            (uint16_t)(20 + i), 0, 1) == FR_OK);
  }
  CHECK("seventeenth distinct source returns capacity",
        fr_event_register(&runtime, FR_EVENT_KIND_GPIO_RISING, 99, 0, 1) ==
            FR_ERR_CAPACITY);
  CHECK("re-registration under capacity still succeeds",
        fr_event_register(&runtime, FR_EVENT_KIND_GPIO_FALLING, 20, 0, 1) ==
            FR_OK);

  CHECK("clear project at capacity",
        fr_runtime_clear_project(&runtime) == FR_OK);
  for (uint16_t i = 0; i < FR_EVENT_BINDING_COUNT; i++) {
    CHECK("clear leaves no live binding",
          runtime.events.entries[i].kind == FR_EVENT_KIND_NONE);
  }
}

static void test_event_drain_dispatch(void) {
  fr_runtime_t runtime;
  fr_instruction_stream_t view;
  fr_code_object_id_t body_id = 0;
  fr_event_binding_t *gpio_entry = NULL;
  fr_event_binding_t *after_entry = NULL;
  fr_tagged_t slot_value = 0;
  fr_int_t decoded = 0;
  /* Body: push 42, store to FR_TEST_FIRST_USER_SLOT, return. The slot
   * operand sits one byte after the FR_OP_STORE_SLOT op, at offset
   * header + push_int_size + 1. */
  uint8_t body_bytes[] = {0x00, 0x00, FR_TEST_PUSH_INT(42),
                          FR_OP_STORE_SLOT, 0x00, 0x00,
                          FR_OP_RETURN};
  const size_t store_operand_offset =
      FR_INSTRUCTION_MIN_HEADER_SIZE + FR_INSTRUCTION_PUSH_INT_SIZE + 1u;

  write_instruction_header(body_bytes, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_slot_operand(&body_bytes[store_operand_offset],
                     FR_TEST_FIRST_USER_SLOT);

  CHECK("dispatch runtime init", fr_runtime_init(&runtime) == FR_OK);
  CHECK("dispatch view",
        fr_instruction_stream_init(&view, body_bytes, sizeof(body_bytes)) ==
            FR_OK);
  CHECK("install body code",
        fr_code_install(&runtime, &view, NULL, 0, &body_id) == FR_OK);

  CHECK("register gpio rising",
        fr_event_register(&runtime, FR_EVENT_KIND_GPIO_RISING, 0, 0, body_id) ==
            FR_OK);
  gpio_entry = &runtime.events.entries[0];

  /* Stale generation drops without running the body. */
  CHECK("post stale generation",
        fr_platform_event_post_test_candidate(0, 0, 50) == FR_OK);
  CHECK("drain stale", fr_event_drain(&runtime) == FR_OK);
  CHECK("dispatch stale", fr_event_dispatch(&runtime) == FR_OK);
  CHECK("stale leaves last fire zero", gpio_entry->last_fire_ms == 0);
  CHECK("stale leaves slot unset",
        fr_slot_read(&runtime, FR_TEST_FIRST_USER_SLOT, &slot_value) == FR_OK &&
            fr_tagged_decode_int(slot_value, &decoded) != FR_OK);

  /* Fresh candidate fires the body. */
  CHECK("post fresh candidate",
        fr_platform_event_post_test_candidate(0, gpio_entry->generation, 50) ==
            FR_OK);
  CHECK("drain fresh", fr_event_drain(&runtime) == FR_OK);
  CHECK("dispatch fresh", fr_event_dispatch(&runtime) == FR_OK);
  CHECK("body wrote slot",
        fr_slot_read(&runtime, FR_TEST_FIRST_USER_SLOT, &slot_value) == FR_OK &&
            fr_tagged_decode_int(slot_value, &decoded) == FR_OK &&
            decoded == 42);
  CHECK("last fire stamped", gpio_entry->last_fire_ms == 50);
  CHECK("pending cleared after run", gpio_entry->pending == false);
  CHECK("binding still registered after fire",
        gpio_entry->kind == FR_EVENT_KIND_GPIO_RISING);

  /* AFTER binding is removed before its body runs. */
  CHECK("register after",
        fr_event_register(&runtime, FR_EVENT_KIND_AFTER, 1000, 0, body_id) ==
            FR_OK);
  after_entry = &runtime.events.entries[1];
  CHECK("after lands at slot one",
        after_entry->kind == FR_EVENT_KIND_AFTER &&
            after_entry->generation == 1);
  CHECK("post after candidate",
        fr_platform_event_post_test_candidate(1, after_entry->generation,
                                              200) == FR_OK);
  CHECK("drain after", fr_event_drain(&runtime) == FR_OK);
  CHECK("dispatch after", fr_event_dispatch(&runtime) == FR_OK);
  CHECK("after binding cleared on fire",
        after_entry->kind == FR_EVENT_KIND_NONE);
  CHECK("after generation bumped on fire", after_entry->generation == 2);
  CHECK("after slot reverted", after_entry->source == 0 &&
                                   after_entry->body == 0 &&
                                   after_entry->last_fire_ms == 0);
}

static void test_event_register_native(void) {
  fr_runtime_t runtime;
  fr_instruction_stream_t body_view;
  fr_instruction_stream_t call_view;
  fr_code_object_id_t body_id = 0;
  fr_tagged_t result = 0;
  const fr_event_binding_t *entry = NULL;
  uint8_t body_bytes[] = {0x00, 0x00, FR_TEST_PUSH_INT(42),
                          FR_OP_STORE_SLOT, 0x00, 0x00, FR_OP_RETURN};
  uint8_t register_bytes[] = {
      0x00, 0x00,
      FR_TEST_PUSH_INT(1),    /* kind = GPIO_RISING */
      FR_TEST_PUSH_INT(0),    /* source = pin 0 */
      FR_TEST_PUSH_INT(0),    /* debounce = 0 */
      FR_TEST_PUSH_INT(0),    /* body = code id, patched after install */
      FR_OP_CALL_NATIVE_SLOT, 0x00, 0x00,
      FR_OP_RETURN};
  const size_t store_operand_offset =
      FR_INSTRUCTION_MIN_HEADER_SIZE + FR_INSTRUCTION_PUSH_INT_SIZE + 1u;
  const size_t body_int_offset =
      FR_INSTRUCTION_MIN_HEADER_SIZE + FR_INSTRUCTION_PUSH_INT_SIZE * 3u + 1u;
  const size_t call_slot_offset =
      body_int_offset + FR_INSTRUCTION_INT_OPERAND_BYTES + 1u;

  write_instruction_header(body_bytes, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_slot_operand(&body_bytes[store_operand_offset],
                     FR_TEST_FIRST_USER_SLOT);
  write_instruction_header(register_bytes, FR_INSTRUCTION_MIN_HEADER_SIZE);

  CHECK("event register native base install",
        fr_base_image_install(&runtime) == FR_OK);
  CHECK("event register native body view",
        fr_instruction_stream_init(&body_view, body_bytes,
                                   sizeof(body_bytes)) == FR_OK);
  CHECK("event register native body installs",
        fr_code_install(&runtime, &body_view, NULL, 0, &body_id) == FR_OK);

  /* Low 16 bits carry the code id; higher operand bytes (on 32-bit profiles)
     stay zero from the FR_TEST_PUSH_INT(0) placeholder. */
  write_u16_little_endian(&register_bytes[body_int_offset], (uint16_t)body_id);
  write_u16_little_endian(&register_bytes[call_slot_offset],
                          FR_SLOT_EVENT_REGISTER);

  CHECK("event register native runs",
        fr_instruction_stream_init(&call_view, register_bytes,
                                   sizeof(register_bytes)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &call_view, &result) ==
                FR_OK &&
            fr_tagged_is_nil(result));

  entry = &runtime.events.entries[0];
  CHECK("event register native installed binding",
        entry->kind == FR_EVENT_KIND_GPIO_RISING && entry->source == 0 &&
            entry->debounce_ms == 0 && entry->body == body_id &&
            entry->generation == 1 && !entry->pending);
}

#if FR_FEATURE_COMPILER
static void test_event_compile_on_form(void) {
  fr_runtime_t runtime;
  fr_compile_overlay_update_t update;
  fr_tagged_t result = 0;
  const fr_event_binding_t *entry = NULL;
  fr_code_object_id_t expected_body_id = 0;

  CHECK("compile on base image",
        fr_base_image_install(&runtime) == FR_OK);
  expected_body_id = runtime.code.count + 1u;
  CHECK("compile on form",
        fr_compile_overlay_update_for_runtime(
            &runtime, "boot is fn [ on 0 rising [ 1 ] ]", &update) == FR_OK);
  CHECK("compile on emits two code objects",
        update.overlay_update.code_object_count == 2);
  CHECK("compile on apply",
        fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK);
  CHECK("compile on runs boot",
        fr_vm_run_boot(&runtime, &result) == FR_OK && fr_tagged_is_nil(result));
  entry = &runtime.events.entries[0];
  CHECK("compile on installed binding",
        entry->kind == FR_EVENT_KIND_GPIO_RISING && entry->source == 0 &&
            entry->debounce_ms == 0 && entry->body == expected_body_id &&
            entry->generation == 1 && !entry->pending);

  CHECK("compile on debounce base image",
        fr_base_image_install(&runtime) == FR_OK);
  CHECK("compile on debounce form",
        fr_compile_overlay_update_for_runtime(
            &runtime, "boot is fn [ on 0 falling debounce 30 [ 1 ] ]",
            &update) == FR_OK);
  CHECK("compile on debounce apply",
        fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK);
  CHECK("compile on debounce runs",
        fr_vm_run_boot(&runtime, &result) == FR_OK);
  entry = &runtime.events.entries[0];
  CHECK("compile on debounce binding",
        entry->kind == FR_EVENT_KIND_GPIO_FALLING && entry->source == 0 &&
            entry->debounce_ms == 30);

  {
    fr_instruction_stream_t body_view = {NULL, 0};
    fr_instruction_header_t body_header = {0};

    CHECK("compile on with here base image",
          fr_base_image_install(&runtime) == FR_OK);
    CHECK("compile on with here form",
          fr_compile_overlay_update_for_runtime(
              &runtime, "boot is fn [ on 0 rising [ here y is 7 ; y ] ]",
              &update) == FR_OK);
    CHECK("compile on with here apply",
          fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK);
    CHECK("compile on with here runs",
          fr_vm_run_boot(&runtime, &result) == FR_OK);
    entry = &runtime.events.entries[0];
    CHECK("compile on with here body view",
          fr_code_get_instructions(&runtime, entry->body, &body_view) == FR_OK);
    CHECK("compile on with here body header",
          fr_instruction_read_header(&body_view, &body_header) == FR_OK &&
              body_header.header_size == FR_INSTRUCTION_LOCALS_HEADER_SIZE &&
              body_header.arity == 0 && body_header.local_count == 1);
  }
}
#endif

#if FR_INCLUDE_TEST_NATIVES && FR_FEATURE_TEXT
static void test_event_fire_event_native(void) {
  fr_runtime_t runtime;
  fr_instruction_stream_t view;
  fr_code_object_id_t body_id = 0;
  fr_tagged_t slot_value = 0;
  fr_int_t decoded = 0;
  char out[64];
  uint8_t body_bytes[] = {0x00, 0x00, FR_TEST_PUSH_INT(42),
                          FR_OP_STORE_SLOT, 0x00, 0x00,
                          FR_OP_RETURN};
  const size_t store_operand_offset =
      FR_INSTRUCTION_MIN_HEADER_SIZE + FR_INSTRUCTION_PUSH_INT_SIZE + 1u;

  write_instruction_header(body_bytes, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_slot_operand(&body_bytes[store_operand_offset],
                     FR_TEST_FIRST_USER_SLOT);

  CHECK("fire-event installs base image",
        fr_base_image_install(&runtime) == FR_OK);
  CHECK("fire-event view",
        fr_instruction_stream_init(&view, body_bytes, sizeof(body_bytes)) ==
            FR_OK);
  CHECK("fire-event installs body",
        fr_code_install(&runtime, &view, NULL, 0, &body_id) == FR_OK);
  CHECK("fire-event registers gpio rising binding",
        fr_event_register(&runtime, FR_EVENT_KIND_GPIO_RISING, 0, 0, body_id) ==
            FR_OK);

  CHECK("fire-event wrong edge returns not found",
        fr_repl_eval_line(&runtime,
                          "frothy.fire-event: \"on\", 0, \"falling\"", out,
                          sizeof(out)) == FR_ERR_NOT_FOUND);
  CHECK("fire-event rising fires the body",
        fr_repl_eval_line(&runtime, "frothy.fire-event: \"on\", 0, \"rising\"",
                          out, sizeof(out)) == FR_OK);
  CHECK("fire-event body wrote the slot",
        fr_slot_read(&runtime, FR_TEST_FIRST_USER_SLOT, &slot_value) == FR_OK &&
            fr_tagged_decode_int(slot_value, &decoded) == FR_OK &&
            decoded == 42);
}
#endif

#if FR_FEATURE_COMPILER && FR_INCLUDE_TEST_NATIVES && FR_FEATURE_TEXT &&       \
    FR_PROFILE_MAX_OVERLAY_NAMES > 0
static void test_event_compiled_body_fires(void) {
  fr_runtime_t runtime;
  fr_tagged_t boot_result = 0;
  fr_tagged_t slot_value = 0;
  fr_int_t decoded = 0;
  fr_slot_id_t counter_slot = 0;
  char out[64];

  CHECK("compiled body fires base",
        fr_base_image_install(&runtime) == FR_OK);
  CHECK("compiled body fires defines counter",
        fr_repl_eval_line(&runtime, "counter is 0", out, sizeof(out)) ==
            FR_OK);
  CHECK("compiled body fires defines boot",
        fr_repl_eval_line(
            &runtime,
            "boot is fn [ on 0 rising [ set counter to counter + 1 ] ]",
            out, sizeof(out)) == FR_OK);
  CHECK("compiled body fires resolves counter slot",
        fr_slot_id_for_name(&runtime, "counter", &counter_slot) == FR_OK);
  CHECK("compiled body fires runs boot",
        fr_vm_run_boot(&runtime, &boot_result) == FR_OK);
  CHECK("compiled body fires counter starts at zero",
        fr_slot_read(&runtime, counter_slot, &slot_value) == FR_OK &&
            fr_tagged_decode_int(slot_value, &decoded) == FR_OK &&
            decoded == 0);
  CHECK("compiled body fires the event",
        fr_repl_eval_line(&runtime,
                          "frothy.fire-event: \"on\", 0, \"rising\"", out,
                          sizeof(out)) == FR_OK);
  CHECK("compiled body fires incremented counter",
        fr_slot_read(&runtime, counter_slot, &slot_value) == FR_OK &&
            fr_tagged_decode_int(slot_value, &decoded) == FR_OK &&
            decoded == 1);
}
#endif

static void test_code(void) {
  fr_runtime_t runtime;
  fr_instruction_stream_t view;
  fr_instruction_stream_t empty_view = {NULL, 0};
  fr_instruction_stream_t owned_view;
  fr_code_object_id_t code_object_id = 0;
  fr_tagged_t result = 0;
  fr_int_t decoded = 0;
  uint8_t push_one[] = {0x00, 0x00, FR_TEST_PUSH_INT(1), FR_OP_RETURN};
  uint8_t push_two[] = {0x00, 0x00, FR_TEST_PUSH_INT(2), FR_OP_RETURN};

  write_instruction_header(push_one, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(push_two, FR_INSTRUCTION_MIN_HEADER_SIZE);

  CHECK("code runtime init", fr_runtime_init(&runtime) == FR_OK);
  CHECK("code rejects empty instruction stream",
        fr_code_install(&runtime, &empty_view, NULL, 0, &code_object_id) ==
            FR_ERR_INVALID);
  CHECK("view init push one",
        fr_instruction_stream_init(&view, push_one, sizeof(push_one)) == FR_OK);
  CHECK("code install push one",
        fr_code_install(&runtime, &view, NULL, 0, &code_object_id) == FR_OK &&
            code_object_id == 0);
  push_one[3] = 0x02;
  push_one[4] = 0x00;
  CHECK("code owns bytes",
        fr_code_get_instructions(&runtime, code_object_id, &owned_view) ==
                FR_OK &&
            owned_view.bytes != push_one && owned_view.bytes[3] == 0x01 &&
            fr_vm_run_code_object(&runtime, code_object_id, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 1);
  CHECK("second code install",
        fr_instruction_stream_init(&view, push_two, sizeof(push_two)) ==
                FR_OK &&
            fr_code_install(&runtime, &view, NULL, 0, &code_object_id) == FR_OK &&
            code_object_id == 1);
  fr_code_mark_base(&runtime);
  CHECK("code restore keeps base",
        (fr_code_restore_base(&runtime), true) &&
            fr_code_get_instructions(&runtime, 1, &owned_view) == FR_OK);
  CHECK("overlay code install",
        fr_code_install(&runtime, &view, NULL, 0, &code_object_id) == FR_OK &&
            code_object_id == 2);
  CHECK("code restore drops overlay",
        (fr_code_restore_base(&runtime), true) &&
            fr_code_get_instructions(&runtime, 2, &owned_view) ==
                FR_ERR_NOT_FOUND);
  CHECK("code reset clears installs",
        (fr_code_reset(&runtime), true) &&
            fr_code_get_instructions(&runtime, 0, &owned_view) ==
                FR_ERR_NOT_FOUND);
}

static void test_image(void) {
  fr_runtime_t runtime;
  const fr_native_entry_t *entry = NULL;
  fr_base_layer_t layer = FR_BASE_LAYER_CORE;
  fr_tagged_t one = 0;
  fr_tagged_t two = 0;
  fr_tagged_t three = 0;
  fr_tagged_t tagged = 0;
  fr_code_object_id_t code_object_id = 0;
  fr_native_id_t native_id = 0;
  fr_object_id_t object_id = 0;
  fr_slot_id_t slot_id = 0;
  fr_int_t decoded = 0;
  fr_instruction_stream_t view;
  fr_overlay_update_decoded_t decoded_update;
  uint8_t overlay_bytes[128];
  uint16_t overlay_length = 0;
  uint8_t push_one[] = {0x00, 0x00, FR_TEST_PUSH_INT(1), FR_OP_RETURN};
  uint8_t push_two[] = {0x00, 0x00, FR_TEST_PUSH_INT(2), FR_OP_RETURN};
  uint8_t push_three[] = {0x00, 0x00, FR_TEST_PUSH_INT(3), FR_OP_RETURN};
  uint8_t invalid_opcode[] = {0x00, 0x00, 0xfe};

#if !FR_BASE_IMAGE_INCLUDE_SYMBOLS && FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES == 0
  (void)slot_id;
#endif

  write_instruction_header(push_one, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(push_two, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(push_three, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(invalid_opcode, FR_INSTRUCTION_MIN_HEADER_SIZE);

  CHECK("image runtime init", fr_runtime_init(&runtime) == FR_OK);
  CHECK("image encode one", fr_tagged_encode_int(1, &one) == FR_OK);
  CHECK("image encode two", fr_tagged_encode_int(2, &two) == FR_OK);
  CHECK("image encode three", fr_tagged_encode_int(3, &three) == FR_OK);

  const fr_image_slot_init_t image_a_slots[] = {
      {1, {FR_IMAGE_REF_LITERAL_TAGGED, one, 0}},
      {2, {FR_IMAGE_REF_CODE_OBJECT, 0, 0}},
      {3, {FR_IMAGE_REF_NATIVE, 0, 1}},
  };
  const fr_image_code_object_t image_a_code[] = {
      {{push_one, sizeof(push_one)}, NULL, 0},
      {{push_two, sizeof(push_two)}, NULL, 0},
  };
  const fr_image_native_t image_a_natives[] = {
      {.fn = test_native_one, .arity = 0},
      {.fn = test_native_add, .arity = 2},
  };
  const fr_image_t image_a = {
      .slot_inits = image_a_slots,
      .slot_init_count = 3,
      .code_objects = image_a_code,
      .code_object_count = 2,
      .natives = image_a_natives,
      .native_count = 2,
  };

  const fr_image_slot_init_t image_b_slots[] = {
      {1, {FR_IMAGE_REF_LITERAL_TAGGED, three, 0}},
      {2, {FR_IMAGE_REF_CODE_OBJECT, 0, 0}},
      {3, {FR_IMAGE_REF_NATIVE, 0, 0}},
  };
  const fr_image_code_object_t image_b_code[] = {
      {{push_three, sizeof(push_three)}, NULL, 0},
  };
  const fr_image_native_t image_b_natives[] = {
      {.fn = test_native_error, .arity = 0},
  };
  const fr_image_t image_b = {
      .slot_inits = image_b_slots,
      .slot_init_count = 3,
      .code_objects = image_b_code,
      .code_object_count = 1,
      .natives = image_b_natives,
      .native_count = 1,
  };
  const fr_tagged_t image_cell_initial[] = {one};
  const fr_image_slot_init_t image_cell_slots[] = {
      {FR_TEST_FIRST_USER_SLOT, {FR_IMAGE_REF_CELL_OBJECT, 0, 0}},
  };
  const fr_image_cell_object_t image_cell_objects[] = {
      {.length = 1, .initial_values = image_cell_initial},
  };
  const fr_image_t image_cells = {
      .slot_inits = image_cell_slots,
      .slot_init_count = 1,
      .cell_objects = image_cell_objects,
      .cell_object_count = 1,
  };
  const fr_image_slot_init_t update_slots[] = {
      {1, {FR_IMAGE_REF_LITERAL_TAGGED, three, 0}},
      {4, {FR_IMAGE_REF_CODE_OBJECT, 0, 0}},
  };
  const fr_image_code_object_t update_code[] = {
      {{push_three, sizeof(push_three)}, NULL, 0},
  };
  const fr_overlay_update_t overlay_update = {
      .slot_inits = update_slots,
      .slot_init_count = 2,
      .code_objects = update_code,
      .code_object_count = 1,
  };
#if FR_FEATURE_TEXT
  const uint8_t overlay_text_bytes[] = {'o', 'k'};
  const fr_image_slot_init_t text_update_slots[] = {
      {FR_TEST_FIRST_USER_SLOT, {FR_IMAGE_REF_TEXT_OBJECT, 0, 0}},
  };
  const fr_image_text_object_t text_update_objects[] = {
      {.bytes = overlay_text_bytes, .length = sizeof(overlay_text_bytes)},
  };
  const fr_overlay_update_t text_overlay_update = {
      .slot_inits = text_update_slots,
      .slot_init_count = 1,
      .text_objects = text_update_objects,
      .text_object_count = 1,
  };
#endif
#if FR_FEATURE_RECORDS
  const uint8_t point_record_name[] = {'P', 'o', 'i', 'n', 't'};
  const uint8_t x_record_name[] = {'x'};
  const uint8_t y_record_name[] = {'y'};
  const fr_record_name_t point_record_fields[] = {
      {.bytes = x_record_name, .length = sizeof(x_record_name)},
      {.bytes = y_record_name, .length = sizeof(y_record_name)},
  };
  const fr_image_ref_t point_record_values[] = {
      {FR_IMAGE_REF_LITERAL_TAGGED, one, 0},
      {FR_IMAGE_REF_LITERAL_TAGGED, two, 0},
  };
  const fr_image_record_shape_object_t point_record_shapes[] = {
      {.name = {.bytes = point_record_name,
                .length = sizeof(point_record_name)},
       .fields = point_record_fields,
       .field_count = 2},
  };
  const fr_image_record_object_t point_record_objects[] = {
      {.shape = {FR_IMAGE_REF_RECORD_SHAPE_OBJECT, 0, 0},
       .field_values = point_record_values,
       .field_count = 2},
  };
  const fr_image_slot_init_t record_update_slots[] = {
      {FR_TEST_FIRST_USER_SLOT, {FR_IMAGE_REF_RECORD_OBJECT, 0, 0}},
  };
  const fr_overlay_update_t record_overlay_update = {
      .slot_inits = record_update_slots,
      .slot_init_count = 1,
      .record_shape_objects = point_record_shapes,
      .record_shape_object_count = 1,
      .record_objects = point_record_objects,
      .record_object_count = 1,
  };
#endif
  const fr_image_slot_init_t invalid_code_slots[] = {
      {FR_TEST_FIRST_USER_SLOT, {FR_IMAGE_REF_CODE_OBJECT, 0, 0}},
  };
  const fr_image_code_object_t invalid_code_update_code[] = {
      {{invalid_opcode, sizeof(invalid_opcode)}, NULL, 0},
  };
  const fr_overlay_update_t invalid_code_update = {
      .slot_inits = invalid_code_slots,
      .slot_init_count = 1,
      .code_objects = invalid_code_update_code,
      .code_object_count = 1,
  };
#if FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES > 0
  const fr_image_slot_init_t named_update_slots[] = {
      {FR_TEST_FIRST_USER_SLOT, {FR_IMAGE_REF_CODE_OBJECT, 0, 0}},
  };
  const fr_slot_name_t update_names[] = {
      {FR_TEST_FIRST_USER_SLOT, "three"},
  };
  const fr_overlay_update_t named_overlay_update = {
      .slot_inits = named_update_slots,
      .slot_init_count = 1,
      .code_objects = update_code,
      .code_object_count = 1,
      .slot_names = update_names,
      .slot_name_count = 1,
  };
  const fr_slot_name_t orphan_slot_name[] = {
      {4, "orphan"},
  };
  const fr_overlay_update_t orphan_name_update = {
      .code_objects = update_code,
      .code_object_count = 1,
      .slot_names = orphan_slot_name,
      .slot_name_count = 1,
  };
#endif

  CHECK("image installs base state",
        fr_image_install(&runtime, &image_a) == FR_OK &&
            fr_slot_read(&runtime, 1, &tagged) == FR_OK && tagged == one &&
            fr_slot_read(&runtime, 2, &tagged) == FR_OK &&
            fr_tagged_decode_code_object_id(tagged, &code_object_id) == FR_OK &&
            code_object_id == 0 &&
            fr_slot_read(&runtime, 3, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            native_id == 1);
  CHECK("image code slot executes",
        fr_vm_run_slot(&runtime, 2, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
#if FR_FEATURE_CELLS
  CHECK("image installs base cell object",
        fr_image_install(&runtime, &image_cells) == FR_OK &&
            fr_slot_read(&runtime, FR_TEST_FIRST_USER_SLOT, &tagged) ==
                FR_OK &&
            fr_tagged_decode_object_id(tagged, &object_id) == FR_OK &&
            object_id == 0 &&
            fr_cells_read(&runtime, object_id, 0, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1 &&
            !fr_object_is_overlay(&runtime, object_id));
  CHECK("image reset restores base cell value",
        fr_cells_write(&runtime, object_id, 0, two) == FR_OK &&
            fr_runtime_reset(&runtime) == FR_OK &&
            fr_cells_read(&runtime, object_id, 0, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1 &&
            fr_image_install(&runtime, &image_a) == FR_OK);
#else
  (void)object_id;
  CHECK("image rejects cells without feature",
        fr_image_install(&runtime, &image_cells) == FR_ERR_UNSUPPORTED);
#endif
#if FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES > 0
  CHECK("overlay rejects name without slot write before mutation",
        fr_overlay_apply(&runtime, &orphan_name_update) == FR_ERR_INVALID &&
            fr_code_get_instructions(&runtime, 2, &view) == FR_ERR_NOT_FOUND);
#endif
  CHECK("image overlay reset restores base",
        fr_slot_write(&runtime, 1, two) == FR_OK &&
            fr_runtime_reset(&runtime) == FR_OK &&
            fr_slot_read(&runtime, 1, &tagged) == FR_OK && tagged == one);
  CHECK("overlay apply preserves existing records",
        fr_overlay_apply(&runtime, &overlay_update) == FR_OK &&
            fr_slot_read(&runtime, 1, &tagged) == FR_OK && tagged == three &&
            fr_slot_is_overlay(&runtime, 1) &&
            fr_slot_read(&runtime, 3, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            native_id == 1 && fr_vm_run_slot(&runtime, 2, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1 &&
            fr_vm_run_slot(&runtime, 4, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 3);
  CHECK("overlay update byte codec checks crc",
        fr_overlay_update_encode(&overlay_update, overlay_bytes,
                                 (uint16_t)sizeof(overlay_bytes),
                                 &overlay_length) == FR_OK &&
            overlay_length > 4 &&
            fr_overlay_update_decode(overlay_bytes, overlay_length,
                                     &decoded_update) == FR_OK &&
            ((overlay_bytes[5] ^= 0x01u), true) &&
            fr_overlay_update_decode(overlay_bytes, overlay_length,
                                     &decoded_update) == FR_ERR_CORRUPT);
#if FR_FEATURE_TEXT
  CHECK("overlay update byte codec rejects text-object slot inits",
        fr_overlay_update_encode(&text_overlay_update, overlay_bytes,
                                 (uint16_t)sizeof(overlay_bytes),
                                 &overlay_length) == FR_ERR_UNSUPPORTED);
#endif
#if FR_FEATURE_RECORDS
  CHECK("overlay installs record objects",
        fr_overlay_apply(&runtime, &record_overlay_update) == FR_OK &&
            fr_slot_read(&runtime, FR_TEST_FIRST_USER_SLOT, &tagged) ==
                FR_OK &&
            fr_tagged_decode_object_id(tagged, &object_id) == FR_OK &&
            fr_record_read_field(
                &runtime, object_id,
                (fr_record_name_t){.bytes = x_record_name,
                                   .length = sizeof(x_record_name)},
                &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("overlay update byte codec rejects record objects",
        fr_overlay_update_encode(&record_overlay_update, overlay_bytes,
                                 (uint16_t)sizeof(overlay_bytes),
                                 &overlay_length) == FR_ERR_UNSUPPORTED);
#endif
  CHECK("overlay update decode rejects bad magic",
        fr_overlay_update_encode(&overlay_update, overlay_bytes,
                                 (uint16_t)sizeof(overlay_bytes),
                                 &overlay_length) == FR_OK &&
            ((overlay_bytes[0] ^= 0x01u), write_overlay_crc(overlay_bytes,
                                                            overlay_length),
             true) &&
            fr_overlay_update_decode(overlay_bytes, overlay_length,
                                     &decoded_update) == FR_ERR_CORRUPT);
  CHECK("overlay update decode rejects bad version",
        fr_overlay_update_encode(&overlay_update, overlay_bytes,
                                 (uint16_t)sizeof(overlay_bytes),
                                 &overlay_length) == FR_OK &&
            ((overlay_bytes[4] =
                  FR_PROFILE_OVERLAY_UPDATE_VERSION == 1 ? 2u : 1u),
             write_overlay_crc(overlay_bytes, overlay_length),
             true) &&
            fr_overlay_update_decode(overlay_bytes, overlay_length,
                                     &decoded_update) == FR_ERR_CORRUPT);
  CHECK("overlay update decode rejects truncated crc",
        fr_overlay_update_encode(&overlay_update, overlay_bytes,
                                 (uint16_t)sizeof(overlay_bytes),
                                 &overlay_length) == FR_OK &&
            fr_overlay_update_decode(overlay_bytes, (uint16_t)(overlay_length - 1u),
                                     &decoded_update) == FR_ERR_CORRUPT);
  CHECK("crc-valid invalid code reaches runtime guard",
        fr_overlay_update_encode(&invalid_code_update, overlay_bytes,
                                 (uint16_t)sizeof(overlay_bytes),
                                 &overlay_length) == FR_OK &&
            fr_overlay_update_decode(overlay_bytes, overlay_length,
                                     &decoded_update) == FR_OK &&
            fr_image_install(&runtime, &image_a) == FR_OK &&
            fr_overlay_apply(&runtime, &decoded_update.update) == FR_OK &&
            fr_vm_run_slot(&runtime, FR_TEST_FIRST_USER_SLOT, &tagged) ==
                FR_ERR_INVALID);
#if FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES > 0
  CHECK("overlay update byte codec applies",
        fr_overlay_update_encode(&named_overlay_update, overlay_bytes,
                                 (uint16_t)sizeof(overlay_bytes),
                                 &overlay_length) == FR_OK &&
            overlay_length > 0 &&
            fr_overlay_update_decode(overlay_bytes, overlay_length,
                                     &decoded_update) == FR_OK &&
            fr_image_install(&runtime, &image_a) == FR_OK &&
            fr_overlay_apply(&runtime, &decoded_update.update) == FR_OK &&
            fr_slot_id_for_name(&runtime, "three", &slot_id) == FR_OK &&
            slot_id == FR_TEST_FIRST_USER_SLOT &&
            fr_vm_run_slot(&runtime, slot_id, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 3);
  CHECK("overlay update encode checks capacity",
        fr_overlay_update_encode(&named_overlay_update, overlay_bytes, 4,
                                 &overlay_length) == FR_ERR_CAPACITY);
#endif
  CHECK("overlay update decode rejects corrupt bytes",
        fr_overlay_update_decode(overlay_bytes, 4, &decoded_update) ==
            FR_ERR_CORRUPT);
  CHECK("runtime reset discards overlay update",
        fr_runtime_reset(&runtime) == FR_OK &&
            fr_slot_read(&runtime, 1, &tagged) == FR_OK && tagged == one &&
            !fr_slot_is_overlay(&runtime, 1) &&
            fr_slot_read(&runtime, 4, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged) &&
            fr_code_get_instructions(&runtime, 2, &view) == FR_ERR_NOT_FOUND);
  CHECK("base image installs boot",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_slot_read(&runtime, FR_SLOT_BOOT, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("base image installs ms native",
        fr_slot_read(&runtime, FR_SLOT_MS, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            fr_native_get(&runtime, native_id, &entry) == FR_OK &&
            entry->arity == 1);
  CHECK("base image installs gpio.write native",
        fr_slot_read(&runtime, FR_SLOT_GPIO_WRITE, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            fr_native_get(&runtime, native_id, &entry) == FR_OK &&
            entry->arity == 2);
  CHECK("base image installs gpio.mode native",
        fr_slot_read(&runtime, FR_SLOT_GPIO_MODE, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            fr_native_get(&runtime, native_id, &entry) == FR_OK &&
            entry->arity == 2);
  CHECK("base image installs gpio.read native",
        fr_slot_read(&runtime, FR_SLOT_GPIO_READ, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            fr_native_get(&runtime, native_id, &entry) == FR_OK &&
            entry->arity == 1);
  CHECK("base image installs adc.read native",
        fr_slot_read(&runtime, FR_SLOT_ADC_READ, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            fr_native_get(&runtime, native_id, &entry) == FR_OK &&
            entry->arity == 1);
  CHECK("base image installs adc.above native",
        fr_slot_read(&runtime, FR_SLOT_ADC_ABOVE, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            fr_native_get(&runtime, native_id, &entry) == FR_OK &&
            entry->arity == 2);
  CHECK("base image installs millis native",
        fr_slot_read(&runtime, FR_SLOT_MILLIS, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            fr_native_get(&runtime, native_id, &entry) == FR_OK &&
            entry->arity == 0);
#if FR_FEATURE_UART
  CHECK("base image installs uart.open native",
        fr_slot_read(&runtime, FR_SLOT_UART_OPEN, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            fr_native_get(&runtime, native_id, &entry) == FR_OK &&
            entry->arity == 2);
  CHECK("base image installs uart.write-byte native",
        fr_slot_read(&runtime, FR_SLOT_UART_WRITE_BYTE, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            fr_native_get(&runtime, native_id, &entry) == FR_OK &&
            entry->arity == 2);
  CHECK("base image installs uart.read-byte native",
        fr_slot_read(&runtime, FR_SLOT_UART_READ_BYTE, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            fr_native_get(&runtime, native_id, &entry) == FR_OK &&
            entry->arity == 1);
  CHECK("base image installs uart.available native",
        fr_slot_read(&runtime, FR_SLOT_UART_AVAILABLE, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            fr_native_get(&runtime, native_id, &entry) == FR_OK &&
            entry->arity == 1);
  CHECK("base image installs uart.close native",
        fr_slot_read(&runtime, FR_SLOT_UART_CLOSE, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            fr_native_get(&runtime, native_id, &entry) == FR_OK &&
            entry->arity == 1);
  CHECK("base image installs baud constants",
        fr_slot_read(&runtime, FR_SLOT_BAUD_9600, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK &&
            decoded == FR_UART_RATE_9600 &&
            fr_slot_read(&runtime, FR_SLOT_BAUD_115200, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK &&
            decoded == FR_UART_RATE_115200);
#endif
  CHECK("base image installs led builtin constant",
        fr_slot_read(&runtime, FR_SLOT_LED_BUILTIN, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 13);
  CHECK("base image installs one tagged word",
        fr_slot_read(&runtime, FR_SLOT_ONE, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  CHECK("base image exposes host slot names",
        fr_base_slot_count() == FR_TEST_BASE_SLOT_COUNT &&
            strcmp(fr_base_slot_name_at(0), "boot") == 0 &&
            strcmp(fr_base_slot_name_at(1), "ms") == 0 &&
            strcmp(fr_base_slot_name_at(2), "one") == 0 &&
            strcmp(fr_base_slot_name_at(3), "gpio.write") == 0 &&
            strcmp(fr_base_slot_name_at(4), "$led_builtin") == 0 &&
            fr_base_slot_name_at(FR_TEST_BASE_SLOT_COUNT) == NULL &&
            strcmp(fr_base_slot_name(FR_SLOT_BOOT), "boot") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_MS), "ms") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_ONE), "one") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_GPIO_WRITE), "gpio.write") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_GPIO_MODE), "gpio.mode") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_GPIO_READ), "gpio.read") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_ADC_READ), "adc.read") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_ADC_ABOVE), "adc.above") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_MILLIS), "millis") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_LED_BUILTIN), "$led_builtin") ==
                0);
#if FR_FEATURE_SOURCE_BASE
  CHECK("base image names source word at board-local base",
        strcmp(fr_base_slot_name(FR_SLOT_BOARD_LOCAL_BASE), "gpio.high") == 0);
#else
  CHECK("base image leaves board-local base unnamed",
        fr_base_slot_name(FR_SLOT_BOARD_LOCAL_BASE) == NULL);
#endif
#if FR_FEATURE_UART
  CHECK("base image exposes uart slot names",
        strcmp(fr_base_slot_name_at(13), "uart.open") == 0 &&
            strcmp(fr_base_slot_name_at(14), "uart.open-on") == 0 &&
            strcmp(fr_base_slot_name_at(15), "uart.write-byte") == 0 &&
            strcmp(fr_base_slot_name_at(16), "uart.read-byte") == 0 &&
            strcmp(fr_base_slot_name_at(17), "uart.available") == 0 &&
            strcmp(fr_base_slot_name_at(18), "uart.close") == 0 &&
            strcmp(fr_base_slot_name_at(19), "$baud_9600") == 0 &&
            strcmp(fr_base_slot_name_at(20), "$baud_19200") == 0 &&
            strcmp(fr_base_slot_name_at(21), "$baud_38400") == 0 &&
            strcmp(fr_base_slot_name_at(22), "$baud_57600") == 0 &&
            strcmp(fr_base_slot_name_at(23), "$baud_115200") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_UART_OPEN), "uart.open") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_UART_OPEN_ON),
                   "uart.open-on") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_UART_WRITE_BYTE),
                   "uart.write-byte") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_UART_READ_BYTE),
                   "uart.read-byte") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_UART_AVAILABLE),
                   "uart.available") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_UART_CLOSE), "uart.close") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_BAUD_9600), "$baud_9600") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_BAUD_115200),
                   "$baud_115200") == 0);
#endif
#if FR_FEATURE_PERSISTENCE
  CHECK("base image exposes persistence slot names",
        strcmp(fr_base_slot_name_at(5), "save") == 0 &&
            strcmp(fr_base_slot_name_at(6), "restore") == 0 &&
            strcmp(fr_base_slot_name_at(7), "dangerous.wipe") == 0 &&
            strcmp(fr_base_slot_name_at(8), "gpio.mode") == 0 &&
            strcmp(fr_base_slot_name_at(9), "gpio.read") == 0 &&
            strcmp(fr_base_slot_name_at(10), "adc.read") == 0 &&
            strcmp(fr_base_slot_name_at(11), "adc.above") == 0 &&
            strcmp(fr_base_slot_name_at(12), "millis") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_SAVE), "save") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_RESTORE), "restore") == 0 &&
            strcmp(fr_base_slot_name(FR_SLOT_WIPE), "dangerous.wipe") == 0);
#else
  CHECK("base image exposes glue slot names without persistence",
        fr_base_slot_name(FR_SLOT_SAVE) == NULL &&
            fr_base_slot_name(FR_SLOT_RESTORE) == NULL &&
            fr_base_slot_name(FR_SLOT_WIPE) == NULL &&
            strcmp(fr_base_slot_name_at(5), "gpio.mode") == 0 &&
            strcmp(fr_base_slot_name_at(6), "gpio.read") == 0 &&
            strcmp(fr_base_slot_name_at(7), "adc.read") == 0 &&
            strcmp(fr_base_slot_name_at(8), "adc.above") == 0 &&
            strcmp(fr_base_slot_name_at(9), "millis") == 0);
#endif
  CHECK("base image looks up host slot names",
        fr_base_slot_id_for_name("boot", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_BOOT &&
            fr_base_slot_id_for_name("ms", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_MS &&
            fr_base_slot_id_for_name("one", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_ONE &&
            fr_base_slot_id_for_name("gpio.write", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_GPIO_WRITE &&
            fr_base_slot_id_for_name("pin", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_GPIO_WRITE &&
            fr_base_slot_id_for_name("gpio.mode", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_GPIO_MODE &&
            fr_base_slot_id_for_name("gpio.read", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_GPIO_READ &&
            fr_base_slot_id_for_name("adc.read", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_ADC_READ &&
            fr_base_slot_id_for_name("adc.above", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_ADC_ABOVE &&
            fr_base_slot_id_for_name("millis", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_MILLIS &&
            fr_base_slot_id_for_name("$led_builtin", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_LED_BUILTIN &&
            fr_base_slot_id_for_name("missing", &slot_id) == FR_ERR_NOT_FOUND);
#if FR_FEATURE_UART
  CHECK("base image looks up uart slot names",
        fr_base_slot_id_for_name("uart.open", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_UART_OPEN &&
            fr_base_slot_id_for_name("uart.write-byte", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_UART_WRITE_BYTE &&
            fr_base_slot_id_for_name("uart.read-byte", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_UART_READ_BYTE &&
            fr_base_slot_id_for_name("uart.available", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_UART_AVAILABLE &&
            fr_base_slot_id_for_name("uart.close", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_UART_CLOSE &&
            fr_base_slot_id_for_name("$baud_9600", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_BAUD_9600 &&
            fr_base_slot_id_for_name("$baud_115200", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_BAUD_115200);
#endif
#if FR_FEATURE_PERSISTENCE
  CHECK("base image looks up persistence slot names",
            fr_base_slot_id_for_name("save", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_SAVE &&
            fr_base_slot_id_for_name("restore", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_RESTORE &&
            fr_base_slot_id_for_name("dangerous.wipe", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_WIPE &&
            fr_base_slot_id_for_name("wipe", &slot_id) == FR_ERR_NOT_FOUND);
#else
  CHECK("base image rejects persistence slot names",
        fr_base_slot_id_for_name("save", &slot_id) == FR_ERR_NOT_FOUND &&
            fr_base_slot_id_for_name("restore", &slot_id) == FR_ERR_NOT_FOUND &&
            fr_base_slot_id_for_name("dangerous.wipe", &slot_id) ==
                FR_ERR_NOT_FOUND &&
            fr_base_slot_id_for_name("wipe", &slot_id) == FR_ERR_NOT_FOUND);
#endif
#else
  CHECK("base image omits symbols",
        fr_base_slot_count() == 0 && fr_base_slot_name_at(0) == NULL &&
            fr_base_slot_name(FR_SLOT_BOOT) == NULL &&
            fr_base_slot_id_for_name("boot", &slot_id) == FR_ERR_UNSUPPORTED);
#endif
  CHECK("base image records native layer",
        fr_base_slot_layer(FR_SLOT_MS, &layer) == FR_OK &&
            layer == FR_BASE_LAYER_TARGET &&
            fr_base_slot_layer(FR_SLOT_GPIO_WRITE, &layer) == FR_OK &&
            layer == FR_BASE_LAYER_TARGET &&
            fr_base_slot_layer(FR_SLOT_GPIO_MODE, &layer) == FR_OK &&
            layer == FR_BASE_LAYER_TARGET &&
            fr_base_slot_layer(FR_SLOT_GPIO_READ, &layer) == FR_OK &&
            layer == FR_BASE_LAYER_TARGET &&
            fr_base_slot_layer(FR_SLOT_ADC_READ, &layer) == FR_OK &&
            layer == FR_BASE_LAYER_TARGET &&
            fr_base_slot_layer(FR_SLOT_ADC_ABOVE, &layer) == FR_OK &&
            layer == FR_BASE_LAYER_TARGET &&
            fr_base_slot_layer(FR_SLOT_MILLIS, &layer) == FR_OK &&
            layer == FR_BASE_LAYER_TARGET &&
            fr_base_slot_layer(FR_SLOT_LED_BUILTIN, &layer) == FR_OK &&
            layer == FR_BASE_LAYER_BOARD &&
            fr_base_slot_layer(FR_SLOT_ONE, &layer) == FR_OK &&
            layer == FR_BASE_LAYER_CORE);
#if FR_FEATURE_PERSISTENCE
  CHECK("base image records persistence layer",
        fr_base_slot_layer(FR_SLOT_SAVE, &layer) == FR_OK &&
            layer == FR_BASE_LAYER_PERSISTENCE &&
            fr_base_slot_layer(FR_SLOT_RESTORE, &layer) == FR_OK &&
            layer == FR_BASE_LAYER_PERSISTENCE &&
            fr_base_slot_layer(FR_SLOT_WIPE, &layer) == FR_OK &&
            layer == FR_BASE_LAYER_PERSISTENCE);
#else
  CHECK("base image has no persistence layer",
        fr_base_slot_layer(FR_SLOT_SAVE, &layer) == FR_ERR_NOT_FOUND &&
            fr_base_slot_layer(FR_SLOT_RESTORE, &layer) == FR_ERR_NOT_FOUND &&
            fr_base_slot_layer(FR_SLOT_WIPE, &layer) == FR_ERR_NOT_FOUND);
#endif
#if FR_FEATURE_SOURCE_BASE
  fr_base_source_record_reset();
  CHECK("source slot absent before registration",
        fr_base_slot_layer(FR_SLOT_BOARD_LOCAL_BASE, &layer) == FR_ERR_NOT_FOUND);
  CHECK("source record reports source layer and name",
        fr_base_source_record_add(FR_SLOT_BOARD_LOCAL_BASE, "gpio.high") ==
                FR_OK &&
            fr_base_slot_layer(FR_SLOT_BOARD_LOCAL_BASE, &layer) == FR_OK &&
            layer == FR_BASE_LAYER_SOURCE &&
            fr_base_slot_name(FR_SLOT_BOARD_LOCAL_BASE) != NULL &&
            strcmp(fr_base_slot_name(FR_SLOT_BOARD_LOCAL_BASE), "gpio.high") ==
                0);
  fr_base_source_record_reset();
  CHECK("source record clears on reset",
        fr_base_slot_layer(FR_SLOT_BOARD_LOCAL_BASE, &layer) ==
                FR_ERR_NOT_FOUND &&
            fr_base_slot_name(FR_SLOT_BOARD_LOCAL_BASE) == NULL);
  CHECK("baked source bytes carry base/core.frothy",
        fr_source_base_bytes_len > 0 &&
            memcmp(fr_source_base_bytes, "to gpio.high", 12) == 0);
  /* After boot compile, gpio.high resolves by name, sits past the fixed base
     slot range, reports the source layer, and pushes user words past it. */
  CHECK("boot compile binds gpio.high as a source-layer base word",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_slot_id_for_name(&runtime, "gpio.high", &slot_id) == FR_OK &&
            slot_id >= FR_SLOT_BOARD_LOCAL_BASE &&
            fr_base_slot_layer(slot_id, &layer) == FR_OK &&
            layer == FR_BASE_LAYER_SOURCE &&
            fr_slot_first_project_id() > slot_id);
#endif
  CHECK("image replacement clears old code and natives",
        fr_image_install(&runtime, &image_a) == FR_OK &&
            fr_image_install(&runtime, &image_b) == FR_OK &&
            fr_code_get_instructions(&runtime, 1, &view) == FR_ERR_NOT_FOUND &&
            fr_native_get(&runtime, 1, &entry) == FR_ERR_NOT_FOUND);
  CHECK("image replacement installs new base",
        fr_slot_read(&runtime, 1, &tagged) == FR_OK && tagged == three &&
            fr_vm_run_slot(&runtime, 2, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 3 &&
            fr_runtime_reset(&runtime) == FR_OK &&
            fr_slot_read(&runtime, 1, &tagged) == FR_OK && tagged == three);
  CHECK(
      "image rejects bad code ref",
      fr_image_install(&runtime, &(const fr_image_t){
                                     .slot_inits =
                                         (const fr_image_slot_init_t[]){
                                         {5, {FR_IMAGE_REF_CODE_OBJECT, 0, 1}},
                                     },
                                     .slot_init_count = 1,
                                     .code_objects = image_b_code,
                                     .code_object_count = 1,
                                 }) == FR_ERR_NOT_FOUND);
  CHECK("image rejects bad native ref",
        fr_image_install(&runtime, &(const fr_image_t){
                                       .slot_inits =
                                           (const fr_image_slot_init_t[]){
                                           {5, {FR_IMAGE_REF_NATIVE, 0, 1}},
                                       },
                                       .slot_init_count = 1,
                                       .natives = image_b_natives,
                                       .native_count = 1,
                                   }) == FR_ERR_NOT_FOUND);
  CHECK("base image rejects slot-name records",
        fr_image_install(&runtime, &(const fr_image_t){
                                       .slot_names =
                                           (const fr_slot_name_t[]){
                                               {4, "orphan"},
                                           },
                                       .slot_name_count = 1,
                                   }) == FR_ERR_INVALID);
}

static void test_public_surface(void) {
  fr_runtime_t runtime;
  fr_tagged_t tagged = 0;

  CHECK("public surface init", fr_runtime_init(&runtime) == FR_OK);
  CHECK("public surface tagged encode",
        fr_tagged_encode_int(1, &tagged) == FR_OK);
  CHECK("public surface slot write",
        fr_slot_write(&runtime, 0, tagged) == FR_OK);
  CHECK("public surface slot read",
        fr_slot_read(&runtime, 0, &tagged) == FR_OK &&
            !fr_tagged_is_nil(tagged));
}

#if FR_FEATURE_COMPILER
static void test_parse(void) {
  fr_parse_line_t parsed;
  fr_parse_expr_id_t expr_id = 0;
  const fr_parse_expr_t *value = NULL;
  const fr_parse_expr_t *body = NULL;
  const fr_parse_expr_t *arg = NULL;
  const fr_parse_expr_t *condition = NULL;
  const fr_parse_expr_t *else_body = NULL;
  char line[80];

  CHECK("parse literal definition",
        fr_parse_line("boot is nil", &parsed) == FR_OK &&
            parsed.expr_count == 1 &&
            fr_parse_span_equals(parsed.definition.name, "boot") &&
            parsed.exprs[parsed.definition.value].kind == FR_PARSE_EXPR_NIL);
  CHECK("parse definition trailing semicolon",
        fr_parse_line("boot is nil;", &parsed) == FR_OK &&
            parsed.expr_count == 1 &&
            fr_parse_span_equals(parsed.definition.name, "boot") &&
            parsed.exprs[parsed.definition.value].kind == FR_PARSE_EXPR_NIL);

  CHECK("parse function int body",
        fr_parse_line("boot is fn [ 1 ]", &parsed) == FR_OK &&
            parsed.expr_count == 3 &&
            parsed.exprs[parsed.definition.value].kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[parsed.exprs[parsed.definition.value].child])
                    ->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            parsed.exprs[body->children[0]].kind ==
                FR_PARSE_EXPR_INT &&
            parsed.exprs[body->children[0]].int_value == 1);
  CHECK("parse to definition desugars to fn",
        fr_parse_line("to boot [ 1 ]", &parsed) == FR_OK &&
            parsed.expr_count == 3 &&
            fr_parse_span_equals(parsed.definition.name, "boot") &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            value->param_count == 0 &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            parsed.exprs[body->children[0]].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[body->children[0]].int_value == 1);
  CHECK("parse to rejects missing name",
        fr_parse_line("to [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse to rejects is as name",
        fr_parse_line("to is [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse to rejects missing body",
        fr_parse_line("to boot", &parsed) == FR_ERR_INVALID);
  CHECK("parse to with parameters",
        fr_parse_line("to add with a, b [ a ]", &parsed) == FR_OK &&
            fr_parse_span_equals(parsed.definition.name, "add") &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            value->param_count == 2 &&
            fr_parse_span_equals(parsed.params[value->param_start], "a") &&
            fr_parse_span_equals(parsed.params[value->param_start + 1], "b") &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            parsed.exprs[body->children[0]].kind == FR_PARSE_EXPR_NAME &&
            fr_parse_span_equals(parsed.exprs[body->children[0]].name, "a"));
  CHECK("parse to rejects with as name",
        fr_parse_line("to with [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse to rejects empty with list",
        fr_parse_line("to add with [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse true literal",
        fr_parse_line("boot is true", &parsed) == FR_OK &&
            parsed.exprs[parsed.definition.value].kind == FR_PARSE_EXPR_TRUE);
  CHECK("parse false literal",
        fr_parse_line("boot is false", &parsed) == FR_OK &&
            parsed.exprs[parsed.definition.value].kind == FR_PARSE_EXPR_FALSE);
  CHECK("parse true rejects as parameter",
        fr_parse_line("to f with true [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse false rejects as parameter",
        fr_parse_line("to f with false [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse true rejects as is-definition name",
        fr_parse_line("true is 1", &parsed) == FR_ERR_INVALID);
  CHECK("parse false rejects as is-definition name",
        fr_parse_line("false is 1", &parsed) == FR_ERR_INVALID);
  CHECK("parse true rejects as to-definition name",
        fr_parse_line("to true [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse false rejects as to-definition name",
        fr_parse_line("to false [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse on rejects as parameter",
        fr_parse_line("to f with on [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse every rejects as parameter",
        fr_parse_line("to f with every [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse after rejects as parameter",
        fr_parse_line("to f with after [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse cancel rejects as parameter",
        fr_parse_line("to f with cancel [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse rising rejects as parameter",
        fr_parse_line("to f with rising [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse falling rejects as parameter",
        fr_parse_line("to f with falling [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse changes rejects as parameter",
        fr_parse_line("to f with changes [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse debounce rejects as parameter",
        fr_parse_line("to f with debounce [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse on rejects as is-definition name",
        fr_parse_line("on is 1", &parsed) == FR_ERR_INVALID);
  CHECK("parse every rejects as is-definition name",
        fr_parse_line("every is 1", &parsed) == FR_ERR_INVALID);
  CHECK("parse after rejects as is-definition name",
        fr_parse_line("after is 1", &parsed) == FR_ERR_INVALID);
  CHECK("parse cancel rejects as is-definition name",
        fr_parse_line("cancel is 1", &parsed) == FR_ERR_INVALID);
  CHECK("parse rising rejects as is-definition name",
        fr_parse_line("rising is 1", &parsed) == FR_ERR_INVALID);
  CHECK("parse falling rejects as is-definition name",
        fr_parse_line("falling is 1", &parsed) == FR_ERR_INVALID);
  CHECK("parse changes rejects as is-definition name",
        fr_parse_line("changes is 1", &parsed) == FR_ERR_INVALID);
  CHECK("parse debounce rejects as is-definition name",
        fr_parse_line("debounce is 1", &parsed) == FR_ERR_INVALID);
  CHECK("parse on rejects as to-definition name",
        fr_parse_line("to on [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse every rejects as to-definition name",
        fr_parse_line("to every [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse after rejects as to-definition name",
        fr_parse_line("to after [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse cancel rejects as to-definition name",
        fr_parse_line("to cancel [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse rising rejects as to-definition name",
        fr_parse_line("to rising [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse falling rejects as to-definition name",
        fr_parse_line("to falling [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse changes rejects as to-definition name",
        fr_parse_line("to changes [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse debounce rejects as to-definition name",
        fr_parse_line("to debounce [ 1 ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse on rising",
        fr_parse_line("boot is fn [ on 0 rising [ 1 ] ]", &parsed) == FR_OK &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            (value = &parsed.exprs[body->children[0]])->kind ==
                FR_PARSE_EXPR_EVENT_REGISTER &&
            value->int_value == 1 && value->child_count == 2 &&
            parsed.exprs[value->children[0]].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[value->children[0]].int_value == 0 &&
            (body = &parsed.exprs[value->children[1]])->kind ==
                FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            parsed.exprs[body->children[0]].kind == FR_PARSE_EXPR_INT);
  CHECK("parse on falling",
        fr_parse_line("boot is fn [ on 0 falling [ 1 ] ]", &parsed) == FR_OK &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            (value = &parsed.exprs[body->children[0]])->kind ==
                FR_PARSE_EXPR_EVENT_REGISTER &&
            value->int_value == 2 && value->child_count == 2);
  CHECK("parse on changes",
        fr_parse_line("boot is fn [ on 0 changes [ 1 ] ]", &parsed) == FR_OK &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            (value = &parsed.exprs[body->children[0]])->kind ==
                FR_PARSE_EXPR_EVENT_REGISTER &&
            value->int_value == 3 && value->child_count == 2);
  CHECK("parse on debounce",
        fr_parse_line("boot is fn [ on 0 rising debounce 30 [ 1 ] ]",
                      &parsed) == FR_OK &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            (value = &parsed.exprs[body->children[0]])->kind ==
                FR_PARSE_EXPR_EVENT_REGISTER &&
            value->int_value == 1 && value->child_count == 3 &&
            parsed.exprs[value->children[2]].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[value->children[2]].int_value == 30);
  CHECK("parse on rejects unknown edge",
        fr_parse_line("boot is fn [ on 0 sideways [ 1 ] ]", &parsed) ==
            FR_ERR_INVALID);
  CHECK("parse on rejects missing body",
        fr_parse_line("boot is fn [ on 0 rising ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse on top-level expression",
        fr_parse_expression_line("on 0 rising [ 1 ]", &parsed, &expr_id) ==
                FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_EVENT_REGISTER &&
            parsed.exprs[expr_id].int_value == 1);
#if FR_TAGGED_INT_MAX >= 115200
  CHECK("parse roomier int body",
        fr_parse_line("boot is fn [ 115200 ]", &parsed) == FR_OK &&
            (body = &parsed.exprs[parsed.exprs[parsed.definition.value].child])
                    ->kind == FR_PARSE_EXPR_LIST &&
            parsed.exprs[body->children[0]].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[body->children[0]].int_value == 115200);
#else
  CHECK("parse rejects roomier int body",
        fr_parse_line("boot is fn [ 115200 ]", &parsed) == FR_ERR_RANGE);
#endif
  snprintf(line, sizeof(line), "boot is fn [ %ld ]",
           (long)FR_TAGGED_INT_MAX);
  CHECK("parse max int boundary",
        fr_parse_line(line, &parsed) == FR_OK &&
            (body = &parsed.exprs[parsed.exprs[parsed.definition.value].child])
                    ->kind == FR_PARSE_EXPR_LIST &&
            parsed.exprs[body->children[0]].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[body->children[0]].int_value == FR_TAGGED_INT_MAX);
  snprintf(line, sizeof(line), "boot is fn [ %ld ]",
           (long)FR_TAGGED_INT_MAX + 1L);
  CHECK("parse rejects above max int boundary",
        fr_parse_line(line, &parsed) == FR_ERR_RANGE);
  snprintf(line, sizeof(line), "boot is fn [ %ld ]",
           (long)FR_TAGGED_INT_MIN);
  CHECK("parse min int boundary",
        fr_parse_line(line, &parsed) == FR_OK &&
            (body = &parsed.exprs[parsed.exprs[parsed.definition.value].child])
                    ->kind == FR_PARSE_EXPR_LIST &&
            parsed.exprs[body->children[0]].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[body->children[0]].int_value == FR_TAGGED_INT_MIN);
  snprintf(line, sizeof(line), "boot is fn [ %ld ]",
           (long)FR_TAGGED_INT_MIN - 1L);
  CHECK("parse rejects below min int boundary",
        fr_parse_line(line, &parsed) == FR_ERR_RANGE);

  CHECK("parse call punctuation without spaces",
        fr_parse_line("boot is fn[ms:100]", &parsed) == FR_OK &&
            parsed.expr_count == 4 &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            (body = &parsed.exprs[body->children[0]])->kind ==
                FR_PARSE_EXPR_CALL &&
            fr_parse_span_equals(body->name, "ms") && body->child_count == 1 &&
            (arg = &parsed.exprs[body->child])->kind == FR_PARSE_EXPR_INT &&
            arg->int_value == 100);

  CHECK("parse call with comma arguments",
        fr_parse_line("boot is fn [ pin: $led_builtin, 1 ]", &parsed) ==
                FR_OK &&
            parsed.expr_count == 5 &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            (body = &parsed.exprs[body->children[0]])->kind ==
                FR_PARSE_EXPR_CALL &&
            fr_parse_span_equals(body->name, "pin") &&
            body->child_count == 2 &&
            parsed.exprs[body->children[0]].kind == FR_PARSE_EXPR_NAME &&
            fr_parse_span_equals(parsed.exprs[body->children[0]].name,
                                 "$led_builtin") &&
            parsed.exprs[body->children[1]].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[body->children[1]].int_value == 1);

  CHECK("parse bare name is not call",
        fr_parse_line("boot is fn [ ms ]", &parsed) == FR_OK &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            parsed.exprs[body->children[0]].kind ==
                FR_PARSE_EXPR_NAME);

  CHECK("parse expression line call",
        fr_parse_expression_line("boot:", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_CALL &&
            fr_parse_span_equals(parsed.exprs[expr_id].name, "boot") &&
            parsed.exprs[expr_id].child_count == 0);
  CHECK("parse expression line trailing semicolon",
        fr_parse_expression_line("boot:;", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_CALL &&
            fr_parse_span_equals(parsed.exprs[expr_id].name, "boot") &&
            parsed.exprs[expr_id].child_count == 0);
  CHECK("parse expression line bare name",
        fr_parse_expression_line("boot", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_NAME &&
            fr_parse_span_equals(parsed.exprs[expr_id].name, "boot"));
  CHECK("parse rejects bare semicolon expression",
        fr_parse_expression_line(";", &parsed, &expr_id) == FR_ERR_INVALID);
#if FR_FEATURE_TEXT
  CHECK("parse text definition",
        fr_parse_line("message is \"ready\"", &parsed) == FR_OK &&
            parsed.exprs[parsed.definition.value].kind == FR_PARSE_EXPR_TEXT &&
            parsed.exprs[parsed.definition.value].text.length == 5 &&
            memcmp(parsed.exprs[parsed.definition.value].text.start, "ready",
                   5) == 0);
  CHECK("parse empty text expression",
        fr_parse_expression_line("\"\"", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_TEXT &&
            parsed.exprs[expr_id].text.length == 0);
  CHECK("parse rejects unterminated text",
        fr_parse_line("message is \"ready", &parsed) == FR_ERR_INVALID);
#else
  CHECK("parse text unsupported without feature",
        fr_parse_line("message is \"ready\"", &parsed) ==
            FR_ERR_UNSUPPORTED);
#endif

  CHECK("parse semicolon statement list",
        fr_parse_line("boot is fn [ ms: 100; one ]", &parsed) == FR_OK &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 2);
  CHECK("parse statement list final semicolon",
        fr_parse_line("boot is fn [ ms: 100; one; ]", &parsed) == FR_OK &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 2);
  CHECK("parse if expression",
        fr_parse_line("boot is fn [ if 1 [ one ] else [ nil ] ]", &parsed) ==
                FR_OK &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            (value = &parsed.exprs[body->children[0]])->kind ==
                FR_PARSE_EXPR_IF &&
            value->child_count == 3 &&
            (condition = &parsed.exprs[value->children[0]])->kind ==
                FR_PARSE_EXPR_INT &&
            condition->int_value == 1 &&
            (body = &parsed.exprs[value->children[1]])->kind ==
                FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            parsed.exprs[body->children[0]].kind == FR_PARSE_EXPR_NAME &&
            fr_parse_span_equals(parsed.exprs[body->children[0]].name,
                                 "one") &&
            (else_body = &parsed.exprs[value->children[2]])->kind ==
                FR_PARSE_EXPR_LIST &&
            else_body->child_count == 1 &&
            parsed.exprs[else_body->children[0]].kind == FR_PARSE_EXPR_NIL);
  CHECK("parse if condition zero-arg call before block",
        fr_parse_line("boot is fn [ if boot: [ one ] ]", &parsed) == FR_OK &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            (value = &parsed.exprs[body->children[0]])->kind ==
                FR_PARSE_EXPR_IF &&
            value->child_count == 2 &&
            (condition = &parsed.exprs[value->children[0]])->kind ==
                FR_PARSE_EXPR_CALL &&
            condition->child_count == 0 &&
            fr_parse_span_equals(condition->name, "boot"));
  CHECK("parse when expression",
        fr_parse_line("boot is fn [ when 1 [ one ] ]", &parsed) == FR_OK &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            (value = &parsed.exprs[body->children[0]])->kind ==
                FR_PARSE_EXPR_IF &&
            value->child_count == 2 &&
            (condition = &parsed.exprs[value->children[0]])->kind ==
                FR_PARSE_EXPR_INT &&
            condition->int_value == 1 &&
            (body = &parsed.exprs[value->children[1]])->kind ==
                FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            parsed.exprs[body->children[0]].kind == FR_PARSE_EXPR_NAME &&
            fr_parse_span_equals(parsed.exprs[body->children[0]].name, "one"));
  CHECK("parse when rejects else clause",
        fr_parse_line("boot is fn [ when 1 [ one ] else [ nil ] ]", &parsed) ==
            FR_ERR_INVALID);
  CHECK("parse unless expression",
        fr_parse_line("boot is fn [ unless 1 [ one ] ]", &parsed) == FR_OK &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            (value = &parsed.exprs[body->children[0]])->kind ==
                FR_PARSE_EXPR_IF &&
            value->child_count == 3 &&
            (condition = &parsed.exprs[value->children[0]])->kind ==
                FR_PARSE_EXPR_INT &&
            condition->int_value == 1 &&
            parsed.exprs[value->children[1]].kind == FR_PARSE_EXPR_NIL &&
            (body = &parsed.exprs[value->children[2]])->kind ==
                FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            parsed.exprs[body->children[0]].kind == FR_PARSE_EXPR_NAME &&
            fr_parse_span_equals(parsed.exprs[body->children[0]].name, "one"));
  CHECK("parse unless rejects else clause",
        fr_parse_line("boot is fn [ unless 1 [ one ] else [ nil ] ]",
                      &parsed) == FR_ERR_INVALID);
  CHECK("parse repeat expression",
        fr_parse_line("boot is fn [ repeat 2 [ ms: 10 ] ]", &parsed) ==
                FR_OK &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            (value = &parsed.exprs[body->children[0]])->kind ==
                FR_PARSE_EXPR_REPEAT &&
            value->child_count == 2 &&
            (condition = &parsed.exprs[value->children[0]])->kind ==
                FR_PARSE_EXPR_INT &&
            condition->int_value == 2 &&
            (body = &parsed.exprs[value->children[1]])->kind ==
                FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            parsed.exprs[body->children[0]].kind == FR_PARSE_EXPR_CALL &&
            fr_parse_span_equals(parsed.exprs[body->children[0]].name, "ms"));
  CHECK("parse while expression",
        fr_parse_line("boot is fn [ while 1 [ one ] ]", &parsed) == FR_OK &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            (value = &parsed.exprs[body->children[0]])->kind ==
                FR_PARSE_EXPR_WHILE &&
            value->child_count == 2 &&
            (condition = &parsed.exprs[value->children[0]])->kind ==
                FR_PARSE_EXPR_INT &&
            condition->int_value == 1 &&
            (body = &parsed.exprs[value->children[1]])->kind ==
                FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            parsed.exprs[body->children[0]].kind == FR_PARSE_EXPR_NAME &&
            fr_parse_span_equals(parsed.exprs[body->children[0]].name, "one"));
  CHECK("parse forever expression",
        fr_parse_line("boot is fn [ forever [ ms: 10 ] ]", &parsed) ==
                FR_OK &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            (value = &parsed.exprs[body->children[0]])->kind ==
                FR_PARSE_EXPR_FOREVER &&
            value->child_count == 1 &&
            (body = &parsed.exprs[value->children[0]])->kind ==
                FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            parsed.exprs[body->children[0]].kind == FR_PARSE_EXPR_CALL &&
            fr_parse_span_equals(parsed.exprs[body->children[0]].name, "ms"));
  CHECK("parse set name to expr is slot write",
        fr_parse_expression_line("set count to 7", &parsed, &expr_id) ==
                FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_SLOT_WRITE &&
            fr_parse_span_equals(parsed.exprs[expr_id].name, "count") &&
            parsed.exprs[parsed.exprs[expr_id].child].kind ==
                FR_PARSE_EXPR_INT &&
            parsed.exprs[parsed.exprs[expr_id].child].int_value == 7);
#if FR_FEATURE_CELLS
  CHECK("parse cells definition",
        fr_parse_line("counter is cells(2)", &parsed) == FR_OK &&
            parsed.exprs[parsed.definition.value].kind ==
                FR_PARSE_EXPR_CELLS &&
            parsed.exprs[parsed.definition.value].int_value == 2);
  CHECK("parse cell read expression",
        fr_parse_expression_line("counter[1]", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_CELL_READ &&
            fr_parse_span_equals(parsed.exprs[expr_id].name, "counter") &&
            parsed.exprs[expr_id].int_value == 1);
  CHECK("parse cell write expression",
        fr_parse_expression_line("set counter[1] to 7", &parsed, &expr_id) ==
                FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_CELL_WRITE &&
            fr_parse_span_equals(parsed.exprs[expr_id].name, "counter") &&
            parsed.exprs[expr_id].int_value == 1 &&
            parsed.exprs[parsed.exprs[expr_id].child].kind ==
                FR_PARSE_EXPR_INT);
  CHECK("parse rejects zero cells",
        fr_parse_line("counter is cells(0)", &parsed) == FR_ERR_RANGE);
  CHECK("parse rejects negative cell index",
        fr_parse_expression_line("counter[-1]", &parsed, &expr_id) ==
            FR_ERR_RANGE);
#else
  CHECK("parse cells unsupported without feature",
        fr_parse_line("counter is cells(2)", &parsed) == FR_ERR_UNSUPPORTED);
  CHECK("parse cell read unsupported without feature",
        fr_parse_expression_line("counter[1]", &parsed, &expr_id) ==
            FR_ERR_UNSUPPORTED);
#endif
#if FR_FEATURE_RECORDS
  CHECK("parse record shape",
        fr_parse_line("record Point [ x, y ]", &parsed) == FR_OK &&
            parsed.kind == FR_PARSE_LINE_RECORD_SHAPE &&
            fr_parse_span_equals(parsed.definition.name, "Point") &&
            parsed.record_field_count == 2 &&
            fr_parse_span_equals(parsed.record_fields[0], "x") &&
            fr_parse_span_equals(parsed.record_fields[1], "y"));
  CHECK("parse field read expression",
        fr_parse_expression_line("point->x", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_FIELD_READ &&
            fr_parse_span_equals(parsed.exprs[expr_id].name, "x") &&
            parsed.exprs[parsed.exprs[expr_id].child].kind ==
                FR_PARSE_EXPR_NAME &&
            fr_parse_span_equals(parsed.exprs[parsed.exprs[expr_id].child].name,
                                 "point"));
  CHECK("parse field write expression",
        fr_parse_expression_line("set point->x to 7", &parsed, &expr_id) ==
                FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_FIELD_WRITE &&
            fr_parse_span_equals(parsed.exprs[expr_id].name, "x") &&
            parsed.exprs[parsed.exprs[expr_id].children[1]].kind ==
                FR_PARSE_EXPR_INT);
  CHECK("parse rejects empty record shape",
        fr_parse_line("record Empty [ ]", &parsed) == FR_ERR_RANGE);
  CHECK("parse rejects duplicate record field",
        fr_parse_line("record Bad [ x, x ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse rejects trailing record field comma",
        fr_parse_line("record Bad [ x, ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse rejects dotted record field",
        fr_parse_line("record Bad [ x.y ]", &parsed) == FR_ERR_INVALID);
#else
  CHECK("parse record unsupported without feature",
        fr_parse_line("record Point [ x, y ]", &parsed) ==
            FR_ERR_UNSUPPORTED);
  CHECK("parse field read unsupported without feature",
        fr_parse_expression_line("point->x", &parsed, &expr_id) ==
            FR_ERR_UNSUPPORTED);
#endif
  CHECK("parse function parameters",
        fr_parse_line("boot is fn with count, pin [ count ]", &parsed) ==
                FR_OK &&
            (value = &parsed.exprs[parsed.definition.value])->kind ==
                FR_PARSE_EXPR_FUNCTION &&
            value->param_count == 2 &&
            fr_parse_span_equals(parsed.params[value->param_start], "count") &&
            fr_parse_span_equals(parsed.params[value->param_start + 1], "pin") &&
            (body = &parsed.exprs[value->child])->kind == FR_PARSE_EXPR_LIST &&
            body->child_count == 1 &&
            parsed.exprs[body->children[0]].kind == FR_PARSE_EXPR_NAME &&
            fr_parse_span_equals(parsed.exprs[body->children[0]].name,
                                 "count"));

  CHECK("parse rejects extra expression",
        fr_parse_line("boot is fn [ one two ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse rejects duplicate parameter",
        fr_parse_line("boot is fn with count, count [ count ]", &parsed) ==
            FR_ERR_INVALID);
  CHECK("parse rejects reserved parameter",
        fr_parse_line("boot is fn with repeat [ repeat ]", &parsed) ==
            FR_ERR_INVALID);
  CHECK("parse rejects reserved forever parameter",
        fr_parse_line("boot is fn with forever [ forever ]", &parsed) ==
            FR_ERR_INVALID);
  CHECK("parse rejects malformed forever",
        fr_parse_line("boot is fn [ forever 1 [ one ] ]", &parsed) ==
                FR_ERR_INVALID &&
            fr_parse_line("boot is fn [ forever ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse rejects trailing parameter comma",
        fr_parse_line("boot is fn with count, [ count ]", &parsed) ==
            FR_ERR_INVALID);
  CHECK("parse rejects missing expression",
        fr_parse_line("boot is fn [ ]", &parsed) == FR_ERR_INVALID);
  CHECK("parse rejects empty expression",
        fr_parse_line("boot is fn [ one;; ]", &parsed) == FR_ERR_INVALID &&
            fr_parse_expression_line("boot:;;", &parsed, &expr_id) ==
                FR_ERR_INVALID);
  CHECK("parse rejects unterminated final semicolon",
        fr_parse_line("boot is fn [ one;", &parsed) == FR_ERR_INVALID);
  CHECK("parse rejects excessive expression depth",
        fr_parse_line("boot is fn [ a: b: c: d: e: f: g: h: i: 1 ]",
                      &parsed) == FR_ERR_OVERFLOW);
  CHECK("parse paren overrides precedence",
        fr_parse_expression_line("(1 + 2) * 3", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_MUL &&
            parsed.exprs[parsed.exprs[expr_id].children[0]].kind ==
                FR_PARSE_EXPR_ADD &&
            parsed.exprs[parsed.exprs[expr_id].children[1]].kind ==
                FR_PARSE_EXPR_INT &&
            parsed.exprs[parsed.exprs[expr_id].children[1]].int_value == 3);
  CHECK("parse paren chain reaches max depth",
        fr_parse_expression_line("(((((((1)))))))", &parsed, &expr_id) ==
                FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[expr_id].int_value == 1);
  CHECK("parse rejects overflow paren chain",
        fr_parse_expression_line("((((((((1))))))))", &parsed, &expr_id) ==
            FR_ERR_OVERFLOW);
  CHECK("parse rejects lonely lparen",
        fr_parse_expression_line("(", &parsed, &expr_id) == FR_ERR_INVALID);
  CHECK("parse rejects missing rparen",
        fr_parse_expression_line("(1", &parsed, &expr_id) == FR_ERR_INVALID);
  CHECK("parse percent reads as mod binop",
        fr_parse_expression_line("7 % 3", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_MOD &&
            parsed.exprs[expr_id].child_count == 2 &&
            parsed.exprs[parsed.exprs[expr_id].children[0]].int_value == 7 &&
            parsed.exprs[parsed.exprs[expr_id].children[1]].int_value == 3);
  CHECK("parse percent shares multiplicative precedence",
        fr_parse_expression_line("7 % 2 * 3", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_MUL &&
            parsed.exprs[parsed.exprs[expr_id].children[0]].kind ==
                FR_PARSE_EXPR_MOD);
  CHECK("parse hex literal lowercase",
        fr_parse_expression_line("0xff", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[expr_id].int_value == 255);
  CHECK("parse hex literal uppercase",
        fr_parse_expression_line("0xFF", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[expr_id].int_value == 255);
  CHECK("parse hex literal big X",
        fr_parse_expression_line("0Xff", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[expr_id].int_value == 255);
  CHECK("parse hex literal negative",
        fr_parse_expression_line("-0x10", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[expr_id].int_value == -16);
  CHECK("parse binary literal",
        fr_parse_expression_line("0b1010", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[expr_id].int_value == 10);
  CHECK("parse binary literal zero",
        fr_parse_expression_line("0b0", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[expr_id].int_value == 0);
  CHECK("parse binary literal negative big B",
        fr_parse_expression_line("-0B1", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[expr_id].int_value == -1);
  CHECK("parse bare zero stays decimal",
        fr_parse_expression_line("0", &parsed, &expr_id) == FR_OK &&
            parsed.exprs[expr_id].kind == FR_PARSE_EXPR_INT &&
            parsed.exprs[expr_id].int_value == 0);
  CHECK("parse rejects bare hex prefix",
        fr_parse_expression_line("0x", &parsed, &expr_id) == FR_ERR_INVALID);
  CHECK("parse rejects non-hex digit",
        fr_parse_expression_line("0xg", &parsed, &expr_id) == FR_ERR_INVALID);
  CHECK("parse rejects bare binary prefix",
        fr_parse_expression_line("0b", &parsed, &expr_id) == FR_ERR_INVALID);
  CHECK("parse rejects non-binary digit",
        fr_parse_expression_line("0b2", &parsed, &expr_id) == FR_ERR_INVALID);
  CHECK("parse rejects hex overflow",
        fr_parse_expression_line("0x80000000", &parsed, &expr_id) ==
            FR_ERR_RANGE);
  CHECK("parse rejects null out",
        fr_parse_line("boot is nil", NULL) == FR_ERR_INVALID);
}

static void test_compile(void) {
  fr_runtime_t runtime;
  fr_runtime_t binding_runtime;
  fr_runtime_t cell_runtime;
  fr_runtime_t project_runtime;
#if FR_FEATURE_TEXT
  fr_runtime_t text_runtime;
#endif
#if FR_FEATURE_RECORDS
  fr_runtime_t record_runtime;
#endif
  fr_compile_overlay_update_t update;
  fr_compile_expression_t expression;
  fr_compile_value_binding_t binding;
  fr_tagged_t tagged = 0;
  fr_int_t decoded = 0;
  fr_native_id_t native_id = 0;
  fr_object_id_t object_id = 0;
  fr_slot_id_t slot_id = 0;
  fr_slot_id_t foo_slot = 0;
  fr_slot_id_t bar_slot = 0;
  fr_slot_id_t before_slot_count = 0;
  uint16_t before_name_count = 0;
  fr_code_object_id_t param_code_id = 0;
  const char *param_names = NULL;
  uint16_t param_names_len = 0;
#if FR_FEATURE_TEXT
  const uint8_t *text_bytes = NULL;
  uint16_t text_length = 0;
#endif
  char line[32];
  const uint16_t push_size = FR_INSTRUCTION_PUSH_INT_SIZE;

  CHECK("compile boot is nil",
        fr_compile_overlay_update("boot is nil", &update) == FR_OK &&
            update.slot_inits[0].slot_id == FR_SLOT_BOOT &&
            update.slot_inits[0].ref.kind == FR_IMAGE_REF_LITERAL_TAGGED &&
            fr_tagged_is_nil(update.slot_inits[0].ref.literal_tagged));
  CHECK("compile definition trailing semicolon",
        fr_compile_overlay_update("boot is nil;", &update) == FR_OK &&
            update.slot_inits[0].slot_id == FR_SLOT_BOOT &&
            update.slot_inits[0].ref.kind == FR_IMAGE_REF_LITERAL_TAGGED &&
            fr_tagged_is_nil(update.slot_inits[0].ref.literal_tagged));
  CHECK("compile tolerates whitespace",
        fr_compile_overlay_update(" \tboot\nis\rnil ", &update) == FR_OK &&
            update.slot_inits[0].slot_id == FR_SLOT_BOOT &&
            fr_tagged_is_nil(update.slot_inits[0].ref.literal_tagged));
  CHECK("compile positive int",
        fr_compile_overlay_update("boot is 1", &update) == FR_OK &&
            fr_tagged_decode_int(update.slot_inits[0].ref.literal_tagged,
                                 &decoded) == FR_OK &&
            decoded == 1);
  CHECK("compile negative int",
        fr_compile_overlay_update("boot is -1", &update) == FR_OK &&
            fr_tagged_decode_int(update.slot_inits[0].ref.literal_tagged,
                                 &decoded) == FR_OK &&
            decoded == -1);
  CHECK("compile min int literal round-trips",
        ((void)snprintf(line, sizeof(line), "boot is fn [ %" PRId32 " ]",
                        (int32_t)FR_TAGGED_INT_MIN),
         fr_base_image_install(&runtime) == FR_OK &&
             fr_compile_overlay_update(line, &update) == FR_OK &&
             fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
             fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
             fr_tagged_decode_int(tagged, &decoded) == FR_OK &&
             decoded == FR_TAGGED_INT_MIN));
#if FR_TAGGED_INT_MAX >= 115200
  CHECK("compile roomier int",
        fr_compile_overlay_update("boot is 115200", &update) == FR_OK &&
            fr_tagged_decode_int(update.slot_inits[0].ref.literal_tagged,
                                 &decoded) == FR_OK &&
            decoded == 115200);
#else
  CHECK("compile rejects roomier int",
        fr_compile_overlay_update("boot is 115200", &update) == FR_ERR_RANGE);
#endif
  CHECK("compile int range",
        ((void)snprintf(line, sizeof(line), "boot is %" PRId32,
                        (int32_t)FR_TAGGED_INT_MAX + 1),
         fr_compile_overlay_update(line, &update) == FR_ERR_RANGE));
  CHECK("compile rejects bad int",
        fr_compile_overlay_update("boot is 12x", &update) == FR_ERR_UNSUPPORTED);
  CHECK("compile rejects unknown slot",
        fr_compile_overlay_update("missing is nil", &update) == FR_ERR_NOT_FOUND);
  CHECK("compile rejects extra token",
        fr_compile_overlay_update("boot is nil now", &update) == FR_ERR_INVALID);
  CHECK("compile rejects missing token",
        fr_compile_overlay_update("boot is", &update) == FR_ERR_INVALID);
  CHECK("compile rejects null source",
        fr_compile_overlay_update(NULL, &update) == FR_ERR_INVALID);
  CHECK("compile rejects null out",
        fr_compile_overlay_update("boot is nil", NULL) == FR_ERR_INVALID);

  CHECK("compiled overlay update owns slot init",
        fr_compile_overlay_update("boot is 1", &update) == FR_OK &&
            update.overlay_update.slot_inits == update.slot_inits &&
            update.overlay_update.slot_init_count == 1 &&
            update.overlay_update.code_objects == NULL &&
            update.overlay_update.code_object_count == 0 &&
            update.overlay_update.natives == NULL &&
            update.overlay_update.native_count == 0);
  CHECK("compiled overlay update applies",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is 1", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_slot_read(&runtime, FR_SLOT_BOOT, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compile overlay update rejects null out",
        fr_compile_overlay_update("boot is nil", NULL) == FR_ERR_INVALID);
  CHECK("compile without runtime rejects dynamic definition",
        fr_compile_overlay_update("led is $led_builtin", &update) ==
            FR_ERR_NOT_FOUND);
  CHECK("compile runtime overlay name",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "led is $led_builtin", &update) == FR_OK &&
            update.slot_inits[0].slot_id == FR_TEST_FIRST_USER_SLOT &&
            update.slot_inits[0].ref.kind == FR_IMAGE_REF_LITERAL_TAGGED &&
            fr_tagged_decode_int(update.slot_inits[0].ref.literal_tagged,
                                 &decoded) == FR_OK &&
            decoded == 13 && update.overlay_update.slot_names == &update.slot_name &&
            update.overlay_update.slot_name_count == 1 &&
            update.slot_name.slot_id == FR_TEST_FIRST_USER_SLOT &&
            strcmp(update.slot_name.name, "led") == 0);
  CHECK("compiled runtime overlay name applies",
        fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_slot_id_for_name(&runtime, "led", &slot_id) == FR_OK &&
            slot_id == FR_TEST_FIRST_USER_SLOT &&
            strcmp(fr_slot_name(&runtime, slot_id), "led") == 0);
#if FR_FEATURE_TEXT
  CHECK("compile runtime text definition",
        fr_base_image_install(&text_runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(&text_runtime,
                                                  "message is \"ready\"",
                                                  &update) == FR_OK &&
            update.slot_inits[0].slot_id == FR_TEST_FIRST_USER_SLOT &&
            update.slot_inits[0].ref.kind == FR_IMAGE_REF_TEXT_OBJECT &&
            update.text_object.length == 5 &&
            memcmp(update.text_object.bytes, "ready", 5) == 0 &&
            update.overlay_update.text_objects == &update.text_object &&
            update.overlay_update.text_object_count == 1 &&
            fr_overlay_apply(&text_runtime, &update.overlay_update) == FR_OK &&
            fr_slot_read(&text_runtime, FR_TEST_FIRST_USER_SLOT, &tagged) ==
                FR_OK &&
            fr_tagged_decode_object_id(tagged, &object_id) == FR_OK &&
            fr_text_view(&text_runtime, object_id, &text_bytes, &text_length) ==
                FR_OK &&
            text_length == 5 && memcmp(text_bytes, "ready", 5) == 0);
  CHECK("compile text expression pushes object id",
        fr_compile_expression_for_runtime(&text_runtime, "\"ready\"",
                                          &expression) == FR_OK &&
            expression.instructions.length ==
                FR_INSTRUCTION_MIN_HEADER_SIZE +
                    FR_INSTRUCTION_PUSH_OBJECT_ID_SIZE + 1u &&
            expression.instruction_bytes[2] == FR_OP_PUSH_OBJECT_ID &&
            fr_vm_run_instruction_stream(&text_runtime, &expression.instructions,
                                         &tagged) == FR_OK &&
            fr_tagged_decode_object_id(tagged, &object_id) == FR_OK &&
            fr_text_view(&text_runtime, object_id, &text_bytes, &text_length) ==
                FR_OK &&
            text_length == 5 && memcmp(text_bytes, "ready", 5) == 0);
  CHECK("compile text expression without runtime stays unsupported",
        fr_compile_expression("\"ready\"", &expression) == FR_ERR_UNSUPPORTED);
#else
  CHECK("compile text unsupported without feature",
        fr_compile_overlay_update_for_runtime(&runtime, "message is \"ready\"",
                                              &update) == FR_ERR_UNSUPPORTED);
#endif
#if FR_FEATURE_CELLS
  CHECK("compile runtime cells definition",
        fr_base_image_install(&cell_runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(&cell_runtime,
                                                  "counter is cells(2)",
                                                  &update) == FR_OK &&
            update.slot_inits[0].slot_id == FR_TEST_FIRST_USER_SLOT &&
            update.slot_inits[0].ref.kind == FR_IMAGE_REF_CELL_OBJECT &&
            update.cell_object.length == 2 &&
            update.overlay_update.cell_objects == &update.cell_object &&
            update.overlay_update.cell_object_count == 1 &&
            update.overlay_update.slot_name_count == 1 &&
            strcmp(update.slot_name.name, "counter") == 0 &&
            fr_overlay_apply(&cell_runtime, &update.overlay_update) == FR_OK &&
            fr_slot_id_for_name(&cell_runtime, "counter", &slot_id) ==
                FR_OK &&
            fr_slot_read(&cell_runtime, slot_id, &tagged) == FR_OK &&
            fr_tagged_decode_object_id(tagged, &object_id) == FR_OK &&
            fr_cells_length(&cell_runtime, object_id, &(uint16_t){0}) ==
                FR_OK);
  CHECK("compile cell read owns instruction bytes",
        fr_compile_expression_for_runtime(&cell_runtime, "counter[0]",
                                          &expression) == FR_OK &&
            expression.instructions.length == 8 &&
            expression.instruction_bytes[2] == FR_OP_LOAD_CELL &&
            expression.instruction_bytes[3] == slot_id &&
            expression.instruction_bytes[5] == 0 &&
            expression.instruction_bytes[7] == FR_OP_RETURN);
  CHECK("compile cell write stores and returns nil",
        fr_compile_expression_for_runtime(&cell_runtime, "set counter[0] to 7",
                                          &expression) == FR_OK &&
            expression.instructions.length == 8u + push_size &&
            expression.instruction_bytes[2] == FR_OP_PUSH_INT &&
            expression.instruction_bytes[2u + push_size] ==
                FR_OP_STORE_CELL &&
            expression.instruction_bytes[3u + push_size] == slot_id &&
            expression.instruction_bytes[7u + push_size] == FR_OP_RETURN &&
            fr_vm_run_instruction_stream(&cell_runtime, &expression.instructions,
                                         &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged) &&
            fr_compile_expression_for_runtime(&cell_runtime, "counter[0]",
                                              &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&cell_runtime,
                                         &expression.instructions, &tagged) ==
                FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 7);
#if FR_FEATURE_TEXT
  CHECK("compile cell write stores named text",
        fr_compile_overlay_update_for_runtime(&cell_runtime,
                                              "message is \"ready\"",
                                              &update) == FR_OK &&
            fr_overlay_apply(&cell_runtime, &update.overlay_update) == FR_OK &&
            fr_compile_expression_for_runtime(&cell_runtime,
                                              "set counter[0] to message",
                                              &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&cell_runtime, &expression.instructions,
                                         &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged) &&
            fr_compile_expression_for_runtime(&cell_runtime, "counter[0]",
                                              &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&cell_runtime,
                                         &expression.instructions, &tagged) ==
                FR_OK &&
            fr_tagged_decode_object_id(tagged, &object_id) == FR_OK &&
            fr_text_view(&cell_runtime, object_id, &text_bytes,
                         &text_length) == FR_OK &&
            text_length == 5 && memcmp(text_bytes, "ready", 5) == 0);
#endif
  CHECK("compile function cell write",
        fr_compile_overlay_update_for_runtime(
            &cell_runtime, "write_counter is fn [ set counter[1] to one ]",
            &update) == FR_OK &&
            fr_overlay_apply(&cell_runtime, &update.overlay_update) == FR_OK &&
            fr_slot_id_for_name(&cell_runtime, "write_counter", &slot_id) ==
                FR_OK &&
            fr_vm_run_slot(&cell_runtime, slot_id, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged) &&
            fr_compile_expression_for_runtime(&cell_runtime, "counter[1]",
                                              &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&cell_runtime,
                                         &expression.instructions, &tagged) ==
                FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compile cell read range fails at runtime",
        fr_compile_expression_for_runtime(&cell_runtime, "counter[2]",
                                          &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&cell_runtime,
                                         &expression.instructions, &tagged) ==
                FR_ERR_RANGE);
#if FR_TAGGED_INT_MAX > 65535
  CHECK("compile rejects cell index beyond operand width",
        fr_compile_expression_for_runtime(&cell_runtime, "counter[65536]",
                                          &expression) == FR_ERR_RANGE &&
            fr_compile_expression_for_runtime(
                &cell_runtime, "set counter[65536] to 7", &expression) ==
                FR_ERR_RANGE);
#endif
  CHECK("compile rejects bare cells expression",
        fr_compile_expression_for_runtime(&cell_runtime, "cells(1)",
                                          &expression) == FR_ERR_UNSUPPORTED);
  CHECK("compile rejects oversized cells definition",
        ((void)snprintf(line, sizeof(line), "large is cells(%u)",
                        (unsigned)FR_PROFILE_MAX_CELL_LENGTH + 1u),
         fr_compile_overlay_update_for_runtime(&cell_runtime, line, &update) ==
             FR_ERR_RANGE));
  CHECK("cell write rejects code values",
        fr_compile_expression_for_runtime(&cell_runtime,
                                          "set counter[0] to write_counter",
                                          &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&cell_runtime,
                                         &expression.instructions, &tagged) ==
                FR_ERR_TYPE);
  CHECK("cell write rejects object values",
        fr_compile_expression_for_runtime(&cell_runtime,
                                          "set counter[0] to counter",
                                          &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&cell_runtime,
                                         &expression.instructions, &tagged) ==
                FR_ERR_TYPE);
#else
  (void)cell_runtime;
  (void)object_id;
  CHECK("compile cells unsupported without feature",
        fr_compile_overlay_update_for_runtime(&runtime, "counter is cells(1)",
                                              &update) == FR_ERR_UNSUPPORTED);
#endif
#if FR_FEATURE_RECORDS
  {
    fr_object_id_t shape_object_id = 0;
    fr_object_id_t record_object_id = 0;
    fr_record_name_t shape_name = {0};
    fr_tagged_t field_value = 0;
    uint16_t field_count = 0;

    CHECK("compile runtime record shape",
          fr_base_image_install(&record_runtime) == FR_OK &&
              fr_compile_overlay_update_for_runtime(
                  &record_runtime, "record Point [ x, y ]", &update) ==
                  FR_OK &&
              update.slot_inits[0].ref.kind ==
                  FR_IMAGE_REF_RECORD_SHAPE_OBJECT &&
              update.record_shape_object.field_count == 2 &&
              update.overlay_update.record_shape_object_count == 1 &&
              update.overlay_update.slot_name_count == 1 &&
              strcmp(update.slot_name.name, "Point") == 0 &&
              fr_overlay_apply(&record_runtime, &update.overlay_update) ==
                  FR_OK &&
              fr_slot_id_for_name(&record_runtime, "Point", &slot_id) ==
                  FR_OK &&
              fr_slot_read(&record_runtime, slot_id, &tagged) == FR_OK &&
              fr_tagged_decode_object_id(tagged, &shape_object_id) == FR_OK &&
              fr_record_shape_view(&record_runtime, shape_object_id,
                                   &shape_name, &field_count) == FR_OK &&
              field_count == 2 &&
              memcmp(shape_name.bytes, "Point", 5) == 0);
    CHECK("compile accepts matching record shape redefinition",
          fr_compile_overlay_update_for_runtime(
              &record_runtime, "record Point [ x, y ]", &update) == FR_OK &&
              update.overlay_update.record_shape_object_count == 1 &&
              update.overlay_update.slot_name_count == 0);
    CHECK("compile rejects record shape field drift",
          fr_compile_overlay_update_for_runtime(
              &record_runtime, "record Point [ x, z ]", &update) ==
          FR_ERR_INVALID);
    CHECK("compile runtime record constructor",
          fr_compile_overlay_update_for_runtime(
              &record_runtime, "point is Point: 10, \"ready\"", &update) ==
                  FR_OK &&
              update.slot_inits[0].ref.kind == FR_IMAGE_REF_RECORD_OBJECT &&
              update.record_object.field_count == 2 &&
              update.overlay_update.record_object_count == 1 &&
              update.overlay_update.text_object_count == 1 &&
              fr_overlay_apply(&record_runtime, &update.overlay_update) ==
                  FR_OK &&
              fr_slot_id_for_name(&record_runtime, "point", &slot_id) ==
                  FR_OK &&
              fr_slot_read(&record_runtime, slot_id, &tagged) == FR_OK &&
              fr_tagged_decode_object_id(tagged, &record_object_id) == FR_OK &&
              fr_record_read_field(
                  &record_runtime, record_object_id,
                  (fr_record_name_t){.bytes = (const uint8_t *)"x",
                                     .length = 1},
                  &field_value) == FR_OK &&
              fr_tagged_decode_int(field_value, &decoded) == FR_OK &&
              decoded == 10 &&
              fr_record_read_field(
                  &record_runtime, record_object_id,
                  (fr_record_name_t){.bytes = (const uint8_t *)"y",
                                     .length = 1},
                  &field_value) == FR_OK &&
              fr_tagged_decode_object_id(field_value, &object_id) == FR_OK &&
              fr_text_view(&record_runtime, object_id, &text_bytes,
                           &text_length) == FR_OK &&
              text_length == 5 && memcmp(text_bytes, "ready", 5) == 0);
    CHECK("compile field read owns instruction bytes",
          fr_compile_expression_for_runtime(&record_runtime, "point->x",
                                            &expression) == FR_OK &&
              expression.instructions.length == 9 &&
              expression.instruction_bytes[2] == FR_OP_LOAD_SLOT &&
              expression.instruction_bytes[5] == FR_OP_LOAD_FIELD &&
              expression.instruction_bytes[6] == 1 &&
              expression.instruction_bytes[7] == 'x' &&
              expression.instruction_bytes[8] == FR_OP_RETURN &&
              fr_vm_run_instruction_stream(&record_runtime,
                                           &expression.instructions,
                                           &tagged) == FR_OK &&
              fr_tagged_decode_int(tagged, &decoded) == FR_OK &&
              decoded == 10);
    CHECK("compile field write stores and returns nil",
          fr_compile_expression_for_runtime(&record_runtime,
                                            "set point->x to 11",
                                            &expression) == FR_OK &&
              expression.instructions.length == 9u + push_size &&
              expression.instruction_bytes[2] == FR_OP_LOAD_SLOT &&
              expression.instruction_bytes[5] == FR_OP_PUSH_INT &&
              expression.instruction_bytes[5u + push_size] ==
                  FR_OP_STORE_FIELD &&
              expression.instruction_bytes[6u + push_size] == 1 &&
              expression.instruction_bytes[7u + push_size] == 'x' &&
              expression.instruction_bytes[8u + push_size] == FR_OP_RETURN &&
              fr_vm_run_instruction_stream(&record_runtime,
                                           &expression.instructions,
                                           &tagged) == FR_OK &&
              fr_tagged_is_nil(tagged) &&
              fr_compile_expression_for_runtime(&record_runtime, "point->x",
                                                &expression) == FR_OK &&
              fr_vm_run_instruction_stream(&record_runtime,
                                           &expression.instructions,
                                           &tagged) == FR_OK &&
              fr_tagged_decode_int(tagged, &decoded) == FR_OK &&
              decoded == 11);
    CHECK("field read rejects record shape object",
          fr_compile_expression_for_runtime(&record_runtime, "Point->x",
                                            &expression) == FR_OK &&
              fr_vm_run_instruction_stream(&record_runtime,
                                           &expression.instructions,
                                           &tagged) == FR_ERR_TYPE);
    CHECK("field write rejects record shape object",
          fr_compile_expression_for_runtime(&record_runtime,
                                            "set Point->x to 1",
                                            &expression) == FR_OK &&
              fr_vm_run_instruction_stream(&record_runtime,
                                           &expression.instructions,
                                           &tagged) == FR_ERR_TYPE);
    CHECK("compile function field read",
          fr_compile_overlay_update_for_runtime(
              &record_runtime, "get_x is fn [ point->x ]", &update) ==
                  FR_OK &&
              fr_overlay_apply(&record_runtime, &update.overlay_update) ==
                  FR_OK &&
              fr_slot_id_for_name(&record_runtime, "get_x", &slot_id) ==
                  FR_OK &&
              fr_vm_run_slot(&record_runtime, slot_id, &tagged) == FR_OK &&
              fr_tagged_decode_int(tagged, &decoded) == FR_OK &&
              decoded == 11);
    CHECK("compile rejects runtime constructor in function",
          fr_compile_overlay_update_for_runtime(
              &record_runtime, "make_point is fn [ Point: 1, 2 ]", &update) ==
              FR_ERR_UNSUPPORTED);
  }
#else
  CHECK("compile field read unsupported without feature",
        fr_compile_expression_for_runtime(&runtime, "point->x", &expression) ==
            FR_ERR_UNSUPPORTED);
#endif
  CHECK("compile runtime existing overlay name",
        fr_compile_overlay_update_for_runtime(&runtime, "led is 12", &update) ==
                FR_OK &&
            update.slot_inits[0].slot_id == FR_TEST_FIRST_USER_SLOT &&
            update.overlay_update.slot_name_count == 0);
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  CHECK("compile unknown name does not allocate project slot",
        fr_base_image_install(&project_runtime) == FR_OK &&
            (before_slot_count = fr_slot_count(&project_runtime), true) &&
            (before_name_count =
                 fr_slot_project_name_count(&project_runtime),
             true) &&
            fr_compile_expression_for_runtime(&project_runtime, "ghost",
                                              &expression) ==
                FR_ERR_NOT_FOUND &&
            fr_slot_count(&project_runtime) == before_slot_count &&
            fr_slot_project_name_count(&project_runtime) == before_name_count);
  CHECK("compile project definition allocates first project slot",
        fr_compile_overlay_update_for_runtime(&project_runtime, "foo is 1",
                                              &update) == FR_OK &&
            update.slot_inits[0].slot_id == FR_TEST_FIRST_USER_SLOT &&
            update.overlay_update.slot_name_count == 1 &&
            fr_overlay_apply(&project_runtime, &update.overlay_update) ==
                FR_OK &&
            fr_slot_id_for_name(&project_runtime, "foo", &foo_slot) == FR_OK &&
            foo_slot == FR_TEST_FIRST_USER_SLOT);
  CHECK("compile project redefinition reuses slot",
        (before_slot_count = fr_slot_count(&project_runtime), true) &&
            (before_name_count =
                 fr_slot_project_name_count(&project_runtime),
             true) &&
            fr_compile_overlay_update_for_runtime(&project_runtime, "foo is 2",
                                                  &update) == FR_OK &&
            update.slot_inits[0].slot_id == foo_slot &&
            update.overlay_update.slot_name_count == 0 &&
            fr_overlay_apply(&project_runtime, &update.overlay_update) ==
                FR_OK &&
            fr_slot_count(&project_runtime) == before_slot_count &&
            fr_slot_project_name_count(&project_runtime) ==
                before_name_count &&
            fr_slot_read(&project_runtime, foo_slot, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 2);
  CHECK("compile project rename leaves old name until clear",
        fr_compile_overlay_update_for_runtime(&project_runtime, "bar is foo",
                                              &update) == FR_OK &&
            update.slot_inits[0].slot_id ==
                (fr_slot_id_t)(FR_TEST_FIRST_USER_SLOT + 1) &&
            fr_overlay_apply(&project_runtime, &update.overlay_update) ==
                FR_OK &&
            fr_slot_id_for_name(&project_runtime, "foo", &foo_slot) == FR_OK &&
            fr_slot_id_for_name(&project_runtime, "bar", &bar_slot) == FR_OK &&
            foo_slot == FR_TEST_FIRST_USER_SLOT &&
            bar_slot == (fr_slot_id_t)(FR_TEST_FIRST_USER_SLOT + 1));
  for (uint16_t i = fr_slot_project_name_count(&project_runtime);
       i < FR_PROFILE_MAX_OVERLAY_NAMES; i++) {
    (void)snprintf(line, sizeof(line), "p%u is 1", (unsigned)i);
    CHECK("compile project fills overlay name capacity",
          fr_compile_overlay_update_for_runtime(&project_runtime, line,
                                                &update) == FR_OK &&
              fr_overlay_apply(&project_runtime, &update.overlay_update) ==
                  FR_OK);
  }
  CHECK("compile project overlay name capacity error",
        fr_compile_overlay_update_for_runtime(&project_runtime, "extra is 1",
                                              &update) == FR_ERR_CAPACITY);
  CHECK("runtime clear project drops project names",
        fr_runtime_clear_project(&project_runtime) == FR_OK &&
            fr_slot_project_name_count(&project_runtime) == 0 &&
            fr_slot_count(&project_runtime) == FR_TEST_FIRST_USER_SLOT &&
            fr_slot_id_for_name(&project_runtime, "foo", &foo_slot) ==
                FR_ERR_NOT_FOUND &&
            fr_slot_id_for_name(&project_runtime, "boot", &slot_id) == FR_OK &&
            slot_id == FR_SLOT_BOOT);
#endif
  CHECK("compile runtime dynamic function",
        fr_compile_overlay_update_for_runtime(
            &runtime, "myblink is fn [ pin: led, 1 ]", &update) == FR_OK &&
            update.slot_inits[0].slot_id == FR_TEST_FIRST_USER_SLOT + 1 &&
            update.overlay_update.slot_name_count == 1 &&
            strcmp(update.slot_name.name, "myblink") == 0 &&
            update.instruction_bytes[2] == FR_OP_LOAD_SLOT &&
            update.instruction_bytes[3] == FR_TEST_FIRST_USER_SLOT &&
            update.instruction_bytes[5] == FR_OP_PUSH_INT &&
            update.instruction_bytes[5u + push_size] ==
                FR_OP_CALL_NATIVE_SLOT &&
            update.instruction_bytes[6u + push_size] == FR_SLOT_GPIO_WRITE);
  CHECK("compiled runtime dynamic function applies and runs",
        fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_slot_id_for_name(&runtime, "myblink", &slot_id) == FR_OK &&
            slot_id == FR_TEST_FIRST_USER_SLOT + 1 &&
            fr_vm_run_slot(&runtime, slot_id, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("compile runtime expression uses overlay name",
        fr_compile_expression_for_runtime(&runtime, "pin: led, 1",
                                          &expression) == FR_OK &&
            expression.instruction_bytes[2] == FR_OP_LOAD_SLOT &&
            expression.instruction_bytes[3] == FR_TEST_FIRST_USER_SLOT &&
            expression.instruction_bytes[5u + push_size] ==
                FR_OP_CALL_NATIVE_SLOT &&
            expression.instruction_bytes[6u + push_size] ==
                FR_SLOT_GPIO_WRITE);
  CHECK("compile static overlay rejects runtime call binding",
        fr_base_image_install(&binding_runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &binding_runtime, "reading is adc.read: 14", &update) ==
                FR_ERR_UNSUPPORTED);
  CHECK("compile runtime call binding owns store instruction",
        fr_compile_value_binding_for_runtime(
            &binding_runtime, "reading is adc.read: 14", &binding) == FR_OK &&
            binding.slot_id == FR_TEST_FIRST_USER_SLOT &&
            binding.has_slot_name &&
            strcmp(binding.slot_name.name, "reading") == 0 &&
            binding.instructions.length == 9u + push_size &&
            binding.instruction_bytes[2] == FR_OP_PUSH_INT &&
            binding.instruction_bytes[2u + push_size] ==
                FR_OP_CALL_NATIVE_SLOT &&
            binding.instruction_bytes[3u + push_size] == FR_SLOT_ADC_READ &&
            binding.instruction_bytes[5u + push_size] == FR_OP_STORE_SLOT &&
            binding.instruction_bytes[6u + push_size] ==
                FR_TEST_FIRST_USER_SLOT &&
            binding.instruction_bytes[8u + push_size] == FR_OP_RETURN &&
            fr_vm_run_instruction_stream(&binding_runtime,
                                         &binding.instructions, &tagged) ==
                FR_OK &&
            fr_tagged_is_nil(tagged) &&
            fr_slot_bind_project_name(&binding_runtime, binding.slot_name.name,
                                      binding.slot_name.slot_id) == FR_OK &&
            fr_slot_id_for_name(&binding_runtime, "reading", &slot_id) ==
                FR_OK &&
            fr_slot_read(&binding_runtime, slot_id, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 512);
  CHECK("compile runtime value binding is call-only",
        fr_compile_value_binding_for_runtime(&binding_runtime, "static is 1",
                                             &binding) == FR_ERR_UNSUPPORTED);
  CHECK("compiled parameter function owns arity header",
        fr_compile_overlay_update("boot is fn with count [ count ]",
                                  &update) == FR_OK &&
            update.code_object.instructions.length == 6 &&
            update.instruction_bytes[0] == FR_INSTRUCTION_FORMAT_VERSION &&
            update.instruction_bytes[1] == FR_INSTRUCTION_ARITY_HEADER_SIZE &&
            update.instruction_bytes[2] == 1 &&
            update.instruction_bytes[3] == FR_OP_LOAD_ARG &&
            update.instruction_bytes[4] == 0 &&
            update.instruction_bytes[5] == FR_OP_RETURN);
  CHECK("compiled parameter shadows slot name",
        fr_compile_overlay_update("boot is fn with one [ one ]", &update) ==
                FR_OK &&
            update.instruction_bytes[1] == FR_INSTRUCTION_ARITY_HEADER_SIZE &&
            update.instruction_bytes[2] == 1 &&
            update.instruction_bytes[3] == FR_OP_LOAD_ARG &&
            update.instruction_bytes[4] == 0);
  CHECK("compiled parameter function rejects missing arg at direct run",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn with count [ count ]",
                                      &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_ERR_INVALID);
  CHECK("compiled code call passes argument",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "echo is fn with value [ value ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_slot_id_for_name(&runtime, "echo", &slot_id) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "boot is fn [ echo: 7 ]", &update) == FR_OK &&
            update.code_object.instructions.length == 7u + push_size &&
            update.instruction_bytes[2] == FR_OP_PUSH_INT &&
            update.instruction_bytes[3] == 7 &&
            update.instruction_bytes[2u + push_size] ==
                FR_OP_CALL_SLOT_ARG &&
            update.instruction_bytes[3u + push_size] == slot_id &&
            update.instruction_bytes[5u + push_size] == 1 &&
            update.instruction_bytes[6u + push_size] == FR_OP_RETURN &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 7);
  CHECK("compiled code call rejects missing arg",
        fr_compile_overlay_update_for_runtime(&runtime,
                                              "boot is fn [ echo: ]",
                                              &update) == FR_ERR_INVALID);
  CHECK("compiled code call rejects extra arg",
        fr_compile_overlay_update_for_runtime(
            &runtime, "boot is fn [ echo: 1, 2 ]", &update) ==
            FR_ERR_INVALID);
  CHECK("compiled parameter repeat uses load arg",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn with count [ repeat count [ ms: 1 ] ]",
                &update) == FR_OK &&
            update.code_object.instructions.length == 17u + push_size &&
            update.instruction_bytes[1] == FR_INSTRUCTION_ARITY_HEADER_SIZE &&
            update.instruction_bytes[2] == 1 &&
            update.instruction_bytes[3] == FR_OP_LOAD_ARG &&
            update.instruction_bytes[4] == 0 &&
            update.instruction_bytes[5] == FR_OP_REPEAT_BEGIN &&
            update.instruction_bytes[6] == 15u + push_size &&
            update.instruction_bytes[12u + push_size] ==
                FR_OP_REPEAT_NEXT &&
            update.instruction_bytes[13u + push_size] == 8);

  CHECK("compiled function owns instruction bytes",
        fr_compile_overlay_update("boot is fn [ 1 ]", &update) == FR_OK &&
            update.slot_inits[0].slot_id == FR_SLOT_BOOT &&
            update.slot_inits[0].ref.kind == FR_IMAGE_REF_CODE_OBJECT &&
            update.slot_inits[0].ref.index == 0 &&
            update.overlay_update.code_objects == &update.code_object &&
            update.overlay_update.code_object_count == 1 &&
            update.code_object.instructions.bytes == update.instruction_bytes &&
            update.code_object.instructions.length == 3u + push_size &&
            update.instruction_bytes[0] == FR_INSTRUCTION_FORMAT_VERSION &&
            update.instruction_bytes[1] == FR_INSTRUCTION_MIN_HEADER_SIZE &&
            update.instruction_bytes[2] == FR_OP_PUSH_INT &&
            update.instruction_bytes[3] == 1 &&
            update.instruction_bytes[2u + push_size] == FR_OP_RETURN);
  CHECK("compiled function applies and runs",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 1 ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compiled to definition runs like is fn",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("to boot [ 1 ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compiled to with parameters runs",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "to first with a, b [ a ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "boot is fn [ first: 7, 9 ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 7);
  CHECK("compiled to records param names on its code object",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "to pair with a, b [ a ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_slot_id_for_name(&runtime, "pair", &slot_id) == FR_OK &&
            fr_slot_read(&runtime, slot_id, &tagged) == FR_OK &&
            fr_tagged_decode_code_object_id(tagged, &param_code_id) == FR_OK &&
            fr_code_get_param_names(&runtime, param_code_id, &param_names,
                                    &param_names_len) == FR_OK &&
            param_names_len == 4 && memcmp(param_names, "a\0b\0", 4) == 0);
  CHECK("compiled to with rejects wrong arity",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "to first with a, b [ a ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "boot is fn [ first: 7 ]", &update) ==
                FR_ERR_INVALID);
  CHECK("compiled true literal round-trips through slot",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is true", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_slot_read(&runtime, FR_SLOT_BOOT, &tagged) == FR_OK &&
            fr_tagged_is_true(tagged));
  CHECK("compiled false literal round-trips through slot",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is false", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_slot_read(&runtime, FR_SLOT_BOOT, &tagged) == FR_OK &&
            fr_tagged_is_false(tagged));
  CHECK("compiled if true takes then branch",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ if true [ 1 ] else [ 2 ] ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compiled if false takes else branch",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ if false [ 1 ] else [ 2 ] ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 2);
  CHECK("compiled fn returning true",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ true ]", &update) ==
                FR_OK &&
            update.instruction_bytes[2] == FR_OP_PUSH_TRUE &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_true(tagged));
  CHECK("compiled fn returning false",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ false ]", &update) ==
                FR_OK &&
            update.instruction_bytes[2] == FR_OP_PUSH_FALSE &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_false(tagged));
  CHECK("compiled less-than true",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 1 < 2 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_true(tagged));
  CHECK("compiled less-than false",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 2 < 1 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_false(tagged));
  CHECK("compiled greater-than true",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 3 > 2 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_true(tagged));
  CHECK("compiled greater-than false",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 2 > 3 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_false(tagged));
  CHECK("compiled less-or-equal true when equal",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 2 <= 2 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_true(tagged));
  CHECK("compiled less-or-equal false",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 3 <= 2 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_false(tagged));
  CHECK("compiled greater-or-equal true when equal",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 2 >= 2 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_true(tagged));
  CHECK("compiled greater-or-equal false",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 1 >= 2 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_false(tagged));
  CHECK("compiled equal true",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 5 = 5 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_true(tagged));
  CHECK("compiled equal false",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 5 = 6 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_false(tagged));
  CHECK("compiled not-equal true",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 5 <> 6 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_true(tagged));
  CHECK("compiled not-equal false",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 5 <> 5 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_false(tagged));
  CHECK("compiled comparison rejects boolean operand",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ true < 1 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_ERR_TYPE);
  CHECK("compiled comparison feeds if",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ if 3 > 2 [ 1 ] else [ 0 ] ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compiled addition",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 2 + 3 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 5);
  CHECK("compiled addition mixes with subtraction at same precedence",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 10 - 4 + 1 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 7);
  CHECK("compiled addition rejects boolean operand",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ true + 1 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_ERR_TYPE);
  CHECK("compiled subtraction",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 5 - 2 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 3);
  CHECK("compiled subtraction without spaces",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 5-2 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 3);
  CHECK("compiled subtraction after parameter without spaces",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "to dec with x [ x-1 ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "boot is fn [ dec: 1 ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 0);
  CHECK("compiled multiplication",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 3 * 4 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 12);
  CHECK("compiled division truncates",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 7 / 2 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 3);
  CHECK("compiled multiplication binds tighter than subtraction",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 4 - 2 * 3 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == -2);
  CHECK("compiled subtraction rejects boolean operand",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ true - 1 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_ERR_TYPE);
  CHECK("compiled division by zero rejects",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 1 / 0 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_ERR_RANGE);
  CHECK("compiled subtraction below min rejects",
        ((void)snprintf(line, sizeof(line),
                        "boot is fn [ %" PRId32 " - 1 ]",
                        (int32_t)FR_TAGGED_INT_MIN),
         fr_runtime_init(&runtime) == FR_OK &&
             fr_compile_overlay_update(line, &update) == FR_OK &&
             fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
             fr_vm_run_boot(&runtime, &tagged) == FR_ERR_RANGE));
  CHECK("compiled multiplication above max rejects",
        ((void)snprintf(line, sizeof(line),
                        "boot is fn [ %" PRId32 " * 2 ]",
                        (int32_t)FR_TAGGED_INT_MAX),
         fr_runtime_init(&runtime) == FR_OK &&
             fr_compile_overlay_update(line, &update) == FR_OK &&
             fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
             fr_vm_run_boot(&runtime, &tagged) == FR_ERR_RANGE));
  CHECK("compiled division negates min above max",
        ((void)snprintf(line, sizeof(line),
                        "boot is fn [ %" PRId32 " / -1 ]",
                        (int32_t)FR_TAGGED_INT_MIN),
         fr_runtime_init(&runtime) == FR_OK &&
             fr_compile_overlay_update(line, &update) == FR_OK &&
             fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
             fr_vm_run_boot(&runtime, &tagged) == FR_ERR_RANGE));
  CHECK("compiled parens override precedence",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ (1 + 2) * 3 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 9);
  CHECK("compiled precedence unchanged without parens",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ 1 + 2 * 3 ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 7);
  CHECK("compiled paren chain at max depth",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_expression("(((((((1)))))))", &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &expression.instructions,
                                         &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compiled native call owns instruction bytes",
        fr_compile_overlay_update("boot is fn [ ms: 100 ]", &update) == FR_OK &&
            update.slot_inits[0].slot_id == FR_SLOT_BOOT &&
            update.slot_inits[0].ref.kind == FR_IMAGE_REF_CODE_OBJECT &&
            update.slot_inits[0].ref.index == 0 &&
            update.overlay_update.code_objects == &update.code_object &&
            update.overlay_update.code_object_count == 1 &&
            update.overlay_update.slot_init_count == 1 &&
            update.overlay_update.natives == NULL &&
            update.overlay_update.native_count == 0 &&
            update.code_object.instructions.bytes == update.instruction_bytes &&
            update.code_object.instructions.length == 6u + push_size &&
            update.instruction_bytes[0] == FR_INSTRUCTION_FORMAT_VERSION &&
            update.instruction_bytes[1] == FR_INSTRUCTION_MIN_HEADER_SIZE &&
            update.instruction_bytes[2] == FR_OP_PUSH_INT &&
            update.instruction_bytes[3] == 100 &&
            update.instruction_bytes[2u + push_size] ==
                FR_OP_CALL_NATIVE_SLOT &&
            update.instruction_bytes[3u + push_size] == FR_SLOT_MS &&
            update.instruction_bytes[5u + push_size] == FR_OP_RETURN);
  CHECK("compiled native call uses base slot",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ ms: 100 ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("compiled statement list drops non-final result",
        fr_compile_overlay_update("boot is fn [ ms: 100; one ]", &update) == FR_OK &&
            update.code_object.instructions.length == 10u + push_size &&
            update.instruction_bytes[2] == FR_OP_PUSH_INT &&
            update.instruction_bytes[3] == 100 &&
            update.instruction_bytes[2u + push_size] ==
                FR_OP_CALL_NATIVE_SLOT &&
            update.instruction_bytes[3u + push_size] == FR_SLOT_MS &&
            update.instruction_bytes[5u + push_size] == FR_OP_DROP &&
            update.instruction_bytes[6u + push_size] == FR_OP_LOAD_SLOT &&
            update.instruction_bytes[7u + push_size] == FR_SLOT_ONE &&
            update.instruction_bytes[9u + push_size] == FR_OP_RETURN);
  CHECK("compiled statement list returns final result",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ ms: 100; one ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compiled statement list accepts final terminator",
        fr_compile_overlay_update("boot is fn [ ms: 100; one; ]", &update) ==
                FR_OK &&
            update.code_object.instructions.length == 10u + push_size &&
            update.instruction_bytes[5u + push_size] == FR_OP_DROP &&
            update.instruction_bytes[6u + push_size] == FR_OP_LOAD_SLOT &&
            update.instruction_bytes[7u + push_size] == FR_SLOT_ONE &&
            update.instruction_bytes[9u + push_size] == FR_OP_RETURN);
  CHECK("compiled statement list supports repeated calls",
        fr_compile_overlay_update("boot is fn [ ms: 100; ms: 100; one ]", &update) ==
                FR_OK &&
            update.code_object.instructions.length == 14u + (push_size * 2u) &&
            update.instruction_bytes[2] == FR_OP_PUSH_INT &&
            update.instruction_bytes[2u + push_size] ==
                FR_OP_CALL_NATIVE_SLOT &&
            update.instruction_bytes[5u + push_size] == FR_OP_DROP &&
            update.instruction_bytes[6u + push_size] == FR_OP_PUSH_INT &&
            update.instruction_bytes[6u + (push_size * 2u)] ==
                FR_OP_CALL_NATIVE_SLOT &&
            update.instruction_bytes[9u + (push_size * 2u)] == FR_OP_DROP &&
            update.instruction_bytes[10u + (push_size * 2u)] ==
                FR_OP_LOAD_SLOT &&
            update.instruction_bytes[13u + (push_size * 2u)] ==
                FR_OP_RETURN);
  CHECK("compiled repeated calls return final result",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ ms: 100; ms: 100; one ]",
                              &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compiled mechanical boot owns instruction bytes",
        fr_compile_overlay_update(
            "boot is fn [ pin: $led_builtin, 1; ms: 100; "
            "pin: $led_builtin, 0; ms: 100 ]",
            &update) == FR_OK &&
            update.code_object.instructions.length == 24u + (push_size * 4u) &&
            update.instruction_bytes[2] == FR_OP_LOAD_SLOT &&
            update.instruction_bytes[3] == FR_SLOT_LED_BUILTIN &&
            update.instruction_bytes[5] == FR_OP_PUSH_INT &&
            update.instruction_bytes[6] == 1 &&
            update.instruction_bytes[5u + push_size] ==
                FR_OP_CALL_NATIVE_SLOT &&
            update.instruction_bytes[6u + push_size] == FR_SLOT_GPIO_WRITE &&
            update.instruction_bytes[8u + push_size] == FR_OP_DROP &&
            update.instruction_bytes[9u + push_size] == FR_OP_PUSH_INT &&
            update.instruction_bytes[9u + (push_size * 2u)] ==
                FR_OP_CALL_NATIVE_SLOT &&
            update.instruction_bytes[10u + (push_size * 2u)] == FR_SLOT_MS &&
            update.instruction_bytes[12u + (push_size * 2u)] == FR_OP_DROP &&
            update.instruction_bytes[13u + (push_size * 2u)] ==
                FR_OP_LOAD_SLOT &&
            update.instruction_bytes[14u + (push_size * 2u)] ==
                FR_SLOT_LED_BUILTIN &&
            update.instruction_bytes[16u + (push_size * 2u)] ==
                FR_OP_PUSH_INT &&
            update.instruction_bytes[17u + (push_size * 2u)] == 0 &&
            update.instruction_bytes[16u + (push_size * 3u)] ==
                FR_OP_CALL_NATIVE_SLOT &&
            update.instruction_bytes[17u + (push_size * 3u)] ==
                FR_SLOT_GPIO_WRITE &&
            update.instruction_bytes[19u + (push_size * 3u)] == FR_OP_DROP &&
            update.instruction_bytes[20u + (push_size * 3u)] ==
                FR_OP_PUSH_INT &&
            update.instruction_bytes[20u + (push_size * 4u)] ==
                FR_OP_CALL_NATIVE_SLOT &&
            update.instruction_bytes[21u + (push_size * 4u)] == FR_SLOT_MS &&
            update.instruction_bytes[23u + (push_size * 4u)] ==
                FR_OP_RETURN);
  CHECK("compiled mechanical boot runs",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ pin: $led_builtin, 1; ms: 100; "
                "pin: $led_builtin, 0; ms: 100 ]",
                &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("compiled ms rejects negative domain",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ ms: -1 ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_ERR_DOMAIN);
  CHECK("compiled pin rejects negative value",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ pin: $led_builtin, -1 ]",
                              &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_ERR_DOMAIN);
  CHECK("compiled bare native name reads base slot",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ ms ]", &update) == FR_OK &&
            update.instruction_bytes[2] == FR_OP_LOAD_SLOT &&
            update.instruction_bytes[3] == FR_SLOT_MS &&
            update.instruction_bytes[4] == 0 &&
            update.instruction_bytes[5] == FR_OP_RETURN &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_native_id(tagged, &native_id) == FR_OK &&
            native_id == 0);
  CHECK("compiled one slot function uses base slot",
        fr_compile_overlay_update("boot is fn [ one ]", &update) == FR_OK &&
            update.slot_inits[0].slot_id == FR_SLOT_BOOT &&
            update.slot_inits[0].ref.kind == FR_IMAGE_REF_CODE_OBJECT &&
            update.overlay_update.slot_init_count == 1 &&
            update.instruction_bytes[2] == FR_OP_LOAD_SLOT &&
            update.instruction_bytes[3] == FR_SLOT_ONE &&
            update.instruction_bytes[4] == 0 &&
            update.instruction_bytes[5] == FR_OP_RETURN);
  CHECK("compiled one slot function runs",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ one ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compiled function rejects unknown name",
        fr_compile_overlay_update("boot is fn [ x ]", &update) == FR_ERR_NOT_FOUND);
  CHECK("compiled function rejects int range",
        ((void)snprintf(line, sizeof(line), "boot is fn [ %" PRId32 " ]",
                        (int32_t)FR_TAGGED_INT_MAX + 1),
         fr_compile_overlay_update(line, &update) == FR_ERR_RANGE));
  CHECK("compiled native call rejects missing arg",
        fr_compile_overlay_update("boot is fn [ ms: ]", &update) == FR_ERR_INVALID);
  CHECK("compiled if else owns jumps",
        fr_compile_overlay_update("boot is fn [ if 1 [ one ] else [ nil ] ]",
                                  &update) == FR_OK &&
            update.code_object.instructions.length == 13u + push_size &&
            update.instruction_bytes[2] == FR_OP_PUSH_INT &&
            update.instruction_bytes[3] == 1 &&
            update.instruction_bytes[2u + push_size] ==
                FR_OP_JUMP_IF_FALSY &&
            update.instruction_bytes[3u + push_size] == 11u + push_size &&
            update.instruction_bytes[5u + push_size] == FR_OP_LOAD_SLOT &&
            update.instruction_bytes[6u + push_size] == FR_SLOT_ONE &&
            update.instruction_bytes[8u + push_size] == FR_OP_JUMP &&
            update.instruction_bytes[9u + push_size] == 12u + push_size &&
            update.instruction_bytes[11u + push_size] == FR_OP_PUSH_NIL &&
            update.instruction_bytes[12u + push_size] == FR_OP_RETURN);
  CHECK("compiled if else returns selected branch",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ if 1 [ one ] else [ nil ] ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compiled if without else returns nil when false",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ if 0 [ one ] ]",
                                      &update) == FR_OK &&
            update.code_object.instructions.length == 13u + push_size &&
            update.instruction_bytes[11u + push_size] == FR_OP_PUSH_NIL &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("compiled chained else if first arm",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ if true [ 1 ] else if true [ 2 ] else [ 3 ] ]",
                &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compiled chained else if middle arm",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ if false [ 1 ] else if true [ 2 ] else [ 3 ] ]",
                &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 2);
  CHECK("compiled chained else if falls to final else",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ if false [ 1 ] else if false [ 2 ] else [ 3 ] ]",
                &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 3);
  CHECK("compiled chained else if without final else yields nil",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ if false [ 1 ] else if false [ 2 ] "
                "else if false [ 3 ] ]",
                &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("compiled recursive else-if form still parses (backward compat)",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ if false [ 1 ] else [ if true [ 2 ] else [ 3 ] ] ]",
                &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 2);
  CHECK("compiled chained else if rejects over-capacity arms",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ if false [ 1 ] else if false [ 2 ] "
                "else if false [ 3 ] else if false [ 4 ] "
                "else if false [ 5 ] else if false [ 6 ] ]",
                &update) == FR_ERR_CAPACITY);
  CHECK("compiled here local binding yields nil",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ here x is 5 ]",
                                      &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("compiled here local binding reads back its value",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ here x is 5 ; x + 1 ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 6);
  CHECK("top-level expression line carries a here binding across `;`",
        fr_compile_expression("here x is 5 ; x + 1", &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &expression.instructions,
                                         &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 6);
  CHECK("here local declared in an if body is not visible after the block",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ here x is 1 ; if true [ here y is 2 ] ; y ]",
                &update) == FR_ERR_NOT_FOUND);
  CHECK("here local cannot shadow a function param",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn with x [ here x is 1 ; x ]",
                &update) == FR_ERR_INVALID);
  CHECK("here local shadows a previous local in the same block",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ here x is 1 ; here x is 2 ; x ]",
                &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 2);
  CHECK("five here locals in one function exceed the cap",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ here a is 1 ; here b is 2 ; here c is 3 ; "
                "here d is 4 ; here e is 5 ; a ]",
                &update) == FR_ERR_CAPACITY);
  CHECK("here after a closed inner block still binds a fresh slot",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ here a is 1 ; if true [ here b is 2 ] ; "
                "here c is 3 ; a + c ]",
                &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 4);
  CHECK("compiled when true runs body",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ when true [ 1 ] ]",
                                      &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compiled when non-zero int runs body",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ when 5 [ 1 ] ]",
                                      &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compiled when false skips body and yields nil",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ when false [ 1 ] ]",
                                      &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("compiled when nil skips body and yields nil",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ when nil [ 1 ] ]",
                                      &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("compiled unless false runs body",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ unless false [ 1 ] ]",
                                      &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compiled unless nil runs body",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ unless nil [ 1 ] ]",
                                      &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("compiled unless true skips body and yields nil",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ unless true [ 1 ] ]",
                                      &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("compiled unless non-zero int skips body and yields nil",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ unless 5 [ 1 ] ]",
                                      &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("compiled repeat uses loop opcodes",
        fr_compile_overlay_update("boot is fn [ repeat 2 [ ms: 1 ] ]",
                                  &update) == FR_OK &&
            update.code_object.instructions.length == 14u + (push_size * 2u) &&
            update.instruction_bytes[2] == FR_OP_PUSH_INT &&
            update.instruction_bytes[3] == 2 &&
            update.instruction_bytes[2u + push_size] == FR_OP_REPEAT_BEGIN &&
            update.instruction_bytes[3u + push_size] ==
                12u + (push_size * 2u) &&
            update.instruction_bytes[5u + push_size] == FR_OP_PUSH_INT &&
            update.instruction_bytes[6u + push_size] == 1 &&
            update.instruction_bytes[5u + (push_size * 2u)] ==
                FR_OP_CALL_NATIVE_SLOT &&
            update.instruction_bytes[6u + (push_size * 2u)] == FR_SLOT_MS &&
            update.instruction_bytes[8u + (push_size * 2u)] == FR_OP_DROP &&
            update.instruction_bytes[9u + (push_size * 2u)] ==
                FR_OP_REPEAT_NEXT &&
            update.instruction_bytes[10u + (push_size * 2u)] ==
                5u + push_size &&
            update.instruction_bytes[12u + (push_size * 2u)] ==
                FR_OP_PUSH_NIL &&
            update.instruction_bytes[13u + (push_size * 2u)] ==
                FR_OP_RETURN);
  CHECK("compiled repeat returns nil",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ repeat 2 [ ms: 1 ] ]",
                                      &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("compiled zero repeat returns nil",
        fr_compile_overlay_update("boot is fn [ repeat 0 [ one ] ]",
                                  &update) == FR_OK &&
            update.code_object.instructions.length == 14u + push_size &&
            update.instruction_bytes[2] == FR_OP_PUSH_INT &&
            update.instruction_bytes[3] == 0 &&
            update.instruction_bytes[2u + push_size] == FR_OP_REPEAT_BEGIN &&
            update.instruction_bytes[3u + push_size] == 12u + push_size &&
            update.instruction_bytes[5u + push_size] == FR_OP_LOAD_SLOT &&
            update.instruction_bytes[6u + push_size] == FR_SLOT_ONE &&
            update.instruction_bytes[8u + push_size] == FR_OP_DROP &&
            update.instruction_bytes[9u + push_size] == FR_OP_REPEAT_NEXT &&
            update.instruction_bytes[10u + push_size] == 5u + push_size &&
            update.instruction_bytes[12u + push_size] == FR_OP_PUSH_NIL &&
            update.instruction_bytes[13u + push_size] == FR_OP_RETURN);
  CHECK("compiled repeat accepts dynamic count",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ repeat one [ ms: 1 ] ]",
                                      &update) == FR_OK &&
            update.code_object.instructions.length == 17u + push_size &&
            update.instruction_bytes[2] == FR_OP_LOAD_SLOT &&
            update.instruction_bytes[3] == FR_SLOT_ONE &&
            update.instruction_bytes[5] == FR_OP_REPEAT_BEGIN &&
            update.instruction_bytes[6] == 15u + push_size &&
            update.instruction_bytes[12u + push_size] ==
                FR_OP_REPEAT_NEXT &&
            update.instruction_bytes[13u + push_size] == 8 &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("compiled repeat rejects negative dynamic count at runtime",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(&runtime, "count is -1",
                                                  &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "boot is fn [ repeat count [ one ] ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_ERR_RANGE);
  CHECK("compile rejects negative repeat count",
        fr_compile_overlay_update("boot is fn [ repeat -1 [ one ] ]",
                                  &update) == FR_ERR_RANGE);
  CHECK("compiled while uses jump loop",
        fr_compile_overlay_update("boot is fn [ while 0 [ 1 ] ]",
                                  &update) == FR_OK &&
            update.code_object.instructions.length ==
                11u + (push_size * 2u) &&
            update.instruction_bytes[2] == FR_OP_PUSH_INT &&
            update.instruction_bytes[3] == 0 &&
            update.instruction_bytes[2u + push_size] ==
                FR_OP_JUMP_IF_FALSY &&
            update.instruction_bytes[3u + push_size] ==
                9u + (push_size * 2u) &&
            update.instruction_bytes[5u + push_size] == FR_OP_PUSH_INT &&
            update.instruction_bytes[6u + push_size] == 1 &&
            update.instruction_bytes[5u + (push_size * 2u)] == FR_OP_DROP &&
            update.instruction_bytes[6u + (push_size * 2u)] == FR_OP_JUMP &&
            update.instruction_bytes[7u + (push_size * 2u)] == 2 &&
            update.instruction_bytes[9u + (push_size * 2u)] ==
                FR_OP_PUSH_NIL &&
            update.instruction_bytes[10u + (push_size * 2u)] ==
                FR_OP_RETURN);
  CHECK("compiled while false skips body and yields nil",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_overlay_update("boot is fn [ while false [ 1 ] ]",
                                      &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
#if FR_FEATURE_CELLS
  CHECK("compiled while decrements cell to zero and exits",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "counter is cells(1)", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime,
                "boot is fn [ set counter[0] to 3; "
                "while counter[0] [ set counter[0] to counter[0] - 1 ] ]",
                &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged) &&
            fr_compile_expression_for_runtime(&runtime, "counter[0]",
                                              &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &expression.instructions,
                                         &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 0);
  CHECK("compiled while exits when comparison on slot flips",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "counter is cells(1)", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime,
                "boot is fn [ set counter[0] to 1; "
                "while counter[0] > 0 [ set counter[0] to 0 ] ]",
                &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged) &&
            fr_compile_expression_for_runtime(&runtime, "counter[0]",
                                              &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &expression.instructions,
                                         &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 0);
#endif
  CHECK("compiled forever uses jump loop",
        fr_compile_overlay_update("boot is fn [ forever [ ms: 1 ] ]",
                                  &update) == FR_OK &&
            update.code_object.instructions.length == 11u + push_size &&
            update.instruction_bytes[2] == FR_OP_PUSH_INT &&
            update.instruction_bytes[3] == 1 &&
            update.instruction_bytes[2u + push_size] ==
                FR_OP_CALL_NATIVE_SLOT &&
            update.instruction_bytes[3u + push_size] == FR_SLOT_MS &&
            update.instruction_bytes[5u + push_size] == FR_OP_DROP &&
            update.instruction_bytes[6u + push_size] == FR_OP_JUMP &&
            update.instruction_bytes[7u + push_size] == 2 &&
            update.instruction_bytes[9u + push_size] == FR_OP_PUSH_NIL &&
            update.instruction_bytes[10u + push_size] == FR_OP_RETURN);
  CHECK("compiled pure forever sees pending interrupt",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "boot is fn [ forever [ 1 ] ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            (fr_runtime_interrupt(&runtime), true) &&
            fr_vm_run_boot(&runtime, &tagged) == FR_ERR_INTERRUPTED);
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  CHECK("compiled forever exits on cooperative interrupt",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_native_install(&runtime, test_native_interrupt, 0, NULL,
                              &native_id) == FR_OK &&
            fr_tagged_encode_native_id(native_id, &tagged) == FR_OK &&
            fr_slot_write(&runtime, fr_slot_first_project_id(), tagged) ==
                FR_OK &&
            fr_slot_bind_project_name(&runtime, "stop",
                                      fr_slot_first_project_id()) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "boot is fn [ forever [ stop: ] ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            (fr_runtime_clear_interrupt(&runtime), true) &&
            fr_vm_run_boot(&runtime, &tagged) == FR_ERR_INTERRUPTED);
#endif

  CHECK("compiled expression owns instruction bytes",
        fr_compile_expression("one", &expression) == FR_OK &&
            expression.instructions.bytes == expression.instruction_bytes &&
            expression.instructions.length == 6 &&
            expression.instruction_bytes[0] == FR_INSTRUCTION_FORMAT_VERSION &&
            expression.instruction_bytes[1] == FR_INSTRUCTION_MIN_HEADER_SIZE &&
            expression.instruction_bytes[2] == FR_OP_LOAD_SLOT &&
            expression.instruction_bytes[3] == FR_SLOT_ONE &&
            expression.instruction_bytes[4] == 0 &&
            expression.instruction_bytes[5] == FR_OP_RETURN);
  CHECK("compiled nil expression owns instruction bytes",
        fr_compile_expression("nil", &expression) == FR_OK &&
            expression.instructions.bytes == expression.instruction_bytes &&
            expression.instructions.length == 4 &&
            expression.instruction_bytes[0] == FR_INSTRUCTION_FORMAT_VERSION &&
            expression.instruction_bytes[1] == FR_INSTRUCTION_MIN_HEADER_SIZE &&
            expression.instruction_bytes[2] == FR_OP_PUSH_NIL &&
            expression.instruction_bytes[3] == FR_OP_RETURN);
  CHECK("compiled nil expression runs",
        fr_runtime_init(&runtime) == FR_OK &&
            fr_compile_expression("nil", &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &expression.instructions,
                                         &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("compiled expression supports native calls",
        fr_compile_expression("pin: $led_builtin, 1", &expression) == FR_OK &&
            expression.instructions.bytes == expression.instruction_bytes &&
            expression.instructions.length == 9u + push_size &&
            expression.instruction_bytes[2] == FR_OP_LOAD_SLOT &&
            expression.instruction_bytes[3] == FR_SLOT_LED_BUILTIN &&
            expression.instruction_bytes[4] == 0 &&
            expression.instruction_bytes[5] == FR_OP_PUSH_INT &&
            expression.instruction_bytes[6] == 1 &&
            expression.instruction_bytes[5u + push_size] ==
                FR_OP_CALL_NATIVE_SLOT &&
            expression.instruction_bytes[6u + push_size] ==
                FR_SLOT_GPIO_WRITE &&
            expression.instruction_bytes[8u + push_size] == FR_OP_RETURN);
  CHECK("compiled expression accepts trailing semicolon",
        fr_compile_expression("pin: $led_builtin, 1;", &expression) == FR_OK &&
            expression.instructions.length == 9u + push_size &&
            expression.instruction_bytes[5u + push_size] ==
                FR_OP_CALL_NATIVE_SLOT &&
            expression.instruction_bytes[6u + push_size] ==
                FR_SLOT_GPIO_WRITE);
  CHECK("compiled expression rejects definition",
        fr_compile_expression("boot is 1", &expression) == FR_ERR_INVALID);
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  CHECK("compile set name to expr stores into existing slot",
        fr_base_image_install(&binding_runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(&binding_runtime,
                                                  "count is 1", &update) ==
                FR_OK &&
            fr_overlay_apply(&binding_runtime, &update.overlay_update) ==
                FR_OK &&
            fr_compile_expression_for_runtime(&binding_runtime,
                                              "set count to 42",
                                              &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&binding_runtime,
                                         &expression.instructions,
                                         &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged) &&
            fr_slot_id_for_name(&binding_runtime, "count", &slot_id) ==
                FR_OK &&
            fr_slot_read(&binding_runtime, slot_id, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 42);
  CHECK("compile rejects set on undeclared slot",
        fr_compile_expression_for_runtime(&binding_runtime,
                                          "set never_declared to 1",
                                          &expression) == FR_ERR_NOT_FOUND);
#endif
  CHECK("compile rejects set on function parameter",
        fr_compile_overlay_update("boot is fn with x [ set x to 1 ]",
                                  &update) == FR_ERR_INVALID);
}

/*
 * Host-owned-name helper profiles strip names before sending updates. This
 * fixture proves the device-owned-name boundary only.
 */
#if FR_FEATURE_REPL && FR_BASE_IMAGE_INCLUDE_SYMBOLS &&                         \
    FR_PROFILE_MAX_OVERLAY_NAMES > 0 && !FR_HOST_TINY_NAMES_MODE
static fr_err_t test_compile_apply_source_direct(fr_runtime_t *runtime,
                                                 const char *source) {
  fr_compile_overlay_update_t update;

  FR_TRY(fr_compile_overlay_update_for_runtime(runtime, source, &update));
  return fr_overlay_apply(runtime, &update.overlay_update);
}

static fr_err_t test_compile_apply_source_via_wire(fr_runtime_t *host_mirror,
                                                   fr_runtime_t *device,
                                                   const char *source) {
  fr_compile_overlay_update_t update;
  fr_overlay_update_decoded_t decoded;
  uint8_t bytes[FR_REPL_APPLY_BYTES];
  uint16_t byte_count = 0;

  FR_TRY(fr_compile_overlay_update_for_runtime(host_mirror, source, &update));
  FR_TRY(fr_overlay_update_encode(&update.overlay_update, bytes,
                                  (uint16_t)sizeof(bytes), &byte_count));
  FR_TRY(fr_overlay_update_decode(bytes, byte_count, &decoded));
  FR_TRY(fr_overlay_apply(device, &decoded.update));
  return fr_overlay_apply(host_mirror, &update.overlay_update);
}

static bool test_code_streams_match(const fr_runtime_t *lhs,
                                    fr_code_object_id_t lhs_code_id,
                                    const fr_runtime_t *rhs,
                                    fr_code_object_id_t rhs_code_id) {
  fr_instruction_stream_t lhs_view = {0};
  fr_instruction_stream_t rhs_view = {0};

  if (fr_code_get_instructions(lhs, lhs_code_id, &lhs_view) != FR_OK ||
      fr_code_get_instructions(rhs, rhs_code_id, &rhs_view) != FR_OK) {
    return false;
  }
  return lhs_view.length == rhs_view.length &&
         memcmp(lhs_view.bytes, rhs_view.bytes, lhs_view.length) == 0;
}

static bool test_named_slot_effects_match(fr_runtime_t *lhs, fr_runtime_t *rhs,
                                          const char *name) {
  fr_slot_id_t lhs_slot_id = 0;
  fr_slot_id_t rhs_slot_id = 0;
  fr_tagged_t lhs_tagged = 0;
  fr_tagged_t rhs_tagged = 0;
  fr_code_object_id_t lhs_code_id = 0;
  fr_code_object_id_t rhs_code_id = 0;
  bool lhs_is_code = false;
  bool rhs_is_code = false;

  if (fr_slot_id_for_name(lhs, name, &lhs_slot_id) != FR_OK ||
      fr_slot_id_for_name(rhs, name, &rhs_slot_id) != FR_OK ||
      fr_slot_read(lhs, lhs_slot_id, &lhs_tagged) != FR_OK ||
      fr_slot_read(rhs, rhs_slot_id, &rhs_tagged) != FR_OK) {
    return false;
  }
  if (lhs_slot_id != rhs_slot_id) {
    return false;
  }

  lhs_is_code = fr_tagged_decode_code_object_id(lhs_tagged, &lhs_code_id) ==
                FR_OK;
  rhs_is_code = fr_tagged_decode_code_object_id(rhs_tagged, &rhs_code_id) ==
                FR_OK;
  if (lhs_is_code || rhs_is_code) {
    return lhs_is_code && rhs_is_code &&
           test_code_streams_match(lhs, lhs_code_id, rhs, rhs_code_id);
  }

  return lhs_tagged == rhs_tagged;
}

static void test_compiler_overlay_wire_parity(void) {
  fr_runtime_t direct_runtime;
  fr_runtime_t host_mirror;
  fr_runtime_t wire_runtime;
  fr_tagged_t direct_result = 0;
  fr_tagged_t wire_result = 0;
  const char *sources[] = {
      "led is $led_builtin",
      "myblink is fn [ pin: led, 1 ]",
      "boot is fn [ myblink: ]",
  };

  CHECK("compiler parity installs base runtimes",
        fr_base_image_install(&direct_runtime) == FR_OK &&
            fr_base_image_install(&host_mirror) == FR_OK &&
            fr_base_image_install(&wire_runtime) == FR_OK);

  for (uint16_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
    CHECK("compiler parity direct apply",
          test_compile_apply_source_direct(&direct_runtime, sources[i]) ==
              FR_OK);
    CHECK("compiler parity wire apply",
          test_compile_apply_source_via_wire(&host_mirror, &wire_runtime,
                                             sources[i]) == FR_OK);
  }

  CHECK("compiler parity slot counts match",
        direct_runtime.slots.count == wire_runtime.slots.count &&
            host_mirror.slots.count == wire_runtime.slots.count);
  CHECK("compiler parity code counts match",
        direct_runtime.code.count == wire_runtime.code.count &&
            host_mirror.code.count == wire_runtime.code.count &&
            direct_runtime.code.used_instruction_bytes ==
                wire_runtime.code.used_instruction_bytes &&
            host_mirror.code.used_instruction_bytes ==
                wire_runtime.code.used_instruction_bytes);
  CHECK("compiler parity literal slot matches",
        test_named_slot_effects_match(&direct_runtime, &wire_runtime, "led"));
  CHECK("compiler parity code slot matches",
        test_named_slot_effects_match(&direct_runtime, &wire_runtime, "myblink"));
  CHECK("compiler parity boot slot matches",
        test_named_slot_effects_match(&direct_runtime, &wire_runtime, "boot"));
  CHECK("compiler parity host mirror matches target",
        test_named_slot_effects_match(&host_mirror, &wire_runtime, "boot"));
  CHECK("compiler parity direct boot runs",
        fr_vm_run_boot(&direct_runtime, &direct_result) == FR_OK &&
            fr_tagged_is_nil(direct_result));
  CHECK("compiler parity wire boot runs",
        fr_vm_run_boot(&wire_runtime, &wire_result) == FR_OK &&
            fr_tagged_is_nil(wire_result));
}
#endif
#endif

#if FR_FEATURE_PERSISTENCE && FR_FEATURE_COMPILER
static void test_persist(void) {
  fr_runtime_t runtime;
  fr_compile_overlay_update_t update;
  fr_compile_expression_t expression;
  fr_tagged_t tagged = 0;
  fr_int_t decoded = 0;
  fr_code_object_id_t code_id = 0;
  fr_slot_id_t slot_id = 0;
#if FR_FEATURE_HANDLES
  fr_runtime_t handle_restore_runtime;
  fr_handle_ref_t handle_ref = {0};
  fr_tagged_t handle_tagged = 0;
  uint16_t handle_payload_bytes = 0;
#endif
#if FR_FEATURE_TEXT
  const uint8_t binary_text[] = {'a', '\0', 'b'};
  const uint8_t *text_bytes = NULL;
  fr_object_id_t object_id = 0;
  fr_object_id_t text_id = 0;
  fr_tagged_t text_tagged = 0;
  uint16_t text_length = 0;
#endif
  uint8_t junk = 0xa5;
  uint8_t header[FR_PERSIST_HEADER_BYTES];
  uint8_t wrong_version_payload[] = {
      'F',
      'R',
      'P',
      'O',
#if FR_WORD_SIZE == 16
      1,
#else
      0,
#endif
      FR_TEST_PERSIST_RECORD_END,
  };
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0 && FR_WORD_SIZE == 16
  uint8_t old_name_free_payload[] = {
      'F',
      'R',
      'P',
      'O',
      0,
      FR_TEST_PERSIST_RECORD_BIND,
      0,
      0,
      FR_TEST_PERSIST_VALUE_INT,
      0,
      0,
      FR_TEST_PERSIST_RECORD_END,
  };
#endif
#if !FR_FEATURE_CELLS
  uint8_t cell_payload[] = {
      'F',
      'R',
      'P',
      'O',
      0,
      FR_TEST_PERSIST_RECORD_CELLS,
      0,
      0,
      0,
      0,
      FR_TEST_PERSIST_VALUE_INT,
      0,
      0,
      FR_TEST_PERSIST_RECORD_END,
  };
#endif
#if !FR_FEATURE_TEXT
  uint8_t text_payload[] = {
      'F',
      'R',
      'P',
      'O',
      0,
      FR_TEST_PERSIST_RECORD_TEXT,
      0,
      0,
      1,
      0,
      'x',
      FR_TEST_PERSIST_RECORD_END,
  };
#endif
#if !FR_FEATURE_RECORDS
  uint8_t record_shape_payload[] = {
      'F',
      'R',
      'P',
      'O',
      0,
      FR_TEST_PERSIST_RECORD_RECORD_SHAPE,
      0,
      0,
      1,
      0,
      'P',
      1,
      0,
      1,
      0,
      'x',
      FR_TEST_PERSIST_RECORD_END,
  };
#endif
#if FR_FEATURE_RECORDS
  uint8_t record_before_shape_payload[] = {
      'F',
      'R',
      'P',
      'O',
      0,
      FR_TEST_PERSIST_RECORD_RECORD,
      0,
      0,
      0,
      0,
      FR_TEST_PERSIST_RECORD_END,
  };
#endif
  fr_platform_storage_debug_reset();
#if !FR_FEATURE_CELLS
  (void)expression;
#endif
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0 && FR_WORD_SIZE == 16
  write_u16_little_endian(&old_name_free_payload[6],
                          FR_TEST_FIRST_USER_SLOT);
  write_u16_little_endian(&old_name_free_payload[9], (uint16_t)(int16_t)13);
#endif
#if !FR_FEATURE_CELLS
  write_u16_little_endian(&cell_payload[8], 1);
  write_u16_little_endian(&cell_payload[11], (uint16_t)(int16_t)7);
#endif

  CHECK("persist restore missing leaves base",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_ERR_NOT_FOUND &&
            fr_slot_read(&runtime, FR_SLOT_BOOT, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("persist payload rejects other word-size version",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_payload_restore(
                &runtime, wrong_version_payload,
                (uint16_t)sizeof(wrong_version_payload)) == FR_ERR_CORRUPT);
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0 && FR_WORD_SIZE == 16
  CHECK("persist restores old name-free payload",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_payload_restore(&runtime, old_name_free_payload,
                                       (uint16_t)sizeof(old_name_free_payload)) ==
                FR_OK &&
            fr_slot_read(&runtime, FR_TEST_FIRST_USER_SLOT, &tagged) ==
                FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 13 &&
            fr_slot_id_for_name(&runtime, "legacy", &slot_id) ==
                FR_ERR_NOT_FOUND);
#endif
#if !FR_FEATURE_CELLS
  CHECK("persist payload rejects compiled-out cell records",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_payload_restore(&runtime, cell_payload,
                                       (uint16_t)sizeof(cell_payload)) ==
                FR_ERR_UNSUPPORTED);
#endif
#if !FR_FEATURE_TEXT
  CHECK("persist payload rejects compiled-out text records",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_payload_restore(&runtime, text_payload,
                                       (uint16_t)sizeof(text_payload)) ==
                FR_ERR_UNSUPPORTED);
#endif
#if !FR_FEATURE_RECORDS
  CHECK("persist payload rejects compiled-out record shapes",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_payload_restore(
                &runtime, record_shape_payload,
                (uint16_t)sizeof(record_shape_payload)) ==
                FR_ERR_UNSUPPORTED);
#endif
#if FR_FEATURE_RECORDS
  CHECK("persist payload rejects record before shape",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_payload_restore(
                &runtime, record_before_shape_payload,
                (uint16_t)sizeof(record_before_shape_payload)) ==
                FR_ERR_CORRUPT);
#endif
  CHECK("persist save mechanical boot",
        fr_compile_overlay_update(
            "boot is fn [ pin: $led_builtin, 1; ms: 100; "
            "pin: $led_builtin, 0; ms: 100 ]",
            &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged) &&
            fr_persist_save(&runtime) == FR_OK &&
            fr_persist_debug_last_payload_bytes() > 0 &&
            fr_persist_debug_last_payload_bytes() < FR_PROFILE_PERSISTENCE_BYTES);
  CHECK("persist header stores profile hash",
        fr_platform_storage_read(0, 0, header, (uint16_t)sizeof(header)) ==
                FR_OK &&
            read_u32_little_endian(&header[FR_PERSIST_PROFILE_HASH_OFFSET]) ==
                fr_persist_debug_profile_hash());
  CHECK("persist restore mechanical boot",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("persist ignores partial inactive payload",
        fr_platform_storage_write(1, FR_PERSIST_HEADER_BYTES, &junk, 1) ==
                FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("persist saves newer generation",
        fr_compile_overlay_update("boot is fn [ one ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
#if FR_FEATURE_HANDLES
  CHECK("persist rejects volatile handles without replacing saved payload",
        (handle_payload_bytes = fr_persist_debug_last_payload_bytes(), true) &&
            fr_handle_reserve(&runtime, FR_TEST_SYNTHETIC_HANDLE_KIND, &handle_ref,
                              &handle_tagged) == FR_OK &&
            fr_handle_activate(&runtime, handle_ref, 5) == FR_OK &&
            fr_slot_write(&runtime, FR_SLOT_BOOT, handle_tagged) == FR_OK &&
            fr_persist_save(&runtime) == FR_ERR_VOLATILE &&
            fr_persist_debug_last_payload_bytes() == handle_payload_bytes &&
            fr_base_image_install(&handle_restore_runtime) == FR_OK &&
            fr_persist_restore(&handle_restore_runtime) == FR_OK &&
            fr_vm_run_boot(&handle_restore_runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1 &&
            fr_handle_close(&runtime, handle_ref) == FR_OK);
#endif
  CHECK("persist corrupt newer payload falls back",
        fr_platform_storage_write(1, FR_PERSIST_HEADER_BYTES + 5, &junk, 1) ==
                FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
#if FR_WORD_SIZE == 32
  CHECK("persist saves roomier int",
        fr_compile_overlay_update("boot is 100000", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_slot_read(&runtime, FR_SLOT_BOOT, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK &&
            decoded == 100000);
  {
    /* Drive payload well past the 16-bit profile 512-byte ceiling so the
       uint16_t writer/reader cursors are exercised at 32-bit-profile scale. */
    char line[40];
    uint16_t saturated_bytes = 0;
    fr_base_image_install(&runtime);
    for (uint16_t i = 0; i < FR_PROFILE_MAX_OVERLAY_NAMES; i++) {
      (void)snprintf(line, sizeof(line), "saturated_overlay_%02u is %u",
                     (unsigned)i, 100000u + i);
      if (fr_compile_overlay_update_for_runtime(&runtime, line, &update) !=
          FR_OK) {
        break;
      }
      if (fr_overlay_apply(&runtime, &update.overlay_update) != FR_OK) {
        break;
      }
    }
    CHECK("persist 32-bit saturated payload cursor stays bounded",
          fr_persist_save(&runtime) == FR_OK &&
              (saturated_bytes = fr_persist_debug_last_payload_bytes()) >
                  1024 &&
              saturated_bytes < FR_PROFILE_PERSISTENCE_BYTES);
    CHECK("persist 32-bit saturated payload round-trips",
          fr_base_image_install(&runtime) == FR_OK &&
              fr_persist_restore(&runtime) == FR_OK);
  }
#endif
  CHECK("persist restores dynamic slot ids",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "led is $led_builtin", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "boot is fn [ pin: led, 1 ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged) &&
            fr_slot_read(&runtime, FR_SLOT_BOOT, &tagged) == FR_OK &&
            fr_tagged_decode_code_object_id(tagged, &code_id) == FR_OK &&
            fr_slot_read(&runtime, FR_TEST_FIRST_USER_SLOT, &tagged) ==
                FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 13);
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  CHECK("persist restores dynamic slot names",
        fr_slot_id_for_name(&runtime, "led", &slot_id) == FR_OK &&
            slot_id == FR_TEST_FIRST_USER_SLOT);
#else
  CHECK("persist omits dynamic slot names without overlay names",
            fr_slot_id_for_name(&runtime, "led", &slot_id) ==
                FR_ERR_NOT_FOUND);
#endif
  CHECK("persist restore survives project clear",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "led is $led_builtin", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "boot is fn [ led ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK &&
            fr_runtime_clear_project(&runtime) == FR_OK &&
            fr_slot_id_for_name(&runtime, "led", &slot_id) ==
                FR_ERR_NOT_FOUND &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged) &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 13 &&
            fr_slot_read(&runtime, FR_SLOT_BOOT, &tagged) == FR_OK &&
            fr_tagged_decode_code_object_id(tagged, &code_id) == FR_OK &&
            fr_slot_read(&runtime, FR_TEST_FIRST_USER_SLOT, &tagged) ==
                FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 13);
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  CHECK("persist restore after clear restores project name",
        fr_slot_id_for_name(&runtime, "led", &slot_id) == FR_OK &&
            slot_id == FR_TEST_FIRST_USER_SLOT);
#endif
  CHECK("persist restores duplicate dynamic code bytes",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "led is $led_builtin", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "boot is fn [ pin: led, 1 ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "myblink is fn [ pin: led, 1 ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged) &&
            fr_vm_run_slot(&runtime, FR_TEST_FIRST_USER_SLOT + 1, &tagged) ==
                FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("persist prepares control flow code",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update(
                "boot is fn [ if 1 [ repeat 2 [ ms: 1 ]; one ] else [ nil ] ]",
                &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("persist saves control flow code", fr_persist_save(&runtime) == FR_OK);
  CHECK("persist restores control flow code",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
  CHECK("persist restores parameter code call",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "echo is fn with value [ value ]", &update) ==
                FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "boot is fn [ echo: 7 ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_vm_run_boot(&runtime, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 7);
#if FR_PROFILE_MAX_OVERLAY_NAMES > 0
  CHECK("persist restores parameter code name",
        fr_slot_id_for_name(&runtime, "echo", &slot_id) == FR_OK &&
            slot_id == FR_TEST_FIRST_USER_SLOT);
#else
  CHECK("persist omits parameter code name without overlay names",
        fr_slot_id_for_name(&runtime, "echo", &slot_id) == FR_ERR_NOT_FOUND);
#endif
#if FR_FEATURE_TEXT
  CHECK("persist restores text definition",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(&runtime,
                                                  "message is \"ready\"",
                                                  &update) == FR_OK &&
            (slot_id = update.slot_inits[0].slot_id, true) &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_slot_read(&runtime, slot_id, &tagged) == FR_OK &&
            fr_tagged_decode_object_id(tagged, &object_id) == FR_OK &&
            fr_text_view(&runtime, object_id, &text_bytes, &text_length) ==
                FR_OK &&
            text_length == 5 && memcmp(text_bytes, "ready", 5) == 0);
  {
    char out[64];

    CHECK("persist restores function with embedded text literal",
          fr_base_image_install(&runtime) == FR_OK &&
              fr_repl_eval_line(&runtime,
                                "to greet with x [ text.concat: \"led=\", "
                                "text.from-int: x ]",
                                out, sizeof(out)) == FR_OK &&
              fr_persist_save(&runtime) == FR_OK &&
              fr_base_image_install(&runtime) == FR_OK &&
              fr_persist_restore(&runtime) == FR_OK &&
              fr_repl_eval_line(&runtime, "labeled is greet: 1", out,
                                sizeof(out)) == FR_OK &&
              fr_repl_eval_line(&runtime, "text.length: labeled", out,
                                sizeof(out)) == FR_OK &&
              strcmp(out, "5\nok\n") == 0 &&
              test_text_bytes_match(&runtime, "labeled", "led=1"));
  }
#if FR_FEATURE_CELLS
  CHECK("persist restores cell-held binary text",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(&runtime,
                                                  "status is cells(1)",
                                                  &update) == FR_OK &&
            (slot_id = update.slot_inits[0].slot_id, true) &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_slot_read(&runtime, slot_id, &tagged) == FR_OK &&
            fr_tagged_decode_object_id(tagged, &object_id) == FR_OK &&
            fr_text_install(&runtime, binary_text,
                            (uint16_t)sizeof(binary_text), &text_id) ==
                FR_OK &&
            fr_tagged_encode_object_id(text_id, &text_tagged) == FR_OK &&
            fr_cells_write(&runtime, object_id, 0, text_tagged) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_slot_read(&runtime, slot_id, &tagged) == FR_OK &&
            fr_tagged_decode_object_id(tagged, &object_id) == FR_OK &&
            fr_cells_read(&runtime, object_id, 0, &tagged) == FR_OK &&
            fr_tagged_decode_object_id(tagged, &text_id) == FR_OK &&
            fr_text_view(&runtime, text_id, &text_bytes, &text_length) ==
                FR_OK &&
            text_length == sizeof(binary_text) &&
            memcmp(text_bytes, binary_text, sizeof(binary_text)) == 0);
#endif
#endif
  {
    char out[32];

    /* Locals ride through persistence as bytecode; the new LOAD_LOCAL /
     * STORE_LOCAL opcodes have no text dependency, so this case stays
     * outside the FR_FEATURE_TEXT block and exercises tiny too. */
    CHECK("persist restores function using here local binding",
          fr_base_image_install(&runtime) == FR_OK &&
              fr_repl_eval_line(&runtime,
                                "to twice with n [ here x is n * 2 ; x ]",
                                out, sizeof(out)) == FR_OK &&
              fr_persist_save(&runtime) == FR_OK &&
              fr_base_image_install(&runtime) == FR_OK &&
              fr_persist_restore(&runtime) == FR_OK &&
              fr_repl_eval_line(&runtime, "twice: 7", out, sizeof(out)) ==
                  FR_OK &&
              strcmp(out, "14\nok\n") == 0);
  }
#if FR_FEATURE_CELLS
  CHECK("persist restores cells",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(&runtime,
                                                  "counter is cells(2)",
                                                  &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_compile_expression_for_runtime(&runtime,
                                              "set counter[0] to "
                                              FR_TEST_PERSIST_INT_SOURCE,
                                              &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &expression.instructions,
                                         &tagged) == FR_OK &&
            fr_compile_expression_for_runtime(&runtime,
                                              "set counter[1] to one",
                                              &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &expression.instructions,
                                         &tagged) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_compile_expression_for_runtime(&runtime, "counter[0]",
                                              &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &expression.instructions,
                                         &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK &&
            decoded == FR_TEST_PERSIST_INT_VALUE &&
            fr_compile_expression_for_runtime(&runtime, "counter[1]",
                                              &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &expression.instructions,
                                         &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 1);
#endif
#if FR_FEATURE_RECORDS
  CHECK("persist restores records with text fields",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, "record Point [ x, label ]", &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_compile_overlay_update_for_runtime(
                &runtime, FR_TEST_PERSIST_RECORD_INIT, &update) ==
                FR_OK &&
            (slot_id = update.slot_inits[0].slot_id, true) &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_compile_expression_for_runtime(&runtime,
                                              FR_TEST_PERSIST_RECORD_SET,
                                              &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &expression.instructions,
                                         &tagged) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_compile_expression_for_runtime(&runtime, "point->x",
                                              &expression) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &expression.instructions,
                                         &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK &&
            decoded == FR_TEST_PERSIST_RECORD_VALUE &&
            fr_slot_read(&runtime, slot_id, &tagged) == FR_OK &&
            fr_tagged_decode_object_id(tagged, &object_id) == FR_OK &&
            fr_record_read_field(
                &runtime, object_id,
                (fr_record_name_t){.bytes = (const uint8_t *)"label",
                                   .length = 5},
                &tagged) == FR_OK &&
            fr_tagged_decode_object_id(tagged, &text_id) == FR_OK &&
            fr_text_view(&runtime, text_id, &text_bytes, &text_length) ==
                FR_OK &&
            text_length == 5 && memcmp(text_bytes, "ready", 5) == 0);
#if FR_FEATURE_CELLS
  CHECK("persist restores cell-held record ref",
        fr_compile_overlay_update_for_runtime(&runtime,
                                              "holder is cells(1)",
                                              &update) == FR_OK &&
            fr_overlay_apply(&runtime, &update.overlay_update) == FR_OK &&
            fr_slot_id_for_name(&runtime, "holder", &slot_id) == FR_OK &&
            fr_slot_read(&runtime, slot_id, &tagged) == FR_OK &&
            fr_tagged_decode_object_id(tagged, &object_id) == FR_OK &&
            fr_slot_id_for_name(&runtime, "point", &slot_id) == FR_OK &&
            fr_slot_read(&runtime, slot_id, &tagged) == FR_OK &&
            fr_cells_write(&runtime, object_id, 0, tagged) == FR_OK &&
            fr_persist_save(&runtime) == FR_OK &&
            fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_slot_id_for_name(&runtime, "holder", &slot_id) == FR_OK &&
            fr_slot_read(&runtime, slot_id, &tagged) == FR_OK &&
            fr_tagged_decode_object_id(tagged, &object_id) == FR_OK &&
            fr_cells_read(&runtime, object_id, 0, &tagged) == FR_OK &&
            fr_tagged_decode_object_id(tagged, &object_id) == FR_OK &&
            fr_record_read_field(
                &runtime, object_id,
                (fr_record_name_t){.bytes = (const uint8_t *)"x",
                                   .length = 1},
                &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK &&
            decoded == FR_TEST_PERSIST_RECORD_VALUE);
#endif
#endif
  CHECK("persist wipe clears stored overlay",
        fr_persist_wipe(&runtime) == FR_OK &&
            fr_slot_read(&runtime, FR_SLOT_BOOT, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged) &&
            fr_persist_restore(&runtime) == FR_ERR_NOT_FOUND);
  CHECK("persist wipe frees session overlay names",
        fr_compile_overlay_update_for_runtime(
            &runtime, "led is $led_builtin", &update) == FR_OK &&
            update.slot_inits[0].slot_id == FR_TEST_FIRST_USER_SLOT);
}
#elif FR_FEATURE_PERSISTENCE
static void test_persist(void) {
  fr_runtime_t runtime;
  fr_tagged_t tagged = 0;
  uint8_t header[FR_PERSIST_HEADER_BYTES];
#if FR_PROFILE_MAX_OVERLAY_NAMES == 0
  uint8_t named_payload[] = {
      'F',
      'R',
      'P',
      'O',
      0,
      FR_TEST_PERSIST_RECORD_NAME,
      0,
      0,
      1,
      'x',
      FR_TEST_PERSIST_RECORD_END,
  };
#endif
#if !FR_FEATURE_CELLS
  uint8_t cell_payload[] = {
      'F',
      'R',
      'P',
      'O',
      0,
      FR_TEST_PERSIST_RECORD_CELLS,
      0,
      0,
      0,
      0,
      FR_TEST_PERSIST_VALUE_INT,
      0,
      0,
      FR_TEST_PERSIST_RECORD_END,
  };
#endif
#if !FR_FEATURE_TEXT
  uint8_t text_payload[] = {
      'F',
      'R',
      'P',
      'O',
      0,
      FR_TEST_PERSIST_RECORD_TEXT,
      0,
      0,
      1,
      0,
      'x',
      FR_TEST_PERSIST_RECORD_END,
  };
#endif
#if !FR_FEATURE_RECORDS
  uint8_t record_shape_payload[] = {
      'F',
      'R',
      'P',
      'O',
      0,
      FR_TEST_PERSIST_RECORD_RECORD_SHAPE,
      0,
      0,
      1,
      0,
      'P',
      1,
      0,
      1,
      0,
      'x',
      FR_TEST_PERSIST_RECORD_END,
  };
#endif

  fr_platform_storage_debug_reset();
#if FR_PROFILE_MAX_OVERLAY_NAMES == 0
  write_u16_little_endian(&named_payload[6], FR_TEST_FIRST_USER_SLOT);
#endif
#if !FR_FEATURE_CELLS
  write_u16_little_endian(&cell_payload[8], 1);
  write_u16_little_endian(&cell_payload[11], (uint16_t)(int16_t)7);
#endif

  CHECK("persist restore missing leaves base without compiler",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_ERR_NOT_FOUND &&
            fr_slot_read(&runtime, FR_SLOT_BOOT, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
#if FR_PROFILE_MAX_OVERLAY_NAMES == 0
  CHECK("persist payload rejects compiled-out name records",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_payload_restore(&runtime, named_payload,
                                       (uint16_t)sizeof(named_payload)) ==
                FR_ERR_UNSUPPORTED);
#endif
#if !FR_FEATURE_CELLS
  CHECK("persist payload rejects compiled-out cell records without compiler",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_payload_restore(&runtime, cell_payload,
                                       (uint16_t)sizeof(cell_payload)) ==
                FR_ERR_UNSUPPORTED);
#endif
#if !FR_FEATURE_TEXT
  CHECK("persist payload rejects compiled-out text records without compiler",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_payload_restore(&runtime, text_payload,
                                       (uint16_t)sizeof(text_payload)) ==
                FR_ERR_UNSUPPORTED);
#endif
#if !FR_FEATURE_RECORDS
  CHECK("persist payload rejects compiled-out record shapes without compiler",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_payload_restore(
                &runtime, record_shape_payload,
                (uint16_t)sizeof(record_shape_payload)) ==
                FR_ERR_UNSUPPORTED);
#endif
  CHECK("persist save base without compiler",
        fr_persist_save(&runtime) == FR_OK &&
            fr_persist_debug_last_payload_bytes() > 0 &&
            fr_persist_debug_last_payload_bytes() < FR_PROFILE_PERSISTENCE_BYTES);
  CHECK("persist header stores profile hash without compiler",
        fr_platform_storage_read(0, 0, header, (uint16_t)sizeof(header)) ==
                FR_OK &&
            read_u32_little_endian(&header[FR_PERSIST_PROFILE_HASH_OFFSET]) ==
                fr_persist_debug_profile_hash());
  CHECK("persist restore base without compiler",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_OK &&
            fr_slot_read(&runtime, FR_SLOT_BOOT, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("persist wipe without compiler",
        fr_persist_wipe(&runtime) == FR_OK &&
            fr_persist_restore(&runtime) == FR_ERR_NOT_FOUND);
}
#endif

static void test_vm(void) {
  fr_runtime_t runtime;
  fr_instruction_stream_t view;
  fr_tagged_t result = 0;
  fr_tagged_t tagged = 0;
  fr_int_t decoded = 0;
  uint8_t return_bytes[] = {0x00, 0x00, FR_OP_RETURN};
  uint8_t push_one[] = {0x00, 0x00, FR_TEST_PUSH_INT(1), FR_OP_RETURN};
  uint8_t push_nil[] = {0x00, 0x00, FR_OP_PUSH_NIL, FR_OP_RETURN};
  uint8_t add_two_three[] = {0x00, 0x00, FR_TEST_PUSH_INT(2),
                             FR_TEST_PUSH_INT(3), FR_OP_ADD_INT,
                             FR_OP_RETURN};
  uint8_t add_overflow[] = {0x00, 0x00, FR_TEST_PUSH_INT(FR_TAGGED_INT_MAX),
                            FR_TEST_PUSH_INT(1), FR_OP_ADD_INT, FR_OP_RETURN};
  /* Sum sits outside the tagged band, so FR_ERR_RANGE is expected. The
   * wide-temp width rule is enforced by the typedef checks next to
   * fr_vm_add_int in src/vm.c, not by this case. */
  uint8_t add_partition[] = {0x00, 0x00, FR_TEST_PUSH_INT(FR_TAGGED_INT_MAX),
                             FR_TEST_PUSH_INT(FR_TAGGED_INT_MAX),
                             FR_OP_ADD_INT, FR_OP_RETURN};
  uint8_t add_underflow[] = {0x00, 0x00, FR_TEST_PUSH_INT(FR_TAGGED_INT_MIN),
                             FR_TEST_PUSH_INT(-1), FR_OP_ADD_INT,
                             FR_OP_RETURN};
  uint8_t add_partition_low[] = {0x00, 0x00, FR_TEST_PUSH_INT(FR_TAGGED_INT_MIN),
                                 FR_TEST_PUSH_INT(FR_TAGGED_INT_MIN),
                                 FR_OP_ADD_INT, FR_OP_RETURN};
  uint8_t add_type_error[] = {
      0x00, 0x00, FR_OP_LOAD_SLOT, 0x00, 0x00, FR_TEST_PUSH_INT(1),
      FR_OP_ADD_INT, FR_OP_RETURN};
  uint8_t store_slot_zero[] = {0x00, 0x00, FR_TEST_PUSH_INT(7),
                               FR_OP_STORE_SLOT, 0x00, 0x00, FR_OP_RETURN};
  uint8_t load_slot_zero[] = {0x00, 0x00, FR_OP_LOAD_SLOT,
                              0x00, 0x00, FR_OP_RETURN};
#if FR_FEATURE_CELLS
  uint8_t load_cell_zero[] = {0x00, 0x00, FR_OP_LOAD_CELL, 0x00,
                              0x00, 0x00, 0x00, FR_OP_RETURN};
  uint8_t store_cell_zero[] = {0x00, 0x00, FR_TEST_PUSH_INT(9),
                               FR_OP_STORE_CELL, 0x00, 0x00, 0x00, 0x00,
                               FR_OP_RETURN};
#endif
  uint8_t load_slot_too_large[] = {0x00, 0x00, FR_OP_LOAD_SLOT,
                                   0x00, 0x00, FR_OP_RETURN};
  uint8_t store_underflow[] = {0x00, 0x00, FR_OP_STORE_SLOT,
                               0x00, 0x00, FR_OP_RETURN};
  uint8_t call_slot_one[] = {0x00, 0x00, FR_OP_CALL_SLOT,
                             0x00, 0x00, FR_OP_RETURN};
  uint8_t arg_identity[] = {0x00, 0x00, 0x00, FR_OP_LOAD_ARG, 0x00,
                            FR_OP_RETURN};
  uint8_t call_slot_arg_one[] = {0x00, 0x00, FR_TEST_PUSH_INT(7),
                                 FR_OP_CALL_SLOT_ARG, 0x00, 0x00, 0x00,
                                 FR_OP_RETURN};
  uint8_t call_slot_arg_mismatch[] = {0x00, 0x00, FR_OP_CALL_SLOT_ARG,
                                      0x00, 0x00, 0x00, FR_OP_RETURN};
  uint8_t call_native_slot_two[] = {0x00, 0x00, FR_OP_CALL_NATIVE_SLOT,
                                    0x00, 0x00, FR_OP_RETURN};
  uint8_t call_native_add[] = {0x00, 0x00, FR_TEST_PUSH_INT(2),
                               FR_TEST_PUSH_INT(3), FR_OP_CALL_NATIVE_SLOT,
                               0x00, 0x00, FR_OP_RETURN};
  uint8_t jump_push_two[] = {
      0x00, 0x00, FR_OP_JUMP, 0x00, 0x00, FR_TEST_PUSH_INT(1),
      FR_TEST_PUSH_INT(2), FR_OP_RETURN};
  uint8_t jump_if_falsy_false[] = {
      0x00, 0x00, FR_TEST_PUSH_INT(0), FR_OP_JUMP_IF_FALSY, 0x00, 0x00,
      FR_TEST_PUSH_INT(1), FR_OP_JUMP, 0x00, 0x00, FR_TEST_PUSH_INT(2),
      FR_OP_RETURN};
  uint8_t jump_if_falsy_true[] = {
      0x00, 0x00, FR_TEST_PUSH_INT(1), FR_OP_JUMP_IF_FALSY, 0x00, 0x00,
      FR_TEST_PUSH_INT(1), FR_OP_JUMP, 0x00, 0x00, FR_TEST_PUSH_INT(2),
      FR_OP_RETURN};
  uint8_t repeat_two[] = {
      0x00, 0x00, FR_TEST_PUSH_INT(2), FR_OP_REPEAT_BEGIN, 0x00, 0x00,
      FR_TEST_PUSH_INT(1), FR_OP_DROP, FR_OP_REPEAT_NEXT, 0x00, 0x00,
      FR_OP_PUSH_NIL, FR_OP_RETURN};
  uint8_t repeat_zero[] = {
      0x00, 0x00, FR_TEST_PUSH_INT(0), FR_OP_REPEAT_BEGIN, 0x00, 0x00,
      FR_TEST_PUSH_INT(1), FR_OP_DROP, FR_OP_REPEAT_NEXT, 0x00, 0x00,
      FR_OP_PUSH_NIL, FR_OP_RETURN};
  uint8_t repeat_negative[] = {
      0x00, 0x00, FR_TEST_PUSH_INT(-1), FR_OP_REPEAT_BEGIN, 0x00, 0x00,
      FR_TEST_PUSH_INT(1), FR_OP_DROP, FR_OP_REPEAT_NEXT, 0x00, 0x00,
      FR_OP_PUSH_NIL, FR_OP_RETURN};
  uint8_t interrupt_loop[] = {0x00, 0x00, FR_OP_CALL_NATIVE_SLOT,
                              0x00, 0x00, FR_OP_STORE_SLOT,
                              0x00, 0x00, FR_OP_JUMP,
                              0x00, 0x00};
  fr_code_object_id_t code_object_id = 0;
  fr_code_object_id_t arg_code_object_id = 0;
  fr_code_object_id_t recursive_code_object_id = 0;
  fr_code_object_id_t decoded_code_object_id = 0;
  fr_native_id_t native_id = 0;
  fr_native_id_t add_native_id = 0;
  fr_native_id_t error_native_id = 0;
  fr_native_id_t interrupt_native_id = 0;
#if FR_FEATURE_CELLS
  fr_object_id_t object_id = 0;
  fr_tagged_t cell_initial = 0;
#endif
  const fr_code_offset_t store_slot_ip =
      2u + FR_INSTRUCTION_PUSH_INT_SIZE;
#if FR_FEATURE_CELLS
  const fr_code_offset_t store_cell_ip =
      2u + FR_INSTRUCTION_PUSH_INT_SIZE;
#endif
  const fr_code_offset_t call_slot_arg_ip =
      2u + FR_INSTRUCTION_PUSH_INT_SIZE;
  const fr_code_offset_t call_native_add_ip =
      2u + (FR_INSTRUCTION_PUSH_INT_SIZE * 2u);
  const fr_code_offset_t jump_first_push_ip = 5u;
  const fr_code_offset_t jump_second_push_ip =
      jump_first_push_ip + FR_INSTRUCTION_PUSH_INT_SIZE;
  const fr_code_offset_t jump_if_ip =
      2u + FR_INSTRUCTION_PUSH_INT_SIZE;
  const fr_code_offset_t jump_if_then_ip = jump_if_ip + 3u;
  const fr_code_offset_t jump_if_after_then_ip =
      jump_if_then_ip + FR_INSTRUCTION_PUSH_INT_SIZE;
  const fr_code_offset_t jump_if_else_ip = jump_if_after_then_ip + 3u;
  const fr_code_offset_t jump_if_done_ip =
      jump_if_else_ip + FR_INSTRUCTION_PUSH_INT_SIZE;
  const fr_code_offset_t repeat_begin_ip =
      2u + FR_INSTRUCTION_PUSH_INT_SIZE;
  const fr_code_offset_t repeat_body_ip = repeat_begin_ip + 3u;
  const fr_code_offset_t repeat_drop_ip =
      repeat_body_ip + FR_INSTRUCTION_PUSH_INT_SIZE;
  const fr_code_offset_t repeat_next_ip = repeat_drop_ip + 1u;
  const fr_code_offset_t repeat_done_ip = repeat_next_ip + 3u;

  write_instruction_header(return_bytes, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(push_one, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(push_nil, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(add_two_three, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(add_overflow, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(add_partition, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(add_underflow, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(add_partition_low, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(add_type_error, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(store_slot_zero, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(load_slot_zero, FR_INSTRUCTION_MIN_HEADER_SIZE);
#if FR_FEATURE_CELLS
  write_instruction_header(load_cell_zero, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(store_cell_zero, FR_INSTRUCTION_MIN_HEADER_SIZE);
#endif
  write_instruction_header(load_slot_too_large,
                           FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(store_underflow, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(call_slot_one, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header_arity(arg_identity, 1);
  write_instruction_header(call_slot_arg_one, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(call_slot_arg_mismatch,
                           FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(call_native_slot_two,
                           FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(call_native_add, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(jump_push_two, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(jump_if_falsy_false, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(jump_if_falsy_true, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(repeat_two, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(repeat_zero, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(repeat_negative, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(interrupt_loop, FR_INSTRUCTION_MIN_HEADER_SIZE);

  write_slot_operand(&add_type_error[3], 0);
  write_slot_operand(&store_slot_zero[store_slot_ip + 1u], 0);
  write_slot_operand(&load_slot_zero[3], 0);
#if FR_FEATURE_CELLS
  write_cell_operands(&load_cell_zero[3], 5, 0);
  write_cell_operands(&store_cell_zero[store_cell_ip + 1u], 5, 0);
#endif
  write_slot_operand(&load_slot_too_large[3], FR_PROFILE_MAX_SLOTS);
  write_slot_operand(&store_underflow[3], 0);
  write_slot_operand(&call_slot_one[3], 1);
  arg_identity[4] = 0;
  write_call_slot_arg_operands(&call_slot_arg_one[call_slot_arg_ip + 1u], 1,
                               1);
  write_call_slot_arg_operands(&call_slot_arg_mismatch[3], 1, 0);
  write_slot_operand(&call_native_slot_two[3], 2);
  write_slot_operand(&call_native_add[call_native_add_ip + 1u], 4);
  write_jump_operand(&jump_push_two[3], jump_second_push_ip);
  write_jump_operand(&jump_if_falsy_false[jump_if_ip + 1u], jump_if_else_ip);
  write_jump_operand(&jump_if_falsy_false[jump_if_after_then_ip + 1u],
                     jump_if_done_ip);
  write_jump_operand(&jump_if_falsy_true[jump_if_ip + 1u], jump_if_else_ip);
  write_jump_operand(&jump_if_falsy_true[jump_if_after_then_ip + 1u],
                     jump_if_done_ip);
  write_jump_operand(&repeat_two[repeat_begin_ip + 1u], repeat_done_ip);
  write_jump_operand(&repeat_two[repeat_next_ip + 1u], repeat_body_ip);
  write_jump_operand(&repeat_zero[repeat_begin_ip + 1u], repeat_done_ip);
  write_jump_operand(&repeat_zero[repeat_next_ip + 1u], repeat_body_ip);
  write_jump_operand(&repeat_negative[repeat_begin_ip + 1u], repeat_done_ip);
  write_jump_operand(&repeat_negative[repeat_next_ip + 1u], repeat_body_ip);
  write_slot_operand(&interrupt_loop[3], 3);
  write_slot_operand(&interrupt_loop[6], 0);
  write_jump_operand(&interrupt_loop[9], 2);

  CHECK("vm runtime init", fr_runtime_init(&runtime) == FR_OK);
  CHECK("vm return nil",
        fr_instruction_stream_init(&view, return_bytes, sizeof(return_bytes)) ==
                FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_is_nil(result));
  CHECK("vm push int",
        fr_instruction_stream_init(&view, push_one, sizeof(push_one)) ==
                FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 1);
  CHECK("vm push nil",
        fr_instruction_stream_init(&view, push_nil, sizeof(push_nil)) ==
                FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_is_nil(result));
  CHECK("vm add int",
        fr_instruction_stream_init(&view, add_two_three,
                                   sizeof(add_two_three)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 5);
  CHECK("vm add rejects overflow",
        fr_instruction_stream_init(&view, add_overflow, sizeof(add_overflow)) ==
                FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_RANGE);
  CHECK("vm add rejects max+max overflow",
        fr_instruction_stream_init(&view, add_partition,
                                   sizeof(add_partition)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_RANGE);
  CHECK("vm add rejects underflow",
        fr_instruction_stream_init(&view, add_underflow,
                                   sizeof(add_underflow)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_RANGE);
  CHECK("vm add rejects min+min underflow",
        fr_instruction_stream_init(&view, add_partition_low,
                                   sizeof(add_partition_low)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_RANGE);
  CHECK("vm add rejects non-int",
        fr_instruction_stream_init(&view, add_type_error,
                                   sizeof(add_type_error)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_TYPE);
  CHECK("vm jump",
        fr_instruction_stream_init(&view, jump_push_two,
                                   sizeof(jump_push_two)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 2);
  CHECK("vm jump if falsy jumps",
        fr_instruction_stream_init(&view, jump_if_falsy_false,
                                   sizeof(jump_if_falsy_false)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 2);
  CHECK("vm jump if falsy falls through",
        fr_instruction_stream_init(&view, jump_if_falsy_true,
                                   sizeof(jump_if_falsy_true)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 1);
  CHECK("vm repeat runs dynamic count",
        fr_instruction_stream_init(&view, repeat_two, sizeof(repeat_two)) ==
                FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_is_nil(result));
  CHECK("vm repeat zero skips body",
        fr_instruction_stream_init(&view, repeat_zero, sizeof(repeat_zero)) ==
                FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_is_nil(result));
  CHECK("vm repeat rejects negative count",
        fr_instruction_stream_init(&view, repeat_negative,
                                   sizeof(repeat_negative)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_RANGE);
  CHECK("vm store slot",
        fr_instruction_stream_init(&view, store_slot_zero,
                                   sizeof(store_slot_zero)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_is_nil(result) &&
            fr_slot_read(&runtime, 0, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 7);
  CHECK("vm load slot",
        fr_instruction_stream_init(&view, load_slot_zero,
                                   sizeof(load_slot_zero)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 7);
#if FR_FEATURE_CELLS
  CHECK("vm load cell",
        fr_tagged_encode_int(1, &cell_initial) == FR_OK &&
            fr_cells_install(&runtime, 1, &cell_initial, &object_id) ==
                FR_OK &&
            fr_tagged_encode_object_id(object_id, &tagged) == FR_OK &&
            fr_slot_write(&runtime, 5, tagged) == FR_OK &&
            fr_instruction_stream_init(&view, load_cell_zero,
                                       sizeof(load_cell_zero)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 1);
  CHECK("vm store cell",
        fr_instruction_stream_init(&view, store_cell_zero,
                                   sizeof(store_cell_zero)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_is_nil(result) &&
            fr_cells_read(&runtime, object_id, 0, &tagged) == FR_OK &&
            fr_tagged_decode_int(tagged, &decoded) == FR_OK && decoded == 9);
  CHECK("vm load cell rejects non-cell slot",
        fr_slot_write(&runtime, 5, cell_initial) == FR_OK &&
            fr_instruction_stream_init(&view, load_cell_zero,
                                       sizeof(load_cell_zero)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_TYPE);
#endif
  CHECK("vm load slot rejects range",
        fr_instruction_stream_init(&view, load_slot_too_large,
                                   sizeof(load_slot_too_large)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_RANGE);
  CHECK("vm rejects invalid instruction stream",
        fr_instruction_stream_init(&view, store_underflow,
                                   sizeof(store_underflow)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_UNDERFLOW);
  CHECK("code install for vm",
        fr_instruction_stream_init(&view, push_one, sizeof(push_one)) ==
                FR_OK &&
            fr_code_install(&runtime, &view, NULL, 0, &code_object_id) == FR_OK &&
            code_object_id == 0);
  CHECK("code-tagged stores in slot",
        fr_tagged_encode_code_object_id(code_object_id, &tagged) == FR_OK &&
            fr_slot_write(&runtime, 0, tagged) == FR_OK &&
            fr_slot_read(&runtime, 0, &result) == FR_OK &&
            fr_tagged_decode_code_object_id(result, &decoded_code_object_id) ==
                FR_OK &&
            decoded_code_object_id == code_object_id);
  CHECK("vm run code object",
        fr_vm_run_code_object(&runtime, code_object_id, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 1);
  CHECK("vm run slot", fr_vm_run_slot(&runtime, 0, &result) == FR_OK &&
                           fr_tagged_decode_int(result, &decoded) == FR_OK &&
                           decoded == 1);
  CHECK("vm slot rejects non-code",
        fr_slot_write(&runtime, 0, fr_tagged_nil()) == FR_OK &&
            fr_vm_run_slot(&runtime, 0, &result) == FR_ERR_TYPE);
  CHECK("vm slot rejects missing code object",
        fr_tagged_encode_code_object_id(FR_PROFILE_CODE_OBJECT_TABLE_SIZE,
                                        &tagged) == FR_OK &&
            fr_slot_write(&runtime, 0, tagged) == FR_OK &&
            fr_vm_run_slot(&runtime, 0, &result) == FR_ERR_NOT_FOUND);
  CHECK("vm missing code object",
        fr_vm_run_code_object(&runtime, FR_PROFILE_CODE_OBJECT_TABLE_SIZE,
                              &result) == FR_ERR_NOT_FOUND);
  CHECK("boot nil is no-op",
        fr_slot_write(&runtime, FR_SLOT_BOOT, fr_tagged_nil()) == FR_OK &&
            fr_vm_run_boot(&runtime, &result) == FR_OK &&
            fr_tagged_is_nil(result));
  CHECK("boot slot runs code object",
        fr_tagged_encode_code_object_id(code_object_id, &tagged) == FR_OK &&
            fr_slot_write(&runtime, FR_SLOT_BOOT, tagged) == FR_OK &&
            fr_vm_run_boot(&runtime, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 1);
  CHECK("boot rejects non-code",
        fr_slot_write(&runtime, FR_SLOT_BOOT, fr_tagged_true()) == FR_OK &&
            fr_vm_run_boot(&runtime, &result) == FR_ERR_TYPE);
  CHECK("vm call slot",
        fr_tagged_encode_code_object_id(code_object_id, &tagged) == FR_OK &&
            fr_slot_write(&runtime, 1, tagged) == FR_OK &&
            fr_instruction_stream_init(&view, call_slot_one,
                                       sizeof(call_slot_one)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 1);
  CHECK("vm call slot rejects non-code",
        fr_slot_write(&runtime, 1, fr_tagged_false()) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_TYPE);
  CHECK("vm call slot rejects missing code object",
        fr_tagged_encode_code_object_id(FR_PROFILE_CODE_OBJECT_TABLE_SIZE,
                                        &tagged) == FR_OK &&
            fr_slot_write(&runtime, 1, tagged) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_NOT_FOUND);
  CHECK("vm recursive call overflows",
        fr_code_install(&runtime, &view, NULL, 0, &recursive_code_object_id) ==
            FR_OK &&
            fr_tagged_encode_code_object_id(recursive_code_object_id,
                                            &tagged) == FR_OK &&
            fr_slot_write(&runtime, 1, tagged) == FR_OK &&
            fr_vm_run_slot(&runtime, 1, &result) == FR_ERR_OVERFLOW);
  CHECK("code install parameter function for vm",
        fr_instruction_stream_init(&view, arg_identity,
                                   sizeof(arg_identity)) == FR_OK &&
            fr_code_install(&runtime, &view, NULL, 0, &arg_code_object_id) ==
                FR_OK);
  CHECK("vm code object rejects missing argument",
        fr_vm_run_code_object(&runtime, arg_code_object_id, &result) ==
            FR_ERR_INVALID);
  CHECK("vm call slot arg passes argument",
        fr_tagged_encode_code_object_id(arg_code_object_id, &tagged) ==
                FR_OK &&
            fr_slot_write(&runtime, 1, tagged) == FR_OK &&
            fr_instruction_stream_init(&view, call_slot_arg_one,
                                       sizeof(call_slot_arg_one)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 7);
  CHECK("vm call slot rejects parameter arity mismatch",
        fr_instruction_stream_init(&view, call_slot_one,
                                   sizeof(call_slot_one)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_INVALID);
  CHECK("vm call slot arg rejects parameter arity mismatch",
        fr_instruction_stream_init(&view, call_slot_arg_mismatch,
                                   sizeof(call_slot_arg_mismatch)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_INVALID);
  CHECK("vm sees interrupt",
        (fr_runtime_clear_interrupt(&runtime), true) &&
            fr_instruction_stream_init(&view, push_one, sizeof(push_one)) ==
                FR_OK &&
            (fr_runtime_interrupt(&runtime), true) &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_INTERRUPTED);
  CHECK("vm after interrupt clear",
        (fr_runtime_clear_interrupt(&runtime), true) &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 1);
  CHECK("vm slot sees interrupt",
        fr_tagged_encode_code_object_id(code_object_id, &tagged) == FR_OK &&
            fr_slot_write(&runtime, 1, tagged) == FR_OK &&
            (fr_runtime_interrupt(&runtime), true) &&
            fr_vm_run_slot(&runtime, 1, &result) == FR_ERR_INTERRUPTED);
  CHECK("native install for vm",
        (fr_runtime_clear_interrupt(&runtime), true) &&
            fr_native_install(&runtime, test_native_one, 0, NULL, &native_id) ==
                FR_OK &&
            native_id == 0);
  CHECK("vm call native slot",
        fr_tagged_encode_native_id(native_id, &tagged) == FR_OK &&
            fr_slot_write(&runtime, 2, tagged) == FR_OK &&
            fr_instruction_stream_init(&view, call_native_slot_two,
                                       sizeof(call_native_slot_two)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 1);
  CHECK("vm call native slot rejects non-native",
        fr_slot_write(&runtime, 2, fr_tagged_false()) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_TYPE);
  CHECK("vm call native slot rejects missing native",
        fr_tagged_encode_native_id(FR_PROFILE_NATIVE_TABLE_SIZE, &tagged) ==
                FR_OK &&
            fr_slot_write(&runtime, 2, tagged) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_NOT_FOUND);
  CHECK("native add install", fr_native_install(&runtime, test_native_add, 2, NULL,
                                                &add_native_id) == FR_OK);
  CHECK("vm call native rejects missing args",
        fr_tagged_encode_native_id(add_native_id, &tagged) == FR_OK &&
            fr_slot_write(&runtime, 2, tagged) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_UNDERFLOW);
  CHECK("vm call native passes args",
        fr_slot_write(&runtime, 4, tagged) == FR_OK &&
            fr_instruction_stream_init(&view, call_native_add,
                                       sizeof(call_native_add)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 5);
  CHECK("vm native propagates error",
        fr_native_install(&runtime, test_native_error, 0, NULL, &error_native_id) ==
                FR_OK &&
            fr_tagged_encode_native_id(error_native_id, &tagged) == FR_OK &&
            fr_slot_write(&runtime, 2, tagged) == FR_OK &&
            fr_instruction_stream_init(&view, call_native_slot_two,
                                       sizeof(call_native_slot_two)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_IO);
  CHECK("vm loop sees interrupt",
        fr_native_install(&runtime, test_native_interrupt, 0, NULL,
                          &interrupt_native_id) == FR_OK &&
            fr_tagged_encode_native_id(interrupt_native_id, &tagged) == FR_OK &&
            fr_slot_write(&runtime, 3, tagged) == FR_OK &&
            fr_instruction_stream_init(&view, interrupt_loop,
                                       sizeof(interrupt_loop)) == FR_OK &&
            fr_vm_run_instruction_stream(&runtime, &view, &result) ==
                FR_ERR_INTERRUPTED);
  CHECK("vm core allocation audit has no heap calls", true);
}

static void test_repl(void) {
  fr_runtime_t runtime;
  char status_prefix[96];
#if FR_FEATURE_INTROSPECTION
  fr_runtime_t see_runtime;
  fr_slot_id_t see_slot_id = 0;
  char see_line[24];
#if FR_FEATURE_HANDLES
  fr_handle_ref_t see_handle_ref = {0};
  fr_tagged_t see_handle_tagged = 0;
#endif
#endif
#if FR_FEATURE_COMPILER
  fr_tagged_t result = 0;
  fr_int_t decoded = 0;
  fr_slot_id_t slot_id = 0;
#endif
#if FR_FEATURE_OVERLAY_APPLY_COMMAND
  uint8_t apply_code_bytes[] = {0x00, 0x00, FR_TEST_PUSH_INT(1),
                                FR_OP_RETURN};
  uint8_t run_gpio_write_bytes[] = {
      0x00, 0x00, FR_TEST_PUSH_INT(4), FR_TEST_PUSH_INT(1),
      FR_OP_CALL_NATIVE_SLOT, 0x00, 0x00, FR_OP_RETURN};
  uint8_t run_value_bytes[] = {0x00, 0x00, FR_TEST_PUSH_INT(7),
                               FR_OP_RETURN};
  const fr_image_code_object_t apply_code[] = {
      {{apply_code_bytes, sizeof(apply_code_bytes)}, NULL, 0},
  };
  const fr_image_slot_init_t apply_slots[] = {
      {FR_SLOT_BOOT, {FR_IMAGE_REF_CODE_OBJECT, 0, 0}},
  };
  const fr_overlay_update_t apply_update = {
      .slot_inits = apply_slots,
      .slot_init_count = 1,
      .code_objects = apply_code,
      .code_object_count = 1,
  };
  uint8_t apply_bytes[128];
  uint16_t apply_length = 0;
  char apply_line[FR_PROFILE_REPL_LINE_BYTES];
  char run_line[FR_PROFILE_REPL_LINE_BYTES];
  uint8_t apply_limit_bytes[FR_REPL_APPLY_BYTES + 1];
  char apply_limit_line[FR_REPL_LINE_BYTES];
  uint8_t run_limit_bytes[FR_REPL_APPLY_BYTES + 1];
  char run_limit_line[FR_REPL_LINE_BYTES];
  const fr_code_offset_t run_gpio_write_call_ip =
      2u + (FR_INSTRUCTION_PUSH_INT_SIZE * 2u);
#endif
  char out[1024];
#if FR_FEATURE_COMPILER && FR_BASE_IMAGE_INCLUDE_SYMBOLS
  uint16_t before_ms = 0;
  uint16_t after_ms = 0;
#if FR_TAGGED_INT_MAX > 65535
  uint16_t gpio_value = 0;
#endif
  char expected_ms[16];
#endif

#if FR_FEATURE_OVERLAY_APPLY_COMMAND
  write_instruction_header(apply_code_bytes, FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_instruction_header(run_gpio_write_bytes,
                           FR_INSTRUCTION_MIN_HEADER_SIZE);
  write_slot_operand(&run_gpio_write_bytes[run_gpio_write_call_ip + 1u],
                     FR_SLOT_GPIO_WRITE);
  write_instruction_header(run_value_bytes, FR_INSTRUCTION_MIN_HEADER_SIZE);
  memset(apply_limit_bytes, 0, sizeof(apply_limit_bytes));
  memset(run_limit_bytes, 0, sizeof(run_limit_bytes));
#endif

  CHECK("repl installs base image", fr_base_image_install(&runtime) == FR_OK);
  snprintf(status_prefix, sizeof(status_prefix),
           "frothy status v1 profile=%s profile_hash=",
           fr_profile_contract_name());
  CHECK("repl status reports profile contract",
        fr_repl_eval_line(&runtime, "status", out, sizeof(out)) == FR_OK &&
            strncmp(out, status_prefix, strlen(status_prefix)) == 0 &&
            strstr(out, " compiler=") != NULL &&
            strstr(out, fr_profile_compiler_mode()) != NULL &&
            strstr(out, " names=") != NULL &&
            strstr(out, fr_profile_names_mode()) != NULL &&
            strstr(out, " storage=") != NULL &&
            strstr(out, fr_profile_storage_mode()) != NULL &&
            strstr(out, " interrupt=") != NULL &&
            strstr(out, fr_profile_interrupt_mode()) != NULL &&
            strstr(out, " apply_bytes=") != NULL &&
            strstr(out, "\nok\n") != NULL);
  CHECK("repl trims status command",
        fr_repl_eval_line(&runtime, " \tstatus \t", out, sizeof(out)) ==
                FR_OK &&
            strncmp(out, status_prefix, strlen(status_prefix)) == 0);
  {
    char expected_int_min[32];
    char expected_int_max[32];
    (void)snprintf(expected_int_min, sizeof(expected_int_min),
                   " int_min=%" PRId32 " ", (int32_t)FR_TAGGED_INT_MIN);
    (void)snprintf(expected_int_max, sizeof(expected_int_max),
                   " int_max=%" PRId32 " ", (int32_t)FR_TAGGED_INT_MAX);
    CHECK("repl status formats int_min and int_max for active width",
          fr_repl_eval_line(&runtime, "status", out, sizeof(out)) == FR_OK &&
              strstr(out, expected_int_min) != NULL &&
              strstr(out, expected_int_max) != NULL);
  }
#if FR_FEATURE_COMPILER
  CHECK("repl command matching is case-sensitive",
        fr_repl_eval_line(&runtime, "STATUS", out, sizeof(out)) ==
            FR_ERR_NOT_FOUND);
#else
  CHECK("repl command matching is case-sensitive without compiler",
        fr_repl_eval_line(&runtime, "STATUS", out, sizeof(out)) ==
            FR_ERR_UNSUPPORTED);
#endif
  CHECK("repl see command requires one argument",
        fr_repl_eval_line(&runtime, "see", out, sizeof(out)) ==
            FR_ERR_INVALID);
  CHECK("repl see command rejects extra arguments",
        fr_repl_eval_line(&runtime, "see boot extra", out, sizeof(out)) ==
            FR_ERR_INVALID);
#if FR_FEATURE_INTROSPECTION
  CHECK("repl see next numeric slot rejects",
        (snprintf(see_line, sizeof(see_line), "see %u",
                  (unsigned)fr_slot_count(&runtime)),
         true) &&
            fr_repl_eval_line(&runtime, see_line, out, sizeof(out)) ==
                FR_ERR_NOT_FOUND);
  CHECK("repl see unowned project slot rejects",
        fr_base_image_install(&see_runtime) == FR_OK &&
            (see_slot_id = fr_slot_count(&see_runtime), true) &&
            see_slot_id < FR_PROFILE_MAX_SLOTS &&
            fr_slot_write(&see_runtime, see_slot_id, fr_tagged_nil()) ==
                FR_OK &&
            (snprintf(see_line, sizeof(see_line), "see %u",
                      (unsigned)see_slot_id),
             true) &&
            fr_repl_eval_line(&see_runtime, see_line, out, sizeof(out)) ==
                FR_ERR_NOT_FOUND);
#if FR_FEATURE_HANDLES
  CHECK("repl displays volatile handles",
        fr_base_image_install(&see_runtime) == FR_OK &&
            fr_handle_reserve(&see_runtime, FR_TEST_SYNTHETIC_HANDLE_KIND,
                              &see_handle_ref, &see_handle_tagged) == FR_OK &&
            fr_handle_activate(&see_runtime, see_handle_ref, 6) == FR_OK &&
            fr_slot_write(&see_runtime, FR_SLOT_BOOT, see_handle_tagged) ==
                FR_OK &&
            fr_repl_eval_line(&see_runtime, "boot", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "handle " FR_TEST_SYNTHETIC_HANDLE_NAME "\nok\n") == 0 &&
            fr_repl_eval_line(&see_runtime, "see boot", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay volatile " FR_TEST_SYNTHETIC_HANDLE_NAME "\nok\n") == 0 &&
            fr_handle_close(&see_runtime, see_handle_ref) == FR_OK &&
            fr_repl_eval_line(&see_runtime, "see boot", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay volatile closed\nok\n") == 0);
#endif
#endif
  CHECK("repl apply command requires an argument",
        fr_repl_eval_line(&runtime, "apply", out, sizeof(out)) ==
            FR_ERR_INVALID);
  CHECK("repl run command requires an argument",
        fr_repl_eval_line(&runtime, "run", out, sizeof(out)) ==
            FR_ERR_INVALID);
#if !FR_FEATURE_OVERLAY_APPLY_COMMAND
  CHECK("repl rejects disabled apply command before source",
        fr_repl_eval_line(&runtime, "apply 00", out, sizeof(out)) ==
            FR_ERR_UNSUPPORTED);
  CHECK("repl rejects disabled run command before source",
        fr_repl_eval_line(&runtime, "run 00", out, sizeof(out)) ==
            FR_ERR_UNSUPPORTED);
#endif
#if FR_FEATURE_INTROSPECTION
  CHECK("repl words",
        fr_repl_eval_line(&runtime, "words", out, sizeof(out)) == FR_OK &&
            strcmp(out, FR_TEST_WORDS) == 0);
#if !FR_FEATURE_TEXT
  CHECK("repl words omits text natives when text is off",
        fr_repl_eval_line(&runtime, "words", out, sizeof(out)) == FR_OK &&
            strstr(out, "text.length") == NULL &&
            strstr(out, "text.equals?") == NULL &&
            strstr(out, "text.concat") == NULL &&
            strstr(out, "text.at") == NULL &&
            strstr(out, "text.from-int") == NULL);
#endif
#else
  CHECK("repl rejects words without introspection",
        fr_repl_eval_line(&runtime, "words", out, sizeof(out)) ==
            FR_ERR_UNSUPPORTED);
#endif
  CHECK("repl blank line is no-op",
        fr_repl_eval_line(&runtime, " \t", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  CHECK("repl displays bare boot",
        fr_repl_eval_line(&runtime, "boot", out, sizeof(out)) == FR_OK &&
            strcmp(out, "nil\nok\n") == 0);
  CHECK("repl displays literal value",
        fr_repl_eval_line(&runtime, "one", out, sizeof(out)) == FR_OK &&
            strcmp(out, "1\nok\n") == 0);
  CHECK("repl displays board literal value",
        fr_repl_eval_line(&runtime, "$led_builtin", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "13\nok\n") == 0);
#else
  CHECK("repl runs numeric boot",
        fr_repl_eval_line(&runtime, "0:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#endif
#if FR_FEATURE_COMPILER
  CHECK("repl defines overlay alias",
        fr_repl_eval_line(&runtime, "led is $led_builtin", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl displays overlay alias",
        fr_repl_eval_line(&runtime, "led", out, sizeof(out)) == FR_OK &&
            strcmp(out, "13\nok\n") == 0);
  CHECK("repl see overlay alias",
        fr_repl_eval_line(&runtime, "see led", out, sizeof(out)) == FR_OK &&
            strcmp(out, "overlay 13\nok\n") == 0);
  CHECK("repl words includes overlay alias",
        fr_repl_eval_line(&runtime, "words", out, sizeof(out)) == FR_OK &&
            strcmp(out, FR_TEST_WORDS_WITH_LED) == 0);
  CHECK("repl displays gpio.write native value",
        fr_repl_eval_line(&runtime, "gpio.write", out, sizeof(out)) == FR_OK &&
            strcmp(out, "native 1\nok\n") == 0);
  CHECK("repl displays pin sugar native value",
        fr_repl_eval_line(&runtime, "pin", out, sizeof(out)) == FR_OK &&
            strcmp(out, "native 1\nok\n") == 0);
  CHECK("repl see base nil",
        fr_repl_eval_line(&runtime, "see boot", out, sizeof(out)) == FR_OK &&
            strcmp(out, "base core nil\nok\n") == 0);
  CHECK("repl see numeric base nil",
        fr_repl_eval_line(&runtime, "see 0", out, sizeof(out)) == FR_OK &&
            strcmp(out, "base core nil\nok\n") == 0);
  CHECK("repl see base literal value",
        fr_repl_eval_line(&runtime, "see one", out, sizeof(out)) == FR_OK &&
            strcmp(out, "base core 1\nok\n") == 0);
  CHECK("repl see board literal value",
        fr_repl_eval_line(&runtime, "see $led_builtin", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "base board 13\nok\n") == 0);
#if FR_TAGGED_INT_MAX >= 115200
  CHECK("repl evaluates roomier int",
        fr_repl_eval_line(&runtime, "115200", out, sizeof(out)) == FR_OK &&
            strcmp(out, "115200\nok\n") == 0);
#else
  CHECK("repl rejects roomier int",
        fr_repl_eval_line(&runtime, "115200", out, sizeof(out)) ==
            FR_ERR_RANGE);
#endif
#if FR_FEATURE_NATIVE_SIGNATURES
  CHECK("repl see gpio.write renders signature",
        fr_repl_eval_line(&runtime, "see gpio.write", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "gpio.write(pin: int, level: int) -> nil\n"
                   "set gpio pin to a level (0 or 1)\n"
                   "ok\n") == 0);
  CHECK("repl see pin sugar renders signature under canonical name",
        fr_repl_eval_line(&runtime, "see pin", out, sizeof(out)) == FR_OK &&
            strcmp(out,
                   "gpio.write(pin: int, level: int) -> nil\n"
                   "set gpio pin to a level (0 or 1)\n"
                   "ok\n") == 0);
  CHECK("repl see gpio.mode falls back without names or help",
        fr_repl_eval_line(&runtime, "see gpio.mode", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "base target native arity 2\nok\n") == 0);
  CHECK("repl see millis renders signature",
        fr_repl_eval_line(&runtime, "see millis", out, sizeof(out)) == FR_OK &&
            strcmp(out,
                   "millis() -> int\n"
                   "read the millisecond clock since boot\n"
                   "ok\n") == 0);
  CHECK("repl see ms renders signature",
        fr_repl_eval_line(&runtime, "see ms", out, sizeof(out)) == FR_OK &&
            strcmp(out,
                   "ms(millis: int) -> nil\n"
                   "sleep for a number of milliseconds\n"
                   "ok\n") == 0);
  CHECK("repl see gpio.read renders signature",
        fr_repl_eval_line(&runtime, "see gpio.read", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "gpio.read(pin: int) -> int\n"
                   "read the level of a gpio pin\n"
                   "ok\n") == 0);
  CHECK("repl see adc.above renders signature",
        fr_repl_eval_line(&runtime, "see adc.above", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out,
                   "adc.above(pin: int, threshold: int) -> any\n"
                   "true when an adc pin reads above a threshold\n"
                   "ok\n") == 0);
#else
  CHECK("repl see gpio.write falls back on tiny",
        fr_repl_eval_line(&runtime, "see gpio.write", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "base target native arity 2\nok\n") == 0);
#endif
#if FR_FEATURE_PERSISTENCE
#if FR_FEATURE_NATIVE_SIGNATURES
  CHECK("repl see save renders signature",
        fr_repl_eval_line(&runtime, "see save", out, sizeof(out)) == FR_OK &&
            strcmp(out,
                   "save() -> nil\n"
                   "write the current slot image to persistent storage\n"
                   "ok\n") == 0);
  CHECK("repl see restore falls back without help",
        fr_repl_eval_line(&runtime, "see restore", out, sizeof(out)) == FR_OK &&
            strcmp(out, "base persistence native arity 0\nok\n") == 0);
#else
  CHECK("repl see save falls back on tiny",
        fr_repl_eval_line(&runtime, "see save", out, sizeof(out)) == FR_OK &&
            strcmp(out, "base persistence native arity 0\nok\n") == 0);
#endif
#endif
  CHECK("repl runs top-level native call",
        fr_repl_eval_line(&runtime, "pin: $led_builtin, 1", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl runs top-level gpio.write native call",
        fr_repl_eval_line(&runtime, "gpio.write: $led_builtin, 1", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#if FR_TAGGED_INT_MAX > 65535
  CHECK("repl rejects oversized platform integer before cast",
        fr_platform_gpio_write(13, 0) == FR_OK &&
            fr_repl_eval_line(&runtime, "pin: 65549, 1", out, sizeof(out)) ==
                FR_ERR_DOMAIN &&
            fr_platform_gpio_read(13, &gpio_value) == FR_OK &&
            gpio_value == 0);
#endif
  CHECK("repl runs top-level native call with overlay alias",
        fr_repl_eval_line(&runtime, "pin: led, 1", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl runs gpio.mode native",
        fr_repl_eval_line(&runtime, "gpio.mode: $led_builtin, 1", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl reads gpio value",
        fr_repl_eval_line(&runtime, "gpio.read: $led_builtin", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "1\nok\n") == 0);
  CHECK("repl reads adc value",
        fr_repl_eval_line(&runtime, "adc.read: 14", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "512\nok\n") == 0);
  CHECK("repl compares adc threshold",
        fr_repl_eval_line(&runtime, "adc.above: 14, 500", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "true\nok\n") == 0 &&
            fr_repl_eval_line(&runtime, "adc.above: 14, 600", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "false\nok\n") == 0);
  CHECK("repl samples millis before delay",
        fr_platform_millis(&before_ms) == FR_OK);
  snprintf(expected_ms, sizeof(expected_ms), "%u\nok\n", (unsigned)before_ms);
  CHECK("repl reads millis",
        fr_repl_eval_line(&runtime, "millis:", out, sizeof(out)) == FR_OK &&
            strcmp(out, expected_ms) == 0);
  after_ms = (uint16_t)((before_ms + 10u) % 16384u);
  snprintf(expected_ms, sizeof(expected_ms), "%u\nok\n", (unsigned)after_ms);
  CHECK("repl advances millis during ms",
        fr_repl_eval_line(&runtime, "ms: 10", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "millis:", out, sizeof(out)) == FR_OK &&
            strcmp(out, expected_ms) == 0);
  CHECK("repl runs if expression",
        fr_repl_eval_line(&runtime, "if one [ one ] else [ nil ]", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "1\nok\n") == 0);
  CHECK("repl runs false if expression",
        fr_repl_eval_line(&runtime, "if 0 [ one ]", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl runs repeat expression",
        fr_repl_eval_line(&runtime, "repeat 2 [ ms: 1 ]", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl runs repeat with dynamic count",
        fr_repl_eval_line(&runtime, "repeat one [ ms: 1 ]", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl boot colon nil",
        fr_repl_eval_line(&runtime, "boot:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl compile boot",
        fr_repl_eval_line(&runtime, "boot is fn [ 1 ]", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_vm_run_boot(&runtime, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 1);
  CHECK("repl runs boot colon",
        fr_repl_eval_line(&runtime, "boot:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#if FR_FEATURE_SOURCE_BASE
  /* Source-base words take code object ids 0..N-1 at boot compile, so boot's
     id is the source-word count (12 today). */
  CHECK("repl displays bare compiled boot",
        fr_repl_eval_line(&runtime, "boot", out, sizeof(out)) == FR_OK &&
            strcmp(out, "code 12\nok\n") == 0);
#else
  CHECK("repl displays bare compiled boot",
        fr_repl_eval_line(&runtime, "boot", out, sizeof(out)) == FR_OK &&
            strcmp(out, "code 0\nok\n") == 0);
#endif
  CHECK("repl see overlay code",
        fr_repl_eval_line(&runtime, "see boot", out, sizeof(out)) == FR_OK &&
            strcmp(out, "overlay code\nto boot [ 1 ]\nok\n") == 0);
  CHECK("repl see numeric overlay code",
        fr_repl_eval_line(&runtime, "see 0", out, sizeof(out)) == FR_OK &&
            strcmp(out, "overlay code\nto boot [ 1 ]\nok\n") == 0);
  CHECK("repl compiles dynamic function",
        fr_repl_eval_line(&runtime, "myblink is fn [ pin: led, 1 ]", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl runs dynamic function",
        fr_repl_eval_line(&runtime, "myblink:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl see dynamic function",
        fr_repl_eval_line(&runtime, "see myblink", out, sizeof(out)) == FR_OK &&
            strcmp(out,
                   "overlay code\nto myblink [ gpio.write: led, 1 ]\nok\n") ==
                0);
  CHECK("repl words includes dynamic function",
        fr_repl_eval_line(&runtime, "words", out, sizeof(out)) == FR_OK &&
            strcmp(out, FR_TEST_WORDS_WITH_LED_AND_MYBLINK) == 0);
  CHECK("repl redefines overlay name to nil",
        fr_repl_eval_line(&runtime, "led is nil", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl see named overlay nil",
        fr_repl_eval_line(&runtime, "see led", out, sizeof(out)) == FR_OK &&
            strcmp(out, "overlay nil\nok\n") == 0);
  CHECK("repl compiles parameter function",
        fr_repl_eval_line(&runtime, "myblink is fn with value [ value ]", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl runs parameter function call",
        fr_repl_eval_line(&runtime, "myblink: 9", out, sizeof(out)) == FR_OK &&
            strcmp(out, "9\nok\n") == 0);
#if FR_FEATURE_INTROSPECTION
  CHECK("repl see parameter function",
        fr_repl_eval_line(&runtime, "see myblink", out, sizeof(out)) ==
                FR_OK &&
            strcmp(
                out,
                "overlay code\nto myblink with value [ value ]\nok\n") == 0);
#endif
  CHECK("repl compiles boot with parameter call",
        fr_repl_eval_line(&runtime, "boot is fn [ myblink: 7 ]", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_vm_run_boot(&runtime, &result) == FR_OK &&
            fr_tagged_decode_int(result, &decoded) == FR_OK && decoded == 7);
#if FR_FEATURE_CELLS
  CHECK("repl defines cells",
        fr_repl_eval_line(&runtime, "counter is cells(1)", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl displays cells",
        fr_repl_eval_line(&runtime, "counter", out, sizeof(out)) == FR_OK &&
            strcmp(out, "cells 1\nok\n") == 0);
  CHECK("repl see cells",
        fr_repl_eval_line(&runtime, "see counter", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay cells 1\nok\n") == 0);
  CHECK("repl reads nil cell as ok",
        fr_repl_eval_line(&runtime, "counter[0]", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl writes cell",
        fr_repl_eval_line(&runtime, "set counter[0] to 7", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl reads written cell",
        fr_repl_eval_line(&runtime, "counter[0]", out, sizeof(out)) == FR_OK &&
            strcmp(out, "7\nok\n") == 0);
#if FR_FEATURE_TEXT
  CHECK("repl defines text",
        fr_repl_eval_line(&runtime, "message is \"ready\"", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl displays text",
        fr_repl_eval_line(&runtime, "message", out, sizeof(out)) == FR_OK &&
            strcmp(out, "\"ready\"\nok\n") == 0);
  CHECK("repl see text",
        fr_repl_eval_line(&runtime, "see message", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay text 5\nok\n") == 0);
  CHECK("repl stores text in cell",
        fr_repl_eval_line(&runtime, "status is cells(1)", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "set status[0] to message", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl reads text from cell",
        fr_repl_eval_line(&runtime, "status[0]", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "\"ready\"\nok\n") == 0);
#endif
#if FR_FEATURE_RECORDS
  CHECK("repl defines record shape",
        fr_repl_eval_line(&runtime, "record Point [ x, label ]", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl see record shape",
        fr_repl_eval_line(&runtime, "see Point", out, sizeof(out)) == FR_OK &&
            strcmp(out, "overlay record-shape Point [ x, label ]\nok\n") ==
                0);
  CHECK("repl constructs record",
        fr_repl_eval_line(&runtime, "point is Point: 10, \"ready\"", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl displays record",
        fr_repl_eval_line(&runtime, "point", out, sizeof(out)) == FR_OK &&
            strcmp(out, "Point: 10, \"ready\"\nok\n") == 0);
  CHECK("repl see record",
        fr_repl_eval_line(&runtime, "see point", out, sizeof(out)) == FR_OK &&
            strcmp(out, "overlay record Point [ x, label ]\nok\n") == 0);
  CHECK("repl reads record field",
        fr_repl_eval_line(&runtime, "point->x", out, sizeof(out)) == FR_OK &&
            strcmp(out, "10\nok\n") == 0);
  CHECK("repl writes record field",
        fr_repl_eval_line(&runtime, "set point->x to 11", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "point->x", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "11\nok\n") == 0);
  CHECK("repl stores record in cell",
        fr_repl_eval_line(&runtime, "set counter[0] to point", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "counter[0]", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "Point: 11, \"ready\"\nok\n") == 0);
#endif
#endif
  CHECK("repl clear drops project definitions",
        fr_repl_eval_line(&runtime, "clear", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_slot_id_for_name(&runtime, "led", &slot_id) ==
                FR_ERR_NOT_FOUND &&
            fr_vm_run_boot(&runtime, &result) == FR_OK &&
            fr_tagged_is_nil(result) &&
            fr_repl_eval_line(&runtime, "clear", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "words", out, sizeof(out)) == FR_OK &&
            strcmp(out, FR_TEST_WORDS) == 0);
  CHECK("repl source may use exact command names",
        fr_repl_eval_line(&runtime, "status is fn [ one ]", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "status:", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "1\nok\n") == 0 &&
            fr_repl_eval_line(&runtime, "status", out, sizeof(out)) == FR_OK &&
            strncmp(out, status_prefix, strlen(status_prefix)) == 0 &&
            fr_repl_eval_line(&runtime, "clear", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "clear is fn [ one ]", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "clear:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "1\nok\n") == 0 &&
            fr_repl_eval_line(&runtime, "clear", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#else
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  CHECK("repl displays gpio.write native value without compiler",
        fr_repl_eval_line(&runtime, "gpio.write", out, sizeof(out)) == FR_OK &&
            strcmp(out, "native 1\nok\n") == 0);
  CHECK("repl displays pin sugar native value without compiler",
        fr_repl_eval_line(&runtime, "pin", out, sizeof(out)) == FR_OK &&
            strcmp(out, "native 1\nok\n") == 0);
#endif
#if FR_FEATURE_INTROSPECTION
  CHECK("repl see base nil without compiler",
        fr_repl_eval_line(&runtime, "see boot", out, sizeof(out)) == FR_OK &&
            strcmp(out, "base core nil\nok\n") == 0);
  CHECK("repl see numeric base nil without compiler",
        fr_repl_eval_line(&runtime, "see 0", out, sizeof(out)) == FR_OK &&
            strcmp(out, "base core nil\nok\n") == 0);
  CHECK("repl see base literal value without compiler",
        fr_repl_eval_line(&runtime, "see one", out, sizeof(out)) == FR_OK &&
            strcmp(out, "base core 1\nok\n") == 0);
  CHECK("repl see board literal value without compiler",
        fr_repl_eval_line(&runtime, "see $led_builtin", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "base board 13\nok\n") == 0);
  CHECK("repl see gpio.write native arity without compiler",
        fr_repl_eval_line(&runtime, "see gpio.write", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "base target native arity 2\nok\n") == 0);
  CHECK("repl see pin sugar native arity without compiler",
        fr_repl_eval_line(&runtime, "see pin", out, sizeof(out)) == FR_OK &&
            strcmp(out, "base target native arity 2\nok\n") == 0);
#if FR_FEATURE_PERSISTENCE
  CHECK("repl see persistence native owner without compiler",
        fr_repl_eval_line(&runtime, "see save", out, sizeof(out)) == FR_OK &&
            strcmp(out, "base persistence native arity 0\nok\n") == 0);
#endif
#else
  CHECK("repl rejects see without introspection",
        fr_repl_eval_line(&runtime, "see 0", out, sizeof(out)) ==
            FR_ERR_UNSUPPORTED);
#endif
  CHECK("repl rejects definition without compiler",
        fr_repl_eval_line(&runtime, "led is $led_builtin", out,
                          sizeof(out)) == FR_ERR_UNSUPPORTED);
  CHECK("repl rejects expression without compiler",
        fr_repl_eval_line(&runtime, "pin: $led_builtin, 1", out,
                          sizeof(out)) == FR_ERR_UNSUPPORTED);
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  CHECK("repl boot colon nil",
        fr_repl_eval_line(&runtime, "boot:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#else
  CHECK("repl numeric boot colon nil",
        fr_repl_eval_line(&runtime, "0:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#endif
  CHECK("repl clear without compiler",
        fr_repl_eval_line(&runtime, "clear", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#endif
#if FR_FEATURE_OVERLAY_APPLY_COMMAND
  CHECK("repl apply byte budget accounts for prefix",
        FR_REPL_APPLY_BYTES ==
            (FR_REPL_LINE_BYTES - FR_REPL_APPLY_PREFIX_BYTES - 1) / 2);
  CHECK("repl apply line fits budget",
        write_apply_hex_line(apply_limit_bytes, FR_REPL_APPLY_BYTES,
                             apply_limit_line,
                             (uint16_t)sizeof(apply_limit_line)));
  CHECK("repl apply line rejects one over budget",
        !write_apply_hex_line(apply_limit_bytes,
                              (uint16_t)(FR_REPL_APPLY_BYTES + 1),
                              apply_limit_line,
                              (uint16_t)sizeof(apply_limit_line)));
  CHECK("repl builds apply line",
        fr_overlay_update_encode(&apply_update, apply_bytes,
                                 (uint16_t)sizeof(apply_bytes),
                                 &apply_length) == FR_OK &&
            write_apply_hex_line(apply_bytes, apply_length, apply_line,
                                 (uint16_t)sizeof(apply_line)));
  CHECK("repl apply installs overlay update",
        fr_repl_eval_line(&runtime, apply_line, out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  CHECK("repl apply runs installed boot",
        fr_repl_eval_line(&runtime, "boot:", out, sizeof(out)) == FR_OK &&
        strcmp(out, "ok\n") == 0);
#else
  CHECK("repl apply runs installed numeric boot",
        fr_repl_eval_line(&runtime, "0:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#endif
#if FR_FEATURE_INTROSPECTION
  CHECK("repl apply exposes installed code",
        fr_repl_eval_line(&runtime, "see boot", out, sizeof(out)) == FR_OK &&
            strcmp(out, "overlay code\nto boot [ 1 ]\nok\n") == 0);
#endif
  CHECK("repl apply rejects odd hex",
        fr_repl_eval_line(&runtime, "apply 0", out, sizeof(out)) ==
            FR_ERR_INVALID);
  CHECK("repl builds run line",
        write_run_hex_line(run_gpio_write_bytes,
                           (uint16_t)sizeof(run_gpio_write_bytes), run_line,
                           (uint16_t)sizeof(run_line)));
  CHECK("repl run executes one-shot gpio write",
        fr_repl_eval_line(&runtime, run_line, out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl run returns tagged result",
        write_run_hex_line(run_value_bytes, (uint16_t)sizeof(run_value_bytes),
                           run_line, (uint16_t)sizeof(run_line)) &&
            fr_repl_eval_line(&runtime, run_line, out, sizeof(out)) == FR_OK &&
            strcmp(out, "7\nok\n") == 0);
  CHECK("repl run rejects one over shared wire budget",
        write_run_hex_line(run_limit_bytes,
                           (uint16_t)(FR_REPL_APPLY_BYTES + 1),
                           run_limit_line,
                           (uint16_t)sizeof(run_limit_line)) &&
            fr_repl_eval_line(&runtime, run_limit_line, out, sizeof(out)) ==
                FR_ERR_CAPACITY);
  CHECK("repl run rejects odd hex",
        fr_repl_eval_line(&runtime, "run 0", out, sizeof(out)) ==
            FR_ERR_INVALID);
#endif
#if FR_FEATURE_INTROSPECTION
  CHECK("repl see rejects unknown",
        fr_repl_eval_line(&runtime, "see unknown", out, sizeof(out)) ==
            FR_ERR_NOT_FOUND);
#endif
#if FR_FEATURE_COMPILER
  CHECK("repl rejects unknown",
        fr_repl_eval_line(&runtime, "unknown", out, sizeof(out)) ==
            FR_ERR_NOT_FOUND);
#else
  CHECK("repl rejects unknown without compiler",
        fr_repl_eval_line(&runtime, "unknown", out, sizeof(out)) ==
            FR_ERR_UNSUPPORTED);
#endif
#if FR_FEATURE_INTROSPECTION
  CHECK("repl checks output cap",
        fr_repl_eval_line(&runtime, "words", out, 4) == FR_ERR_RANGE);
#else
  CHECK("repl words ignores output cap without introspection",
        fr_repl_eval_line(&runtime, "words", out, 4) == FR_ERR_UNSUPPORTED);
#endif
  CHECK("repl rejects null runtime",
        fr_repl_eval_line(NULL, "words", out, sizeof(out)) == FR_ERR_INVALID);
  CHECK("repl rejects null line",
        fr_repl_eval_line(&runtime, NULL, out, sizeof(out)) == FR_ERR_INVALID);
  CHECK("repl rejects null out",
        fr_repl_eval_line(&runtime, "words", NULL, sizeof(out)) ==
            FR_ERR_INVALID);
}

typedef struct test_repl_io_state_t {
  const char *const *lines;
  uint8_t line_count;
  uint8_t next_line;
  char *out;
  uint16_t out_cap;
  uint16_t out_used;
} test_repl_io_state_t;

static test_repl_io_state_t *test_repl_io_state = NULL;

static fr_err_t test_repl_read_line(char *line, uint16_t cap, bool *out_eof) {
  const char *source = NULL;
  size_t length = 0;

  if (test_repl_io_state == NULL || line == NULL || cap == 0 ||
      out_eof == NULL) {
    return FR_ERR_INVALID;
  }
  if (test_repl_io_state->next_line >= test_repl_io_state->line_count) {
    line[0] = '\0';
    *out_eof = true;
    return FR_OK;
  }

  source = test_repl_io_state->lines[test_repl_io_state->next_line];
  test_repl_io_state->next_line += 1;
  length = strlen(source);
  if (length + 1 > cap) {
    return FR_ERR_RANGE;
  }
  memcpy(line, source, length + 1);
  *out_eof = false;
  return FR_OK;
}

static fr_err_t test_repl_write_text(const char *text) {
  size_t length = 0;

  if (test_repl_io_state == NULL || text == NULL) {
    return FR_ERR_INVALID;
  }

  length = strlen(text);
  if ((uint32_t)test_repl_io_state->out_used + length + 1 >
      test_repl_io_state->out_cap) {
    return FR_ERR_RANGE;
  }
  memcpy(&test_repl_io_state->out[test_repl_io_state->out_used], text, length);
  test_repl_io_state->out_used =
      (uint16_t)(test_repl_io_state->out_used + length);
  test_repl_io_state->out[test_repl_io_state->out_used] = '\0';
  return FR_OK;
}

#if FR_FEATURE_COMPILER && FR_FEATURE_INTROSPECTION &&                         \
    FR_PROFILE_MAX_OVERLAY_NAMES > 0
/* A parameter + binop one-liner is the smallest body that exercises the source
 * renderer's arg-name lookup and infix reduction; pin its exact form. */
static void test_repl_see_source_form(void) {
  fr_runtime_t runtime;
  char out[128];

#if FR_FEATURE_PERSISTENCE
  fr_platform_storage_debug_reset();
#endif
  CHECK("see source one-liner",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(&runtime, "twice is fn with n [ n * 2 ]", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see twice", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay code\nto twice with n [ n * 2 ]\nok\n") == 0);
  CHECK("see source if/else",
        fr_repl_eval_line(&runtime,
                          "abs1 is fn with n [ if n < 0 [ -1 * n ] else [ n ] ]",
                          out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see abs1", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay code\n"
                        "to abs1 with n [ if n < 0 [ -1 * n ] else [ n ] ]\n"
                        "ok\n") == 0);
  /* Chained else if: a three-arm dispatch with a final else, the canonical
   * shape from T9b. Fresh install for the overlay-name budget. */
  CHECK("see source chained else if",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(
                &runtime,
                "step is fn with n [ if n < 0 [ -1 ] else if n > 0 [ 1 ] "
                "else [ 0 ] ]",
                out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see step", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay code\n"
                        "to step with n [ if n < 0 [ -1 ] else if n > 0 [ 1 ] "
                        "else [ 0 ] ]\n"
                        "ok\n") == 0);
  /* A chained arm whose body is a `;`-separated statement list. The renderer
   * has to track bracket depth, since a top-level scan would see the inner
   * `; ` and wrongly bracket the else clause. Fresh install for the budget. */
  CHECK("see source chained else if with statement-list arm",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(
                &runtime,
                "two is fn [ if false [ -1 ] else if true [ 1 ; 2 ] "
                "else [ 0 ] ]",
                out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see two", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay code\n"
                        "to two [ if false [ -1 ] else if true [ 1 ; 2 ] "
                        "else [ 0 ] ]\n"
                        "ok\n") == 0);
  /* Old recursive form still parses, and `see` canonicalizes it to the chained
   * spelling per the T9b render decision. Two-level only to fit the 16-node
   * expression cap. Fresh install for the budget. */
  CHECK("see source recursive else collapses to chained",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(
                &runtime,
                "pick is fn [ if false [ 1 ] else [ if true [ 2 ] "
                "else [ 3 ] ] ]",
                out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see pick", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay code\n"
                        "to pick [ if false [ 1 ] else if true [ 2 ] "
                        "else [ 3 ] ]\n"
                        "ok\n") == 0);
  /* Locals render with canonical localN names regardless of the original source
   * names — local names are not persisted in T9b, mirroring the argN answer for
   * restored params (T10c). Fresh install for the overlay-name budget. */
  CHECK("see source with here local binding",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(&runtime,
                              "g is fn [ here x is 5 ; x + 1 ]", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see g", out, sizeof(out)) == FR_OK &&
            strcmp(out, "overlay code\n"
                        "to g [ here local0 is 5 ; local0 + 1 ]\n"
                        "ok\n") == 0);
  /* Fresh install: tiny's overlay-name budget only holds two words at once. A
   * real wait loop polls external state and so can end; spin on a pin read. */
  CHECK("see source while",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(
                &runtime,
                "wait is fn with p [ while gpio.read: p [ ms: 1 ] ]", out,
                sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see wait", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay code\n"
                        "to wait with p [ while gpio.read: p [ ms: 1 ] ]\n"
                        "ok\n") == 0);
  /* repeat count: a blink-shaped body that drives a pin a fixed number of
   * passes. Fresh install for the same overlay-name budget reason. */
  CHECK("see source repeat",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(
                &runtime,
                "myblink is fn with p [ repeat 3 [ gpio.write: p, 1 ] ]", out,
                sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see myblink", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay code\n"
                        "to myblink with p [ repeat 3 [ gpio.write: p, 1 ] ]\n"
                        "ok\n") == 0);
  /* Nested form: an if/else inside a while. The while body must reduce to one
   * fragment, and the if/else does, so the span walk renders both. Fresh
   * install for the overlay-name budget. */
  CHECK("see source nested while/if",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(
                &runtime,
                "poll is fn with p [ while gpio.read: p [ if p > 0 [ ms: 1 ] "
                "else [ ms: 2 ] ] ]",
                out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see poll", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay code\n"
                        "to poll with p [ while gpio.read: p [ if p > 0 "
                        "[ ms: 1 ] else [ ms: 2 ] ] ]\n"
                        "ok\n") == 0);
  /* Multi-statement body: two `;`-separated statements over the param. Each
   * leaves a value the compiler drops between statements. Fresh install for
   * the overlay-name budget. */
  CHECK("see source multi-statement",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(
                &runtime,
                "flash is fn with p [ gpio.write: p, 1 ; gpio.write: p, 0 ]",
                out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see flash", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay code\n"
                        "to flash with p [ gpio.write: p, 1 ; "
                        "gpio.write: p, 0 ]\n"
                        "ok\n") == 0);
  /* Bare zero-arg call to a source-defined word: the body is one CALL_SLOT
   * before the return. Two overlay names fit tiny's budget. */
  CHECK("see source bare zero-arg call",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(&runtime, "tick is fn [ 1 ]", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "kick is fn [ tick: ]", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see kick", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay code\nto kick [ tick: ]\nok\n") == 0);
  /* Bare one-arg native call: the count sits on the stack, the native arity
   * picks up the lone arg. Fresh install for the overlay-name budget. */
  CHECK("see source bare native one-arg call",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(&runtime, "baz is fn [ ms: 100 ]", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see baz", out, sizeof(out)) == FR_OK &&
            strcmp(out, "overlay code\nto baz [ ms: 100 ]\nok\n") == 0);
  /* Bare two-arg native call over two params. Fresh install for the budget. */
  CHECK("see source bare native two-arg call",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(&runtime,
                              "dim is fn with pin, lvl [ gpio.write: pin, lvl ]",
                              out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see dim", out, sizeof(out)) == FR_OK &&
            strcmp(out,
                   "overlay code\nto dim with pin, lvl [ gpio.write: pin, lvl ]"
                   "\nok\n") == 0);
  /* CALL before a DROP is not call-then-return, so it stays a fallback. Pin
   * the marker, not the disassembly tail (which is opcode-encoding specific). */
  {
    char fallback[256];
    const char *prefix = "overlay code\n;; source reconstruction unavailable\n";

    CHECK("see source non-tail call falls back",
          fr_base_image_install(&runtime) == FR_OK &&
              fr_repl_eval_line(&runtime, "tick is fn [ 1 ]", fallback,
                                sizeof(fallback)) == FR_OK &&
              strcmp(fallback, "ok\n") == 0 &&
              fr_repl_eval_line(&runtime, "tock is fn [ tick: ; 2 ]", fallback,
                                sizeof(fallback)) == FR_OK &&
              strcmp(fallback, "ok\n") == 0 &&
              fr_repl_eval_line(&runtime, "see tock", fallback,
                                sizeof(fallback)) == FR_OK &&
              strncmp(fallback, prefix, strlen(prefix)) == 0);
  }
  CHECK("see source paren overrides precedence",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(&runtime,
                              "lift is fn with a, b, c [ (a + b) * c ]", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see lift", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay code\nto lift with a, b, c "
                        "[ (a + b) * c ]\nok\n") == 0);
  CHECK("see source conservative wrap on nested binop",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(&runtime,
                              "step is fn with a, b, c [ a + b * c ]", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see step", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay code\nto step with a, b, c "
                        "[ a + (b * c) ]\nok\n") == 0);
#if FR_FEATURE_MATH
  CHECK("see source percent infix",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(&runtime, "rem is fn with a, b [ a % b ]", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see rem", out, sizeof(out)) == FR_OK &&
            strcmp(out, "overlay code\nto rem with a, b [ a % b ]\nok\n") == 0);
  CHECK("see source percent feeds add with parens",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(&runtime,
                              "shift is fn with a, b, c [ (a % b) + c ]", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see shift", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "overlay code\nto shift with a, b, c "
                        "[ (a % b) + c ]\nok\n") == 0);
#endif
#if FR_FEATURE_SOURCE_BASE
  /* The spec-named examples use base words, present only under source-base:
   * a zero-arg source call (led.on) and a one-arg source call (gpio.high). */
  CHECK("see source bare zero-arg base call",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(&runtime, "boot is fn [ led.on: ]", out,
                              sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see boot", out, sizeof(out)) == FR_OK &&
            strcmp(out, "overlay code\nto boot [ led.on: ]\nok\n") == 0);
  CHECK("see source bare one-arg base call",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_eval_line(&runtime, "bar is fn with pin [ gpio.high: pin ]",
                              out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_repl_eval_line(&runtime, "see bar", out, sizeof(out)) == FR_OK &&
            strcmp(out, "overlay code\nto bar with pin [ gpio.high: pin ]"
                        "\nok\n") == 0);
#endif
}

#if FR_FEATURE_SOURCE_BASE
/* Every convenience word the boot compile binds from base/core.frothy: it
   resolves by name past the fixed base slots, owns the source layer, and `see`
   renders the exact source form. The `;` separators in blink render spaced
   (` ; `) — that is what the renderer emits, so pin it. */
static void test_source_base_word_proofs(void) {
  static const struct {
    const char *name;
    const char *source;
  } words[] = {
      {"gpio.high", "to gpio.high with pin [ gpio.write: pin, 1 ]"},
      {"gpio.low", "to gpio.low with pin [ gpio.write: pin, 0 ]"},
      {"gpio.toggle",
       "to gpio.toggle with pin [ gpio.write: pin, 1 - gpio.read: pin ]"},
      {"led.on", "to led.on [ gpio.high: $led_builtin ]"},
      {"led.off", "to led.off [ gpio.low: $led_builtin ]"},
      {"led.toggle", "to led.toggle [ gpio.toggle: $led_builtin ]"},
      {"blink", "to blink with pin, count, wait [ repeat count [ gpio.high: "
                "pin ; ms: wait ; gpio.low: pin ; ms: wait ] ]"},
      {"led.blink", "to led.blink with count, wait [ blink: $led_builtin, "
                    "count, wait ]"},
      {"wrap", "to wrap with value, size [ if size <= 0 [ 0 ] else "
               "[ value % size ] ]"},
      {"random.chance?", "to random.chance? with numer, denom [ if denom <= 0 "
                         "[ false ] else [ numer > random.below: denom ] ]"},
      {"random.percent?",
       "to random.percent? with percent [ random.chance?: percent, 100 ]"},
      {"sign", "to sign with n [ clamp: n, -1, 1 ]"},
  };
  fr_runtime_t runtime;
  char out[256];
  char expected[256];

  CHECK("source word count matches the locked library",
        sizeof(words) / sizeof(words[0]) == FR_TEST_SOURCE_BASE_WORD_COUNT);
  CHECK("base image installs for source word proofs",
        fr_base_image_install(&runtime) == FR_OK);
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    fr_slot_id_t slot_id = 0;
    fr_base_layer_t layer = FR_BASE_LAYER_CORE;
    char see_line[64];

    CHECK("source word resolves to a board-local source slot",
          fr_slot_id_for_name(&runtime, words[i].name, &slot_id) == FR_OK &&
              slot_id >= FR_SLOT_BOARD_LOCAL_BASE &&
              fr_base_slot_layer(slot_id, &layer) == FR_OK &&
              layer == FR_BASE_LAYER_SOURCE);
    snprintf(see_line, sizeof(see_line), "see %s", words[i].name);
    snprintf(expected, sizeof(expected), "base source code\n%s\nok\n",
             words[i].source);
    CHECK("source word sees its exact source form",
          fr_repl_eval_line(&runtime, see_line, out, sizeof(out)) == FR_OK &&
              strcmp(out, expected) == 0);
  }
}
#endif
#endif

static void test_err_name(void) {
  static const struct {
    fr_err_t err;
    const char *name;
  } cases[] = {
      {FR_OK, NULL},
      {FR_ERR_RANGE, "out of range"},
      {FR_ERR_TYPE, "wrong type"},
      {FR_ERR_DOMAIN, "bad value"},
      {FR_ERR_CAPACITY, "capacity exceeded"},
      {FR_ERR_OVERFLOW, "overflow"},
      {FR_ERR_UNDERFLOW, "underflow"},
      {FR_ERR_NOT_FOUND, "not found"},
      {FR_ERR_INVALID, "bad source"},
      {FR_ERR_UNSUPPORTED, "unsupported"},
      {FR_ERR_INTERRUPTED, "interrupted"},
      {FR_ERR_CORRUPT, "corrupt data"},
      {FR_ERR_IO, "i/o failed"},
      {FR_ERR_VOLATILE, "not saved"},
      {FR_ERR_HANDLE, "bad handle"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const char *got = fr_err_name(cases[i].err);
    if (cases[i].name == NULL) {
      CHECK("fr_err_name returns NULL for FR_OK", got == NULL);
    } else {
      CHECK("fr_err_name returns the table phrase",
            got != NULL && strcmp(got, cases[i].name) == 0);
    }
  }
  CHECK("fr_err_name returns NULL for out-of-range",
        fr_err_name((fr_err_t)9999) == NULL);
}

static void test_repl_pump(void) {
  fr_runtime_t runtime;
  char out[1024] = {0};
#if FR_FEATURE_COMPILER
  const char *lines[] = {
      "words",
      "boot is fn [ pin: $led_builtin, 1; pin: $led_builtin, 0; one ]",
      "see boot",
      "boot:",
      "unknown",
  };
#elif FR_FEATURE_INTROSPECTION
  const char *lines[] = {
      "words",
      "see boot",
      "boot:",
      "unknown",
  };
#else
  const char *lines[] = {
      "words",
      "0:",
      "unknown",
  };
#endif
  test_repl_io_state_t state = {
      .lines = lines,
      .line_count = (uint8_t)(sizeof(lines) / sizeof(lines[0])),
      .out = out,
      .out_cap = (uint16_t)sizeof(out),
  };
  const fr_repl_io_t io = {
      .read_line = test_repl_read_line,
      .write_text = test_repl_write_text,
  };

  test_repl_io_state = &state;
  CHECK("repl pump runs transcript",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_repl_run(&runtime, &io) == FR_OK);
#if FR_FEATURE_COMPILER
  CHECK("repl pump transcript output",
        strcmp(out, "> " FR_TEST_WORDS "> ok\n> overlay code\n"
                    "to boot [ gpio.write: $led_builtin, 1 ; "
                    "gpio.write: $led_builtin, 0 ; one ]\n"
                    "ok\n> ok\n> error: not found (7)\n> ") == 0);
#elif FR_FEATURE_INTROSPECTION
  CHECK("repl pump transcript output without compiler",
        strcmp(out, "> " FR_TEST_WORDS
                    "> base core nil\nok\n> ok\n> error: unsupported (9)\n> ") ==
            0);
#else
  CHECK("repl pump transcript output without introspection",
        strcmp(out,
               "> error: unsupported (9)\n> ok\n> error: unsupported (9)\n> ") ==
            0);
#endif
  test_repl_io_state = NULL;
}

static void test_repl_transcript(void) {
  fr_runtime_t runtime;
  char out[1024];

#if FR_FEATURE_PERSISTENCE
  fr_platform_storage_debug_reset();
#endif

  CHECK("repl transcript installs base image",
        fr_base_image_install(&runtime) == FR_OK);
#if FR_FEATURE_INTROSPECTION
  CHECK("repl transcript words",
        fr_repl_eval_line(&runtime, "words", out, sizeof(out)) == FR_OK &&
            strcmp(out, FR_TEST_WORDS) == 0);
#else
  CHECK("repl transcript rejects words without introspection",
        fr_repl_eval_line(&runtime, "words", out, sizeof(out)) ==
            FR_ERR_UNSUPPORTED);
#endif
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  CHECK("repl transcript boot colon nil",
        fr_repl_eval_line(&runtime, "boot:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#else
  CHECK("repl transcript numeric boot colon nil",
        fr_repl_eval_line(&runtime, "0:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#endif
#if FR_FEATURE_COMPILER
  CHECK("repl transcript define mechanical boot",
        fr_repl_eval_line(
            &runtime,
            "boot is fn [ pin: $led_builtin, 1; ms: 100; "
            "pin: $led_builtin, 0; ms: 100 ]",
            out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl transcript direct pin on",
        fr_repl_eval_line(&runtime, "pin: $led_builtin, 1", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl transcript direct pin off",
        fr_repl_eval_line(&runtime, "pin: $led_builtin, 0", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("repl transcript run mechanical boot",
        fr_repl_eval_line(&runtime, "boot:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#else
  CHECK("repl transcript rejects definition without compiler",
        fr_repl_eval_line(
            &runtime,
            "boot is fn [ pin: $led_builtin, 1; ms: 100; "
            "pin: $led_builtin, 0; ms: 100 ]",
            out, sizeof(out)) == FR_ERR_UNSUPPORTED);
  CHECK("repl transcript rejects direct pin without compiler",
        fr_repl_eval_line(&runtime, "pin: $led_builtin, 1", out,
                          sizeof(out)) == FR_ERR_UNSUPPORTED);
#endif
#if FR_FEATURE_PERSISTENCE
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  CHECK("repl transcript save",
        fr_repl_eval_line(&runtime, "save", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#else
  CHECK("repl transcript numeric save",
        fr_repl_eval_line(&runtime, "5:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#endif
  CHECK("repl transcript reinstall base",
        fr_base_image_install(&runtime) == FR_OK);
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  CHECK("repl transcript restore",
        fr_repl_eval_line(&runtime, "restore", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#else
  CHECK("repl transcript numeric restore",
        fr_repl_eval_line(&runtime, "6:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#endif
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  CHECK("repl transcript restored boot",
        fr_repl_eval_line(&runtime, "boot:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#else
  CHECK("repl transcript restored numeric boot",
        fr_repl_eval_line(&runtime, "0:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
#endif
#else
#if FR_BASE_IMAGE_INCLUDE_SYMBOLS
  CHECK("repl transcript rejects save without persistence",
        fr_repl_eval_line(&runtime, "save", out, sizeof(out)) ==
        FR_ERR_NOT_FOUND);
#else
  CHECK("repl transcript rejects save without persistence",
        fr_repl_eval_line(&runtime, "save", out, sizeof(out)) ==
            FR_ERR_UNSUPPORTED);
#endif
#endif
}

#if FR_FEATURE_COMPILER && FR_BASE_IMAGE_INCLUDE_SYMBOLS
static void test_repl_zero_arg_call_result(void) {
  fr_runtime_t runtime;
  char out[64];

  CHECK("zero-arg result installs base image",
        fr_base_image_install(&runtime) == FR_OK);
  CHECK("zero-arg result compiles function",
        fr_repl_eval_line(&runtime, "value is fn [ one ]", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0);
  CHECK("zero-arg result prints non-nil return",
        fr_repl_eval_line(&runtime, "value:", out, sizeof(out)) == FR_OK &&
            strcmp(out, "1\nok\n") == 0);
}
#endif

#if FR_FEATURE_COMPILER && FR_FEATURE_PERSISTENCE && FR_BASE_IMAGE_INCLUDE_SYMBOLS
static void test_dangerous_wipe_bare_word(void) {
  fr_runtime_t runtime;
  fr_tagged_t tagged = 0;
  fr_slot_id_t slot_id = 0;
  char out[64];

  fr_platform_storage_debug_reset();
  CHECK("dangerous.wipe installs base image",
        fr_base_image_install(&runtime) == FR_OK);
  CHECK("dangerous.wipe saves a boot overlay first",
        fr_repl_eval_line(&runtime, "boot is fn [ one ]", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "ok\n") == 0 && fr_persist_save(&runtime) == FR_OK);
  CHECK("dangerous.wipe bare word runs and clears persisted state",
        fr_repl_eval_line(&runtime, "dangerous.wipe", out, sizeof(out)) ==
                FR_OK &&
            strcmp(out, "ok\n") == 0 &&
            fr_slot_read(&runtime, FR_SLOT_BOOT, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged) &&
            fr_persist_restore(&runtime) == FR_ERR_NOT_FOUND);
  CHECK("dangerous.wipe rename retires the old wipe name",
        fr_slot_id_for_name(&runtime, "wipe", &slot_id) == FR_ERR_NOT_FOUND);
}
#endif

#if FR_FEATURE_COMPILER && FR_FEATURE_PERSISTENCE && FR_BASE_IMAGE_INCLUDE_SYMBOLS
static void test_repl_startup_restore_and_boot(void) {
  fr_runtime_t runtime;
  char out[128];
  uint16_t gpio_value = 0;
  fr_tagged_t tagged = 0;
  fr_code_object_id_t code_id = 0;

  fr_platform_storage_debug_reset();
  CHECK("startup boot installs base image",
        fr_base_image_install(&runtime) == FR_OK);
  CHECK("startup boot tolerates first boot without saved image",
        fr_repl_startup_restore_and_boot(&runtime) == FR_OK &&
            fr_slot_read(&runtime, FR_SLOT_BOOT, &tagged) == FR_OK &&
            fr_tagged_is_nil(tagged));
  CHECK("startup boot prepares low gpio",
        fr_platform_gpio_write(13, 0) == FR_OK &&
            fr_platform_gpio_read(13, &gpio_value) == FR_OK &&
            gpio_value == 0);
  CHECK("startup boot saves boot overlay",
        fr_repl_eval_line(&runtime, "boot is fn [ pin: 13, 1 ]", out,
                          sizeof(out)) == FR_OK &&
            strcmp(out, "ok\n") == 0 && fr_persist_save(&runtime) == FR_OK);
  CHECK("startup boot resets runtime and gpio",
        fr_base_image_install(&runtime) == FR_OK &&
            fr_platform_gpio_write(13, 0) == FR_OK &&
            fr_platform_gpio_read(13, &gpio_value) == FR_OK &&
            gpio_value == 0);
  CHECK("startup boot restores and runs saved boot",
        fr_repl_startup_restore_and_boot(&runtime) == FR_OK &&
            fr_platform_gpio_read(13, &gpio_value) == FR_OK &&
            gpio_value == 1 &&
            fr_slot_read(&runtime, FR_SLOT_BOOT, &tagged) == FR_OK &&
            fr_tagged_decode_code_object_id(tagged, &code_id) == FR_OK);
}
#endif

int main(void) {
  test_tagged_bands();
  test_specials();
  test_ints();
  test_refs();
  test_addr();
  test_base_def_contract();
  test_profile_hash_word_size();
  test_instruction_stream();
  test_slots();
#if FR_FEATURE_HANDLES
  test_handles();
#endif
#if FR_FEATURE_UART
  test_uart();
#endif
#if FR_FEATURE_PWM
  test_pwm();
#endif
#if FR_FEATURE_I2C
  test_i2c();
#endif
#if FR_FEATURE_RANDOM
  test_random();
#endif
#if FR_FEATURE_MATH
  test_math();
#endif
#if FR_FEATURE_PAD
  test_pad();
#endif
#if FR_FEATURE_CELLS
  test_cells();
#endif
#if FR_FEATURE_TEXT
  test_text_objects();
  test_text_natives();
#endif
#if FR_FEATURE_RECORDS
  test_records();
#endif
  test_natives();
  test_event_table();
  test_event_register_cancel();
  test_event_drain_dispatch();
  test_event_register_native();
#if FR_FEATURE_COMPILER
  test_event_compile_on_form();
#endif
#if FR_INCLUDE_TEST_NATIVES && FR_FEATURE_TEXT
  test_event_fire_event_native();
#endif
#if FR_FEATURE_COMPILER && FR_INCLUDE_TEST_NATIVES && FR_FEATURE_TEXT &&       \
    FR_PROFILE_MAX_OVERLAY_NAMES > 0
  test_event_compiled_body_fires();
#endif
  test_code();
  test_image();
  test_public_surface();
#if FR_FEATURE_COMPILER
  test_parse();
  test_compile();
#if FR_FEATURE_REPL && FR_BASE_IMAGE_INCLUDE_SYMBOLS &&                       \
    FR_PROFILE_MAX_OVERLAY_NAMES > 0 && !FR_HOST_TINY_NAMES_MODE
  test_compiler_overlay_wire_parity();
#endif
#endif
#if FR_FEATURE_PERSISTENCE
  test_persist();
  test_persist_code_id_round_trip();
  test_persist_cross_width_header_rejection();
#if FR_FEATURE_SOURCE_BASE
  test_persist_source_change_header_rejection();
#endif
#endif
  test_vm();
  test_repl();
#if FR_FEATURE_COMPILER && FR_FEATURE_INTROSPECTION &&                         \
    FR_PROFILE_MAX_OVERLAY_NAMES > 0
  test_repl_see_source_form();
#if FR_FEATURE_SOURCE_BASE
  test_source_base_word_proofs();
#endif
#endif
  test_err_name();
  test_repl_pump();
  test_repl_transcript();
#if FR_FEATURE_COMPILER && FR_BASE_IMAGE_INCLUDE_SYMBOLS
  test_repl_zero_arg_call_result();
#endif
#if FR_FEATURE_COMPILER && FR_FEATURE_PERSISTENCE && FR_BASE_IMAGE_INCLUDE_SYMBOLS
  test_dangerous_wipe_bare_word();
  test_repl_startup_restore_and_boot();
#endif

  if (failures != 0) {
    printf("%d test failure(s)\n", failures);
    return 1;
  }

  printf("ok\n");
  return 0;
}
