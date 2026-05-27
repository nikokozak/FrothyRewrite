/*
 * Host compiler mirror for the ATmega328P tethered profile.
 *
 * It keeps the tiny tethered limits but enables the source compiler so host
 * tooling can emit overlay update bytes for the device.
 */

#pragma once

#include "tiny_328_tethered.h"

#undef FR_FEATURE_COMPILER
#define FR_FEATURE_COMPILER 1

#undef FR_FEATURE_OVERLAY_APPLY_COMMAND
#define FR_FEATURE_OVERLAY_APPLY_COMMAND 0

#define FR_PROFILE_CONTRACT_NAME "tiny_328_tethered"
#define FR_PROFILE_HASH_FEATURE_COMPILER 0
#define FR_PROFILE_HASH_FEATURE_OVERLAY_APPLY_COMMAND 1
