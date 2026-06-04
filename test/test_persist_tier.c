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
#include "crc.h"
#include "persist.h"
#include "persist_payload.h"
#include "platform.h"
#include "repl.h"
#include "runtime.h"
#include "slot.h"
#include "tagged.h"

#include "unity/unity.h"

/* SPEC D6 boot two-call sequence — declared via extern because persist.h is
 * not in T12L-7's files-in-scope list, matching the wipe-user pattern from
 * round 20. */
extern fr_err_t fr_persist_restore_library(fr_runtime_t *runtime);
extern fr_err_t fr_persist_restore_user(fr_runtime_t *runtime);

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

/* D5 acceptance #5 leans on install-library (#9) eventually persisting L1
 * to NVS, but install-library doesn't yet — tests that need NVS to already
 * contain L1 records seed it directly. The runtime passed in is whatever
 * the caller has set up; this helper encodes that runtime's overlay state
 * via the public encoder, then wraps it in a valid header so the dispatch
 * paths (fr_persist_save, fr_persist_restore, fr_persist_restore_library /
 * _user) treat the slot as a normal active save. */
static void seed_nvs_slot_from_runtime(fr_runtime_t *runtime, uint8_t slot,
                                       uint32_t generation) {
  uint8_t payload[FR_PROFILE_PERSISTENCE_BYTES];
  uint8_t header_bytes[FR_PERSIST_HEADER_BYTES];
  uint16_t payload_length = 0;

  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_encode(runtime, payload,
                                                     (uint16_t)sizeof(payload),
                                                     &payload_length));

  memset(header_bytes, 0, sizeof(header_bytes));
  memcpy(header_bytes, "FRPH", 4);
  header_bytes[4] = 1;
  header_bytes[5] = FR_PERSIST_HEADER_BYTES;
  fr_write_u32_le(&header_bytes[FR_PERSIST_PROFILE_HASH_OFFSET],
                  fr_persist_debug_profile_hash());
  fr_write_u32_le(&header_bytes[12], generation);
  header_bytes[16] = (uint8_t)(payload_length & 0xffu);
  header_bytes[17] = (uint8_t)((payload_length >> 8) & 0xffu);
  fr_write_u32_le(&header_bytes[20], fr_crc32(payload, payload_length));
  fr_write_u32_le(&header_bytes[24],
                  fr_crc32(header_bytes, FR_PERSIST_HEADER_BYTES));

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_storage_erase(slot));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_storage_write(slot, 0, header_bytes,
                                                     FR_PERSIST_HEADER_BYTES));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_storage_write(slot, FR_PERSIST_HEADER_BYTES,
                                                     payload, payload_length));
}

/* Returns the slot whose header carries the highest generation. Tests with
 * multiple save_full / save calls cannot hard-code a slot index because
 * fr_persist_pick_inactive rotates them; this helper mirrors the dispatch
 * logic by reading both headers, validating the "FRPH" magic, comparing the
 * little-endian u32 generation at offset 12, and returning the winner. The
 * caller asserts at least one slot is valid by virtue of the index check. */
static uint8_t read_active_slot(void) {
  uint8_t header[FR_PERSIST_STORAGE_SLOT_COUNT][FR_PERSIST_HEADER_BYTES];
  uint32_t generation[FR_PERSIST_STORAGE_SLOT_COUNT] = {0};
  bool valid[FR_PERSIST_STORAGE_SLOT_COUNT] = {false, false};
  uint8_t i = 0;

  for (i = 0; i < (uint8_t)FR_PERSIST_STORAGE_SLOT_COUNT; i++) {
    if (fr_platform_storage_read(i, 0, header[i], FR_PERSIST_HEADER_BYTES) !=
        FR_OK) {
      continue;
    }
    if (memcmp(header[i], "FRPH", 4) != 0) {
      continue;
    }
    valid[i] = true;
    generation[i] = (uint32_t)header[i][12] |
                    ((uint32_t)header[i][13] << 8) |
                    ((uint32_t)header[i][14] << 16) |
                    ((uint32_t)header[i][15] << 24);
  }
  TEST_ASSERT_TRUE(valid[0] || valid[1]);
  if (valid[0] && valid[1]) {
    return generation[1] > generation[0] ? (uint8_t)1 : (uint8_t)0;
  }
  return valid[0] ? (uint8_t)0 : (uint8_t)1;
}

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

/* Walks BIND records the same way find_bind_tier_for_slot does and writes
 * the encoded VALUE_INT for the BIND whose slot matches target through the
 * out pointer. Returns FR_OK on hit, FR_ERR_NOT_FOUND on miss, FR_ERR_CORRUPT
 * on malformed records. The reshaped round-trip test uses this to confirm
 * save committed the runtime's L2 mutation. */
