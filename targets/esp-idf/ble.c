#include "config.h"

#if FR_FEATURE_BLE

#include "platform.h"
#include "runtime.h"

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "sdkconfig.h"

#include <stdint.h>
#include <string.h>

#if !CONFIG_BT_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
#error "Frothy BLE requires ESP-IDF NimBLE"
#endif

#if FR_BLE_ENABLE_OBSERVER != CONFIG_BT_NIMBLE_ROLE_OBSERVER
#error "Frothy and NimBLE observer roles disagree"
#endif

#if FR_BLE_ENABLE_BROADCASTER != CONFIG_BT_NIMBLE_ROLE_BROADCASTER
#error "Frothy and NimBLE broadcaster roles disagree"
#endif

#if FR_BLE_ENABLE_CENTRAL != CONFIG_BT_NIMBLE_ROLE_CENTRAL
#error "Frothy and NimBLE central roles disagree"
#endif

#if FR_BLE_ENABLE_PERIPHERAL != CONFIG_BT_NIMBLE_ROLE_PERIPHERAL
#error "Frothy and NimBLE peripheral roles disagree"
#endif

#if (FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL) &&                  \
    FR_BLE_CONNECTION_COUNT != CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#error "Frothy and NimBLE connection counts disagree"
#endif

#if (FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL) &&                  \
    CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU != 23
#error "the first Frothy connection profile requires ATT MTU 23"
#endif

#if (FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL) &&                  \
    (!CONFIG_BT_NIMBLE_GATT_CLIENT || CONFIG_BT_NIMBLE_GATT_MAX_PROCS < 1)
#error "Frothy connection MTU exchange requires one GATT client procedure"
#endif

enum {
  FR_ESP_BLE_WAIT_TICKS = 1,
  FR_ESP_BLE_CLEANUP_STACK_BYTES = 3072,
  FR_ESP_BLE_CLEANUP_PRIORITY = tskIDLE_PRIORITY + 2,
  FR_ESP_BLE_SCAN_INTERVAL_MIN_MS = 3,
  FR_ESP_BLE_SCAN_INTERVAL_MAX_MS = 10240,
  FR_ESP_BLE_ADVERTISE_INTERVAL_MIN_MS = 20,
  FR_ESP_BLE_ADVERTISE_INTERVAL_MAX_MS = 10240,
  FR_ESP_BLE_RSSI_MIN = -127,
  FR_ESP_BLE_RSSI_MAX = 20,
#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
  FR_ESP_BLE_CONNECTION_INDEX = 0,
  FR_ESP_BLE_CONNECTION_INTERVAL_UNIT_US = 1250,
  FR_ESP_BLE_SUPERVISION_TIMEOUT_UNIT_US = 10000,
  FR_ESP_BLE_CONNECTION_INTERVAL_MIN_UNITS = 6,
  FR_ESP_BLE_CONNECTION_INTERVAL_MAX_UNITS = 3200,
  FR_ESP_BLE_SUPERVISION_TIMEOUT_MIN_UNITS = 10,
  FR_ESP_BLE_SUPERVISION_TIMEOUT_MAX_UNITS = 3200,
#endif
};

#if FR_BLE_ENABLE_OBSERVER
typedef struct fr_esp_ble_scan_t {
  fr_ble_scan_state_t state;
  uint32_t generation;

  uint16_t requested_interval_ms;
  uint16_t requested_window_ms;
  uint32_t actual_interval_us;
  uint32_t actual_window_us;
  int8_t minimum_rssi;
  bool active;
  bool repeats;

  fr_ble_scan_report_t queue[FR_BLE_SCAN_QUEUE_COUNT];
  uint8_t head;
  uint8_t count;
  uint8_t high_water;
  uint32_t received;
  uint32_t accepted;
  uint32_t filtered_rssi;
  uint32_t dequeued;
  uint32_t dropped;
  uint32_t malformed;

  bool current_valid;
  fr_ble_scan_report_t current;
} fr_esp_ble_scan_t;
#endif

#if FR_BLE_ENABLE_BROADCASTER
typedef struct fr_esp_ble_advertise_t {
  fr_ble_advertise_state_t state;
  uint32_t generation;

  uint16_t requested_interval_ms;
  uint32_t actual_interval_us;
  bool connectable;
  uint8_t advertising_data_length;
  uint8_t advertising_data[FR_BLE_ADVERTISEMENT_DATA_BYTES];
  uint8_t scan_response_data_length;
  uint8_t scan_response_data[FR_BLE_ADVERTISEMENT_DATA_BYTES];
  uint32_t starts;
  uint32_t stops;
} fr_esp_ble_advertise_t;
#endif

#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
typedef struct fr_esp_ble_connection_t {
  fr_ble_connection_state_t state;
  fr_ble_connection_role_t role;
  uint32_t generation;
  uint16_t stack_handle;

  fr_handle_ref_t runtime_ref;
  bool has_runtime_ref;

  fr_ble_address_type_t peer_address_type;
  uint8_t peer_address[6];

  uint16_t interval_units;
  uint16_t latency;
  uint16_t supervision_timeout_units;
  uint16_t mtu;

  uint16_t requested_minimum_interval_ms;
  uint16_t requested_maximum_interval_ms;
  uint16_t requested_latency;
  uint16_t requested_supervision_timeout_ms;

  bool encrypted;
  bool authenticated;
  bool bonded;
  bool rssi_valid;
  int8_t last_rssi;
  int32_t last_reason;
  uint32_t connected_at_ms;
  uint32_t disconnected_at_ms;

  bool connect_complete;
  int32_t connect_status;
  bool params_pending;
  bool mtu_pending;
  int32_t mtu_status;
} fr_esp_ble_connection_t;

#if FR_BLE_ENABLE_PERIPHERAL
typedef struct fr_esp_ble_connection_notice_t {
  uint16_t platform_index;
  uint32_t connection_generation;
} fr_esp_ble_connection_notice_t;
#endif
#endif

typedef struct fr_esp_ble_state_t {
  fr_ble_radio_state_t radio_state;
  uint32_t lifecycle_generation;
  uint32_t completion_generation;
  bool port_initialized;
  bool host_task_created;
  bool host_task_running;
  bool shutdown_in_progress;
  bool cleanup_required;
  uint32_t late_callback_count;

  fr_ble_address_type_t own_address_type;
  uint8_t own_address_raw_type;
  uint8_t own_address[6];
  bool own_address_valid;

#if FR_BLE_ENABLE_OBSERVER
  fr_esp_ble_scan_t scan;
#endif
#if FR_BLE_ENABLE_BROADCASTER
  fr_esp_ble_advertise_t advertise;
#endif
#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
  fr_esp_ble_connection_t connection;
  uint32_t connection_connects;
  uint32_t connection_accepts;
  uint32_t connection_disconnects;
  uint32_t incoming_rejected;
#endif
#if FR_BLE_ENABLE_PERIPHERAL
  fr_esp_ble_connection_notice_t
      connection_notices[FR_BLE_CONNECTION_NOTICE_COUNT];
  uint8_t connection_notice_head;
  uint8_t connection_notice_count;
#endif

  fr_ble_operation_t last_operation;
  fr_err_t last_result;
  int32_t last_platform_code;
  int32_t last_protocol_reason;
  uint32_t last_operation_ms;
  uint32_t reset_count;
} fr_esp_ble_state_t;

static portMUX_TYPE fr_esp_ble_lock = portMUX_INITIALIZER_UNLOCKED;
static fr_esp_ble_state_t fr_esp_ble;
static struct ble_npl_event fr_esp_ble_cleanup_ready_event;

static uint32_t fr_esp_ble_now_ms(void) {
  return (uint32_t)((uint64_t)esp_timer_get_time() / 1000u);
}

static uint8_t fr_esp_ble_roles(void) {
  uint8_t roles = 0;

#if FR_BLE_ENABLE_OBSERVER
  roles |= FR_BLE_ROLE_OBSERVER;
#endif
#if FR_BLE_ENABLE_BROADCASTER
  roles |= FR_BLE_ROLE_BROADCASTER;
#endif
#if FR_BLE_ENABLE_CENTRAL
  roles |= FR_BLE_ROLE_CENTRAL;
#endif
#if FR_BLE_ENABLE_PERIPHERAL
  roles |= FR_BLE_ROLE_PERIPHERAL;
#endif
  return roles;
}

static void fr_esp_ble_record_locked(fr_ble_operation_t operation,
                                     fr_err_t result, int32_t platform_code) {
  fr_esp_ble.last_operation = operation;
  fr_esp_ble.last_result = result;
  fr_esp_ble.last_platform_code = platform_code;
  fr_esp_ble.last_operation_ms = fr_esp_ble_now_ms();
}

#if FR_BLE_ENABLE_OBSERVER
static void fr_esp_ble_clear_reports_locked(void) {
  memset(fr_esp_ble.scan.queue, 0, sizeof(fr_esp_ble.scan.queue));
  fr_esp_ble.scan.head = 0;
  fr_esp_ble.scan.count = 0;
  fr_esp_ble.scan.high_water = 0;
  fr_esp_ble.scan.received = 0;
  fr_esp_ble.scan.accepted = 0;
  fr_esp_ble.scan.filtered_rssi = 0;
  fr_esp_ble.scan.dequeued = 0;
  fr_esp_ble.scan.dropped = 0;
  fr_esp_ble.scan.malformed = 0;
  fr_esp_ble.scan.current_valid = false;
  memset(&fr_esp_ble.scan.current, 0, sizeof(fr_esp_ble.scan.current));
}

static void fr_esp_ble_clear_scan_parameters_locked(void) {
  fr_esp_ble.scan.requested_interval_ms = 0;
  fr_esp_ble.scan.requested_window_ms = 0;
  fr_esp_ble.scan.actual_interval_us = 0;
  fr_esp_ble.scan.actual_window_us = 0;
  fr_esp_ble.scan.minimum_rssi = 0;
  fr_esp_ble.scan.active = false;
  fr_esp_ble.scan.repeats = false;
}

