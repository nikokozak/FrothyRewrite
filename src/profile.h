#pragma once

#include "types.h"

const char *fr_profile_name(void);
const char *fr_profile_contract_name(void);
uint32_t fr_profile_hash(void);
/* Debug observability for tests; lets a single binary prove word size drives hash divergence. */
uint32_t fr_profile_debug_hash_for_word_size(uint16_t word_size);
const char *fr_profile_compiler_mode(void);
const char *fr_profile_names_mode(void);
const char *fr_profile_storage_mode(void);
const char *fr_profile_interrupt_mode(void);
