#pragma once

#include "native.h"

/* One library-native entry. The build generator emits a static array of these
   at .frothy/build/<target>/lib_natives.c for every native declared in
   resolved libs' lib.toml (SPEC D13). */
typedef struct fr_lib_native_def_t {
  const char *name;
  fr_native_fn_t fn;
  uint8_t arity;
} fr_lib_native_def_t;

/* Resolved at link time. With no project driving the build, lib_native.c
   ships weak empty defaults; the generator's output overrides them. */
extern const fr_lib_native_def_t fr_lib_natives[];
extern const uint16_t fr_lib_natives_count;

/* Boot hook called once after fr_base_image_install. Empty table is a no-op. */
fr_err_t fr_lib_natives_install(fr_runtime_t *runtime);
