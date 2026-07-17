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
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "os/os_mbuf.h"
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

#if FR_BLE_ENABLE_GATT_SERVER && !CONFIG_BT_NIMBLE_GATT_SERVER
#error "the Frothy GATT server requires the NimBLE GATT server"
#endif

#if FR_BLE_ENABLE_GATT_SERVER &&                                           \
    FR_BLE_GATT_CCCD_COUNT != CONFIG_BT_NIMBLE_MAX_CCCDS
#error "Frothy and NimBLE CCCD counts disagree"
#endif

#if FR_BLE_ENABLE_GATT_SERVER && CONFIG_BT_NIMBLE_SECURITY_ENABLE
#error "the first Frothy GATT server profile does not enable security"
#endif

#if FR_BLE_ENABLE_GATT_SERVER && CONFIG_BT_NIMBLE_ATT_MAX_PREP_ENTRIES != 0
#error "the first Frothy GATT server profile defers prepared writes"
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

#if FR_BLE_ENABLE_GATT_SERVER
typedef struct fr_esp_ble_gatt_t {
  fr_ble_gatt_table_t table;
  uint32_t table_generation;
  uint8_t value_bytes[FR_BLE_GATT_VALUE_BYTES];

  ble_uuid_any_t service_uuids[FR_BLE_GATT_SERVICE_COUNT];
  ble_uuid_any_t characteristic_uuids[FR_BLE_GATT_CHARACTERISTIC_COUNT];
  struct ble_gatt_svc_def services[FR_BLE_GATT_SERVICE_COUNT + 1];
  struct ble_gatt_chr_def
      characteristic_definitions[FR_BLE_GATT_CHARACTERISTIC_COUNT +
                                 FR_BLE_GATT_SERVICE_COUNT];

  fr_ble_gatt_subscription_t subscriptions[FR_BLE_GATT_CCCD_COUNT];
  uint8_t subscription_count;

  fr_ble_gatt_write_t writes[FR_BLE_GATT_WRITE_QUEUE_COUNT];
  uint8_t write_head;
  uint8_t write_count;
  uint8_t write_high_water;
  uint32_t write_overflow;
  uint32_t write_stale;
  uint32_t preaccept_write_rejected;
  bool current_write_valid;
  fr_ble_gatt_write_t current_write;

  bool indication_pending;
  uint32_t indication_connection_generation;
  uint16_t indication_stack_handle;
  uint16_t indication_target_handle;
  int32_t indication_status;

  int32_t last_att_error;
  int32_t last_platform_code;
} fr_esp_ble_gatt_t;
#endif

#if FR_BLE_ENABLE_GATT_CLIENT
typedef struct fr_esp_ble_gatt_client_cache_t {
  uint32_t connection_generation;
  uint16_t value_handle;
  uint16_t cccd_handle;
  uint16_t properties;
  fr_ble_gatt_subscription_mode_t subscription_mode;
  bool valid;
} fr_esp_ble_gatt_client_cache_t;

typedef enum fr_esp_ble_gatt_client_stage_t {
  FR_ESP_BLE_GATT_CLIENT_STAGE_NONE = 0,
  FR_ESP_BLE_GATT_CLIENT_STAGE_SERVICE,
  FR_ESP_BLE_GATT_CLIENT_STAGE_CHARACTERISTIC,
  FR_ESP_BLE_GATT_CLIENT_STAGE_DESCRIPTOR,
  FR_ESP_BLE_GATT_CLIENT_STAGE_READ,
  FR_ESP_BLE_GATT_CLIENT_STAGE_WRITE,
  FR_ESP_BLE_GATT_CLIENT_STAGE_SUBSCRIPTION,
} fr_esp_ble_gatt_client_stage_t;

typedef struct fr_esp_ble_gatt_client_procedure_t {
  bool pending;
  bool abandoned;
  bool stage_complete;
  fr_ble_operation_t operation;
  fr_esp_ble_gatt_client_stage_t stage;
  uint32_t connection_generation;
  uint16_t stack_handle;
  uint16_t attribute_handle;
  int32_t status;

  ble_uuid_any_t service_uuid;
  ble_uuid_any_t characteristic_uuid;
  uint16_t service_start_handle;
  uint16_t service_end_handle;
  uint16_t value_handle;
  uint16_t characteristic_end_handle;
  uint16_t cccd_handle;
  uint16_t properties;
  fr_ble_gatt_subscription_mode_t subscription_mode;

  uint8_t data[FR_BLE_GATT_CLIENT_DATA_BYTES];
  uint8_t data_length;
} fr_esp_ble_gatt_client_procedure_t;

typedef struct fr_esp_ble_gatt_client_t {
  fr_esp_ble_gatt_client_cache_t cache[FR_BLE_GATT_CLIENT_CACHE_COUNT];
  uint8_t cache_count;

  fr_ble_gatt_notification_t
      notifications[FR_BLE_GATT_NOTIFICATION_QUEUE_COUNT];
  uint8_t notification_head;
  uint8_t notification_count;
  uint8_t notification_high_water;
  uint32_t notification_dropped;
  uint32_t notification_stale;
  bool current_notification_valid;
  fr_ble_gatt_notification_t current_notification;

  fr_esp_ble_gatt_client_procedure_t procedure;
  uint16_t service_match_count;
  uint16_t characteristic_match_count;
  int32_t last_att_error;
  int32_t last_platform_code;
} fr_esp_ble_gatt_client_t;
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
#if FR_BLE_ENABLE_GATT_SERVER
  fr_esp_ble_gatt_t gatt;
#endif
#if FR_BLE_ENABLE_GATT_CLIENT
  fr_esp_ble_gatt_client_t gatt_client;
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

#if FR_BLE_ENABLE_GATT_CLIENT
static void fr_esp_ble_gatt_client_clear_locked(void);
#endif

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

#if FR_BLE_ENABLE_GATT_SERVER || FR_BLE_ENABLE_GATT_CLIENT
static void fr_esp_ble_gatt_uuid_build(const fr_ble_uuid_t *portable,
                                       ble_uuid_any_t *target) {
  memset(target, 0, sizeof(*target));
  if (portable->kind == FR_BLE_UUID_16) {
    target->u16.u.type = BLE_UUID_TYPE_16;
    target->u16.value =
        (uint16_t)((uint16_t)portable->bytes[0] << 8u) | portable->bytes[1];
    return;
  }

  target->u128.u.type = BLE_UUID_TYPE_128;
  for (uint8_t i = 0; i < sizeof(target->u128.value); i++) {
    target->u128.value[i] =
        portable->bytes[sizeof(target->u128.value) - 1u - i];
  }
}
#endif

#if FR_BLE_ENABLE_GATT_SERVER
enum {
  FR_ESP_BLE_GATT_PROPERTIES =
      FR_BLE_GATT_CHR_READ | FR_BLE_GATT_CHR_WRITE |
      FR_BLE_GATT_CHR_WRITE_COMMAND | FR_BLE_GATT_CHR_NOTIFY |
      FR_BLE_GATT_CHR_INDICATE,
  FR_ESP_BLE_GATT_SECURITY =
      FR_BLE_GATT_CHR_READ_ENCRYPTED |
      FR_BLE_GATT_CHR_WRITE_ENCRYPTED |
      FR_BLE_GATT_CHR_READ_AUTHENTICATED |
      FR_BLE_GATT_CHR_WRITE_AUTHENTICATED,
  FR_ESP_BLE_GATT_FLAGS =
      FR_ESP_BLE_GATT_PROPERTIES | FR_ESP_BLE_GATT_SECURITY,
};

static fr_ble_gatt_characteristic_row_t *
fr_esp_ble_gatt_characteristic_locked(uint16_t attribute_id) {
  for (uint16_t i = 0; i < fr_esp_ble.gatt.table.characteristic_count; i++) {
    fr_ble_gatt_characteristic_row_t *characteristic =
        &fr_esp_ble.gatt.table.characteristics[i];

    if (characteristic->attribute_id == attribute_id) {
      return characteristic;
    }
  }
  return NULL;
}

static fr_ble_gatt_characteristic_row_t *
fr_esp_ble_gatt_characteristic_for_access_locked(const void *argument,
                                                  uint16_t target_handle) {
  for (uint16_t i = 0; i < fr_esp_ble.gatt.table.characteristic_count; i++) {
    fr_ble_gatt_characteristic_row_t *characteristic =
        &fr_esp_ble.gatt.table.characteristics[i];

    if (argument == characteristic &&
        characteristic->target_value_handle == target_handle) {
      return characteristic;
    }
  }
  return NULL;
}

static fr_ble_gatt_characteristic_row_t *
fr_esp_ble_gatt_characteristic_for_target_locked(uint16_t target_handle) {
  for (uint16_t i = 0; i < fr_esp_ble.gatt.table.characteristic_count; i++) {
    fr_ble_gatt_characteristic_row_t *characteristic =
        &fr_esp_ble.gatt.table.characteristics[i];

    if (characteristic->target_value_handle == target_handle) {
      return characteristic;
    }
  }
  return NULL;
}

static bool
fr_esp_ble_gatt_table_valid(const fr_ble_gatt_table_t *table) {
  bool rows[FR_BLE_GATT_SERVICE_COUNT + FR_BLE_GATT_CHARACTERISTIC_COUNT] = {
      false};
  uint16_t expected_characteristic = 0;
  uint16_t expected_value_offset = 0;
  uint16_t cccd_count = 0;

  if (table == NULL || table->service_count == 0 ||
      table->service_count > FR_BLE_GATT_SERVICE_COUNT ||
      table->characteristic_count > FR_BLE_GATT_CHARACTERISTIC_COUNT ||
      table->row_count != table->service_count + table->characteristic_count ||
      table->value_bytes_used > FR_BLE_GATT_VALUE_BYTES) {
    return false;
  }

  for (uint16_t i = 0; i < table->service_count; i++) {
    const fr_ble_gatt_service_row_t *service = &table->services[i];

    if ((service->uuid.kind != FR_BLE_UUID_16 &&
         service->uuid.kind != FR_BLE_UUID_128) ||
        service->attribute_id >= table->row_count ||
        rows[service->attribute_id] ||
        service->first_characteristic != expected_characteristic ||
        service->characteristic_count >
            table->characteristic_count - expected_characteristic) {
      return false;
    }
    rows[service->attribute_id] = true;
    expected_characteristic =
        (uint16_t)(expected_characteristic + service->characteristic_count);
  }
  if (expected_characteristic != table->characteristic_count) {
    return false;
  }

  for (uint16_t i = 0; i < table->characteristic_count; i++) {
    const fr_ble_gatt_characteristic_row_t *characteristic =
        &table->characteristics[i];

    if ((characteristic->uuid.kind != FR_BLE_UUID_16 &&
         characteristic->uuid.kind != FR_BLE_UUID_128) ||
        characteristic->attribute_id >= table->row_count ||
        rows[characteristic->attribute_id] ||
        characteristic->value_offset != expected_value_offset ||
        expected_value_offset > table->value_bytes_used ||
        characteristic->maximum_length >
            table->value_bytes_used - expected_value_offset ||
        (characteristic->portable_flags & ~FR_ESP_BLE_GATT_FLAGS) != 0 ||
        (characteristic->portable_flags & FR_ESP_BLE_GATT_PROPERTIES) == 0 ||
        (characteristic->portable_flags & FR_ESP_BLE_GATT_SECURITY) != 0) {
      return false;
    }
    rows[characteristic->attribute_id] = true;
    expected_value_offset =
        (uint16_t)(expected_value_offset + characteristic->maximum_length);
    if ((characteristic->portable_flags &
         (FR_BLE_GATT_CHR_NOTIFY | FR_BLE_GATT_CHR_INDICATE)) != 0) {
      cccd_count += 1u;
    }
  }

  if (expected_value_offset != table->value_bytes_used ||
      cccd_count > FR_BLE_GATT_CCCD_COUNT) {
    return false;
  }
  for (uint16_t i = 0; i < table->row_count; i++) {
    if (!rows[i]) {
      return false;
    }
  }
  return true;
}

static uint16_t fr_esp_ble_gatt_flags(uint16_t portable) {
  uint16_t target = 0;

  if ((portable & FR_BLE_GATT_CHR_READ) != 0) {
    target |= BLE_GATT_CHR_F_READ;
  }
  if ((portable & FR_BLE_GATT_CHR_WRITE) != 0) {
    target |= BLE_GATT_CHR_F_WRITE;
  }
  if ((portable & FR_BLE_GATT_CHR_WRITE_COMMAND) != 0) {
    target |= BLE_GATT_CHR_F_WRITE_NO_RSP;
  }
  if ((portable & FR_BLE_GATT_CHR_NOTIFY) != 0) {
    target |= BLE_GATT_CHR_F_NOTIFY;
  }
  if ((portable & FR_BLE_GATT_CHR_INDICATE) != 0) {
    target |= BLE_GATT_CHR_F_INDICATE;
  }
  if ((portable & FR_BLE_GATT_CHR_READ_ENCRYPTED) != 0) {
    target |= BLE_GATT_CHR_F_READ_ENC;
  }
  if ((portable & FR_BLE_GATT_CHR_WRITE_ENCRYPTED) != 0) {
    target |= BLE_GATT_CHR_F_WRITE_ENC;
  }
  if ((portable & FR_BLE_GATT_CHR_READ_AUTHENTICATED) != 0) {
    target |= BLE_GATT_CHR_F_READ_AUTHEN;
  }
  if ((portable & FR_BLE_GATT_CHR_WRITE_AUTHENTICATED) != 0) {
    target |= BLE_GATT_CHR_F_WRITE_AUTHEN;
  }
  return target;
}

static int fr_esp_ble_gatt_access(uint16_t connection_handle,
                                  uint16_t target_handle,
                                  struct ble_gatt_access_ctxt *context,
                                  void *argument);

static void fr_esp_ble_gatt_build_definitions_locked(void) {
  uint16_t definition_index = 0;

  memset(fr_esp_ble.gatt.service_uuids, 0,
         sizeof(fr_esp_ble.gatt.service_uuids));
  memset(fr_esp_ble.gatt.characteristic_uuids, 0,
         sizeof(fr_esp_ble.gatt.characteristic_uuids));
  memset(fr_esp_ble.gatt.services, 0, sizeof(fr_esp_ble.gatt.services));
  memset(fr_esp_ble.gatt.characteristic_definitions, 0,
         sizeof(fr_esp_ble.gatt.characteristic_definitions));

  for (uint16_t service_index = 0;
       service_index < fr_esp_ble.gatt.table.service_count; service_index++) {
    fr_ble_gatt_service_row_t *service =
        &fr_esp_ble.gatt.table.services[service_index];

    fr_esp_ble_gatt_uuid_build(
        &service->uuid, &fr_esp_ble.gatt.service_uuids[service_index]);
    fr_esp_ble.gatt.services[service_index] = (struct ble_gatt_svc_def){
        .type = service->secondary ? BLE_GATT_SVC_TYPE_SECONDARY
                                   : BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &fr_esp_ble.gatt.service_uuids[service_index].u,
        .characteristics =
            &fr_esp_ble.gatt.characteristic_definitions[definition_index],
    };

    for (uint16_t local_index = 0;
         local_index < service->characteristic_count; local_index++) {
      uint16_t characteristic_index =
          (uint16_t)(service->first_characteristic + local_index);
      fr_ble_gatt_characteristic_row_t *characteristic =
          &fr_esp_ble.gatt.table.characteristics[characteristic_index];

      fr_esp_ble_gatt_uuid_build(
          &characteristic->uuid,
          &fr_esp_ble.gatt.characteristic_uuids[characteristic_index]);
      fr_esp_ble.gatt.characteristic_definitions[definition_index++] =
          (struct ble_gatt_chr_def){
              .uuid =
                  &fr_esp_ble.gatt
                       .characteristic_uuids[characteristic_index]
                       .u,
              .access_cb = fr_esp_ble_gatt_access,
              .arg = characteristic,
              .flags = fr_esp_ble_gatt_flags(
                  characteristic->portable_flags),
              .val_handle = &characteristic->target_value_handle,
          };
    }
    definition_index += 1u;
  }
}

