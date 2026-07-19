/*
 * Unity tests for T16 bytes kind.
 *
 * D22 coverage: 7 natives happy path, text.pack happy + capacity,
 * slot reject, cell + record reject, generation bump, generation
 * exhaust, loop reset, event-depth suppression.
 * Migrated-native regression is covered by test_t15_net.c and
 * test_t15b_tcp.c (updated for bytes-shaped assertions per D23).
 */

#include "base_image.h"
#include "object.h"
#include "platform.h"
#include "repl.h"
#include "runtime.h"
#include "tagged.h"
#include "types.h"

#include "unity/unity.h"

#include <stdint.h>

void setUp(void) {
#if FR_FEATURE_NET
  fr_host_net_reset();
#endif
}

void tearDown(void) {}

#if FR_FEATURE_BYTES

static fr_runtime_t s_runtime;

static void install_base(void) {
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
}

static void eval_ok(const char *line) {
  char out[256];
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, line, out, sizeof(out)));
}

static fr_err_t eval_err(const char *line) {
  char out[256];
  return fr_repl_eval_line(&s_runtime, line, out, sizeof(out));
}

static void eval_expect(const char *line, const char *expected) {
  char out[256];
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, line, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING(expected, out);
}

static void eval_error_expect(const char *line, fr_err_t err,
                              const char *expected) {
  char out[256];
  TEST_ASSERT_EQUAL(err, fr_repl_eval_line(&s_runtime, line, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING(expected, out);
}

/* --- 7 natives happy path --- */

static void test_bytes_from_text(void) {
  install_base();
  eval_expect("bytes.length: bytes.from-text: \"hello\"", "5\nok\n");
}

static void test_bytes_from_byte(void) {
  install_base();
  eval_expect("bytes.length: bytes.from-byte: 10", "1\nok\n");
  eval_ok("to fb-test [ here b is bytes.from-byte: 65; bytes.at: b, 0 ]");
  eval_expect("fb-test:", "65\nok\n");
}

static void test_bytes_from_int(void) {
  install_base();
  eval_expect("text.pack: bytes.from-int: 42", "\"42\"\nok\n");
}

static void test_bytes_length(void) {
  install_base();
  eval_expect("bytes.length: bytes.from-text: \"abc\"", "3\nok\n");
  eval_error_expect("bytes.length: 5", FR_ERR_TYPE,
                    "error: wrong type: 5 (2)\n"
                    "bytes.length argument 1 expects bytes, got an int\n");
}

static void test_bytes_at(void) {
  install_base();
  eval_ok("to at-test [ here b is bytes.from-text: \"A\"; bytes.at: b, 0 ]");
  eval_expect("at-test:", "65\nok\n");
  eval_ok("to at-oob [ here b is bytes.from-text: \"A\"; bytes.at: b, 1 ]");
  eval_error_expect("at-oob:", FR_ERR_RANGE,
                    "error: out of range: 1 (1)\n"
                    "detail: bytes.at argument 2 was rejected\n");
}

static void test_bytes_equals_p(void) {
  install_base();
  eval_ok("to eq-test [ here a is bytes.from-text: \"hi\";"
          " here b is bytes.from-text: \"hi\"; bytes.equals?: a, b ]");
  eval_expect("eq-test:", "true\nok\n");
  eval_ok("to neq-test [ here a is bytes.from-text: \"hi\";"
          " here b is bytes.from-text: \"ho\"; bytes.equals?: a, b ]");
  eval_expect("neq-test:", "false\nok\n");
}

static void test_bytes_concat(void) {
  install_base();
  eval_ok("to cat-test [ here a is bytes.from-text: \"ab\";"
          " here b is bytes.from-text: \"cd\";"
          " text.pack: bytes.concat: a, b ]");
  eval_expect("cat-test:", "\"abcd\"\nok\n");
}

/* --- text.pack happy + capacity --- */

static void test_text_pack_happy(void) {
  install_base();
  eval_expect("text.pack: bytes.from-text: \"hello\"", "\"hello\"\nok\n");
}

static void test_text_pack_capacity(void) {
  fr_object_id_t oid;
  uint8_t buf[3];

  install_base();
  for (int i = 0; i < FR_PROFILE_OBJECT_TABLE_SIZE + 10; i++) {
    buf[0] = (uint8_t)(0x80 + (i & 0x3F));
    buf[1] = (uint8_t)(0xC0 + ((i >> 6) & 0x3F));
    buf[2] = (uint8_t)(i & 0xFF);
    if (fr_text_install(&s_runtime, buf, 3, &oid) != FR_OK) {
      break;
    }
  }
  TEST_ASSERT_EQUAL(FR_ERR_CAPACITY,
                    eval_err("text.pack: bytes.from-byte: 254"));
}

/* --- slot reject --- */

static void test_slot_rejects_bytes(void) {
  install_base();
  TEST_ASSERT_EQUAL(FR_ERR_VOLATILE,
                    eval_err("x is bytes.from-text: \"hi\""));
}

/* --- cell + record reject --- */

static void test_cell_rejects_bytes(void) {
  install_base();
  eval_ok("c is cells(1)");
  TEST_ASSERT_EQUAL(FR_ERR_VOLATILE,
                    eval_err("set c[0] to bytes.from-text: \"hi\""));
}

static void test_record_field_rejects_bytes(void) {
  const uint8_t data[2] = {'h', 'i'};
  fr_tagged_t tagged = 0;

  install_base();
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_bytes_install(&s_runtime, data, 2, &tagged));
  TEST_ASSERT_FALSE(fr_record_field_value_allowed(&s_runtime, tagged));
}

/* --- generation bump --- */

static void test_generation_bump_invalidates_old_ref(void) {
  const uint8_t data[2] = {'h', 'i'};
  fr_tagged_t tagged = 0;
  fr_bytes_ref_t ref = {0};
  const uint8_t *view = NULL;
  uint16_t view_len = 0;

  install_base();
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_bytes_install(&s_runtime, data, 2, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_bytes_ref(tagged, &ref));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_bytes_view(&s_runtime, ref, &view, &view_len));
  TEST_ASSERT_EQUAL_UINT16(2, view_len);

  fr_bytes_reset_if_outermost(&s_runtime);

  TEST_ASSERT_EQUAL(FR_ERR_VOLATILE,
                    fr_bytes_view(&s_runtime, ref, &view, &view_len));
}

