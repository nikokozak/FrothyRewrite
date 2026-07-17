#include "froth.h"
#include "repl.h"

#include "esp_platform.h"
#include "platform.h"

#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void fr_esp_halt(fr_err_t err) {
  esp_rom_printf("frothy halt err %u\n", (unsigned)err);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void app_main(void) {
#if FR_FEATURE_REPL
  /* Reserve the profile-sized runtime before Wi-Fi or Bluetooth can fragment
   * internal RAM. It remains allocated for the target lifetime. */
  fr_runtime_t *runtime = heap_caps_calloc(
      1, sizeof(*runtime), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  fr_err_t err;

  if (runtime == NULL) {
    fr_esp_halt(FR_ERR_CAPACITY);
  }

  err = fr_esp_platform_init();
  if (err != FR_OK) {
    fr_esp_halt(err);
  }

  err = fr_base_image_install(runtime);
  if (err != FR_OK) {
    fr_platform_write_text("startup err\n");
    fr_esp_halt(err);
  }

  err = fr_repl_startup_restore_and_boot(runtime);
  if (err != FR_OK) {
    fr_platform_write_text("startup err\n");
    fr_esp_halt(err);
  }

  err = fr_repl_run_platform(runtime);
  if (err != FR_OK) {
    fr_platform_write_text("repl err\n");
    fr_esp_halt(err);
  }
#endif
}