static void fr_esp_ble_gatt_clear_subscriptions_locked(void) {
  memset(fr_esp_ble.gatt.subscriptions, 0,
         sizeof(fr_esp_ble.gatt.subscriptions));
  fr_esp_ble.gatt.subscription_count = 0;
}

static fr_ble_gatt_subscription_t *
fr_esp_ble_gatt_subscription_locked(uint16_t attribute_id) {
  for (uint8_t i = 0; i < fr_esp_ble.gatt.subscription_count; i++) {
    fr_ble_gatt_subscription_t *subscription =
        &fr_esp_ble.gatt.subscriptions[i];

    if (subscription->connection_index == FR_ESP_BLE_CONNECTION_INDEX &&
        subscription->connection_generation ==
            fr_esp_ble.connection.generation &&
        subscription->attribute_id == attribute_id) {
      return subscription;
    }
  }
  return NULL;
}

static void fr_esp_ble_gatt_connection_closed_locked(uint32_t generation,
                                                      int status) {
  fr_esp_ble_gatt_clear_subscriptions_locked();
  if (fr_esp_ble.gatt.indication_pending &&
      fr_esp_ble.gatt.indication_connection_generation == generation) {
    fr_esp_ble.gatt.indication_pending = false;
    fr_esp_ble.gatt.indication_status = status;
    fr_esp_ble.gatt.last_platform_code = status;
  }
}

static void fr_esp_ble_gatt_subscribe_locked(
    const struct ble_gap_event *event) {
  fr_ble_gatt_characteristic_row_t *characteristic =
      fr_esp_ble_gatt_characteristic_for_target_locked(
          event->subscribe.attr_handle);
  fr_ble_gatt_subscription_t *subscription = NULL;

  if (characteristic == NULL ||
      (event->subscribe.cur_notify &&
       (characteristic->portable_flags & FR_BLE_GATT_CHR_NOTIFY) == 0) ||
      (event->subscribe.cur_indicate &&
       (characteristic->portable_flags & FR_BLE_GATT_CHR_INDICATE) == 0)) {
    fr_esp_ble.gatt.last_platform_code = BLE_HS_EINVAL;
    return;
  }

  subscription =
      fr_esp_ble_gatt_subscription_locked(characteristic->attribute_id);
  if (!event->subscribe.cur_notify && !event->subscribe.cur_indicate) {
    if (subscription != NULL) {
      uint8_t index =
          (uint8_t)(subscription - fr_esp_ble.gatt.subscriptions);

      fr_esp_ble.gatt.subscription_count -= 1u;
      fr_esp_ble.gatt.subscriptions[index] =
          fr_esp_ble.gatt
              .subscriptions[fr_esp_ble.gatt.subscription_count];
      memset(&fr_esp_ble.gatt
                  .subscriptions[fr_esp_ble.gatt.subscription_count],
             0, sizeof(fr_esp_ble.gatt.subscriptions[0]));
    }
    return;
  }

  if (subscription == NULL) {
    if (fr_esp_ble.gatt.subscription_count == FR_BLE_GATT_CCCD_COUNT) {
      fr_esp_ble.gatt.last_platform_code = BLE_HS_ENOMEM;
      return;
    }
    subscription =
        &fr_esp_ble.gatt
             .subscriptions[fr_esp_ble.gatt.subscription_count++];
  }
  *subscription = (fr_ble_gatt_subscription_t){
      .connection_index = FR_ESP_BLE_CONNECTION_INDEX,
      .connection_generation = fr_esp_ble.connection.generation,
      .attribute_id = characteristic->attribute_id,
      .notify = event->subscribe.cur_notify,
      .indicate = event->subscribe.cur_indicate,
  };
  fr_esp_ble.gatt.last_platform_code = 0;
}

static void fr_esp_ble_gatt_notify_tx_locked(
    const struct ble_gap_event *event) {
  if (!event->notify_tx.indication || event->notify_tx.status == 0) {
    return;
  }
  if (!fr_esp_ble.gatt.indication_pending ||
      fr_esp_ble.gatt.indication_connection_generation !=
          fr_esp_ble.connection.generation ||
      fr_esp_ble.gatt.indication_stack_handle !=
          event->notify_tx.conn_handle ||
      fr_esp_ble.gatt.indication_target_handle !=
          event->notify_tx.attr_handle) {
    fr_esp_ble.late_callback_count += 1u;
    return;
  }

  fr_esp_ble.gatt.indication_pending = false;
  fr_esp_ble.gatt.indication_status = event->notify_tx.status;
  fr_esp_ble.gatt.last_platform_code =
      event->notify_tx.status == BLE_HS_EDONE ? 0 : event->notify_tx.status;
}

static void fr_esp_ble_gatt_radio_off_locked(void) {
  for (uint16_t i = 0; i < fr_esp_ble.gatt.table.characteristic_count; i++) {
    fr_esp_ble.gatt.table.characteristics[i].target_value_handle = 0;
  }
  fr_esp_ble_gatt_clear_subscriptions_locked();
  fr_esp_ble.gatt.write_stale += fr_esp_ble.gatt.write_count;
  memset(fr_esp_ble.gatt.writes, 0, sizeof(fr_esp_ble.gatt.writes));
  fr_esp_ble.gatt.write_head = 0;
  fr_esp_ble.gatt.write_count = 0;
  fr_esp_ble.gatt.current_write_valid = false;
  memset(&fr_esp_ble.gatt.current_write, 0,
         sizeof(fr_esp_ble.gatt.current_write));
  fr_esp_ble.gatt.indication_pending = false;
  fr_esp_ble.gatt.indication_status = BLE_HS_ENOTCONN;
}

