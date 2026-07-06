#include "event.h"

#include "runtime.h"
#include "vm.h"

#include <stdbool.h>
#include <string.h>

#if FR_FEATURE_REPL
/* Avoid pulling stdio formatting into the event path for one async counter. */
static void fr_event_write_u32(uint32_t value) {
  char digits[11];
  uint8_t pos = (uint8_t)(sizeof(digits) - 1u);

  digits[pos] = '\0';
  do {
    pos--;
    digits[pos] = (char)('0' + (value % 10u));
    value /= 10u;
  } while (value > 0);
  (void)fr_platform_write_text(&digits[pos]);
}
#endif

static bool fr_event_kind_is_gpio(fr_event_kind_t kind) {
  return kind == FR_EVENT_KIND_GPIO_RISING ||
         kind == FR_EVENT_KIND_GPIO_FALLING ||
         kind == FR_EVENT_KIND_GPIO_CHANGES;
}

static bool fr_event_kind_is_timer(fr_event_kind_t kind) {
  return kind == FR_EVENT_KIND_EVERY || kind == FR_EVENT_KIND_AFTER;
}

static bool fr_event_kind_is_wifi(fr_event_kind_t kind) {
  return kind == FR_EVENT_KIND_WIFI_DISCONNECTED ||
         kind == FR_EVENT_KIND_WIFI_RECONNECTED;
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
  uint32_t ms = 0;
  (void)fr_platform_millis(&ms);
  return ms;
}

static fr_err_t fr_event_platform_install(fr_event_kind_t kind, uint16_t source,
                                          uint16_t binding_index,
                                          uint16_t generation) {
  if (fr_event_kind_is_gpio(kind)) {
    return fr_platform_event_gpio_install(kind, source, binding_index,
                                          generation);
  }
  if (fr_event_kind_is_wifi(kind)) {
#if FR_FEATURE_NET
    (void)source;
    return fr_platform_event_wifi_install(kind, binding_index, generation);
#else
    (void)source;
    (void)binding_index;
    (void)generation;
    return FR_ERR_INVALID;
#endif
  }
  return fr_platform_event_timer_install(kind, (uint32_t)source, binding_index,
                                         generation);
}

static fr_err_t fr_event_platform_remove(const fr_event_binding_t *entry,
                                         uint16_t binding_index) {
  if (fr_event_kind_is_gpio(entry->kind)) {
    return fr_platform_event_gpio_remove(entry->source);
  }
  if (fr_event_kind_is_wifi(entry->kind)) {
#if FR_FEATURE_NET
    return fr_platform_event_wifi_remove(binding_index);
#else
    (void)binding_index;
    return FR_ERR_INVALID;
#endif
  }
  return fr_platform_event_timer_remove(binding_index);
}

fr_err_t fr_event_register(fr_runtime_t *runtime, fr_event_kind_t kind,
                           uint16_t source, uint16_t debounce_ms,
                           fr_code_object_id_t body) {
  fr_event_binding_t *entry = NULL;
  fr_event_kind_t old_kind = FR_EVENT_KIND_NONE;
  uint16_t target = FR_EVENT_BINDING_COUNT;
  uint16_t next_generation = 0;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_event_kind_is_gpio(kind) && !fr_event_kind_is_timer(kind) &&
      !fr_event_kind_is_wifi(kind)) {
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
  old_kind = entry->kind;
  next_generation = (uint16_t)(entry->generation + 1);
  /* Stage: try to arm the platform first. If install fails we leave the
     prior binding (or the empty slot) untouched. */
  FR_TRY(fr_event_platform_install(kind, source, target, next_generation));

  if (old_kind == FR_EVENT_KIND_NONE && kind != FR_EVENT_KIND_NONE) {
    runtime->events.active_count++;
  }
  entry->kind = kind;
  entry->source = source;
  entry->debounce_ms = debounce_ms;
  entry->body = body;
  entry->pending = false;
  entry->generation = next_generation;
  entry->registered_at_ms = fr_event_now_ms();
  entry->last_fire_ms = 0;
  entry->has_fired = false;
  return FR_OK;
}

fr_err_t fr_event_cancel(fr_runtime_t *runtime, fr_event_kind_t kind,
                         uint16_t source) {
  fr_event_binding_t *entry = NULL;
  fr_event_kind_t old_kind = FR_EVENT_KIND_NONE;
  uint16_t target = FR_EVENT_BINDING_COUNT;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_event_kind_is_gpio(kind) && !fr_event_kind_is_timer(kind) &&
      !fr_event_kind_is_wifi(kind)) {
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
  old_kind = entry->kind;
  FR_TRY(fr_event_platform_remove(entry, target));
  if (old_kind != FR_EVENT_KIND_NONE) {
    runtime->events.active_count--;
  }
  entry->kind = FR_EVENT_KIND_NONE;
  entry->generation = (uint16_t)(entry->generation + 1);
  entry->pending = false;
  entry->source = 0;
  entry->debounce_ms = 0;
  entry->body = 0;
  entry->registered_at_ms = 0;
  entry->last_fire_ms = 0;
  entry->has_fired = false;
  return FR_OK;
}

