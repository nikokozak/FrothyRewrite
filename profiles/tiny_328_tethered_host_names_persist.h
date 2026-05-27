/*
 * ATmega328P tiny host-names profile with EEPROM persistence restored.
 *
 * This keeps the name-free, introspection-free tiny device surface while
 * measuring whether save/restore can remain part of the kernel proof.
 */

#pragma once

#include "tiny_328_tethered_host_names.h"

#undef FR_FEATURE_PERSISTENCE
#define FR_FEATURE_PERSISTENCE 1

#undef FR_PROFILE_PERSISTENCE_BYTES
#define FR_PROFILE_PERSISTENCE_BYTES 512
