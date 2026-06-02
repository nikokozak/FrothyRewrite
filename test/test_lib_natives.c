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
#include "runtime.h"
#include "slot.h"

#include "unity/unity.h"

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

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_name_resolves_after_install);
  RUN_TEST(test_names_survive_runtime_reset);
  RUN_TEST(test_repeated_install_same_slot);
  return UNITY_END();
}
