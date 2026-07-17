/* Focused proof for the deterministic host BLE platform contract. */

#include "base_defs.h"
#include "base_image.h"
#include "handle.h"
#include "object.h"
#include "persist.h"
#include "platform.h"
#include "repl.h"
#include "runtime.h"
#include "tagged.h"

#include "unity/unity.h"

#include <string.h>

#if !FR_FEATURE_BLE || !FR_BLE_ENABLE_OBSERVER ||                         \
    !FR_BLE_ENABLE_BROADCASTER || !FR_BLE_ENABLE_CENTRAL ||              \
    !FR_BLE_ENABLE_PERIPHERAL
#error "test_ble requires all four BLE role gates"
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
      "print BLE roles, radio, scan, advertising, connections, queue pressure, "
      "and last raw reason",
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

static void test_advertising_words_validate_and_gate_scan(void) {
  static const uint8_t advertising_data[] = {2, 0x01, 0x06};
  static const uint8_t scan_response_data[] = {7, 0x09, 'F', 'r',
                                                'o', 't',  'h', 'y'};
  static const uint8_t malformed_data[] = {3, 0x09, 'x'};
  const fr_base_def_t *start = NULL;
  const fr_base_def_t *stop = NULL;
  fr_object_id_t advertising_id = 0;
  fr_object_id_t scan_response_id = 0;
  fr_object_id_t malformed_id = 0;
  fr_tagged_t args[4] = {0};
  fr_tagged_t result = fr_tagged_nil();
  fr_ble_status_t status;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  read_native_def("ble.advertise.start", FR_SLOT_BLE_ADVERTISE_START, 4,
                  &start);
  read_native_def("ble.advertise.stop", FR_SLOT_BLE_ADVERTISE_STOP, 0,
                  &stop);
#if FR_FEATURE_NATIVE_SIGNATURES
  TEST_ASSERT_EQUAL(FR_NATIVE_VALUE_TEXT_OR_BYTES,
                    start->native_signature->params[0].type);
  TEST_ASSERT_EQUAL(FR_NATIVE_VALUE_TEXT_OR_BYTES,
                    start->native_signature->params[1].type);
  TEST_ASSERT_EQUAL_STRING("start legacy BLE advertising with raw AD payloads",
                           start->native_signature->help);
#endif

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_text_install(&s_runtime, advertising_data,
                                    sizeof(advertising_data), &advertising_id));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_text_install(&s_runtime, scan_response_data,
                                    sizeof(scan_response_data),
                                    &scan_response_id));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_text_install(&s_runtime, malformed_data,
                                    sizeof(malformed_data), &malformed_id));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_tagged_encode_object_id(advertising_id, &args[0]));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_tagged_encode_object_id(scan_response_id, &args[1]));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(100, &args[2]));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(1, &args[3]));

  TEST_ASSERT_EQUAL(FR_ERR_BLE_NOT_READY,
                    start->native_fn(&s_runtime, args, 4, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_tagged_encode_object_id(malformed_id, &args[0]));
  TEST_ASSERT_EQUAL(FR_ERR_INVALID,
                    start->native_fn(&s_runtime, args, 4, &result));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_tagged_encode_object_id(advertising_id, &args[0]));
  TEST_ASSERT_EQUAL(FR_OK, start->native_fn(&s_runtime, args, 4, &result));
  TEST_ASSERT_TRUE(fr_tagged_is_nil(result));

  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_ADVERTISE_ACTIVE, status.advertise_state);
  TEST_ASSERT_EQUAL_UINT16(100, status.advertise_requested_interval_ms);
  TEST_ASSERT_EQUAL_UINT32(100000, status.advertise_actual_interval_us);
  TEST_ASSERT_TRUE(status.advertise_connectable);
  TEST_ASSERT_EQUAL_UINT8(sizeof(advertising_data),
                          status.advertising_data_length);
  TEST_ASSERT_EQUAL_UINT8(sizeof(scan_response_data),
                          status.scan_response_data_length);
  TEST_ASSERT_EQUAL_UINT32(1, status.advertise_starts);
  TEST_ASSERT_EQUAL(FR_ERR_BLE_BUSY,
                    fr_platform_ble_scan_start(100, 50, false, false, -90));
  TEST_ASSERT_EQUAL(FR_ERR_BLE_BUSY,
                    start->native_fn(&s_runtime, args, 4, &result));

  TEST_ASSERT_EQUAL(FR_OK,
                    stop->native_fn(&s_runtime, NULL, 0, &result));
  TEST_ASSERT_EQUAL(FR_BLE_ADVERTISE_IDLE, read_status().advertise_state);
  TEST_ASSERT_EQUAL_UINT32(1, read_status().advertise_stops);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_ble_scan_start(100, 50, false, false, -90));
  TEST_ASSERT_EQUAL(FR_ERR_BLE_BUSY,
                    start->native_fn(&s_runtime, args, 4, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_scan_stop(&s_runtime));

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_project_clear());
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_ADVERTISE_IDLE, status.advertise_state);
  TEST_ASSERT_EQUAL_UINT16(0, status.advertise_requested_interval_ms);
  TEST_ASSERT_EQUAL_UINT32(0, status.advertise_starts);
  TEST_ASSERT_EQUAL_UINT32(0, status.advertise_stops);
}

