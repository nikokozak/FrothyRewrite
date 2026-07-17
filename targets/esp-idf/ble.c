#include "config.h"

#if FR_FEATURE_BLE

#include "platform.h"
#include "runtime.h"

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
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
};

typedef struct fr_esp_ble_state_t {
  fr_ble_radio_state_t radio_state;
  uint32_t lifecycle_generation;
  uint32_t completion_generation;
  bool host_task_running;
  bool shutdown_in_progress;
  bool cleanup_required;
  uint32_t late_callback_count;

  fr_ble_address_type_t own_address_type;
  uint8_t own_address[6];
  bool own_address_valid;

  fr_ble_operation_t last_operation;
  fr_err_t last_result;
  int32_t last_platform_code;
  int32_t last_protocol_reason;
  uint32_t last_operation_ms;
  uint32_t reset_count;
} fr_esp_ble_state_t;

static portMUX_TYPE fr_esp_ble_lock = portMUX_INITIALIZER_UNLOCKED;
static fr_esp_ble_state_t fr_esp_ble;

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

static void fr_esp_ble_mark_off_locked(void) {
  fr_esp_ble.radio_state = FR_BLE_RADIO_OFF;
  fr_esp_ble.completion_generation = 0;
  fr_esp_ble.host_task_running = false;
  fr_esp_ble.shutdown_in_progress = false;
  fr_esp_ble.cleanup_required = false;
  fr_esp_ble.own_address_valid = false;
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

static fr_err_t fr_esp_ble_begin_cleanup(void) {
  uint32_t generation = 0;
  BaseType_t created = pdFAIL;

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
  fr_esp_ble.radio_state = FR_BLE_RADIO_STOPPING;
  fr_esp_ble.shutdown_in_progress = true;
  fr_esp_ble.cleanup_required = true;
  fr_esp_ble.own_address_valid = false;
  portEXIT_CRITICAL(&fr_esp_ble_lock);

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

  ble_hs_cfg.reset_cb = fr_esp_ble_on_reset;
  ble_hs_cfg.sync_cb = fr_esp_ble_on_sync;

  portENTER_CRITICAL(&fr_esp_ble_lock);
  fr_esp_ble.host_task_running = true;
  portEXIT_CRITICAL(&fr_esp_ble_lock);
  nimble_port_freertos_init(fr_esp_ble_host_task);

  return fr_esp_ble_wait_start(runtime, generation);
}

fr_err_t fr_platform_ble_project_clear(void) {
  fr_err_t err = fr_esp_ble_begin_cleanup();

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
      .scan_state = FR_BLE_SCAN_IDLE,
      .roles = fr_esp_ble_roles(),
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
      .coexistence_enabled = true,
#else
      .coexistence_enabled = false,
#endif
      .lifecycle_generation = fr_esp_ble.lifecycle_generation,
      .shutdown_in_progress = fr_esp_ble.shutdown_in_progress,
      .cleanup_required = fr_esp_ble.cleanup_required,
      .late_callback_count = fr_esp_ble.late_callback_count,
      .own_address_type = fr_esp_ble.own_address_type,
      .own_address_valid = fr_esp_ble.own_address_valid,
      .queue_capacity = FR_BLE_SCAN_QUEUE_COUNT,
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
