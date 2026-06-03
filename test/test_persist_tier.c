/*
 * Unity narrow proof for the T12L-7 tier system:
 *
 * D7 (persist record format): BIND records carry a tier byte at the current
 * payload version, reject out-of-range tier bytes, and decode under the
 * legacy payload version with the tier defaulting to user. The encoder
 * writes the byte at fr_persist_payload_encode; the decoder at
 * fr_persist_decode_payload reads it under the current version and falls
 * back to user tier under legacy.
 *
 * D3 (REPL install tier): install-library and install-user are outside-
 * parser tokens that flip runtime->install_tier and reply with ok\n;
 * fr_repl_run resets the field to USER on entry per the session-scope rule.
 */

#include "base_image.h"
#include "base_defs.h"
#include "config.h"
#include "persist_payload.h"
#include "platform.h"
#include "repl.h"
#include "runtime.h"
#include "slot.h"
#include "tagged.h"

#include "unity/unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Mirror persist_payload.c's enum so a public test can shape the bytes. */
#if FR_WORD_SIZE == 16
#define FR_TEST_TIER_PAYLOAD_VERSION 3
#define FR_TEST_TIER_PAYLOAD_VERSION_LEGACY 2
#else
#define FR_TEST_TIER_PAYLOAD_VERSION 4
#define FR_TEST_TIER_PAYLOAD_VERSION_LEGACY 3
#endif

#define FR_TEST_TIER_RECORD_BIND 2
#define FR_TEST_TIER_RECORD_END 0xff
#define FR_TEST_TIER_VALUE_INT 3
#define FR_TEST_TIER_LIBRARY 1
#define FR_TEST_TIER_USER 2

#define FR_TEST_TIER_SLOT                                                     \
  ((fr_slot_id_t)FR_SLOT_BOARD_LOCAL_BASE)

static void write_u16_le(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void write_int_le(uint8_t *p, fr_int_t v) {
#if FR_WORD_SIZE == 16
  write_u16_le(p, (uint16_t)(int16_t)v);
#else
  p[0] = (uint8_t)((uint32_t)(int32_t)v & 0xff);
  p[1] = (uint8_t)(((uint32_t)(int32_t)v >> 8) & 0xff);
  p[2] = (uint8_t)(((uint32_t)(int32_t)v >> 16) & 0xff);
  p[3] = (uint8_t)(((uint32_t)(int32_t)v >> 24) & 0xff);
#endif
}

#define FR_TEST_TIER_INT_BYTES (FR_WORD_SIZE / 8)
/* magic 4 + version 1 + BIND tag 1 + slot 2 + tier 1 + value_kind 1 + int N + END 1. */
#define FR_TEST_TIER_PAYLOAD_LEN ((uint16_t)(11 + FR_TEST_TIER_INT_BYTES))
#define FR_TEST_TIER_LEGACY_LEN ((uint16_t)(FR_TEST_TIER_PAYLOAD_LEN - 1))

static void build_current_bind(uint8_t *out, uint8_t tier, fr_int_t value) {
  uint8_t *p = out;
  *p++ = 'F';
  *p++ = 'R';
  *p++ = 'P';
  *p++ = 'O';
  *p++ = FR_TEST_TIER_PAYLOAD_VERSION;
  *p++ = FR_TEST_TIER_RECORD_BIND;
  write_u16_le(p, FR_TEST_TIER_SLOT);
  p += 2;
  *p++ = tier;
  *p++ = FR_TEST_TIER_VALUE_INT;
  write_int_le(p, value);
  p += FR_TEST_TIER_INT_BYTES;
  *p++ = FR_TEST_TIER_RECORD_END;
}

static void build_legacy_bind(uint8_t *out, fr_int_t value) {
  uint8_t *p = out;
  *p++ = 'F';
  *p++ = 'R';
  *p++ = 'P';
  *p++ = 'O';
  *p++ = FR_TEST_TIER_PAYLOAD_VERSION_LEGACY;
  *p++ = FR_TEST_TIER_RECORD_BIND;
  write_u16_le(p, FR_TEST_TIER_SLOT);
  p += 2;
  *p++ = FR_TEST_TIER_VALUE_INT;
  write_int_le(p, value);
  p += FR_TEST_TIER_INT_BYTES;
  *p++ = FR_TEST_TIER_RECORD_END;
}

static fr_runtime_t s_runtime;

void setUp(void) { fr_platform_storage_debug_reset(); }
void tearDown(void) {}

static void test_current_payload_accepts_user_tier(void) {
  uint8_t payload[FR_TEST_TIER_PAYLOAD_LEN];
  fr_tagged_t tagged = 0;
  fr_int_t decoded = 0;

  build_current_bind(payload, FR_TEST_TIER_USER, 11);
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_restore(&s_runtime, payload,
                                                      FR_TEST_TIER_PAYLOAD_LEN));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_read(&s_runtime, FR_TEST_TIER_SLOT, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(tagged, &decoded));
  TEST_ASSERT_EQUAL(11, decoded);
}

