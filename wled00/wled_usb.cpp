// USB mass storage host subsystem.
// Handles MSC device connect/disconnect and SD card mount via SDMMC.
// Public interface: usb_init() called from setup(), usb_poll() called from background loop.
//
// KNOWN IDF BUG (ESP32-P4): Random crash during USB host transfers:
//   assert failed: usb_dwc_hal_chan_decode_intr usb_dwc_hal.c:502
//   (chan_intrs & USB_DWC_LL_INTR_CHAN_CHHLTD)
// Race condition in the DWC HAL — a channel interrupt fires before the hardware sets CHHLTD.
// Fix: in esp32-arduino-lib-builder/esp-idf/components/hal/usb_dwc_hal.c ~line 502, replace:
//   HAL_ASSERT(chan_intrs & USB_DWC_LL_INTR_CHAN_CHHLTD);
// with:
//   if (!(chan_intrs & USB_DWC_LL_INTR_CHAN_CHHLTD)) { return USB_DWC_HAL_CHAN_EVENT_NONE; }
// This ignores the spurious interrupt; the channel will re-interrupt once actually halted.

#include "wled.h"

#ifdef SOC_USB_OTG_SUPPORTED

#ifndef CONFIG_USB_HOST_HW_BUFFER_BIAS_BALANCED
  #error "USB Hardware Buffer Bias must be set to 'Balanced' via USB-OTG or CONFIG_USB_HOST_HW_BUFFER_BIAS_BALANCED=y."
  // You could also comment out the #error and try (untested):
  #undef CONFIG_USB_HOST_HW_BUFFER_BIAS_IN
  #undef CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT
  #define CONFIG_USB_HOST_HW_BUFFER_BIAS_BALANCED 1
#endif

#include <dirent.h>
#include "usb/usb_host.h"
#include "usb/msc_host_vfs.h"
#include "ImageCacheManager.h"

#define MNT_PATH "/usb"     // Base mount path prefix; devices mount as /usb0, /usb1, /usb2...
#define MAX_MSC_DEVICES  CONFIG_FATFS_VOLUME_COUNT

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include <sys/stat.h>
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "esp_ldo_regulator.h"  // ESP32-P4: LDO4 (VO4) powers the SD card via P-FET
#endif

#define MOUNT_POINT    "/sdcard"
#define SD_POWER_PIN   GPIO_NUM_45
#define APP_QUEUE_SIZE 5

// ============================================================
// SD card
// ============================================================
static sdmmc_card_t* card = NULL;
#if defined(CONFIG_IDF_TARGET_ESP32P4)
static esp_ldo_channel_handle_t sd_ldo = NULL;
#endif

static void sdcard_power_on(void) {
  #if defined(CONFIG_IDF_TARGET_ESP32P4)
  // On Olimex ESP32-P4-PC the SD card P-FET source is fed by ESP32-P4 internal LDO4 (VO4).
  // LDO4 must be acquired before toggling the P-FET or the card gets no power at all.
  if (!sd_ldo) {
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = 4, .voltage_mv = 3300 };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &sd_ldo));
  }
  #endif
  gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << SD_POWER_PIN),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
  };
  gpio_config(&io_conf);
  gpio_set_level(SD_POWER_PIN, 0);  // P-FET: low = on
  vTaskDelay(pdMS_TO_TICKS(50));    // let power stabilize
}

static void sdcard_power_off(void) {
  gpio_set_level(SD_POWER_PIN, 1);  // P-FET: high = off
  #if defined(CONFIG_IDF_TARGET_ESP32P4)
  if (sd_ldo) { esp_ldo_release_channel(sd_ldo); sd_ldo = NULL; }
  #endif
}

esp_err_t mount_sdcard(void) {
  sdcard_power_on();

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 5,
    .allocation_unit_size = 16 * 1024
  };

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_52M;  // 52MHz — standard HS ceiling; try 80000 or SDMMC_FREQ_SDR104 if card supports UHS-I

  // GPIO39-44 are SD1_* pins on ESP32-P4 → must use SDMMC_HOST_SLOT_1.
  // The CONFIG_ESP_HOSTED_SDIO_SLOT logic inverts the slot to avoid conflicts with
  // the ESP-Hosted SDIO interface, but the default must still be slot 1 for this board.
  #if CONFIG_ESP_HOSTED_SDIO_SLOT == 1
  host.slot = SDMMC_HOST_SLOT_0;
  #else
  host.slot = SDMMC_HOST_SLOT_1;
  #endif

  sdmmc_slot_config_t slot_config = {
    .clk = GPIO_NUM_43,
    .cmd = GPIO_NUM_44,
    .d0 = GPIO_NUM_39,
    .d1 = GPIO_NUM_40,
    .d2 = GPIO_NUM_41,
    .d3 = GPIO_NUM_42,
    .cd = SDMMC_SLOT_NO_CD,
    .wp = SDMMC_SLOT_NO_WP,
    .width = 4,
    .flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP,
  };

  esp_err_t ret = ESP_ERR_TIMEOUT;
  for (int attempt = 1; attempt <= 3 && ret != ESP_OK; attempt++) {
    ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK && attempt < 3) {
      USER_PRINTF("Mount attempt %d failed (%s), retrying...\n", attempt, esp_err_to_name(ret));
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }
  if (ret != ESP_OK) {
    USER_PRINTF("Mount failed : %s\n", esp_err_to_name(ret));
    sdcard_power_off();
    return ret;
  }
  sdmmc_card_print_info(stdout, card);
  USER_PRINTF("Mounted at %s\n", MOUNT_POINT);
  return ESP_OK;
}

