// ArduinoOTA.h - the Arduino OTA class.
//
// The P4 does not use Arduino's OTA: the firmware is updated through
// wled00/ota_littlefs.cpp, which drives the C6 coprocessor's own update over
// esp-hosted.  WLED includes this header unconditionally, so it has to exist;
// the class itself does nothing.

#pragma once

#include "Arduino.h"

class ArduinoOTAClass {
public:
    void setHostname(const char *hostname);
    void setPassword(const char *password);
    void setPort(uint16_t port);
    void setTimeout(uint32_t timeout);
    void setAuthentication(const char *username, const char *password);
    void begin();
    void end();
    void handle();
    bool getCommandHandler(void *handler);
};

extern ArduinoOTAClass ArduinoOTA;