static fr_err_t find_bind_int_value_for_slot(const uint8_t *encoded,
                                             uint16_t len,
                                             fr_slot_id_t target_slot,
                                             fr_int_t *out_value) {
  uint16_t i = 5;
  while (i < len) {
    uint8_t tag = encoded[i];

    if (tag == FR_TEST_TIER_RECORD_END) {
      return FR_ERR_NOT_FOUND;
    }
    if (tag == FR_TEST_TIER_RECORD_CODE) {
      uint16_t length = 0;

      if ((uint16_t)(i + 5) > len) {
        return FR_ERR_CORRUPT;
      }
      length = (uint16_t)encoded[i + 3] | ((uint16_t)encoded[i + 4] << 8);
      i = (uint16_t)(i + 5 + length);
      continue;
    }
    if (tag == FR_TEST_TIER_RECORD_BIND) {
      uint16_t slot = 0;
      uint8_t value_kind = 0;

      if ((uint16_t)(i + 5) > len) {
        return FR_ERR_CORRUPT;
      }
      slot = (uint16_t)encoded[i + 1] | ((uint16_t)encoded[i + 2] << 8);
      value_kind = encoded[i + 4];
      i = (uint16_t)(i + 5);
      if (value_kind == FR_TEST_TIER_VALUE_INT) {
        if (slot == target_slot) {
#if FR_WORD_SIZE == 16
          *out_value = (fr_int_t)(int16_t)((uint16_t)encoded[i] |
                                           ((uint16_t)encoded[i + 1] << 8));
#else
          *out_value = (fr_int_t)(int32_t)((uint32_t)encoded[i] |
                                           ((uint32_t)encoded[i + 1] << 8) |
                                           ((uint32_t)encoded[i + 2] << 16) |
                                           ((uint32_t)encoded[i + 3] << 24));
#endif
          return FR_OK;
        }
        i = (uint16_t)(i + FR_TEST_TIER_INT_BYTES);
        continue;
      }
      switch (value_kind) {
      case FR_TEST_TIER_VALUE_NIL:
      case FR_TEST_TIER_VALUE_FALSE:
      case FR_TEST_TIER_VALUE_TRUE:
        break;
      case FR_TEST_TIER_VALUE_CODE:
      case FR_TEST_TIER_VALUE_NATIVE:
      case FR_TEST_TIER_VALUE_OBJECT:
        i = (uint16_t)(i + 2);
        break;
      default:
        return FR_ERR_CORRUPT;
      }
      continue;
    }
    if (tag == FR_TEST_TIER_RECORD_NAME) {
      uint8_t name_length = 0;

      if ((uint16_t)(i + 4) > len) {
        return FR_ERR_CORRUPT;
      }
      name_length = encoded[i + 3];
      i = (uint16_t)(i + 4 + name_length);
      continue;
    }
    return FR_ERR_CORRUPT;
  }
  return FR_ERR_NOT_FOUND;
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

/* Acceptance #5: save reads the existing payload's L1 records from NVS,
 * encodes the runtime's L2 records, and writes the merged payload. L1 bytes
 * persist byte-for-byte across save. The pre-#5 implementation encoded the
 * runtime overlay as a single fresh payload — that path doesn't consult NVS,
 * so a save with no L1 in the runtime would produce a payload with no L1
 * BIND. This test seeds slot 0 with an L1-only image (the post-#9 shape of
 * install-library's NVS write) then sets up a runtime that has only L2 and
 * calls save. The L1 record bytes in the new slot 1 payload must equal the
 * L1 record bytes in the slot 0 seed. */
static void test_save_preserves_library_records_from_nvs(void) {
  uint8_t l1_only_payload[FR_PROFILE_PERSISTENCE_BYTES];
  uint8_t saved_header[FR_PERSIST_HEADER_BYTES];
  uint8_t saved_payload[FR_PROFILE_PERSISTENCE_BYTES];
  uint16_t l1_payload_length = 0;
  uint16_t saved_length = 0;
  uint16_t l1_records_length = 0;
  char repl_out[FR_REPL_OUTPUT_BYTES];

  fr_platform_storage_debug_reset();
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-library", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "lib_word is 5", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);

  /* Capture the L1-only payload bytes so we can compare them later. */
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_encode(&s_runtime, l1_only_payload,
                                                     (uint16_t)sizeof(l1_only_payload),
                                                     &l1_payload_length));

  /* Seed slot 0 with the L1-only payload + a valid header. */
  seed_nvs_slot_from_runtime(&s_runtime, 0, 1);

  /* Reset the runtime fully and bring up only L2 — proves the saved L1
   * came from NVS, not from runtime overlay state. */
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-user", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "usr_word is 7", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);

  /* fr_persist_pick_inactive picks slot 1 since slot 0 has generation 1.
   * Save reads slot 0's payload, extracts the L1 records (BIND + NAME for
   * lib_word), and writes them as the prefix of slot 1's payload. */
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_save(&s_runtime));

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_storage_read(1, 0, saved_header,
                                                     FR_PERSIST_HEADER_BYTES));
  saved_length = (uint16_t)saved_header[16] |
                 ((uint16_t)saved_header[17] << 8);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_storage_read(1, FR_PERSIST_HEADER_BYTES,
                                             saved_payload, saved_length));

  /* L1-only payload layout: magic+version(5) + L1 records + END(1).
   * The L1 record bytes are at [5 .. l1_payload_length - 2]. The new save
   * payload starts with magic+version(5) + the same L1 record bytes (the
   * extract step is a deterministic re-encode), then L2 records, then END.
   * Therefore saved_payload[5 .. 5 + l1_records_length - 1] equals
   * l1_only_payload[5 .. 5 + l1_records_length - 1]. */
  TEST_ASSERT_GREATER_OR_EQUAL(7, l1_payload_length);
  l1_records_length = (uint16_t)(l1_payload_length - 6);
  TEST_ASSERT_GREATER_THAN((uint16_t)(5 + l1_records_length), saved_length);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(&l1_only_payload[5], &saved_payload[5],
                                l1_records_length);
}