esp_err_t unmount_sdcard(void) {
  if (card == NULL) return ESP_ERR_INVALID_STATE;
  esp_err_t ret = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
  if (ret == ESP_OK) {
    card = NULL;
    sdcard_power_off();
    USER_PRINTLN("Unmounted");
  }
  return ret;
}

bool is_sdcard_mounted(void) { return card != NULL; }

// ============================================================
// USB MSC device tracking
// ============================================================
typedef struct {
  uint8_t usb_addr;
  msc_host_device_handle_t msc_device;
  msc_host_vfs_handle_t vfs_handle;
} msc_dev_entry_t;

static msc_dev_entry_t *msc_devices[MAX_MSC_DEVICES] = {0};
static QueueHandle_t app_queue;

typedef struct {
  enum {
    APP_QUIT,
    APP_DEVICE_CONNECTED,
    APP_DEVICE_DISCONNECTED,
  } id;
  union {
    uint8_t new_dev_address;
    msc_host_device_handle_t device_handle;
  } data;
} app_message_t;

static inline int find_free_slot(void) {
  for (int i = 0; i < MAX_MSC_DEVICES; i++) if (msc_devices[i] == NULL) return i;
  return -1;
}

static esp_err_t allocate_new_msc_device(const app_message_t *msg, int *out_slot) {
  int slot = find_free_slot();
  if (slot < 0) { ESP_LOGE(TAG, "No free slots for new MSC device (max %d)", MAX_MSC_DEVICES); return ESP_ERR_NOT_FOUND; }

  msc_devices[slot] = (msc_dev_entry_t *)calloc(1, sizeof(msc_dev_entry_t));
  if (!msc_devices[slot]) { ESP_LOGE(TAG, "Failed to allocate memory for new MSC device entry"); return ESP_ERR_NO_MEM; }

  esp_err_t err = msc_host_install_device(msg->data.new_dev_address, &msc_devices[slot]->msc_device);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "msc_host_install_device failed: %s", esp_err_to_name(err));
    free(msc_devices[slot]); msc_devices[slot] = NULL;
    return err;
  }
  msc_devices[slot]->usb_addr = msg->data.new_dev_address;

  const esp_vfs_fat_mount_config_t mount_config = { .format_if_mount_failed = false, .max_files = 3, .allocation_unit_size = 8192 };
  char mount_path[16];
  snprintf(mount_path, sizeof(mount_path), MNT_PATH "%d", slot);
  err = msc_host_vfs_register(msc_devices[slot]->msc_device, mount_path, &mount_config, &msc_devices[slot]->vfs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "msc_host_vfs_register failed: %s", esp_err_to_name(err));
    esp_err_t uninstall_err = msc_host_uninstall_device(msc_devices[slot]->msc_device);
    if (uninstall_err != ESP_OK) ESP_LOGW(TAG, "msc_host_uninstall_device failed during cleanup: %s", esp_err_to_name(uninstall_err));
    free(msc_devices[slot]); msc_devices[slot] = NULL;
    return err;
  }
  *out_slot = slot;
  return ESP_OK;
}

static int find_slot_by_handle(msc_host_device_handle_t handle) {
  for (int i = 0; i < MAX_MSC_DEVICES; i++) if (msc_devices[i] && msc_devices[i]->msc_device == handle) return i;
  return -1;
}

static void free_msc_device(int slot) {
  if (slot < 0 || slot >= MAX_MSC_DEVICES || !msc_devices[slot]) { ESP_LOGE(TAG, "Invalid slot index for MSC device deallocation"); return; }
  if (msc_devices[slot]->vfs_handle)  ESP_ERROR_CHECK(msc_host_vfs_unregister(msc_devices[slot]->vfs_handle));
  if (msc_devices[slot]->msc_device)  ESP_ERROR_CHECK(msc_host_uninstall_device(msc_devices[slot]->msc_device));
  free(msc_devices[slot]); msc_devices[slot] = NULL;
}

