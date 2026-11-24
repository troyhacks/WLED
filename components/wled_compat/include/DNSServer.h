// DNSServer.h - the Arduino DNS server class.
//
// WLED includes this header but the P4 does not run a captive portal, so the
// class is here only so that the header resolves.  start() does nothing.

#pragma once

#include "Arduino.h"

class DNSServer {
public:
    bool start(uint16_t port);
    void stop();
    void processNextRequest();
};
