/* Focused proof for the deterministic host BLE platform contract. */

#include "base_defs.h"
#include "base_image.h"
#include "object.h"
#include "persist.h"
#include "platform.h"
#include "repl.h"
#include "runtime.h"
#include "tagged.h"

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

static void read_native_def(const char *name, fr_slot_id_t expected_slot,
                            uint8_t expected_arity,
                            const fr_base_def_t **out_def) {
  fr_base_layer_t layer = FR_BASE_LAYER_CORE;
  fr_slot_id_t slot_id = 0;

  TEST_ASSERT_NOT_NULL(out_def);
  TEST_ASSERT_EQUAL(FR_OK, fr_base_slot_id_for_name(name, &slot_id));
  TEST_ASSERT_EQUAL_UINT16(expected_slot, slot_id);
  TEST_ASSERT_EQUAL(FR_OK, fr_base_def_for_slot(slot_id, out_def, &layer));
  TEST_ASSERT_EQUAL(FR_BASE_LAYER_TARGET, layer);
  TEST_ASSERT_EQUAL(FR_BASE_DEF_NATIVE, (*out_def)->kind);
  TEST_ASSERT_EQUAL_UINT8(expected_arity, (*out_def)->native_arity);
}

static void test_lifecycle_words_and_status_retention(void) {
  const fr_base_def_t *def = NULL;
  fr_base_layer_t layer = FR_BASE_LAYER_CORE;
  fr_ble_status_t status;
  fr_slot_id_t slot_id = 0;
  char out[64];

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_base_slot_id_for_name("ble.on", &slot_id));
  TEST_ASSERT_EQUAL_UINT16(FR_SLOT_BLE_ON, slot_id);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_base_def_for_slot(slot_id, &def, &layer));
  TEST_ASSERT_EQUAL(FR_BASE_LAYER_TARGET, layer);
  TEST_ASSERT_EQUAL_UINT8(0, def->native_arity);
#if FR_FEATURE_NATIVE_SIGNATURES
  TEST_ASSERT_NOT_NULL(def->native_signature);
  TEST_ASSERT_EQUAL(FR_NATIVE_VALUE_NIL, def->native_signature->result);
  TEST_ASSERT_EQUAL_STRING(
      "initialize the compiled BLE roles and wait for radio readiness",
      def->native_signature->help);
#endif

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_base_slot_id_for_name("ble.info", &slot_id));
  TEST_ASSERT_EQUAL_UINT16(FR_SLOT_BLE_INFO, slot_id);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_base_def_for_slot(slot_id, &def, &layer));
  TEST_ASSERT_EQUAL(FR_BASE_LAYER_TARGET, layer);
  TEST_ASSERT_EQUAL_UINT8(0, def->native_arity);
#if FR_FEATURE_NATIVE_SIGNATURES
  TEST_ASSERT_NOT_NULL(def->native_signature);
  TEST_ASSERT_EQUAL(FR_NATIVE_VALUE_NIL, def->native_signature->result);
  TEST_ASSERT_EQUAL_STRING(
      "print BLE roles, radio state, scan state, queue pressure, and last raw "
      "reason",
      def->native_signature->help);
#endif

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "ble.info:", out,
                                      sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", out);
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_OP_NONE, status.last_operation);

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "ble.on:", out,
                                      sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", out);
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_RADIO_READY, status.radio_state);
  TEST_ASSERT_EQUAL(FR_BLE_OP_ON, status.last_operation);

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "ble.info:", out,
                                      sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", out);
  TEST_ASSERT_EQUAL(FR_BLE_OP_ON, read_status().last_operation);
}

static void test_scanner_words_bridge_tagged_values_and_reports(void) {
  static const struct {
    const char *name;
    fr_slot_id_t slot_id;
    uint8_t arity;
    fr_native_value_kind_t result;
    const char *help;
  } words[] = {
      {"ble.scan.start", FR_SLOT_BLE_SCAN_START, 5, FR_NATIVE_VALUE_NIL,
       "start an indefinite BLE scan with interval/window ms, active/repeat "
       "flags, and minimum RSSI"},
      {"ble.scan.stop", FR_SLOT_BLE_SCAN_STOP, 0, FR_NATIVE_VALUE_NIL,
       "stop the active BLE scan and retain queued reports"},
      {"ble.scan.next?", FR_SLOT_BLE_SCAN_NEXT, 0, FR_NATIVE_VALUE_ANY,
       "move to the next queued BLE report and say whether one was available"},
      {"ble.scan.rssi", FR_SLOT_BLE_SCAN_RSSI, 0, FR_NATIVE_VALUE_INT,
       "return the current BLE report RSSI in dBm"},
      {"ble.scan.peer", FR_SLOT_BLE_SCAN_PEER, 0, FR_NATIVE_VALUE_ANY,
       "copy the current BLE peer type and canonical address bytes"},
      {"ble.scan.flags", FR_SLOT_BLE_SCAN_FLAGS, 0, FR_NATIVE_VALUE_INT,
       "return the current BLE report flags"},
      {"ble.scan.data", FR_SLOT_BLE_SCAN_DATA, 0, FR_NATIVE_VALUE_ANY,
       "copy the current raw BLE advertisement bytes"},
  };
  const fr_base_def_t *defs[sizeof(words) / sizeof(words[0])] = {0};
  fr_ble_scan_report_t report = {0};
  fr_tagged_t args[5] = {0};
  fr_tagged_t result = fr_tagged_nil();
  fr_bytes_ref_t bytes_ref = {0};
  const uint8_t *bytes = NULL;
  uint16_t length = 0;
  fr_int_t integer = 0;
  bool available = false;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    read_native_def(words[i].name, words[i].slot_id, words[i].arity,
                    &defs[i]);
#if FR_FEATURE_NATIVE_SIGNATURES
    TEST_ASSERT_NOT_NULL(defs[i]->native_signature);
    TEST_ASSERT_EQUAL_UINT8(words[i].arity,
                            defs[i]->native_signature->arg_count);
    TEST_ASSERT_EQUAL(words[i].result, defs[i]->native_signature->result);
    TEST_ASSERT_EQUAL_STRING(words[i].help, defs[i]->native_signature->help);
#endif
  }