/* Acceptance #5 — VALUE_CODE case. A library word with a fn body produces an
 * L1 BIND of VALUE_CODE plus the CODE record it references. The R42 extract
 * implementation rejected VALUE_CODE / VALUE_OBJECT L1 BINDs outright with
 * FR_ERR_UNSUPPORTED, so this exact sequence (install lib word with body,
 * install user word, save) caused fr_persist_save to fail. SPEC D5 demands
 * L1 NVS bytes survive save unchanged regardless of value_kind. The new
 * extract walks the L1 BIND -> CODE closure (and PUSH_CODE_ID / PUSH_OBJECT_ID
 * operands within those bodies) and re-emits each record byte-verbatim from
 * the decoded source, so the L1 region in the saved payload byte-equals the
 * L1 region the install-library encode produced. */
static void test_save_preserves_library_code_word_from_nvs(void) {
  uint8_t l1_only_payload[FR_PROFILE_PERSISTENCE_BYTES];
  uint8_t saved_header[FR_PERSIST_HEADER_BYTES];
  uint8_t saved_payload[FR_PROFILE_PERSISTENCE_BYTES];
  uint16_t l1_payload_length = 0;
  uint16_t saved_length = 0;
  uint16_t l1_records_length = 0;
  char repl_out[FR_REPL_OUTPUT_BYTES];

  fr_platform_storage_debug_reset();
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-library", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "lib_thunk is fn [ 99 ]",
                                      repl_out, (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);

  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_encode(&s_runtime, l1_only_payload,
                                                     (uint16_t)sizeof(l1_only_payload),
                                                     &l1_payload_length));
  seed_nvs_slot_from_runtime(&s_runtime, 0, 1);

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-user", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "usr_word is 7", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);

  /* Under R42 this returns FR_ERR_UNSUPPORTED because extract_library_records
   * refuses VALUE_CODE L1 BINDs. The new extract walks the closure (CODE 0
   * here, the fn body), so save succeeds and the L1 region survives. */
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_save(&s_runtime));

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_storage_read(1, 0, saved_header,
                                                     FR_PERSIST_HEADER_BYTES));
  saved_length = (uint16_t)saved_header[16] |
                 ((uint16_t)saved_header[17] << 8);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_storage_read(1, FR_PERSIST_HEADER_BYTES,
                                             saved_payload, saved_length));

  TEST_ASSERT_GREATER_OR_EQUAL(7, l1_payload_length);
  l1_records_length = (uint16_t)(l1_payload_length - 6);
  TEST_ASSERT_GREATER_THAN((uint16_t)(5 + l1_records_length), saved_length);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(&l1_only_payload[5], &saved_payload[5],
                                l1_records_length);
}

#if FR_FEATURE_TEXT
/* Acceptance #5 — text-bearing fn body. A library word whose body pushes a
 * text literal produces, on the wire, a TEXT record before the CODE record
 * that references it (the install encoder walks PUSH_OBJECT_ID operands
 * first, so the TEXT lands ahead of its parent CODE). The R44 extract
 * grouped all L1 CODEs ahead of all L1 TEXTs, reordering those bytes; this
 * test fails against that grouping and passes against the source-order
 * walker because the saved L1 prefix byte-equals the input L1 region only
 * when TEXT comes before CODE just like the input. */
static void test_save_preserves_library_text_in_code_from_nvs(void) {
  uint8_t l1_only_payload[FR_PROFILE_PERSISTENCE_BYTES];
  uint8_t saved_header[FR_PERSIST_HEADER_BYTES];
  uint8_t saved_payload[FR_PROFILE_PERSISTENCE_BYTES];
  uint16_t l1_payload_length = 0;
  uint16_t saved_length = 0;
  uint16_t l1_records_length = 0;
  char repl_out[FR_REPL_OUTPUT_BYTES];

  fr_platform_storage_debug_reset();
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-library", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime,
                                      "lib_text is fn [ \"lib-hello\" ]",
                                      repl_out, (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);

  TEST_ASSERT_EQUAL(FR_OK, fr_persist_payload_encode(&s_runtime, l1_only_payload,
                                                     (uint16_t)sizeof(l1_only_payload),
                                                     &l1_payload_length));
  seed_nvs_slot_from_runtime(&s_runtime, 0, 1);

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-user", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "usr_word is 7", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);

  TEST_ASSERT_EQUAL(FR_OK, fr_persist_save(&s_runtime));

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_storage_read(1, 0, saved_header,
                                                     FR_PERSIST_HEADER_BYTES));
  saved_length = (uint16_t)saved_header[16] |
                 ((uint16_t)saved_header[17] << 8);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_storage_read(1, FR_PERSIST_HEADER_BYTES,
                                             saved_payload, saved_length));

  /* L1 record region is the source payload minus magic+version (5) and END
   * (1). saved_payload's L1 prefix sits at offset 5 (after its own
   * magic+version) and must byte-equal the input's L1 region. The new test
   * exercises the TEXT-before-CODE interleaving the encoder produces. */
  TEST_ASSERT_GREATER_OR_EQUAL(7, l1_payload_length);
  l1_records_length = (uint16_t)(l1_payload_length - 6);
  TEST_ASSERT_GREATER_THAN((uint16_t)(5 + l1_records_length), saved_length);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(&l1_only_payload[5], &saved_payload[5],
                                l1_records_length);
}
#endif

