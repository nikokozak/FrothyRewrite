/*
 * Unity tests for T15 Wi-Fi + HTTP natives.
 *
 * Drives the four natives through fr_repl_eval_line against the host stub,
 * hits every reachable FR_ERR_NET_* path, exercises the HTTP cap overflow,
 * and proves wifi.disconnected / wifi.reconnected bindings dispatch when the
 * host event helper fires a candidate.
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

static void test_wifi_save_then_connect_then_ready(void) {
  char out[64];

  install_base();
  eval_ok("wifi.save: \"my-ssid\", \"my-pass\"");
  eval_ok("wifi.connect:");
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "wifi.ready?:",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("true\nok\n", out);
}

static void test_wifi_ready_false_before_connect(void) {
  char out[64];

  install_base();
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "wifi.ready?:",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("false\nok\n", out);
}

static void test_wifi_connect_without_creds_disconnected(void) {
  install_base();
  TEST_ASSERT_EQUAL(FR_ERR_NET_DISCONNECTED, eval_err("wifi.connect:"));
}

static void test_http_get_without_wifi_disconnected(void) {
  install_base();
  TEST_ASSERT_EQUAL(FR_ERR_NET_DISCONNECTED,
                    eval_err("http.get: \"http://example.com/\""));
}

static void test_http_get_empty_url_protocol(void) {
  install_base();
  fr_host_wifi_set_connected(true);
  TEST_ASSERT_EQUAL(FR_ERR_NET_PROTOCOL, eval_err("http.get: \"\""));
}

static void test_http_get_no_response_refused(void) {
  install_base();
  fr_host_wifi_set_connected(true);
  TEST_ASSERT_EQUAL(FR_ERR_NET_REFUSED,
                    eval_err("http.get: \"http://example.com/\""));
}

static void test_http_get_non_2xx_refused(void) {
  uint8_t body[3] = {'n', 'o', '\0'};

  install_base();
  fr_host_wifi_set_connected(true);
  fr_host_http_queue_response(404, body, 2);
  TEST_ASSERT_EQUAL(FR_ERR_NET_REFUSED,
                    eval_err("http.get: \"http://example.com/\""));
}

/* Sized to back a length=FR_HTTP_MAX_BODY+1 claim without the helper's memcpy
 * reading past the source. The helper copies min(length, FR_HTTP_MAX_BODY) bytes
 * regardless of which length the caller claims. */
static uint8_t oversized_body[FR_HTTP_MAX_BODY];

static void test_http_get_oversized_too_large(void) {
  install_base();
  fr_host_wifi_set_connected(true);
  fr_host_http_queue_response(200, oversized_body,
                              (uint16_t)(FR_HTTP_MAX_BODY + 1u));
  TEST_ASSERT_EQUAL(FR_ERR_NET_TOO_LARGE,
                    eval_err("http.get: \"http://example.com/\""));
}

static void test_http_get_success_returns_body(void) {
  char out[64];
  const uint8_t body[2] = {'h', 'i'};

  install_base();
  fr_host_wifi_set_connected(true);
  fr_host_http_queue_response(200, body, 2);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(
                        &s_runtime,
                        "text.pack: http.get: \"http://example.com/\"",
                        out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("\"hi\"\nok\n", out);
}

static void test_credentials_round_trip(void) {
  char out[64];

  install_base();
  eval_ok("wifi.save: \"saved-ssid\", \"saved-pass\"");
  /* wifi.connect with no helper flip succeeds only because save staged creds
   * the host stub reads back on the connect call. */
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "wifi.connect:",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", out);
}

static void test_err_names_cover_all_six(void) {
  TEST_ASSERT_NOT_NULL(fr_err_name(FR_ERR_NET_DISCONNECTED));
  TEST_ASSERT_NOT_NULL(fr_err_name(FR_ERR_NET_TIMEOUT));
  TEST_ASSERT_NOT_NULL(fr_err_name(FR_ERR_NET_DNS));
  TEST_ASSERT_NOT_NULL(fr_err_name(FR_ERR_NET_REFUSED));
  TEST_ASSERT_NOT_NULL(fr_err_name(FR_ERR_NET_TOO_LARGE));
  TEST_ASSERT_NOT_NULL(fr_err_name(FR_ERR_NET_PROTOCOL));
}

static void test_wifi_disconnected_event_runs_body(void) {
  char out[64];

  install_base();
  eval_ok("counter is cells: 1");
  eval_ok("set counter[0] to 1");
  eval_ok("mark is fn [ set counter[0] to 42 ]");
  eval_ok("boot is fn [ on wifi.disconnected [ mark: ] ]");
  eval_ok("boot:");
  fr_host_wifi_fire_event(FR_EVENT_KIND_WIFI_DISCONNECTED);
  /* Drain + dispatch happen across a VM run; the body sets counter[0] during
   * this line's RETURN so the next read observes the update. */
  eval_ok("0");
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "counter[0]",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("42\nok\n", out);
}

static void test_wifi_reconnected_event_runs_body(void) {
  char out[64];

  install_base();
  eval_ok("counter is cells: 1");
  eval_ok("set counter[0] to 1");
  eval_ok("mark is fn [ set counter[0] to 99 ]");
  eval_ok("boot is fn [ on wifi.reconnected [ mark: ] ]");
  eval_ok("boot:");
  fr_host_wifi_fire_event(FR_EVENT_KIND_WIFI_RECONNECTED);
  eval_ok("0");
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "counter[0]",
                                      out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("99\nok\n", out);
}

#endif /* FR_FEATURE_NET */

int main(void) {
  UNITY_BEGIN();
#if FR_FEATURE_NET
  RUN_TEST(test_wifi_save_then_connect_then_ready);
  RUN_TEST(test_wifi_ready_false_before_connect);
  RUN_TEST(test_wifi_connect_without_creds_disconnected);
  RUN_TEST(test_http_get_without_wifi_disconnected);
  RUN_TEST(test_http_get_empty_url_protocol);
  RUN_TEST(test_http_get_no_response_refused);
  RUN_TEST(test_http_get_non_2xx_refused);
  RUN_TEST(test_http_get_oversized_too_large);
  RUN_TEST(test_http_get_success_returns_body);
  RUN_TEST(test_credentials_round_trip);
  RUN_TEST(test_err_names_cover_all_six);
  RUN_TEST(test_wifi_disconnected_event_runs_body);
  RUN_TEST(test_wifi_reconnected_event_runs_body);
#endif
  return UNITY_END();
}
