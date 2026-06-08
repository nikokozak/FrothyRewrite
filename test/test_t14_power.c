/*
 * Unity tests for T14 watchdog + sleep natives.
 *
 * Drives the four power natives through fr_repl_eval_line, covering D19:
 * arm happy path, arm range clamp (D9), feed-when-not-armed (D11), re-arm
 * (D10), force_timeout's WDT-fire simulation (D17), sleep.deep ms capture,
 * sleep.wake-on-gpio pin/level capture, non-RTC pin reject (D12), and
 * sleep.deep 0 with no wake pending (D12). Absence on FR_FEATURE_POWER=0
 * profiles is compile-time: the FR_FEATURE_POWER gate around every native
 * keeps them out of the host (POWER=0) test binary.
 */

#include "base_image.h"
#include "config.h"
#include "platform.h"
#include "repl.h"
#include "runtime.h"
#include "types.h"

#include "unity/unity.h"

#include <stdint.h>

#if FR_FEATURE_POWER
static fr_runtime_t s_runtime;
#endif

void setUp(void) {
#if FR_FEATURE_POWER
  fr_host_watchdog_force_timeout();
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
#endif
}

void tearDown(void) {}

#if FR_FEATURE_POWER

static void eval_ok(const char *line) {
  char out[64];
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, line, out, sizeof(out)));
}

static fr_err_t eval_err(const char *line) {
  char out[64];
  return fr_repl_eval_line(&s_runtime, line, out, sizeof(out));
}

static void test_watchdog_arm_then_feed_ok(void) {
  eval_ok("watchdog.arm: 1000");
  eval_ok("watchdog.feed:");
  eval_ok("watchdog.feed:");
}

static void test_watchdog_arm_below_min_invalid(void) {
  TEST_ASSERT_EQUAL(FR_ERR_INVALID, eval_err("watchdog.arm: 999"));
}

static void test_watchdog_arm_above_max_invalid(void) {
  TEST_ASSERT_EQUAL(FR_ERR_INVALID, eval_err("watchdog.arm: 60001"));
}

static void test_watchdog_feed_when_not_armed_invalid(void) {
  TEST_ASSERT_EQUAL(FR_ERR_INVALID, eval_err("watchdog.feed:"));
}

static void test_watchdog_rearm_replaces(void) {
  eval_ok("watchdog.arm: 1000");
  eval_ok("watchdog.arm: 60000");
  eval_ok("watchdog.feed:");
}

static void test_watchdog_force_timeout_clears_armed(void) {
  eval_ok("watchdog.arm: 5000");
  eval_ok("watchdog.feed:");
  fr_host_watchdog_force_timeout();
  TEST_ASSERT_EQUAL(FR_ERR_INVALID, eval_err("watchdog.feed:"));
}

static void test_sleep_deep_zero_no_wake_invalid(void) {
  TEST_ASSERT_EQUAL(FR_ERR_INVALID, eval_err("sleep.deep: 0"));
}

static void test_sleep_wake_on_gpio_non_rtc_invalid(void) {
  TEST_ASSERT_EQUAL(FR_ERR_INVALID,
                    eval_err("sleep.wake-on-gpio: 5, 0"));
}

static void test_sleep_wake_on_gpio_bad_level_invalid(void) {
  TEST_ASSERT_EQUAL(FR_ERR_INVALID,
                    eval_err("sleep.wake-on-gpio: 0, 2"));
}

static void test_sleep_deep_records_ms(void) {
  uint32_t ms = 0;
  uint16_t pin = 0xFFFFu;
  uint16_t level = 0xFFFFu;

  eval_ok("sleep.deep: 1500");
  fr_host_sleep_deep_captures(&ms, &pin, &level);
  TEST_ASSERT_EQUAL_UINT32(1500u, ms);
}

static void test_sleep_wake_on_gpio_records_then_consumed(void) {
  uint32_t ms = 0;
  uint16_t pin = 0xFFFFu;
  uint16_t level = 0xFFFFu;

  eval_ok("sleep.wake-on-gpio: 25, 1");
  eval_ok("sleep.deep: 2000");
  fr_host_sleep_deep_captures(&ms, &pin, &level);
  TEST_ASSERT_EQUAL_UINT32(2000u, ms);
  TEST_ASSERT_EQUAL_UINT16(25u, pin);
  TEST_ASSERT_EQUAL_UINT16(1u, level);

  /* D12: after the sleep, pending is consumed — the next sleep.deep: 0
   * with no fresh wake-on-gpio: surfaces FR_ERR_INVALID. */
  TEST_ASSERT_EQUAL(FR_ERR_INVALID, eval_err("sleep.deep: 0"));
}

#endif

int main(void) {
  UNITY_BEGIN();
#if FR_FEATURE_POWER
  RUN_TEST(test_watchdog_feed_when_not_armed_invalid);
  RUN_TEST(test_watchdog_arm_below_min_invalid);
  RUN_TEST(test_watchdog_arm_above_max_invalid);
  RUN_TEST(test_watchdog_arm_then_feed_ok);
  RUN_TEST(test_watchdog_rearm_replaces);
  RUN_TEST(test_watchdog_force_timeout_clears_armed);
  RUN_TEST(test_sleep_deep_zero_no_wake_invalid);
  RUN_TEST(test_sleep_wake_on_gpio_non_rtc_invalid);
  RUN_TEST(test_sleep_wake_on_gpio_bad_level_invalid);
  RUN_TEST(test_sleep_deep_records_ms);
  RUN_TEST(test_sleep_wake_on_gpio_records_then_consumed);
#endif
  return UNITY_END();
}
