/*
 * Unity coverage for the T21 `mem` REPL command.
 *
 * Asserts exact host heap zeros, per-subset filtering, the `ok` terminator,
 * the two FR_ERR_DOMAIN error paths, and that slots.used moves when a user
 * slot is installed.
 */

#include "base_image.h"
#include "config.h"
#include "repl.h"
#include "runtime.h"

#include "unity/unity.h"

#include <stdint.h>
#include <string.h>

static fr_runtime_t s_runtime;

void setUp(void) {
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
}

void tearDown(void) {}

static void assert_ends_with_ok(const char *out) {
  size_t len = strlen(out);
  TEST_ASSERT_TRUE(len >= 3);
  TEST_ASSERT_EQUAL_STRING("ok\n", out + len - 3);
}

static uint32_t value_after_key(const char *out, const char *key) {
  const char *p = strstr(out, key);
  uint32_t v = 0;

  TEST_ASSERT_NOT_NULL(p);
  p += strlen(key);
  while (*p >= '0' && *p <= '9') {
    v = v * 10u + (uint32_t)(*p - '0');
    p++;
  }
  return v;
}

static void test_mem_heap_emits_zero_pair_on_host(void) {
  char out[128] = {0};

  TEST_ASSERT_EQUAL(FR_OK, fr_repl_eval_line(&s_runtime, "mem heap", out,
                                             (uint16_t)sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("heap.free 0\nheap.largest 0\nok\n", out);
}

static void test_mem_bare_emits_subsets_in_spec_order(void) {
  char out[1024] = {0};
  const char *heap_free = NULL;
  const char *heap_largest = NULL;
  const char *slots_used = NULL;
  const char *slots_total = NULL;
  const char *objects_entries_used = NULL;
  const char *events_used = NULL;
  const char *events_total = NULL;
  const char *ok_term = NULL;

  TEST_ASSERT_EQUAL(FR_OK, fr_repl_eval_line(&s_runtime, "mem", out,
                                             (uint16_t)sizeof(out)));
  heap_free = strstr(out, "heap.free 0\n");
  heap_largest = strstr(out, "heap.largest 0\n");
  slots_used = strstr(out, "slots.used ");
  slots_total = strstr(out, "slots.total ");
  objects_entries_used = strstr(out, "objects.entries.used ");
  events_used = strstr(out, "events.used ");
  events_total = strstr(out, "events.total ");
  ok_term = strstr(out, "\nok\n");

  TEST_ASSERT_NOT_NULL(heap_free);
  TEST_ASSERT_NOT_NULL(heap_largest);
  TEST_ASSERT_NOT_NULL(slots_used);
  TEST_ASSERT_NOT_NULL(slots_total);
  TEST_ASSERT_NOT_NULL(objects_entries_used);
  TEST_ASSERT_NOT_NULL(events_used);
  TEST_ASSERT_NOT_NULL(events_total);
  TEST_ASSERT_NOT_NULL(ok_term);
  TEST_ASSERT_TRUE(heap_free < heap_largest);
  TEST_ASSERT_TRUE(heap_largest < slots_used);
  TEST_ASSERT_TRUE(slots_used < slots_total);
  TEST_ASSERT_TRUE(slots_total < objects_entries_used);
  TEST_ASSERT_TRUE(objects_entries_used < events_used);
  TEST_ASSERT_TRUE(events_used < events_total);
  TEST_ASSERT_TRUE(events_total < ok_term);
  assert_ends_with_ok(out);
}

static void test_mem_slots_emits_only_slot_pair(void) {
  char out[128] = {0};

  TEST_ASSERT_EQUAL(FR_OK, fr_repl_eval_line(&s_runtime, "mem slots", out,
                                             (uint16_t)sizeof(out)));
  TEST_ASSERT_NULL(strstr(out, "heap."));
  TEST_ASSERT_NULL(strstr(out, "objects."));
  TEST_ASSERT_NULL(strstr(out, "events."));
  TEST_ASSERT_NOT_NULL(strstr(out, "slots.used "));
  TEST_ASSERT_NOT_NULL(strstr(out, "slots.total "));
  assert_ends_with_ok(out);
}

static void test_mem_objects_emits_object_keys(void) {
  char out[1024] = {0};

  TEST_ASSERT_EQUAL(FR_OK, fr_repl_eval_line(&s_runtime, "mem objects", out,
                                             (uint16_t)sizeof(out)));
  TEST_ASSERT_NULL(strstr(out, "heap."));
  TEST_ASSERT_NULL(strstr(out, "slots."));
  TEST_ASSERT_NULL(strstr(out, "events."));
  TEST_ASSERT_NOT_NULL(strstr(out, "objects.entries.used "));
  TEST_ASSERT_NOT_NULL(strstr(out, "objects.entries.total "));
  TEST_ASSERT_NOT_NULL(strstr(out, "objects.cells.used "));
  TEST_ASSERT_NOT_NULL(strstr(out, "objects.cells.total "));
  TEST_ASSERT_NOT_NULL(strstr(out, "objects.text.used "));
  TEST_ASSERT_NOT_NULL(strstr(out, "objects.text.total "));
#if FR_FEATURE_RECORDS
  TEST_ASSERT_NOT_NULL(strstr(out, "objects.record_names.used "));
  TEST_ASSERT_NOT_NULL(strstr(out, "objects.record_names.total "));
  TEST_ASSERT_NOT_NULL(strstr(out, "objects.record_shape_fields.used "));
  TEST_ASSERT_NOT_NULL(strstr(out, "objects.record_shape_fields.total "));
  TEST_ASSERT_NOT_NULL(strstr(out, "objects.record_values.used "));
  TEST_ASSERT_NOT_NULL(strstr(out, "objects.record_values.total "));
  TEST_ASSERT_NOT_NULL(strstr(out, "objects.record_name_bytes.used "));
  TEST_ASSERT_NOT_NULL(strstr(out, "objects.record_name_bytes.total "));
#else
  TEST_ASSERT_NULL(strstr(out, "objects.record_"));
#endif
  assert_ends_with_ok(out);
}

static void test_mem_events_emits_event_pair(void) {
  char out[128] = {0};

  TEST_ASSERT_EQUAL(FR_OK, fr_repl_eval_line(&s_runtime, "mem events", out,
                                             (uint16_t)sizeof(out)));
  TEST_ASSERT_NULL(strstr(out, "heap."));
  TEST_ASSERT_NULL(strstr(out, "slots."));
  TEST_ASSERT_NULL(strstr(out, "objects."));
  TEST_ASSERT_NOT_NULL(strstr(out, "events.used "));
  TEST_ASSERT_NOT_NULL(strstr(out, "events.total "));
  assert_ends_with_ok(out);
}

static void test_mem_unknown_topic_returns_domain(void) {
  char out[128] = {0};

  TEST_ASSERT_EQUAL(FR_ERR_DOMAIN,
                    fr_repl_eval_line(&s_runtime, "mem foo", out,
                                      (uint16_t)sizeof(out)));
}

static void test_mem_extra_token_returns_domain(void) {
  char out[128] = {0};

  TEST_ASSERT_EQUAL(FR_ERR_DOMAIN,
                    fr_repl_eval_line(&s_runtime, "mem slots extra", out,
                                      (uint16_t)sizeof(out)));
}

static void test_mem_slots_used_advances_after_install(void) {
  char before[128] = {0};
  char after[128] = {0};
  char eval[64] = {0};
  uint32_t used_before = 0;
  uint32_t used_after = 0;

  TEST_ASSERT_EQUAL(FR_OK, fr_repl_eval_line(&s_runtime, "mem slots", before,
                                             (uint16_t)sizeof(before)));
  used_before = value_after_key(before, "slots.used ");
  TEST_ASSERT_EQUAL(FR_OK, fr_repl_eval_line(&s_runtime, "x is 5", eval,
                                             (uint16_t)sizeof(eval)));
  TEST_ASSERT_EQUAL(FR_OK, fr_repl_eval_line(&s_runtime, "mem slots", after,
                                             (uint16_t)sizeof(after)));
  used_after = value_after_key(after, "slots.used ");
  TEST_ASSERT_EQUAL_UINT32(used_before + 1u, used_after);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_mem_heap_emits_zero_pair_on_host);
  RUN_TEST(test_mem_bare_emits_subsets_in_spec_order);
  RUN_TEST(test_mem_slots_emits_only_slot_pair);
  RUN_TEST(test_mem_objects_emits_object_keys);
  RUN_TEST(test_mem_events_emits_event_pair);
  RUN_TEST(test_mem_unknown_topic_returns_domain);
  RUN_TEST(test_mem_extra_token_returns_domain);
  RUN_TEST(test_mem_slots_used_advances_after_install);
  return UNITY_END();
}
