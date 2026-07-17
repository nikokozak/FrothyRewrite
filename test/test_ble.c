/* Focused proof for the deterministic host BLE platform contract. */

#include "platform.h"
#include "runtime.h"

#include "unity/unity.h"

#include <string.h>

#if !FR_FEATURE_BLE || !FR_BLE_ENABLE_OBSERVER
#error "test_ble requires the BLE observer profile gates"
#endif

static fr_runtime_t s_runtime;

void setUp(void) {
  TEST_ASSERT_EQUAL(FR_OK, fr_runtime_init(&s_runtime));
  fr_host_ble_reset();
}

void tearDown(void) {}

static fr_ble_status_t read_status(void) {
  fr_ble_status_t status;

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_status(&status));
  return status;
}

static fr_ble_scan_report_t report_with(uint8_t id, int8_t rssi,
                                        uint8_t data_length) {
  fr_ble_scan_report_t report;

  memset(&report, 0, sizeof(report));
  report.address_type = FR_BLE_ADDRESS_PUBLIC;
  report.address[5] = id;
  report.rssi = rssi;
  report.flags = FR_BLE_REPORT_LEGACY;
  report.data_length = data_length;
  if (data_length <= FR_BLE_SCAN_DATA_BYTES) {
    report.data[0] = id;
  }
  return report;
}

static void test_radio_lifecycle_and_status(void) {
  static const uint8_t own_address[6] = {0xaa, 0xbb, 0xcc,
                                         0xdd, 0xee, 0xff};
  fr_ble_status_t status = read_status();

  TEST_ASSERT_EQUAL_STRING("host-fixture", fr_platform_ble_backend_name());
  TEST_ASSERT_EQUAL(FR_BLE_RADIO_OFF, status.radio_state);
  TEST_ASSERT_EQUAL(FR_BLE_SCAN_IDLE, status.scan_state);
  TEST_ASSERT_EQUAL_UINT8(FR_BLE_ROLE_OBSERVER, status.roles);
  TEST_ASSERT_EQUAL_UINT8(8, status.queue_capacity);
  TEST_ASSERT_FALSE(status.coexistence_enabled);

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_RADIO_READY, status.radio_state);
  TEST_ASSERT_EQUAL_UINT32(1, status.lifecycle_generation);
  TEST_ASSERT_TRUE(status.own_address_valid);
  TEST_ASSERT_EQUAL_MEMORY(own_address, status.own_address,
                           sizeof(own_address));

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));
  TEST_ASSERT_EQUAL_UINT32(1, read_status().lifecycle_generation);

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_project_clear());
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_RADIO_OFF, status.radio_state);
  TEST_ASSERT_EQUAL(FR_BLE_OP_NONE, status.last_operation);
  TEST_ASSERT_FALSE(status.own_address_valid);
}

static void test_scan_parameters_and_state_gates(void) {
  fr_ble_status_t status;

  TEST_ASSERT_EQUAL(FR_ERR_BLE_NOT_READY,
                    fr_platform_ble_scan_start(100, 50, false, false, -90));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));

  TEST_ASSERT_EQUAL(FR_ERR_RANGE,
                    fr_platform_ble_scan_start(2, 2, false, false, -90));
  TEST_ASSERT_EQUAL(
      FR_ERR_RANGE,
      fr_platform_ble_scan_start(10241, 10241, false, false, -90));
  TEST_ASSERT_EQUAL(FR_ERR_RANGE,
                    fr_platform_ble_scan_start(50, 51, false, false, -90));
  status = read_status();
  TEST_ASSERT_EQUAL_UINT32(0, status.scan_generation);
  TEST_ASSERT_EQUAL_UINT16(0, status.requested_interval_ms);

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_ble_scan_start(3, 3, true, true, -127));
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_SCAN_ACTIVE, status.scan_state);
  TEST_ASSERT_EQUAL_UINT32(1, status.scan_generation);
  TEST_ASSERT_EQUAL_UINT16(3, status.requested_interval_ms);
  TEST_ASSERT_EQUAL_UINT16(3, status.requested_window_ms);
  TEST_ASSERT_EQUAL_UINT32(3000, status.actual_interval_us);
  TEST_ASSERT_EQUAL_UINT32(3000, status.actual_window_us);
  TEST_ASSERT_EQUAL_INT8(-127, status.minimum_rssi);
  TEST_ASSERT_TRUE(status.active_scan);
  TEST_ASSERT_TRUE(status.repeats);

  TEST_ASSERT_EQUAL(FR_ERR_BLE_BUSY,
                    fr_platform_ble_scan_start(100, 50, false, false, -90));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_scan_stop(&s_runtime));
  TEST_ASSERT_EQUAL(
      FR_OK, fr_platform_ble_scan_start(10240, 10240, false, false, 20));
}

