// main_idf.cpp — Pure IDF entry point, replaces wled00.ino for WLED_IDF_BUILD.
// app_main() pins the WLED task to Core 0 and runs setup()/loop() via FreeRTOS.
#ifdef WLED_IDF_BUILD

#include "wled.h"

#ifdef WLED_DEBUG_HEAP
static void heap_caps_alloc_failed_hook(size_t requested_size, uint32_t caps, const char* function_name) {
  printf("*** %s failed to allocate %zu bytes with caps 0x%08lX\n",
         function_name, requested_size, (unsigned long)caps);
  printf("    largest free block: %zu  total free: %zu\n",
         heap_caps_get_largest_free_block(caps),
         heap_caps_get_free_size(caps));
  if (!heap_caps_check_integrity_all(false))
    printf("*** Heap CORRUPTED\n");
}
#endif

static void wled_main_task_fn(void*) {
#ifdef WLED_DEBUG_HEAP
  heap_caps_register_failed_alloc_callback(heap_caps_alloc_failed_hook);
#endif
  WLED::instance().setup();
  while (true) {
    WLED::instance().loop();
    vTaskDelay(1);
  }
}

extern "C" void app_main(void) {
  // Spawn WLED on Core 0 with a generous stack (setup() allocates a lot).
  // app_main() itself can return once the task is running.
  xTaskCreatePinnedToCore(
    wled_main_task_fn,
    "wled_main",
    65536,           // 64 KB — same ballpark as Arduino default
    NULL,
    1,               // priority
    &wled_main_task,
    0                // Core 0
  );
}

#endif // WLED_IDF_BUILD