static int fr_esp_ble_gatt_access(uint16_t connection_handle,
                                  uint16_t target_handle,
                                  struct ble_gatt_access_ctxt *context,
                                  void *argument) {
  fr_ble_gatt_characteristic_row_t *characteristic = NULL;
  uint8_t bytes[FR_BLE_GATT_VALUE_BYTES];
  uint16_t length = 0;
  uint16_t value_offset = 0;
  int rc = 0;

  if (context == NULL || context->om == NULL) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  if (context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    portENTER_CRITICAL(&fr_esp_ble_lock);
    characteristic = fr_esp_ble_gatt_characteristic_for_access_locked(
        argument, target_handle);
    if (characteristic == NULL) {
      fr_esp_ble.gatt.last_att_error = BLE_ATT_ERR_INVALID_HANDLE;
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return BLE_ATT_ERR_INVALID_HANDLE;
    }
    if ((characteristic->portable_flags & FR_BLE_GATT_CHR_READ) == 0) {
      fr_esp_ble.gatt.last_att_error = BLE_ATT_ERR_READ_NOT_PERMITTED;
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    if (context->offset > characteristic->value_length) {
      fr_esp_ble.gatt.last_att_error = BLE_ATT_ERR_INVALID_OFFSET;
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return BLE_ATT_ERR_INVALID_OFFSET;
    }
    length = (uint16_t)(characteristic->value_length - context->offset);
    value_offset =
        (uint16_t)(characteristic->value_offset + context->offset);
    if (length > 0) {
      memcpy(bytes, &fr_esp_ble.gatt.value_bytes[value_offset], length);
    }
    fr_esp_ble.gatt.last_att_error = 0;
    portEXIT_CRITICAL(&fr_esp_ble_lock);

    rc = length > 0 ? os_mbuf_append(context->om, bytes, length) : 0;
    if (rc == 0) {
      return 0;
    }
    portENTER_CRITICAL(&fr_esp_ble_lock);
    fr_esp_ble.gatt.last_att_error = BLE_ATT_ERR_INSUFFICIENT_RES;
    fr_esp_ble.gatt.last_platform_code = rc;
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return BLE_ATT_ERR_INSUFFICIENT_RES;
  }

  if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    uint32_t packet_length = OS_MBUF_PKTLEN(context->om);
    uint8_t write_bytes[FR_BLE_GATT_WRITE_DATA_BYTES];
    uint8_t tail = 0;

    if (context->offset != 0) {
      portENTER_CRITICAL(&fr_esp_ble_lock);
      fr_esp_ble.gatt.last_att_error = BLE_ATT_ERR_INVALID_OFFSET;
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return BLE_ATT_ERR_INVALID_OFFSET;
    }
    if (packet_length > FR_BLE_GATT_WRITE_DATA_BYTES) {
      portENTER_CRITICAL(&fr_esp_ble_lock);
      fr_esp_ble.gatt.last_att_error = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    rc = ble_hs_mbuf_to_flat(context->om, write_bytes,
                             sizeof(write_bytes), &length);
    if (rc != 0) {
      portENTER_CRITICAL(&fr_esp_ble_lock);
      fr_esp_ble.gatt.last_att_error = BLE_ATT_ERR_UNLIKELY;
      fr_esp_ble.gatt.last_platform_code = rc;
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return BLE_ATT_ERR_UNLIKELY;
    }

    portENTER_CRITICAL(&fr_esp_ble_lock);
    characteristic = fr_esp_ble_gatt_characteristic_for_access_locked(
        argument, target_handle);
    if (characteristic == NULL) {
      fr_esp_ble.gatt.last_att_error = BLE_ATT_ERR_INVALID_HANDLE;
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return BLE_ATT_ERR_INVALID_HANDLE;
    }
    if (connection_handle == BLE_HS_CONN_HANDLE_NONE ||
        fr_esp_ble.connection.stack_handle != connection_handle ||
        fr_esp_ble.connection.state != FR_BLE_CONNECTION_LIVE ||
        !fr_esp_ble.connection.has_runtime_ref ||
        fr_esp_ble.connection.role != FR_BLE_CONNECTION_ROLE_PERIPHERAL) {
      fr_esp_ble.gatt.preaccept_write_rejected += 1u;
      fr_esp_ble.gatt.last_att_error = BLE_ATT_ERR_WRITE_NOT_PERMITTED;
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    if ((characteristic->portable_flags &
         (FR_BLE_GATT_CHR_WRITE | FR_BLE_GATT_CHR_WRITE_COMMAND)) == 0) {
      fr_esp_ble.gatt.last_att_error = BLE_ATT_ERR_WRITE_NOT_PERMITTED;
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    if (length > characteristic->maximum_length) {
      fr_esp_ble.gatt.last_att_error = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (fr_esp_ble.gatt.write_count == FR_BLE_GATT_WRITE_QUEUE_COUNT) {
      fr_esp_ble.gatt.write_overflow += 1u;
      fr_esp_ble.gatt.last_att_error = BLE_ATT_ERR_INSUFFICIENT_RES;
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    tail = (uint8_t)((fr_esp_ble.gatt.write_head +
                      fr_esp_ble.gatt.write_count) %
                     FR_BLE_GATT_WRITE_QUEUE_COUNT);
    fr_esp_ble.gatt.writes[tail] = (fr_ble_gatt_write_t){
        .connection_index = FR_ESP_BLE_CONNECTION_INDEX,
        .connection_generation = fr_esp_ble.connection.generation,
        .table_generation = fr_esp_ble.gatt.table_generation,
        .attribute_id = characteristic->attribute_id,
        .data_length = length,
        .timestamp_ms = fr_esp_ble_now_ms(),
    };
    if (length > 0) {
      memcpy(fr_esp_ble.gatt.writes[tail].data, write_bytes, length);
      memcpy(&fr_esp_ble.gatt.value_bytes[characteristic->value_offset],
             write_bytes, length);
    }
    characteristic->value_length = length;
    fr_esp_ble.gatt.write_count += 1u;
    if (fr_esp_ble.gatt.write_count > fr_esp_ble.gatt.write_high_water) {
      fr_esp_ble.gatt.write_high_water = fr_esp_ble.gatt.write_count;
    }
    fr_esp_ble.gatt.last_att_error = 0;
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return 0;
  }

  return BLE_ATT_ERR_UNLIKELY;
}

static bool fr_esp_ble_gatt_handles_ready_locked(void) {
  for (uint16_t i = 0; i < fr_esp_ble.gatt.table.characteristic_count; i++) {
    if (fr_esp_ble.gatt.table.characteristics[i].target_value_handle == 0) {
      return false;
    }
  }
  return true;
}

static int fr_esp_ble_gatt_register(void) {
  int rc = 0;

  if (fr_esp_ble.gatt.table.service_count == 0) {
    return 0;
  }
  rc = ble_gatts_count_cfg(fr_esp_ble.gatt.services);
  if (rc == 0) {
    rc = ble_gatts_add_svcs(fr_esp_ble.gatt.services);
  }
  return rc;
}
#endif

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
#if FR_BLE_ENABLE_GATT_SERVER
  fr_esp_ble_gatt_radio_off_locked();
#endif
#if FR_BLE_ENABLE_GATT_CLIENT
  fr_esp_ble_gatt_client_clear_locked();
#endif
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

#if FR_BLE_ENABLE_GATT_CLIENT
static fr_err_t fr_esp_ble_connection_entry_locked(uint16_t platform_index);
static bool fr_esp_ble_connection_event_current_locked(
    uint32_t generation, uint16_t stack_handle);

static int32_t fr_esp_ble_gatt_client_att_error(int32_t status) {
  if (status > BLE_HS_ERR_ATT_BASE && status < BLE_HS_ERR_HCI_BASE) {
    return status - BLE_HS_ERR_ATT_BASE;
  }
  return 0;
}

static fr_err_t fr_esp_ble_gatt_client_error(int32_t status) {
  int32_t att_error = fr_esp_ble_gatt_client_att_error(status);

  if (status == 0) {
    return FR_OK;
  }
  if (att_error != 0) {
    switch (att_error) {
    case BLE_ATT_ERR_INVALID_HANDLE:
    case BLE_ATT_ERR_ATTR_NOT_FOUND:
      return FR_ERR_NOT_FOUND;
    case BLE_ATT_ERR_READ_NOT_PERMITTED:
    case BLE_ATT_ERR_WRITE_NOT_PERMITTED:
    case BLE_ATT_ERR_REQ_NOT_SUPPORTED:
    case BLE_ATT_ERR_INSUFFICIENT_AUTHEN:
    case BLE_ATT_ERR_INSUFFICIENT_AUTHOR:
    case BLE_ATT_ERR_INSUFFICIENT_KEY_SZ:
    case BLE_ATT_ERR_INSUFFICIENT_ENC:
      return FR_ERR_UNSUPPORTED;
    case BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN:
    case BLE_ATT_ERR_INSUFFICIENT_RES:
      return FR_ERR_CAPACITY;
    default:
      return FR_ERR_IO;
    }
  }
  switch (status) {
  case BLE_HS_ENOENT:
    return FR_ERR_NOT_FOUND;
  case BLE_HS_ENOTSUP:
    return FR_ERR_UNSUPPORTED;
  case BLE_HS_EMSGSIZE:
  case BLE_HS_ENOMEM_EVT:
    return FR_ERR_CAPACITY;
  case BLE_HS_EAUTHEN:
  case BLE_HS_EAUTHOR:
  case BLE_HS_EENCRYPT:
  case BLE_HS_EENCRYPT_KEY_SZ:
    return FR_ERR_UNSUPPORTED;
  default:
    return fr_esp_ble_host_error(status);
  }
}

static uint16_t fr_esp_ble_gatt_client_properties(uint8_t target) {
  uint16_t portable = 0;

  if ((target & BLE_GATT_CHR_PROP_READ) != 0) {
    portable |= FR_BLE_GATT_CHR_READ;
  }
  if ((target & BLE_GATT_CHR_PROP_WRITE) != 0) {
    portable |= FR_BLE_GATT_CHR_WRITE;
  }
  if ((target & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0) {
    portable |= FR_BLE_GATT_CHR_WRITE_COMMAND;
  }
  if ((target & BLE_GATT_CHR_PROP_NOTIFY) != 0) {
    portable |= FR_BLE_GATT_CHR_NOTIFY;
  }
  if ((target & BLE_GATT_CHR_PROP_INDICATE) != 0) {
    portable |= FR_BLE_GATT_CHR_INDICATE;
  }
  return portable;
}

static fr_esp_ble_gatt_client_cache_t *
fr_esp_ble_gatt_client_cache_locked(uint16_t attribute_handle) {
  for (uint8_t i = 0; i < FR_BLE_GATT_CLIENT_CACHE_COUNT; i++) {
    fr_esp_ble_gatt_client_cache_t *entry =
        &fr_esp_ble.gatt_client.cache[i];

    if (entry->valid && entry->value_handle == attribute_handle &&
        entry->connection_generation == fr_esp_ble.connection.generation) {
      return entry;
    }
  }
  return NULL;
}

static uint8_t fr_esp_ble_gatt_client_subscription_count_locked(void) {
  uint8_t count = 0;

  for (uint8_t i = 0; i < FR_BLE_GATT_CLIENT_CACHE_COUNT; i++) {
    const fr_esp_ble_gatt_client_cache_t *entry =
        &fr_esp_ble.gatt_client.cache[i];

    if (entry->valid && entry->subscription_mode != 0) {
      count += 1u;
    }
  }
  return count;
}

static void fr_esp_ble_gatt_client_finish_locked(int32_t status) {
  fr_esp_ble.gatt_client.procedure.pending = false;
  fr_esp_ble.gatt_client.procedure.status = status;
  fr_esp_ble.gatt_client.last_att_error =
      fr_esp_ble_gatt_client_att_error(status);
  fr_esp_ble.gatt_client.last_platform_code = status == 0 ? 0 : status;
}

static void fr_esp_ble_gatt_client_connection_closed_locked(
    uint32_t generation) {
  memset(fr_esp_ble.gatt_client.cache, 0,
         sizeof(fr_esp_ble.gatt_client.cache));
  fr_esp_ble.gatt_client.cache_count = 0;
  if (fr_esp_ble.gatt_client.procedure.pending &&
      fr_esp_ble.gatt_client.procedure.connection_generation == generation) {
    fr_esp_ble_gatt_client_finish_locked(BLE_HS_ENOTCONN);
  }
}

static void fr_esp_ble_gatt_client_clear_locked(void) {
  memset(&fr_esp_ble.gatt_client, 0, sizeof(fr_esp_ble.gatt_client));
}

static bool fr_esp_ble_gatt_client_callback_current_locked(
    uint32_t generation, uint16_t stack_handle,
    fr_esp_ble_gatt_client_stage_t stage) {
  return fr_esp_ble.gatt_client.procedure.pending &&
         fr_esp_ble.gatt_client.procedure.connection_generation ==
             generation &&
         fr_esp_ble.gatt_client.procedure.stack_handle == stack_handle &&
         fr_esp_ble.gatt_client.procedure.stage == stage &&
         generation == fr_esp_ble.connection.generation;
}

static fr_err_t fr_esp_ble_gatt_client_check_locked(
    uint16_t connection_index) {
  fr_err_t err = FR_OK;

  if (fr_esp_ble.radio_state != FR_BLE_RADIO_READY) {
    return FR_ERR_BLE_NOT_READY;
  }
  err = fr_esp_ble_connection_entry_locked(connection_index);
  if (err != FR_OK) {
    return err;
  }
  if (fr_esp_ble.connection.state != FR_BLE_CONNECTION_LIVE ||
      fr_esp_ble.connection.role != FR_BLE_CONNECTION_ROLE_CENTRAL) {
    return FR_ERR_BLE_DISCONNECTED;
  }
  if (fr_esp_ble.connection.mtu_pending ||
      fr_esp_ble.gatt_client.procedure.pending) {
    return FR_ERR_BLE_BUSY;
  }
  return FR_OK;
}

static uint32_t fr_esp_ble_gatt_client_start_locked(
    fr_ble_operation_t operation, fr_esp_ble_gatt_client_stage_t stage,
    uint16_t attribute_handle) {
  uint32_t generation = fr_esp_ble.connection.generation;

  memset(&fr_esp_ble.gatt_client.procedure, 0,
         sizeof(fr_esp_ble.gatt_client.procedure));
  fr_esp_ble.gatt_client.procedure.pending = true;
  fr_esp_ble.gatt_client.procedure.operation = operation;
  fr_esp_ble.gatt_client.procedure.stage = stage;
  fr_esp_ble.gatt_client.procedure.connection_generation = generation;
  fr_esp_ble.gatt_client.procedure.stack_handle =
      fr_esp_ble.connection.stack_handle;
  fr_esp_ble.gatt_client.procedure.attribute_handle = attribute_handle;
  fr_esp_ble.gatt_client.last_att_error = 0;
  fr_esp_ble.gatt_client.last_platform_code = 0;
  return generation;
}

static void fr_esp_ble_gatt_client_finish_find_locked(void) {
  fr_esp_ble_gatt_client_cache_t *entry =
      fr_esp_ble_gatt_client_cache_locked(
          fr_esp_ble.gatt_client.procedure.value_handle);
  fr_ble_gatt_subscription_mode_t subscription_mode = 0;

  if (entry != NULL &&
      entry->cccd_handle == fr_esp_ble.gatt_client.procedure.cccd_handle) {
    subscription_mode = entry->subscription_mode;
  }
  if (entry == NULL) {
    for (uint8_t i = 0; i < FR_BLE_GATT_CLIENT_CACHE_COUNT; i++) {
      if (!fr_esp_ble.gatt_client.cache[i].valid) {
        entry = &fr_esp_ble.gatt_client.cache[i];
        fr_esp_ble.gatt_client.cache_count += 1u;
        break;
      }
    }
  }
  if (entry == NULL) {
    fr_esp_ble_gatt_client_finish_locked(BLE_HS_ENOMEM);
    return;
  }

  *entry = (fr_esp_ble_gatt_client_cache_t){
      .connection_generation = fr_esp_ble.connection.generation,
      .value_handle = fr_esp_ble.gatt_client.procedure.value_handle,
      .cccd_handle = fr_esp_ble.gatt_client.procedure.cccd_handle,
      .properties = fr_esp_ble.gatt_client.procedure.properties,
      .subscription_mode = subscription_mode,
      .valid = true,
  };
  fr_esp_ble.gatt_client.procedure.attribute_handle = entry->value_handle;
  fr_esp_ble_gatt_client_finish_locked(0);
}

static void fr_esp_ble_gatt_client_launch_failed(
    uint32_t generation, fr_esp_ble_gatt_client_stage_t stage, int status) {
  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (fr_esp_ble_gatt_client_callback_current_locked(
          generation, fr_esp_ble.gatt_client.procedure.stack_handle, stage)) {
    fr_esp_ble_gatt_client_finish_locked(status);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
}

static int fr_esp_ble_gatt_client_descriptor_discovered(
    uint16_t stack_handle, const struct ble_gatt_error *error,
    uint16_t characteristic_handle, const struct ble_gatt_dsc *descriptor,
    void *argument);

static int fr_esp_ble_gatt_client_characteristic_discovered(
    uint16_t stack_handle, const struct ble_gatt_error *error,
    const struct ble_gatt_chr *characteristic, void *argument) {
  uint32_t generation = (uint32_t)(uintptr_t)argument;
  int status = error == NULL ? BLE_HS_EINVAL : error->status;

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble_gatt_client_callback_current_locked(
          generation, stack_handle,
          FR_ESP_BLE_GATT_CLIENT_STAGE_CHARACTERISTIC)) {
    fr_esp_ble.late_callback_count += 1u;
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return 0;
  }
  if (status == 0 && characteristic != NULL) {
    if (fr_esp_ble.gatt_client.procedure.value_handle != 0 &&
        fr_esp_ble.gatt_client.procedure.characteristic_end_handle == 0 &&
        characteristic->def_handle >
            fr_esp_ble.gatt_client.procedure.value_handle) {
      fr_esp_ble.gatt_client.procedure.characteristic_end_handle =
          (uint16_t)(characteristic->def_handle - 1u);
    }
    if (ble_uuid_cmp(
            &characteristic->uuid.u,
            &fr_esp_ble.gatt_client.procedure.characteristic_uuid.u) == 0) {
      fr_esp_ble.gatt_client.characteristic_match_count += 1u;
      if (fr_esp_ble.gatt_client.procedure.value_handle == 0) {
        fr_esp_ble.gatt_client.procedure.value_handle =
            characteristic->val_handle;
        fr_esp_ble.gatt_client.procedure.properties =
            fr_esp_ble_gatt_client_properties(characteristic->properties);
      }
    }
  } else if (status == BLE_HS_EDONE) {
    if (fr_esp_ble.gatt_client.procedure.abandoned) {
      fr_esp_ble_gatt_client_finish_locked(BLE_HS_EPREEMPTED);
    } else if (fr_esp_ble.gatt_client.procedure.value_handle == 0) {
      fr_esp_ble_gatt_client_finish_locked(BLE_HS_ENOENT);
    } else {
      if (fr_esp_ble.gatt_client.procedure.characteristic_end_handle == 0) {
        fr_esp_ble.gatt_client.procedure.characteristic_end_handle =
            fr_esp_ble.gatt_client.procedure.service_end_handle;
      }
      fr_esp_ble.gatt_client.procedure.status = 0;
      fr_esp_ble.gatt_client.procedure.stage_complete = true;
    }
  } else {
    fr_esp_ble_gatt_client_finish_locked(status);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return 0;
}

static int fr_esp_ble_gatt_client_service_discovered(
    uint16_t stack_handle, const struct ble_gatt_error *error,
    const struct ble_gatt_svc *service, void *argument) {
  uint32_t generation = (uint32_t)(uintptr_t)argument;
  int status = error == NULL ? BLE_HS_EINVAL : error->status;

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble_gatt_client_callback_current_locked(
          generation, stack_handle, FR_ESP_BLE_GATT_CLIENT_STAGE_SERVICE)) {
    fr_esp_ble.late_callback_count += 1u;
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return 0;
  }
  if (status == 0 && service != NULL) {
    fr_esp_ble.gatt_client.service_match_count += 1u;
    if (fr_esp_ble.gatt_client.procedure.service_start_handle == 0) {
      fr_esp_ble.gatt_client.procedure.service_start_handle =
          service->start_handle;
      fr_esp_ble.gatt_client.procedure.service_end_handle =
          service->end_handle;
    }
  } else if (status == BLE_HS_EDONE) {
    if (fr_esp_ble.gatt_client.procedure.abandoned) {
      fr_esp_ble_gatt_client_finish_locked(BLE_HS_EPREEMPTED);
    } else if (fr_esp_ble.gatt_client.procedure.service_start_handle == 0) {
      fr_esp_ble_gatt_client_finish_locked(BLE_HS_ENOENT);
    } else {
      fr_esp_ble.gatt_client.procedure.status = 0;
      fr_esp_ble.gatt_client.procedure.stage_complete = true;
    }
  } else {
    fr_esp_ble_gatt_client_finish_locked(status);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return 0;
}

static int fr_esp_ble_gatt_client_descriptor_discovered(
    uint16_t stack_handle, const struct ble_gatt_error *error,
    uint16_t characteristic_handle, const struct ble_gatt_dsc *descriptor,
    void *argument) {
  uint32_t generation = (uint32_t)(uintptr_t)argument;
  int status = error == NULL ? BLE_HS_EINVAL : error->status;

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble_gatt_client_callback_current_locked(
          generation, stack_handle,
          FR_ESP_BLE_GATT_CLIENT_STAGE_DESCRIPTOR)) {
    fr_esp_ble.late_callback_count += 1u;
  } else if (status == 0 && descriptor != NULL &&
             characteristic_handle ==
                 fr_esp_ble.gatt_client.procedure.value_handle) {
    if (fr_esp_ble.gatt_client.procedure.cccd_handle == 0 &&
        ble_uuid_cmp(&descriptor->uuid.u,
                     BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
      fr_esp_ble.gatt_client.procedure.cccd_handle = descriptor->handle;
    }
  } else if (status == BLE_HS_EDONE) {
    if (fr_esp_ble.gatt_client.procedure.abandoned) {
      fr_esp_ble_gatt_client_finish_locked(BLE_HS_EPREEMPTED);
    } else {
      fr_esp_ble_gatt_client_finish_find_locked();
    }
  } else {
    fr_esp_ble_gatt_client_finish_locked(status);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return 0;
}

static int fr_esp_ble_gatt_client_read_complete(
    uint16_t stack_handle, const struct ble_gatt_error *error,
    struct ble_gatt_attr *attribute, void *argument) {
  uint32_t generation = (uint32_t)(uintptr_t)argument;
  uint8_t bytes[FR_BLE_GATT_CLIENT_DATA_BYTES] = {0};
  uint16_t length = 0;
  int status = error == NULL ? BLE_HS_EINVAL : error->status;

  if (status == 0) {
    if (attribute == NULL || attribute->om == NULL) {
      status = BLE_HS_EINVAL;
    } else if (OS_MBUF_PKTLEN(attribute->om) >
               FR_BLE_GATT_CLIENT_DATA_BYTES) {
      status = BLE_HS_EMSGSIZE;
    } else {
      status = ble_hs_mbuf_to_flat(attribute->om, bytes, sizeof(bytes),
                                   &length);
    }
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble_gatt_client_callback_current_locked(
          generation, stack_handle, FR_ESP_BLE_GATT_CLIENT_STAGE_READ)) {
    fr_esp_ble.late_callback_count += 1u;
  } else {
    if (status == 0) {
      memcpy(fr_esp_ble.gatt_client.procedure.data, bytes, length);
      fr_esp_ble.gatt_client.procedure.data_length = (uint8_t)length;
    }
    fr_esp_ble_gatt_client_finish_locked(status);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return 0;
}

static int fr_esp_ble_gatt_client_write_complete(
    uint16_t stack_handle, const struct ble_gatt_error *error,
    struct ble_gatt_attr *attribute, void *argument) {
  uint32_t generation = (uint32_t)(uintptr_t)argument;
  int status = error == NULL ? BLE_HS_EINVAL : error->status;

  (void)attribute;
  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble_gatt_client_callback_current_locked(
          generation, stack_handle, FR_ESP_BLE_GATT_CLIENT_STAGE_WRITE)) {
    fr_esp_ble.late_callback_count += 1u;
  } else {
    fr_esp_ble_gatt_client_finish_locked(status);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return 0;
}

static int fr_esp_ble_gatt_client_subscription_complete(
    uint16_t stack_handle, const struct ble_gatt_error *error,
    struct ble_gatt_attr *attribute, void *argument) {
  uint32_t generation = (uint32_t)(uintptr_t)argument;
  int status = error == NULL ? BLE_HS_EINVAL : error->status;

  (void)attribute;
  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble_gatt_client_callback_current_locked(
          generation, stack_handle,
          FR_ESP_BLE_GATT_CLIENT_STAGE_SUBSCRIPTION)) {
    fr_esp_ble.late_callback_count += 1u;
  } else {
    if (status == 0) {
      fr_esp_ble_gatt_client_cache_t *entry =
          fr_esp_ble_gatt_client_cache_locked(
              fr_esp_ble.gatt_client.procedure.attribute_handle);

      if (entry == NULL) {
        status = BLE_HS_ENOENT;
      } else {
        entry->subscription_mode =
            fr_esp_ble.gatt_client.procedure.subscription_mode;
      }
    }
    fr_esp_ble_gatt_client_finish_locked(status);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return 0;
}

static void fr_esp_ble_gatt_client_receive_notification(
    uint32_t generation, const struct ble_gap_event *event) {
  uint8_t bytes[FR_BLE_GATT_CLIENT_DATA_BYTES] = {0};
  uint16_t length = 0;
  int copy_status = BLE_HS_EINVAL;
  uint8_t tail = 0;

  if (event->notify_rx.om != NULL &&
      OS_MBUF_PKTLEN(event->notify_rx.om) <=
          FR_BLE_GATT_CLIENT_DATA_BYTES) {
    copy_status = ble_hs_mbuf_to_flat(event->notify_rx.om, bytes,
                                      sizeof(bytes), &length);
  } else if (event->notify_rx.om != NULL) {
    copy_status = BLE_HS_EMSGSIZE;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble_connection_event_current_locked(
          generation, event->notify_rx.conn_handle) ||
      fr_esp_ble.connection.state != FR_BLE_CONNECTION_LIVE ||
      fr_esp_ble.connection.role != FR_BLE_CONNECTION_ROLE_CENTRAL ||
      !fr_esp_ble.connection.has_runtime_ref) {
    fr_esp_ble.late_callback_count += 1u;
  } else {
    fr_esp_ble_gatt_client_cache_t *entry =
        fr_esp_ble_gatt_client_cache_locked(event->notify_rx.attr_handle);
    fr_ble_gatt_subscription_mode_t expected =
        event->notify_rx.indication ? FR_BLE_GATT_SUBSCRIBE_INDICATIONS
                                    : FR_BLE_GATT_SUBSCRIBE_NOTIFICATIONS;

    if (copy_status != 0 || entry == NULL ||
        entry->subscription_mode != expected ||
        fr_esp_ble.gatt_client.notification_count ==
            FR_BLE_GATT_NOTIFICATION_QUEUE_COUNT) {
      fr_esp_ble.gatt_client.notification_dropped += 1u;
      if (copy_status != 0) {
        fr_esp_ble.gatt_client.last_platform_code = copy_status;
      }
    } else {
      tail = (uint8_t)((fr_esp_ble.gatt_client.notification_head +
                        fr_esp_ble.gatt_client.notification_count) %
                       FR_BLE_GATT_NOTIFICATION_QUEUE_COUNT);
      fr_esp_ble.gatt_client.notifications[tail] =
          (fr_ble_gatt_notification_t){
              .connection_index = FR_ESP_BLE_CONNECTION_INDEX,
              .connection_generation = fr_esp_ble.connection.generation,
              .attribute_handle = event->notify_rx.attr_handle,
              .data_length = (uint8_t)length,
              .timestamp_ms = fr_esp_ble_now_ms(),
              .indication = event->notify_rx.indication,
          };
      if (length > 0) {
        memcpy(fr_esp_ble.gatt_client.notifications[tail].data, bytes,
               length);
      }
      fr_esp_ble.gatt_client.notification_count += 1u;
      if (fr_esp_ble.gatt_client.notification_count >
          fr_esp_ble.gatt_client.notification_high_water) {
        fr_esp_ble.gatt_client.notification_high_water =
            fr_esp_ble.gatt_client.notification_count;
      }
    }
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
}

static fr_err_t fr_esp_ble_gatt_client_wait(
    fr_runtime_t *runtime, uint32_t generation,
    fr_ble_operation_t operation, uint64_t deadline_us,
    bool wait_for_stage) {
  for (;;) {
    bool current = false;
    bool pending = false;
    bool stage_complete = false;
    int32_t status = 0;
    fr_err_t err = FR_OK;

    portENTER_CRITICAL(&fr_esp_ble_lock);
    current = fr_esp_ble.gatt_client.procedure.connection_generation ==
                  generation &&
              fr_esp_ble.gatt_client.procedure.operation == operation;
    if (current) {
      pending = fr_esp_ble.gatt_client.procedure.pending;
      stage_complete = fr_esp_ble.gatt_client.procedure.stage_complete;
      status = fr_esp_ble.gatt_client.procedure.status;
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);

    if (!current) {
      err = FR_ERR_BLE_DISCONNECTED;
    } else if (!pending) {
      err = fr_esp_ble_gatt_client_error(status);
    } else if (!wait_for_stage || !stage_complete) {
      err = fr_platform_poll_interrupt(runtime);
      if (err == FR_OK && fr_runtime_is_interrupted(runtime)) {
        err = FR_ERR_INTERRUPTED;
      }
      if (err == FR_OK && (uint64_t)esp_timer_get_time() > deadline_us) {
        err = FR_ERR_BLE_TIMEOUT;
      }
    }
    if (err == FR_OK && pending && wait_for_stage && stage_complete) {
      return FR_OK;
    }
    if (err != FR_OK || !pending) {
      portENTER_CRITICAL(&fr_esp_ble_lock);
      if (err != FR_OK && current &&
          fr_esp_ble.gatt_client.procedure.pending) {
        fr_esp_ble.gatt_client.procedure.abandoned = true;
      }
      fr_esp_ble_record_locked(operation, err, status);
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return err;
    }
    vTaskDelay(FR_ESP_BLE_WAIT_TICKS);
  }
}
#endif

static uint16_t fr_esp_ble_connection_interval_units(uint16_t milliseconds) {
  return (uint16_t)(((uint32_t)milliseconds * 4u + 2u) / 5u);
}

static uint16_t
fr_esp_ble_supervision_timeout_units(uint16_t milliseconds) {
  return (uint16_t)(((uint32_t)milliseconds + 5u) / 10u);
}

static void fr_esp_ble_connection_free_locked(void) {
  uint32_t generation = fr_esp_ble.connection.generation;

#if FR_BLE_ENABLE_GATT_SERVER
  fr_esp_ble_gatt_connection_closed_locked(generation, BLE_HS_ENOTCONN);
#endif
#if FR_BLE_ENABLE_GATT_CLIENT
  fr_esp_ble_gatt_client_connection_closed_locked(generation);
#endif
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
#if FR_BLE_ENABLE_GATT_SERVER
  fr_esp_ble_gatt_connection_closed_locked(fr_esp_ble.connection.generation,
                                            BLE_HS_ENOTCONN);
#endif
#if FR_BLE_ENABLE_GATT_CLIENT
  fr_esp_ble_gatt_client_connection_closed_locked(
      fr_esp_ble.connection.generation);
#endif
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

static fr_err_t fr_esp_ble_connection_entry_locked(uint16_t platform_index) {
  if (platform_index != FR_ESP_BLE_CONNECTION_INDEX ||
      fr_esp_ble.connection.state == FR_BLE_CONNECTION_FREE ||
      !fr_esp_ble.connection.has_runtime_ref) {
    return FR_ERR_HANDLE;
  }
  return FR_OK;
}

static void fr_esp_ble_connection_info_locked(
    fr_ble_connection_info_t *out_info) {
  *out_info = (fr_ble_connection_info_t){
      .state = fr_esp_ble.connection.state,
      .role = fr_esp_ble.connection.role,
      .peer_address_type = fr_esp_ble.connection.peer_address_type,
      .interval_us = (uint32_t)fr_esp_ble.connection.interval_units *
                     FR_ESP_BLE_CONNECTION_INTERVAL_UNIT_US,
      .latency = fr_esp_ble.connection.latency,
      .supervision_timeout_us =
          (uint32_t)fr_esp_ble.connection.supervision_timeout_units *
          FR_ESP_BLE_SUPERVISION_TIMEOUT_UNIT_US,
      .mtu = fr_esp_ble.connection.mtu,
      .encrypted = fr_esp_ble.connection.encrypted,
      .authenticated = fr_esp_ble.connection.authenticated,
      .bonded = fr_esp_ble.connection.bonded,
      .rssi_valid = fr_esp_ble.connection.rssi_valid,
      .last_rssi = fr_esp_ble.connection.last_rssi,
      .last_reason = fr_esp_ble.connection.last_reason,
      .connected_at_ms = fr_esp_ble.connection.connected_at_ms,
      .disconnected_at_ms = fr_esp_ble.connection.disconnected_at_ms,
  };
  memcpy(out_info->peer_address, fr_esp_ble.connection.peer_address,
         sizeof(out_info->peer_address));
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

#if FR_BLE_ENABLE_GATT_CLIENT
  case BLE_GAP_EVENT_NOTIFY_RX:
    fr_esp_ble_gatt_client_receive_notification(generation, event);
    return 0;
#endif

#if FR_BLE_ENABLE_GATT_SERVER
  case BLE_GAP_EVENT_SUBSCRIBE:
    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (!fr_esp_ble_connection_event_current_locked(
            generation, event->subscribe.conn_handle) ||
        fr_esp_ble.connection.role != FR_BLE_CONNECTION_ROLE_PERIPHERAL) {
      fr_esp_ble.late_callback_count += 1u;
    } else {
      fr_esp_ble_gatt_subscribe_locked(event);
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return 0;

  case BLE_GAP_EVENT_NOTIFY_TX:
    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (!fr_esp_ble_connection_event_current_locked(
            generation, event->notify_tx.conn_handle) ||
        fr_esp_ble.connection.role != FR_BLE_CONNECTION_ROLE_PERIPHERAL) {
      fr_esp_ble.late_callback_count += 1u;
    } else {
      fr_esp_ble_gatt_notify_tx_locked(event);
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return 0;
#endif

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
    (void)ble_gap_terminate(event->connect.conn_handle,
                            BLE_ERR_REM_USER_CONN_TERM);
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
      (void)ble_gap_terminate(event->connect.conn_handle,
                              BLE_ERR_REM_USER_CONN_TERM);
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
#if FR_BLE_ENABLE_GATT_SERVER
  if (rc == 0 && !fr_esp_ble_gatt_handles_ready_locked()) {
    rc = BLE_HS_EINVAL;
  }
#endif
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
#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
  if (fr_esp_ble.connection.state == FR_BLE_CONNECTION_LIVE) {
    fr_esp_ble_connection_mark_disconnected_locked(NULL, reason);
  } else if (fr_esp_ble.connection.state == FR_BLE_CONNECTION_PENDING) {
    fr_esp_ble_connection_mark_disconnected_locked(NULL, reason);
    fr_esp_ble_connection_free_locked();
  } else if (fr_esp_ble.connection.state == FR_BLE_CONNECTION_CLOSING ||
             (fr_esp_ble.connection.state == FR_BLE_CONNECTION_CONNECTING &&
              fr_esp_ble.connection.role ==
                  FR_BLE_CONNECTION_ROLE_PERIPHERAL)) {
    fr_esp_ble_connection_free_locked();
#if FR_BLE_ENABLE_CENTRAL
  } else if (fr_esp_ble.connection.state == FR_BLE_CONNECTION_CONNECTING) {
    fr_esp_ble.connection.connect_complete = true;
    fr_esp_ble.connection.connect_status = reason;
    fr_esp_ble.connection.last_reason = reason;
#endif
  } else if (fr_esp_ble.connection.state == FR_BLE_CONNECTION_DISCONNECTED) {
    fr_esp_ble.connection.last_reason = reason;
  }
#endif
#if FR_BLE_ENABLE_GATT_SERVER
  fr_esp_ble_gatt_radio_off_locked();
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
#if FR_BLE_ENABLE_GATT_SERVER
  int gatt_code = 0;
#endif

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

#if FR_BLE_ENABLE_GATT_SERVER
  gatt_code = fr_esp_ble_gatt_register();
  if (gatt_code != 0) {
    fr_err_t result = fr_esp_ble_host_error(gatt_code);

    init_code = nimble_port_deinit();
    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (generation == fr_esp_ble.lifecycle_generation) {
      fr_esp_ble.gatt.last_platform_code = gatt_code;
      if (init_code == ESP_OK) {
        fr_esp_ble_mark_off_locked();
        fr_esp_ble_record_locked(FR_BLE_OP_ON, result, gatt_code);
      } else {
        fr_esp_ble.radio_state = FR_BLE_RADIO_FAILED;
        fr_esp_ble.port_initialized = true;
        fr_esp_ble.cleanup_required = true;
        fr_esp_ble_record_locked(FR_BLE_OP_ON, FR_ERR_IO,
                                 (int32_t)init_code);
      }
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return init_code == ESP_OK ? result : FR_ERR_IO;
  }
#endif

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

#if FR_BLE_ENABLE_CENTRAL
fr_err_t fr_platform_ble_connect(fr_runtime_t *runtime, const uint8_t peer[7],
                                 uint16_t timeout_ms,
                                 fr_handle_ref_t runtime_ref,
                                 uint16_t *out_platform_index) {
  ble_addr_t address = {0};
  uint64_t start_us = 0;
  uint64_t budget_us = 0;
  uint32_t generation = 0;
  uint8_t own_address_type = 0;
  int rc = 0;

  if (runtime == NULL || out_platform_index == NULL ||
      !fr_esp_ble_raw_address(peer, &address)) {
    return FR_ERR_INVALID;
  }
  *out_platform_index = FR_HANDLE_PLATFORM_NONE;
  if (timeout_ms == 0 || timeout_ms > FR_BLE_CONNECT_TIMEOUT_MAX_MS) {
    return FR_ERR_RANGE;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (fr_esp_ble.radio_state == FR_BLE_RADIO_STARTING ||
      fr_esp_ble.radio_state == FR_BLE_RADIO_STOPPING) {
    fr_esp_ble_record_locked(FR_BLE_OP_CONNECT, FR_ERR_BLE_BUSY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_BUSY;
  }
  if (fr_esp_ble.radio_state != FR_BLE_RADIO_READY) {
    fr_esp_ble_record_locked(FR_BLE_OP_CONNECT, FR_ERR_BLE_NOT_READY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_NOT_READY;
  }
#if FR_BLE_ENABLE_OBSERVER
  if (fr_esp_ble.scan.state != FR_BLE_SCAN_IDLE) {
    fr_esp_ble_record_locked(FR_BLE_OP_CONNECT, FR_ERR_BLE_BUSY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_BUSY;
  }
#endif
#if FR_BLE_ENABLE_BROADCASTER
  if (fr_esp_ble.advertise.state != FR_BLE_ADVERTISE_IDLE) {
    fr_esp_ble_record_locked(FR_BLE_OP_CONNECT, FR_ERR_BLE_BUSY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_BUSY;
  }
#endif
  if (fr_esp_ble.connection.state != FR_BLE_CONNECTION_FREE) {
    fr_esp_ble_record_locked(FR_BLE_OP_CONNECT, FR_ERR_CAPACITY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_CAPACITY;
  }

  generation = fr_esp_ble_connection_reserve_locked(
      FR_BLE_CONNECTION_ROLE_CENTRAL, runtime_ref, true);
  fr_esp_ble.connection.peer_address_type =
      (fr_ble_address_type_t)peer[0];
  memcpy(fr_esp_ble.connection.peer_address, &peer[1],
         sizeof(fr_esp_ble.connection.peer_address));
  own_address_type = fr_esp_ble.own_address_raw_type;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  rc = ble_gap_connect(own_address_type, &address, timeout_ms, NULL,
                       fr_esp_ble_connection_event,
                       (void *)(uintptr_t)generation);
  if (rc != 0) {
    fr_err_t result = fr_esp_ble_host_error(rc);

    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (generation == fr_esp_ble.connection.generation &&
        fr_esp_ble.connection.state == FR_BLE_CONNECTION_CONNECTING) {
      fr_esp_ble_connection_free_locked();
    }
    fr_esp_ble_record_locked(FR_BLE_OP_CONNECT, result, rc);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return result;
  }

  start_us = (uint64_t)esp_timer_get_time();
  budget_us = (uint64_t)timeout_ms * 1000u;
  for (;;) {
    fr_ble_connection_state_t state = FR_BLE_CONNECTION_FREE;
    bool complete = false;
    int32_t status = 0;
    fr_err_t abort_result = FR_OK;

    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (generation == fr_esp_ble.connection.generation) {
      state = fr_esp_ble.connection.state;
      complete = fr_esp_ble.connection.connect_complete;
      status = fr_esp_ble.connection.connect_status;
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);

    if (complete && state == FR_BLE_CONNECTION_LIVE) {
      portENTER_CRITICAL(&fr_esp_ble_lock);
      fr_esp_ble_record_locked(FR_BLE_OP_CONNECT, FR_OK, 0);
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      *out_platform_index = FR_ESP_BLE_CONNECTION_INDEX;
      return FR_OK;
    }
    if (complete) {
      fr_err_t result = state == FR_BLE_CONNECTION_DISCONNECTED
                            ? FR_ERR_BLE_DISCONNECTED
                            : fr_esp_ble_host_error(status);

      portENTER_CRITICAL(&fr_esp_ble_lock);
      if (generation == fr_esp_ble.connection.generation &&
          fr_esp_ble.connection.state != FR_BLE_CONNECTION_CLOSING) {
        fr_esp_ble_connection_free_locked();
      }
      fr_esp_ble_record_locked(FR_BLE_OP_CONNECT, result, status);
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return result;
    }
    if (state == FR_BLE_CONNECTION_FREE) {
      portENTER_CRITICAL(&fr_esp_ble_lock);
      fr_esp_ble_record_locked(FR_BLE_OP_CONNECT, FR_ERR_IO, status);
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return FR_ERR_IO;
    }

    abort_result = fr_platform_poll_interrupt(runtime);
    if (abort_result == FR_OK && fr_runtime_is_interrupted(runtime)) {
      abort_result = FR_ERR_INTERRUPTED;
    }
    if (abort_result == FR_OK &&
        (uint64_t)esp_timer_get_time() - start_us > budget_us) {
      abort_result = FR_ERR_BLE_TIMEOUT;
    }
    if (abort_result != FR_OK) {
      bool cancel = false;

      portENTER_CRITICAL(&fr_esp_ble_lock);
      if (generation == fr_esp_ble.connection.generation &&
          fr_esp_ble.connection.state == FR_BLE_CONNECTION_CONNECTING &&
          !fr_esp_ble.connection.connect_complete) {
        fr_esp_ble.connection.state = FR_BLE_CONNECTION_CLOSING;
        fr_esp_ble.connection.connect_status = BLE_HS_EAPP;
        fr_esp_ble.connection.last_reason = BLE_HS_EAPP;
        cancel = true;
      }
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      if (!cancel) {
        continue;
      }

      rc = ble_gap_conn_cancel();
      portENTER_CRITICAL(&fr_esp_ble_lock);
      fr_esp_ble_record_locked(
          FR_BLE_OP_CONNECT, abort_result,
          rc == 0 || rc == BLE_HS_EALREADY ? 0 : rc);
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return abort_result;
    }
    vTaskDelay(FR_ESP_BLE_WAIT_TICKS);
  }
}
#endif

#if FR_BLE_ENABLE_PERIPHERAL
fr_err_t fr_platform_ble_connection_pending(bool *out_pending) {
  if (out_pending == NULL) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  *out_pending = fr_esp_ble_connection_notice_find_locked();
  fr_esp_ble_record_locked(FR_BLE_OP_ACCEPT, FR_OK, 0);
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_OK;
}

fr_err_t fr_platform_ble_accept(fr_handle_ref_t runtime_ref,
                                uint16_t *out_platform_index,
                                bool *out_accepted) {
  if (out_platform_index == NULL || out_accepted == NULL) {
    return FR_ERR_INVALID;
  }
  *out_platform_index = FR_HANDLE_PLATFORM_NONE;
  *out_accepted = false;

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble_connection_notice_find_locked()) {
    fr_esp_ble_record_locked(FR_BLE_OP_ACCEPT, FR_OK, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_OK;
  }

  fr_esp_ble_connection_notice_pop_locked();
  fr_esp_ble.connection.runtime_ref = runtime_ref;
  fr_esp_ble.connection.has_runtime_ref = true;
  fr_esp_ble.connection.state = FR_BLE_CONNECTION_LIVE;
  fr_esp_ble.connection_accepts += 1u;
  *out_platform_index = FR_ESP_BLE_CONNECTION_INDEX;
  *out_accepted = true;
  fr_esp_ble_record_locked(FR_BLE_OP_ACCEPT, FR_OK, 0);
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_OK;
}

fr_err_t fr_platform_ble_reject_pending(void) {
  uint32_t generation = 0;
  uint16_t stack_handle = BLE_HS_CONN_HANDLE_NONE;
  int rc = 0;

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble_connection_notice_find_locked()) {
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_OK;
  }

  fr_esp_ble_connection_notice_pop_locked();
  generation = fr_esp_ble.connection.generation;
  stack_handle = fr_esp_ble.connection.stack_handle;
  fr_esp_ble.connection.state = FR_BLE_CONNECTION_CLOSING;
  fr_esp_ble.incoming_rejected += 1u;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  rc = ble_gap_terminate(stack_handle, BLE_ERR_REM_USER_CONN_TERM);
  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (generation == fr_esp_ble.connection.generation &&
      fr_esp_ble.connection.state == FR_BLE_CONNECTION_CLOSING &&
      rc == BLE_HS_ENOTCONN) {
    fr_esp_ble.connection_disconnects += 1u;
    fr_esp_ble_connection_free_locked();
  }
  fr_esp_ble_record_locked(FR_BLE_OP_ACCEPT, FR_ERR_CAPACITY,
                           rc == 0 || rc == BLE_HS_ENOTCONN ? 0 : rc);
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return rc == 0 || rc == BLE_HS_ENOTCONN ? FR_OK
                                           : fr_esp_ble_host_error(rc);
}
#endif

#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
fr_err_t fr_platform_ble_connection_ready(uint16_t platform_index,
                                          bool *out_ready) {
  fr_err_t err = FR_OK;

  if (out_ready == NULL) {
    return FR_ERR_INVALID;
  }
  portENTER_CRITICAL(&fr_esp_ble_lock);
  err = fr_esp_ble_connection_entry_locked(platform_index);
  if (err == FR_OK) {
    *out_ready = fr_esp_ble.connection.state == FR_BLE_CONNECTION_LIVE;
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return err;
}

fr_err_t fr_platform_ble_connection_close(uint16_t platform_index) {
  fr_err_t err = FR_OK;
  uint32_t generation = 0;
  uint16_t stack_handle = BLE_HS_CONN_HANDLE_NONE;
  int rc = 0;

  portENTER_CRITICAL(&fr_esp_ble_lock);
  err = fr_esp_ble_connection_entry_locked(platform_index);
  if (err != FR_OK) {
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return err;
  }
  if (fr_esp_ble.connection.state == FR_BLE_CONNECTION_DISCONNECTED) {
    fr_esp_ble_connection_free_locked();
    fr_esp_ble_record_locked(FR_BLE_OP_CONNECTION_CLOSE, FR_OK, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_OK;
  }
  if (fr_esp_ble.connection.state != FR_BLE_CONNECTION_LIVE) {
    fr_esp_ble_record_locked(FR_BLE_OP_CONNECTION_CLOSE, FR_ERR_BLE_BUSY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_BUSY;
  }

  generation = fr_esp_ble.connection.generation;
  stack_handle = fr_esp_ble.connection.stack_handle;
  fr_esp_ble.connection.state = FR_BLE_CONNECTION_CLOSING;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  rc = ble_gap_terminate(stack_handle, BLE_ERR_REM_USER_CONN_TERM);
  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (generation == fr_esp_ble.connection.generation &&
      fr_esp_ble.connection.state == FR_BLE_CONNECTION_CLOSING) {
    if (rc == BLE_HS_ENOTCONN) {
      fr_esp_ble.connection_disconnects += 1u;
      fr_esp_ble_connection_free_locked();
    } else if (rc != 0) {
      fr_esp_ble.connection.state = FR_BLE_CONNECTION_LIVE;
    } else {
      fr_esp_ble.connection.has_runtime_ref = false;
    }
  }
  err = rc == 0 || rc == BLE_HS_ENOTCONN ? FR_OK
                                          : fr_esp_ble_host_error(rc);
  fr_esp_ble_record_locked(FR_BLE_OP_CONNECTION_CLOSE, err,
                           err == FR_OK ? 0 : rc);
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return err;
}

fr_err_t fr_platform_ble_connection_info(
    uint16_t platform_index, fr_ble_connection_info_t *out_info) {
  fr_err_t err = FR_OK;

  if (out_info == NULL) {
    return FR_ERR_INVALID;
  }
  portENTER_CRITICAL(&fr_esp_ble_lock);
  err = fr_esp_ble_connection_entry_locked(platform_index);
  if (err == FR_OK) {
    fr_esp_ble_connection_info_locked(out_info);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return err;
}

fr_err_t fr_platform_ble_connection_rssi(uint16_t platform_index,
                                         int8_t *out_rssi) {
  fr_err_t err = FR_OK;
  uint32_t generation = 0;
  uint16_t stack_handle = BLE_HS_CONN_HANDLE_NONE;
  int8_t rssi = 0;
  int rc = 0;

  if (out_rssi == NULL) {
    return FR_ERR_INVALID;
  }
  portENTER_CRITICAL(&fr_esp_ble_lock);
  err = fr_esp_ble_connection_entry_locked(platform_index);
  if (err == FR_OK &&
      fr_esp_ble.connection.state != FR_BLE_CONNECTION_LIVE) {
    err = FR_ERR_BLE_DISCONNECTED;
  }
  if (err == FR_OK) {
    generation = fr_esp_ble.connection.generation;
    stack_handle = fr_esp_ble.connection.stack_handle;
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (err != FR_OK) {
    return err;
  }

  rc = ble_gap_conn_rssi(stack_handle, &rssi);
  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble_connection_event_current_locked(generation,
                                                   stack_handle) ||
      fr_esp_ble.connection.state != FR_BLE_CONNECTION_LIVE) {
    err = FR_ERR_BLE_DISCONNECTED;
  } else if (rc == BLE_HS_ENOTCONN) {
    fr_esp_ble_connection_mark_disconnected_locked(NULL, rc);
    err = FR_ERR_BLE_DISCONNECTED;
  } else if (rc != 0) {
    err = fr_esp_ble_host_error(rc);
  } else if (rssi < FR_ESP_BLE_RSSI_MIN || rssi > FR_ESP_BLE_RSSI_MAX) {
    fr_esp_ble.connection.rssi_valid = false;
    err = FR_ERR_NOT_FOUND;
  } else {
    fr_esp_ble.connection.rssi_valid = true;
    fr_esp_ble.connection.last_rssi = rssi;
    *out_rssi = rssi;
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return err;
}

fr_err_t fr_platform_ble_connection_params(
    uint16_t platform_index, uint16_t minimum_interval_ms,
    uint16_t maximum_interval_ms, uint16_t latency,
    uint16_t supervision_timeout_ms) {
  struct ble_gap_upd_params parameters = {0};
  fr_err_t err = FR_OK;
  uint32_t generation = 0;
  uint16_t stack_handle = BLE_HS_CONN_HANDLE_NONE;
  uint16_t minimum_interval_units =
      fr_esp_ble_connection_interval_units(minimum_interval_ms);
  uint16_t maximum_interval_units =
      fr_esp_ble_connection_interval_units(maximum_interval_ms);
  uint16_t supervision_timeout_units =
      fr_esp_ble_supervision_timeout_units(supervision_timeout_ms);
  int rc = 0;

  if (minimum_interval_units < FR_ESP_BLE_CONNECTION_INTERVAL_MIN_UNITS ||
      maximum_interval_units > FR_ESP_BLE_CONNECTION_INTERVAL_MAX_UNITS ||
      minimum_interval_units > maximum_interval_units || latency > 499u ||
      supervision_timeout_units <
          FR_ESP_BLE_SUPERVISION_TIMEOUT_MIN_UNITS ||
      supervision_timeout_units >
          FR_ESP_BLE_SUPERVISION_TIMEOUT_MAX_UNITS ||
      (uint32_t)supervision_timeout_units * 8u <=
          ((uint32_t)latency + 1u) * maximum_interval_units * 2u) {
    return FR_ERR_RANGE;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  err = fr_esp_ble_connection_entry_locked(platform_index);
  if (err == FR_OK &&
      fr_esp_ble.connection.state != FR_BLE_CONNECTION_LIVE) {
    err = FR_ERR_BLE_DISCONNECTED;
  }
  if (err == FR_OK && fr_esp_ble.connection.params_pending) {
    err = FR_ERR_BLE_BUSY;
  }
  if (err != FR_OK) {
    fr_esp_ble_record_locked(FR_BLE_OP_CONNECTION_PARAMS, err, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return err;
  }

  generation = fr_esp_ble.connection.generation;
  stack_handle = fr_esp_ble.connection.stack_handle;
  fr_esp_ble.connection.requested_minimum_interval_ms = minimum_interval_ms;
  fr_esp_ble.connection.requested_maximum_interval_ms = maximum_interval_ms;
  fr_esp_ble.connection.requested_latency = latency;
  fr_esp_ble.connection.requested_supervision_timeout_ms =
      supervision_timeout_ms;
  fr_esp_ble.connection.params_pending = true;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  parameters.itvl_min = minimum_interval_units;
  parameters.itvl_max = maximum_interval_units;
  parameters.latency = latency;
  parameters.supervision_timeout = supervision_timeout_units;
  rc = ble_gap_update_params(stack_handle, &parameters);

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (generation == fr_esp_ble.connection.generation &&
      fr_esp_ble.connection.stack_handle == stack_handle && rc != 0) {
    fr_esp_ble.connection.params_pending = false;
    if (rc == BLE_HS_ENOTCONN &&
        fr_esp_ble.connection.state == FR_BLE_CONNECTION_LIVE) {
      fr_esp_ble_connection_mark_disconnected_locked(NULL, rc);
    }
  }
  err = fr_esp_ble_host_error(rc);
  fr_esp_ble_record_locked(FR_BLE_OP_CONNECTION_PARAMS, err, rc);
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return err;
}

fr_err_t fr_platform_ble_connection_mtu(fr_runtime_t *runtime,
                                        uint16_t platform_index,
                                        uint16_t requested_mtu,
                                        uint16_t timeout_ms,
                                        uint16_t *out_actual_mtu) {
  fr_err_t err = FR_OK;
  uint64_t start_us = 0;
  uint64_t budget_us = 0;
  uint32_t generation = 0;
  uint16_t stack_handle = BLE_HS_CONN_HANDLE_NONE;
  int rc = 0;

  if (runtime == NULL || out_actual_mtu == NULL) {
    return FR_ERR_INVALID;
  }
  if (requested_mtu != CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU ||
      timeout_ms == 0 || timeout_ms > 60000u) {
    return FR_ERR_RANGE;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  err = fr_esp_ble_connection_entry_locked(platform_index);
  if (err == FR_OK &&
      fr_esp_ble.connection.state != FR_BLE_CONNECTION_LIVE) {
    err = FR_ERR_BLE_DISCONNECTED;
  }
  if (err == FR_OK &&
      fr_esp_ble.connection.role == FR_BLE_CONNECTION_ROLE_PERIPHERAL) {
    *out_actual_mtu = fr_esp_ble.connection.mtu;
    fr_esp_ble_record_locked(FR_BLE_OP_CONNECTION_MTU, FR_OK, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_OK;
  }
#if FR_BLE_ENABLE_GATT_CLIENT
  if (err == FR_OK && fr_esp_ble.gatt_client.procedure.pending) {
    err = FR_ERR_BLE_BUSY;
  }
#endif
  if (err == FR_OK && fr_esp_ble.connection.mtu_pending) {
    err = FR_ERR_BLE_BUSY;
  }
  if (err != FR_OK) {
    fr_esp_ble_record_locked(FR_BLE_OP_CONNECTION_MTU, err, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return err;
  }

  generation = fr_esp_ble.connection.generation;
  stack_handle = fr_esp_ble.connection.stack_handle;
  fr_esp_ble.connection.mtu_pending = true;
  fr_esp_ble.connection.mtu_status = 0;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  rc = ble_gattc_exchange_mtu(stack_handle, fr_esp_ble_mtu_complete,
                              (void *)(uintptr_t)generation);
  if (rc == BLE_HS_EALREADY) {
    uint16_t actual_mtu = ble_att_mtu(stack_handle);

    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (fr_esp_ble_connection_event_current_locked(generation,
                                                    stack_handle) &&
        fr_esp_ble.connection.state == FR_BLE_CONNECTION_LIVE) {
      fr_esp_ble.connection.mtu_pending = false;
      fr_esp_ble.connection.mtu_status = 0;
      fr_esp_ble.connection.mtu = actual_mtu;
      *out_actual_mtu = actual_mtu;
      fr_esp_ble_record_locked(FR_BLE_OP_CONNECTION_MTU, FR_OK, 0);
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return FR_OK;
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_DISCONNECTED;
  }
  if (rc != 0) {
    err = fr_esp_ble_host_error(rc);
    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (generation == fr_esp_ble.connection.generation &&
        fr_esp_ble.connection.stack_handle == stack_handle) {
      fr_esp_ble.connection.mtu_pending = false;
      fr_esp_ble.connection.mtu_status = rc;
    }
    fr_esp_ble_record_locked(FR_BLE_OP_CONNECTION_MTU, err, rc);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return err;
  }

  start_us = (uint64_t)esp_timer_get_time();
  budget_us = (uint64_t)timeout_ms * 1000u;
  for (;;) {
    fr_ble_connection_state_t state = FR_BLE_CONNECTION_FREE;
    bool pending = false;
    int32_t status = 0;
    uint16_t actual_mtu = BLE_ATT_MTU_DFLT;

    portENTER_CRITICAL(&fr_esp_ble_lock);
    if (generation == fr_esp_ble.connection.generation) {
      state = fr_esp_ble.connection.state;
      pending = fr_esp_ble.connection.mtu_pending;
      status = fr_esp_ble.connection.mtu_status;
      actual_mtu = fr_esp_ble.connection.mtu;
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);

    if (state != FR_BLE_CONNECTION_LIVE) {
      portENTER_CRITICAL(&fr_esp_ble_lock);
      fr_esp_ble_record_locked(FR_BLE_OP_CONNECTION_MTU,
                               FR_ERR_BLE_DISCONNECTED, status);
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return FR_ERR_BLE_DISCONNECTED;
    }
    if (!pending) {
      err = fr_esp_ble_host_error(status);
      portENTER_CRITICAL(&fr_esp_ble_lock);
      fr_esp_ble_record_locked(FR_BLE_OP_CONNECTION_MTU, err, status);
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      if (err == FR_OK) {
        *out_actual_mtu = actual_mtu;
      }
      return err;
    }

    err = fr_platform_poll_interrupt(runtime);
    if (err == FR_OK && fr_runtime_is_interrupted(runtime)) {
      err = FR_ERR_INTERRUPTED;
    }
    if (err == FR_OK &&
        (uint64_t)esp_timer_get_time() - start_us > budget_us) {
      err = FR_ERR_BLE_TIMEOUT;
    }
    if (err != FR_OK) {
      portENTER_CRITICAL(&fr_esp_ble_lock);
      fr_esp_ble_record_locked(FR_BLE_OP_CONNECTION_MTU, err, 0);
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return err;
    }
    vTaskDelay(FR_ESP_BLE_WAIT_TICKS);
  }
}
#endif

#if FR_BLE_ENABLE_GATT_CLIENT
fr_err_t fr_platform_ble_gatt_client_find(
    fr_runtime_t *runtime, uint16_t connection_index,
    const fr_ble_uuid_t *service_uuid,
    const fr_ble_uuid_t *characteristic_uuid, uint16_t timeout_ms,
    uint16_t *out_attribute_handle) {
  fr_err_t err = FR_OK;
  uint64_t deadline_us = 0;
  uint32_t generation = 0;
  uint16_t stack_handle = BLE_HS_CONN_HANDLE_NONE;
  uint16_t start_handle = 0;
  uint16_t end_handle = 0;
  int rc = 0;
  bool launch = false;

  if (runtime == NULL || service_uuid == NULL || characteristic_uuid == NULL ||
      out_attribute_handle == NULL) {
    return FR_ERR_INVALID;
  }
  if ((service_uuid->kind != FR_BLE_UUID_16 &&
       service_uuid->kind != FR_BLE_UUID_128) ||
      (characteristic_uuid->kind != FR_BLE_UUID_16 &&
       characteristic_uuid->kind != FR_BLE_UUID_128) ||
      timeout_ms == 0 || timeout_ms > FR_BLE_GATT_CLIENT_TIMEOUT_MAX_MS) {
    return FR_ERR_RANGE;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  err = fr_esp_ble_gatt_client_check_locked(connection_index);
  if (err == FR_OK) {
    generation = fr_esp_ble_gatt_client_start_locked(
        FR_BLE_OP_GATT_FIND, FR_ESP_BLE_GATT_CLIENT_STAGE_SERVICE, 0);
    stack_handle = fr_esp_ble.gatt_client.procedure.stack_handle;
    fr_esp_ble_gatt_uuid_build(
        service_uuid, &fr_esp_ble.gatt_client.procedure.service_uuid);
    fr_esp_ble_gatt_uuid_build(
        characteristic_uuid,
        &fr_esp_ble.gatt_client.procedure.characteristic_uuid);
    fr_esp_ble.gatt_client.service_match_count = 0;
    fr_esp_ble.gatt_client.characteristic_match_count = 0;
  } else {
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_FIND, err, 0);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (err != FR_OK) {
    return err;
  }
  deadline_us = (uint64_t)esp_timer_get_time() +
                (uint64_t)timeout_ms * 1000u;

  rc = ble_gattc_disc_svc_by_uuid(
      stack_handle, &fr_esp_ble.gatt_client.procedure.service_uuid.u,
      fr_esp_ble_gatt_client_service_discovered,
      (void *)(uintptr_t)generation);
  if (rc != 0) {
    fr_esp_ble_gatt_client_launch_failed(
        generation, FR_ESP_BLE_GATT_CLIENT_STAGE_SERVICE, rc);
  }
  err = fr_esp_ble_gatt_client_wait(runtime, generation,
                                    FR_BLE_OP_GATT_FIND, deadline_us, true);
  if (err != FR_OK) {
    return err;
  }

  /* NimBLE frees its one procedure record immediately after the terminal
   * callback returns. Yield once before asking that same pool for the next
   * discovery stage. */
  vTaskDelay(FR_ESP_BLE_WAIT_TICKS);
  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (fr_esp_ble_gatt_client_callback_current_locked(
          generation, stack_handle, FR_ESP_BLE_GATT_CLIENT_STAGE_SERVICE)) {
    start_handle = fr_esp_ble.gatt_client.procedure.service_start_handle;
    end_handle = fr_esp_ble.gatt_client.procedure.service_end_handle;
    fr_esp_ble.gatt_client.procedure.stage =
        FR_ESP_BLE_GATT_CLIENT_STAGE_CHARACTERISTIC;
    fr_esp_ble.gatt_client.procedure.stage_complete = false;
    launch = true;
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (launch) {
    rc = ble_gattc_disc_all_chrs(
        stack_handle, start_handle, end_handle,
        fr_esp_ble_gatt_client_characteristic_discovered,
        (void *)(uintptr_t)generation);
    if (rc != 0) {
      fr_esp_ble_gatt_client_launch_failed(
          generation, FR_ESP_BLE_GATT_CLIENT_STAGE_CHARACTERISTIC, rc);
    }
  }
  err = fr_esp_ble_gatt_client_wait(runtime, generation,
                                    FR_BLE_OP_GATT_FIND, deadline_us, true);
  if (err != FR_OK) {
    return err;
  }

  vTaskDelay(FR_ESP_BLE_WAIT_TICKS);
  launch = false;
  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (fr_esp_ble_gatt_client_callback_current_locked(
          generation, stack_handle,
          FR_ESP_BLE_GATT_CLIENT_STAGE_CHARACTERISTIC)) {
    start_handle = fr_esp_ble.gatt_client.procedure.value_handle;
    end_handle =
        fr_esp_ble.gatt_client.procedure.characteristic_end_handle;
    if ((fr_esp_ble.gatt_client.procedure.properties &
         (FR_BLE_GATT_CHR_NOTIFY | FR_BLE_GATT_CHR_INDICATE)) == 0 ||
        start_handle >= end_handle) {
      fr_esp_ble_gatt_client_finish_find_locked();
    } else {
      fr_esp_ble.gatt_client.procedure.stage =
          FR_ESP_BLE_GATT_CLIENT_STAGE_DESCRIPTOR;
      fr_esp_ble.gatt_client.procedure.stage_complete = false;
      launch = true;
    }
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (launch) {
    rc = ble_gattc_disc_all_dscs(
        stack_handle, start_handle, end_handle,
        fr_esp_ble_gatt_client_descriptor_discovered,
        (void *)(uintptr_t)generation);
    if (rc != 0) {
      fr_esp_ble_gatt_client_launch_failed(
          generation, FR_ESP_BLE_GATT_CLIENT_STAGE_DESCRIPTOR, rc);
    }
  }
  err = fr_esp_ble_gatt_client_wait(runtime, generation,
                                    FR_BLE_OP_GATT_FIND, deadline_us, false);
  if (err == FR_OK) {
    portENTER_CRITICAL(&fr_esp_ble_lock);
    *out_attribute_handle =
        fr_esp_ble.gatt_client.procedure.attribute_handle;
    portEXIT_CRITICAL(&fr_esp_ble_lock);
  }
  return err;
}

fr_err_t fr_platform_ble_gatt_client_read(
    fr_runtime_t *runtime, uint16_t connection_index,
    uint16_t attribute_handle, uint16_t timeout_ms, uint8_t *out_bytes,
    uint16_t capacity, uint16_t *out_length) {
  fr_esp_ble_gatt_client_cache_t *entry = NULL;
  fr_err_t err = FR_OK;
  uint32_t generation = 0;
  uint16_t stack_handle = BLE_HS_CONN_HANDLE_NONE;
  int rc = 0;

  if (runtime == NULL || out_bytes == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }
  if (attribute_handle == 0 || timeout_ms == 0 ||
      timeout_ms > FR_BLE_GATT_CLIENT_TIMEOUT_MAX_MS) {
    return FR_ERR_RANGE;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  err = fr_esp_ble_gatt_client_check_locked(connection_index);
  if (err == FR_OK) {
    entry = fr_esp_ble_gatt_client_cache_locked(attribute_handle);
    if (entry == NULL) {
      err = FR_ERR_NOT_FOUND;
      fr_esp_ble.gatt_client.last_att_error = BLE_ATT_ERR_INVALID_HANDLE;
    } else if ((entry->properties & FR_BLE_GATT_CHR_READ) == 0) {
      err = FR_ERR_UNSUPPORTED;
      fr_esp_ble.gatt_client.last_att_error =
          BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
  }
  if (err == FR_OK) {
    generation = fr_esp_ble_gatt_client_start_locked(
        FR_BLE_OP_GATT_READ, FR_ESP_BLE_GATT_CLIENT_STAGE_READ,
        attribute_handle);
    stack_handle = fr_esp_ble.gatt_client.procedure.stack_handle;
  } else {
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_READ, err, 0);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (err != FR_OK) {
    return err;
  }

  rc = ble_gattc_read(stack_handle, attribute_handle,
                      fr_esp_ble_gatt_client_read_complete,
                      (void *)(uintptr_t)generation);
  if (rc != 0) {
    fr_esp_ble_gatt_client_launch_failed(
        generation, FR_ESP_BLE_GATT_CLIENT_STAGE_READ, rc);
  }
  err = fr_esp_ble_gatt_client_wait(runtime, generation,
                                    FR_BLE_OP_GATT_READ,
                                    (uint64_t)esp_timer_get_time() +
                                        (uint64_t)timeout_ms * 1000u,
                                    false);
  if (err != FR_OK) {
    return err;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (fr_esp_ble.gatt_client.procedure.data_length > capacity) {
    err = FR_ERR_CAPACITY;
  } else {
    *out_length = fr_esp_ble.gatt_client.procedure.data_length;
    memcpy(out_bytes, fr_esp_ble.gatt_client.procedure.data, *out_length);
  }
  if (err != FR_OK) {
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_READ, err, BLE_HS_EMSGSIZE);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return err;
}

fr_err_t fr_platform_ble_gatt_client_write(
    fr_runtime_t *runtime, uint16_t connection_index,
    uint16_t attribute_handle, const uint8_t *bytes, uint16_t length,
    bool with_response, uint16_t timeout_ms) {
  fr_esp_ble_gatt_client_cache_t *entry = NULL;
  uint16_t required_property = with_response ? FR_BLE_GATT_CHR_WRITE
                                             : FR_BLE_GATT_CHR_WRITE_COMMAND;
  fr_err_t err = FR_OK;
  uint32_t generation = 0;
  uint16_t stack_handle = BLE_HS_CONN_HANDLE_NONE;
  int rc = 0;

  if (runtime == NULL || (length > 0 && bytes == NULL)) {
    return FR_ERR_INVALID;
  }
  if (attribute_handle == 0 || length > FR_BLE_GATT_CLIENT_DATA_BYTES ||
      timeout_ms == 0 || timeout_ms > FR_BLE_GATT_CLIENT_TIMEOUT_MAX_MS) {
    return FR_ERR_RANGE;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  err = fr_esp_ble_gatt_client_check_locked(connection_index);
  if (err == FR_OK) {
    entry = fr_esp_ble_gatt_client_cache_locked(attribute_handle);
    if (entry == NULL) {
      err = FR_ERR_NOT_FOUND;
      fr_esp_ble.gatt_client.last_att_error = BLE_ATT_ERR_INVALID_HANDLE;
    } else if ((entry->properties & required_property) == 0) {
      err = FR_ERR_UNSUPPORTED;
      fr_esp_ble.gatt_client.last_att_error =
          BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
  }
  if (err == FR_OK) {
    stack_handle = fr_esp_ble.connection.stack_handle;
    if (with_response) {
      generation = fr_esp_ble_gatt_client_start_locked(
          FR_BLE_OP_GATT_WRITE, FR_ESP_BLE_GATT_CLIENT_STAGE_WRITE,
          attribute_handle);
    }
  } else {
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_WRITE, err, 0);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (err != FR_OK) {
    return err;
  }

  if (!with_response) {
    rc = ble_gattc_write_no_rsp_flat(stack_handle, attribute_handle, bytes,
                                     length);
    err = fr_esp_ble_gatt_client_error(rc);
    portENTER_CRITICAL(&fr_esp_ble_lock);
    fr_esp_ble.gatt_client.last_att_error =
        fr_esp_ble_gatt_client_att_error(rc);
    fr_esp_ble.gatt_client.last_platform_code = rc == 0 ? 0 : rc;
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_WRITE, err, rc);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return err;
  }

  rc = ble_gattc_write_flat(stack_handle, attribute_handle, bytes, length,
                            fr_esp_ble_gatt_client_write_complete,
                            (void *)(uintptr_t)generation);
  if (rc != 0) {
    fr_esp_ble_gatt_client_launch_failed(
        generation, FR_ESP_BLE_GATT_CLIENT_STAGE_WRITE, rc);
  }
  return fr_esp_ble_gatt_client_wait(runtime, generation,
                                     FR_BLE_OP_GATT_WRITE,
                                     (uint64_t)esp_timer_get_time() +
                                         (uint64_t)timeout_ms * 1000u,
                                     false);
}

static fr_err_t fr_esp_ble_gatt_client_subscription(
    fr_runtime_t *runtime, uint16_t connection_index,
    uint16_t attribute_handle, fr_ble_gatt_subscription_mode_t mode,
    uint16_t timeout_ms, fr_ble_operation_t operation) {
  fr_esp_ble_gatt_client_cache_t *entry = NULL;
  uint16_t required_property =
      mode == FR_BLE_GATT_SUBSCRIBE_NOTIFICATIONS
          ? FR_BLE_GATT_CHR_NOTIFY
          : FR_BLE_GATT_CHR_INDICATE;
  uint8_t cccd_value[2] = {(uint8_t)mode, 0};
  fr_err_t err = FR_OK;
  uint32_t generation = 0;
  uint16_t stack_handle = BLE_HS_CONN_HANDLE_NONE;
  uint16_t cccd_handle = 0;
  int rc = 0;

  if (runtime == NULL || attribute_handle == 0 || timeout_ms == 0 ||
      timeout_ms > FR_BLE_GATT_CLIENT_TIMEOUT_MAX_MS ||
      (mode != 0 && mode != FR_BLE_GATT_SUBSCRIBE_NOTIFICATIONS &&
       mode != FR_BLE_GATT_SUBSCRIBE_INDICATIONS)) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  err = fr_esp_ble_gatt_client_check_locked(connection_index);
  if (err == FR_OK) {
    entry = fr_esp_ble_gatt_client_cache_locked(attribute_handle);
    if (entry == NULL || entry->cccd_handle == 0) {
      err = FR_ERR_NOT_FOUND;
    } else if (mode != 0 && (entry->properties & required_property) == 0) {
      err = FR_ERR_UNSUPPORTED;
    }
  }
  if (err == FR_OK) {
    generation = fr_esp_ble_gatt_client_start_locked(
        operation, FR_ESP_BLE_GATT_CLIENT_STAGE_SUBSCRIPTION,
        attribute_handle);
    fr_esp_ble.gatt_client.procedure.subscription_mode = mode;
    stack_handle = fr_esp_ble.gatt_client.procedure.stack_handle;
    cccd_handle = entry->cccd_handle;
  } else {
    fr_esp_ble_record_locked(operation, err, 0);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (err != FR_OK) {
    return err;
  }

  rc = ble_gattc_write_flat(stack_handle, cccd_handle, cccd_value,
                            sizeof(cccd_value),
                            fr_esp_ble_gatt_client_subscription_complete,
                            (void *)(uintptr_t)generation);
  if (rc != 0) {
    fr_esp_ble_gatt_client_launch_failed(
        generation, FR_ESP_BLE_GATT_CLIENT_STAGE_SUBSCRIPTION, rc);
  }
  return fr_esp_ble_gatt_client_wait(runtime, generation, operation,
                                     (uint64_t)esp_timer_get_time() +
                                         (uint64_t)timeout_ms * 1000u,
                                     false);
}

fr_err_t fr_platform_ble_gatt_client_subscribe(
    fr_runtime_t *runtime, uint16_t connection_index,
    uint16_t attribute_handle, fr_ble_gatt_subscription_mode_t mode,
    uint16_t timeout_ms) {
  if (mode != FR_BLE_GATT_SUBSCRIBE_NOTIFICATIONS &&
      mode != FR_BLE_GATT_SUBSCRIBE_INDICATIONS) {
    return FR_ERR_INVALID;
  }
  return fr_esp_ble_gatt_client_subscription(
      runtime, connection_index, attribute_handle, mode, timeout_ms,
      FR_BLE_OP_GATT_SUBSCRIBE);
}

fr_err_t fr_platform_ble_gatt_client_unsubscribe(
    fr_runtime_t *runtime, uint16_t connection_index,
    uint16_t attribute_handle, uint16_t timeout_ms) {
  return fr_esp_ble_gatt_client_subscription(
      runtime, connection_index, attribute_handle, 0, timeout_ms,
      FR_BLE_OP_GATT_UNSUBSCRIBE);
}

fr_err_t fr_platform_ble_gatt_notification_next(
    bool *out_has_notification, fr_handle_ref_t *out_runtime_ref) {
  if (out_has_notification == NULL || out_runtime_ref == NULL) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  *out_has_notification = false;
  *out_runtime_ref = (fr_handle_ref_t){0};
  fr_esp_ble.gatt_client.current_notification_valid = false;
  memset(&fr_esp_ble.gatt_client.current_notification, 0,
         sizeof(fr_esp_ble.gatt_client.current_notification));
  while (fr_esp_ble.gatt_client.notification_count > 0) {
    fr_ble_gatt_notification_t notification =
        fr_esp_ble.gatt_client
            .notifications[fr_esp_ble.gatt_client.notification_head];

    fr_esp_ble.gatt_client.notification_head =
        (uint8_t)((fr_esp_ble.gatt_client.notification_head + 1u) %
                  FR_BLE_GATT_NOTIFICATION_QUEUE_COUNT);
    fr_esp_ble.gatt_client.notification_count -= 1u;
    if (notification.connection_index != FR_ESP_BLE_CONNECTION_INDEX ||
        notification.connection_generation !=
            fr_esp_ble.connection.generation ||
        !fr_esp_ble.connection.has_runtime_ref ||
        fr_esp_ble.connection.state == FR_BLE_CONNECTION_FREE) {
      fr_esp_ble.gatt_client.notification_stale += 1u;
      continue;
    }

    fr_esp_ble.gatt_client.current_notification = notification;
    fr_esp_ble.gatt_client.current_notification_valid = true;
    *out_runtime_ref = fr_esp_ble.connection.runtime_ref;
    *out_has_notification = true;
    break;
  }
  fr_esp_ble_record_locked(FR_BLE_OP_GATT_NOTIFICATION_NEXT, FR_OK, 0);
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_OK;
}

fr_err_t fr_platform_ble_gatt_notification_current(
    fr_ble_gatt_notification_t *out_notification) {
  if (out_notification == NULL) {
    return FR_ERR_INVALID;
  }
  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble.gatt_client.current_notification_valid) {
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_NOT_FOUND;
  }
  *out_notification = fr_esp_ble.gatt_client.current_notification;
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_OK;
}

fr_err_t fr_platform_ble_gatt_client_status(
    fr_ble_gatt_client_status_t *out_status) {
  if (out_status == NULL) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  *out_status = (fr_ble_gatt_client_status_t){
      .cache_count = fr_esp_ble.gatt_client.cache_count,
      .subscription_count =
          fr_esp_ble_gatt_client_subscription_count_locked(),
      .notification_queue_count =
          fr_esp_ble.gatt_client.notification_count,
      .notification_queue_high_water =
          fr_esp_ble.gatt_client.notification_high_water,
      .notification_dropped =
          fr_esp_ble.gatt_client.notification_dropped,
      .notification_stale = fr_esp_ble.gatt_client.notification_stale,
      .current_notification_valid =
          fr_esp_ble.gatt_client.current_notification_valid,
      .current_notification_attribute_handle =
          fr_esp_ble.gatt_client.current_notification.attribute_handle,
      .current_notification_data_length =
          fr_esp_ble.gatt_client.current_notification.data_length,
      .current_notification_indication =
          fr_esp_ble.gatt_client.current_notification.indication,
      .procedure_pending = fr_esp_ble.gatt_client.procedure.pending,
      .procedure_operation = fr_esp_ble.gatt_client.procedure.pending
                                 ? fr_esp_ble.gatt_client.procedure.operation
                                 : FR_BLE_OP_NONE,
      .procedure_attribute_handle =
          fr_esp_ble.gatt_client.procedure.attribute_handle,
      .service_match_count = fr_esp_ble.gatt_client.service_match_count,
      .characteristic_match_count =
          fr_esp_ble.gatt_client.characteristic_match_count,
      .last_att_error = fr_esp_ble.gatt_client.last_att_error,
      .last_platform_code = fr_esp_ble.gatt_client.last_platform_code,
  };
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_OK;
}
#endif

#if FR_BLE_ENABLE_GATT_SERVER
fr_err_t fr_platform_ble_gatt_install(const fr_ble_gatt_table_t *table) {
  fr_ble_gatt_table_t copy;
  uint32_t generation = 0;

  if (table == NULL) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (fr_esp_ble.radio_state != FR_BLE_RADIO_OFF) {
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_INSTALL, FR_ERR_BLE_BUSY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_BUSY;
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  if (!fr_esp_ble_gatt_table_valid(table)) {
    portENTER_CRITICAL(&fr_esp_ble_lock);
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_INSTALL, FR_ERR_INVALID, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_INVALID;
  }

  copy = *table;
  for (uint16_t i = 0; i < copy.characteristic_count; i++) {
    copy.characteristics[i].value_length = 0;
    copy.characteristics[i].target_value_handle = 0;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (fr_esp_ble.radio_state != FR_BLE_RADIO_OFF) {
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_INSTALL, FR_ERR_BLE_BUSY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_BUSY;
  }
  generation = fr_esp_ble.gatt.table_generation + 1u;
  memset(&fr_esp_ble.gatt, 0, sizeof(fr_esp_ble.gatt));
  fr_esp_ble.gatt.table = copy;
  fr_esp_ble.gatt.table_generation = generation;
  fr_esp_ble_gatt_build_definitions_locked();
  fr_esp_ble_record_locked(FR_BLE_OP_GATT_INSTALL, FR_OK, 0);
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_OK;
}

fr_err_t fr_platform_ble_gatt_status(fr_ble_gatt_status_t *out_status) {
  if (out_status == NULL) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  *out_status = (fr_ble_gatt_status_t){
      .table = fr_esp_ble.gatt.table,
      .table_generation = fr_esp_ble.gatt.table_generation,
      .subscription_count = fr_esp_ble.gatt.subscription_count,
      .write_queue_count = fr_esp_ble.gatt.write_count,
      .write_queue_high_water = fr_esp_ble.gatt.write_high_water,
      .write_queue_overflow = fr_esp_ble.gatt.write_overflow,
      .write_queue_stale = fr_esp_ble.gatt.write_stale,
      .preaccept_write_rejected =
          fr_esp_ble.gatt.preaccept_write_rejected,
      .current_write_valid = fr_esp_ble.gatt.current_write_valid,
      .current_write_attribute_id =
          fr_esp_ble.gatt.current_write.attribute_id,
      .current_write_data_length =
          fr_esp_ble.gatt.current_write.data_length,
      .indication_pending = fr_esp_ble.gatt.indication_pending,
      .last_att_error = fr_esp_ble.gatt.last_att_error,
      .last_platform_code = fr_esp_ble.gatt.last_platform_code,
  };
  memcpy(out_status->subscriptions, fr_esp_ble.gatt.subscriptions,
         sizeof(out_status->subscriptions));
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_OK;
}

fr_err_t fr_platform_ble_gatt_set(uint16_t attribute_id,
                                  const uint8_t *bytes, uint16_t length) {
  fr_ble_gatt_characteristic_row_t *characteristic = NULL;

  if (length > 0 && bytes == NULL) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  characteristic = fr_esp_ble_gatt_characteristic_locked(attribute_id);
  if (characteristic == NULL) {
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_SET, FR_ERR_NOT_FOUND, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_NOT_FOUND;
  }
  if (length > characteristic->maximum_length) {
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_SET, FR_ERR_CAPACITY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_CAPACITY;
  }

  if (length > 0) {
    memcpy(&fr_esp_ble.gatt.value_bytes[characteristic->value_offset], bytes,
           length);
  }
  characteristic->value_length = length;
  fr_esp_ble_record_locked(FR_BLE_OP_GATT_SET, FR_OK, 0);
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_OK;
}

fr_err_t fr_platform_ble_gatt_get(uint16_t attribute_id, uint8_t *out_bytes,
                                  uint16_t capacity, uint16_t *out_length) {
  fr_ble_gatt_characteristic_row_t *characteristic = NULL;

  if (out_bytes == NULL || out_length == NULL) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  characteristic = fr_esp_ble_gatt_characteristic_locked(attribute_id);
  if (characteristic == NULL) {
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_NOT_FOUND;
  }
  if (characteristic->value_length > capacity) {
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_CAPACITY;
  }

  if (characteristic->value_length > 0) {
    memcpy(out_bytes,
           &fr_esp_ble.gatt.value_bytes[characteristic->value_offset],
           characteristic->value_length);
  }
  *out_length = characteristic->value_length;
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_OK;
}

static fr_err_t fr_esp_ble_gatt_send_check_locked(
    uint16_t connection_index, uint16_t attribute_id, uint16_t length,
    uint16_t required_flag, bool require_indicate,
    uint32_t *out_connection_generation, uint16_t *out_stack_handle,
    uint16_t *out_target_handle) {
  fr_ble_gatt_characteristic_row_t *characteristic = NULL;
  fr_ble_gatt_subscription_t *subscription = NULL;
  fr_err_t err = fr_esp_ble_connection_entry_locked(connection_index);

  if (err != FR_OK) {
    return err;
  }
  if (fr_esp_ble.connection.state != FR_BLE_CONNECTION_LIVE ||
      fr_esp_ble.connection.role != FR_BLE_CONNECTION_ROLE_PERIPHERAL) {
    return FR_ERR_BLE_DISCONNECTED;
  }
  if (fr_esp_ble.radio_state != FR_BLE_RADIO_READY) {
    return FR_ERR_BLE_NOT_READY;
  }

  characteristic = fr_esp_ble_gatt_characteristic_locked(attribute_id);
  if (characteristic == NULL) {
    return FR_ERR_NOT_FOUND;
  }
  if ((characteristic->portable_flags & required_flag) == 0) {
    return FR_ERR_UNSUPPORTED;
  }
  if (characteristic->target_value_handle == 0 ||
      fr_esp_ble.connection.mtu < 3u) {
    return FR_ERR_BLE_NOT_READY;
  }
  if (length > characteristic->maximum_length ||
      length > (uint16_t)(fr_esp_ble.connection.mtu - 3u)) {
    return FR_ERR_CAPACITY;
  }

  subscription =
      fr_esp_ble_gatt_subscription_locked(characteristic->attribute_id);
  if (subscription == NULL ||
      (require_indicate ? !subscription->indicate : !subscription->notify)) {
    return FR_ERR_BLE_NOT_READY;
  }

  *out_connection_generation = fr_esp_ble.connection.generation;
  *out_stack_handle = fr_esp_ble.connection.stack_handle;
  *out_target_handle = characteristic->target_value_handle;
  return FR_OK;
}

fr_err_t fr_platform_ble_gatt_notify(uint16_t connection_index,
                                     uint16_t attribute_id,
                                     const uint8_t *bytes, uint16_t length) {
  uint8_t empty = 0;
  uint32_t connection_generation = 0;
  uint16_t stack_handle = BLE_HS_CONN_HANDLE_NONE;
  uint16_t target_handle = 0;
  struct os_mbuf *packet = NULL;
  fr_err_t err = FR_OK;
  int rc = 0;

  if (length > 0 && bytes == NULL) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  err = fr_esp_ble_gatt_send_check_locked(
      connection_index, attribute_id, length, FR_BLE_GATT_CHR_NOTIFY, false,
      &connection_generation, &stack_handle, &target_handle);
  if (err != FR_OK) {
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_NOTIFY, err, 0);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (err != FR_OK) {
    return err;
  }

  packet = ble_hs_mbuf_from_flat(length > 0 ? bytes : &empty, length);
  if (packet == NULL) {
    portENTER_CRITICAL(&fr_esp_ble_lock);
    fr_esp_ble.gatt.last_platform_code = BLE_HS_ENOMEM;
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_NOTIFY, FR_ERR_CAPACITY,
                             BLE_HS_ENOMEM);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_CAPACITY;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  err = fr_esp_ble_gatt_send_check_locked(
      connection_index, attribute_id, length, FR_BLE_GATT_CHR_NOTIFY, false,
      &connection_generation, &stack_handle, &target_handle);
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (err != FR_OK) {
    os_mbuf_free_chain(packet);
    portENTER_CRITICAL(&fr_esp_ble_lock);
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_NOTIFY, err, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return err;
  }

  rc = ble_gatts_notify_custom(stack_handle, target_handle, packet);
  err = fr_esp_ble_host_error(rc);
  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble.gatt.last_platform_code = rc;
  fr_esp_ble_record_locked(FR_BLE_OP_GATT_NOTIFY, err, rc);
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return err;
}

fr_err_t fr_platform_ble_gatt_indicate(fr_runtime_t *runtime,
                                       uint16_t connection_index,
                                       uint16_t attribute_id,
                                       const uint8_t *bytes, uint16_t length,
                                       uint16_t timeout_ms) {
  uint8_t empty = 0;
  uint32_t connection_generation = 0;
  uint16_t stack_handle = BLE_HS_CONN_HANDLE_NONE;
  uint16_t target_handle = 0;
  struct os_mbuf *packet = NULL;
  uint64_t start_us = 0;
  uint64_t budget_us = 0;
  fr_err_t err = FR_OK;
  int rc = 0;

  if (runtime == NULL || (length > 0 && bytes == NULL)) {
    return FR_ERR_INVALID;
  }
  if (timeout_ms == 0 || timeout_ms > 60000u) {
    portENTER_CRITICAL(&fr_esp_ble_lock);
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_INDICATE, FR_ERR_RANGE, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_RANGE;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (fr_esp_ble.gatt.indication_pending) {
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_INDICATE, FR_ERR_BLE_BUSY, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_BLE_BUSY;
  }
  err = fr_esp_ble_gatt_send_check_locked(
      connection_index, attribute_id, length, FR_BLE_GATT_CHR_INDICATE, true,
      &connection_generation, &stack_handle, &target_handle);
  if (err != FR_OK) {
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_INDICATE, err, 0);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (err != FR_OK) {
    return err;
  }

  packet = ble_hs_mbuf_from_flat(length > 0 ? bytes : &empty, length);
  if (packet == NULL) {
    portENTER_CRITICAL(&fr_esp_ble_lock);
    fr_esp_ble.gatt.last_platform_code = BLE_HS_ENOMEM;
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_INDICATE, FR_ERR_CAPACITY,
                             BLE_HS_ENOMEM);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_CAPACITY;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (fr_esp_ble.gatt.indication_pending) {
    err = FR_ERR_BLE_BUSY;
  } else {
    err = fr_esp_ble_gatt_send_check_locked(
        connection_index, attribute_id, length, FR_BLE_GATT_CHR_INDICATE,
        true, &connection_generation, &stack_handle, &target_handle);
  }
  if (err == FR_OK) {
    fr_esp_ble.gatt.indication_pending = true;
    fr_esp_ble.gatt.indication_connection_generation =
        connection_generation;
    fr_esp_ble.gatt.indication_stack_handle = stack_handle;
    fr_esp_ble.gatt.indication_target_handle = target_handle;
    fr_esp_ble.gatt.indication_status = 0;
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (err != FR_OK) {
    os_mbuf_free_chain(packet);
    portENTER_CRITICAL(&fr_esp_ble_lock);
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_INDICATE, err, 0);
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return err;
  }

  rc = ble_gatts_indicate_custom(stack_handle, target_handle, packet);
  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (rc != 0 && fr_esp_ble.gatt.indication_pending &&
      fr_esp_ble.gatt.indication_connection_generation ==
          connection_generation &&
      fr_esp_ble.gatt.indication_stack_handle == stack_handle &&
      fr_esp_ble.gatt.indication_target_handle == target_handle) {
    fr_esp_ble.gatt.indication_pending = false;
    fr_esp_ble.gatt.indication_status = rc;
  }
  fr_esp_ble.gatt.last_platform_code = rc;
  if (rc != 0) {
    err = fr_esp_ble_host_error(rc);
    fr_esp_ble_record_locked(FR_BLE_OP_GATT_INDICATE, err, rc);
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  if (rc != 0) {
    return err;
  }

  start_us = (uint64_t)esp_timer_get_time();
  budget_us = (uint64_t)timeout_ms * 1000u;
  for (;;) {
    bool pending = false;
    int32_t status = 0;

    portENTER_CRITICAL(&fr_esp_ble_lock);
    pending = fr_esp_ble.gatt.indication_pending;
    status = fr_esp_ble.gatt.indication_status;
    if (!pending) {
      err = status == BLE_HS_EDONE ? FR_OK : fr_esp_ble_host_error(status);
      fr_esp_ble_record_locked(FR_BLE_OP_GATT_INDICATE, err,
                               status == BLE_HS_EDONE ? 0 : status);
    }
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    if (!pending) {
      return err;
    }

    err = fr_platform_poll_interrupt(runtime);
    if (err == FR_OK && fr_runtime_is_interrupted(runtime)) {
      err = FR_ERR_INTERRUPTED;
    }
    if (err == FR_OK &&
        (uint64_t)esp_timer_get_time() - start_us > budget_us) {
      err = FR_ERR_BLE_TIMEOUT;
    }
    if (err != FR_OK) {
      portENTER_CRITICAL(&fr_esp_ble_lock);
      fr_esp_ble_record_locked(FR_BLE_OP_GATT_INDICATE, err, 0);
      portEXIT_CRITICAL(&fr_esp_ble_lock);
      return err;
    }
    vTaskDelay(FR_ESP_BLE_WAIT_TICKS);
  }
}

fr_err_t fr_platform_ble_gatt_write_next(bool *out_has_write,
                                         fr_handle_ref_t *out_runtime_ref) {
  if (out_has_write == NULL || out_runtime_ref == NULL) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  *out_has_write = false;
  *out_runtime_ref = (fr_handle_ref_t){0};
  fr_esp_ble.gatt.current_write_valid = false;
  memset(&fr_esp_ble.gatt.current_write, 0,
         sizeof(fr_esp_ble.gatt.current_write));

  while (fr_esp_ble.gatt.write_count > 0) {
    fr_ble_gatt_write_t write =
        fr_esp_ble.gatt.writes[fr_esp_ble.gatt.write_head];

    fr_esp_ble.gatt.write_head =
        (uint8_t)((fr_esp_ble.gatt.write_head + 1u) %
                  FR_BLE_GATT_WRITE_QUEUE_COUNT);
    fr_esp_ble.gatt.write_count -= 1u;
    if (write.table_generation != fr_esp_ble.gatt.table_generation ||
        write.connection_index != FR_ESP_BLE_CONNECTION_INDEX ||
        write.connection_generation != fr_esp_ble.connection.generation ||
        !fr_esp_ble.connection.has_runtime_ref ||
        fr_esp_ble.connection.state == FR_BLE_CONNECTION_FREE) {
      fr_esp_ble.gatt.write_stale += 1u;
      continue;
    }

    fr_esp_ble.gatt.current_write = write;
    fr_esp_ble.gatt.current_write_valid = true;
    *out_runtime_ref = fr_esp_ble.connection.runtime_ref;
    *out_has_write = true;
    break;
  }

  fr_esp_ble_record_locked(FR_BLE_OP_GATT_WRITE_NEXT, FR_OK, 0);
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_OK;
}

fr_err_t fr_platform_ble_gatt_write_current(fr_ble_gatt_write_t *out_write) {
  if (out_write == NULL) {
    return FR_ERR_INVALID;
  }

  portENTER_CRITICAL(&fr_esp_ble_lock);
  if (!fr_esp_ble.gatt.current_write_valid) {
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_ERR_NOT_FOUND;
  }
  *out_write = fr_esp_ble.gatt.current_write;
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_OK;
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
#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
  fr_esp_ble_connections_clear_locked(false);
#endif
#if FR_BLE_ENABLE_GATT_SERVER
  fr_esp_ble_gatt_radio_off_locked();
#endif
#if FR_BLE_ENABLE_GATT_CLIENT
  fr_esp_ble_gatt_client_clear_locked();
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
#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble_connections_clear_locked(true);
  portEXIT_CRITICAL(&fr_esp_ble_lock);
#endif
#if FR_BLE_ENABLE_GATT_SERVER
  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble_gatt_radio_off_locked();
  portEXIT_CRITICAL(&fr_esp_ble_lock);
#endif
#if FR_BLE_ENABLE_GATT_CLIENT
  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble_gatt_client_clear_locked();
  portEXIT_CRITICAL(&fr_esp_ble_lock);
#endif
  err = fr_esp_ble_begin_cleanup();

  if (err == FR_OK) {
    err = fr_esp_ble_wait_cleanup(NULL);
  }
  if (err == FR_OK) {
    portENTER_CRITICAL(&fr_esp_ble_lock);
#if FR_BLE_ENABLE_GATT_SERVER
    memset(&fr_esp_ble.gatt, 0, sizeof(fr_esp_ble.gatt));
#endif
#if FR_BLE_ENABLE_GATT_CLIENT
    memset(&fr_esp_ble.gatt_client, 0,
           sizeof(fr_esp_ble.gatt_client));
#endif
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
#if FR_BLE_ENABLE_CENTRAL || FR_BLE_ENABLE_PERIPHERAL
      .connection_count =
          fr_esp_ble.connection.state == FR_BLE_CONNECTION_FREE ? 0 : 1,
      .connection_capacity = FR_BLE_CONNECTION_COUNT,
#if FR_BLE_ENABLE_PERIPHERAL
      .pending_connection_count =
          fr_esp_ble.connection.state == FR_BLE_CONNECTION_PENDING ? 1 : 0,
      .connection_notice_count = fr_esp_ble.connection_notice_count,
      .connection_notice_capacity = FR_BLE_CONNECTION_NOTICE_COUNT,
#endif
      .connection_connects = fr_esp_ble.connection_connects,
      .connection_accepts = fr_esp_ble.connection_accepts,
      .connection_disconnects = fr_esp_ble.connection_disconnects,
      .incoming_rejected = fr_esp_ble.incoming_rejected,
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