static void fr_esp_ble_invalidate_scan_locked(bool clear_parameters) {
  if (fr_esp_ble.scan.state != FR_BLE_SCAN_IDLE ||
      fr_esp_ble.scan.count > 0 || fr_esp_ble.scan.current_valid) {
    fr_esp_ble.scan.generation += 1u;
  }
  fr_esp_ble.scan.state = FR_BLE_SCAN_IDLE;
  fr_esp_ble_clear_reports_locked();
  if (clear_parameters) {
    fr_esp_ble_clear_scan_parameters_locked();
  }
}

static void fr_esp_ble_abandon_scan_locked(void) {
  if (fr_esp_ble.scan.state != FR_BLE_SCAN_IDLE ||
      fr_esp_ble.scan.count > 0 || fr_esp_ble.scan.current_valid) {
    fr_esp_ble.scan.generation += 1u;
  }
  fr_esp_ble.scan.dropped += fr_esp_ble.scan.count;
  memset(fr_esp_ble.scan.queue, 0, sizeof(fr_esp_ble.scan.queue));
  fr_esp_ble.scan.head = 0;
  fr_esp_ble.scan.count = 0;
  fr_esp_ble.scan.state = FR_BLE_SCAN_IDLE;
  fr_esp_ble.scan.current_valid = false;
  memset(&fr_esp_ble.scan.current, 0, sizeof(fr_esp_ble.scan.current));
}
#endif

#if FR_BLE_ENABLE_BROADCASTER
static void fr_esp_ble_clear_advertise_locked(void) {
  fr_esp_ble.advertise.requested_interval_ms = 0;
  fr_esp_ble.advertise.actual_interval_us = 0;
  fr_esp_ble.advertise.connectable = false;
  fr_esp_ble.advertise.advertising_data_length = 0;
  memset(fr_esp_ble.advertise.advertising_data, 0,
         sizeof(fr_esp_ble.advertise.advertising_data));
  fr_esp_ble.advertise.scan_response_data_length = 0;
  memset(fr_esp_ble.advertise.scan_response_data, 0,
         sizeof(fr_esp_ble.advertise.scan_response_data));
}

static void fr_esp_ble_abandon_advertise_locked(void) {
  if (fr_esp_ble.advertise.state != FR_BLE_ADVERTISE_IDLE) {
    fr_esp_ble.advertise.generation += 1u;
  }
  fr_esp_ble.advertise.state = FR_BLE_ADVERTISE_IDLE;
}

static void fr_esp_ble_clear_advertise_all_locked(void) {
  fr_esp_ble_abandon_advertise_locked();
  fr_esp_ble_clear_advertise_locked();
  fr_esp_ble.advertise.starts = 0;
  fr_esp_ble.advertise.stops = 0;
}
#endif

static void fr_esp_ble_mark_off_locked(void) {
  fr_esp_ble.radio_state = FR_BLE_RADIO_OFF;
  fr_esp_ble.completion_generation = 0;
  fr_esp_ble.port_initialized = false;
  fr_esp_ble.host_task_created = false;
  fr_esp_ble.host_task_running = false;
  fr_esp_ble.shutdown_in_progress = false;
  fr_esp_ble.cleanup_required = false;
  fr_esp_ble.own_address_valid = false;
  fr_esp_ble.own_address_raw_type = 0;
  memset(fr_esp_ble.own_address, 0, sizeof(fr_esp_ble.own_address));
}

static bool fr_esp_ble_address_type(uint8_t raw_type,
                                    fr_ble_address_type_t *out_type) {
  switch (raw_type) {
  case BLE_ADDR_PUBLIC:
    *out_type = FR_BLE_ADDRESS_PUBLIC;
    return true;
  case BLE_ADDR_RANDOM:
    *out_type = FR_BLE_ADDRESS_RANDOM;
    return true;
  case BLE_ADDR_PUBLIC_ID:
    *out_type = FR_BLE_ADDRESS_PUBLIC_ID;
    return true;
  case BLE_ADDR_RANDOM_ID:
    *out_type = FR_BLE_ADDRESS_RANDOM_ID;
    return true;
  default:
    return false;
  }
}

#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
static bool fr_esp_ble_raw_address(const uint8_t peer[7],
                                   ble_addr_t *out_address) {
  uint8_t raw_type = 0;

  if (peer == NULL || out_address == NULL) {
    return false;
  }
  switch ((fr_ble_address_type_t)peer[0]) {
  case FR_BLE_ADDRESS_PUBLIC:
    raw_type = BLE_ADDR_PUBLIC;
    break;
  case FR_BLE_ADDRESS_RANDOM:
    raw_type = BLE_ADDR_RANDOM;
    break;
  case FR_BLE_ADDRESS_PUBLIC_ID:
    raw_type = BLE_ADDR_PUBLIC_ID;
    break;
  case FR_BLE_ADDRESS_RANDOM_ID:
    raw_type = BLE_ADDR_RANDOM_ID;
    break;
  default:
    return false;
  }

  memset(out_address, 0, sizeof(*out_address));
  out_address->type = raw_type;
  for (uint8_t i = 0; i < sizeof(out_address->val); i++) {
    out_address->val[i] = peer[sizeof(out_address->val) - i];
  }
  return true;
}

static fr_err_t fr_esp_ble_host_error(int rc) {
  switch (rc) {
  case 0:
    return FR_OK;
  case BLE_HS_EBUSY:
  case BLE_HS_EALREADY:
    return FR_ERR_BLE_BUSY;
  case BLE_HS_ENOTCONN:
    return FR_ERR_BLE_DISCONNECTED;
  case BLE_HS_ETIMEOUT:
    return FR_ERR_BLE_TIMEOUT;
  case BLE_HS_ENOMEM:
    return FR_ERR_CAPACITY;
  case BLE_HS_EINVAL:
    return FR_ERR_RANGE;
  default:
    return FR_ERR_IO;
  }
}

static uint16_t fr_esp_ble_connection_interval_units(uint16_t milliseconds) {
  return (uint16_t)(((uint32_t)milliseconds * 4u + 2u) / 5u);
}

static uint16_t
fr_esp_ble_supervision_timeout_units(uint16_t milliseconds) {
  return (uint16_t)(((uint32_t)milliseconds + 5u) / 10u);
}

static void fr_esp_ble_connection_free_locked(void) {
  uint32_t generation = fr_esp_ble.connection.generation;

  memset(&fr_esp_ble.connection, 0, sizeof(fr_esp_ble.connection));
  fr_esp_ble.connection.generation = generation;
  fr_esp_ble.connection.state = FR_BLE_CONNECTION_FREE;
  fr_esp_ble.connection.stack_handle = BLE_HS_CONN_HANDLE_NONE;
}

static uint32_t fr_esp_ble_connection_reserve_locked(
    fr_ble_connection_role_t role, fr_handle_ref_t runtime_ref,
    bool has_runtime_ref) {
  uint32_t generation = fr_esp_ble.connection.generation + 1u;

  memset(&fr_esp_ble.connection, 0, sizeof(fr_esp_ble.connection));
  fr_esp_ble.connection.state = FR_BLE_CONNECTION_CONNECTING;
  fr_esp_ble.connection.role = role;
  fr_esp_ble.connection.generation = generation;
  fr_esp_ble.connection.stack_handle = BLE_HS_CONN_HANDLE_NONE;
  fr_esp_ble.connection.runtime_ref = runtime_ref;
  fr_esp_ble.connection.has_runtime_ref = has_runtime_ref;
  fr_esp_ble.connection.mtu = BLE_ATT_MTU_DFLT;
  return generation;
}

static bool fr_esp_ble_connection_copy_descriptor_locked(
    const struct ble_gap_conn_desc *descriptor, uint16_t mtu) {
  fr_ble_address_type_t address_type = FR_BLE_ADDRESS_PUBLIC;
  fr_ble_connection_role_t role = FR_BLE_CONNECTION_ROLE_CENTRAL;

  if (descriptor == NULL ||
      !fr_esp_ble_address_type(descriptor->peer_ota_addr.type,
                               &address_type)) {
    return false;
  }
  if (descriptor->role == BLE_GAP_ROLE_MASTER) {
    role = FR_BLE_CONNECTION_ROLE_CENTRAL;
  } else if (descriptor->role == BLE_GAP_ROLE_SLAVE) {
    role = FR_BLE_CONNECTION_ROLE_PERIPHERAL;
  } else {
    return false;
  }
  if (role != fr_esp_ble.connection.role) {
    return false;
  }

  fr_esp_ble.connection.stack_handle = descriptor->conn_handle;
  fr_esp_ble.connection.peer_address_type = address_type;
  for (uint8_t i = 0; i < sizeof(fr_esp_ble.connection.peer_address); i++) {
    fr_esp_ble.connection.peer_address[i] =
        descriptor->peer_ota_addr
            .val[sizeof(fr_esp_ble.connection.peer_address) - 1u - i];
  }
  fr_esp_ble.connection.interval_units = descriptor->conn_itvl;
  fr_esp_ble.connection.latency = descriptor->conn_latency;
  fr_esp_ble.connection.supervision_timeout_units =
      descriptor->supervision_timeout;
  fr_esp_ble.connection.mtu = mtu > 0 ? mtu : BLE_ATT_MTU_DFLT;
  fr_esp_ble.connection.encrypted = descriptor->sec_state.encrypted;
  fr_esp_ble.connection.authenticated = descriptor->sec_state.authenticated;
  fr_esp_ble.connection.bonded = descriptor->sec_state.bonded;
  return true;
}

