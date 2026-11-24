// ArduinoOTA.h - the Arduino OTA class.
//
// The Arduino core serves an OTA update over its own HTTP server on port 3232.
// On the P4 that is not how the board is updated: the firmware goes over
// esp-hosted to the C6 (wled00/ota_littlefs.cpp), and the web UI has its own
// update path through the Update class.
//
// So this class has the core's shape - WLED's code sets a hostname, asks for
// it back, and hooks the start and the error - but it does not open a
// server.  The P4 does not listen on 3232.

#pragma once

#include "Arduino.h"
#include "Update.h"

#include <functional>

// The Arduino core keeps these in libraries/ArduinoOTA/src/ArduinoOTA.h.
typedef enum {
    OTA_IDLE,
    OTA_WAITAUTH,
    OTA_RUNUPDATE
} ota_state_t;

typedef enum {
    OTA_AUTH_ERROR,
    OTA_BEGIN_ERROR,
    OTA_CONNECT_ERROR,
    OTA_RECEIVE_ERROR,
    OTA_END_ERROR
} ota_error_t;

class ArduinoOTAClass {
public:
    ArduinoOTAClass &setPort(uint16_t port);
    ArduinoOTAClass &setHostname(const char *hostname);
    String getHostname();
    ArduinoOTAClass &setPassword(const char *password);
    ArduinoOTAClass &setPasswordHash(const char *passwordHash);
    ArduinoOTAClass &setAuthentication(const char *username, const char *password);
    void setTimeout(int timeoutInMillis);
    void setMd5Hash(const char *expectedMd5Hash);

    void begin();
    void end();
    void handle();

    void onStart(std::function<void(void)> callback);
    void onEnd(std::function<void(void)> callback);
    void onProgress(std::function<void(unsigned int, unsigned int)> callback);
    void onError(std::function<void(ota_error_t)> callback);

private:
    uint16_t _port = 3232;
    String _hostname;
    String _password;
    std::function<void(void)> _onStart;
    std::function<void(void)> _onEnd;
    std::function<void(unsigned int, unsigned int)> _onProgress;
    std::function<void(ota_error_t)> _onError;
};

extern ArduinoOTAClass ArduinoOTA;
