// wled_idf.cpp - the ESP-IDF entry point for WLED-MM-P4.
//
// The Arduino build gets its app_main() from the Arduino core
// (cores/esp32/main.cpp in framework-arduinoespressif32), which then calls the
// sketch's setup()/loop() from a FreeRTOS task.  Without the core we own the
// entry point and the task ourselves.
//
// The core-0 behaviour is deliberate and is kept here: WLED's main loop runs
// on core 0 on the P4 (wled00/wled00.ino, "Core 0 Migrator"), and the
// network/TCP-IP stack runs on core 1 (CONFIG_LWIP_TCPIP_TASK_AFFINITY=1).
// Keeping the two apart is what makes the P4's frame times predictable.
//
// The Arduino build compiles this file too, so the whole thing is behind
// WLED_BUILD_IDF - which only the ESP-IDF component sets.

#if defined(WLED_BUILD_IDF)

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_private/esp_psram_extram.h"

#include "wled.h"

static const char *TAG = "wled_idf";

// The stack the WLED loop runs on.  The Arduino build used 12000 bytes for the
// task it created in wled00.ino; keep the same headroom.
#define WLED_TASK_STACK_SIZE 12000
#define WLED_TASK_PRIORITY   1

static void wled_loop_task(void *arg)
{
    (void)arg;
    WLED::instance().setup();
    for (;;) {
        WLED::instance().loop();
        // WLED paces itself; the yield keeps the idle task fed so the task
        // watchdog does not trip on a quiet board.
        vTaskDelay(1);
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "WLED-MM-P4 - pure ESP-IDF build");

    // The PSRAM.
    //
    // On the older chip the bootloader brings it up and the IDF adds it to
    // the heap by itself.  On the rev 3 neither happens: Espressif's own
    // configuration for that chip turns the boot-time initialisation off
    // (CONFIG_SPIRAM_BOOT_INIT), and the IDF only adds the PSRAM to the
    // heap when that is on - so without this the application runs with the
    // internal memory alone.  The AudioReactive's history buffer does not
    // fit, and the display's framebuffers have nowhere to go.
    //
    // The Arduino core does the same thing (cores/esp32/esp32-hal-psram.c,
    // psramInit() and psramAddToHeap()).
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
        if (esp_psram_init() == ESP_OK) {
            if (esp_psram_extram_add_to_heap_allocator() == ESP_OK) {
                heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
                ESP_LOGI(TAG, "PSRAM: %d bytes",
                         (int)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
            } else {
                ESP_LOGE(TAG, "PSRAM could not be added to the heap");
            }
        } else {
            ESP_LOGE(TAG, "PSRAM init failed");
        }
    }

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "cores=%d rev=%d psram=%d ram=%d",
             chip.cores, chip.revision,
             (int)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (int)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));

    BaseType_t ok = xTaskCreatePinnedToCore(wled_loop_task, "wled_main",
                                             WLED_TASK_STACK_SIZE, nullptr,
                                             WLED_TASK_PRIORITY, nullptr, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "could not create the WLED task");
        return;
    }
    ESP_LOGI(TAG, "WLED task created on core 0");
}

#endif // WLED_BUILD_IDF