static void free_all_msc_devices(void) {
  for (int i = 0; i < MAX_MSC_DEVICES; i++) if (msc_devices[i]) free_msc_device(i);
}

static void gpio_cb(void *arg) {
  BaseType_t xTaskWoken = pdFALSE;
  app_message_t message = { .id = app_message_t::APP_QUIT };
  if (app_queue) xQueueSendFromISR(app_queue, &message, &xTaskWoken);
  if (xTaskWoken == pdTRUE) portYIELD_FROM_ISR();
}

static inline int8_t find_usb_addr_by_handle(msc_host_device_handle_t handle) {
  for (int8_t i = 0; i < MAX_MSC_DEVICES; i++) if (msc_devices[i] && msc_devices[i]->msc_device == handle) return msc_devices[i]->usb_addr;
  return -1;
}

static void msc_event_cb(const msc_host_event_t *event, void *arg) {
  if (event->event == event->MSC_DEVICE_CONNECTED) {
    DEBUG_PRINTF("MSC device connected (usb_addr=%d)\n", event->device.address);
    app_message_t message = {};
    message.id = app_message_t::APP_DEVICE_CONNECTED;
    message.data.new_dev_address = event->device.address;
    xQueueSend(app_queue, &message, portMAX_DELAY);
  } else if (event->event == event->MSC_DEVICE_DISCONNECTED) {
    int usb_addr = find_usb_addr_by_handle(event->device.handle);
    if (usb_addr >= 0) DEBUG_PRINTF("MSC device disconnected (usb_addr=%d)\n", usb_addr);
    else               DEBUG_PRINTLN("MSC device disconnected, but failed to retrieve USB address");
    app_message_t message = {};
    message.id = app_message_t::APP_DEVICE_DISCONNECTED;
    message.data.device_handle = event->device.handle;
    xQueueSend(app_queue, &message, portMAX_DELAY);
  }
}

static void print_device_info(msc_host_device_info_t *info) {
  const size_t megabyte = 1024 * 1024;
  uint64_t capacity = ((uint64_t)info->sector_size * info->sector_count) / megabyte;
  USER_PRINTF("USB Disk Capacity: %llu MB\n", capacity);
}

static void usb_task(void *args) {
  usb_host_config_t host_config = {};
  host_config.intr_flags     = ESP_INTR_FLAG_LEVEL1;
  host_config.peripheral_map = BIT(0);
  ESP_ERROR_CHECK(usb_host_install(&host_config));

  const msc_host_driver_config_t msc_config = {
    .create_backround_task = true,
    .task_priority = 1,
    .stack_size = 4096,
    .core_id = 0,
    .callback = msc_event_cb,
  };
  ESP_ERROR_CHECK(msc_host_install(&msc_config));

  bool has_clients = true;
  while (true) {
    uint32_t event_flags;
    usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      has_clients = false;
      if (usb_host_device_free_all() == ESP_OK) break;
    }
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE && !has_clients) break;
  }
  vTaskDelay(10);
  USER_PRINTLN("Deinitializing USB (This shouldn't happen)");
  ESP_ERROR_CHECK(usb_host_uninstall());
  vTaskDelete(NULL);
}

// ============================================================
// Public interface
// ============================================================

void usb_init() {
  DEBUG_PRINTLN("Initializing USB Host...");
  app_queue = xQueueCreate(APP_QUEUE_SIZE, sizeof(app_message_t));
  if (!app_queue) { DEBUG_PRINTLN("Failed to create USB Host app_queue"); return; }
  xTaskCreatePinnedToCore(usb_task, "usb_task", 4096, NULL, 2, NULL, 0);
  DEBUG_PRINTLN("Setup complete. Waiting for USB Host events.");
}

void usb_poll() {
  app_message_t msg;
  if (!xQueueReceive(app_queue, &msg, 0)) return;
  switch (msg.id) {
  case app_message_t::APP_DEVICE_CONNECTED: {
    int slot;
    if (allocate_new_msc_device(&msg, &slot) == ESP_OK) {
      msc_host_device_info_t info;
      ESP_ERROR_CHECK_WITHOUT_ABORT(msc_host_get_device_info(msc_devices[slot]->msc_device, &info));
      print_device_info(&info);
      USER_PRINTLN("ImageCache started");
      ImageCacheManager::getInstance().startPreload("/usb0");
    } else {
      USER_PRINTLN("USB operation failed. Try replugging?");
    }
    break;
  }
  case app_message_t::APP_DEVICE_DISCONNECTED: {
    USER_PRINTLN("USB Device Disconnected");
    int slot = find_slot_by_handle(msg.data.device_handle);
    if (slot >= 0) free_msc_device(slot);
    break;
  }
  default:
    USER_PRINTF("Unknown USB Error message ID: %d\n", msg.id);
    break;
  }
}

#endif // SOC_USB_OTG_SUPPORTED
