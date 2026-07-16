#include "persist.h"

#if !FR_FEATURE_PERSISTENCE
#error "persist.c should only be compiled when FR_FEATURE_PERSISTENCE is enabled"
#endif

#include "base_defs.h"
#include "base_image.h"
#include "code.h"
#include "crc.h"
#include "event.h"
#include "object.h"
#include "persist_payload.h"
#include "platform.h"
#include "profile.h"
#include "slot.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static uint16_t fr_persist_last_payload_bytes = 0;
static const uint8_t *fr_persist_boot_payload_bytes = NULL;
static uint16_t fr_persist_boot_payload_length = 0;
static bool fr_persist_boot_image_pinned;

static void fr_persist_forget_boot_image(void) {
  fr_persist_boot_payload_bytes = NULL;
  fr_persist_boot_payload_length = 0;
  fr_persist_boot_image_pinned = false;
}

typedef struct fr_persist_stream_ctx_t {
  uint32_t crc;
  uint32_t length;
} fr_persist_stream_ctx_t;

static fr_err_t fr_persist_stream_write_payload(void *ctx,
                                                const uint8_t *bytes,
                                                uint16_t length) {
  fr_persist_stream_ctx_t *stream = (fr_persist_stream_ctx_t *)ctx;

  if (stream == NULL || (bytes == NULL && length > 0)) {
    return FR_ERR_INVALID;
  }
  if (stream->length + length > FR_PERSIST_PAYLOAD_BYTES) {
    return FR_ERR_CAPACITY;
  }
  if (length > 0) {
    FR_TRY(fr_platform_persist_stream_write(bytes, length));
    stream->crc = fr_crc32_update(stream->crc, bytes, length);
    stream->length += length;
  }
  return FR_OK;
}

static fr_err_t fr_persist_stream_commit(
    const fr_runtime_t *runtime, const uint8_t *old_payload,
    uint16_t old_payload_length, bool preserve_library) {
  fr_persist_stream_ctx_t stream = {0xffffffffu, 0};
  uint16_t payload_length = 0;
  uint8_t header[FR_PERSIST_HEADER_BYTES];
  fr_err_t err = FR_OK;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  err = fr_platform_persist_stream_begin();
  if (err != FR_OK) {
    return err;
  }
  if (preserve_library) {
    err = fr_persist_payload_save_stream(
        runtime, old_payload, old_payload_length, fr_persist_stream_write_payload,
        &stream, &payload_length);
  } else {
    err = fr_persist_payload_encode_stream(
        runtime, fr_persist_stream_write_payload, &stream, &payload_length);
  }
  if (err == FR_OK && payload_length != stream.length) {
    err = FR_ERR_CORRUPT;
  }
  if (err == FR_OK) {
    err = fr_persist_format_build_header(header, stream.length, ~stream.crc);
  }
  if (err == FR_OK) {
    err = fr_platform_persist_stream_finalize(header);
  }
  if (err != FR_OK) {
    fr_platform_persist_stream_abort();
    return err;
  }
  fr_persist_last_payload_bytes = payload_length;
  return FR_OK;
}

/* Defined in persist_payload.c — boot two-call restore. fr_persist_restore_library
 * resets the runtime, installs resources, and applies only L1 binds (per-bind
 * failures log + skip per SPEC D6). fr_persist_restore_user must follow
 * within the same boot sequence; it applies L2 binds plus names and events
 * onto the runtime the L1 pass left behind. fr_persist_payload_restore_user_only
 * is the SPEC D5 user-`restore` path: applies L2 binds plus names and events
 * to the runtime without resetting, so the L1 already in place from boot
 * survives unchanged. */
extern fr_err_t fr_persist_payload_restore_library(fr_runtime_t *runtime,
                                                   const uint8_t *bytes,
                                                   uint16_t length);
extern fr_err_t fr_persist_payload_restore_user_after_library(
    fr_runtime_t *runtime, const uint8_t *bytes, uint16_t length);
extern fr_err_t fr_persist_payload_restore_user_only(fr_runtime_t *runtime,
                                                     const uint8_t *bytes,
                                                     uint16_t length);
extern fr_err_t fr_persist_payload_restore(fr_runtime_t *runtime,
                                           const uint8_t *bytes,
                                           uint16_t length);

typedef fr_err_t (*fr_persist_payload_apply_fn_t)(fr_runtime_t *runtime,
                                                  const uint8_t *bytes,
                                                  uint16_t length);

static fr_err_t fr_persist_apply_payload(fr_runtime_t *runtime,
                                         const uint8_t *payload,
                                         uint16_t payload_length,
                                         fr_persist_payload_apply_fn_t apply,
                                         bool reset_on_error) {
  fr_err_t err = FR_OK;

  if (runtime == NULL || payload == NULL || apply == NULL) {
    return FR_ERR_INVALID;
  }

  err = apply(runtime, payload, payload_length);
  if (err != FR_OK && reset_on_error) {
    FR_TRY(fr_runtime_reset(runtime));
  }
  return err;
}

