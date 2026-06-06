#pragma once

#include "platform.h"
#include "types.h"

#include <stdbool.h>

fr_err_t fr_event_register(fr_runtime_t *runtime, fr_event_kind_t kind,
                           uint16_t source, uint16_t debounce_ms,
                           fr_code_object_id_t body);

fr_err_t fr_event_cancel(fr_runtime_t *runtime, fr_event_kind_t kind,
                         uint16_t source);

fr_err_t fr_event_clear_table(fr_runtime_t *runtime);

fr_err_t fr_event_drain(fr_runtime_t *runtime);

fr_err_t fr_event_dispatch(fr_runtime_t *runtime);

/* D19: the platform Wi-Fi event handler reads the kernel-assigned binding
 * identity for the wifi.disconnected / wifi.reconnected kinds via this lookup.
 * Returns false when no Frothy `on wifi.*` binding is currently registered.
 * Source field stays at 0 because each wifi kind has at most one binding. */
bool fr_event_wifi_lookup(fr_event_kind_t kind, uint16_t *out_binding_index,
                          uint16_t *out_generation);
