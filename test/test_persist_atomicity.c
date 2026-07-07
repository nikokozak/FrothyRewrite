/*
 * Unity persistence durability tests.
 *
 * S1: the core commits one complete envelope, and platform durability must
 * return the newest good image while falling back to an older good image when
 * the newer one is torn or corrupt.
 *
 * T10c: `see <function>` on a function with a text literal must render the
 * canonical `argN` form after a save/restore round-trip (text feature only).
 */

#include "base_image.h"
#include "crc.h"
#include "persist.h"
#include "platform.h"
#include "repl.h"
#include "runtime.h"
#include "tagged.h"
#include "vm.h"

#include "unity/unity.h"

#include <stdint.h>

static fr_runtime_t s_runtime;

static void save_boot_one_after_nil_save(void) {
  char buf[128];

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_save(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "boot is fn [ one ]", buf,
                                      (uint16_t)sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("ok\n", buf);
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_save(&s_runtime));
}

static void assert_restored_boot_is_one(void) {
  fr_runtime_t restore_runtime;
  fr_tagged_t tagged = 0;
  fr_int_t decoded = 0;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&restore_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore(&restore_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_vm_run_boot(&restore_runtime, &tagged));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(tagged, &decoded));
  TEST_ASSERT_EQUAL(1, decoded);
}

static void assert_restored_boot_is_nil(void) {
  fr_runtime_t restore_runtime;
  fr_tagged_t tagged = 0;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&restore_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore(&restore_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_vm_run_boot(&restore_runtime, &tagged));
  TEST_ASSERT_TRUE(fr_tagged_is_nil(tagged));
}

void setUp(void) { TEST_ASSERT_EQUAL(FR_OK, fr_platform_persist_clear()); }

void tearDown(void) {}

static void test_read_returns_newest_good_image(void) {
  save_boot_one_after_nil_save();
  assert_restored_boot_is_one();
}

static void test_mid_commit_power_loss_keeps_previous_good_image(void) {
  uint8_t image[FR_PROFILE_PERSISTENCE_BYTES];
  uint16_t image_length = 0;

  save_boot_one_after_nil_save();
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_persist_read(
                               image, (uint16_t)sizeof(image), &image_length,
                               0));
  TEST_ASSERT_GREATER_THAN(FR_PERSIST_HEADER_BYTES + 1u, image_length);

  fr_host_persist_debug_interrupt_next_commit(
      (uint16_t)(FR_PERSIST_HEADER_BYTES + 1u));
  TEST_ASSERT_EQUAL(FR_ERR_IO, fr_platform_persist_commit(image, image_length));

  assert_restored_boot_is_one();
}

static void test_bad_payload_newer_image_falls_back_to_older_good_image(void) {
  uint8_t image[FR_PROFILE_PERSISTENCE_BYTES];
  uint16_t image_length = 0;
  fr_persist_format_info_t info = {0};
  const uint16_t version_offset = (uint16_t)(FR_PERSIST_HEADER_BYTES + 4u);

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_save(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_persist_read(
                               image, (uint16_t)sizeof(image), &image_length,
                               0));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_persist_format_validate(image, image_length, &info));
  TEST_ASSERT_GREATER_THAN(version_offset, image_length);

  image[version_offset] ^= 0x5au;
  TEST_ASSERT_EQUAL(
      FR_OK, fr_persist_format_build_header(
                 image, info.payload_length,
                 fr_crc32(&image[FR_PERSIST_HEADER_BYTES],
                          info.payload_length)));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_persist_commit(image, info.total_length));

  assert_restored_boot_is_nil();
}

static void test_corrupt_newer_image_falls_back_to_older_good_image(void) {
  uint8_t image[FR_PROFILE_PERSISTENCE_BYTES];
  uint16_t image_length = 0;
  uint16_t offset = (uint16_t)(FR_PERSIST_HEADER_BYTES + 5u);

  save_boot_one_after_nil_save();
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_persist_read(
                               image, (uint16_t)sizeof(image), &image_length,
                               0));
  TEST_ASSERT_GREATER_THAN(offset, image_length);

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_host_persist_debug_corrupt_newest(
                        offset, (uint8_t)(image[offset] ^ 0x5au)));

  assert_restored_boot_is_nil();
}

static void test_clear_reports_not_found(void) {
  fr_runtime_t restore_runtime;

  save_boot_one_after_nil_save();
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_persist_clear());

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&restore_runtime));
  TEST_ASSERT_EQUAL(FR_ERR_NOT_FOUND, fr_persist_restore(&restore_runtime));
}

#if FR_FEATURE_TEXT
/* T10c: restore drops stored param names, so the renderer must fall back to
 * canonical argN display. The full REPL path catches both the renderer
 * result and the "source reconstruction unavailable" fallback line. */
static void test_see_after_restore_renders_canonical_arg_names(void) {
  fr_runtime_t runtime;
  char buf[128];

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&runtime));
  TEST_ASSERT_EQUAL(
      FR_OK, fr_repl_eval_line(
                 &runtime,
                 "to greet with x [ text.concat: \"led=\", text.from-int: x ]",
                 buf, sizeof(buf)));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_save(&runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore(&runtime));
  TEST_ASSERT_EQUAL(
      FR_OK, fr_repl_eval_line(&runtime, "see greet", buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING(
      "overlay code\n"
      "to greet with arg0 [ text.concat: \"led=\", text.from-int: arg0 ]\n"
      "ok\n",
      buf);
}
#endif

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_read_returns_newest_good_image);
  RUN_TEST(test_mid_commit_power_loss_keeps_previous_good_image);
  RUN_TEST(test_bad_payload_newer_image_falls_back_to_older_good_image);
  RUN_TEST(test_corrupt_newer_image_falls_back_to_older_good_image);
  RUN_TEST(test_clear_reports_not_found);
#if FR_FEATURE_TEXT
  RUN_TEST(test_see_after_restore_renders_canonical_arg_names);
#endif
  return UNITY_END();
}
