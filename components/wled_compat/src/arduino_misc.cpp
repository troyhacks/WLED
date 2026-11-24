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
#include <string.h>
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

// The DNS server answers the captive portal's name requests.  On the P4 there
// is no access point - the board runs Ethernet only - so there is nothing to
// answer for.

bool DNSServer::start()
{
    return false;
}

bool DNSServer::start(uint16_t port, const String &domainName, const IPAddress &resolvedIP)
{
    (void)port;
    (void)domainName;
    (void)resolvedIP;
    return false;
}

void DNSServer::stop() {}
void DNSServer::processNextRequest() {}
void DNSServer::setErrorReplyCode(const DNSReplyCode &) {}

// --- ArduinoOTA -------------------------------------------------------------

ArduinoOTAClass ArduinoOTA;

ArduinoOTAClass &ArduinoOTAClass::setPort(uint16_t port)
{
    _port = port;
    return *this;
}

ArduinoOTAClass &ArduinoOTAClass::setHostname(const char *hostname)
{
    _hostname = hostname ? hostname : "";
    return *this;
}

String ArduinoOTAClass::getHostname()
{
    return _hostname;
}

ArduinoOTAClass &ArduinoOTAClass::setPassword(const char *password)
{
    _password = password ? password : "";
    return *this;
}

ArduinoOTAClass &ArduinoOTAClass::setPasswordHash(const char *passwordHash)
{
    _password = passwordHash ? passwordHash : "";
    return *this;
}

ArduinoOTAClass &ArduinoOTAClass::setAuthentication(const char *, const char *password)
{
    _password = password ? password : "";
    return *this;
}

void ArduinoOTAClass::setTimeout(int) {}
void ArduinoOTAClass::setMd5Hash(const char *) {}

void ArduinoOTAClass::begin()
{
    // The P4 does not serve an OTA over the network: the firmware goes over
    // esp-hosted to the C6, and the web UI has its own path through Update.
}

void ArduinoOTAClass::end() {}
void ArduinoOTAClass::handle() {}

void ArduinoOTAClass::onStart(std::function<void(void)> callback) { _onStart = callback; }
void ArduinoOTAClass::onEnd(std::function<void(void)> callback) { _onEnd = callback; }
void ArduinoOTAClass::onProgress(std::function<void(unsigned int, unsigned int)> callback) { _onProgress = callback; }
void ArduinoOTAClass::onError(std::function<void(ota_error_t)> callback) { _onError = callback; }

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

// --- the core's log helpers -------------------------------------------------
// These are the platform layer's now - the filesystem needs them, and the
// filesystem is over there.
