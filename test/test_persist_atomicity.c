/*
 * Unity beachhead test. Proves ADR 0042's central claim:
 * if a save is interrupted before the new slot's header lands, the previous
 * good save in the other slot stays restorable. No new mocking layer — the
 * truncation is just direct calls into the existing platform storage API
 * that the save sequence itself would use.
 */

#include "base_image.h"
#include "persist.h"
#include "platform.h"
#include "runtime.h"

#include "unity/unity.h"

#include <stdint.h>

static fr_runtime_t s_runtime;

static void prime_two_good_slots(void) {
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_save(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_save(&s_runtime));
}

void setUp(void) { fr_platform_storage_debug_reset(); }

void tearDown(void) {}

/* Save sequence into the inactive slot is: erase, write payload, write header.
 * Power dies right after erase. The other slot, holding the previous good
 * save, must still restore. */
static void test_truncated_after_erase_falls_back(void) {
  fr_runtime_t restore_runtime;

  prime_two_good_slots();
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_storage_erase(0));

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&restore_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore(&restore_runtime));
}

/* Same flow, but the truncation lands later: erase succeeded, payload bytes
 * were partially written, header never made it. Restore must still return
 * the previous good save from the other slot. */
static void test_truncated_after_partial_payload_falls_back(void) {
  fr_runtime_t restore_runtime;
  uint8_t torn_payload[8] = {0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5};

  prime_two_good_slots();
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_storage_erase(0));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_storage_write(0, FR_PERSIST_HEADER_BYTES,
                                              torn_payload,
                                              (uint16_t)sizeof(torn_payload)));

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&restore_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_persist_restore(&restore_runtime));
}

/* Sanity: with both slots wiped, restore reports no save and resets. The
 * other two tests' FR_OK is therefore evidence that a slot's previous good
 * save carried the restore, not a silent fall-through. */
static void test_both_slots_wiped_reports_not_found(void) {
  fr_runtime_t restore_runtime;

  prime_two_good_slots();
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_storage_erase(0));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_storage_erase(1));

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&restore_runtime));
  TEST_ASSERT_EQUAL(FR_ERR_NOT_FOUND, fr_persist_restore(&restore_runtime));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_truncated_after_erase_falls_back);
  RUN_TEST(test_truncated_after_partial_payload_falls_back);
  RUN_TEST(test_both_slots_wiped_reports_not_found);
  return UNITY_END();
}