static void test_queue_conservation_overflow_and_cursor(void) {
  fr_ble_scan_report_t report;
  fr_ble_scan_report_t current;
  fr_ble_status_t status;
  bool has_report = false;

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));
  TEST_ASSERT_EQUAL(
      FR_OK, fr_platform_ble_scan_start(100, 50, false, false, -90));

  report = report_with(90, -100, 1);
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_push_scan_report(&report));
  report = report_with(91, -40, 32);
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_push_scan_report(&report));
  for (uint8_t id = 0; id < 9; id++) {
    report = report_with(id, -40, 1);
    TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_push_scan_report(&report));
  }

  status = read_status();
  TEST_ASSERT_EQUAL_UINT8(8, status.queue_count);
  TEST_ASSERT_EQUAL_UINT8(8, status.queue_high_water);
  TEST_ASSERT_EQUAL_UINT32(11, status.received);
  TEST_ASSERT_EQUAL_UINT32(9, status.accepted);
  TEST_ASSERT_EQUAL_UINT32(1, status.filtered_rssi);
  TEST_ASSERT_EQUAL_UINT32(1, status.malformed);
  TEST_ASSERT_EQUAL_UINT32(1, status.dropped);
  TEST_ASSERT_EQUAL_UINT32(
      status.received,
      status.accepted + status.filtered_rssi + status.malformed);
  TEST_ASSERT_EQUAL_UINT32(
      status.accepted,
      status.queue_count + status.dequeued + status.dropped);

  TEST_ASSERT_EQUAL(FR_ERR_NOT_FOUND,
                    fr_platform_ble_scan_current(&current));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_scan_next(&has_report));
  TEST_ASSERT_TRUE(has_report);
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_scan_current(&current));
  TEST_ASSERT_EQUAL_UINT8(1, current.address[5]);
  TEST_ASSERT_EQUAL_UINT8(1, current.data[0]);

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_scan_stop(&s_runtime));
  TEST_ASSERT_EQUAL(FR_ERR_NOT_FOUND,
                    fr_platform_ble_scan_current(&current));
  TEST_ASSERT_EQUAL_UINT8(7, read_status().queue_count);
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_scan_next(&has_report));
  TEST_ASSERT_TRUE(has_report);
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_scan_current(&current));
  TEST_ASSERT_EQUAL_UINT8(2, current.address[5]);

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_scan_stop(&s_runtime));
  TEST_ASSERT_EQUAL(FR_ERR_NOT_FOUND,
                    fr_platform_ble_scan_current(&current));
  TEST_ASSERT_EQUAL(
      FR_OK, fr_platform_ble_scan_start(100, 50, false, false, -90));
  status = read_status();
  TEST_ASSERT_EQUAL_UINT8(0, status.queue_count);
  TEST_ASSERT_EQUAL_UINT32(0, status.received);
  TEST_ASSERT_EQUAL_UINT32(0, status.accepted);
  TEST_ASSERT_EQUAL_UINT32(2, status.scan_generation);
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_scan_next(&has_report));
  TEST_ASSERT_FALSE(has_report);
  TEST_ASSERT_EQUAL(FR_ERR_NOT_FOUND,
                    fr_platform_ble_scan_current(&current));
}