static fr_tagged_t install_peer(uint8_t address_type, uint8_t id) {
  uint8_t peer[7] = {address_type, 0x10, 0x20, 0x30,
                     0x40,         0x50, id};
  fr_tagged_t tagged = fr_tagged_nil();

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_bytes_install(&s_runtime, peer, sizeof(peer), &tagged));
  return tagged;
}

static void test_central_connection_owns_one_inspectable_handle(void) {
  const fr_base_def_t *connect = NULL;
  const fr_base_def_t *ready = NULL;
  const fr_base_def_t *close = NULL;
  const fr_base_def_t *info = NULL;
  const fr_base_def_t *rssi = NULL;
  const fr_base_def_t *params = NULL;
  const fr_base_def_t *mtu = NULL;
  fr_tagged_t connect_args[2] = {0};
  fr_tagged_t one_arg[1] = {0};
  fr_tagged_t params_args[5] = {0};
  fr_tagged_t mtu_args[3] = {0};
  fr_tagged_t connection = fr_tagged_nil();
  fr_tagged_t result = fr_tagged_nil();
  fr_handle_ref_t connection_ref = {0};
  fr_ble_connection_info_t connection_info = {0};
  fr_ble_status_t status;
  uint16_t platform_index = FR_HANDLE_PLATFORM_NONE;
  fr_int_t integer = 0;
  bool is_ready = false;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  read_native_def("ble.connect", FR_SLOT_BLE_CONNECT, 2, &connect);
  read_native_def("ble.connection.ready?", FR_SLOT_BLE_CONNECTION_READY, 1,
                  &ready);
  read_native_def("ble.connection.close", FR_SLOT_BLE_CONNECTION_CLOSE, 1,
                  &close);
  read_native_def("ble.connection.info", FR_SLOT_BLE_CONNECTION_INFO, 1,
                  &info);
  read_native_def("ble.connection.rssi", FR_SLOT_BLE_CONNECTION_RSSI, 1,
                  &rssi);
  read_native_def("ble.connection.params", FR_SLOT_BLE_CONNECTION_PARAMS, 5,
                  &params);
  read_native_def("ble.connection.mtu", FR_SLOT_BLE_CONNECTION_MTU, 3, &mtu);
#if FR_FEATURE_NATIVE_SIGNATURES
  TEST_ASSERT_EQUAL(FR_NATIVE_VALUE_HANDLE, connect->native_signature->result);
  TEST_ASSERT_EQUAL_STRING("peer", connect->native_signature->params[0].name);
  TEST_ASSERT_EQUAL_STRING("timeout_ms",
                           connect->native_signature->params[1].name);
  TEST_ASSERT_EQUAL_STRING("say whether a BLE connection is live",
                           ready->native_signature->help);
  TEST_ASSERT_EQUAL_STRING(
      "print peer, link parameters, security state, and raw reason",
      info->native_signature->help);
#endif

  connect_args[0] = install_peer(FR_BLE_ADDRESS_PUBLIC, 0x60);
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(1000, &connect_args[1]));
  TEST_ASSERT_EQUAL(FR_ERR_BLE_NOT_READY,
                    connect->native_fn(&s_runtime, connect_args, 2, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));
  TEST_ASSERT_EQUAL(
      FR_OK, fr_platform_ble_scan_start(100, 50, false, false, -90));
  TEST_ASSERT_EQUAL(FR_ERR_BLE_BUSY,
                    connect->native_fn(&s_runtime, connect_args, 2, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_scan_stop(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    connect->native_fn(&s_runtime, connect_args, 2,
                                       &connection));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_tagged_decode_handle_ref(connection, &connection_ref));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_handle_lookup(&s_runtime, connection_ref,
                                     FR_HANDLE_KIND_BLE_CONNECTION, NULL,
                                     &platform_index));
  TEST_ASSERT_EQUAL_UINT16(0, platform_index);

  status = read_status();
  TEST_ASSERT_EQUAL_UINT8(1, status.connection_count);
  TEST_ASSERT_EQUAL_UINT32(1, status.connection_connects);
  TEST_ASSERT_EQUAL(FR_BLE_OP_CONNECT, status.last_operation);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_ble_connection_info(platform_index,
                                                    &connection_info));
  TEST_ASSERT_EQUAL(FR_BLE_CONNECTION_LIVE, connection_info.state);
  TEST_ASSERT_EQUAL(FR_BLE_CONNECTION_ROLE_CENTRAL, connection_info.role);
  TEST_ASSERT_EQUAL_UINT8(0x60, connection_info.peer_address[5]);
  TEST_ASSERT_EQUAL_UINT32(30000, connection_info.interval_us);
  TEST_ASSERT_EQUAL_UINT16(23, connection_info.mtu);

  one_arg[0] = connection;
  TEST_ASSERT_EQUAL(FR_OK,
                    ready->native_fn(&s_runtime, one_arg, 1, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_bool(result, &is_ready));
  TEST_ASSERT_TRUE(is_ready);
  TEST_ASSERT_EQUAL(FR_OK,
                    rssi->native_fn(&s_runtime, one_arg, 1, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(result, &integer));
  TEST_ASSERT_EQUAL_INT(-42, integer);

  params_args[0] = connection;
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(15, &params_args[1]));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(30, &params_args[2]));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(0, &params_args[3]));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(4000, &params_args[4]));
  TEST_ASSERT_EQUAL(FR_OK,
                    params->native_fn(&s_runtime, params_args, 5, &result));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_ble_connection_info(platform_index,
                                                    &connection_info));
  TEST_ASSERT_EQUAL_UINT32(30000, connection_info.interval_us);
  TEST_ASSERT_EQUAL_UINT32(4000000, connection_info.supervision_timeout_us);

  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(4000, &params_args[1]));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(4000, &params_args[2]));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(499, &params_args[3]));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(100, &params_args[4]));
  TEST_ASSERT_EQUAL(FR_ERR_RANGE,
                    params->native_fn(&s_runtime, params_args, 5, &result));

  mtu_args[0] = connection;
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(24, &mtu_args[1]));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(1000, &mtu_args[2]));
  TEST_ASSERT_EQUAL(FR_ERR_RANGE,
                    mtu->native_fn(&s_runtime, mtu_args, 3, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(23, &mtu_args[1]));
  TEST_ASSERT_EQUAL(FR_OK,
                    mtu->native_fn(&s_runtime, mtu_args, 3, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_int(result, &integer));
  TEST_ASSERT_EQUAL_INT(23, integer);

  TEST_ASSERT_EQUAL(FR_ERR_CAPACITY,
                    connect->native_fn(&s_runtime, connect_args, 2, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_disconnect(platform_index, 19));
  TEST_ASSERT_EQUAL(FR_OK,
                    ready->native_fn(&s_runtime, one_arg, 1, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_decode_bool(result, &is_ready));
  TEST_ASSERT_FALSE(is_ready);
  TEST_ASSERT_EQUAL(FR_ERR_BLE_DISCONNECTED,
                    rssi->native_fn(&s_runtime, one_arg, 1, &result));
  TEST_ASSERT_EQUAL(FR_ERR_BLE_DISCONNECTED,
                    params->native_fn(&s_runtime, params_args, 5, &result));
  TEST_ASSERT_EQUAL(FR_ERR_BLE_DISCONNECTED,
                    mtu->native_fn(&s_runtime, mtu_args, 3, &result));
  TEST_ASSERT_EQUAL(FR_OK,
                    info->native_fn(&s_runtime, one_arg, 1, &result));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_ble_connection_info(platform_index,
                                                    &connection_info));
  TEST_ASSERT_EQUAL(FR_BLE_CONNECTION_DISCONNECTED, connection_info.state);
  TEST_ASSERT_EQUAL_INT32(19, connection_info.last_reason);

  TEST_ASSERT_EQUAL(FR_OK,
                    close->native_fn(&s_runtime, one_arg, 1, &result));
  TEST_ASSERT_EQUAL(FR_ERR_HANDLE,
                    ready->native_fn(&s_runtime, one_arg, 1, &result));
  status = read_status();
  TEST_ASSERT_EQUAL_UINT8(0, status.connection_count);
  TEST_ASSERT_EQUAL_UINT32(1, status.connection_disconnects);
  TEST_ASSERT_EQUAL(FR_BLE_OP_CONNECTION_CLOSE, status.last_operation);
}

static void test_peripheral_accept_drains_stale_notices_and_ble_off(void) {
  static const uint8_t advertising_data[] = {2, 0x01, 0x06};
  const uint8_t peer[7] = {FR_BLE_ADDRESS_RANDOM, 0xA1, 0xA2, 0xA3,
                           0xA4,                  0xA5, 0xA6};
  const fr_base_def_t *accept = NULL;
  const fr_base_def_t *ready = NULL;
  const fr_base_def_t *off = NULL;
  fr_tagged_t connection = fr_tagged_nil();
  fr_tagged_t result = fr_tagged_nil();
  fr_tagged_t one_arg[1] = {0};
  fr_tagged_t uart = fr_tagged_nil();
  fr_handle_ref_t connection_ref = {0};
  fr_handle_ref_t uart_ref = {0};
  uint16_t connection_index = FR_HANDLE_PLATFORM_NONE;
  uint16_t uart_index = FR_HANDLE_PLATFORM_NONE;
  fr_ble_status_t status;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  read_native_def("ble.accept", FR_SLOT_BLE_ACCEPT, 0, &accept);
  read_native_def("ble.connection.ready?", FR_SLOT_BLE_CONNECTION_READY, 1,
                  &ready);
  read_native_def("ble.off", FR_SLOT_BLE_OFF, 0, &off);
#if FR_FEATURE_NATIVE_SIGNATURES
  TEST_ASSERT_EQUAL(FR_NATIVE_VALUE_ANY, accept->native_signature->result);
  TEST_ASSERT_EQUAL_STRING("accept one pending BLE connection or return nil",
                           accept->native_signature->help);
#endif

  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));
  TEST_ASSERT_EQUAL(FR_OK,
                    accept->native_fn(&s_runtime, NULL, 0, &result));
  TEST_ASSERT_TRUE(fr_tagged_is_nil(result));
  TEST_ASSERT_EQUAL(FR_BLE_OP_ACCEPT, read_status().last_operation);
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_ble_advertise_start(
                        advertising_data, sizeof(advertising_data), NULL, 0,
                        100, true));
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_queue_incoming(peer));
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_ADVERTISE_IDLE, status.advertise_state);
  TEST_ASSERT_EQUAL_UINT8(1, status.connection_count);
  TEST_ASSERT_EQUAL_UINT8(1, status.pending_connection_count);
  TEST_ASSERT_EQUAL_UINT8(1, status.connection_notice_count);

  TEST_ASSERT_EQUAL(FR_OK,
                    accept->native_fn(&s_runtime, NULL, 0, &connection));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_tagged_decode_handle_ref(connection, &connection_ref));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_handle_lookup(&s_runtime, connection_ref,
                                     FR_HANDLE_KIND_BLE_CONNECTION, NULL,
                                     &connection_index));
  status = read_status();
  TEST_ASSERT_EQUAL_UINT8(0, status.pending_connection_count);
  TEST_ASSERT_EQUAL_UINT8(0, status.connection_notice_count);
  TEST_ASSERT_EQUAL_UINT32(1, status.connection_accepts);

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_ble_advertise_start(
                        advertising_data, sizeof(advertising_data), NULL, 0,
                        100, true));
  TEST_ASSERT_EQUAL(FR_ERR_CAPACITY, fr_host_ble_queue_incoming(peer));
  TEST_ASSERT_EQUAL_UINT32(1, read_status().incoming_rejected);
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_disconnect(connection_index, 8));
  one_arg[0] = connection;
  TEST_ASSERT_EQUAL(FR_OK,
                    ready->native_fn(&s_runtime, one_arg, 1, &result));
  TEST_ASSERT_EQUAL(FR_OK, fr_handle_close(&s_runtime, connection_ref));

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_ble_advertise_start(
                        advertising_data, sizeof(advertising_data), NULL, 0,
                        100, true));
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_queue_incoming(peer));
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_disconnect(0, 22));
  TEST_ASSERT_EQUAL_UINT8(1, read_status().connection_notice_count);
  TEST_ASSERT_EQUAL(FR_OK,
                    accept->native_fn(&s_runtime, NULL, 0, &result));
  TEST_ASSERT_TRUE(fr_tagged_is_nil(result));
  TEST_ASSERT_EQUAL_UINT8(0, read_status().connection_notice_count);

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_platform_ble_advertise_start(
                        advertising_data, sizeof(advertising_data), NULL, 0,
                        100, true));
  TEST_ASSERT_EQUAL(FR_OK, fr_host_ble_queue_incoming(peer));
  TEST_ASSERT_EQUAL(FR_OK,
                    accept->native_fn(&s_runtime, NULL, 0, &connection));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_tagged_decode_handle_ref(connection, &connection_ref));

  TEST_ASSERT_EQUAL(FR_OK,
                    fr_handle_reserve(&s_runtime, FR_HANDLE_KIND_UART,
                                      &uart_ref, &uart));
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_uart_open(0, 115200, &uart_index));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_handle_activate(&s_runtime, uart_ref, uart_index));
  TEST_ASSERT_EQUAL(FR_OK, off->native_fn(&s_runtime, NULL, 0, &result));
  TEST_ASSERT_EQUAL(FR_ERR_HANDLE,
                    fr_handle_lookup(&s_runtime, connection_ref,
                                     FR_HANDLE_KIND_BLE_CONNECTION, NULL,
                                     NULL));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_handle_lookup(&s_runtime, uart_ref, FR_HANDLE_KIND_UART,
                                     NULL, NULL));
  TEST_ASSERT_EQUAL(FR_OK, fr_handle_close(&s_runtime, uart_ref));
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_RADIO_OFF, status.radio_state);
  TEST_ASSERT_EQUAL_UINT8(0, status.connection_count);
  TEST_ASSERT_EQUAL_UINT8(0, status.connection_notice_count);
  TEST_ASSERT_EQUAL(FR_BLE_OP_OFF, status.last_operation);
}

