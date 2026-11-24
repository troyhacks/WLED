// arduino_random.cpp - random(), map() and the ESP chip object.
//
// The Arduino core's random() is a xorshift with a seed the sketch sets; here
// the entropy comes from the RFQ number generator the silicon already has,
// which is what the core does on the ESP32 as well (it reseeds the xorshift
// from esp_random()).

#include "Arduino.h"

extern "C" {
#include "esp_random.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_mac.h"
#include "esp_private/esp_clk.h"
#include "soc/soc.h"
}

long random(long howbig)
{
    if (howbig == 0) {
        return 0;
    }
    uint32_t value = esp_random();
    if (howbig < 0) {
        // A negative range means the result runs from 0 down to howbig.
        return -(long)(value % (uint32_t)(-howbig + 1));
    }
    return (long)(value % (uint32_t)howbig);
}

long random(long howsmall, long howbig)
{
    if (howsmall >= howbig) {
        return howsmall;
    }
    long diff = howbig - howsmall;
    return howsmall + random(diff);
}

void randomSeed(unsigned long seed)
{
    // The ESP32 core ignores the seed: the generator is seeded from the
    // hardware.  The argument is kept so that the sketches compile.
    (void)seed;
}

long map(long x, long in_min, long in_max, long out_min, long out_max)
{
    if (in_max == in_min) {
        return out_min;
    }
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// The Arduino core's dtostrf() is the platform layer's now - the String class
// needs it, and the String class is over there.

// --- the chip ---------------------------------------------------------------

EspClass ESP;

uint32_t EspClass::getChipId()
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    return ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 8) | mac[4];
}

const char *EspClass::getChipModel()
{
    return CONFIG_IDF_TARGET_ESP32P4 ? "ESP32-P4" : "ESP32";
}

uint8_t EspClass::getChipRevision()
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    return chip.revision;
}

uint8_t EspClass::getChipCores()
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    return chip.cores;
}

uint32_t EspClass::getCpuFreqMHz()
{
    return esp_clk_cpu_freq();
}

uint32_t EspClass::getFlashChipSize()
{
    uint32_t size = 0;
    if (esp_flash_get_size(nullptr, &size) != ESP_OK) {
        return 0;
    }
    return size;
}

uint32_t EspClass::getFlashChipRealSize()
{
    return getFlashChipSize();
}

uint8_t EspClass::getFlashChipMode()
{
    // The Arduino core reports the QIO/DIO/QOUT mode the flash is in.
    return 2; // QIO
}

uint32_t EspClass::getFlashChipSpeed()
{
    return 80000000; // 80 MHz
}

uint32_t EspClass::getHeapSize()
{
    return (uint32_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

uint32_t EspClass::getFreeHeap()
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

uint32_t EspClass::getMinFreeHeap()
{
    return (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

uint32_t EspClass::getMaxAllocHeap()
{
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

uint32_t EspClass::getPsramSize()
{
    return (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
}

uint32_t EspClass::getFreePsram()
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

uint32_t EspClass::getMinFreePsram()
{
    return (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
}

uint32_t EspClass::getMaxAllocPsram()
{
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

uint32_t EspClass::getMaxFreeBlockSize()
{
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
}

const char *EspClass::getSdkVersion()
{
    return esp_get_idf_version();
}

const char *EspClass::getCoreVersion()
{
    // The Arduino core is gone; the "core" is the IDF now.
    return "2.0.17";
}

void EspClass::restart()
{
    esp_restart();
}

uint8_t EspClass::getResetReason()
{
    return (uint8_t)esp_reset_reason();
}

void *EspClass::getResetInfoPtr()
{
    return (void *)esp_reset_reason();
}

uint32_t EspClass::getFreeSketchSpace()
{
    // The room for an update: the app partition that is not the running one.
    const esp_partition_t *running = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    if (!running) {
        return 0;
    }
    const esp_partition_t *ota0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    const esp_partition_t *ota1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    const esp_partition_t *target = (running == ota0) ? ota1 : ota0;
    if (!target) {
        return 0;
    }
    return (uint32_t)target->size;
}

// --- the partition helpers -------------------------------------------------
//
// The Arduino core has these on the ESP object, and its Update library uses
// them to write the new image.

bool EspClass::partitionEraseRange(const esp_partition_t *partition, size_t offset, size_t size)
{
    if (!partition) {
        return false;
    }
    return esp_partition_erase_range(partition, offset, size) == ESP_OK;
}

bool EspClass::partitionWrite(const esp_partition_t *partition, size_t offset, const uint32_t *data, size_t size)
{
    if (!partition || !data) {
        return false;
    }
    return esp_partition_write(partition, offset, data, size) == ESP_OK;
}

bool EspClass::partitionRead(const esp_partition_t *partition, size_t offset, uint32_t *data, size_t size)
{
    if (!partition || !data) {
        return false;
    }
    return esp_partition_read(partition, offset, data, size) == ESP_OK;
}
