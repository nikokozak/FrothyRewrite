/*
 * ATmega328P tethered pressure profile.
 *
 * The device keeps the runtime, human serial loop, persistence, and basic
 * inspection. Source compilation is expected to happen on the host and arrive
 * later as overlay update bytes.
 */

#pragma once

#include "tiny_328.h"

#undef FR_FEATURE_COMPILER
#define FR_FEATURE_COMPILER 0

#undef FR_FEATURE_OVERLAY_APPLY_COMMAND
#define FR_FEATURE_OVERLAY_APPLY_COMMAND 1

#undef FR_PROFILE_MAX_OVERLAY_UPDATE_SLOT_INITS
#define FR_PROFILE_MAX_OVERLAY_UPDATE_SLOT_INITS 4

#undef FR_PROFILE_MAX_OVERLAY_UPDATE_CODE_OBJECTS
#define FR_PROFILE_MAX_OVERLAY_UPDATE_CODE_OBJECTS 2

#undef FR_PROFILE_MAX_OVERLAY_UPDATE_INSTRUCTION_BYTES
#define FR_PROFILE_MAX_OVERLAY_UPDATE_INSTRUCTION_BYTES 64

#undef FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES
#define FR_PROFILE_MAX_OVERLAY_UPDATE_NAMES 2