static fr_err_t fr_persist_cleanup_failed_apply(fr_runtime_t *runtime) {
  fr_err_t err = fr_runtime_clear_project(runtime);

  fr_code_restore_base(runtime);
  fr_object_restore_base(runtime);
  return err;
}

static fr_err_t
fr_persist_restore_read_and_apply(fr_runtime_t *runtime,
                                  fr_persist_payload_apply_fn_t apply,
                                  bool reset_on_miss,
                                  uint16_t *out_applied_payload_length) {
  fr_err_t err = FR_OK;
  fr_err_t last_err = FR_ERR_NOT_FOUND;
  const uint8_t *image = NULL;
  const uint8_t *payload = NULL;
  uint16_t image_length = 0;
  uint16_t payload_length = 0;
  fr_persist_format_info_t info = {0};

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  for (uint8_t image_index = 0;; image_index++) {
    bool apply_started = false;

    err = fr_platform_persist_mount(image_index, &image, &image_length);
    if (err == FR_ERR_NOT_FOUND) {
      break;
    }
    if (err != FR_OK) {
      if (reset_on_miss) {
        FR_TRY(fr_runtime_reset(runtime));
      }
      return err;
    }

    err = fr_persist_format_validate(image, image_length, &info);
    if (err == FR_OK) {
      if (info.payload_length > UINT16_MAX) {
        err = FR_ERR_CAPACITY;
      } else {
        payload_length = (uint16_t)info.payload_length;
        payload = &image[FR_PERSIST_HEADER_BYTES];
        apply_started = true;
        err = fr_persist_apply_payload(runtime, payload, payload_length, apply,
                                       reset_on_miss);
      }
    }
    if (err == FR_OK) {
      err = fr_platform_persist_mount_commit();
      if (err != FR_OK) {
        fr_err_t cleanup_err = fr_persist_cleanup_failed_apply(runtime);

        fr_platform_persist_mount_discard();
        if (cleanup_err != FR_OK) {
          return cleanup_err;
        }
        return err;
      }
      if (out_applied_payload_length != NULL) {
        *out_applied_payload_length = payload_length;
      }
      fr_code_mark_persist_image(runtime);
      fr_object_mark_persist_image(runtime);
      fr_slot_mark_persist_image(runtime);
      if (apply == fr_persist_payload_restore_library) {
        fr_persist_boot_payload_bytes = payload;
        fr_persist_boot_payload_length = payload_length;
      }
      return FR_OK;
    }
    if (apply_started) {
      fr_err_t cleanup_err = fr_persist_cleanup_failed_apply(runtime);

      fr_platform_persist_mount_discard();
      if (cleanup_err != FR_OK) {
        return cleanup_err;
      }
    } else {
      fr_platform_persist_mount_discard();
    }
    last_err = err;
  }

  if (last_err == FR_ERR_NOT_FOUND && reset_on_miss) {
    FR_TRY(fr_runtime_reset(runtime));
  }
  return last_err;
}

static fr_err_t fr_persist_remount_latest(fr_runtime_t *runtime) {
  return fr_persist_restore_read_and_apply(runtime, fr_persist_payload_restore,
                                           true, NULL);
}

/* D5: save persists the user tier only and preserves the library tier in
 * durable storage byte-for-byte. The existing committed payload (if any) is
 * scanned for L1 record spans, those source bytes are copied into the new
 * stream in source order, and freshly encoded L2 records follow them. A first
 * save against empty storage produces an L2-only payload. */
fr_err_t fr_persist_save(fr_runtime_t *runtime) {
  fr_err_t read_err = FR_OK;
  fr_err_t save_err = FR_OK;
  const uint8_t *image = NULL;
  const uint8_t *old_payload = NULL;
  uint16_t image_length = 0;
  uint16_t old_payload_length = 0;
  fr_persist_format_info_t info = {0};

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  fr_persist_forget_boot_image();

  read_err = fr_platform_persist_mount(0, &image, &image_length);
  if (read_err == FR_OK) {
    save_err = fr_persist_format_validate(image, image_length, &info);
    if (save_err == FR_OK) {
      if (info.payload_length > UINT16_MAX) {
        save_err = FR_ERR_CAPACITY;
      } else {
        old_payload = &image[FR_PERSIST_HEADER_BYTES];
        old_payload_length = (uint16_t)info.payload_length;
      }
    }
  } else if (read_err != FR_ERR_NOT_FOUND && read_err != FR_ERR_CORRUPT) {
    return read_err;
  }
  if (save_err == FR_OK) {
    save_err = fr_persist_stream_commit(runtime, old_payload,
                                        old_payload_length, true);
  }
  if (save_err != FR_OK) {
    fr_platform_persist_mount_discard();
    return save_err;
  }
  return fr_persist_remount_latest(runtime);
}

