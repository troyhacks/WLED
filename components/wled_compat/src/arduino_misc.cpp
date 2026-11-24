// arduino_misc.cpp - the classes that only need to exist.
//
// DNSServer and ArduinoOTA: WLED includes the headers unconditionally, but
// the P4 runs neither a captive portal nor Arduino's OTA (the firmware is
// updated through wled00/ota_littlefs.cpp, which drives the C6 coprocessor's
// own update over esp-hosted).  The classes are here so the headers resolve.
//
// EEPROM: the Arduino core keeps the EEPROM in the NVS partition under the
// "eeprom" namespace, so a board flashed with either build finds its settings
// in the same place.

#include "Arduino.h"
#include "DNSServer.h"
#include "ArduinoOTA.h"
#include "EEPROM.h"

extern "C" {
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
}

static const char *TAG = "wled_eeprom";

// --- DNSServer --------------------------------------------------------------

bool DNSServer::start(uint16_t port)
{
    (void)port;
    return false;
}

void DNSServer::stop()
{
}

void DNSServer::processNextRequest()
{
}

// --- ArduinoOTA -------------------------------------------------------------

ArduinoOTAClass ArduinoOTA;

void ArduinoOTAClass::setHostname(const char *) {}
void ArduinoOTAClass::setPassword(const char *) {}
void ArduinoOTAClass::setPort(uint16_t) {}
void ArduinoOTAClass::setTimeout(uint32_t) {}
void ArduinoOTAClass::setAuthentication(const char *, const char *) {}
void ArduinoOTAClass::begin() {}
void ArduinoOTAClass::end() {}
void ArduinoOTAClass::handle() {}
bool ArduinoOTAClass::getCommandHandler(void *) { return false; }

// --- EEPROM -----------------------------------------------------------------

EEPROMClass EEPROM;

namespace {
nvs_handle_t s_nvs = 0;
size_t s_size = 0;
bool s_dirty = false;
} // namespace

bool EEPROMClass::begin(size_t size)
{
    end();
    s_size = size;
    s_dirty = false;
    esp_err_t err = nvs_open("eeprom", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed (err %d)", (int)err);
        s_nvs = 0;
        return false;
    }
    return true;
}

void EEPROMClass::end()
{
    if (s_nvs) {
        commit();
        nvs_close(s_nvs);
        s_nvs = 0;
    }
}

uint8_t EEPROMClass::read(int address)
{
    if (!s_nvs || address < 0 || (size_t)address >= s_size) {
        return 0;
    }
    uint8_t value = 0;
    char key[16];
    snprintf(key, sizeof(key), "e%u", (unsigned)address);
    if (nvs_get_u8(s_nvs, key, &value) != ESP_OK) {
        return 0;
    }
    return value;
}

void EEPROMClass::write(int address, uint8_t value)
{
    if (!s_nvs || address < 0 || (size_t)address >= s_size) {
        return;
    }
    char key[16];
    snprintf(key, sizeof(key), "e%u", (unsigned)address);
    if (nvs_set_u8(s_nvs, key, value) == ESP_OK) {
        s_dirty = true;
    }
}

bool EEPROMClass::commit()
{
    if (!s_nvs) {
        return false;
    }
    esp_err_t err = nvs_commit(s_nvs);
    s_dirty = false;
    return err == ESP_OK;
}
