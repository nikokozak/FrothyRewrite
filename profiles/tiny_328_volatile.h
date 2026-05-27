/*
 * ATmega328P volatile-live pressure profile.
 *
 * This keeps the line evaluator and compiler but removes persistence, so the
 * device can be live while powered without carrying EEPROM save/restore cost.
 */

#pragma once

#include "tiny_328.h"

#undef FR_FEATURE_PERSISTENCE
#define FR_FEATURE_PERSISTENCE 0

#undef FR_PROFILE_PERSISTENCE_BYTES
#define FR_PROFILE_PERSISTENCE_BYTES 0
