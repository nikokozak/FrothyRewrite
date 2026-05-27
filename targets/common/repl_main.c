#include "froth.h"
#include "repl.h"

int main(void) {
#if FR_FEATURE_REPL
  static fr_runtime_t runtime;
  fr_err_t err = fr_base_image_install(&runtime);

  if (err != FR_OK) {
    return (int)err;
  }

  err = fr_repl_startup_restore_and_boot(&runtime);
  if (err != FR_OK) {
    return (int)err;
  }

  err = fr_repl_run_platform(&runtime);
  return err == FR_OK ? 0 : (int)err;
#else
  return (int)FR_ERR_UNSUPPORTED;
#endif
}
