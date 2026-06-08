/*
 * Host proof profile. It intentionally starts with tiny limits so host tests do
 * not hide target pressure.
 */

#pragma once

#define FR_WORD_SIZE 16

#define FR_PROFILE_MAX_SLOTS 32
#define FR_PROFILE_MAX_INSTRUCTION_BYTES 128
#define FR_PROFILE_MAX_STACK_DEPTH 16
#define FR_PROFILE_CODE_OBJECT_TABLE_SIZE 16
#define FR_PROFILE_NATIVE_TABLE_SIZE 20
#define FR_PROFILE_MAX_HANDLES 4
#define FR_PROFILE_MAX_CALL_DEPTH 8
#define FR_PROFILE_PERSISTENCE_BYTES 1024
#define FR_PROFILE_MAX_NAME_BYTES 32
#define FR_PROFILE_MAX_OVERLAY_NAMES 8
#define FR_BASE_IMAGE_INCLUDE_SYMBOLS 1

#define FR_FEATURE_REPL 1
#define FR_FEATURE_COMPILER 1
#define FR_FEATURE_PERSISTENCE 1
#define FR_FEATURE_INTROSPECTION 1
#define FR_FEATURE_HANDLES 1

/* T12-servo turns PWM on for host so the servo library has a stub to drive. */
#undef FR_FEATURE_PWM
#define FR_FEATURE_PWM 1

/* I2C would add four rows on top of the same 13, overshooting the 16 cap.
 * It also needs text, which the pressure profile does not carry. */
#undef FR_FEATURE_I2C
#define FR_FEATURE_I2C 0

/* Math adds six rows on top of the same 13, overshooting the 16 cap. */
#undef FR_FEATURE_MATH
#define FR_FEATURE_MATH 0

/* The convenience-word base library (T7) calls mod and assumes math is
   on; with math off here, the pressure profile skips source-base. */
#undef FR_FEATURE_SOURCE_BASE
#define FR_FEATURE_SOURCE_BASE 0

#define FR_FEATURE_NET 0
#define FR_FEATURE_POWER 0