/* Acceptance #10: the full save/restore round-trip integrates D5's tier
 * separation across a single device session — install through restore,
 * with runtime mutations between the persisted operations to prove which
 * tier the implementation actually owns.
 *
 *   1. install-library + `lib_word is 5`. R54's auto-save commits the
 *      L1-only payload to NVS; this is the "L1 bytes in NVS" baseline the
 *      rest of the test compares against. install-user + `usr_word is 7`
 *      adds runtime L2 (no NVS write — L2 mode skips auto-save).
 *      First explicit save merges the existing NVS L1 prefix with the
 *      runtime's L2; the new payload's L1 region byte-equals the install
 *      baseline (D5: save preserves L1 bytes from NVS).
 *   2. Mutate runtime: lib_word := 99, usr_word := 13. Save again.
 *      D5: save's L1 prefix still comes from NVS, NOT from runtime — so
 *      the new payload's L1 region still byte-equals the install baseline
 *      even though runtime L1 disagrees. L2 in the new payload encodes
 *      usr_word := 13 (runtime's mutation flows through L2 into NVS).
 *   3. Mutate runtime again: lib_word := 77. Public restore.
 *      D5: restore brings only L2 back from NVS. Runtime L1 keeps the 77
 *      (the latest mutation, never written to NVS); runtime L2 reads 13
 *      (the value the previous save committed to NVS).
 *
 * Regressions this catches:
 *   - Pre-R42 save (full re-encode from runtime): step 2's L1-bytes-
 *     unchanged assertion fails because save would emit lib_word := 99.
 *   - Pre-R50 restore (FULL path: reset + replay both tiers): step 3's
 *     runtime lib_word == 77 assertion fails because restore would drop
 *     L1 to NVS's lib_word == 5.
 *   - Pre-R54 install-library / L1-mode compile (no auto-save): step 1's
 *     install-baseline payload would carry no L1 records, so the byte
 *     comparison would degenerate. */
