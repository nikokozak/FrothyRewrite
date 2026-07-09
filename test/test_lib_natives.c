/*
 * Unity test for the library-native install path.
 *
 * Strong-overrides the weak fr_lib_natives table in src/lib_native.c with two
 * fake entries, then asserts the names resolve at the slot table both before
 * AND after fr_runtime_reset. The R5 review found that overlay-table-bound
 * names disappear on reset; this test pins the fix in place.
 */

#include "base_image.h"
#include "lib_native.h"
#include "profile.h"
#include "repl.h"
#include "runtime.h"
#include "slot.h"

#include "unity/unity.h"

#include <string.h>

static fr_runtime_t s_runtime;

static fr_err_t lib_native_noop(fr_runtime_t *runtime, const fr_tagged_t *args,
                                uint8_t arg_count, fr_tagged_t *out) {
  (void)runtime;
  (void)args;
  (void)arg_count;
  *out = fr_tagged_nil();
  return FR_OK;
}

/* One entry — the host profile's native table is sized to base + room for one
   more, and the bug under test (name lifetime across reset) needs only one. */
const fr_lib_native_def_t fr_lib_natives[] = {
    {"libtest.one", lib_native_noop, 0},
};
const uint16_t fr_lib_natives_count = 1;

void setUp(void) {}
void tearDown(void) {}

static void test_name_resolves_after_install(void) {
  fr_slot_id_t slot_id = 0;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "libtest.one", &slot_id));
  TEST_ASSERT_EQUAL_STRING("libtest.one", fr_slot_name(&s_runtime, slot_id));
}

/* The bug from R5 review: names stored in runtime->slots.overlay_names[] get
   cleared by fr_runtime_clear_project. Library-native names must survive. */
static void test_names_survive_runtime_reset(void) {
  fr_slot_id_t before = 0;
  fr_slot_id_t after = 0;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "libtest.one", &before));

  TEST_ASSERT_EQUAL(FR_OK, fr_runtime_reset(&s_runtime));

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "libtest.one", &after));
  TEST_ASSERT_EQUAL(before, after);
  TEST_ASSERT_EQUAL_STRING("libtest.one", fr_slot_name(&s_runtime, after));
}

static void assert_words_include_libtest_one(void) {
  char out[2048];

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "words", out, sizeof(out)));
  TEST_ASSERT_NOT_NULL(strstr(out, "libtest.one"));
}

static void test_words_list_lib_native_after_runtime_reset(void) {
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  assert_words_include_libtest_one();

  TEST_ASSERT_EQUAL(FR_OK, fr_runtime_reset(&s_runtime));
  assert_words_include_libtest_one();
}

/* R8 review: a second fr_base_image_install in the same process must allocate
   the same lib-native slot as the first. Without the records-reset-before-
   source-base fix, source-base words on the second install see stale lib
   records and land past them, drifting "libtest.one"'s slot. */
static void test_repeated_install_same_slot(void) {
  fr_slot_id_t first = 0;
  fr_slot_id_t second = 0;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "libtest.one", &first));

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_slot_id_for_name(&s_runtime, "libtest.one", &second));

  TEST_ASSERT_EQUAL(first, second);
}

static void test_profile_hash_tracks_lib_native_table(void) {
  const fr_lib_native_def_t base[] = {
      {"alpha", lib_native_noop, 1},
      {"beta", lib_native_noop, 2},
  };
  const fr_lib_native_def_t same[] = {
      {"alpha", lib_native_noop, 1},
      {"beta", lib_native_noop, 2},
  };
  const fr_lib_native_def_t renamed[] = {
      {"alpha", lib_native_noop, 1},
      {"gamma", lib_native_noop, 2},
  };
  const fr_lib_native_def_t arity_changed[] = {
      {"alpha", lib_native_noop, 1},
      {"beta", lib_native_noop, 3},
  };
  const fr_lib_native_def_t reordered[] = {
      {"beta", lib_native_noop, 2},
      {"alpha", lib_native_noop, 1},
  };
  /* Isolates the name terminator: without it, 0x63 == 'c' makes both tables
     hash as a,b,c,c,1. */
  const fr_lib_native_def_t boundary_left[] = {
      {"ab", lib_native_noop, 0x63},
      {"c", lib_native_noop, 1},
  };
  const fr_lib_native_def_t boundary_right[] = {
      {"abc", lib_native_noop, 0x63},
      {"", lib_native_noop, 1},
  };
  const uint32_t base_hash =
      fr_profile_debug_hash_for_lib_natives(base, 2);

  TEST_ASSERT_EQUAL_HEX32(base_hash,
                          fr_profile_debug_hash_for_lib_natives(same, 2));
  TEST_ASSERT_NOT_EQUAL_HEX32(
      base_hash, fr_profile_debug_hash_for_lib_natives(renamed, 2));
  TEST_ASSERT_NOT_EQUAL_HEX32(
      base_hash, fr_profile_debug_hash_for_lib_natives(arity_changed, 2));
  TEST_ASSERT_NOT_EQUAL_HEX32(
      base_hash, fr_profile_debug_hash_for_lib_natives(reordered, 2));
  TEST_ASSERT_NOT_EQUAL_HEX32(base_hash,
                              fr_profile_debug_hash_for_lib_natives(base, 1));
  TEST_ASSERT_NOT_EQUAL_HEX32(
      fr_profile_debug_hash_for_lib_natives(boundary_left, 2),
      fr_profile_debug_hash_for_lib_natives(boundary_right, 2));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_name_resolves_after_install);
  RUN_TEST(test_names_survive_runtime_reset);
  RUN_TEST(test_words_list_lib_native_after_runtime_reset);
  RUN_TEST(test_repeated_install_same_slot);
  RUN_TEST(test_profile_hash_tracks_lib_native_table);
  return UNITY_END();
}