static void fr_esp_ble_connection_mark_disconnected_locked(
    const struct ble_gap_conn_desc *descriptor, int32_t reason) {
  if (descriptor != NULL) {
    (void)fr_esp_ble_connection_copy_descriptor_locked(
        descriptor, fr_esp_ble.connection.mtu);
  }
  fr_esp_ble.connection.state = FR_BLE_CONNECTION_DISCONNECTED;
  fr_esp_ble.connection.stack_handle = BLE_HS_CONN_HANDLE_NONE;
  fr_esp_ble.connection.rssi_valid = false;
  fr_esp_ble.connection.last_reason = reason;
  fr_esp_ble.connection.disconnected_at_ms = fr_esp_ble_now_ms();
  fr_esp_ble.connection.params_pending = false;
  fr_esp_ble.connection.mtu_pending = false;
  fr_esp_ble.connection.mtu_status = BLE_HS_ENOTCONN;
  fr_esp_ble.connection_disconnects += 1u;
  fr_esp_ble.last_protocol_reason = reason;
}

#if FR_BLE_ENABLE_PERIPHERAL
static void fr_esp_ble_connection_notice_pop_locked(void) {
  if (fr_esp_ble.connection_notice_count == 0) {
    return;
  }
  fr_esp_ble.connection_notice_head =
      (uint8_t)((fr_esp_ble.connection_notice_head + 1u) %
                FR_BLE_CONNECTION_NOTICE_COUNT);
  fr_esp_ble.connection_notice_count -= 1u;
}

static bool fr_esp_ble_connection_notice_current_locked(void) {
  const fr_esp_ble_connection_notice_t *notice = NULL;

  if (fr_esp_ble.connection_notice_count == 0) {
    return false;
  }
  notice =
      &fr_esp_ble.connection_notices[fr_esp_ble.connection_notice_head];
  return notice->platform_index == FR_ESP_BLE_CONNECTION_INDEX &&
         notice->connection_generation == fr_esp_ble.connection.generation &&
         fr_esp_ble.connection.state == FR_BLE_CONNECTION_PENDING;
}

static bool fr_esp_ble_connection_notice_find_locked(void) {
  while (fr_esp_ble.connection_notice_count > 0 &&
         !fr_esp_ble_connection_notice_current_locked()) {
    fr_esp_ble_connection_notice_pop_locked();
  }
  return fr_esp_ble.connection_notice_count > 0;
}

static void fr_esp_ble_connection_notices_clear_locked(void) {
  memset(fr_esp_ble.connection_notices, 0,
         sizeof(fr_esp_ble.connection_notices));
  fr_esp_ble.connection_notice_head = 0;
  fr_esp_ble.connection_notice_count = 0;
}

static void fr_esp_ble_connection_notice_push_locked(uint32_t generation) {
  uint8_t tail =
      (uint8_t)((fr_esp_ble.connection_notice_head +
                 fr_esp_ble.connection_notice_count) %
                FR_BLE_CONNECTION_NOTICE_COUNT);

  fr_esp_ble.connection_notices[tail] =
      (fr_esp_ble_connection_notice_t){
          .platform_index = FR_ESP_BLE_CONNECTION_INDEX,
          .connection_generation = generation,
      };
  fr_esp_ble.connection_notice_count += 1u;
}
#endif

static void fr_esp_ble_connections_clear_locked(bool reset_counters) {
  if (fr_esp_ble.connection.state != FR_BLE_CONNECTION_FREE) {
    fr_esp_ble.connection.generation += 1u;
  }
  fr_esp_ble_connection_free_locked();
#if FR_BLE_ENABLE_PERIPHERAL
  fr_esp_ble_connection_notices_clear_locked();
#endif
  if (reset_counters) {
    fr_esp_ble.connection_connects = 0;
    fr_esp_ble.connection_accepts = 0;
    fr_esp_ble.connection_disconnects = 0;
    fr_esp_ble.incoming_rejected = 0;
  }
}
#endif

#if FR_BLE_ENABLE_OBSERVER || FR_BLE_ENABLE_BROADCASTER
static uint16_t fr_esp_ble_gap_units(uint16_t milliseconds) {
  /* Legacy GAP interval units are 625 us. Adding half a unit rounds to the
   * nearest supported value; an exact half cannot occur for integer ms. */
  return (uint16_t)(((uint32_t)milliseconds * 1000u + 312u) / 625u);
}
#endif

#if FR_BLE_ENABLE_OBSERVER
static bool fr_esp_ble_report_flags(uint8_t event_type, uint8_t *out_flags) {
  uint8_t flags = FR_BLE_REPORT_LEGACY;

  if (out_flags == NULL) {
    return false;
  }
  switch (event_type) {
  case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:
    flags |= FR_BLE_REPORT_CONNECTABLE | FR_BLE_REPORT_SCANNABLE;
    break;
  case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:
    flags |= FR_BLE_REPORT_CONNECTABLE | FR_BLE_REPORT_DIRECTED;
    break;
  case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:
    flags |= FR_BLE_REPORT_SCANNABLE;
    break;
  case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND:
    break;
  case BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP:
    flags |= FR_BLE_REPORT_SCANNABLE | FR_BLE_REPORT_SCAN_RESPONSE;
    break;
  default:
    return false;
  }
  *out_flags = flags;
  return true;
}

static bool fr_esp_ble_scan_callback_current_locked(uint32_t generation) {
  return fr_esp_ble.scan.state == FR_BLE_SCAN_ACTIVE &&
         fr_esp_ble.scan.generation == generation;
}

static void fr_esp_ble_receive_report(uint32_t generation,
                                      const struct ble_gap_disc_desc *desc) {
  fr_ble_scan_report_t report = {0};
  fr_ble_address_type_t address_type = FR_BLE_ADDRESS_PUBLIC;
  int8_t minimum_rssi = 0;
  bool malformed = false;
  bool filtered = false;
  uint8_t i = 0;
  uint8_t tail = 0;

  if (desc == NULL) {
    return;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble_scan_callback_current_locked(generation)) {
    fr_esp_ble.late_callback_count += 1u;
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return;
  }
  minimum_rssi = fr_esp_ble.scan.minimum_rssi;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  malformed =
      !fr_esp_ble_address_type(desc->addr.type, &address_type) ||
      !fr_esp_ble_report_flags(desc->event_type, &report.flags) ||
      desc->rssi < FR_ESP_BLE_RSSI_MIN || desc->rssi > FR_ESP_BLE_RSSI_MAX ||
      desc->length_data > FR_BLE_SCAN_DATA_BYTES ||
      (desc->length_data > 0 && desc->data == NULL);
  if (!malformed) {
    report.address_type = address_type;
    for (i = 0; i < sizeof(report.address); ++i) {
      report.address[i] = desc->addr.val[sizeof(report.address) - 1u - i];
    }
    report.rssi = desc->rssi;
    report.data_length = desc->length_data;
    if (report.data_length > 0) {
      memcpy(report.data, desc->data, report.data_length);
    }
    report.timestamp_ms = fr_esp_ble_now_ms();
    filtered = report.rssi < minimum_rssi;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble_scan_callback_current_locked(generation)) {
    fr_esp_ble.late_callback_count += 1u;
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return;
  }

  fr_esp_ble.scan.received += 1u;
  if (malformed) {
    fr_esp_ble.scan.malformed += 1u;
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return;
  }
  if (filtered) {
    fr_esp_ble.scan.filtered_rssi += 1u;
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return;
  }
  if (fr_esp_ble.scan.count == FR_BLE_SCAN_QUEUE_COUNT) {
    fr_esp_ble.scan.head =
        (uint8_t)((fr_esp_ble.scan.head + 1u) % FR_BLE_SCAN_QUEUE_COUNT);
    fr_esp_ble.scan.count -= 1u;
    fr_esp_ble.scan.dropped += 1u;
  }
  tail = (uint8_t)((fr_esp_ble.scan.head + fr_esp_ble.scan.count) %
                   FR_BLE_SCAN_QUEUE_COUNT);
  fr_esp_ble.scan.queue[tail] = report;
  fr_esp_ble.scan.count += 1u;
  fr_esp_ble.scan.accepted += 1u;
  if (fr_esp_ble.scan.count > fr_esp_ble.scan.high_water) {
    fr_esp_ble.scan.high_water = fr_esp_ble.scan.count;
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
}

static int fr_esp_ble_scan_event(struct ble_gap_event *event, void *argument) {
  uint32_t generation = (uint32_t)(uintptr_t)argument;

  if (event == NULL) {
    return 0;
  }
  if (event->type == BLE_GAP_EVENT_DISC) {
    fr_esp_ble_receive_report(generation, &event->disc);
    return 0;
  }
  if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (generation != fr_esp_ble.scan.generation ||
        fr_esp_ble.scan.state == FR_BLE_SCAN_IDLE) {
      fr_esp_ble.late_callback_count += 1u;
    } else {
      fr_esp_ble.scan.state = FR_BLE_SCAN_IDLE;
      fr_esp_ble.last_protocol_reason = event->disc_complete.reason;
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);
  }
  return 0;
}
#endif

#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
static bool fr_esp_ble_connection_event_current_locked(uint32_t generation,
                                                       uint16_t stack_handle) {
  return generation == fr_esp_ble.connection.generation &&
         fr_esp_ble.connection.state != FR_BLE_CONNECTION_FREE &&
         fr_esp_ble.connection.stack_handle == stack_handle;
}

static void fr_esp_ble_connection_refresh(uint32_t generation,
                                          uint16_t stack_handle,
                                          int status) {
  struct ble_gap_conn_desc descriptor = {0};
  int descriptor_status = ble_gap_conn_find(stack_handle, &descriptor);
  uint16_t mtu = descriptor_status == 0 ? ble_att_mtu(stack_handle)
                                        : BLE_ATT_MTU_DFLT;

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble_connection_event_current_locked(generation, stack_handle)) {
    fr_esp_ble.late_callback_count += 1u;
  } else if (status == 0 && descriptor_status == 0) {
    (void)fr_esp_ble_connection_copy_descriptor_locked(&descriptor, mtu);
  } else {
    fr_esp_ble.last_protocol_reason =
        status != 0 ? status : descriptor_status;
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
}

