/* Restarting from core 1 hangs about one time in three on this board: after
 * the shutdown handlers, esp_restart() stalls (with the RGB panel running
 * and code executing from PSRAM) until the RTC watchdog resets the chip,
 * seen in /status as "vakthund i maskinvaren". From core 0 it never did
 * (in 15 restarts, against 8 hangs in 22 unpinned). So a task that may be
 * on core 1 hands the restart to a task pinned to core 0. */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

#include "restart.h"

static void restart_task(void *arg)
{
    (void)arg;
    esp_restart();
}

void restart_device(void)
{
    if (xTaskGetCoreID(NULL) == 0) {
        esp_restart();
    }
    /* No memory for the task: restart from here, a hang ends in a reset too. */
    if (xTaskCreatePinnedToCore(restart_task, "restart", 4096, NULL, configMAX_PRIORITIES - 1, NULL, 0) != pdPASS) {
        esp_restart();
    }
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
