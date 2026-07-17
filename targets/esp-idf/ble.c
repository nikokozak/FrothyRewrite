#include "config.h"

#if FR_FEATURE_BLE

#include "platform.h"
#include "runtime.h"

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
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

enum {
  FR_ESP_BLE_WAIT_TICKS = 1,
  FR_ESP_BLE_CLEANUP_STACK_BYTES = 3072,
  FR_ESP_BLE_CLEANUP_PRIORITY = tskIDLE_PRIORITY + 2,
  FR_ESP_BLE_SCAN_INTERVAL_MIN_MS = 3,
  FR_ESP_BLE_SCAN_INTERVAL_MAX_MS = 10240,
  FR_ESP_BLE_RSSI_MIN = -127,
  FR_ESP_BLE_RSSI_MAX = 20,
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

typedef struct fr_esp_ble_state_t {
  fr_ble_radio_state_t radio_state;
  uint32_t lifecycle_generation;
  uint32_t completion_generation;
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

static void fr_esp_ble_mark_off_locked(void) {
  fr_esp_ble.radio_state = FR_BLE_RADIO_OFF;
  fr_esp_ble.completion_generation = 0;
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

static uint16_t fr_esp_ble_scan_units(uint16_t milliseconds) {
  /* Legacy scan units are 625 us. Adding half a unit rounds to the nearest
   * supported value; an exact half cannot occur for integer milliseconds. */
  return (uint16_t)(((uint32_t)milliseconds * 1000u + 312u) / 625u);
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
  if (!fr_esp_ble.shutdown_in_progress) {
    fr_esp_ble.radio_state = FR_BLE_RADIO_FAILED;
    fr_esp_ble.cleanup_required = fr_esp_ble.host_task_running;
  }
  portEXIT_CRITICAL(&fr_esp_ble_lock);
}

static void fr_esp_ble_host_task(void *argument) {
  (void)argument;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static void fr_esp_ble_cleanup_task(void *argument) {
  uint32_t generation = (uint32_t)(uintptr_t)argument;
  bool stop_host = false;
  int stop_code = 0;
  esp_err_t deinit_code = ESP_OK;

  portENTER_CRITICAL(&fr_esp_ble_lock);
  stop_host = fr_esp_ble.host_task_running;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  if (stop_host) {
    stop_code = nimble_port_stop();
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
  bool host_task_running = false;

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
  if (!fr_esp_ble.host_task_running && !fr_esp_ble.cleanup_required) {
    fr_esp_ble_mark_off_locked();
    portEXIT_CRITICAL(&fr_esp_ble_lock);
    return FR_OK;
  }

  generation = fr_esp_ble.lifecycle_generation;
  host_task_running = fr_esp_ble.host_task_running;
  fr_esp_ble.radio_state = FR_BLE_RADIO_STOPPING;
  fr_esp_ble.shutdown_in_progress = true;
  fr_esp_ble.cleanup_required = true;
  fr_esp_ble.own_address_valid = false;
#if FR_BLE_ENABLE_OBSERVER
  fr_esp_ble_abandon_scan_locked();
#endif
  portEXIT_CRITICAL(&fr_esp_ble_lock);

  if (!host_task_running) {
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
  fr_esp_ble.host_task_running = true;
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  nimble_port_freertos_init(fr_esp_ble_host_task);

  return fr_esp_ble_wait_start(runtime, generation);
}

fr_err_t fr_platform_ble_scan_start(uint16_t interval_ms, uint16_t window_ms,
                                    bool active, bool repeats,
                                    int8_t minimum_rssi) {
#if FR_BLE_ENABLE_OBSERVER
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

  interval_units = fr_esp_ble_scan_units(interval_ms);
  window_units = fr_esp_ble_scan_units(window_ms);
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
#else
  (void)interval_ms;
  (void)window_ms;
  (void)active;
  (void)repeats;
  (void)minimum_rssi;
  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble_record_locked(FR_BLE_OP_SCAN_START, FR_ERR_UNSUPPORTED, 0);
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  return FR_ERR_UNSUPPORTED;
#endif
}

fr_err_t fr_platform_ble_scan_stop(fr_runtime_t *runtime) {
#if FR_BLE_ENABLE_OBSERVER
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
#else
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  return FR_OK;
#endif
}

fr_err_t fr_platform_ble_scan_next(bool *out_has_report) {
#if FR_BLE_ENABLE_OBSERVER
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
#else
  if (out_has_report == NULL) {
    return FR_ERR_INVALID;
  }
  *out_has_report = false;
  return FR_OK;
#endif
}

fr_err_t fr_platform_ble_scan_current(fr_ble_scan_report_t *out_report) {
#if FR_BLE_ENABLE_OBSERVER
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
#else
  if (out_report == NULL) {
    return FR_ERR_INVALID;
  }
  return FR_ERR_NOT_FOUND;
#endif
}

fr_err_t fr_platform_ble_project_clear(void) {
  fr_err_t err = FR_OK;

#if FR_BLE_ENABLE_OBSERVER
  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble_invalidate_scan_locked(true);
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