/* Pull candidates from the platform and mark surviving ones pending. Body-free
 * so this can run from the safe-point service path. Delivery is statement-level:
 * a long expression runs until the next DROP or RETURN. Stale candidates
 * (cancelled slot, generation mismatch, queued before the current registration)
 * are dropped per spec §3. */
fr_err_t fr_event_drain(fr_runtime_t *runtime) {
  fr_event_candidate_t candidates[FR_EVENT_BINDING_COUNT];
  uint8_t count = 0;
  uint32_t overflow_delta = 0;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_platform_event_drain(candidates, FR_EVENT_BINDING_COUNT, &count,
                                 &overflow_delta));
  runtime->events.overflow_count += overflow_delta;

  for (uint8_t i = 0; i < count; i++) {
    const fr_event_candidate_t *cand = &candidates[i];
    fr_event_binding_t *entry;
    uint32_t since_registered_ms;

    if (cand->binding_index >= FR_EVENT_BINDING_COUNT) {
      continue;
    }
    entry = &runtime->events.entries[cand->binding_index];
    if (entry->kind == FR_EVENT_KIND_NONE) {
      continue;
    }
    if (entry->generation != cand->generation) {
      continue;
    }
    /* Candidate and registration use one uint32_t clock. The half-range rule
     * treats deltas above ~24.8 days as before the current registration. */
    since_registered_ms = cand->timestamp_ms - entry->registered_at_ms;
    if (since_registered_ms > (uint32_t)INT32_MAX) {
      continue;
    }
    /* Debounce drops without updating last_fire_ms. has_fired gates the window
     * check so a first fire at t=0 cannot collide with the zero-init. */
    if (entry->debounce_ms != 0 && entry->has_fired &&
        cand->timestamp_ms - entry->last_fire_ms < entry->debounce_ms) {
      continue;
    }
    entry->pending = true;
    entry->last_fire_ms = cand->timestamp_ms;
    entry->has_fired = true;
  }

  return FR_OK;
}

/* Best-effort: losing this notice must not change dispatch or eval results. */
void fr_event_report_overflow(fr_runtime_t *runtime) {
#if FR_FEATURE_REPL
  uint32_t dropped;

  if (runtime == NULL) {
    return;
  }
  if (runtime->events.overflow_count ==
      runtime->events.overflow_reported_count) {
    return;
  }

  dropped =
      runtime->events.overflow_count - runtime->events.overflow_reported_count;
  runtime->events.overflow_reported_count = runtime->events.overflow_count;
  (void)fr_platform_write_text("! events dropped: ");
  fr_event_write_u32(dropped);
  (void)fr_platform_write_text("\n");
#else
  (void)runtime;
#endif
}

/* Run each pending binding's body once. Call only at statement-boundary safe
 * points (after FR_OP_DROP, at loop back-edges, at FR_OP_RETURN). AFTER
 * bindings are removed before the body runs per spec §5. Errors from bodies
 * are recorded but do not stop the round; the first one is returned. */
fr_err_t fr_event_dispatch(fr_runtime_t *runtime) {
  fr_err_t first_err = FR_OK;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (runtime->dispatching_event) {
    return FR_OK;
  }
  runtime->dispatching_event = true;
#if FR_FEATURE_BYTES
  runtime->bytes.eval_depth++;
#endif

  for (uint16_t i = 0; i < FR_EVENT_BINDING_COUNT; i++) {
    fr_event_binding_t *entry = &runtime->events.entries[i];
    fr_code_object_id_t body;
    fr_tagged_t result = 0;
    fr_err_t body_err;

    if (runtime->interrupted) {
      first_err = FR_ERR_INTERRUPTED;
      break;
    }
    if (!entry->pending) {
      continue;
    }
    body = entry->body;
    entry->pending = false;

    if (entry->kind == FR_EVENT_KIND_AFTER) {
      fr_event_kind_t old_kind = entry->kind;
      (void)fr_platform_event_timer_remove(i);
      if (old_kind != FR_EVENT_KIND_NONE) {
        runtime->events.active_count--;
      }
      entry->kind = FR_EVENT_KIND_NONE;
      entry->generation = (uint16_t)(entry->generation + 1);
      entry->source = 0;
      entry->debounce_ms = 0;
      entry->body = 0;
      entry->registered_at_ms = 0;
      entry->last_fire_ms = 0;
      entry->has_fired = false;
    }

    body_err = fr_vm_run_code_object(runtime, body, &result);
    if (body_err != FR_OK && first_err == FR_OK) {
      first_err = body_err;
    }
  }

#if FR_FEATURE_BYTES
  runtime->bytes.eval_depth--;
#endif
  runtime->dispatching_event = false;
  return first_err;
}

fr_err_t fr_event_clear_table(fr_runtime_t *runtime) {
  fr_err_t first_err = FR_OK;
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  for (uint16_t i = 0; i < FR_EVENT_BINDING_COUNT; i++) {
    fr_event_binding_t *entry = &runtime->events.entries[i];
    if (entry->kind != FR_EVENT_KIND_NONE) {
      fr_err_t err = fr_event_platform_remove(entry, i);
      if (err != FR_OK && first_err == FR_OK) {
        first_err = err;
      }
    }
  }
  memset(&runtime->events, 0, sizeof(runtime->events));
  return first_err;
}
