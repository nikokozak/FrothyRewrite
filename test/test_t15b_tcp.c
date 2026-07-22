/*
 * Unity tests for T15b TCP natives.
 *
 * Drives the five natives through fr_repl_eval_line against the host stub,
 * covers the four reachable FR_ERR_NET_* paths (DNS / TIMEOUT / REFUSED /
 * DISCONNECTED), partial-read and EOF semantics, bytes-ready accuracy,
 * handle exhaustion at FR_TCP_HANDLE_COUNT, Wi-Fi-down latching via
 * fr_host_tcp_force_disconnect, and Ctrl-C interrupt via
 * fr_runtime_interrupt.
 */

#include "base_image.h"
#include "platform.h"
#include "repl.h"
#include "runtime.h"
#include "types.h"

#include "unity/unity.h"

#include <stdint.h>

void setUp(void) {
#if FR_FEATURE_NET
  fr_host_net_reset();
#endif
}

void tearDown(void) {}

#if FR_FEATURE_NET

static fr_runtime_t s_runtime;

static void install_base(void) {
  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
}

static void eval_ok(const char *line) {
  char out[128];
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, line, out, sizeof(out)));
}

static fr_err_t eval_err(const char *line) {
  char out[128];
  return fr_repl_eval_line(&s_runtime, line, out, sizeof(out));
}

static void eval_expect_output(const char *line, const char *expected) {
  char out[256];
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, line, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING(expected, out);
}

static void test_open_write_read_close_happy_path(void) {
  const uint8_t body[2] = {'h', 'i'};
  uint8_t drained[8] = {0};
  uint16_t drained_len = 0;

  install_base();
  fr_host_tcp_queue_response(0, body, 2);

  eval_ok("sock is tcp.open: \"example.com\", 80");
  eval_ok("tcp.write: sock, \"GET\"");
  eval_expect_output("text.pack: tcp.read: sock, 100", "\"hi\"\nok\n");

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_host_tcp_drain_writes(0, drained, sizeof(drained),
                                             &drained_len));
  TEST_ASSERT_EQUAL_UINT16(3, drained_len);
  TEST_ASSERT_EQUAL_MEMORY("GET", drained, 3);

  eval_ok("tcp.close: sock");
}

static void test_open_dns_failure(void) {
  install_base();
  TEST_ASSERT_EQUAL(FR_ERR_NET_DNS,
                    eval_err("tcp.open: \"fr.test.dns\", 80"));
}

static void test_open_timeout(void) {
  install_base();
  TEST_ASSERT_EQUAL(FR_ERR_NET_TIMEOUT,
                    eval_err("tcp.open: \"fr.test.timeout\", 80"));
}

static void test_open_refused_without_queue(void) {
  install_base();
  TEST_ASSERT_EQUAL(FR_ERR_NET_REFUSED,
                    eval_err("tcp.open: \"example.com\", 80"));
}

static void test_partial_read_returns_available_bytes(void) {
  const uint8_t body[3] = {'a', 'b', 'c'};

  install_base();
  fr_host_tcp_queue_response(0, body, 3);
  eval_ok("sock is tcp.open: \"example.com\", 80");
  eval_expect_output("text.pack: tcp.read: sock, 1024", "\"abc\"\nok\n");
}

static void test_eof_returns_empty_text(void) {
  const uint8_t body[2] = {'h', 'i'};

  install_base();
  fr_host_tcp_queue_response(0, body, 2);
  eval_ok("sock is tcp.open: \"example.com\", 80");
  eval_expect_output("text.pack: tcp.read: sock, 100", "\"hi\"\nok\n");
  eval_expect_output("text.pack: tcp.read: sock, 100", "\"\"\nok\n");
}

static void test_bytes_ready_tracks_queue_drain(void) {
  const uint8_t body[5] = {'a', 'b', 'c', 'd', 'e'};

  install_base();
  fr_host_tcp_queue_response(0, body, 5);
  eval_ok("sock is tcp.open: \"example.com\", 80");
  eval_expect_output("tcp.available: sock", "5\nok\n");
  eval_expect_output("text.pack: tcp.read: sock, 2", "\"ab\"\nok\n");
  eval_expect_output("tcp.available: sock", "3\nok\n");
}

