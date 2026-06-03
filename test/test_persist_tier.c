/*
 * Unity narrow proof for the T12L-7 tier system:
 *
 * D7 (persist record format): BIND records carry a tier byte at the
 * current payload version. The encoder always writes the byte; the
 * decoder always reads it and rejects out-of-range or missing values.
 * Pre-T12L-7 (legacy) payloads are rejected outright per the
 * project-wide no-backwards-compat stance.
 *
 * D3 (REPL install tier): install-library and install-user are
 * outside-parser tokens that reply with ok\n and set
 * runtime->install_tier (the per-runtime session tier). The session
 * tier is not a public runtime query (SPEC non-goal); the persisted
 * record tag is the downstream proof.
 */

#include "base_image.h"
#include "base_defs.h"
#include "config.h"
#include "persist.h"
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

#define FR_TEST_TIER_RECORD_CODE 1
#define FR_TEST_TIER_RECORD_BIND 2
#define FR_TEST_TIER_RECORD_NAME 3
#define FR_TEST_TIER_RECORD_END 0xff
#define FR_TEST_TIER_VALUE_NIL 0
#define FR_TEST_TIER_VALUE_FALSE 1
#define FR_TEST_TIER_VALUE_TRUE 2
#define FR_TEST_TIER_VALUE_INT 3
#define FR_TEST_TIER_VALUE_CODE 4
#define FR_TEST_TIER_VALUE_NATIVE 5
#define FR_TEST_TIER_VALUE_OBJECT 6
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

/* magic 4 + version 1 + BIND 1 + slot 2 = 8; the tier byte sits here. */
#define FR_TEST_TIER_BYTE_OFFSET 8

static fr_runtime_t s_runtime;

void setUp(void) {
  fr_platform_storage_debug_reset();
  /* T12L-7 D3: install tier is per-runtime. setUp resets to user so
     encode-side tests read a known starting tier; install-* tests flip
     it via fr_repl_eval_line. */
  s_runtime.install_tier = FR_INSTALL_TIER_USER;
}
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

/* T12L-7 D7: project policy is no backwards compatibility. A pre-T12L-7
   payload (version byte FR_TEST_TIER_PAYLOAD_VERSION_LEGACY) is rejected
   by the version check at fr_persist_payload_restore — there is no
   "untagged defaults to user" fallback. */
static void test_legacy_payload_rejected(void) {
  uint8_t payload[FR_TEST_TIER_LEGACY_LEN];

  build_legacy_bind(payload, 13);
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_ERR_CORRUPT,
                    fr_persist_payload_restore(&s_runtime, payload,
                                               FR_TEST_TIER_LEGACY_LEN));
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

/* install-library only marks slots installed under it; a pre-existing user
 * slot keeps its tier across the flip. Without the per-slot table this
 * test fails because the encoder reads back the new global tier. */
static void test_install_library_does_not_relabel_existing_user_slot(void) {
  uint8_t source[FR_TEST_TIER_PAYLOAD_LEN];
  uint8_t encoded[FR_PROFILE_PERSISTENCE_BYTES];
  uint16_t encoded_len = 0;
  char repl_out[FR_REPL_OUTPUT_BYTES];

  build_current_bind(source, FR_TEST_TIER_USER, 21);
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_restore(&s_runtime, source,
                                                      FR_TEST_TIER_PAYLOAD_LEN));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-library", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_encode(&s_runtime, encoded,
                                                     (uint16_t)sizeof(encoded),
                                                     &encoded_len));
  TEST_ASSERT_EQUAL(FR_TEST_TIER_PAYLOAD_LEN, encoded_len);
  TEST_ASSERT_EQUAL_UINT8(FR_TEST_TIER_USER, encoded[FR_TEST_TIER_BYTE_OFFSET]);
}

/* `lib_word is 5` is a literal definition; compile.c emits one slot_init and
 * no code objects, so the encoder writes magic + version + one BIND for the
 * new slot. The BIND tag sits at offset 5 and the tier byte at offset 8. The
 * REPL stamp at fr_overlay_apply must have written FR_TEST_TIER_LIBRARY into
 * the per-slot table for the new slot. */
static void test_install_library_stamps_new_overlay_slot(void) {
  uint8_t encoded[FR_PROFILE_PERSISTENCE_BYTES];
  uint16_t encoded_len = 0;
  char repl_out[FR_REPL_OUTPUT_BYTES];

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-library", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "lib_word is 5", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_encode(&s_runtime, encoded,
                                                     (uint16_t)sizeof(encoded),
                                                     &encoded_len));
  TEST_ASSERT_GREATER_THAN(FR_TEST_TIER_BYTE_OFFSET, encoded_len);
  TEST_ASSERT_EQUAL_UINT8(FR_TEST_TIER_RECORD_BIND, encoded[5]);
  TEST_ASSERT_EQUAL_UINT8(FR_TEST_TIER_LIBRARY, encoded[FR_TEST_TIER_BYTE_OFFSET]);
}

