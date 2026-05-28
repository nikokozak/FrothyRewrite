/*
 * ATmega328P pressure profile.
 *
 * The target has 32 KB flash, 2 KB SRAM, and 1 KB EEPROM. The first durable
 * overlay uses two EEPROM copies, so each persistence slot is capped at 512
 * bytes including its header.
 */

#pragma once

#define FR_WORD_SIZE 16

#define FR_PROFILE_TARGET_FLASH_BYTES 32768u
#define FR_PROFILE_TARGET_SRAM_BYTES 2048u
#define FR_PROFILE_TARGET_EEPROM_BYTES 1024u
#define FR_PROFILE_MIN_STACK_RESERVE_BYTES 512u

#define FR_PROFILE_MAX_SLOTS 24
#define FR_PROFILE_MAX_INSTRUCTION_BYTES 96
#define FR_PROFILE_MAX_STACK_DEPTH 8
#define FR_PROFILE_CODE_OBJECT_TABLE_SIZE 4
#define FR_PROFILE_NATIVE_TABLE_SIZE 12
#define FR_PROFILE_MAX_CALL_DEPTH 4
#define FR_PROFILE_PERSISTENCE_BYTES 512
#define FR_PROFILE_MAX_NAME_BYTES 16
#define FR_PROFILE_MAX_OVERLAY_NAMES 2
#define FR_PROFILE_REPL_LINE_BYTES 128
#define FR_BASE_IMAGE_INCLUDE_SYMBOLS 1

#define FR_FEATURE_REPL 1
#define FR_FEATURE_COMPILER 1
#define FR_FEATURE_PERSISTENCE 1
#define FR_FEATURE_INTROSPECTION 1

/* Tiny pays nothing for signatures: no help text, no per-row data in the hash. */
#undef FR_FEATURE_NATIVE_SIGNATURES
#define FR_FEATURE_NATIVE_SIGNATURES 0

/* Native table caps at 12; three random rows would push the persistent build
 * to 13, so tiny stays without random. */
#undef FR_FEATURE_RANDOM
#define FR_FEATURE_RANDOM 0

/* PWM adds three more native rows; tiny's table is already full. */
#undef FR_FEATURE_PWM
#define FR_FEATURE_PWM 0
