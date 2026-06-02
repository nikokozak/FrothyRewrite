/* T12L test-mixed extension. One native that returns a known constant.
 * Exists to prove the lib_natives.c generator + the kernel's library-
 * native install path link and dispatch correctly. */

#include "runtime.h"
#include "tagged.h"
#include "types.h"

fr_err_t fr_lib_test_mixed_echo(fr_runtime_t *runtime, const fr_tagged_t *args,
                                uint8_t arg_count, fr_tagged_t *out) {
  (void)runtime;
  (void)args;
  (void)arg_count;
  return fr_tagged_encode_int(7, out);
}
