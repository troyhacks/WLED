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
#include "esp_private/esp_private.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "soc/soc.h"
#endif
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

// --- the chip ---------------------------------------------------------------

EspClass ESP;

uint32_t EspClass::getChipId()
{
    uint32_t id = 0;
    esp_read_mac_to_system_flash((uint8_t *)&id, 4);
    return id;
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
    return (uint32_t)esp_flash_get_size();
}

uint32_t EspClass::getFlashChipRealSize()
{
    return (uint32_t)esp_flash_get_size();
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
    // The Arduino core reports the room left in the app partition.
    const esp_partition_t *part = esp_app_get_ota_partition();
    if (!part) {
        return 0;
    }
    return (uint32_t)esp_ota_get_app_partition_size() - (uint32_t)esp_ota_get_app_elf_size();
}