/* --- generation exhaust --- */

static void test_generation_exhaust_retires_entry(void) {
  const uint8_t data[1] = {'x'};
  fr_tagged_t tagged = 0;
  fr_bytes_ref_t ref = {0};

  install_base();
  for (int i = 0; i < 128; i++) {
    TEST_ASSERT_EQUAL(FR_OK,
                      fr_bytes_install(&s_runtime, data, 1, &tagged));
    fr_bytes_reset_if_outermost(&s_runtime);
  }
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_bytes_install(&s_runtime, data, 1, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_bytes_ref(tagged, &ref));
  TEST_ASSERT_EQUAL_UINT8(1, ref.id);
}

/* --- loop reset --- */

static void test_loop_reset_prevents_arena_exhaustion(void) {
  install_base();
  eval_ok("repeat 20 [ bytes.from-text: \"hello world test\" ]");
}

/* --- event-depth suppression --- */

#if FR_FEATURE_NET
static void test_event_depth_suppresses_reset(void) {
  install_base();
  eval_ok("counter is cells(1)");
  eval_ok("set counter[0] to 0");
  eval_ok("handler is fn [ "
          "here b is bytes.from-text: \"abc\"; "
          "repeat 2 [ 0 ]; "
          "set counter[0] to bytes.length: b ]");
  eval_ok("boot is fn [ on wifi.disconnected [ handler: ] ]");
  eval_ok("boot:");
  fr_host_wifi_fire_event(FR_EVENT_KIND_WIFI_DISCONNECTED);
  eval_ok("0");
  eval_expect("counter[0]", "3\nok\n");
}
#endif

#endif /* FR_FEATURE_BYTES */

int main(void) {
  UNITY_BEGIN();
#if FR_FEATURE_BYTES
  RUN_TEST(test_bytes_from_text);
  RUN_TEST(test_bytes_from_byte);
  RUN_TEST(test_bytes_from_int);
  RUN_TEST(test_bytes_length);
  RUN_TEST(test_bytes_at);
  RUN_TEST(test_bytes_equals_p);
  RUN_TEST(test_bytes_concat);
  RUN_TEST(test_text_pack_happy);
  RUN_TEST(test_text_pack_capacity);
  RUN_TEST(test_slot_rejects_bytes);
  RUN_TEST(test_cell_rejects_bytes);
  RUN_TEST(test_record_field_rejects_bytes);
  RUN_TEST(test_generation_bump_invalidates_old_ref);
  RUN_TEST(test_generation_exhaust_retires_entry);
  RUN_TEST(test_loop_reset_prevents_arena_exhaustion);
#if FR_FEATURE_NET
  RUN_TEST(test_event_depth_suppresses_reset);
#endif
#endif
  return UNITY_END();
}
