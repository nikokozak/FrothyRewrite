#pragma once

#include "types.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * A tagged word is the encoded runtime word stored in slots and carried by the
 * VM stack. Parsed integers and instruction operands are raw until encoded.
 */
typedef uint32_t fr_tagged_t;

#define FR_TAGGED_WORD_BYTES 4u
#define FR_TAGGED_INT_END ((fr_tagged_t)0x7FFFFFFFu)
#define FR_TAGGED_SPECIAL_BASE ((fr_tagged_t)0x80000000u)
#define FR_TAGGED_SPECIAL_END ((fr_tagged_t)0x87FFFFFFu)
#define FR_TAGGED_SLOT_BASE ((fr_tagged_t)0x88000000u)
#define FR_TAGGED_SLOT_END ((fr_tagged_t)0x9FFFFFFFu)
#define FR_TAGGED_SLOT_MAX_ID ((fr_tagged_t)UINT16_MAX)
#define FR_TAGGED_CODE_BASE ((fr_tagged_t)0xA0000000u)
#define FR_TAGGED_CODE_END ((fr_tagged_t)0xB7FFFFFFu)
#define FR_TAGGED_CODE_MAX_ID ((fr_tagged_t)UINT16_MAX)
#define FR_TAGGED_NATIVE_BASE ((fr_tagged_t)0xB8000000u)
#define FR_TAGGED_NATIVE_END ((fr_tagged_t)0xCFFFFFFFu)
#define FR_TAGGED_NATIVE_MAX_ID ((fr_tagged_t)UINT16_MAX)
#define FR_TAGGED_OBJECT_BASE ((fr_tagged_t)0xD0000000u)
#define FR_TAGGED_OBJECT_END ((fr_tagged_t)0xEFFFFFFFu)
#define FR_TAGGED_OBJECT_MAX_ID ((fr_tagged_t)UINT16_MAX)
#define FR_TAGGED_RESERVED_END ((fr_tagged_t)0xFFFFFFFFu)
#define FR_TAGGED_INT_BIAS ((int32_t)0x40000000L)
#define FR_TAGGED_INT_MIN (-1073741824)
#define FR_TAGGED_INT_MAX 1073741823

#if FR_FEATURE_HANDLES
#define FR_TAGGED_HANDLE_BASE ((fr_tagged_t)0xF0000000u)
#define FR_TAGGED_HANDLE_END ((fr_tagged_t)0xF00000FFu)
#define FR_TAGGED_RESERVED_BASE ((fr_tagged_t)0xF0000100u)
#define FR_TAGGED_HANDLE_MAX_ID ((fr_tagged_t)0x0Fu)
#define FR_TAGGED_HANDLE_MAX_GENERATION ((fr_tagged_t)0x0Fu)
#define FR_TAGGED_HANDLE_GENERATION_SHIFT 4u
#else
#define FR_TAGGED_RESERVED_BASE ((fr_tagged_t)0xF0000000u)
#endif

#if FR_FEATURE_BYTES
#define FR_TAGGED_BYTES_BASE FR_TAGGED_RESERVED_BASE
#define FR_TAGGED_BYTES_GENERATION_SHIFT 3u
#define FR_TAGGED_BYTES_MAX_ID ((fr_tagged_t)0x07u)
#define FR_TAGGED_BYTES_MAX_GENERATION ((fr_tagged_t)0xFFu)
#define FR_TAGGED_BYTES_LAST_OFFSET                                            \
  ((fr_tagged_t)((FR_TAGGED_BYTES_MAX_GENERATION                               \
                  << FR_TAGGED_BYTES_GENERATION_SHIFT) |                        \
                 FR_TAGGED_BYTES_MAX_ID))
#define FR_TAGGED_BYTES_END                                                    \
  ((fr_tagged_t)(FR_TAGGED_BYTES_BASE + FR_TAGGED_BYTES_LAST_OFFSET))
#endif

#define FR_TAGGED_NIL FR_TAGGED_SPECIAL_BASE
#define FR_TAGGED_FALSE ((fr_tagged_t)(FR_TAGGED_SPECIAL_BASE + 1u))
#define FR_TAGGED_TRUE ((fr_tagged_t)(FR_TAGGED_SPECIAL_BASE + 2u))
#define FR_TAGGED_INT_LITERAL(value)                                          \
  ((fr_tagged_t)((int32_t)(value) + FR_TAGGED_INT_BIAS))

