#include "persist.h"

#if !FR_FEATURE_PERSISTENCE
#error "persist.c should only be compiled when FR_FEATURE_PERSISTENCE is enabled"
#endif

#include "base_defs.h"
#include "crc.h"
#include "event.h"
#include "persist_payload.h"
#include "platform.h"
#include "profile.h"

#include <stdbool.h>
#include <string.h>

enum {
  FR_PERSIST_PAYLOAD_BYTES =
      FR_PROFILE_PERSISTENCE_BYTES - FR_PERSIST_HEADER_BYTES,
};

static uint8_t fr_persist_image[FR_PROFILE_PERSISTENCE_BYTES];
/* Save's L1 preservation step decodes the existing payload from the committed
 * image, extracts its L1 BIND + NAME records into this scratch buffer, then
 * runs the L2 encoder into fr_persist_image's payload area with the scratch
 * passed through as the library prefix. Sized as the same upper bound as
 * the payload itself because a save called against an L1-only persisted image
 * could shape that whole image into the prefix. */
static uint8_t fr_persist_library_prefix[FR_PERSIST_PAYLOAD_BYTES];
static uint16_t fr_persist_last_payload_bytes = 0;
static uint16_t fr_persist_boot_payload_bytes = 0;
static bool fr_persist_boot_image_pinned;

static uint8_t *fr_persist_payload_bytes(void) {
  return &fr_persist_image[FR_PERSIST_HEADER_BYTES];
}

static const uint8_t *fr_persist_payload_const_bytes(void) {
  return &fr_persist_image[FR_PERSIST_HEADER_BYTES];
}

static void fr_persist_forget_boot_image(void) {
  fr_persist_boot_payload_bytes = 0;
  fr_persist_boot_image_pinned = false;
}

static fr_err_t fr_persist_read_image(uint8_t image_index,
                                      uint16_t *out_payload_length) {
  uint16_t image_length = 0;
  fr_persist_format_info_t info = {0};

  if (out_payload_length == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_platform_persist_read(fr_persist_image,
                                  (uint16_t)sizeof(fr_persist_image),
                                  &image_length, image_index));
  FR_TRY(fr_persist_format_validate(fr_persist_image, image_length, &info));
  *out_payload_length = info.payload_length;
  return FR_OK;
}

static fr_err_t fr_persist_commit_payload(uint16_t payload_length) {
  uint16_t image_length = 0;
  uint8_t *payload = fr_persist_payload_bytes();

  if (payload_length > FR_PERSIST_PAYLOAD_BYTES) {
    return FR_ERR_CAPACITY;
  }

  FR_TRY(fr_persist_format_build_header(fr_persist_image, payload_length,
                                        fr_crc32(payload, payload_length)));
  image_length = (uint16_t)(FR_PERSIST_HEADER_BYTES + payload_length);
  return fr_platform_persist_commit(fr_persist_image, image_length);
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

typedef fr_err_t (*fr_persist_payload_apply_fn_t)(fr_runtime_t *runtime,
                                                  const uint8_t *bytes,
                                                  uint16_t length);

static fr_err_t fr_persist_apply_image(fr_runtime_t *runtime,
                                       uint16_t payload_length,
                                       fr_persist_payload_apply_fn_t apply,
                                       bool reset_on_error) {
  fr_err_t err = FR_OK;

  if (runtime == NULL || apply == NULL) {
    return FR_ERR_INVALID;
  }

  err = apply(runtime, fr_persist_payload_const_bytes(), payload_length);
  if (err != FR_OK && reset_on_error) {
    FR_TRY(fr_runtime_reset(runtime));
  }
  return err;
}

static fr_err_t
fr_persist_restore_read_and_apply(fr_runtime_t *runtime,
                                  fr_persist_payload_apply_fn_t apply,
                                  bool reset_on_miss,
                                  uint16_t *out_applied_payload_length) {
  fr_err_t err = FR_OK;
  fr_err_t last_err = FR_ERR_NOT_FOUND;
  uint16_t payload_length = 0;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  for (uint8_t image_index = 0;; image_index++) {
    err = fr_persist_read_image(image_index, &payload_length);
    if (err == FR_ERR_NOT_FOUND) {
      break;
    }
    if (err != FR_OK) {
      if (reset_on_miss) {
        FR_TRY(fr_runtime_reset(runtime));
      }
      return err;
    }

    err =
        fr_persist_apply_image(runtime, payload_length, apply, reset_on_miss);
    if (err == FR_OK) {
      if (out_applied_payload_length != NULL) {
        *out_applied_payload_length = payload_length;
      }
      return FR_OK;
    }
    last_err = err;
  }

  if (last_err == FR_ERR_NOT_FOUND && reset_on_miss) {
    FR_TRY(fr_runtime_reset(runtime));
  }
  return last_err;
}

/* D5: save persists the user tier only and preserves the library tier in
 * durable storage byte-for-byte. The existing committed payload (if any) is
 * decoded, its L1 records are re-encoded into the scratch prefix
 * (literal-value only; see fr_persist_payload_extract_library_records), and
 * the prefix is pasted verbatim ahead of the freshly-encoded L2 records in
 * the new payload. A first save against empty storage produces an L2-only
 * payload. */
fr_err_t fr_persist_save(const fr_runtime_t *runtime) {
  fr_err_t read_err = FR_OK;
  uint16_t payload_length = 0;
  uint16_t previous_payload_length = 0;
  uint16_t library_prefix_length = 0;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  fr_persist_forget_boot_image();

  read_err = fr_persist_read_image(0, &previous_payload_length);
  if (read_err == FR_OK) {
    FR_TRY(fr_persist_payload_extract_library_records(
        fr_persist_payload_const_bytes(), previous_payload_length,
        fr_persist_library_prefix,
        (uint16_t)sizeof(fr_persist_library_prefix), &library_prefix_length));
  } else if (read_err != FR_ERR_NOT_FOUND && read_err != FR_ERR_CORRUPT) {
    return read_err;
  }

  FR_TRY(fr_persist_payload_save_encode(
      runtime, fr_persist_library_prefix, library_prefix_length,
      fr_persist_payload_bytes(), (uint16_t)FR_PERSIST_PAYLOAD_BYTES,
      &payload_length));
  fr_persist_last_payload_bytes = payload_length;
  return fr_persist_commit_payload(payload_length);
}

/* D5: public `restore` brings L2 back from durable storage; L1 already in
 * place from boot stays untouched. The path skips fr_runtime_reset on the
 * no-payload path so a `restore` against empty storage cannot collapse the L1
 * the boot pipeline installed. */
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
    fr_persist_boot_payload_bytes = payload_length;
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

  err = fr_persist_apply_image(runtime, fr_persist_boot_payload_bytes,
                               fr_persist_payload_restore_user_after_library,
                               false);
  if (err == FR_OK || err == FR_ERR_NOT_FOUND) {
    fr_persist_forget_boot_image();
  }
  return err;
}

fr_err_t fr_persist_wipe(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  fr_persist_forget_boot_image();
  FR_TRY(fr_platform_persist_clear());
  return fr_runtime_reset(runtime);
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
fr_err_t fr_persist_save_full(const fr_runtime_t *runtime) {
  uint16_t payload_length = 0;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  fr_persist_forget_boot_image();
  FR_TRY(fr_persist_payload_encode(runtime, fr_persist_payload_bytes(),
                                   (uint16_t)FR_PERSIST_PAYLOAD_BYTES,
                                   &payload_length));
  fr_persist_last_payload_bytes = payload_length;
  return fr_persist_commit_payload(payload_length);
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
