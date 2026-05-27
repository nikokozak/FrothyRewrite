/*
 * ATmega328P tiny host-names tethered profile.
 *
 * This is the tiny-mode exception: the host owns source names because the
 * device cannot afford them. The device still owns slots, tagged words, code
 * objects, native ids, overlay apply, execution, and recovery.
 */

#pragma once

#include "tiny_328_tethered.h"

#undef FR_BASE_IMAGE_INCLUDE_SYMBOLS
#define FR_BASE_IMAGE_INCLUDE_SYMBOLS 0

#undef FR_FEATURE_INTROSPECTION
#define FR_FEATURE_INTROSPECTION 0

#undef FR_FEATURE_PERSISTENCE
#define FR_FEATURE_PERSISTENCE 0

#undef FR_FEATURE_NUMERIC_SLOT_CALLS
#define FR_FEATURE_NUMERIC_SLOT_CALLS 1

#undef FR_FEATURE_NATIVE_SIGNATURES
#define FR_FEATURE_NATIVE_SIGNATURES 0

#undef FR_PROFILE_REPL_LINE_BYTES
#define FR_PROFILE_REPL_LINE_BYTES 192

#if ((FR_PROFILE_REPL_LINE_BYTES - 7u) / 2u) < 92u
#error "tiny host-names profile requires at least 92 decoded apply bytes"
#endif

#undef FR_PROFILE_MAX_INSTRUCTION_BYTES
#define FR_PROFILE_MAX_INSTRUCTION_BYTES 128

#undef FR_PROFILE_PERSISTENCE_BYTES
#define FR_PROFILE_PERSISTENCE_BYTES 0

#undef FR_PROFILE_MAX_OVERLAY_NAMES
#define FR_PROFILE_MAX_OVERLAY_NAMES 0

#undef FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES
#define FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES 0
