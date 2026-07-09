#pragma once

#include "lib_native.h"
#include "types.h"

const char *fr_profile_name(void);
const char *fr_profile_contract_name(void);
uint32_t fr_profile_hash(void);
/* Debug observability for tests; lets a single binary prove word size drives hash divergence. */
uint32_t fr_profile_debug_hash_for_word_size(uint16_t word_size);
#if FR_FEATURE_SOURCE_BASE
/* Same as fr_profile_debug_hash_for_word_size but lets the caller substitute
   source bytes, so a test can prove base/core.frothy edits change the hash
   without rebuilding. */
uint32_t fr_profile_debug_hash_for_source(uint16_t word_size, const char *bytes,
                                          uint16_t length);
#endif
/* Debug observability for tests; lets a single binary substitute library-native rows. */
uint32_t fr_profile_debug_hash_for_lib_natives(
    const fr_lib_native_def_t *defs, uint16_t count);
const char *fr_profile_compiler_mode(void);
const char *fr_profile_names_mode(void);
const char *fr_profile_storage_mode(void);
const char *fr_profile_interrupt_mode(void);