static void test_failures_timeouts_reset_and_clear(void) {
  fr_ble_scan_report_t report = report_with(1, -40, 1);
  fr_ble_scan_report_t current;
  fr_ble_status_t status;
  bool has_report = false;

  fr_host_ble_fail_next_on(FR_ERR_IO, -77);
  TEST_ASSERT_EQUAL(FR_ERR_IO, fr_platform_ble_on(&s_runtime));
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_RADIO_FAILED, status.radio_state);
  TEST_ASSERT_FALSE(status.cleanup_required);
  TEST_ASSERT_EQUAL_INT32(-77, status.last_platform_code);
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));

  TEST_ASSERT_EQUAL(
      FR_OK, fr_platform_ble_scan_start(100, 50, false, false, -90));
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_push_scan_report(&report));
  fr_host_ble_post_reset(19);
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_RADIO_FAILED, status.radio_state);
  TEST_ASSERT_TRUE(status.cleanup_required);
  TEST_ASSERT_EQUAL_UINT32(1, status.reset_count);
  TEST_ASSERT_EQUAL_INT32(19, status.last_protocol_reason);
  TEST_ASSERT_EQUAL_UINT8(0, status.queue_count);
  TEST_ASSERT_EQUAL_UINT32(1, status.dropped);
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));

  TEST_ASSERT_EQUAL(
      FR_OK, fr_platform_ble_scan_start(100, 50, false, false, -90));
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_push_scan_report(&report));
  report.address[5] = 2;
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_push_scan_report(&report));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_scan_next(&has_report));
  TEST_ASSERT_TRUE(has_report);
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_scan_current(&current));

  fr_host_ble_timeout_next_scan_stop();
  TEST_ASSERT_EQUAL(FR_ERR_BLE_TIMEOUT,
                    fr_platform_ble_scan_stop(&s_runtime));
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_SCAN_STOPPING, status.scan_state);
  TEST_ASSERT_EQUAL_UINT8(1, status.queue_count);
  TEST_ASSERT_FALSE(status.current_valid);
  TEST_ASSERT_EQUAL(FR_ERR_BLE_BUSY,
                    fr_platform_ble_scan_start(100, 50, false, false, -90));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_scan_stop(&s_runtime));

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_project_clear());
  fr_host_ble_timeout_next_on();
  TEST_ASSERT_EQUAL(FR_ERR_BLE_TIMEOUT, fr_platform_ble_on(&s_runtime));
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_RADIO_STOPPING, status.radio_state);
  TEST_ASSERT_TRUE(status.shutdown_in_progress);
  TEST_ASSERT_TRUE(status.cleanup_required);
  TEST_ASSERT_EQUAL(FR_ERR_BLE_BUSY, fr_platform_ble_on(&s_runtime));
}

static void test_target_start_failure_owns_new_empty_session(void) {
  fr_ble_scan_report_t report = report_with(1, -40, 1);
  fr_ble_status_t status;

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));
  TEST_ASSERT_EQUAL(
      FR_OK, fr_platform_ble_scan_start(100, 50, false, false, -90));
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_push_scan_report(&report));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_scan_stop(&s_runtime));

  fr_host_ble_fail_next_scan_start(FR_ERR_IO, 63);
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));
  TEST_ASSERT_EQUAL(
      FR_ERR_IO, fr_platform_ble_scan_start(200, 100, false, true, -80));
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_SCAN_IDLE, status.scan_state);
  TEST_ASSERT_EQUAL_UINT32(2, status.scan_generation);
  TEST_ASSERT_EQUAL_UINT8(0, status.queue_count);
  TEST_ASSERT_EQUAL_UINT32(0, status.received);
  TEST_ASSERT_FALSE(status.current_valid);
  TEST_ASSERT_EQUAL_UINT16(200, status.requested_interval_ms);
  TEST_ASSERT_EQUAL_INT32(63, status.last_platform_code);
}

static void test_runtime_clear_shuts_down_ble_before_reuse(void) {
  fr_ble_scan_report_t report = report_with(1, -40, 1);
  fr_ble_status_t status;

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));
  TEST_ASSERT_EQUAL(
      FR_OK, fr_platform_ble_scan_start(100, 50, false, false, -90));
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_push_scan_report(&report));
  TEST_ASSERT_EQUAL_UINT8(1, read_status().queue_count);

  TEST_ASSERT_EQUAL(FR_OK, fr_runtime_clear_project(&s_runtime));
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_RADIO_OFF, status.radio_state);
  TEST_ASSERT_EQUAL(FR_BLE_SCAN_IDLE, status.scan_state);
  TEST_ASSERT_EQUAL_UINT8(0, status.queue_count);
  TEST_ASSERT_FALSE(status.current_valid);
  TEST_ASSERT_EQUAL(FR_BLE_OP_NONE, status.last_operation);

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));
  TEST_ASSERT_EQUAL_UINT32(2, read_status().lifecycle_generation);
  TEST_ASSERT_EQUAL(
      FR_OK, fr_platform_ble_scan_start(100, 50, false, false, -90));
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_push_scan_report(&report));
  TEST_ASSERT_EQUAL_UINT8(1, read_status().queue_count);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_radio_lifecycle_and_status);
  RUN_TEST(test_scan_parameters_and_state_gates);
  RUN_TEST(test_queue_conservation_overflow_and_cursor);
  RUN_TEST(test_failures_timeouts_reset_and_clear);
  RUN_TEST(test_target_start_failure_owns_new_empty_session);
  RUN_TEST(test_runtime_clear_shuts_down_ble_before_reuse);
  return UNITY_END();
}