#if FR_FEATURE_NATIVE_SIGNATURES
  TEST_ASSERT_EQUAL_STRING("interval_ms",
                           defs[0]->native_signature->params[0].name);
  TEST_ASSERT_EQUAL_STRING("window_ms",
                           defs[0]->native_signature->params[1].name);
  TEST_ASSERT_EQUAL_STRING("active",
                           defs[0]->native_signature->params[2].name);
  TEST_ASSERT_EQUAL_STRING("repeats",
                           defs[0]->native_signature->params[3].name);
  TEST_ASSERT_EQUAL_STRING("minimum_rssi",
                           defs[0]->native_signature->params[4].name);
  for (uint8_t i = 0; i < defs[0]->native_signature->arg_count; i++) {
    TEST_ASSERT_EQUAL(FR_NATIVE_VALUE_INT,
                      defs[0]->native_signature->params[i].type);
  }
#endif

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(100, &args[0]));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(50, &args[1]));
  args[2] = fr_tagged_true();
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(1, &args[3]));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(-90, &args[4]));
  TEST_ASSERT_EQUAL(FR_ERR_INVALID,
                    defs[0]->native_fn(&s_runtime, args, 5, &result));
  args[2] = FR_TAGGED_INT_LITERAL(0);
  args[3] = fr_tagged_false();
  TEST_ASSERT_EQUAL(FR_ERR_INVALID,
                    defs[0]->native_fn(&s_runtime, args, 5, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(0, &args[3]));
  args[4] = fr_tagged_true();
  TEST_ASSERT_EQUAL(FR_ERR_INVALID,
                    defs[0]->native_fn(&s_runtime, args, 5, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(-90, &args[4]));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(2, &args[2]));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(1, &args[3]));
  TEST_ASSERT_EQUAL(FR_ERR_RANGE,
                    defs[0]->native_fn(&s_runtime, args, 5, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(0, &args[2]));
  TEST_ASSERT_EQUAL(FR_OK,
                    defs[0]->native_fn(&s_runtime, args, 5, &result));
  TEST_ASSERT_TRUE(fr_tagged_is_nil(result));

  report.address_type = FR_BLE_ADDRESS_RANDOM_ID;
  report.address[0] = 0x10;
  report.address[1] = 0x20;
  report.address[2] = 0x30;
  report.address[3] = 0x40;
  report.address[4] = 0x50;
  report.address[5] = 0x60;
  report.rssi = -42;
  report.flags = FR_BLE_REPORT_CONNECTABLE | FR_BLE_REPORT_SCANNABLE |
                 FR_BLE_REPORT_LEGACY;
  report.data_length = 3;
  report.data[0] = 2;
  report.data[1] = 1;
  report.data[2] = 6;
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_push_scan_report(&report));

  TEST_ASSERT_EQUAL(FR_OK,
                    defs[2]->native_fn(&s_runtime, NULL, 0, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_bool(result, &available));
  TEST_ASSERT_TRUE(available);

  TEST_ASSERT_EQUAL(FR_OK,
                    defs[3]->native_fn(&s_runtime, NULL, 0, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(result, &integer));
  TEST_ASSERT_EQUAL_INT(-42, integer);

  TEST_ASSERT_EQUAL(FR_OK,
                    defs[4]->native_fn(&s_runtime, NULL, 0, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_bytes_ref(result, &bytes_ref));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_bytes_view(&s_runtime, bytes_ref, &bytes, &length));
  TEST_ASSERT_EQUAL_UINT16(7, length);
  TEST_ASSERT_EQUAL_UINT8(FR_BLE_ADDRESS_RANDOM_ID, bytes[0]);
  TEST_ASSERT_EQUAL_MEMORY(report.address, &bytes[1], sizeof(report.address));

  TEST_ASSERT_EQUAL(FR_OK,
                    defs[5]->native_fn(&s_runtime, NULL, 0, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(result, &integer));
  TEST_ASSERT_EQUAL_UINT8(report.flags, integer);

  TEST_ASSERT_EQUAL(FR_OK,
                    defs[6]->native_fn(&s_runtime, NULL, 0, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_bytes_ref(result, &bytes_ref));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_bytes_view(&s_runtime, bytes_ref, &bytes, &length));
  TEST_ASSERT_EQUAL_UINT16(report.data_length, length);
  TEST_ASSERT_EQUAL_MEMORY(report.data, bytes, report.data_length);

  TEST_ASSERT_EQUAL(FR_OK,
                    defs[1]->native_fn(&s_runtime, NULL, 0, &result));
  TEST_ASSERT_TRUE(fr_tagged_is_nil(result));

  for (size_t i = 2; i < sizeof(words) / sizeof(words[0]); i++) {
    TEST_ASSERT_EQUAL(FR_ERR_INVALID,
                      defs[i]->native_fn(&s_runtime, args, 1, &result));
  }
}

static void test_scanner_loop_reuses_eval_bytes(void) {
  fr_ble_scan_report_t report;
  fr_ble_scan_report_t current;
  fr_ble_status_t status;
  char out[64];

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));
  TEST_ASSERT_EQUAL(
      FR_OK, fr_platform_ble_scan_start(100, 50, false, false, -90));
  for (uint8_t id = 0; id < FR_BLE_SCAN_QUEUE_COUNT; id++) {
    report = report_with(id, -40, FR_BLE_SCAN_DATA_BYTES);
    TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_push_scan_report(&report));
  }

  TEST_ASSERT_EQUAL(
      FR_OK,
      fr_repl_eval_line(
          &s_runtime,
          "repeat 8 [ ble.scan.next?:; ble.scan.peer:; ble.scan.data: ]",
          out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", out);
  status = read_status();
  TEST_ASSERT_EQUAL_UINT8(0, status.queue_count);
  TEST_ASSERT_EQUAL_UINT32(8, status.dequeued);
  TEST_ASSERT_TRUE(status.current_valid);
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_scan_current(&current));
  TEST_ASSERT_EQUAL_UINT8(7, current.address[5]);
  TEST_ASSERT_EQUAL_UINT8(7, current.data[0]);
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

static void test_save_and_restore_drop_volatile_ble_state(void) {
  fr_ble_scan_report_t report = report_with(7, -40, 3);
  fr_ble_status_t status;
  char out[64];

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));
  TEST_ASSERT_EQUAL(
      FR_OK, fr_platform_ble_scan_start(100, 50, false, false, -90));
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_push_scan_report(&report));
  TEST_ASSERT_EQUAL(FR_ERR_INVALID, fr_persist_restore(NULL));
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_RADIO_READY, status.radio_state);
  TEST_ASSERT_EQUAL(FR_BLE_SCAN_ACTIVE, status.scan_state);
  TEST_ASSERT_EQUAL_UINT8(1, status.queue_count);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "ble.scan.next?:", out,
                                      sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("true\nok\n", out);
  TEST_ASSERT_EQUAL(FR_ERR_VOLATILE,
                    fr_repl_eval_line(&s_runtime,
                                      "saved-peer is ble.scan.peer:", out,
                                      sizeof(out)));

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_repl_eval_line(&s_runtime, "save", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", out);
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_RADIO_OFF, status.radio_state);
  TEST_ASSERT_EQUAL(FR_BLE_SCAN_IDLE, status.scan_state);
  TEST_ASSERT_EQUAL_UINT8(0, status.queue_count);
  TEST_ASSERT_FALSE(status.current_valid);

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));
  TEST_ASSERT_EQUAL(
      FR_OK, fr_platform_ble_scan_start(100, 50, false, false, -90));
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_push_scan_report(&report));
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_RADIO_READY, status.radio_state);
  TEST_ASSERT_EQUAL(FR_BLE_SCAN_ACTIVE, status.scan_state);
  TEST_ASSERT_EQUAL_UINT8(1, status.queue_count);

  TEST_ASSERT_EQUAL(
      FR_OK, fr_repl_eval_line(&s_runtime, "restore", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("ok\n", out);
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_RADIO_OFF, status.radio_state);
  TEST_ASSERT_EQUAL(FR_BLE_SCAN_IDLE, status.scan_state);
  TEST_ASSERT_EQUAL_UINT8(0, status.queue_count);
  TEST_ASSERT_FALSE(status.current_valid);
  TEST_ASSERT_EQUAL(FR_BLE_OP_NONE, status.last_operation);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_lifecycle_words_and_status_retention);
  RUN_TEST(test_scanner_words_bridge_tagged_values_and_reports);
  RUN_TEST(test_scanner_loop_reuses_eval_bytes);
  RUN_TEST(test_radio_lifecycle_and_status);
  RUN_TEST(test_scan_parameters_and_state_gates);
  RUN_TEST(test_queue_conservation_overflow_and_cursor);
  RUN_TEST(test_failures_timeouts_reset_and_clear);
  RUN_TEST(test_target_start_failure_owns_new_empty_session);
  RUN_TEST(test_runtime_clear_shuts_down_ble_before_reuse);
  RUN_TEST(test_save_and_restore_drop_volatile_ble_state);
  return UNITY_END();
}