/* D5: public `restore` brings L2 back from durable storage; L1 binds already
 * in place from boot stay untouched. The payload layer rewinds code to the
 * base-image boundary and remounts saved code so code ids do not drift. The
 * no-payload path still skips reset so an empty restore cannot collapse L1. */
fr_err_t fr_persist_restore(fr_runtime_t *runtime) {
  fr_persist_forget_boot_image();
  return fr_persist_restore_read_and_apply(
      runtime, fr_persist_payload_restore_user_only, false, NULL);
}

fr_err_t fr_persist_restore_library(fr_runtime_t *runtime) {
  fr_err_t err = FR_OK;
  uint16_t payload_length = 0;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  fr_persist_forget_boot_image();

  err = fr_persist_restore_read_and_apply(
      runtime, fr_persist_payload_restore_library, true, &payload_length);
  if (err == FR_OK) {
    fr_persist_boot_payload_length = payload_length;
    fr_persist_boot_image_pinned = true;
  }
  return err;
}

fr_err_t fr_persist_restore_user(fr_runtime_t *runtime) {
  fr_err_t err = FR_OK;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  if (!fr_persist_boot_image_pinned) {
    return FR_ERR_NOT_FOUND;
  }

  err = fr_persist_apply_payload(runtime, fr_persist_boot_payload_bytes,
                                 fr_persist_boot_payload_length,
                                 fr_persist_payload_restore_user_after_library,
                                 false);
  if (err == FR_OK) {
    fr_slot_mark_persist_image(runtime);
  }
  if (err == FR_OK || err == FR_ERR_NOT_FOUND) {
    fr_persist_forget_boot_image();
  }
  return err;
}

fr_err_t fr_persist_wipe(fr_runtime_t *runtime) {
  fr_err_t restart_err;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_runtime_clear_project(runtime));
  fr_persist_forget_boot_image();
  fr_platform_persist_unmount();
  FR_TRY(fr_platform_persist_clear());

  restart_err = fr_platform_restart();
  FR_TRY(fr_base_image_install(runtime));
  return restart_err == FR_ERR_UNSUPPORTED ? FR_OK : restart_err;
}

/* Defined in persist_payload.c. Drops every user-tier overlay binding from
 * the runtime; library-tier slots stay. The encoder reads runtime->slots so
 * the fr_persist_save below writes only the surviving library binds. */
extern void fr_persist_session_wipe_user_tier(fr_runtime_t *runtime);

fr_err_t fr_persist_wipe_user(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  /* Events are user runtime state: they are only ever armed by user code
   * calling every/after/on. Stop them here (a full wipe does so via
   * fr_runtime_reset) so a timer cannot keep firing into a slot the tier wipe
   * just cleared, which spams 'wrong type' errors. */
  FR_TRY(fr_event_clear_table(runtime));
  fr_persist_session_wipe_user_tier(runtime);
  return fr_persist_save(runtime);
}

/* Defined in persist_payload.c — drops L1 overlay binds from the runtime
 * and clears their tier stamps so the next encode walks only L2. */
extern void fr_persist_session_wipe_library_tier(fr_runtime_t *runtime);

/* Encode the runtime in full (both tiers) and commit it as the active
 * payload. Used by install-library at receipt (after wiping runtime L1, so
 * the encode covers only the surviving L2 state) and by the REPL compile
 * path after every L1 definition (so the new library word lands in durable
 * storage as it is typed — D3's "subsequent definitions are compiled,
 * installed, and persisted to NVS with tier tag L1"). */
fr_err_t fr_persist_save_full(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  fr_persist_forget_boot_image();
  FR_TRY(fr_persist_stream_commit(runtime, NULL, 0, false));
  return fr_persist_remount_latest(runtime);
}

/* SPEC D10: receiving install-library drops L1 from the device — runtime
 * binds and the L1 closure in durable storage — before accepting the next
 * definitions. The runtime wipe restores L1-stamped overlay slots and
 * compacts their names; the full save then commits a payload that reflects
 * the post-wipe runtime (L2 only at this point — definitions arriving after
 * receipt land in storage via the REPL compile path's own save_full call). */
fr_err_t fr_persist_install_library(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  fr_persist_session_wipe_library_tier(runtime);
  return fr_persist_save_full(runtime);
}

uint16_t fr_persist_debug_last_payload_bytes(void) {
  return fr_persist_last_payload_bytes;
}

uint32_t fr_persist_debug_profile_hash(void) {
  return fr_profile_hash();
}