static void test_save_restore_round_trip_preserves_both_tiers(void) {
  char repl_out[FR_REPL_OUTPUT_BYTES];
  uint8_t install_header[FR_PERSIST_HEADER_BYTES];
  uint8_t install_payload[FR_PROFILE_PERSISTENCE_BYTES];
  uint16_t install_length = 0;
  uint16_t l1_region_length = 0;
  uint8_t slot = 0;
  fr_slot_id_t lib_slot = 0;
  fr_slot_id_t usr_slot = 0;
  fr_tagged_t tagged = 0;
  fr_int_t value = 0;
  fr_tagged_t mutated = 0;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-library", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "lib_word is 5", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);

  /* Snapshot post-install NVS: L1-only payload that the next save must
   * preserve byte-for-byte. */
  slot = read_active_slot();
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_storage_read(slot, 0, install_header,
                                                    FR_PERSIST_HEADER_BYTES));
  install_length = (uint16_t)install_header[16] |
                   ((uint16_t)install_header[17] << 8);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_storage_read(slot, FR_PERSIST_HEADER_BYTES,
                                             install_payload, install_length));
  /* L1 region = payload minus magic+version (5) and END (1). */
  TEST_ASSERT_GREATER_OR_EQUAL(7, install_length);
  l1_region_length = (uint16_t)(install_length - 6);

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

  /* Step 1: first save. New payload's L1 region byte-equals the baseline. */
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_save(&s_runtime));
  {
    uint8_t save_header[FR_PERSIST_HEADER_BYTES];
    uint8_t save_payload[FR_PROFILE_PERSISTENCE_BYTES];
    uint16_t save_length = 0;

    slot = read_active_slot();
    TEST_ASSERT_EQUAL(FR_OK, fr_platform_storage_read(slot, 0, save_header,
                                                      FR_PERSIST_HEADER_BYTES));
    save_length = (uint16_t)save_header[16] |
                  ((uint16_t)save_header[17] << 8);
    TEST_ASSERT_EQUAL(FR_OK,
                      fr_platform_storage_read(slot, FR_PERSIST_HEADER_BYTES,
                                               save_payload, save_length));
    TEST_ASSERT_GREATER_THAN((uint16_t)(5 + l1_region_length), save_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&install_payload[5], &save_payload[5],
                                  l1_region_length);
    {
      fr_int_t encoded_value = 0;
      TEST_ASSERT_EQUAL(FR_OK,
                        find_bind_int_value_for_slot(save_payload, save_length,
                                                     usr_slot, &encoded_value));
      TEST_ASSERT_EQUAL(7, encoded_value);
    }
  }

  /* Step 2: mutate runtime L1 and L2, save again. NVS L1 stays the install
   * baseline; NVS L2 picks up the runtime mutation. */
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(99, &mutated));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_write(&s_runtime, lib_slot, mutated));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(13, &mutated));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_write(&s_runtime, usr_slot, mutated));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_save(&s_runtime));
  {
    uint8_t save_header[FR_PERSIST_HEADER_BYTES];
    uint8_t save_payload[FR_PROFILE_PERSISTENCE_BYTES];
    uint16_t save_length = 0;

    slot = read_active_slot();
    TEST_ASSERT_EQUAL(FR_OK, fr_platform_storage_read(slot, 0, save_header,
                                                      FR_PERSIST_HEADER_BYTES));
    save_length = (uint16_t)save_header[16] |
                  ((uint16_t)save_header[17] << 8);
    TEST_ASSERT_EQUAL(FR_OK,
                      fr_platform_storage_read(slot, FR_PERSIST_HEADER_BYTES,
                                               save_payload, save_length));
    TEST_ASSERT_GREATER_THAN((uint16_t)(5 + l1_region_length), save_length);
    /* L1 bytes in NVS: still the install baseline. Runtime L1 mutation
     * did NOT leak through save (D5). */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&install_payload[5], &save_payload[5],
                                  l1_region_length);
    /* L2 BIND value: 13, the runtime's mutation (D5: save writes L2). */
    {
      fr_int_t encoded_value = 0;
      TEST_ASSERT_EQUAL(FR_OK,
                        find_bind_int_value_for_slot(save_payload, save_length,
                                                     usr_slot, &encoded_value));
      TEST_ASSERT_EQUAL(13, encoded_value);
    }
  }

  /* Step 3: mutate L1 to a third value never persisted, then restore.
   * Public restore is L2-only — runtime L1 keeps the 77, runtime L2 picks
   * up the 13 saved in step 2. */
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(77, &mutated));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_write(&s_runtime, lib_slot, mutated));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore(&s_runtime));

  TEST_ASSERT_EQUAL(FR_OK, fr_slot_read(&s_runtime, lib_slot, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(tagged, &value));
  TEST_ASSERT_EQUAL(77, value);

  TEST_ASSERT_EQUAL(FR_OK, fr_slot_read(&s_runtime, usr_slot, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(tagged, &value));
  TEST_ASSERT_EQUAL(13, value);
}

/* SPEC D5 / acceptance #6: the public `restore` word applies only L2
 * records from NVS, leaving runtime L1 alone. A FULL restore (pre-R46
 * behavior) would reset the runtime and replay both tiers, overwriting any
 * runtime L1 mutation; the SPEC says boot owns L1 and `restore` must not
 * undo whatever L1 state is currently live.
 *
 * The test installs lib_word (L1) and usr_word (L2), snapshots into NVS,
 * mutates lib_word in runtime to a value not present in NVS, mutates
 * usr_word in runtime to a value not present in NVS, then calls
 * fr_persist_restore. After restore: lib_word in runtime keeps the mutated
 * value (restore did not touch L1); usr_word in runtime is back to the NVS
 * value (restore did replace L2). This sequence fails against the pre-R46
 * FULL-restore implementation — that path resets and re-applies both
 * tiers, so lib_word would drop to 5. */
static void test_restore_preserves_runtime_library_tier(void) {
  char repl_out[FR_REPL_OUTPUT_BYTES];
  fr_slot_id_t lib_slot = 0;
  fr_slot_id_t usr_slot = 0;
  fr_tagged_t mutated_lib = 0;
  fr_tagged_t mutated_usr = 0;
  fr_tagged_t restored_lib = 0;
  fr_tagged_t restored_usr = 0;
  fr_int_t lib_val = 0;
  fr_int_t usr_val = 0;

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

  /* R54 made install-library + the L1-mode compile path auto-save the
   * runtime to NVS so library replacement persists across a power cycle.
   * The seed below models a single both-tier save; clear the in-session
   * auto-saves first so that seed is the only valid slot the public
   * restore can pick. */
  fr_platform_storage_debug_reset();
  seed_nvs_slot_from_runtime(&s_runtime, 0, 1);

  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(99, &mutated_lib));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_write(&s_runtime, lib_slot, mutated_lib));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(13, &mutated_usr));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_write(&s_runtime, usr_slot, mutated_usr));

  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore(&s_runtime));

  TEST_ASSERT_EQUAL(FR_OK, fr_slot_read(&s_runtime, lib_slot, &restored_lib));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(restored_lib, &lib_val));
  TEST_ASSERT_EQUAL(99, lib_val);

  TEST_ASSERT_EQUAL(FR_OK, fr_slot_read(&s_runtime, usr_slot, &restored_usr));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(restored_usr, &usr_val));
  TEST_ASSERT_EQUAL(7, usr_val);
}

/* SPEC D5 / acceptance #6: the public `restore` word brings L2 back from
 * NVS without touching L1 — including L1's CODE / TEXT / object records.
 * The boot pipeline installed those once; restore must not install them a
 * second time, or the runtime's code table fills up across save/restore
 * cycles. The test installs a code-bearing library word (one L1 CODE
 * record) plus an int-valued user word (an L2 VALUE_INT BIND, no L2
 * resources), runs the full boot pair, snapshots the code count, then
 * calls public restore. The L2 closure of resources here is empty (the
 * L2 bind references no CODE / object), so a SPEC-conformant restore
 * installs nothing and code.count stays put. The pre-R50 full-payload
 * install path would re-emit the L1 thunk's CODE here and grow code.count
 * by one — the assertion below catches that regression. */
static void test_restore_does_not_reinstall_library_resources(void) {
  char repl_out[FR_REPL_OUTPUT_BYTES];
  fr_slot_id_t lib_slot = 0;
  fr_slot_id_t usr_slot = 0;
  uint16_t code_count_after_boot = 0;
  fr_tagged_t tagged = 0;
  fr_int_t value = 0;

  fr_platform_storage_debug_reset();
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-library", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "lib_thunk is fn [ 99 ]",
                                      repl_out, (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);

  /* Seed slot 0 with the L1-only image install-library will eventually
   * write itself (#9). Save below reads it to preserve L1 bytes forward. */
  seed_nvs_slot_from_runtime(&s_runtime, 0, 1);

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-user", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "usr_word is 7", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "lib_thunk", &lib_slot));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "usr_word", &usr_slot));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_save(&s_runtime));

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore_library(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore_user(&s_runtime));
  code_count_after_boot = s_runtime.code.count;

  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore(&s_runtime));
  TEST_ASSERT_EQUAL(code_count_after_boot, s_runtime.code.count);

  TEST_ASSERT_EQUAL(FR_OK, fr_slot_read(&s_runtime, usr_slot, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(tagged, &value));
  TEST_ASSERT_EQUAL(7, value);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "lib_thunk", repl_out,
                                      (uint16_t)sizeof(repl_out)));
}