static void test_current_payload_accepts_library_tier(void) {
  uint8_t payload[FR_TEST_TIER_PAYLOAD_LEN];
  fr_tagged_t tagged = 0;
  fr_int_t decoded = 0;

  build_current_bind(payload, FR_TEST_TIER_LIBRARY, 12);
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_restore(&s_runtime, payload,
                                                      FR_TEST_TIER_PAYLOAD_LEN));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_read(&s_runtime, FR_TEST_TIER_SLOT, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(tagged, &decoded));
  TEST_ASSERT_EQUAL(12, decoded);
}

static void test_current_payload_rejects_tier_zero(void) {
  uint8_t payload[FR_TEST_TIER_PAYLOAD_LEN];

  build_current_bind(payload, 0, 0);
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_ERR_CORRUPT,
                    fr_persist_payload_restore(&s_runtime, payload,
                                               FR_TEST_TIER_PAYLOAD_LEN));
}

static void test_current_payload_rejects_out_of_range_tier(void) {
  uint8_t payload[FR_TEST_TIER_PAYLOAD_LEN];

  build_current_bind(payload, 3, 0);
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_ERR_CORRUPT,
                    fr_persist_payload_restore(&s_runtime, payload,
                                               FR_TEST_TIER_PAYLOAD_LEN));
}

/* Drop the trailing END after the truncated tier slot so the decoder runs
 * out of bytes mid-bind rather than walking the value bytes as a new record. */
static void test_current_payload_rejects_missing_tier(void) {
  uint8_t payload[8];
  uint8_t *p = payload;
  *p++ = 'F';
  *p++ = 'R';
  *p++ = 'P';
  *p++ = 'O';
  *p++ = FR_TEST_TIER_PAYLOAD_VERSION;
  *p++ = FR_TEST_TIER_RECORD_BIND;
  write_u16_le(p, FR_TEST_TIER_SLOT);

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_ERR_CORRUPT,
                    fr_persist_payload_restore(&s_runtime, payload,
                                               (uint16_t)sizeof(payload)));
}

static void test_legacy_payload_decodes_without_tier_byte(void) {
  uint8_t payload[FR_TEST_TIER_LEGACY_LEN];
  fr_tagged_t tagged = 0;
  fr_int_t decoded = 0;

  build_legacy_bind(payload, 13);
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_restore(&s_runtime, payload,
                                                      FR_TEST_TIER_LEGACY_LEN));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_read(&s_runtime, FR_TEST_TIER_SLOT, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(tagged, &decoded));
  TEST_ASSERT_EQUAL(13, decoded);
}

/* The encoder must emit the tier byte for every overlay slot. With one
 * int-bound slot and no objects, the encoded payload is byte-identical to a
 * single current-version BIND with tier USER: dropping the writer at
 * src/persist_payload.c:1172 shortens encoded_len by one and shifts value_kind
 * into the tier position, so both assertions fail. */