/* Two definitions across an install-library / install-user split. Slot ids
 * allocate in source order, so the encoder's slot loop writes lib_word's BIND
 * first at offset 5; usr_word's BIND follows immediately. value_kind = INT
 * gives the int-bytes value width. The per-slot table must keep lib_word's
 * LIBRARY tier intact after install-user flips the current tier back. */
static void test_install_user_after_library_keeps_library_slot(void) {
  uint8_t encoded[FR_PROFILE_PERSISTENCE_BYTES];
  uint16_t encoded_len = 0;
  char repl_out[FR_REPL_OUTPUT_BYTES];
  uint16_t lib_bind = 5;
  uint16_t usr_bind = 0;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-library", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "lib_word is 5", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-user", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "usr_word is 7", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_encode(&s_runtime, encoded,
                                                     (uint16_t)sizeof(encoded),
                                                     &encoded_len));

  TEST_ASSERT_EQUAL_UINT8(FR_TEST_TIER_RECORD_BIND, encoded[lib_bind]);
  TEST_ASSERT_EQUAL_UINT8(FR_TEST_TIER_LIBRARY, encoded[lib_bind + 3]);
  TEST_ASSERT_EQUAL_UINT8(FR_TEST_TIER_VALUE_INT, encoded[lib_bind + 4]);
  usr_bind = (uint16_t)(lib_bind + 5 + FR_TEST_TIER_INT_BYTES);
  TEST_ASSERT_GREATER_THAN(usr_bind + 4, encoded_len);
  TEST_ASSERT_EQUAL_UINT8(FR_TEST_TIER_RECORD_BIND, encoded[usr_bind]);
  TEST_ASSERT_EQUAL_UINT8(FR_TEST_TIER_USER, encoded[usr_bind + 3]);
}

/* Walk the encoded payload past CODE records and through BIND records to find
 * the tier byte for a specific slot. Returns -1 if the slot has no BIND or the
 * payload is malformed for any of CODE/BIND/NAME. NAME records mark the end of
 * the BIND section in fr_persist_payload_encode. */
static int find_bind_tier_for_slot(const uint8_t *encoded, uint16_t len,
                                   fr_slot_id_t target_slot) {
  uint16_t i = 5;
  while (i < len) {
    uint8_t tag = encoded[i];

    if (tag == FR_TEST_TIER_RECORD_END) {
      return -1;
    }
    if (tag == FR_TEST_TIER_RECORD_CODE) {
      uint16_t length = 0;

      if ((uint16_t)(i + 5) > len) {
        return -1;
      }
      length = (uint16_t)encoded[i + 3] | ((uint16_t)encoded[i + 4] << 8);
      i = (uint16_t)(i + 5 + length);
      continue;
    }
    if (tag == FR_TEST_TIER_RECORD_BIND) {
      uint16_t slot = 0;
      uint8_t tier = 0;
      uint8_t value_kind = 0;

      if ((uint16_t)(i + 5) > len) {
        return -1;
      }
      slot = (uint16_t)encoded[i + 1] | ((uint16_t)encoded[i + 2] << 8);
      tier = encoded[i + 3];
      value_kind = encoded[i + 4];
      if (slot == target_slot) {
        return (int)tier;
      }
      i = (uint16_t)(i + 5);
      switch (value_kind) {
      case FR_TEST_TIER_VALUE_NIL:
      case FR_TEST_TIER_VALUE_FALSE:
      case FR_TEST_TIER_VALUE_TRUE:
        break;
      case FR_TEST_TIER_VALUE_INT:
        i = (uint16_t)(i + FR_TEST_TIER_INT_BYTES);
        break;
      case FR_TEST_TIER_VALUE_CODE:
      case FR_TEST_TIER_VALUE_NATIVE:
      case FR_TEST_TIER_VALUE_OBJECT:
        i = (uint16_t)(i + 2);
        break;
      default:
        return -1;
      }
      continue;
    }
    return -1;
  }
  return -1;
}

/* `lib_word is helper:` routes through fr_compile_value_binding_for_runtime +
 * fr_repl_eval_value_binding (the RHS is a CALL), not the overlay-apply path.
 * Without the stamp_slot call in that branch, the encoder would default the
 * lib_word tier byte to USER; the LIBRARY assertion here fails. helper itself
 * comes from the overlay-apply path, which already stamps. */
static void test_install_library_stamps_value_binding_slot(void) {
  uint8_t encoded[FR_PROFILE_PERSISTENCE_BYTES];
  uint16_t encoded_len = 0;
  char repl_out[FR_REPL_OUTPUT_BYTES];
  fr_slot_id_t helper_slot = 0;
  fr_slot_id_t lib_word_slot = 0;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-library", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "helper is fn [ 99 ]",
                                      repl_out, (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "lib_word is helper:",
                                      repl_out, (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "helper", &helper_slot));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "lib_word", &lib_word_slot));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_encode(&s_runtime, encoded,
                                                     (uint16_t)sizeof(encoded),
                                                     &encoded_len));
  TEST_ASSERT_EQUAL_INT(FR_TEST_TIER_LIBRARY,
                        find_bind_tier_for_slot(encoded, encoded_len,
                                                helper_slot));
  TEST_ASSERT_EQUAL_INT(FR_TEST_TIER_LIBRARY,
                        find_bind_tier_for_slot(encoded, encoded_len,
                                                lib_word_slot));
}

