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
  /* v1 widens profile identity beyond persistence-only limits. */
  FR_PERSIST_FORMAT_VERSION = 1,
};

static const uint8_t fr_persist_header_magic[4] = {'F', 'R', 'P', 'H'};
static uint8_t fr_persist_payload[FR_PROFILE_PERSISTENCE_BYTES -
                                  FR_PERSIST_HEADER_BYTES];
/* Save's L1 preservation step decodes the existing payload from
 * fr_persist_payload, extracts its L1 BIND + NAME records into this scratch
 * buffer, then runs the L2 encoder into fr_persist_payload with the scratch
 * passed through as the library prefix. Sized as the same upper bound as
 * the payload itself because a save called against an L1-only NVS image
 * could shape that whole image into the prefix. */
static uint8_t fr_persist_library_prefix[FR_PROFILE_PERSISTENCE_BYTES -
                                         FR_PERSIST_HEADER_BYTES];
static uint16_t fr_persist_last_payload_bytes = 0;

typedef struct fr_persist_header_t {
  uint32_t generation;
  uint16_t payload_length;
  uint32_t payload_crc;
} fr_persist_header_t;

static uint16_t fr_persist_read_u16(const uint8_t *bytes) {
  return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void fr_persist_write_u16_raw(uint8_t *bytes, uint16_t value) {
  bytes[0] = (uint8_t)(value & 0xffu);
  bytes[1] = (uint8_t)(value >> 8);
}

static fr_err_t fr_persist_header_build(uint8_t *bytes,
                                        const fr_persist_header_t *header) {
  if (bytes == NULL || header == NULL) {
    return FR_ERR_INVALID;
  }

  memset(bytes, 0, FR_PERSIST_HEADER_BYTES);
  memcpy(bytes, fr_persist_header_magic, sizeof(fr_persist_header_magic));
  bytes[4] = FR_PERSIST_FORMAT_VERSION;
  bytes[5] = FR_PERSIST_HEADER_BYTES;
  fr_write_u32_le(&bytes[FR_PERSIST_PROFILE_HASH_OFFSET], fr_profile_hash());
  fr_write_u32_le(&bytes[12], header->generation);
  fr_persist_write_u16_raw(&bytes[16], header->payload_length);
  fr_write_u32_le(&bytes[20], header->payload_crc);
  fr_write_u32_le(&bytes[24], fr_crc32(bytes, FR_PERSIST_HEADER_BYTES));
  return FR_OK;
}

static fr_err_t fr_persist_header_parse(const uint8_t *bytes,
                                        fr_persist_header_t *out) {
  uint8_t scratch[FR_PERSIST_HEADER_BYTES];
  uint32_t stored_crc = 0;

  if (bytes == NULL || out == NULL) {
    return FR_ERR_INVALID;
  }
  if (memcmp(bytes, fr_persist_header_magic, sizeof(fr_persist_header_magic)) !=
      0) {
    return FR_ERR_NOT_FOUND;
  }
  if (bytes[4] != FR_PERSIST_FORMAT_VERSION ||
      bytes[5] != FR_PERSIST_HEADER_BYTES) {
    return FR_ERR_CORRUPT;
  }
  if (fr_read_u32_le(&bytes[FR_PERSIST_PROFILE_HASH_OFFSET]) !=
      fr_profile_hash()) {
    return FR_ERR_CORRUPT;
  }

  memcpy(scratch, bytes, sizeof(scratch));
  stored_crc = fr_read_u32_le(&scratch[24]);
  memset(&scratch[24], 0, 4);
  if (fr_crc32(scratch, FR_PERSIST_HEADER_BYTES) != stored_crc) {
    return FR_ERR_CORRUPT;
  }

  out->generation = fr_read_u32_le(&bytes[12]);
  out->payload_length = fr_persist_read_u16(&bytes[16]);
  out->payload_crc = fr_read_u32_le(&bytes[20]);
  if (out->payload_length > sizeof(fr_persist_payload)) {
    return FR_ERR_CORRUPT;
  }
  return FR_OK;
}

static fr_err_t fr_persist_read_header(uint8_t slot,
                                       fr_persist_header_t *out) {
  uint8_t header_bytes[FR_PERSIST_HEADER_BYTES];

  FR_TRY(fr_platform_storage_read(slot, 0, header_bytes,
                                  (uint16_t)sizeof(header_bytes)));
  return fr_persist_header_parse(header_bytes, out);
}

static fr_err_t fr_persist_pick_inactive(uint8_t *out_slot,
                                         uint32_t *out_generation) {
  fr_persist_header_t a = {0};
  fr_persist_header_t b = {0};
  bool a_valid = fr_persist_read_header(0, &a) == FR_OK;
  bool b_valid = fr_persist_read_header(1, &b) == FR_OK;

  if (out_slot == NULL || out_generation == NULL) {
    return FR_ERR_INVALID;
  }

  if (!a_valid && !b_valid) {
    *out_slot = 0;
    *out_generation = 1;
  } else if (a_valid && !b_valid) {
    *out_slot = 1;
    *out_generation = a.generation + 1;
  } else if (!a_valid && b_valid) {
    *out_slot = 0;
    *out_generation = b.generation + 1;
  } else if (a.generation <= b.generation) {
    *out_slot = 0;
    *out_generation = b.generation + 1;
  } else {
    *out_slot = 1;
    *out_generation = a.generation + 1;
  }

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

typedef fr_err_t (*fr_persist_payload_apply_fn_t)(fr_runtime_t *runtime,
                                                  const uint8_t *bytes,
                                                  uint16_t length);

static fr_err_t fr_persist_restore_slot(fr_runtime_t *runtime, uint8_t slot,
                                        const fr_persist_header_t *header,
                                        fr_persist_payload_apply_fn_t apply) {
  FR_TRY(fr_platform_storage_read(slot, FR_PERSIST_HEADER_BYTES,
                                  fr_persist_payload, header->payload_length));
  if (fr_crc32(fr_persist_payload, header->payload_length) !=
      header->payload_crc) {
    return FR_ERR_CORRUPT;
  }
  return apply(runtime, fr_persist_payload, header->payload_length);
}

/* Dual-slot retry shared by FULL restore and the boot L1/L2 passes. The
 * tier-aware boot passes skip the auto-reset paths: L1 owns the reset that
 * resources rely on, and L2 must not undo what L1 just installed. */
static fr_err_t
fr_persist_restore_dispatch(fr_runtime_t *runtime,
                            fr_persist_payload_apply_fn_t apply,
                            bool reset_on_miss) {
  fr_persist_header_t headers[FR_PERSIST_STORAGE_SLOT_COUNT];
  bool valid[FR_PERSIST_STORAGE_SLOT_COUNT];
  fr_err_t header_err[FR_PERSIST_STORAGE_SLOT_COUNT];
  uint8_t first = 0;
  uint8_t second = 1;
  fr_err_t last_err = FR_ERR_NOT_FOUND;

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  header_err[0] = fr_persist_read_header(0, &headers[0]);
  header_err[1] = fr_persist_read_header(1, &headers[1]);
  valid[0] = header_err[0] == FR_OK;
  valid[1] = header_err[1] == FR_OK;
  if (!valid[0] && !valid[1]) {
    if (reset_on_miss) {
      FR_TRY(fr_runtime_reset(runtime));
    }
    if (header_err[0] != FR_ERR_NOT_FOUND) {
      return header_err[0];
    }
    if (header_err[1] != FR_ERR_NOT_FOUND) {
      return header_err[1];
    }
    return FR_ERR_NOT_FOUND;
  }
  if (valid[0] && valid[1] && headers[1].generation > headers[0].generation) {
    first = 1;
    second = 0;
  } else if (!valid[0]) {
    first = 1;
    second = 0;
  }

  if (valid[first]) {
    last_err = fr_persist_restore_slot(runtime, first, &headers[first], apply);
    if (last_err == FR_OK) {
      return FR_OK;
    }
  }
  if (valid[second]) {
    last_err =
        fr_persist_restore_slot(runtime, second, &headers[second], apply);
    if (last_err == FR_OK) {
      return FR_OK;
    }
  }

  if (reset_on_miss) {
    FR_TRY(fr_runtime_reset(runtime));
  }
  return last_err;
}

/* D5: save persists the user tier only and preserves the library tier in
 * NVS byte-for-byte. The active slot's existing payload (if any) is decoded,
 * its L1 records are re-encoded into the scratch prefix (literal-value only;
 * see fr_persist_payload_extract_library_records), and the prefix is pasted
 * verbatim ahead of the freshly-encoded L2 records in the new payload.
 * A first save against empty NVS produces an L2-only payload. */
fr_err_t fr_persist_save(const fr_runtime_t *runtime) {
  uint16_t payload_length = 0;
  uint16_t library_prefix_length = 0;
  uint8_t slot = 0;
  uint32_t generation = 0;
  fr_persist_header_t header = {0};
  uint8_t header_bytes[FR_PERSIST_HEADER_BYTES];
  fr_persist_header_t headers[FR_PERSIST_STORAGE_SLOT_COUNT] = {0};
  bool valid[FR_PERSIST_STORAGE_SLOT_COUNT] = {false, false};

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  valid[0] = fr_persist_read_header(0, &headers[0]) == FR_OK;
  valid[1] = fr_persist_read_header(1, &headers[1]) == FR_OK;
  if (valid[0] || valid[1]) {
    uint8_t read_slot = 0;

    if (valid[0] && valid[1]) {
      read_slot = headers[0].generation >= headers[1].generation ? 0 : 1;
    } else if (valid[1]) {
      read_slot = 1;
    }
    FR_TRY(fr_platform_storage_read(read_slot, FR_PERSIST_HEADER_BYTES,
                                    fr_persist_payload,
                                    headers[read_slot].payload_length));
    if (fr_crc32(fr_persist_payload, headers[read_slot].payload_length) ==
        headers[read_slot].payload_crc) {
      FR_TRY(fr_persist_payload_extract_library_records(
          fr_persist_payload, headers[read_slot].payload_length,
          fr_persist_library_prefix,
          (uint16_t)sizeof(fr_persist_library_prefix),
          &library_prefix_length));
    }
  }

  FR_TRY(fr_persist_payload_save_encode(runtime, fr_persist_library_prefix,
                                        library_prefix_length,
                                        fr_persist_payload,
                                        (uint16_t)sizeof(fr_persist_payload),
                                        &payload_length));
  fr_persist_last_payload_bytes = payload_length;
  FR_TRY(fr_persist_pick_inactive(&slot, &generation));
  FR_TRY(fr_platform_storage_erase(slot));
  FR_TRY(fr_platform_storage_write(slot, FR_PERSIST_HEADER_BYTES,
                                   fr_persist_payload, payload_length));

  header.generation = generation;
  header.payload_length = payload_length;
  header.payload_crc = fr_crc32(fr_persist_payload, payload_length);
  FR_TRY(fr_persist_header_build(header_bytes, &header));
  FR_TRY(fr_platform_storage_write(slot, 0, header_bytes,
                                   (uint16_t)sizeof(header_bytes)));
  return fr_persist_read_header(slot, &header);
}

/* D5: public `restore` brings L2 back from NVS; L1 already in place from
 * boot stays untouched. The dispatch skips fr_runtime_reset on the
 * no-payload path (reset_on_miss=false) so a `restore` against empty NVS
 * cannot collapse the L1 the boot pipeline installed. */
fr_err_t fr_persist_restore(fr_runtime_t *runtime) {
  return fr_persist_restore_dispatch(
      runtime, fr_persist_payload_restore_user_only, false);
}

fr_err_t fr_persist_restore_library(fr_runtime_t *runtime) {
  return fr_persist_restore_dispatch(runtime,
                                     fr_persist_payload_restore_library, true);
}

fr_err_t fr_persist_restore_user(fr_runtime_t *runtime) {
  return fr_persist_restore_dispatch(
      runtime, fr_persist_payload_restore_user_after_library, false);
}

fr_err_t fr_persist_wipe(fr_runtime_t *runtime) {
  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }

  FR_TRY(fr_platform_storage_erase(0));
  FR_TRY(fr_platform_storage_erase(1));
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

/* Encode the runtime in full (both tiers) and commit it as the active NVS
 * payload. Used by install-library at receipt (after wiping runtime L1, so
 * the encode covers only the surviving L2 state) and by the REPL compile
 * path after every L1 definition (so the new library word lands in NVS as
 * it is typed — D3's "subsequent definitions are compiled, installed, and
 * persisted to NVS with tier tag L1"). */
fr_err_t fr_persist_save_full(const fr_runtime_t *runtime) {
  uint16_t payload_length = 0;
  uint8_t slot = 0;
  uint32_t generation = 0;
  fr_persist_header_t header = {0};
  uint8_t header_bytes[FR_PERSIST_HEADER_BYTES];

  if (runtime == NULL) {
    return FR_ERR_INVALID;
  }
  FR_TRY(fr_persist_payload_encode(runtime, fr_persist_payload,
                                   (uint16_t)sizeof(fr_persist_payload),
                                   &payload_length));
  fr_persist_last_payload_bytes = payload_length;
  FR_TRY(fr_persist_pick_inactive(&slot, &generation));
  FR_TRY(fr_platform_storage_erase(slot));
  FR_TRY(fr_platform_storage_write(slot, FR_PERSIST_HEADER_BYTES,
                                   fr_persist_payload, payload_length));

  header.generation = generation;
  header.payload_length = payload_length;
  header.payload_crc = fr_crc32(fr_persist_payload, payload_length);
  FR_TRY(fr_persist_header_build(header_bytes, &header));
  FR_TRY(fr_platform_storage_write(slot, 0, header_bytes,
                                   (uint16_t)sizeof(header_bytes)));
  return fr_persist_read_header(slot, &header);
}

/* SPEC D10: receiving install-library drops L1 from the device — runtime
 * binds and the L1 closure in NVS — before accepting the next definitions.
 * The runtime wipe restores L1-stamped overlay slots and compacts their
 * names; the full save then commits a payload that reflects the post-wipe
 * runtime (L2 only at this point — definitions arriving after receipt land
 * in NVS via the REPL compile path's own save_full call). */
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
