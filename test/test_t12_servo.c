/*
 * Unity tests for T12-servo.
 *
 * Round 2 adds the host PWM stub regression: pwm.write must record every duty
 * value in a per-handle ring; fr_host_pwm_drain_writes must return them in
 * FIFO order. A reversion to the old single-duty stub would only surface the
 * last write and fail these tests.
 *
 * Slice C will extend this file with servo-library and end-to-end coverage.
 */

#include "base_image.h"
#include "platform.h"
#include "repl.h"
#include "runtime.h"

#include "unity/unity.h"

#include <stdint.h>

void setUp(void) {
#if FR_FEATURE_PWM
  fr_platform_pwm_close(0);
#endif
}

void tearDown(void) {}

#if FR_FEATURE_PWM

static fr_runtime_t s_runtime;

static void open_pwm(void) {
  char out[64];
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime,
                                      "h is pwm.open: 4, 50",
                                      out, sizeof(out)));
}

static void test_host_stub_records_writes_in_fifo(void) {
  char out[64];
  uint16_t duties[8] = {0};

  open_pwm();
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "pwm.write: h, 250",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "pwm.write: h, 750",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "pwm.write: h, 1250",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT16(3u, fr_host_pwm_drain_writes(0, duties,
                                                        sizeof(duties) /
                                                            sizeof(duties[0])));
  TEST_ASSERT_EQUAL_UINT16(250u, duties[0]);
  TEST_ASSERT_EQUAL_UINT16(750u, duties[1]);
  TEST_ASSERT_EQUAL_UINT16(1250u, duties[2]);
}

static void test_host_stub_drain_empties_ring(void) {
  char out[64];
  uint16_t duties[4] = {0};

  open_pwm();
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "pwm.write: h, 500",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT16(1u, fr_host_pwm_drain_writes(0, duties,
                                                        sizeof(duties) /
                                                            sizeof(duties[0])));
  TEST_ASSERT_EQUAL_UINT16(500u, duties[0]);
  TEST_ASSERT_EQUAL_UINT16(0u, fr_host_pwm_drain_writes(0, duties,
                                                        sizeof(duties) /
                                                            sizeof(duties[0])));
}

#endif /* FR_FEATURE_PWM */

int main(void) {
  UNITY_BEGIN();
#if FR_FEATURE_PWM
  RUN_TEST(test_host_stub_records_writes_in_fifo);
  RUN_TEST(test_host_stub_drain_empties_ring);
#endif
  return UNITY_END();
}
