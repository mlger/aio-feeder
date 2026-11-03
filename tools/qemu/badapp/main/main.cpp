/* tools/qemu/badapp — the "bad image" of the M8a OTA selftest.
 *
 * Boots, prints exactly one marker, and restarts WITHOUT confirming —
 * the bootloader then invalidates the PENDING_VERIFY slot and falls back
 * (hub/sdkconfig.defaults: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). The
 * selftest runner (tools/qemu/ota_run.py) greps this marker between the
 * "stage2" and "rollback verified" hub markers.
 */
#include <stdio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_system.h>

extern "C" void app_main(void) {
  printf("badapp: running (pending-verify, will not confirm)\n");
  fflush(stdout);
  vTaskDelay(pdMS_TO_TICKS(300));
  esp_restart();
}