static void test_encoder_emits_tier_byte_for_overlay_bind(void) {
  uint8_t source[FR_TEST_TIER_PAYLOAD_LEN];
  uint8_t encoded[FR_PROFILE_PERSISTENCE_BYTES];
  uint16_t encoded_len = 0;

  build_current_bind(source, FR_TEST_TIER_USER, 17);
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_restore(&s_runtime, source,
                                                      FR_TEST_TIER_PAYLOAD_LEN));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_encode(&s_runtime, encoded,
                                                     (uint16_t)sizeof(encoded),
                                                     &encoded_len));
  TEST_ASSERT_EQUAL(FR_TEST_TIER_PAYLOAD_LEN, encoded_len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(source, encoded, FR_TEST_TIER_PAYLOAD_LEN);
}

static void test_repl_install_library_flips_install_tier(void) {
  char out[FR_REPL_OUTPUT_BYTES];

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_INSTALL_TIER_USER, s_runtime.install_tier);
  TEST_ASSERT_EQUAL(FR_OK, fr_repl_eval_line(&s_runtime, "install-library", out,
                                             (uint16_t)sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", out);
  TEST_ASSERT_EQUAL(FR_INSTALL_TIER_LIBRARY, s_runtime.install_tier);
}

static void test_repl_install_user_flips_install_tier(void) {
  char out[FR_REPL_OUTPUT_BYTES];

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  s_runtime.install_tier = FR_INSTALL_TIER_LIBRARY;
  TEST_ASSERT_EQUAL(FR_OK, fr_repl_eval_line(&s_runtime, "install-user", out,
                                             (uint16_t)sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", out);
  TEST_ASSERT_EQUAL(FR_INSTALL_TIER_USER, s_runtime.install_tier);
}

/* Trailing whitespace must not change recognition; argument-bearing forms
 * must fall through to the compiler path (no install-library <word> shape). */
static void test_repl_install_library_with_argument_does_not_flip(void) {
  char out[FR_REPL_OUTPUT_BYTES];

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_INSTALL_TIER_USER, s_runtime.install_tier);
  (void)fr_repl_eval_line(&s_runtime, "install-library foo", out,
                          (uint16_t)sizeof(out));
  TEST_ASSERT_EQUAL(FR_INSTALL_TIER_USER, s_runtime.install_tier);
}

static fr_err_t mock_read_line_eof(char *line, uint16_t cap, bool *out_eof) {
  (void)line;
  (void)cap;
  if (out_eof != NULL) {
    *out_eof = true;
  }
  return FR_OK;
}

static fr_err_t mock_write_text_noop(const char *text) {
  (void)text;
  return FR_OK;
}

static void test_repl_run_resets_install_tier_on_entry(void) {
  const fr_repl_io_t io = {
      .read_line = mock_read_line_eof,
      .write_text = mock_write_text_noop,
  };

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  s_runtime.install_tier = FR_INSTALL_TIER_LIBRARY;
  TEST_ASSERT_EQUAL(FR_OK, fr_repl_run(&s_runtime, &io));
  TEST_ASSERT_EQUAL(FR_INSTALL_TIER_USER, s_runtime.install_tier);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_current_payload_accepts_user_tier);
  RUN_TEST(test_current_payload_accepts_library_tier);
  RUN_TEST(test_current_payload_rejects_tier_zero);
  RUN_TEST(test_current_payload_rejects_out_of_range_tier);
  RUN_TEST(test_current_payload_rejects_missing_tier);
  RUN_TEST(test_legacy_payload_decodes_without_tier_byte);
  RUN_TEST(test_encoder_emits_tier_byte_for_overlay_bind);
  RUN_TEST(test_repl_install_library_flips_install_tier);
  RUN_TEST(test_repl_install_user_flips_install_tier);
  RUN_TEST(test_repl_install_library_with_argument_does_not_flip);
  RUN_TEST(test_repl_run_resets_install_tier_on_entry);
  return UNITY_END();
}