static void test_force_disconnect_surfaces_on_next_op(void) {
  const uint8_t body[2] = {'h', 'i'};

  install_base();
  fr_host_tcp_queue_response(0, body, 2);
  eval_ok("sock is tcp.open: \"example.com\", 80");
  fr_host_tcp_force_disconnect(0);
  TEST_ASSERT_EQUAL(FR_ERR_NET_DISCONNECTED,
                    eval_err("tcp.read: sock, 100"));
  /* D12: failed flag latches so a later call still surfaces disconnected
   * even though the host stub's wifi_down has not been cleared. */
  TEST_ASSERT_TRUE(s_runtime.tcp_handles[0].failed);
  TEST_ASSERT_EQUAL(FR_ERR_NET_DISCONNECTED,
                    eval_err("tcp.available: sock"));
}

static void test_close_then_reopen_clears_failed(void) {
  const uint8_t body[2] = {'h', 'i'};

  install_base();
  fr_host_tcp_queue_response(0, body, 2);
  eval_ok("s1 is tcp.open: \"example.com\", 80");
  fr_host_tcp_force_disconnect(0);
  TEST_ASSERT_EQUAL(FR_ERR_NET_DISCONNECTED,
                    eval_err("tcp.read: s1, 100"));
  eval_ok("tcp.close: s1");
  fr_host_tcp_queue_response(0, body, 2);
  eval_ok("s2 is tcp.open: \"example.com\", 80");
  TEST_ASSERT_FALSE(s_runtime.tcp_handles[0].failed);
  eval_expect_output("text.pack: tcp.read: s2, 100", "\"hi\"\nok\n");
  eval_ok("tcp.close: s2");
}

static void test_handle_exhaustion_returns_capacity(void) {
  const uint8_t one = '.';

  install_base();
  for (uint16_t i = 0; i < FR_TCP_HANDLE_COUNT; i++) {
    fr_host_tcp_queue_response(i, &one, 1);
  }
  eval_ok("a is tcp.open: \"example.com\", 80");
  eval_ok("b is tcp.open: \"example.com\", 80");
  eval_ok("c is tcp.open: \"example.com\", 80");
  eval_ok("d is tcp.open: \"example.com\", 80");
  TEST_ASSERT_EQUAL(FR_ERR_CAPACITY,
                    eval_err("tcp.open: \"example.com\", 80"));
}

static void test_interrupt_during_read_returns_interrupted(void) {
  const uint8_t body[2] = {'h', 'i'};
  uint16_t platform_index = 0;
  uint8_t buf[16] = {0};
  uint16_t length = 0;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  fr_host_tcp_queue_response(0, body, 2);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_tcp_open(&s_runtime, "example.com", 80,
                                         &platform_index));
  fr_runtime_interrupt(&s_runtime);
  TEST_ASSERT_EQUAL(FR_ERR_INTERRUPTED,
                    fr_platform_tcp_read(&s_runtime, platform_index, buf,
                                         sizeof(buf), &length));
  fr_runtime_clear_interrupt(&s_runtime);
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_tcp_close(platform_index));
}

#endif /* FR_FEATURE_NET */

int main(void) {
  UNITY_BEGIN();
#if FR_FEATURE_NET
  RUN_TEST(test_open_write_read_close_happy_path);
  RUN_TEST(test_open_dns_failure);
  RUN_TEST(test_open_timeout);
  RUN_TEST(test_open_refused_without_queue);
  RUN_TEST(test_partial_read_returns_available_bytes);
  RUN_TEST(test_eof_returns_empty_text);
  RUN_TEST(test_bytes_ready_tracks_queue_drain);
  RUN_TEST(test_force_disconnect_surfaces_on_next_op);
  RUN_TEST(test_close_then_reopen_clears_failed);
  RUN_TEST(test_handle_exhaustion_returns_capacity);
  RUN_TEST(test_interrupt_during_read_returns_interrupted);
#endif
  return UNITY_END();
}