/* D6 boot restore order: when a single payload carries BINDs in both tiers,
 * the L1 pass must run before the L2 pass so user code wakes up with the
 * library it references already installed. The wire payload encodes two
 * BINDs against the same slot in [tier=USER value=99, tier=LIBRARY value=11]
 * order; only an L1-before-L2 restore leaves the L2 value (99) winning. The
 * pre-D6 single-pass loop walks payload order and would leave the slot at
 * the L1 value (11). */
static void test_boot_restore_applies_library_before_user(void) {
  /* magic 4 + version 1 + BIND header bytes (5) + int payload (N) per
   * record, two BINDs, then END 1. */
  uint8_t payload[(uint16_t)(4 + 1 + 2 * (5 + FR_TEST_TIER_INT_BYTES) + 1)];
  uint8_t *p = payload;
  fr_tagged_t tagged = 0;
  fr_int_t decoded = 0;

  *p++ = 'F';
  *p++ = 'R';
  *p++ = 'P';
  *p++ = 'O';
  *p++ = FR_TEST_TIER_PAYLOAD_VERSION;
  *p++ = FR_TEST_TIER_RECORD_BIND;
  write_u16_le(p, FR_TEST_TIER_SLOT);
  p += 2;
  *p++ = FR_TEST_TIER_USER;
  *p++ = FR_TEST_TIER_VALUE_INT;
  write_int_le(p, 99);
  p += FR_TEST_TIER_INT_BYTES;
  *p++ = FR_TEST_TIER_RECORD_BIND;
  write_u16_le(p, FR_TEST_TIER_SLOT);
  p += 2;
  *p++ = FR_TEST_TIER_LIBRARY;
  *p++ = FR_TEST_TIER_VALUE_INT;
  write_int_le(p, 11);
  p += FR_TEST_TIER_INT_BYTES;
  *p++ = FR_TEST_TIER_RECORD_END;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_persist_payload_restore(&s_runtime, payload,
                                               (uint16_t)sizeof(payload)));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_read(&s_runtime, FR_TEST_TIER_SLOT, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(tagged, &decoded));
  TEST_ASSERT_EQUAL(99, decoded);
}

/* SPEC D6 boot-call layer: the kernel runs the L1 restore and the L2
 * restore as two distinct calls; user definitions only land after the
 * library tier is in place. This test exercises the public NVS-level pair
 * (fr_persist_restore_library / fr_persist_restore_user) the boot pipeline
 * in fr_repl_startup_restore_and_boot uses, and asserts the intermediate
 * state between the two calls. A single-call FULL restore would not show
 * the lib-overlay / no-user-overlay intermediate state. */
static void test_boot_two_call_applies_library_before_user(void) {
  char repl_out[FR_REPL_OUTPUT_BYTES];
  fr_slot_id_t lib_slot = 0;
  fr_slot_id_t usr_slot = 0;
  fr_tagged_t tagged = 0;
  fr_int_t value = 0;

  fr_platform_storage_debug_reset();
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-library", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "lib_word is 5", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);

  /* D5: save writes only L2 from runtime; L1 enters NVS via install-library
   * (acceptance #9). Seed slot 0 with the L1-only image so save can preserve
   * it forward into the merged save the boot pipeline reads back below. */
  seed_nvs_slot_from_runtime(&s_runtime, 0, 1);

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-user", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "usr_word is 7", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_save(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "lib_word", &lib_slot));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "usr_word", &usr_slot));

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_FALSE(fr_slot_is_overlay(&s_runtime, lib_slot));
  TEST_ASSERT_FALSE(fr_slot_is_overlay(&s_runtime, usr_slot));

  /* L1 pass: SPEC D6 "library Frothy definitions compile and install."
   * Install means name-bound, not just slot-written — between the L1 and
   * L2 passes the library word must resolve via fr_slot_id_for_name so
   * the L2 pass (and any L2 definition) can reference it. A regression
   * that defers all name binding to the L2 pass would leave the library
   * tier in a bind-only state where the lib_word lookup fails. A
   * regression that applies all names in the L1 pass would resolve
   * usr_word here too. */
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore_library(&s_runtime));
  TEST_ASSERT_TRUE(fr_slot_is_overlay(&s_runtime, lib_slot));
  TEST_ASSERT_FALSE(fr_slot_is_overlay(&s_runtime, usr_slot));
  {
    fr_slot_id_t resolved = 0;

    TEST_ASSERT_EQUAL(FR_OK,
                      fr_slot_id_for_name(&s_runtime, "lib_word", &resolved));
    TEST_ASSERT_EQUAL(lib_slot, resolved);
    TEST_ASSERT_EQUAL(FR_ERR_NOT_FOUND,
                      fr_slot_id_for_name(&s_runtime, "usr_word", &resolved));
  }
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_read(&s_runtime, lib_slot, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(tagged, &value));
  TEST_ASSERT_EQUAL(5, value);

  /* L2 pass: user binding lands and resolves by name; library name still
   * resolves to the same slot. */
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore_user(&s_runtime));
  TEST_ASSERT_TRUE(fr_slot_is_overlay(&s_runtime, lib_slot));
  TEST_ASSERT_TRUE(fr_slot_is_overlay(&s_runtime, usr_slot));
  {
    fr_slot_id_t resolved = 0;

    TEST_ASSERT_EQUAL(FR_OK,
                      fr_slot_id_for_name(&s_runtime, "lib_word", &resolved));
    TEST_ASSERT_EQUAL(lib_slot, resolved);
    TEST_ASSERT_EQUAL(FR_OK,
                      fr_slot_id_for_name(&s_runtime, "usr_word", &resolved));
    TEST_ASSERT_EQUAL(usr_slot, resolved);
  }
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_read(&s_runtime, lib_slot, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(tagged, &value));
  TEST_ASSERT_EQUAL(5, value);
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_read(&s_runtime, usr_slot, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(tagged, &value));
  TEST_ASSERT_EQUAL(7, value);
}