/* Acceptance #11: wipe-user clears L2 records and keeps L1. The library word
 * `lib_word` is installed under install-library; the user word `usr_word` is
 * installed under install-user. After `wipe-user`, fr_slot_id_for_name still
 * resolves lib_word and no longer resolves usr_word; the re-encoded payload
 * still carries lib_word's LIBRARY BIND and no longer carries usr_word's. */
static void test_wipe_user_preserves_library_word(void) {
  char repl_out[FR_REPL_OUTPUT_BYTES];
  fr_slot_id_t lib_slot = 0;
  fr_slot_id_t usr_slot = 0;
  uint8_t encoded[FR_PROFILE_PERSISTENCE_BYTES];
  uint16_t encoded_len = 0;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-library", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "lib_word is 5", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-user", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "usr_word is 7", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "lib_word", &lib_slot));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "usr_word", &usr_slot));

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "wipe-user", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "lib_word", &lib_slot));
  TEST_ASSERT_EQUAL(FR_ERR_NOT_FOUND,
                    fr_slot_id_for_name(&s_runtime, "usr_word", &usr_slot));

  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_encode(&s_runtime, encoded,
                                                     (uint16_t)sizeof(encoded),
                                                     &encoded_len));
  TEST_ASSERT_EQUAL_INT(FR_TEST_TIER_LIBRARY,
                        find_bind_tier_for_slot(encoded, encoded_len,
                                                lib_slot));
  TEST_ASSERT_EQUAL_INT(-1,
                        find_bind_tier_for_slot(encoded, encoded_len,
                                                usr_slot));
}

/* Acceptance #10: a library word and a user word both round-trip through
 * fr_persist_save + base-image reset + fr_persist_restore. Without the per-slot
 * tier stamping in the restore path's bind loop, lib_word would come back
 * tagged USER; without save persisting the overlay, both names would miss. */
static void test_save_restore_round_trip_preserves_both_tiers(void) {
  char repl_out[FR_REPL_OUTPUT_BYTES];
  fr_slot_id_t lib_slot = 0;
  fr_slot_id_t usr_slot = 0;
  uint8_t encoded[FR_PROFILE_PERSISTENCE_BYTES];
  uint16_t encoded_len = 0;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-library", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "lib_word is 5", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-user", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "usr_word is 7", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);

  TEST_ASSERT_EQUAL(FR_OK, fr_persist_save(&s_runtime));

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_ERR_NOT_FOUND,
                    fr_slot_id_for_name(&s_runtime, "lib_word", &lib_slot));
  TEST_ASSERT_EQUAL(FR_ERR_NOT_FOUND,
                    fr_slot_id_for_name(&s_runtime, "usr_word", &usr_slot));

  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "lib_word", &lib_slot));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "usr_word", &usr_slot));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "lib_word", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "usr_word", repl_out,
                                      (uint16_t)sizeof(repl_out)));

  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_encode(&s_runtime, encoded,
                                                     (uint16_t)sizeof(encoded),
                                                     &encoded_len));
  TEST_ASSERT_EQUAL_INT(FR_TEST_TIER_LIBRARY,
                        find_bind_tier_for_slot(encoded, encoded_len,
                                                lib_slot));
  TEST_ASSERT_EQUAL_INT(FR_TEST_TIER_USER,
                        find_bind_tier_for_slot(encoded, encoded_len,
                                                usr_slot));
}

static void test_repl_install_library_replies_ok(void) {
  char out[FR_REPL_OUTPUT_BYTES];

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_repl_eval_line(&s_runtime, "install-library", out,
                                             (uint16_t)sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", out);
}

static void test_repl_install_user_replies_ok(void) {
  char out[FR_REPL_OUTPUT_BYTES];

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_repl_eval_line(&s_runtime, "install-user", out,
                                             (uint16_t)sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", out);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_current_payload_accepts_user_tier);
  RUN_TEST(test_current_payload_accepts_library_tier);
  RUN_TEST(test_current_payload_rejects_tier_zero);
  RUN_TEST(test_current_payload_rejects_out_of_range_tier);
  RUN_TEST(test_current_payload_rejects_missing_tier);
  RUN_TEST(test_legacy_payload_rejected);
  RUN_TEST(test_encoder_emits_tier_byte_for_overlay_bind);
  RUN_TEST(test_install_library_does_not_relabel_existing_user_slot);
  RUN_TEST(test_install_library_stamps_new_overlay_slot);
  RUN_TEST(test_install_user_after_library_keeps_library_slot);
  RUN_TEST(test_install_library_stamps_value_binding_slot);
  RUN_TEST(test_wipe_user_preserves_library_word);
  RUN_TEST(test_save_restore_round_trip_preserves_both_tiers);
  RUN_TEST(test_repl_install_library_replies_ok);
  RUN_TEST(test_repl_install_user_replies_ok);
  return UNITY_END();
}
