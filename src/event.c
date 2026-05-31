#include "event.h"

#include "runtime.h"

#include <stdbool.h>
#include <string.h>

static bool fr_event_kind_is_gpio(fr_event_kind_t kind) {
  return kind == FR_EVENT_KIND_GPIO_RISING ||
         kind == FR_EVENT_KIND_GPIO_FALLING ||
         kind == FR_EVENT_KIND_GPIO_CHANGES;
}

static bool fr_event_kind_is_timer(fr_event_kind_t kind) {
  return kind == FR_EVENT_KIND_EVERY || kind == FR_EVENT_KIND_AFTER;
}

/* GPIO last-write replaces on pin alone; timers replace on (kind, ms). */
static bool fr_event_entry_matches(const fr_event_binding_t *entry,
                                   fr_event_kind_t kind, uint16_t source) {
  if (entry->kind == FR_EVENT_KIND_NONE) {
    return false;
  }
  if (fr_event_kind_is_gpio(kind)) {
    return fr_event_kind_is_gpio(entry->kind) && entry->source == source;
  }
  return entry->kind == kind && entry->source == source;
}

static uint32_t fr_event_now_ms(void) {
  uint16_t ms = 0;
  (void)fr_platform_millis(&ms);
  return (uint32_t)ms;
}

static fr_err_t fr_event_platform_install(fr_event_kind_t kind, uint16_t source,
                                          uint16_t binding_index,
                                          uint16_t generation) {
  if (fr_event_kind_is_gpio(kind)) {
    return fr_platform_event_gpio_install(kind, source, binding_index,
                                          generation);
  }
  return fr_platform_event_timer_install(kind, (uint32_t)source, binding_index,
                                         generation);
}

static fr_err_t fr_event_platform_remove(const fr_event_binding_t *entry,
                                         uint16_t binding_index) {
  if (fr_event_kind_is_gpio(entry->kind)) {
    return fr_platform_event_gpio_remove(entry->source);
  }
  return fr_platform_event_timer_remove(binding_index);
}

fr_err_t fr_event_register(fr_runtime_t *runtime, fr_event_kind_t kind,
                           uint16_t source, uint16_t debounce_ms,
                           fr_code_object_id_t body) {
  fr_event_binding_t *entry = NULL;
  uint16_t target = FR_EVENT_BINDING_COUNT;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_event_kind_is_gpio(kind) && !fr_event_kind_is_timer(kind)) {
    return FR_ERR_INVALID;
  }

  for (uint16_t i = 0; i < FR_EVENT_BINDING_COUNT; i++) {
    if (fr_event_entry_matches(&runtime->events.entries[i], kind, source)) {
      target = i;
      break;
    }
  }
  if (target == FR_EVENT_BINDING_COUNT) {
    for (uint16_t i = 0; i < FR_EVENT_BINDING_COUNT; i++) {
      if (runtime->events.entries[i].kind == FR_EVENT_KIND_NONE) {
        target = i;
        break;
      }
    }
  }
  if (target == FR_EVENT_BINDING_COUNT) {
    return FR_ERR_CAPACITY;
  }

  entry = &runtime->events.entries[target];
  entry->kind = kind;
  entry->source = source;
  entry->debounce_ms = debounce_ms;
  entry->body = body;
  entry->pending = false;
  entry->generation = (uint16_t)(entry->generation + 1);
  entry->registered_at_ms = fr_event_now_ms();
  entry->last_fire_ms = 0;

  return fr_event_platform_install(kind, source, target, entry->generation);
}

fr_err_t fr_event_cancel(fr_runtime_t *runtime, fr_event_kind_t kind,
                         uint16_t source) {
  fr_event_binding_t *entry = NULL;
  uint16_t target = FR_EVENT_BINDING_COUNT;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_event_kind_is_gpio(kind) && !fr_event_kind_is_timer(kind)) {
    return FR_ERR_INVALID;
  }

  for (uint16_t i = 0; i < FR_EVENT_BINDING_COUNT; i++) {
    if (fr_event_entry_matches(&runtime->events.entries[i], kind, source)) {
      target = i;
      break;
    }
  }
  if (target == FR_EVENT_BINDING_COUNT) {
    return FR_ERR_NOT_FOUND;
  }

  entry = &runtime->events.entries[target];
  FR_TRY(fr_event_platform_remove(entry, target));
  entry->kind = FR_EVENT_KIND_NONE;
  entry->generation = (uint16_t)(entry->generation + 1);
  entry->pending = false;
  entry->source = 0;
  entry->debounce_ms = 0;
  entry->body = 0;
  entry->registered_at_ms = 0;
  entry->last_fire_ms = 0;
  return FR_OK;
}

void fr_event_clear_table(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return;
  }
  for (uint16_t i = 0; i < FR_EVENT_BINDING_COUNT; i++) {
    fr_event_binding_t *entry = &runtime->events.entries[i];
    if (entry->kind != FR_EVENT_KIND_NONE) {
      (void)fr_event_platform_remove(entry, i);
    }
  }
  memset(&runtime->events, 0, sizeof(runtime->events));
}