/* A standalone L2 pass — with no preceding L1 pass on the same payload —
 * has no map to translate decoded local ids against. The new entry point
 * surfaces FR_ERR_NOT_FOUND rather than silently applying L2 binds against
 * stale or zero maps. Calling order matters; the boot pipeline owns it. */
static void test_user_restore_without_prior_library_returns_not_found(void) {
  char repl_out[FR_REPL_OUTPUT_BYTES];

  fr_platform_storage_debug_reset();
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
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

  TEST_ASSERT_EQUAL(FR_ERR_NOT_FOUND, fr_persist_restore_user(&s_runtime));
}

/* SPEC D10 + D5 acceptance #9: receiving install-library drops L1 from the
 * device — runtime binds AND the L1 closure inside NVS — before accepting
 * the next definitions. L2 bytes in NVS are preserved byte-for-byte across
 * the entry. The test seeds the both-tier image into slot 0 the way a save
 * would, captures the pre-install-library NVS bytes, re-issues
 * install-library, and asserts:
 *   - runtime lib_word no longer resolves;
 *   - runtime usr_word still resolves;
 *   - the new NVS payload has no BIND for lib_slot;
 *   - the new NVS payload still carries usr_slot's BIND with USER tier,
 *     byte-identical to the pre-payload slice.
 * Against the R51 handler (sets install_tier only) the runtime lib_word
 * assertion fails. Against any impl that re-encodes L2 instead of copying
 * the source bytes the byte-identical slice assertion fails. */
static void test_install_library_drops_l1_runtime_and_nvs_preserves_user(void) {
  uint8_t pre_payload[FR_PROFILE_PERSISTENCE_BYTES];
  uint8_t pre_header[FR_PERSIST_HEADER_BYTES];
  uint8_t post_payload[FR_PROFILE_PERSISTENCE_BYTES];
  uint8_t post_header[FR_PERSIST_HEADER_BYTES];
  uint16_t pre_length = 0;
  uint16_t post_length = 0;
  fr_slot_id_t lib_slot = 0;
  fr_slot_id_t usr_slot = 0;
  fr_slot_id_t scratch_slot = 0;
  char repl_out[FR_REPL_OUTPUT_BYTES];

  fr_platform_storage_debug_reset();
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

  /* Both-tier NVS image. The test models the post-save state where the user
   * has issued `save` after an install-library + install-user round. R54's
   * per-line install save has already written intermediate slots during the
   * REPL session above; wipe them so this seed is the only valid payload
   * and the next install-library can pick the inactive slot cleanly. */
  fr_platform_storage_debug_reset();
  seed_nvs_slot_from_runtime(&s_runtime, 0, 1);

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_storage_read(0, 0, pre_header,
                                                     FR_PERSIST_HEADER_BYTES));
  pre_length = (uint16_t)pre_header[16] | ((uint16_t)pre_header[17] << 8);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_storage_read(0, FR_PERSIST_HEADER_BYTES,
                                             pre_payload, pre_length));
  TEST_ASSERT_EQUAL_INT(FR_TEST_TIER_LIBRARY,
                        find_bind_tier_for_slot(pre_payload, pre_length,
                                                lib_slot));
  TEST_ASSERT_EQUAL_INT(FR_TEST_TIER_USER,
                        find_bind_tier_for_slot(pre_payload, pre_length,
                                                usr_slot));

  /* Slice under test: install-library on receipt drops L1 from runtime and
   * rewrites NVS to L2-only. */
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-library", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);

  TEST_ASSERT_EQUAL(FR_ERR_NOT_FOUND,
                    fr_slot_id_for_name(&s_runtime, "lib_word", &scratch_slot));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "usr_word", &scratch_slot));
  TEST_ASSERT_EQUAL(usr_slot, scratch_slot);

  /* pick_inactive picks slot 1 (empty); the new payload lands there. */
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_storage_read(1, 0, post_header,
                                                     FR_PERSIST_HEADER_BYTES));
  post_length = (uint16_t)post_header[16] | ((uint16_t)post_header[17] << 8);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_storage_read(1, FR_PERSIST_HEADER_BYTES,
                                             post_payload, post_length));
  TEST_ASSERT_EQUAL_INT(-1,
                        find_bind_tier_for_slot(post_payload, post_length,
                                                lib_slot));
  TEST_ASSERT_EQUAL_INT(FR_TEST_TIER_USER,
                        find_bind_tier_for_slot(post_payload, post_length,
                                                usr_slot));

  /* L2 BIND bytes preserved byte-for-byte. With `lib_word is 5` and
   * `usr_word is 7` the encoder writes magic + version + BIND(lib) +
   * BIND(usr) + (NAME records) + END. Each VALUE_INT BIND is 5 +
   * FR_TEST_TIER_INT_BYTES bytes: tag(1) + slot(2) + tier(1) + value_kind(1)
   * + int(N). The L2 BIND lives at offset 5 + bind_record_length in
   * pre_payload (the first BIND is the L1 lib_word) and at offset 5 in
   * post_payload (L1 was dropped). The 9-byte BIND slice must match. */
  {
    uint16_t bind_record_length =
        (uint16_t)(5u + (uint16_t)FR_TEST_TIER_INT_BYTES);
    uint16_t pre_l2_offset = (uint16_t)(5u + bind_record_length);
    TEST_ASSERT_GREATER_THAN(pre_l2_offset + bind_record_length, pre_length);
    TEST_ASSERT_GREATER_THAN((uint16_t)(5u + bind_record_length), post_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&pre_payload[pre_l2_offset],
                                  &post_payload[5], bind_record_length);
  }
}