typedef char fr_tagged_width_matches_word_size
    [(sizeof(fr_tagged_t) == FR_TAGGED_WORD_BYTES) ? 1 : -1];
typedef char fr_int_width_matches_word_size
    [(sizeof(fr_int_t) == FR_TAGGED_WORD_BYTES) ? 1 : -1];

typedef enum fr_tagged_kind_t {
  FR_TAGGED_INT,
  FR_TAGGED_SPECIAL,
  FR_TAGGED_SLOT_ID,
  FR_TAGGED_CODE_OBJECT_ID,
  FR_TAGGED_NATIVE_ID,
  FR_TAGGED_OBJECT_ID,
  FR_TAGGED_HANDLE,
  FR_TAGGED_BYTES,
  FR_TAGGED_RESERVED
} fr_tagged_kind_t;

fr_tagged_kind_t fr_tagged_kind(fr_tagged_t tagged);
const char *fr_tagged_kind_name(fr_tagged_kind_t kind);
fr_diag_value_kind_t fr_tagged_diag_value_kind(fr_tagged_t tagged);
bool fr_tagged_is_valid(fr_tagged_t tagged);

fr_tagged_t fr_tagged_nil(void);
fr_tagged_t fr_tagged_false(void);
fr_tagged_t fr_tagged_true(void);

bool fr_tagged_is_nil(fr_tagged_t tagged);
bool fr_tagged_is_false(fr_tagged_t tagged);
bool fr_tagged_is_true(fr_tagged_t tagged);
bool fr_tagged_is_bool(fr_tagged_t tagged);
bool fr_tagged_is_falsy(fr_tagged_t tagged);

bool fr_tagged_can_encode_int(int32_t raw_int);
fr_err_t fr_tagged_encode_int(int32_t raw_int, fr_tagged_t *out_tagged);
fr_err_t fr_tagged_decode_int(fr_tagged_t tagged, fr_int_t *out_int);

fr_err_t fr_tagged_encode_bool(bool value, fr_tagged_t *out_tagged);
fr_err_t fr_tagged_decode_bool(fr_tagged_t tagged, bool *out_value);

fr_err_t fr_tagged_encode_slot_id(fr_slot_id_t slot_id,
                                  fr_tagged_t *out_tagged);
fr_err_t fr_tagged_decode_slot_id(fr_tagged_t tagged,
                                  fr_slot_id_t *out_slot_id);

fr_err_t fr_tagged_encode_code_object_id(fr_code_object_id_t code_object_id,
                                         fr_tagged_t *out_tagged);
fr_err_t
fr_tagged_decode_code_object_id(fr_tagged_t tagged,
                                fr_code_object_id_t *out_code_object_id);

fr_err_t fr_tagged_encode_native_id(fr_native_id_t native_id,
                                    fr_tagged_t *out_tagged);
fr_err_t fr_tagged_decode_native_id(fr_tagged_t tagged,
                                    fr_native_id_t *out_native_id);

fr_err_t fr_tagged_encode_object_id(fr_object_id_t object_id,
                                    fr_tagged_t *out_tagged);
fr_err_t fr_tagged_decode_object_id(fr_tagged_t tagged,
                                    fr_object_id_t *out_object_id);

fr_err_t fr_tagged_encode_handle_ref(fr_handle_ref_t ref,
                                     fr_tagged_t *out_tagged);
fr_err_t fr_tagged_decode_handle_ref(fr_tagged_t tagged,
                                     fr_handle_ref_t *out_ref);

fr_err_t fr_tagged_encode_bytes_ref(fr_bytes_ref_t ref,
                                    fr_tagged_t *out_tagged);
fr_err_t fr_tagged_decode_bytes_ref(fr_tagged_t tagged,
                                    fr_bytes_ref_t *out_ref);

/* Canonical little-endian u32 byte pair. Lives here because tagged.h owns the
 * runtime word byte layout; persist, profile, and instruction call through. */
uint32_t fr_read_u32_le(const uint8_t *bytes);
void fr_write_u32_le(uint8_t *bytes, uint32_t value);