static int fr_esp_ble_connection_event(struct ble_gap_event *event,
                                       void *argument) {
  uint32_t generation = (uint32_t)(uintptr_t)argument;
  struct ble_gap_conn_desc descriptor = {0};
  uint16_t stack_handle = BLE_HS_CONN_HANDLE_NONE;
  uint16_t mtu = BLE_ATT_MTU_DFLT;
  int descriptor_status = 0;
  bool terminate = false;

  if (event == NULL) {
    return 0;
  }

  switch (event->type) {
#if FR_BLE_ENABLE_CENTRAL
  case BLE_GAP_EVENT_CONNECT:
    stack_handle = event->connect.conn_handle;
    if (event->connect.status == 0) {
      descriptor_status = ble_gap_conn_find(stack_handle, &descriptor);
      if (descriptor_status == 0) {
        mtu = ble_att_mtu(stack_handle);
      }
    }

    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (generation != fr_esp_ble.connection.generation ||
        fr_esp_ble.connection.role != FR_BLE_CONNECTION_ROLE_CENTRAL ||
        (fr_esp_ble.connection.state != FR_BLE_CONNECTION_CONNECTING &&
         fr_esp_ble.connection.state != FR_BLE_CONNECTION_CLOSING)) {
      fr_esp_ble.late_callback_count += 1u;
    } else if (event->connect.status != 0 || descriptor_status != 0) {
      int status = event->connect.status != 0 ? event->connect.status
                                              : descriptor_status;

      fr_esp_ble.connection.connect_complete = true;
      fr_esp_ble.connection.connect_status = status;
      fr_esp_ble.connection.last_reason = status;
      fr_esp_ble.last_protocol_reason = status;
      if (fr_esp_ble.connection.state == FR_BLE_CONNECTION_CLOSING) {
        fr_esp_ble_connection_free_locked();
      }
    } else if (fr_esp_ble.connection.state == FR_BLE_CONNECTION_CLOSING) {
      fr_esp_ble.connection.stack_handle = stack_handle;
      fr_esp_ble.connection.connect_complete = true;
      terminate = true;
    } else if (!fr_esp_ble_connection_copy_descriptor_locked(&descriptor,
                                                              mtu)) {
      fr_esp_ble.connection.stack_handle = stack_handle;
      fr_esp_ble.connection.state = FR_BLE_CONNECTION_CLOSING;
      fr_esp_ble.connection.connect_complete = true;
      fr_esp_ble.connection.connect_status = BLE_HS_EINVAL;
      fr_esp_ble.connection.last_reason = BLE_HS_EINVAL;
      fr_esp_ble.last_protocol_reason = BLE_HS_EINVAL;
      terminate = true;
    } else {
      fr_esp_ble.connection.state = FR_BLE_CONNECTION_LIVE;
      fr_esp_ble.connection.connect_complete = true;
      fr_esp_ble.connection.connect_status = 0;
      fr_esp_ble.connection.connected_at_ms = fr_esp_ble_now_ms();
      fr_esp_ble.connection_connects += 1u;
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);

    if (terminate) {
      (void)ble_gap_terminate(stack_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
#endif

  case BLE_GAP_EVENT_DISCONNECT:
    stack_handle = event->disconnect.conn.conn_handle;
    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (!fr_esp_ble_connection_event_current_locked(generation,
                                                     stack_handle)) {
      fr_esp_ble.late_callback_count += 1u;
    } else if (fr_esp_ble.connection.state == FR_BLE_CONNECTION_LIVE) {
      fr_esp_ble_connection_mark_disconnected_locked(
          &event->disconnect.conn, event->disconnect.reason);
    } else if (fr_esp_ble.connection.state == FR_BLE_CONNECTION_PENDING ||
               fr_esp_ble.connection.state == FR_BLE_CONNECTION_CLOSING ||
               fr_esp_ble.connection.state == FR_BLE_CONNECTION_CONNECTING) {
      fr_esp_ble_connection_mark_disconnected_locked(
          &event->disconnect.conn, event->disconnect.reason);
      fr_esp_ble_connection_free_locked();
    } else {
      fr_esp_ble.late_callback_count += 1u;
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return 0;

  case BLE_GAP_EVENT_CONN_UPDATE:
    fr_esp_ble_connection_refresh(generation, event->conn_update.conn_handle,
                                  event->conn_update.status);
    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (fr_esp_ble_connection_event_current_locked(
            generation, event->conn_update.conn_handle)) {
      fr_esp_ble.connection.params_pending = false;
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return 0;

  case BLE_GAP_EVENT_CONN_UPDATE_REQ:
    portENTER_CRITICAL(&fr_esp_ble_lock);
    terminate = !fr_esp_ble_connection_event_current_locked(
        generation, event->conn_update_req.conn_handle);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return terminate ? BLE_ERR_CONN_LIMIT : 0;

  case BLE_GAP_EVENT_ENC_CHANGE:
    fr_esp_ble_connection_refresh(generation, event->enc_change.conn_handle,
                                  event->enc_change.status);
    return 0;

  case BLE_GAP_EVENT_MTU:
    if (event->mtu.channel_id != BLE_L2CAP_CID_ATT) {
      return 0;
    }
    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (!fr_esp_ble_connection_event_current_locked(generation,
                                                     event->mtu.conn_handle)) {
      fr_esp_ble.late_callback_count += 1u;
    } else {
      fr_esp_ble.connection.mtu = event->mtu.value;
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return 0;

  case BLE_GAP_EVENT_TERM_FAILURE:
    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (!fr_esp_ble_connection_event_current_locked(
            generation, event->term_failure.conn_handle)) {
      fr_esp_ble.late_callback_count += 1u;
    } else {
      fr_esp_ble.connection.last_reason = event->term_failure.status;
      fr_esp_ble.last_protocol_reason = event->term_failure.status;
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return 0;

  default:
    return 0;
  }
}

static int fr_esp_ble_mtu_complete(uint16_t stack_handle,
                                   const struct ble_gatt_error *error,
                                   uint16_t mtu, void *argument) {
  uint32_t generation = (uint32_t)(uintptr_t)argument;
  int status = error == NULL ? BLE_HS_EINVAL : error->status;

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble_connection_event_current_locked(generation, stack_handle) ||
      !fr_esp_ble.connection.mtu_pending) {
    fr_esp_ble.late_callback_count += 1u;
  } else {
    fr_esp_ble.connection.mtu_pending = false;
    fr_esp_ble.connection.mtu_status = status;
    if (status == 0) {
      fr_esp_ble.connection.mtu = mtu;
    } else {
      fr_esp_ble.connection.last_reason = status;
      fr_esp_ble.last_protocol_reason = status;
    }
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return 0;
}
#endif

#if FR_BLE_ENABLE_BROADCASTER
static int fr_esp_ble_advertise_event(struct ble_gap_event *event,
                                      void *argument) {
  uint32_t generation = (uint32_t)(uintptr_t)argument;
#if FR_BLE_ENABLE_PERIPHERAL
  struct ble_gap_conn_desc descriptor = {0};
  uint32_t connection_generation = 0;
  uint16_t connection_mtu = BLE_ATT_MTU_DFLT;
  int callback_status = 0;
  int descriptor_status = 0;
  bool reject = false;
#endif

  if (event == NULL ||
      (event->type != BLE_GAP_EVENT_ADV_COMPLETE &&
       event->type != BLE_GAP_EVENT_CONNECT)) {
    return 0;
  }

  if (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status == 0) {
#if FR_BLE_ENABLE_PERIPHERAL
    descriptor_status =
        ble_gap_conn_find(event->connect.conn_handle, &descriptor);
    if (descriptor_status == 0) {
      connection_mtu = ble_att_mtu(event->connect.conn_handle);
    }
#endif
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (generation != fr_esp_ble.advertise.generation ||
      fr_esp_ble.advertise.state == FR_BLE_ADVERTISE_IDLE) {
    fr_esp_ble.late_callback_count += 1u;
#if FR_BLE_ENABLE_PERIPHERAL
    if (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status == 0) {
      fr_esp_ble.incoming_rejected += 1u;
      reject = true;
    }
#endif
  } else {
    fr_esp_ble.advertise.state = FR_BLE_ADVERTISE_IDLE;
    fr_esp_ble.last_protocol_reason =
        event->type == BLE_GAP_EVENT_CONNECT ? event->connect.status
                                             : event->adv_complete.reason;
#if FR_BLE_ENABLE_PERIPHERAL
    if (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status == 0) {
      (void)fr_esp_ble_connection_notice_find_locked();
      if (descriptor_status != 0 ||
          fr_esp_ble.connection.state != FR_BLE_CONNECTION_FREE ||
          fr_esp_ble.connection_notice_count ==
              FR_BLE_CONNECTION_NOTICE_COUNT) {
        fr_esp_ble.incoming_rejected += 1u;
        reject = true;
      } else {
        connection_generation = fr_esp_ble_connection_reserve_locked(
            FR_BLE_CONNECTION_ROLE_PERIPHERAL, (fr_handle_ref_t){0}, false);
        if (!fr_esp_ble_connection_copy_descriptor_locked(
                &descriptor, connection_mtu)) {
          fr_esp_ble_connection_free_locked();
          fr_esp_ble.incoming_rejected += 1u;
          reject = true;
        }
      }
    }
#endif
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);

#if FR_BLE_ENABLE_PERIPHERAL
  if (reject) {
    (void)ble_gap_terminate(event->connect.conn_handle, BLE_ERR_CONN_LIMIT);
    return 0;
  }
  if (connection_generation != 0) {
    callback_status = ble_gap_set_event_cb(
        event->connect.conn_handle, fr_esp_ble_connection_event,
        (void *)(uintptr_t)connection_generation);

    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (connection_generation != fr_esp_ble.connection.generation ||
        fr_esp_ble.connection.state != FR_BLE_CONNECTION_CONNECTING) {
      fr_esp_ble.late_callback_count += 1u;
      reject = true;
    } else if (callback_status != 0) {
      fr_esp_ble_connection_free_locked();
      fr_esp_ble.incoming_rejected += 1u;
      fr_esp_ble.last_protocol_reason = callback_status;
      reject = true;
    } else {
      fr_esp_ble.connection.state = FR_BLE_CONNECTION_PENDING;
      fr_esp_ble.connection.connected_at_ms = fr_esp_ble_now_ms();
      fr_esp_ble_connection_notice_push_locked(connection_generation);
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);

    if (reject) {
      (void)ble_gap_terminate(event->connect.conn_handle, BLE_ERR_CONN_LIMIT);
    }
  }
#endif
  return 0;
}
#endif

static void fr_esp_ble_on_sync(void) {
  fr_ble_address_type_t address_type = FR_BLE_ADDRESS_PUBLIC;
  uint8_t raw_address[6] = {0};
  uint8_t raw_type = 0;
  uint8_t i = 0;
  int rc = ble_hs_util_ensure_addr(0);

  if (rc == 0) {
    rc = ble_hs_id_infer_auto(0, &raw_type);
  }
  if (rc == 0) {
    rc = ble_hs_id_copy_addr(raw_type, raw_address, NULL);
  }
  if (rc == 0 && !fr_esp_ble_address_type(raw_type, &address_type)) {
    rc = BLE_HS_EINVAL;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (fr_esp_ble.radio_state != FR_BLE_RADIO_STARTING ||
      fr_esp_ble.shutdown_in_progress) {
    fr_esp_ble.late_callback_count += 1u;
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return;
  }
  if (rc != 0) {
    fr_esp_ble.radio_state = FR_BLE_RADIO_FAILED;
    fr_esp_ble.cleanup_required = true;
    fr_esp_ble.own_address_valid = false;
    fr_esp_ble.last_platform_code = rc;
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return;
  }

  fr_esp_ble.own_address_type = address_type;
  fr_esp_ble.own_address_raw_type = raw_type;
  /* NimBLE stores addresses least-significant byte first; Frothy stores the
   * same address in the display order used by people and tools. */
  for (i = 0; i < sizeof(raw_address); ++i) {
    fr_esp_ble.own_address[i] = raw_address[sizeof(raw_address) - 1u - i];
  }
  fr_esp_ble.own_address_valid = true;
  fr_esp_ble.completion_generation = fr_esp_ble.lifecycle_generation;
  fr_esp_ble.radio_state = FR_BLE_RADIO_READY;
  portEXIT_CRITICAL(&fr_esp_ble_lock);
}

static void fr_esp_ble_on_reset(int reason) {
  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble.last_protocol_reason = reason;
  fr_esp_ble.reset_count += 1u;
  fr_esp_ble.own_address_valid = false;
#if FR_BLE_ENABLE_OBSERVER
  fr_esp_ble_abandon_scan_locked();
#endif
#if FR_BLE_ENABLE_BROADCASTER
  fr_esp_ble_abandon_advertise_locked();
#endif
  if (!fr_esp_ble.shutdown_in_progress) {
    fr_esp_ble.radio_state = FR_BLE_RADIO_FAILED;
    fr_esp_ble.cleanup_required = fr_esp_ble.port_initialized;
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
}

static void fr_esp_ble_host_task(void *argument) {
  (void)argument;
  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble.host_task_running = true;
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  nimble_port_run();
  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble.host_task_running = false;
  fr_esp_ble.host_task_created = false;
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  vTaskDelete(NULL);
}

static void fr_esp_ble_cleanup_task(void *argument) {
  uint32_t generation = (uint32_t)(uintptr_t)argument;
  bool stop_host = false;
  bool host_running = false;
  int stop_code = 0;
  esp_err_t deinit_code = ESP_OK;

  portENTER_CRITICAL(&fr_esp_ble_lock);
  stop_host = fr_esp_ble.host_task_created;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  if (stop_host) {
    stop_code = nimble_port_stop();
    if (stop_code == 0) {
      do {
        portENTER_CRITICAL(&fr_esp_ble_lock);
        host_running = fr_esp_ble.host_task_running;
        portEXIT_CRITICAL(&fr_esp_ble_lock);
        if (host_running) {
          vTaskDelay(FR_ESP_BLE_WAIT_TICKS);
        }
      } while (host_running);
    }
  }
  if (stop_code == 0) {
    if (fr_esp_ble_cleanup_ready_event.event != NULL) {
      ble_npl_event_deinit(&fr_esp_ble_cleanup_ready_event);
    }
    deinit_code = nimble_port_deinit();
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (generation != fr_esp_ble.lifecycle_generation ||
      !fr_esp_ble.shutdown_in_progress) {
    fr_esp_ble.late_callback_count += 1u;
  } else if (stop_code == 0 && deinit_code == ESP_OK) {
    fr_esp_ble_mark_off_locked();
  } else {
    if (stop_code == 0) {
      fr_esp_ble.host_task_created = false;
      fr_esp_ble.host_task_running = false;
    }
    fr_esp_ble.radio_state = FR_BLE_RADIO_FAILED;
    fr_esp_ble.shutdown_in_progress = false;
    fr_esp_ble.cleanup_required = true;
    fr_esp_ble.last_platform_code =
        stop_code != 0 ? stop_code : (int32_t)deinit_code;
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  vTaskDelete(NULL);
}

static fr_err_t fr_esp_ble_start_cleanup_task(uint32_t generation) {
  BaseType_t created = pdFAIL;

  created = xTaskCreate(fr_esp_ble_cleanup_task, "frothy_ble_stop",
                        FR_ESP_BLE_CLEANUP_STACK_BYTES,
                        (void *)(uintptr_t)generation,
                        FR_ESP_BLE_CLEANUP_PRIORITY, NULL);
  if (created == pdPASS) {
    return FR_OK;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (generation == fr_esp_ble.lifecycle_generation &&
      fr_esp_ble.shutdown_in_progress) {
    fr_esp_ble.radio_state = FR_BLE_RADIO_FAILED;
    fr_esp_ble.shutdown_in_progress = false;
    fr_esp_ble.cleanup_required = true;
    fr_esp_ble.last_platform_code = (int32_t)created;
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_ERR_CAPACITY;
}

static void fr_esp_ble_cleanup_when_host_ready(struct ble_npl_event *event) {
  uint32_t generation = 0;

  (void)event;
  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble.shutdown_in_progress) {
    fr_esp_ble.late_callback_count += 1u;
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return;
  }
  generation = fr_esp_ble.lifecycle_generation;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  /* NimBLE starts its host with queued events. If shutdown is requested while
   * the startup event is waiting for an HCI reply, calling nimble_port_stop()
   * concurrently can switch the host off and make it drop that reply. Run
   * this barrier on the same queue until startup has finished, then let the
   * separate worker use ESP-IDF's blocking stop API. */
  if (!ble_hs_is_enabled()) {
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(),
                       &fr_esp_ble_cleanup_ready_event);
    return;
  }

  (void)fr_esp_ble_start_cleanup_task(generation);
}

static fr_err_t fr_esp_ble_begin_cleanup(void) {
  uint32_t generation = 0;
  bool host_task_created = false;

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (fr_esp_ble.radio_state == FR_BLE_RADIO_OFF) {
    fr_esp_ble_mark_off_locked();
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_OK;
  }
  if (fr_esp_ble.shutdown_in_progress) {
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_OK;
  }
  if (!fr_esp_ble.port_initialized && !fr_esp_ble.cleanup_required) {
    fr_esp_ble_mark_off_locked();
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_OK;
  }

  generation = fr_esp_ble.lifecycle_generation;
  host_task_created = fr_esp_ble.host_task_created;
  fr_esp_ble.radio_state = FR_BLE_RADIO_STOPPING;
  fr_esp_ble.shutdown_in_progress = true;
  fr_esp_ble.cleanup_required = true;
  fr_esp_ble.own_address_valid = false;
#if FR_BLE_ENABLE_OBSERVER
  fr_esp_ble_abandon_scan_locked();
#endif
#if FR_BLE_ENABLE_BROADCASTER
  fr_esp_ble_abandon_advertise_locked();
#endif
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  if (!host_task_created) {
    return fr_esp_ble_start_cleanup_task(generation);
  }
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(),
                     &fr_esp_ble_cleanup_ready_event);
  return FR_OK;
}

static fr_err_t fr_esp_ble_wait_cleanup(fr_runtime_t *runtime) {
  uint64_t start_us = (uint64_t)esp_timer_get_time();
  uint64_t budget_us = (uint64_t)FR_BLE_STOP_TIMEOUT_MS * 1000u;

  for (;;) {
    fr_ble_radio_state_t state = FR_BLE_RADIO_OFF;
    bool in_progress = false;
    bool cleanup_required = false;
    fr_err_t err = FR_OK;

    portENTER_CRITICAL(&fr_esp_ble_lock);
    state = fr_esp_ble.radio_state;
    in_progress = fr_esp_ble.shutdown_in_progress;
    cleanup_required = fr_esp_ble.cleanup_required;
    portEXIT_CRITICAL(&fr_esp_ble_lock);

    if (state == FR_BLE_RADIO_OFF) {
      return FR_OK;
    }
    if (!in_progress) {
      return cleanup_required ? FR_ERR_IO : FR_OK;
    }
    if (runtime != NULL) {
      err = fr_platform_poll_interrupt(runtime);
      if (err != FR_OK) {
        return err;
      }
      if (fr_runtime_is_interrupted(runtime)) {
        return FR_ERR_INTERRUPTED;
      }
    }
    if ((uint64_t)esp_timer_get_time() - start_us > budget_us) {
      portENTER_CRITICAL(&fr_esp_ble_lock);
      state = fr_esp_ble.radio_state;
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      /* The worker may still finish. Keep shutdown ownership intact so a new
       * host cannot start over the one that is still stopping. */
      return state == FR_BLE_RADIO_OFF ? FR_OK : FR_ERR_BLE_TIMEOUT;
    }
    vTaskDelay(FR_ESP_BLE_WAIT_TICKS);
  }
}

static fr_err_t fr_esp_ble_abort_start(uint32_t generation,
                                       fr_err_t result) {
  bool pending = false;

  portENTER_CRITICAL(&fr_esp_ble_lock);
  pending = generation == fr_esp_ble.lifecycle_generation &&
            fr_esp_ble.radio_state == FR_BLE_RADIO_STARTING;
  if (pending) {
    fr_esp_ble_record_locked(FR_BLE_OP_ON, result,
                             fr_esp_ble.last_platform_code);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (!pending) {
    return FR_OK;
  }
  (void)fr_esp_ble_begin_cleanup();
  return result;
}

static fr_err_t fr_esp_ble_wait_start(fr_runtime_t *runtime,
                                      uint32_t generation) {
  uint64_t start_us = (uint64_t)esp_timer_get_time();
  uint64_t budget_us = (uint64_t)FR_BLE_START_TIMEOUT_MS * 1000u;

  for (;;) {
    fr_ble_radio_state_t state = FR_BLE_RADIO_OFF;
    uint32_t completion_generation = 0;
    int32_t platform_code = 0;
    fr_err_t err = FR_OK;

    portENTER_CRITICAL(&fr_esp_ble_lock);
    state = fr_esp_ble.radio_state;
    completion_generation = fr_esp_ble.completion_generation;
    platform_code = fr_esp_ble.last_platform_code;
    if (state == FR_BLE_RADIO_READY && completion_generation == generation) {
      fr_esp_ble_record_locked(FR_BLE_OP_ON, FR_OK, 0);
    } else if (state == FR_BLE_RADIO_FAILED) {
      fr_esp_ble_record_locked(FR_BLE_OP_ON, FR_ERR_IO, platform_code);
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);

    if (state == FR_BLE_RADIO_READY && completion_generation == generation) {
      return FR_OK;
    }
    if (state == FR_BLE_RADIO_FAILED) {
      (void)fr_esp_ble_begin_cleanup();
      return FR_ERR_IO;
    }
    if (state != FR_BLE_RADIO_STARTING) {
      portENTER_CRITICAL(&fr_esp_ble_lock);
      fr_esp_ble_record_locked(FR_BLE_OP_ON, FR_ERR_IO, platform_code);
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      if (state == FR_BLE_RADIO_READY) {
        (void)fr_esp_ble_begin_cleanup();
      }
      return FR_ERR_IO;
    }

    err = fr_platform_poll_interrupt(runtime);
    if (err != FR_OK) {
      err = fr_esp_ble_abort_start(generation, err);
      if (err != FR_OK) {
        return err;
      }
      continue;
    }
    if (fr_runtime_is_interrupted(runtime)) {
      err = fr_esp_ble_abort_start(generation, FR_ERR_INTERRUPTED);
      if (err != FR_OK) {
        return err;
      }
      continue;
    }
    if ((uint64_t)esp_timer_get_time() - start_us > budget_us) {
      err = fr_esp_ble_abort_start(generation, FR_ERR_BLE_TIMEOUT);
      if (err != FR_OK) {
        return err;
      }
      continue;
    }
    vTaskDelay(FR_ESP_BLE_WAIT_TICKS);
  }
}

const char *fr_platform_ble_backend_name(void) { return "nimble"; }

/* Frothy serializes foreground platform calls through its one evaluator. The
 * lock below synchronizes those calls with NimBLE callbacks and cleanup. */
fr_err_t fr_platform_ble_on(fr_runtime_t *runtime) {
  uint32_t generation = 0;
  BaseType_t task_created = pdFAIL;
  esp_err_t init_code = ESP_OK;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  for (;;) {
    fr_ble_radio_state_t state = FR_BLE_RADIO_OFF;
    bool cleanup_required = false;
    fr_err_t err = FR_OK;

    portENTER_CRITICAL(&fr_esp_ble_lock);
    state = fr_esp_ble.radio_state;
    cleanup_required = fr_esp_ble.cleanup_required;
    if (state == FR_BLE_RADIO_READY) {
      fr_esp_ble_record_locked(FR_BLE_OP_ON, FR_OK, 0);
    } else if (state == FR_BLE_RADIO_STARTING ||
               state == FR_BLE_RADIO_STOPPING) {
      fr_esp_ble_record_locked(FR_BLE_OP_ON, FR_ERR_BLE_BUSY, 0);
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);

    if (state == FR_BLE_RADIO_READY) {
      return FR_OK;
    }
    if (state == FR_BLE_RADIO_STARTING || state == FR_BLE_RADIO_STOPPING) {
      return FR_ERR_BLE_BUSY;
    }
    if (state != FR_BLE_RADIO_FAILED || !cleanup_required) {
      break;
    }

    err = fr_esp_ble_begin_cleanup();
    if (err == FR_OK) {
      err = fr_esp_ble_wait_cleanup(runtime);
    }
    if (err != FR_OK) {
      portENTER_CRITICAL(&fr_esp_ble_lock);
      fr_esp_ble_record_locked(FR_BLE_OP_ON, err,
                               fr_esp_ble.last_platform_code);
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return err;
    }
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble.lifecycle_generation += 1u;
  generation = fr_esp_ble.lifecycle_generation;
  fr_esp_ble.completion_generation = 0;
  fr_esp_ble.radio_state = FR_BLE_RADIO_STARTING;
  fr_esp_ble.shutdown_in_progress = false;
  fr_esp_ble.cleanup_required = false;
  fr_esp_ble.own_address_valid = false;
  fr_esp_ble.last_platform_code = 0;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  init_code = nimble_port_init();
  if (init_code != ESP_OK) {
    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (generation == fr_esp_ble.lifecycle_generation &&
        fr_esp_ble.radio_state == FR_BLE_RADIO_STARTING) {
      fr_esp_ble.radio_state = FR_BLE_RADIO_FAILED;
      fr_esp_ble.cleanup_required = false;
      fr_esp_ble_record_locked(FR_BLE_OP_ON, FR_ERR_IO, (int32_t)init_code);
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_IO;
  }

  ble_npl_event_init(&fr_esp_ble_cleanup_ready_event,
                     fr_esp_ble_cleanup_when_host_ready, NULL);

  ble_hs_cfg.reset_cb = fr_esp_ble_on_reset;
  ble_hs_cfg.sync_cb = fr_esp_ble_on_sync;

  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble.port_initialized = true;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  /* ESP-IDF's NimBLE task wrapper discards this creation result. Keep it at
   * the target edge so cleanup can distinguish a failed create from a task
   * that was created successfully but has not entered yet. */
  task_created = xTaskCreatePinnedToCore(
      fr_esp_ble_host_task, "nimble_host", NIMBLE_HS_STACK_SIZE, NULL,
      configMAX_PRIORITIES - 4, NULL, NIMBLE_CORE);
  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (task_created == pdPASS) {
    fr_esp_ble.host_task_created = true;
  } else if (generation == fr_esp_ble.lifecycle_generation &&
             fr_esp_ble.radio_state == FR_BLE_RADIO_STARTING) {
    fr_esp_ble.radio_state = FR_BLE_RADIO_FAILED;
    fr_esp_ble.cleanup_required = true;
    fr_esp_ble_record_locked(FR_BLE_OP_ON, FR_ERR_CAPACITY,
                             (int32_t)task_created);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (task_created != pdPASS) {
    /* A host-task allocation failure should not require another task
     * allocation to release a port that has never run. */
    if (fr_esp_ble_cleanup_ready_event.event != NULL) {
      ble_npl_event_deinit(&fr_esp_ble_cleanup_ready_event);
    }
    init_code = nimble_port_deinit();
    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (generation == fr_esp_ble.lifecycle_generation) {
      if (init_code == ESP_OK) {
        fr_esp_ble_mark_off_locked();
      } else {
        fr_esp_ble.radio_state = FR_BLE_RADIO_FAILED;
        fr_esp_ble.cleanup_required = true;
        fr_esp_ble.last_platform_code = (int32_t)init_code;
      }
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_CAPACITY;
  }

  return fr_esp_ble_wait_start(runtime, generation);
}

#if FR_BLE_ENABLE_OBSERVER
fr_err_t fr_platform_ble_scan_start(uint16_t interval_ms, uint16_t window_ms,
                                    bool active, bool repeats,
                                    int8_t minimum_rssi) {
  struct ble_gap_disc_params parameters = {0};
  uint16_t interval_units = 0;
  uint16_t window_units = 0;
  uint32_t generation = 0;
  uint8_t own_address_type = 0;
  int rc = 0;
  bool started = false;

  if (interval_ms < FR_ESP_BLE_SCAN_INTERVAL_MIN_MS ||
      interval_ms > FR_ESP_BLE_SCAN_INTERVAL_MAX_MS ||
      window_ms < FR_ESP_BLE_SCAN_INTERVAL_MIN_MS ||
      window_ms > interval_ms || minimum_rssi < FR_ESP_BLE_RSSI_MIN ||
      minimum_rssi > FR_ESP_BLE_RSSI_MAX) {
    return FR_ERR_RANGE;
  }

  interval_units = fr_esp_ble_gap_units(interval_ms);
  window_units = fr_esp_ble_gap_units(window_ms);
  if (window_units > interval_units) {
    return FR_ERR_RANGE;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (fr_esp_ble.radio_state == FR_BLE_RADIO_STARTING ||
      fr_esp_ble.radio_state == FR_BLE_RADIO_STOPPING) {
    fr_esp_ble_record_locked(FR_BLE_OP_SCAN_START, FR_ERR_BLE_BUSY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_BUSY;
  }
  if (fr_esp_ble.radio_state != FR_BLE_RADIO_READY) {
    fr_esp_ble_record_locked(FR_BLE_OP_SCAN_START, FR_ERR_BLE_NOT_READY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_NOT_READY;
  }
  if (fr_esp_ble.scan.state != FR_BLE_SCAN_IDLE) {
    fr_esp_ble_record_locked(FR_BLE_OP_SCAN_START, FR_ERR_BLE_BUSY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_BUSY;
  }
#if FR_BLE_ENABLE_BROADCASTER
  if (fr_esp_ble.advertise.state != FR_BLE_ADVERTISE_IDLE) {
    fr_esp_ble_record_locked(FR_BLE_OP_SCAN_START, FR_ERR_BLE_BUSY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_BUSY;
  }
#endif

  fr_esp_ble_clear_reports_locked();
  fr_esp_ble.scan.requested_interval_ms = interval_ms;
  fr_esp_ble.scan.requested_window_ms = window_ms;
  fr_esp_ble.scan.actual_interval_us = (uint32_t)interval_units * 625u;
  fr_esp_ble.scan.actual_window_us = (uint32_t)window_units * 625u;
  fr_esp_ble.scan.minimum_rssi = minimum_rssi;
  fr_esp_ble.scan.active = active;
  fr_esp_ble.scan.repeats = repeats;
  fr_esp_ble.scan.generation += 1u;
  generation = fr_esp_ble.scan.generation;
  own_address_type = fr_esp_ble.own_address_raw_type;
  /* Publish callback ownership before asking NimBLE to enable the controller.
   * A report can then be accepted even if the host task runs immediately on
   * the other core. A failed start returns the state to idle below. */
  fr_esp_ble.scan.state = FR_BLE_SCAN_ACTIVE;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  parameters.itvl = interval_units;
  parameters.window = window_units;
  parameters.filter_policy = 0;
  parameters.limited = 0;
  parameters.passive = active ? 0u : 1u;
  parameters.filter_duplicates = repeats ? 0u : 1u;
  parameters.disable_observer_mode = 0;
  rc = ble_gap_disc(own_address_type, BLE_HS_FOREVER, &parameters,
                    fr_esp_ble_scan_event,
                    (void *)(uintptr_t)generation);
  if (rc != 0) {
    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (generation == fr_esp_ble.scan.generation &&
        fr_esp_ble.scan.state == FR_BLE_SCAN_ACTIVE) {
      fr_esp_ble.scan.state = FR_BLE_SCAN_IDLE;
    }
    fr_esp_ble_record_locked(FR_BLE_OP_SCAN_START, FR_ERR_IO, rc);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_IO;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  started = generation == fr_esp_ble.scan.generation &&
            fr_esp_ble.scan.state == FR_BLE_SCAN_ACTIVE &&
            fr_esp_ble.radio_state == FR_BLE_RADIO_READY;
  if (started) {
    fr_esp_ble_record_locked(FR_BLE_OP_SCAN_START, FR_OK, 0);
  } else {
    /* An immediate completion or reset can win after the target accepted the
     * start. Preserve its protocol reason, but make the foreground result
     * visible instead of leaving the prior operation in status. */
    fr_esp_ble_record_locked(FR_BLE_OP_SCAN_START, FR_ERR_IO, 0);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (!started) {
    (void)ble_gap_disc_cancel();
    return FR_ERR_IO;
  }
  return FR_OK;
}

fr_err_t fr_platform_ble_scan_stop(fr_runtime_t *runtime) {
  uint32_t generation = 0;
  fr_err_t result = FR_OK;
  int rc = 0;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble.scan.current_valid = false;
  memset(&fr_esp_ble.scan.current, 0, sizeof(fr_esp_ble.scan.current));
  if (fr_esp_ble.scan.state == FR_BLE_SCAN_IDLE) {
    fr_esp_ble_record_locked(FR_BLE_OP_SCAN_STOP, FR_OK, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_OK;
  }
  generation = fr_esp_ble.scan.generation;
  fr_esp_ble.scan.state = FR_BLE_SCAN_STOPPING;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  /* In the pinned NimBLE host, a successful cancel synchronously disables
   * discovery and clears its state; it does not emit DISC_COMPLETE. */
  if (ble_gap_disc_active()) {
    rc = ble_gap_disc_cancel();
  } else {
    rc = BLE_HS_EALREADY;
  }
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    portENTER_CRITICAL(&fr_esp_ble_lock);
    fr_esp_ble_record_locked(FR_BLE_OP_SCAN_STOP, FR_ERR_IO, rc);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_IO;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (generation != fr_esp_ble.scan.generation ||
      fr_esp_ble.radio_state != FR_BLE_RADIO_READY ||
      (fr_esp_ble.scan.state != FR_BLE_SCAN_STOPPING &&
       fr_esp_ble.scan.state != FR_BLE_SCAN_IDLE)) {
    fr_esp_ble_record_locked(FR_BLE_OP_SCAN_STOP, FR_ERR_IO, 0);
    result = FR_ERR_IO;
  } else {
    /* DISC_COMPLETE may have moved this generation to idle between cancel
     * and reconciliation. In either state, cancellation is complete. */
    fr_esp_ble.scan.state = FR_BLE_SCAN_IDLE;
    fr_esp_ble_record_locked(FR_BLE_OP_SCAN_STOP, FR_OK, 0);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return result;
}

fr_err_t fr_platform_ble_scan_next(bool *out_has_report) {
  if (out_has_report == NULL) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (fr_esp_ble.scan.count == 0) {
    fr_esp_ble.scan.current_valid = false;
    memset(&fr_esp_ble.scan.current, 0, sizeof(fr_esp_ble.scan.current));
    *out_has_report = false;
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_OK;
  }

  fr_esp_ble.scan.current = fr_esp_ble.scan.queue[fr_esp_ble.scan.head];
  memset(&fr_esp_ble.scan.queue[fr_esp_ble.scan.head], 0,
         sizeof(fr_esp_ble.scan.queue[fr_esp_ble.scan.head]));
  fr_esp_ble.scan.current_valid = true;
  fr_esp_ble.scan.head =
      (uint8_t)((fr_esp_ble.scan.head + 1u) % FR_BLE_SCAN_QUEUE_COUNT);
  fr_esp_ble.scan.count -= 1u;
  fr_esp_ble.scan.dequeued += 1u;
  *out_has_report = true;
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_OK;
}

fr_err_t fr_platform_ble_scan_current(fr_ble_scan_report_t *out_report) {
  if (out_report == NULL) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble.scan.current_valid) {
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_NOT_FOUND;
  }
  *out_report = fr_esp_ble.scan.current;
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_BROADCASTER
fr_err_t fr_platform_ble_advertise_start(
    const uint8_t *advertising_data, uint8_t advertising_data_length,
    const uint8_t *scan_response_data, uint8_t scan_response_data_length,
    uint16_t interval_ms, bool connectable) {
  struct ble_gap_adv_params parameters = {0};
  uint16_t interval_units = 0;
  uint32_t generation = 0;
  uint8_t own_address_type = 0;
  int rc = 0;
  bool started = false;

  if ((advertising_data_length > 0 && advertising_data == NULL) ||
      (scan_response_data_length > 0 && scan_response_data == NULL)) {
    return FR_ERR_INVALID;
  }
  if (advertising_data_length > FR_BLE_ADVERTISEMENT_DATA_BYTES ||
      scan_response_data_length > FR_BLE_ADVERTISEMENT_DATA_BYTES) {
    return FR_ERR_CAPACITY;
  }
  if (interval_ms < FR_ESP_BLE_ADVERTISE_INTERVAL_MIN_MS ||
      interval_ms > FR_ESP_BLE_ADVERTISE_INTERVAL_MAX_MS) {
    return FR_ERR_RANGE;
  }

  interval_units = fr_esp_ble_gap_units(interval_ms);

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (fr_esp_ble.radio_state == FR_BLE_RADIO_STARTING ||
      fr_esp_ble.radio_state == FR_BLE_RADIO_STOPPING) {
    fr_esp_ble_record_locked(FR_BLE_OP_ADVERTISE_START, FR_ERR_BLE_BUSY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_BUSY;
  }
  if (fr_esp_ble.radio_state != FR_BLE_RADIO_READY) {
    fr_esp_ble_record_locked(FR_BLE_OP_ADVERTISE_START,
                             FR_ERR_BLE_NOT_READY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_NOT_READY;
  }
#if FR_BLE_ENABLE_OBSERVER
  if (fr_esp_ble.scan.state != FR_BLE_SCAN_IDLE) {
    fr_esp_ble_record_locked(FR_BLE_OP_ADVERTISE_START, FR_ERR_BLE_BUSY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_BUSY;
  }
#endif
  if (fr_esp_ble.advertise.state != FR_BLE_ADVERTISE_IDLE) {
    fr_esp_ble_record_locked(FR_BLE_OP_ADVERTISE_START, FR_ERR_BLE_BUSY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_BUSY;
  }

  fr_esp_ble_clear_advertise_locked();
  if (advertising_data_length > 0) {
    memcpy(fr_esp_ble.advertise.advertising_data, advertising_data,
           advertising_data_length);
  }
  if (scan_response_data_length > 0) {
    memcpy(fr_esp_ble.advertise.scan_response_data, scan_response_data,
           scan_response_data_length);
  }
  fr_esp_ble.advertise.advertising_data_length = advertising_data_length;
  fr_esp_ble.advertise.scan_response_data_length = scan_response_data_length;
  fr_esp_ble.advertise.requested_interval_ms = interval_ms;
  fr_esp_ble.advertise.actual_interval_us =
      (uint32_t)interval_units * 625u;
  fr_esp_ble.advertise.connectable = connectable;
  fr_esp_ble.advertise.generation += 1u;
  generation = fr_esp_ble.advertise.generation;
  own_address_type = fr_esp_ble.own_address_raw_type;
  fr_esp_ble.advertise.state = FR_BLE_ADVERTISE_ACTIVE;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  rc = ble_gap_adv_set_data(fr_esp_ble.advertise.advertising_data,
                            advertising_data_length);
  if (rc == 0) {
    rc = ble_gap_adv_rsp_set_data(fr_esp_ble.advertise.scan_response_data,
                                  scan_response_data_length);
  }
  if (rc == 0) {
    parameters.conn_mode = connectable ? BLE_GAP_CONN_MODE_UND
                                       : BLE_GAP_CONN_MODE_NON;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    parameters.itvl_min = interval_units;
    parameters.itvl_max = interval_units;
    rc = ble_gap_adv_start(own_address_type, NULL, BLE_HS_FOREVER, &parameters,
                           fr_esp_ble_advertise_event,
                           (void *)(uintptr_t)generation);
  }
  if (rc != 0) {
    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (generation == fr_esp_ble.advertise.generation) {
      fr_esp_ble.advertise.state = FR_BLE_ADVERTISE_IDLE;
    }
    fr_esp_ble_record_locked(FR_BLE_OP_ADVERTISE_START, FR_ERR_IO, rc);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_IO;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  started = generation == fr_esp_ble.advertise.generation &&
            fr_esp_ble.advertise.state == FR_BLE_ADVERTISE_ACTIVE &&
            fr_esp_ble.radio_state == FR_BLE_RADIO_READY;
  if (started) {
    fr_esp_ble.advertise.starts += 1u;
    fr_esp_ble_record_locked(FR_BLE_OP_ADVERTISE_START, FR_OK, 0);
  } else {
    fr_esp_ble_record_locked(FR_BLE_OP_ADVERTISE_START, FR_ERR_IO, 0);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (!started) {
    (void)ble_gap_adv_stop();
    return FR_ERR_IO;
  }
  return FR_OK;
}

fr_err_t fr_platform_ble_advertise_stop(void) {
  uint32_t generation = 0;
  fr_err_t result = FR_OK;
  int rc = 0;

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (fr_esp_ble.advertise.state == FR_BLE_ADVERTISE_IDLE) {
    fr_esp_ble_record_locked(FR_BLE_OP_ADVERTISE_STOP, FR_OK, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_OK;
  }
  generation = fr_esp_ble.advertise.generation;
  fr_esp_ble.advertise.state = FR_BLE_ADVERTISE_STOPPING;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  rc = ble_gap_adv_active() ? ble_gap_adv_stop() : BLE_HS_EALREADY;
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    portENTER_CRITICAL(&fr_esp_ble_lock);
    fr_esp_ble_record_locked(FR_BLE_OP_ADVERTISE_STOP, FR_ERR_IO, rc);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_IO;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (generation != fr_esp_ble.advertise.generation ||
      fr_esp_ble.radio_state != FR_BLE_RADIO_READY ||
      (fr_esp_ble.advertise.state != FR_BLE_ADVERTISE_STOPPING &&
       fr_esp_ble.advertise.state != FR_BLE_ADVERTISE_IDLE)) {
    fr_esp_ble_record_locked(FR_BLE_OP_ADVERTISE_STOP, FR_ERR_IO, 0);
    result = FR_ERR_IO;
  } else {
    fr_esp_ble.advertise.state = FR_BLE_ADVERTISE_IDLE;
    fr_esp_ble.advertise.stops += 1u;
    fr_esp_ble_record_locked(FR_BLE_OP_ADVERTISE_STOP, FR_OK, 0);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return result;
}
#endif

fr_err_t fr_platform_ble_off(fr_runtime_t *runtime) {
  fr_err_t err = FR_OK;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
#if FR_BLE_ENABLE_OBSERVER
  fr_esp_ble_invalidate_scan_locked(true);
#endif
#if FR_BLE_ENABLE_BROADCASTER
  fr_esp_ble_abandon_advertise_locked();
  fr_esp_ble_clear_advertise_locked();
#endif
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  err = fr_esp_ble_begin_cleanup();
  if (err == FR_OK) {
    err = fr_esp_ble_wait_cleanup(runtime);
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble_record_locked(FR_BLE_OP_OFF, err,
                           fr_esp_ble.last_platform_code);
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return err;
}

fr_err_t fr_platform_ble_project_clear(void) {
  fr_err_t err = FR_OK;

#if FR_BLE_ENABLE_OBSERVER
  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble_invalidate_scan_locked(true);
  portEXIT_CRITICAL(&fr_esp_ble_lock);
#endif
#if FR_BLE_ENABLE_BROADCASTER
  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble_clear_advertise_all_locked();
  portEXIT_CRITICAL(&fr_esp_ble_lock);
#endif
  err = fr_esp_ble_begin_cleanup();

  if (err == FR_OK) {
    err = fr_esp_ble_wait_cleanup(NULL);
  }
  if (err == FR_OK) {
    portENTER_CRITICAL(&fr_esp_ble_lock);
    fr_esp_ble.last_operation = FR_BLE_OP_NONE;
    fr_esp_ble.last_result = FR_OK;
    fr_esp_ble.last_platform_code = 0;
    fr_esp_ble.last_protocol_reason = 0;
    fr_esp_ble.last_operation_ms = fr_esp_ble_now_ms();
    portEXIT_CRITICAL(&fr_esp_ble_lock);
  }
  return err;
}

fr_err_t fr_platform_ble_status(fr_ble_status_t *out_status) {
  if (out_status == NULL) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  *out_status = (fr_ble_status_t){
      .radio_state = fr_esp_ble.radio_state,
#if FR_BLE_ENABLE_OBSERVER
      .scan_state = fr_esp_ble.scan.state,
#else
      .scan_state = FR_BLE_SCAN_IDLE,
#endif
#if FR_BLE_ENABLE_BROADCASTER
      .advertise_state = fr_esp_ble.advertise.state,
#else
      .advertise_state = FR_BLE_ADVERTISE_IDLE,
#endif
      .roles = fr_esp_ble_roles(),
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
      .coexistence_enabled = true,
#else
      .coexistence_enabled = false,
#endif
      .lifecycle_generation = fr_esp_ble.lifecycle_generation,
#if FR_BLE_ENABLE_OBSERVER
      .scan_generation = fr_esp_ble.scan.generation,
#endif
      .shutdown_in_progress = fr_esp_ble.shutdown_in_progress,
      .cleanup_required = fr_esp_ble.cleanup_required,
      .late_callback_count = fr_esp_ble.late_callback_count,
      .own_address_type = fr_esp_ble.own_address_type,
      .own_address_valid = fr_esp_ble.own_address_valid,
#if FR_BLE_ENABLE_OBSERVER
      .requested_interval_ms = fr_esp_ble.scan.requested_interval_ms,
      .requested_window_ms = fr_esp_ble.scan.requested_window_ms,
      .actual_interval_us = fr_esp_ble.scan.actual_interval_us,
      .actual_window_us = fr_esp_ble.scan.actual_window_us,
      .minimum_rssi = fr_esp_ble.scan.minimum_rssi,
      .active_scan = fr_esp_ble.scan.active,
      .repeats = fr_esp_ble.scan.repeats,
      .queue_count = fr_esp_ble.scan.count,
      .queue_capacity = FR_BLE_SCAN_QUEUE_COUNT,
      .queue_high_water = fr_esp_ble.scan.high_water,
      .received = fr_esp_ble.scan.received,
      .accepted = fr_esp_ble.scan.accepted,
      .filtered_rssi = fr_esp_ble.scan.filtered_rssi,
      .dequeued = fr_esp_ble.scan.dequeued,
      .dropped = fr_esp_ble.scan.dropped,
      .malformed = fr_esp_ble.scan.malformed,
      .current_valid = fr_esp_ble.scan.current_valid,
      .current_rssi = fr_esp_ble.scan.current.rssi,
      .current_flags = fr_esp_ble.scan.current.flags,
      .current_data_length = fr_esp_ble.scan.current.data_length,
#endif
#if FR_BLE_ENABLE_BROADCASTER
      .advertise_requested_interval_ms =
          fr_esp_ble.advertise.requested_interval_ms,
      .advertise_actual_interval_us =
          fr_esp_ble.advertise.actual_interval_us,
      .advertise_connectable = fr_esp_ble.advertise.connectable,
      .advertising_data_length =
          fr_esp_ble.advertise.advertising_data_length,
      .scan_response_data_length =
          fr_esp_ble.advertise.scan_response_data_length,
      .advertise_starts = fr_esp_ble.advertise.starts,
      .advertise_stops = fr_esp_ble.advertise.stops,
#endif
      .last_operation = fr_esp_ble.last_operation,
      .last_result = fr_esp_ble.last_result,
      .last_platform_code = fr_esp_ble.last_platform_code,
      .last_protocol_reason = fr_esp_ble.last_protocol_reason,
      .last_operation_ms = fr_esp_ble.last_operation_ms,
      .reset_count = fr_esp_ble.reset_count,
  };
  memcpy(out_status->own_address, fr_esp_ble.own_address,
         sizeof(out_status->own_address));
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_OK;
}

#endif
