/*
 * Unity register-helper tests for T12i.
 *
 * Exercises the four register natives through fr_repl_eval_line against the
 * host stub. Read tests pre-queue a response via fr_host_i2c_queue_read; write
 * tests drain the host write ring via fr_host_i2c_drain_writes and assert the
 * exact wire bytes. Domain-error tests cover addr > 127, reg > 0xFF, and the
 * write-byte value range.
 */

#include "base_image.h"
#include "platform.h"
#include "repl.h"
#include "runtime.h"

#include "unity/unity.h"

#include <stdint.h>

void setUp(void) {
#if FR_FEATURE_I2C
  /* Each test reopens the bus at platform_index 0; a prior assertion failure
   * may have left it allocated. close is a memset on the host entry, so
   * calling it on an idle slot is harmless. */
  fr_platform_i2c_close(0);
#endif
}

void tearDown(void) {}

#if FR_FEATURE_I2C

static fr_runtime_t s_runtime;

static void open_bus(void) {
  char out[64];
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime,
                                      "bus is i2c.open: 0, 21, 22, 100000",
                                      out, sizeof(out)));
}

static void test_read_reg_returns_queued_byte_and_writes_register(void) {
  char out[64];
  uint8_t writes[8];
  uint8_t resp[1] = {0x60};

  open_bus();
  fr_host_i2c_queue_read(0, resp, 1);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime,
                                      "i2c.read-reg: bus, 0x76, 0xD0",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("96\nok\n", out);
  TEST_ASSERT_EQUAL_UINT16(1u, fr_host_i2c_drain_writes(0, writes,
                                                        sizeof(writes)));
  TEST_ASSERT_EQUAL_UINT8(0xD0, writes[0]);
}

/* D3 — read-reg16 clocks MSB first; 0x12, 0x34 → 0x1234 = 4660. */
static void test_read_reg16_assembles_msb_first(void) {
  char out[64];
  uint8_t writes[8];
  uint8_t resp[2] = {0x12, 0x34};

  open_bus();
  fr_host_i2c_queue_read(0, resp, 2);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime,
                                      "i2c.read-reg16: bus, 0x68, 0x3B",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("4660\nok\n", out);
  TEST_ASSERT_EQUAL_UINT16(1u, fr_host_i2c_drain_writes(0, writes,
                                                        sizeof(writes)));
  TEST_ASSERT_EQUAL_UINT8(0x3B, writes[0]);
}

static void test_write_reg_emits_two_bytes_in_order(void) {
  char out[64];
  uint8_t writes[8];

  open_bus();
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime,
                                      "i2c.write-reg: bus, 0x76, 0xF4, 0x27",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", out);
  TEST_ASSERT_EQUAL_UINT16(2u, fr_host_i2c_drain_writes(0, writes,
                                                        sizeof(writes)));
  TEST_ASSERT_EQUAL_UINT8(0xF4, writes[0]);
  TEST_ASSERT_EQUAL_UINT8(0x27, writes[1]);
}

/* D3 — write-reg16 clocks MSB first; 0xCAFE → [reg, 0xCA, 0xFE]. */
static void test_write_reg16_emits_msb_first(void) {
  char out[64];
  uint8_t writes[8];

  open_bus();
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(
                        &s_runtime,
                        "i2c.write-reg16: bus, 0x68, 0x1B, 0xCAFE", out,
                        sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", out);
  TEST_ASSERT_EQUAL_UINT16(3u, fr_host_i2c_drain_writes(0, writes,
                                                        sizeof(writes)));
  TEST_ASSERT_EQUAL_UINT8(0x1B, writes[0]);
  TEST_ASSERT_EQUAL_UINT8(0xCA, writes[1]);
  TEST_ASSERT_EQUAL_UINT8(0xFE, writes[2]);
}

static void test_addr_above_127_rejects(void) {
  char out[64];

  open_bus();
  TEST_ASSERT_EQUAL(FR_ERR_DOMAIN,
                    fr_repl_eval_line(&s_runtime,
                                      "i2c.read-reg: bus, 0x80, 0x00",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL(FR_ERR_DOMAIN,
                    fr_repl_eval_line(&s_runtime,
                                      "i2c.read-reg16: bus, 0x80, 0x00",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL(FR_ERR_DOMAIN,
                    fr_repl_eval_line(&s_runtime,
                                      "i2c.write-reg: bus, 0x80, 0x00, 0x00",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL(
      FR_ERR_DOMAIN,
      fr_repl_eval_line(&s_runtime,
                        "i2c.write-reg16: bus, 0x80, 0x00, 0x0000", out,
                        sizeof(out)));
}

static void test_reg_above_255_rejects(void) {
  char out[128];

  open_bus();
  TEST_ASSERT_EQUAL(FR_ERR_DOMAIN,
                    fr_repl_eval_line(&s_runtime,
                                      "i2c.read-reg: bus, 0x76, 0x100",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL(FR_ERR_DOMAIN,
                    fr_repl_eval_line(&s_runtime,
                                      "i2c.write-reg: bus, 0x76, 0x100, 0x00",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("error: bad value: 256 (3)\n"
                           "detail: i2c.write-reg argument 3 was rejected\n",
                           out);
}

static void test_write_value_out_of_range_rejects(void) {
  char out[128];

  open_bus();
  TEST_ASSERT_EQUAL(FR_ERR_DOMAIN,
                    fr_repl_eval_line(&s_runtime,
                                      "i2c.write-reg: bus, 0x76, 0x10, 0x100",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("error: bad value: 256 (3)\n"
                           "detail: i2c.write-reg argument 4 was rejected\n",
                           out);
  TEST_ASSERT_EQUAL(
      FR_ERR_DOMAIN,
      fr_repl_eval_line(&s_runtime,
                        "i2c.write-reg16: bus, 0x76, 0x10, 0x10000", out,
                        sizeof(out)));
}

#endif /* FR_FEATURE_I2C */

int main(void) {
  UNITY_BEGIN();
#if FR_FEATURE_I2C
  RUN_TEST(test_read_reg_returns_queued_byte_and_writes_register);
  RUN_TEST(test_read_reg16_assembles_msb_first);
  RUN_TEST(test_write_reg_emits_two_bytes_in_order);
  RUN_TEST(test_write_reg16_emits_msb_first);
  RUN_TEST(test_addr_above_127_rejects);
  RUN_TEST(test_reg_above_255_rejects);
  RUN_TEST(test_write_value_out_of_range_rejects);
#endif
  return UNITY_END();
}