static void test_radio_lifecycle_and_status(void) {
  static const uint8_t own_address[6] = {0xaa, 0xbb, 0xcc,
                                         0xdd, 0xee, 0xff};
  fr_ble_status_t status = read_status();

  TEST_ASSERT_EQUAL_STRING("host-fixture", fr_platform_ble_backend_name());
  TEST_ASSERT_EQUAL(FR_BLE_RADIO_OFF, status.radio_state);
  TEST_ASSERT_EQUAL(FR_BLE_SCAN_IDLE, status.scan_state);
  TEST_ASSERT_EQUAL_UINT8(FR_BLE_ROLE_OBSERVER | FR_BLE_ROLE_BROADCASTER |
                              FR_BLE_ROLE_CENTRAL | FR_BLE_ROLE_PERIPHERAL,
                          status.roles);
  TEST_ASSERT_EQUAL_UINT8(8, status.queue_capacity);
  TEST_ASSERT_EQUAL_UINT8(1, status.connection_capacity);
  TEST_ASSERT_EQUAL_UINT8(4, status.connection_notice_capacity);
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

static void test_runtime_clear_closes_connection_handle_before_radio(void) {
  const fr_base_def_t *connect = NULL;
  fr_tagged_t args[2] = {0};
  fr_tagged_t connection = fr_tagged_nil();
  fr_handle_ref_t connection_ref = {0};
  fr_ble_status_t status;

  TEST_ASSERT_EQUAL(FR_OK, fr_base_image_install(&s_runtime));
  read_native_def("ble.connect", FR_SLOT_BLE_CONNECT, 2, &connect);
  TEST_ASSERT_EQUAL(FR_OK, fr_platform_ble_on(&s_runtime));
  args[0] = install_peer(FR_BLE_ADDRESS_PUBLIC, 0x77);
  TEST_ASSERT_EQUAL(FR_OK, fr_tagged_encode_int(1000, &args[1]));
  TEST_ASSERT_EQUAL(
      FR_OK, connect->native_fn(&s_runtime, args, 2, &connection));
  TEST_ASSERT_EQUAL(FR_OK,
                    fr_tagged_decode_handle_ref(connection, &connection_ref));
  TEST_ASSERT_EQUAL_UINT8(1, read_status().connection_count);

  TEST_ASSERT_EQUAL(FR_OK, fr_runtime_clear_project(&s_runtime));
  TEST_ASSERT_EQUAL(FR_ERR_HANDLE,
                    fr_handle_lookup(&s_runtime, connection_ref,
                                     FR_HANDLE_KIND_BLE_CONNECTION, NULL,
                                     NULL));
  status = read_status();
  TEST_ASSERT_EQUAL(FR_BLE_RADIO_OFF, status.radio_state);
  TEST_ASSERT_EQUAL_UINT8(0, status.connection_count);
  TEST_ASSERT_EQUAL_UINT8(0, status.pending_connection_count);
  TEST_ASSERT_EQUAL_UINT8(0, status.connection_notice_count);
  TEST_ASSERT_EQUAL_UINT32(0, status.connection_connects);
  TEST_ASSERT_EQUAL_UINT32(0, status.connection_disconnects);
  TEST_ASSERT_EQUAL(FR_BLE_OP_NONE, status.last_operation);
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
  RUN_TEST(test_advertising_words_validate_and_gate_scan);
  RUN_TEST(test_central_connection_owns_one_inspectable_handle);
  RUN_TEST(test_peripheral_accept_drains_stale_notices_and_ble_off);
  RUN_TEST(test_radio_lifecycle_and_status);
  RUN_TEST(test_scan_parameters_and_state_gates);
  RUN_TEST(test_queue_conservation_overflow_and_cursor);
  RUN_TEST(test_failures_timeouts_reset_and_clear);
  RUN_TEST(test_target_start_failure_owns_new_empty_session);
  RUN_TEST(test_runtime_clear_shuts_down_ble_before_reuse);
  RUN_TEST(test_runtime_clear_closes_connection_handle_before_radio);
  RUN_TEST(test_save_and_restore_drop_volatile_ble_state);
  return UNITY_END();
}
