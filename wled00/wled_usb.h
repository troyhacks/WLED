#pragma once
// USB mass storage host subsystem.
// Public interface called from wled.cpp; internals are in wled_usb.cpp.

#ifdef SOC_USB_OTG_SUPPORTED
#include "esp_err.h"
void    usb_init();           // call once from WLED::setup() — creates app_queue and starts usb_task
void    usb_poll();           // call from background_loop_nonblocking() — processes pending MSC events
esp_err_t mount_sdcard();
esp_err_t unmount_sdcard();
bool      is_sdcard_mounted();
// Check mount_path/wled_update/ for firmware.bin, network_adapter*.bin, cfg.json, presets.json, etc.
// Processes and renames each file to .done; returns true if a reboot is needed.
bool      process_storage_updates(const char* mount_path);
// Populate paths[] (each entry 16 chars) with active USB mount points. Returns count.
int       get_usb_mount_paths(char (*paths)[16], int max_paths);
#endif
