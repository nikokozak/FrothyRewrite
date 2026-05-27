/*
 * Host compiler mirror for tiny host-names ATmega328P tethered profiles.
 *
 * The host keeps names in its mirror runtime, but encoded device updates and
 * generated calls target a name-free tiny device.
 */

#pragma once

#include "tiny_328_tether_compiler.h"

#define FR_HOST_TINY_NAMES_MODE 1

/*
 * The host mirror keeps source names for every project slot the target device
 * can actually hold. Device updates still strip name records before apply.
 */
#undef FR_PROFILE_MAX_OVERLAY_NAMES
#define FR_PROFILE_MAX_OVERLAY_NAMES 8

#undef FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES
#define FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES 8

#undef FR_PROFILE_REPL_LINE_BYTES
#define FR_PROFILE_REPL_LINE_BYTES 192

#if ((FR_PROFILE_REPL_LINE_BYTES - 7u) / 2u) < 92u
#error "tiny host-names compiler profile requires at least 92 decoded apply bytes"
#endif

#undef FR_PROFILE_MAX_INSTRUCTION_BYTES
#define FR_PROFILE_MAX_INSTRUCTION_BYTES 128

#undef FR_PROFILE_CONTRACT_NAME
#define FR_PROFILE_CONTRACT_NAME "tiny_328_tethered_host_names_persist"

#define FR_PROFILE_HASH_MAX_OVERLAY_NAMES 0
#define FR_PROFILE_HASH_MAX_OVERLAY_UPDATE_NAMES 0
#define FR_PROFILE_HASH_BASE_IMAGE_INCLUDE_SYMBOLS 0
#define FR_PROFILE_HASH_FEATURE_NUMERIC_SLOT_CALLS 1
#define FR_PROFILE_HASH_FEATURE_NATIVE_SIGNATURES 0