/* SPEC D3 + D9 + D10 close on the install-library lifecycle: a fresh
 * install over the wire (`install-library` + the library lines) replaces
 * every prior L1 record on the device, and the replacement persists across
 * a power cycle. Without per-line save during install, the new L1 lives
 * only in the runtime overlay — reset + boot-restore would lose it.
 *
 * Library v1 = {lib_a is 1, lib_b is 2}. Add a user word, save the user
 * tier. Power cycle (base-image reinstall + the two-call boot restore):
 * all three resolve. Issue install-library + `lib_b is 99` — the install
 * receipt drops lib_a (D10) and the line that follows persists lib_b's
 * new value into NVS (D3). Power cycle again: lib_a is gone for good,
 * lib_b reads 99, the user word survives. The pre-R54 handler stops at
 * the runtime overlay, so lib_b would be missing after the second power
 * cycle — the final assertion block catches that. */
static void test_install_library_persists_replacement_l1(void) {
  char repl_out[FR_REPL_OUTPUT_BYTES];
  fr_slot_id_t scratch = 0;
  fr_tagged_t tagged = 0;
  fr_int_t value = 0;

  fr_platform_storage_debug_reset();
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-library", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "lib_a is 1", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "lib_b is 2", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-user", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "usr_x is 7", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_save(&s_runtime));

  /* Boot cycle 1: all three words from library v1 + user state restore. */
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore_library(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore_user(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_id_for_name(&s_runtime, "lib_a", &scratch));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_id_for_name(&s_runtime, "lib_b", &scratch));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_id_for_name(&s_runtime, "usr_x", &scratch));

  /* Library v2 = {lib_b is 99}. install-library wipes lib_a from runtime
   * and NVS; the next line writes lib_b=99 through to NVS. */
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "install-library", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "lib_b is 99", repl_out,
                                      (uint16_t)sizeof(repl_out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", repl_out);

  TEST_ASSERT_EQUAL(FR_ERR_NOT_FOUND,
                    fr_slot_id_for_name(&s_runtime, "lib_a", &scratch));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_id_for_name(&s_runtime, "lib_b", &scratch));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_read(&s_runtime, scratch, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(tagged, &value));
  TEST_ASSERT_EQUAL(99, value);

  /* Boot cycle 2: prove the replacement persisted. Without R54's per-line
   * save during install, lib_b would not resolve here. */
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore_library(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore_user(&s_runtime));

  TEST_ASSERT_EQUAL(FR_ERR_NOT_FOUND,
                    fr_slot_id_for_name(&s_runtime, "lib_a", &scratch));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_id_for_name(&s_runtime, "lib_b", &scratch));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_read(&s_runtime, scratch, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(tagged, &value));
  TEST_ASSERT_EQUAL(99, value);
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_id_for_name(&s_runtime, "usr_x", &scratch));
  TEST_ASSERT_EQUAL(FR_OK, fr_slot_read(&s_runtime, scratch, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(tagged, &value));
  TEST_ASSERT_EQUAL(7, value);
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
  RUN_TEST(test_save_preserves_library_records_from_nvs);
  RUN_TEST(test_save_preserves_library_code_word_from_nvs);
#if FR_FEATURE_TEXT
  RUN_TEST(test_save_preserves_library_text_in_code_from_nvs);
#endif
  RUN_TEST(test_save_restore_round_trip_preserves_both_tiers);
  RUN_TEST(test_restore_preserves_runtime_library_tier);
  RUN_TEST(test_restore_does_not_reinstall_library_resources);
  RUN_TEST(test_boot_restore_applies_library_before_user);
  RUN_TEST(test_boot_two_call_applies_library_before_user);
  RUN_TEST(test_user_restore_without_prior_library_returns_not_found);
  RUN_TEST(test_install_library_drops_l1_runtime_and_nvs_preserves_user);
  RUN_TEST(test_install_library_persists_replacement_l1);
  RUN_TEST(test_repl_install_library_replies_ok);
  RUN_TEST(test_repl_install_user_replies_ok);
  return UNITY_END();
}
